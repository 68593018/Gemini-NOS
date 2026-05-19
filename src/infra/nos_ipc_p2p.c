#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include "nos_scheduler.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_api.h"
#include "nos_node_priv.h"
#include "nos_shm.h"

#define TX_QUEUE_SIZE 1024
#define MAX_REMOTE_CONNS 16

/* 流控水位线：90% 拥塞，20% 恢复 */
#define TX_HIGH_WATERMARK 900
#define TX_LOW_WATERMARK 200

/**
 * @brief IPC 连接上下文 (支持异步发送)
 */
typedef struct {
    char node_name[32];         // 远程节点标识
    char uds_path[108];
    int fd;
    nos_buffer_t *tx_queue[TX_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    pthread_mutex_t lock;
    int is_connected;
    int is_congested;           // 拥塞标记
    nos_thread_t *owner_thread; // 绑定的 IO 线程
} nos_ipc_conn_t;

static nos_ipc_conn_t g_remote_conns[MAX_REMOTE_CONNS];
static uint32_t g_remote_conn_count = 0;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* 静态函数前置声明 */
static void nos_ipc_event_handler(int fd, void *arg);
static nos_ipc_conn_t* get_or_create_conn(const char *node_name, const char *uds_path);
nos_status_t nos_ipc_send_enqueue_ex(const char *node_name, const char *uds_path, nos_buffer_t *buf);

/**
 * @brief 尝试从队列中发送数据
 */
static void nos_ipc_process_tx(nos_ipc_conn_t *conn) {
    pthread_mutex_lock(&conn->lock);
    while (conn->head != conn->tail) {
        nos_buffer_t *buf = conn->tx_queue[conn->head];
        nos_service_msg_t *header = (nos_service_msg_t *)buf->data;
        size_t full_len = sizeof(nos_service_msg_t) + header->payload_len;

        /* SEQPACKET 保证原子写入整个报文 */
        ssize_t sent = send(conn->fd, buf->data, full_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent == (ssize_t)full_len) {
            atomic_fetch_add(&g_node_ctx.stats.tx_packets, 1);
            atomic_fetch_add(&g_node_ctx.stats.tx_bytes, full_len);
            nos_buffer_release(buf);
            conn->head = (conn->head + 1) % TX_QUEUE_SIZE;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* 缓冲区满，停止发送，等待下次 EPOLLOUT */
            pthread_mutex_unlock(&conn->lock);
            return;
        } else {
            /* 链路故障 */
            nos_sys_log_error("IPC remote send failed to %s: %s", conn->uds_path, strerror(errno));
            atomic_fetch_add(&g_node_ctx.stats.tx_errors, 1);
            close(conn->fd);
            conn->fd = -1;
            conn->is_connected = 0;
            conn->is_congested = 0;
            pthread_mutex_unlock(&conn->lock);
            return;
        }
    }

    /* 检查是否可以解除拥塞状态 (队列已排干到低水位以下) */
    if (conn->is_congested) {
        uint32_t used = (conn->tail >= conn->head) ? (conn->tail - conn->head) : (TX_QUEUE_SIZE - (conn->head - conn->tail));
        if (used < TX_LOW_WATERMARK) {
            conn->is_congested = 0;
            nos_sys_log_info("IPC backpressure relieved for %s (Queue used: %u)", conn->uds_path, used);
        }
    }
    pthread_mutex_unlock(&conn->lock);
}

/**
 * @brief 接收报文并处理 (支持主动/被动连接)
 */
static void nos_ipc_recv_handler(int fd, nos_ipc_conn_t *conn, nos_thread_t *thread) {
    nos_service_msg_t header_tmp;
    ssize_t peek_len = recv(fd, &header_tmp, sizeof(header_tmp), MSG_PEEK | MSG_DONTWAIT);
    if (peek_len <= 0) {
        if (peek_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        /* 连接断开 */
        if (conn) {
            nos_sys_log_info("Remote closed connection: %s", conn->uds_path);
            nos_scheduler_remove_fd(conn->owner_thread, fd);
            close(fd); conn->fd = -1; conn->is_connected = 0;
        } else {
            nos_sys_log_info("Passive connection closed (FD:%d)", fd);
            nos_scheduler_remove_fd(thread, fd);
            close(fd);
        }
        return;
    }

    if (header_tmp.magic != NOS_IPC_MAGIC) {
        nos_sys_log_error("Protocol error: Invalid magic. Dropping link.");
        if (conn) {
            nos_scheduler_remove_fd(conn->owner_thread, fd);
            close(fd); conn->fd = -1; conn->is_connected = 0;
        } else {
            nos_scheduler_remove_fd(thread, fd);
            close(fd);
        }
        return;
    }

    size_t full_msg_len = sizeof(nos_service_msg_t) + header_tmp.payload_len;
    nos_buffer_t *buf = nos_buffer_alloc(full_msg_len, 0);
    if (!buf) {
        atomic_fetch_add(&g_node_ctx.stats.buffer_alloc_fails, 1);
        recv(fd, NULL, 0, MSG_TRUNC | MSG_DONTWAIT); // 丢弃该包
        return;
    }

    ssize_t ret = recv(fd, buf->data, full_msg_len, MSG_DONTWAIT);
    if (ret == (ssize_t)full_msg_len) {
        nos_service_msg_t *header = (nos_service_msg_t *)buf->data;
        
        /* 处理身份握手控制报文 */
        if (header->msg_code == NOS_IPC_MSG_HELLO) {
            char *remote_name = (char*)(header + 1);
            if (conn) {
                strncpy(conn->node_name, remote_name, 31);
                nos_sys_log_info("IPC Handshake complete: Remote Node is %s", remote_name);
            } else {
                /* 被动连接升级为有名连接 */
                nos_ipc_conn_t *new_conn = get_or_create_conn(remote_name, NULL);
                if (new_conn) {
                    pthread_mutex_lock(&new_conn->lock);
                    new_conn->fd = fd;
                    new_conn->is_connected = 1;
                    /* 修改 epoll 监听以支持双向收发 */
                    nos_scheduler_remove_fd(thread, fd);
                    nos_scheduler_add_fd_ex(new_conn->owner_thread, fd, EPOLLIN | EPOLLOUT | EPOLLET, nos_ipc_event_handler, new_conn);
                    pthread_mutex_unlock(&new_conn->lock);
                    nos_sys_log_info("IPC Passive connection promoted to Node %s (FD:%d)", remote_name, fd);
                }
            }
        } else if (header->msg_code == NOS_IPC_MSG_SHM_EVENT) {
            /* 处理共享内存到达事件 */
            nos_shm_mpsc_queue_t *local_q = nos_shm_get_local_queue();
            if (local_q) {
                uint32_t offset;
                while ((offset = nos_shm_mpsc_dequeue(local_q)) != 0) {
                    nos_buffer_t *shm_buf = (nos_buffer_t*)nos_shm_offset_to_ptr(offset);
                    if (shm_buf) {
                        /* 关键修复：重写共享内存中的绝对指针为当前进程的合法虚拟地址 */
                        shm_buf->raw_data = (uint8_t*)(shm_buf + 1);
                        shm_buf->data = shm_buf->raw_data + shm_buf->headroom;
                        
                        atomic_fetch_add(&g_node_ctx.stats.rx_packets, 1);
                        /* 业务线分发 */
                        nos_service_msg_send(shm_buf);
                        /* 释放队列带来的那一次引用计数 */
                        nos_buffer_release(shm_buf);
                    }
                }
            }
        } else {
            atomic_fetch_add(&g_node_ctx.stats.rx_packets, 1);
            atomic_fetch_add(&g_node_ctx.stats.rx_bytes, ret);
            nos_service_msg_send(buf);
        }
    } else {
        nos_sys_log_error("Partial recv or error on SEQPACKET.");
        atomic_fetch_add(&g_node_ctx.stats.rx_errors, 1);
    }
    nos_buffer_release(buf);
}

/**
 * @brief IPC 事件统一回调 (处理读写)
 */
static void nos_ipc_event_handler(int fd, void *arg) {
    nos_ipc_conn_t *conn = (nos_ipc_conn_t *)arg;
    if (fd < 0) return;

    /* 1. 处理写事件 (优先排干发送队列) */
    nos_ipc_process_tx(conn);

    /* 2. 处理读事件 */
    nos_ipc_recv_handler(fd, conn, conn->owner_thread);
}

/**
 * @brief 内部函数：获取或建立跨进程连接
 */
static nos_ipc_conn_t* get_or_create_conn(const char *node_name, const char *uds_path) {
    pthread_mutex_lock(&g_pool_lock);
    for (uint32_t i = 0; i < g_remote_conn_count; i++) {
        /* 优先匹配 Node Name，若尚未识别则匹配 Path */
        if ((node_name && strcmp(g_remote_conns[i].node_name, node_name) == 0) ||
            (uds_path && strcmp(g_remote_conns[i].uds_path, uds_path) == 0)) {
            pthread_mutex_unlock(&g_pool_lock);
            return &g_remote_conns[i];
        }
    }

    if (g_remote_conn_count >= MAX_REMOTE_CONNS) {
        pthread_mutex_unlock(&g_pool_lock); return NULL;
    }

    nos_ipc_conn_t *conn = &g_remote_conns[g_remote_conn_count++];
    if (node_name) strncpy(conn->node_name, node_name, 31);
    else strcpy(conn->node_name, "Unknown");
    
    if (uds_path) strncpy(conn->uds_path, uds_path, 107);
    else strcpy(conn->uds_path, "Passive");

    conn->fd = -1;
    conn->head = conn->tail = 0;
    conn->is_connected = 0;
    conn->is_congested = 0;
    pthread_mutex_init(&conn->lock, NULL);
    conn->owner_thread = g_node_ctx.mgmt_thread; // 统一由管理线程处理 IO
    
    pthread_mutex_unlock(&g_pool_lock);
    return conn;
}

/**
 * @brief 发送身份握手报文
 */
static void nos_ipc_send_hello(nos_ipc_conn_t *conn) {
    nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
    if (!buf) return;
    
    nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
    msg->magic = NOS_IPC_MAGIC;
    msg->version = NOS_IPC_VERSION;
    msg->msg_code = NOS_IPC_MSG_HELLO;
    msg->payload_len = strlen(g_node_ctx.node_def->name) + 1;
    strcpy((char*)(msg + 1), g_node_ctx.node_def->name);
    
    /* 直接入队，不触发递归发送 */
    uint32_t next_tail = (conn->tail + 1) % TX_QUEUE_SIZE;
    if (next_tail != conn->head) {
        nos_buffer_retain(buf);
        conn->tx_queue[conn->tail] = buf;
        conn->tail = next_tail;
    }
    nos_buffer_release(buf);
}

nos_status_t nos_ipc_send_enqueue_shm(const char *node_name, const char *uds_path, nos_buffer_t *buf) {
    if (!buf || buf->flags != 1) return NOS_ERR; // 必须是 SHM Buffer

    nos_shm_mpsc_queue_t *remote_q = nos_shm_get_remote_queue(node_name);
    if (!remote_q) {
        nos_sys_log_error("SHM enqueue failed: remote node %s not found in SHM registry", node_name);
        return NOS_ERR;
    }

    uint32_t offset = nos_shm_ptr_to_offset(buf);
    if (offset == 0) return NOS_ERR;

    nos_buffer_retain(buf); // 必须在入队前增加引用计数，防止消费者过快释放导致 Use-After-Free

    /* 压入远程队列 */
    nos_status_t st = nos_shm_mpsc_enqueue(remote_q, offset);
    if (st != NOS_OK) {
        nos_buffer_release(buf); // 入队失败，回退引用计数
        nos_sys_log_warn("SHM backpressure triggered for %s (Queue Full)", node_name);
        return st;
    }

    /* 通过控制通道 (UDS) 发送唤醒事件 */
    nos_buffer_t *evt_buf = nos_buffer_alloc(sizeof(nos_service_msg_t), 0);
    if (evt_buf) {
        nos_service_msg_t *evt = (nos_service_msg_t *)evt_buf->data;
        evt->magic = NOS_IPC_MAGIC;
        evt->version = NOS_IPC_VERSION;
        evt->msg_code = NOS_IPC_MSG_SHM_EVENT;
        evt->payload_len = 0;
        nos_ipc_send_enqueue_ex(node_name, uds_path, evt_buf);
        nos_buffer_release(evt_buf);
    }

    return NOS_OK;
}

/**
 * @brief 异步发送入队接口 (支持节点名称路由)
 */
nos_status_t nos_ipc_send_enqueue_ex(const char *node_name, const char *uds_path, nos_buffer_t *buf) {
    nos_ipc_conn_t *conn = get_or_create_conn(node_name, uds_path);
    if (!conn) return NOS_ERR;

    pthread_mutex_lock(&conn->lock);
    
    /* 1. 自动重连逻辑 */
    if (conn->fd < 0 && uds_path && strcmp(uds_path, "Passive") != 0) {
        int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (fd >= 0) {
            struct sockaddr_un addr = {.sun_family = AF_UNIX};
            strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path) - 1);
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                conn->fd = fd;
                conn->is_connected = 1;
                conn->is_congested = 0;
                /* 使用调度器标准接口注册 */
                nos_scheduler_add_fd_ex(conn->owner_thread, fd, EPOLLIN | EPOLLOUT | EPOLLET, nos_ipc_event_handler, conn);
                nos_sys_log_info("IPC connected to %s (Node:%s, FD:%d)", uds_path, node_name, fd);
                
                /* 连接建立后立即发送身份信息 */
                nos_ipc_send_hello(conn);
            } else {
                nos_sys_log_error("IPC connect to %s failed: %s", uds_path, strerror(errno));
                close(fd);
            }
        }
    }

    /* 如果仍然没连接成功且没有 path (比如被动连接还没握手)，暂存数据或报错 */
    if (!conn->is_connected) {
        pthread_mutex_unlock(&conn->lock);
        return NOS_ERR;
    }

    /* 2. 入队前检查背压 */
    uint32_t used = (conn->tail >= conn->head) ? (conn->tail - conn->head) : (TX_QUEUE_SIZE - (conn->head - conn->tail));
    if (used > TX_HIGH_WATERMARK) {
        if (!conn->is_congested) {
            conn->is_congested = 1;
            nos_sys_log_warn("IPC backpressure triggered for %s (Queue used: %u)", conn->node_name, used);
        }
        pthread_mutex_unlock(&conn->lock);
        atomic_fetch_add(&g_node_ctx.stats.dropped_full, 1);
        return NOS_ERR_BUSY;
    }

    /* 3. 入队 */
    uint32_t next_tail = (conn->tail + 1) % TX_QUEUE_SIZE;
    if (next_tail == conn->head) {
        pthread_mutex_unlock(&conn->lock);
        atomic_fetch_add(&g_node_ctx.stats.dropped_full, 1);
        return NOS_ERR_BUSY;
    }

    nos_buffer_retain(buf);
    conn->tx_queue[conn->tail] = buf;
    conn->tail = next_tail;
    pthread_mutex_unlock(&conn->lock);

    /* 4. 主动触发一次尝试发送 */
    nos_ipc_process_tx(conn);
    
    return NOS_OK;
}

