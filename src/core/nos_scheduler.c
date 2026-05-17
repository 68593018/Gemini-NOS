#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include "nos_scheduler.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_api.h"

#define MAX_QUEUE_SIZE 1024
#define MAX_EVENTS 10

/* 服务注册表项 */
typedef struct {
    uint32_t service_id;
    nos_component_t *local_provider;
    nos_thread_t *local_thread;  /**< 该服务所在的调度器线程 */
    char remote_uds_path[108];
    int is_remote;
} service_entry_t;

static service_entry_t g_service_registry[64];
static uint32_t g_service_count = 0;

/* 查找注册表 */
static service_entry_t* find_service_entry(uint32_t service_id) {
    for (uint32_t i = 0; i < g_service_count; i++) {
        if (g_service_registry[i].service_id == service_id) {
            return &g_service_registry[i];
        }
    }
    return NULL;
}

nos_status_t nos_scheduler_init_thread(nos_thread_t *thread, uint32_t id, const char *name) {
    thread->thread_id = id;
    thread->name = name;
    thread->component_count = 0;
    thread->components = malloc(sizeof(nos_component_t*) * 32);
    
    thread->fd_count = 0;
    thread->fd_entries = malloc(sizeof(nos_fd_entry_t) * 32);

    thread->msg_queue = malloc(sizeof(nos_buffer_t*) * MAX_QUEUE_SIZE);
    thread->head = thread->tail = 0;
    pthread_mutex_init(&thread->queue_lock, NULL);

    thread->epoll_fd = epoll_create1(0);
    pipe(thread->notify_fd);
    fcntl(thread->notify_fd[0], F_SETFL, O_NONBLOCK);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = thread->notify_fd[0];
    epoll_ctl(thread->epoll_fd, EPOLL_CTL_ADD, thread->notify_fd[0], &ev);

    return NOS_OK;
}

nos_status_t nos_service_register_provider(uint32_t service_id, nos_component_t *provider) {
    /* 
     * 注意：在多线程架构下，需要知道该组件被挂载到了哪个线程。
     * 演示版简单处理：此处仅存组件，线程绑定由 nos_service_register_provider_bind 显式调用。
     */
    return NOS_ERR; // 请使用带 Thread 的接口
}

/**
 * @brief 内部接口：将服务绑定到特定线程
 */
nos_status_t nos_service_register_provider_bind(uint32_t service_id, nos_component_t *provider, nos_thread_t *thread) {
    service_entry_t *entry = find_service_entry(service_id);
    if (!entry) {
        if (g_service_count >= 64) return NOS_ERR;
        entry = &g_service_registry[g_service_count++];
    }
    entry->service_id = service_id;
    entry->local_provider = provider;
    entry->local_thread = thread;
    entry->is_remote = 0;
    return NOS_OK;
}

nos_status_t nos_service_register_remote(uint32_t service_id, const char *uds_path) {
    service_entry_t *entry = find_service_entry(service_id);
    if (!entry) {
        if (g_service_count >= 64) return NOS_ERR;
        entry = &g_service_registry[g_service_count++];
    }
    entry->service_id = service_id;
    entry->is_remote = 1;
    strncpy(entry->remote_uds_path, uds_path, sizeof(entry->remote_uds_path) - 1);
    return NOS_OK;
}

nos_status_t nos_scheduler_register_component(nos_thread_t *thread, nos_component_t *comp) {
    if (!thread || !comp) return NOS_ERR;
    if (thread->component_count >= 32) return NOS_ERR;
    thread->components[thread->component_count++] = comp;
    return NOS_OK;
}

nos_status_t nos_scheduler_unregister_component(nos_thread_t *thread, nos_component_t *comp) {
    if (!thread || !comp) return NOS_ERR;
    for (uint32_t i = 0; i < thread->component_count; i++) {
        if (thread->components[i] == comp) {
            if (i != thread->component_count - 1) {
                thread->components[i] = thread->components[thread->component_count - 1];
            }
            thread->component_count--;
            return NOS_OK;
        }
    }
    return NOS_ERR;
}

nos_status_t nos_service_unregister_provider(uint32_t service_id) {
    for (uint32_t i = 0; i < g_service_count; i++) {
        if (g_service_registry[i].service_id == service_id) {
            if (i != g_service_count - 1) {
                g_service_registry[i] = g_service_registry[g_service_count - 1];
            }
            g_service_count--;
            return NOS_OK;
        }
    }
    return NOS_ERR;
}

