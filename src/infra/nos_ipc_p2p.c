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
        if (ret == 0) printf("[IPC] Remote closed connection (FD:%d).\n", client_fd);
        else perror("recv header");
        goto err_close;
    }

    if (ret < (ssize_t)sizeof(header)) {
        printf("[IPC] Partial header received, protocol out of sync. Closing.\n");
        goto err_close;
    }

    /* 2. 幻数与版本强校验 */
    if (header.magic != NOS_IPC_MAGIC) {
        printf("[IPC] Protocol error: Invalid magic 0x%04X from FD %d. Dropping.\n", 
               header.magic, client_fd);
        goto err_close;
    }

    if (header.version != NOS_IPC_VERSION) {
        printf("[IPC] Protocol error: Version mismatch (%u != %u).\n", 
               header.version, NOS_IPC_VERSION);
        goto err_close;
    }

    /* 3. 分配内存并读取 Payload */
    size_t full_len = sizeof(nos_service_msg_t) + header.payload_len;
    nos_service_msg_t *msg = malloc(full_len);
    memcpy(msg, &header, sizeof(header));

    if (header.payload_len > 0) {
        ret = recv(client_fd, msg->payload, header.payload_len, MSG_WAITALL);
        if (ret < (ssize_t)header.payload_len) {
            printf("[IPC] Failed to read complete payload.\n");
            free(msg);
            goto err_close;
        }
    }

    printf("[IPC] Received valid msg from FD %d: Service=%u, Code=%u, Len=%u\n", 
           client_fd, msg->dst_service, msg->msg_code, msg->payload_len);

    /* 4. 注入本地调度器 */
    nos_service_msg_send(msg); 
    free(msg);
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
        perror("IPC Bind");
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