/**
 * @brief 处理被动接入的连接
 */
static void nos_ipc_passive_handler(int fd, void *arg) {
    nos_thread_t *thread = (nos_thread_t *)arg;
    nos_ipc_recv_handler(fd, NULL, thread);
}

/**
 * @brief 监听 Socket 的新连接回调
 */
static void nos_ipc_accept_handler(int listen_fd, void *arg) {
    nos_thread_t *thread = (nos_thread_t *)arg;
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) return;

    nos_sys_log_info("IPC Accepted new connection: FD %d", client_fd);
    fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);

    /* 直接将 thread 作为 arg 传递给被动处理器 */
    nos_scheduler_add_fd(thread, client_fd, nos_ipc_passive_handler, thread);
}

/**
 * @brief 初始化本地 IPC 监听
 */
nos_status_t nos_ipc_init(nos_thread_t *thread, const char *uds_path) {
    int listen_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (listen_fd < 0) return NOS_ERR;

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path) - 1);
    unlink(uds_path);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        nos_sys_log_error("IPC Bind failed: %s", strerror(errno));
        close(listen_fd); return NOS_ERR;
    }

    if (listen(listen_fd, 5) < 0) { close(listen_fd); return NOS_ERR; }

    return nos_scheduler_add_fd(thread, listen_fd, nos_ipc_accept_handler, thread);
}

/**
 * @brief 清理 IPC 统计数据
 */
void nos_ipc_stats_clear(void) {
    memset(&g_node_ctx.stats, 0, sizeof(g_node_ctx.stats));
}

/**
 * @brief 获取 IPC 连接镜像供 CLI 展示
 */
uint32_t nos_ipc_get_conn_snapshot(char *out_buf, uint32_t max_count) {
    pthread_mutex_lock(&g_pool_lock);
    uint32_t count = (g_remote_conn_count < max_count) ? g_remote_conn_count : max_count;
    for (uint32_t i = 0; i < count; i++) {
        nos_ipc_conn_t *c = &g_remote_conns[i];
        pthread_mutex_lock(&c->lock);
        /* 格式: Node | Path | FD | Status | Queue(H/T) | Congest */
        sprintf(out_buf + (i * 256), "%-15s %-25s %-4d %-10s %u/%u %s", 
                c->node_name, c->uds_path, c->fd, c->is_connected ? "Connected" : "Failed",
                c->head, c->tail, c->is_congested ? "YES" : "NO");
        pthread_mutex_unlock(&c->lock);
    }
    pthread_mutex_unlock(&g_pool_lock);
    return count;
}