nos_status_t nos_scheduler_add_fd(nos_thread_t *thread, int fd, nos_fd_callback_t callback, void *arg) {
    if (thread->fd_count >= 32) return NOS_ERR;

    nos_fd_entry_t *entry = &thread->fd_entries[thread->fd_count];
    entry->fd = fd;
    entry->callback = callback;
    entry->arg = arg;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = entry;
    if (epoll_ctl(thread->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl_add_fd");
        return NOS_ERR;
    }

    thread->fd_count++;
    return NOS_OK;
}

nos_status_t nos_scheduler_remove_fd(nos_thread_t *thread, int fd) {
    if (epoll_ctl(thread->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        if (errno != ENOENT) perror("epoll_ctl_del");
    }

    for (uint32_t i = 0; i < thread->fd_count; i++) {
        if (thread->fd_entries[i].fd == fd) {
            if (i != thread->fd_count - 1) {
                thread->fd_entries[i] = thread->fd_entries[thread->fd_count - 1];
            }
            thread->fd_count--;
            return NOS_OK;
        }
    }
    return NOS_ERR;
}

/* 本地传输：支持跨线程精准投递 */
static nos_status_t nos_transport_local_send(nos_buffer_t *buf, nos_thread_t *target_thread) {
    if (!target_thread) return NOS_ERR;
    
    pthread_mutex_lock(&target_thread->queue_lock);
    int next_tail = (target_thread->tail + 1) % MAX_QUEUE_SIZE;
    if (next_tail == target_thread->head) {
        pthread_mutex_unlock(&target_thread->queue_lock);
        return NOS_ERR_BUSY;
    }

    nos_buffer_retain(buf);
    target_thread->msg_queue[target_thread->tail] = buf;
    target_thread->tail = next_tail;
    pthread_mutex_unlock(&target_thread->queue_lock);

    /* 唤醒目标线程 */
    char notify_cmd = 'm';
    write(target_thread->notify_fd[1], &notify_cmd, 1);
    
    return NOS_OK;
}

/* 出站连接池（全进程共享） */
typedef struct {
    char uds_path[108];
    int fd;
} nos_uds_conn_t;

static nos_uds_conn_t g_uds_conn_pool[16];
static uint32_t g_uds_conn_count = 0;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static int get_or_create_uds_conn(const char *uds_path) {
    pthread_mutex_lock(&g_pool_lock);
    for (uint32_t i = 0; i < g_uds_conn_count; i++) {
        if (strcmp(g_uds_conn_pool[i].uds_path, uds_path) == 0) {
            int fd = g_uds_conn_pool[i].fd;
            pthread_mutex_unlock(&g_pool_lock);
            return fd;
        }
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            if (errno != EINPROGRESS) { close(fd); fd = -1; }
        }
        if (fd >= 0 && g_uds_conn_count < 16) {
            strncpy(g_uds_conn_pool[g_uds_conn_count].uds_path, uds_path, 107);
            g_uds_conn_pool[g_uds_conn_count].fd = fd;
            g_uds_conn_count++;
        }
    }
    pthread_mutex_unlock(&g_pool_lock);
    return fd;
}

static nos_status_t nos_transport_remote_uds_send(nos_buffer_t *buf, const char *uds_path) {
    int retry = 1;
    nos_service_msg_t *header = (nos_service_msg_t *)buf->data;
    size_t full_len = sizeof(nos_service_msg_t) + header->payload_len;

    while (retry >= 0) {
        int fd = get_or_create_uds_conn(uds_path);
        if (fd < 0) return NOS_ERR;

        ssize_t sent = send(fd, buf->data, full_len, MSG_NOSIGNAL);
        if (sent == (ssize_t)full_len) return NOS_OK;

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return NOS_ERR_BUSY;

        pthread_mutex_lock(&g_pool_lock);
        for (uint32_t i = 0; i < g_uds_conn_count; i++) {
            if (strcmp(g_uds_conn_pool[i].uds_path, uds_path) == 0) {
                close(g_uds_conn_pool[i].fd);
                if (i != g_uds_conn_count - 1) g_uds_conn_pool[i] = g_uds_conn_pool[g_uds_conn_count - 1];
                g_uds_conn_count--;
                break;
            }
        }
        pthread_mutex_unlock(&g_pool_lock);
        retry--;
    }
    return NOS_ERR;
}

nos_status_t nos_service_msg_send(nos_buffer_t *buf) {
    nos_service_msg_t *header = (nos_service_msg_t *)buf->data;
    service_entry_t *entry = find_service_entry(header->dst_service);
    
    if (entry && entry->is_remote) {
        return nos_transport_remote_uds_send(buf, entry->remote_uds_path);
    }

    if (entry && !entry->is_remote) {
        return nos_transport_local_send(buf, entry->local_thread);
    }

    /* 如果没找到路由，默认投递给线程池中第一个（兜底逻辑） */
    return NOS_ERR;
}

static void process_thread_messages(nos_thread_t *self) {
    while (1) {
        nos_buffer_t *buf = NULL;
        pthread_mutex_lock(&self->queue_lock);
        if (self->head != self->tail) {
            buf = self->msg_queue[self->head];
            self->head = (self->head + 1) % MAX_QUEUE_SIZE;
        }
        pthread_mutex_unlock(&self->queue_lock);
        if (!buf) break;

        nos_service_msg_t *header = (nos_service_msg_t *)buf->data;
        nos_component_t *target = NULL;
        if (header->dst_service != 0) {
            service_entry_t *entry = find_service_entry(header->dst_service);
            if (entry) target = entry->local_provider;
        }

        if (target && target->on_msg) target->on_msg(target, header);
        nos_buffer_release(buf);
    }
}

void nos_scheduler_stop(nos_thread_t *thread) {
    if (!thread) return;
    char notify_cmd = 'q'; // 'q' for quit
    write(thread->notify_fd[1], &notify_cmd, 1);
}

nos_status_t nos_scheduler_run_loop(nos_thread_t *self) {
    nos_sys_log_info("Thread '%s' (TID: %lu) loop started.", self->name, pthread_self());
    struct epoll_event events[MAX_EVENTS];
    int running = 1;
    
    while (running) {
        int nfds = epoll_wait(self->epoll_fd, events, MAX_EVENTS, -1); 
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == self->notify_fd[0]) {
                /* 检查指令类型 */
                char cmd;
                if (read(self->notify_fd[0], &cmd, 1) > 0) {
                    if (cmd == 'q') {
                        running = 0;
                        nos_sys_log_info("Thread '%s' exit command received.", self->name);
                    } else if (cmd == 'm') {
                        process_thread_messages(self);
                    }
                }
            } else {
                nos_fd_entry_t *entry = (nos_fd_entry_t *)events[i].data.ptr;
                if (entry && entry->callback) entry->callback(entry->fd, entry->arg);
            }
        }
    }
    nos_sys_log_info("Thread '%s' loop terminated.", self->name);
    return NOS_OK;
}
