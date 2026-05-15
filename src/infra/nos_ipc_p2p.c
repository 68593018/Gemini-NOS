#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include "nos_scheduler.h"
#include "nos_service.h"

/**
 * @brief 处理已建立连接的 Socket 数据读取
 */
static void nos_ipc_data_handler(int client_fd, void *arg) {
    nos_service_msg_t header;
    ssize_t ret = recv(client_fd, &header, sizeof(header), MSG_PEEK);
    if (ret <= 0) {
        if (ret == 0) printf("[IPC] Remote closed connection.\n");
        close(client_fd);
        // 注意：这里需要从 epoll 中移除，演示版暂不处理 FD 移除逻辑
        return;
    }

    /* 读取完整报文 */
    size_t full_len = sizeof(nos_service_msg_t) + header.payload_len;
    nos_service_msg_t *msg = malloc(full_len);
    ret = recv(client_fd, msg, full_len, MSG_WAITALL);
    if (ret < (ssize_t)full_len) {
        free(msg);
        close(client_fd);
        return;
    }

    printf("[IPC] Received cross-process msg: Service=%u, Code=%u\n", 
           msg->dst_service, msg->msg_code);

    /* 注入本地调度器 */
    nos_service_msg_send(msg); 
    
    free(msg);
    close(client_fd); // 演示版：处理完即关闭（短连接）
}

/**
 * @brief 处理监听 Socket 的新连接事件
 */
static void nos_ipc_accept_handler(int listen_fd, void *arg) {
    nos_thread_t *thread = (nos_thread_t *)arg;
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) return;

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
