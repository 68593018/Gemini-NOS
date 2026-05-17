#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include "nos_scheduler.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_api.h"

/**
 * @brief 处理已建立连接的 Socket 数据读取
 */
static void nos_ipc_data_handler(int client_fd, void *arg) {
    nos_thread_t *thread = (nos_thread_t *)arg;
    nos_service_msg_t header;
    
    /* 1. 非阻塞读取 Header */
    ssize_t ret = recv(client_fd, &header, sizeof(header), 0);
    if (ret <= 0) {
        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (ret == 0) nos_sys_log_info("Remote closed connection (FD:%d).", client_fd);
        else nos_sys_log_error("recv header failed: %s", strerror(errno));
        goto err_close;
    }

    if (ret < (ssize_t)sizeof(header)) {
        nos_sys_log_error("Partial header received. Protocol out of sync.");
        goto err_close;
    }

    /* 2. 幻数与版本强校验 */
    if (header.magic != NOS_IPC_MAGIC) {
        nos_sys_log_error("Protocol error: Invalid magic 0x%04X. Dropping.", header.magic);
        goto err_close;
    }

    /* 3. 申请 Buffer 池内存并读取 Payload */
    nos_buffer_t *buf = nos_buffer_alloc(sizeof(header) + header.payload_len, 0);
    if (!buf) {
        nos_sys_log_error("Buffer Pool empty! Dropping message.");
        goto err_close;
    }

    /* 将已读取的 Header 考入 Buffer */
    memcpy(buf->data, &header, sizeof(header));
    buf->len = sizeof(header);

    if (header.payload_len > 0) {
        /* 读取剩余 Payload 直接存入 Buffer 数据区 */
        ret = recv(client_fd, buf->data + sizeof(header), header.payload_len, MSG_WAITALL);
        if (ret < (ssize_t)header.payload_len) {
            nos_sys_log_error("Failed to read complete payload.");
            nos_buffer_release(buf);
            goto err_close;
        }
        buf->len += ret;
    }

    nos_sys_log_debug("Received msg from FD %d via BufferPool: Service=%u, Len=%u", 
           client_fd, header.dst_service, buf->len);

    /* 4. 注入本地调度器 (所有权转移) */
    nos_service_msg_send(buf); 
    
    /* 
     * 注意：nos_service_msg_send 内部会调用 retain，
     * 但本函数作为“生产者”申请的 Buffer，使命已完成，应调用一次 release。
     */
    nos_buffer_release(buf);
    return;

err_close:
    nos_scheduler_remove_fd(thread, client_fd);
    close(client_fd);
}

/**
 * @brief 处理监听 Socket 的新连接事件
 */
static void nos_ipc_accept_handler(int listen_fd, void *arg) {
    nos_thread_t *thread = (nos_thread_t *)arg;
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) return;

    /* 设置为非阻塞 */
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    /* 将新连接加入 epoll 监听数据 */
    nos_scheduler_add_fd(thread, client_fd, nos_ipc_data_handler, thread);
}

/**
 * @brief 初始化本地 IPC 监听
 */
nos_status_t nos_ipc_init(nos_thread_t *thread, const char *uds_path) {
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) return NOS_ERR;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path) - 1);

    unlink(uds_path); // 确保路径可用
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        nos_sys_log_error("IPC Bind failed: %s", strerror(errno));
        close(listen_fd);
        return NOS_ERR;
    }

    if (listen(listen_fd, 5) < 0) {
        close(listen_fd);
        return NOS_ERR;
    }

    /* 注册到调度器 */
    return nos_scheduler_add_fd(thread, listen_fd, nos_ipc_accept_handler, thread);
}
