#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include "nos_scheduler.h"
#include "nos_service.h"

#define MAX_QUEUE_SIZE 1024
#define MAX_EVENTS 10

/* 全局服务注册表 (简化版：Service ID -> Component Pointer) */
typedef struct {
    uint32_t service_id;
    nos_component_t *provider;
} service_entry_t;

static service_entry_t g_service_registry[64];
static uint32_t g_service_count = 0;

/* 当前主线程指针 (单线程演示用) */
static nos_thread_t *g_main_thread_ptr = NULL;

nos_status_t nos_scheduler_init_thread(nos_thread_t *thread, uint32_t id, const char *name) {
    thread->thread_id = id;
    thread->name = name;
    thread->component_count = 0;
    thread->components = malloc(sizeof(nos_component_t*) * 32);
    
    thread->fd_count = 0;
    thread->fd_entries = malloc(sizeof(nos_fd_entry_t) * 32);

    /* 初始化消息队列 */
    thread->msg_queue = malloc(sizeof(nos_service_msg_t*) * MAX_QUEUE_SIZE);
    thread->head = thread->tail = 0;
    pthread_mutex_init(&thread->queue_lock, NULL);

    /* 初始化 epoll 和唤醒管道 */
    thread->epoll_fd = epoll_create1(0);
    pipe(thread->notify_fd);
    
    // 设置 pipe 读端为非阻塞
    fcntl(thread->notify_fd[0], F_SETFL, O_NONBLOCK);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = thread->notify_fd[0]; // 唤醒管道特殊处理，直接存 FD
    epoll_ctl(thread->epoll_fd, EPOLL_CTL_ADD, thread->notify_fd[0], &ev);

    g_main_thread_ptr = thread;
    return NOS_OK;
}

nos_status_t nos_service_register_provider(uint32_t service_id, nos_component_t *provider) {
    if (g_service_count >= 64) return NOS_ERR;
    g_service_registry[g_service_count].service_id = service_id;
    g_service_registry[g_service_count].provider = provider;
    g_service_count++;
    return NOS_OK;
}

nos_status_t nos_scheduler_register_component(uint32_t thread_id, nos_component_t *comp) {
    if (!g_main_thread_ptr) return NOS_ERR;
    g_main_thread_ptr->components[g_main_thread_ptr->component_count++] = comp;
    return NOS_OK;
}

nos_status_t nos_scheduler_add_fd(nos_thread_t *thread, int fd, nos_fd_callback_t callback, void *arg) {
    if (thread->fd_count >= 32) return NOS_ERR;

    nos_fd_entry_t *entry = &thread->fd_entries[thread->fd_count];
    entry->fd = fd;
    entry->callback = callback;
    entry->arg = arg;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = entry; // 携带回调条目信息
    if (epoll_ctl(thread->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl_add_fd");
        return NOS_ERR;
    }

    thread->fd_count++;
    return NOS_OK;
}

/* 异步发送消息：写入队列并唤醒 epoll */
nos_status_t nos_service_msg_send(nos_service_msg_t *msg) {
    nos_thread_t *target_thread = g_main_thread_ptr; // 演示版固定主线程
    
    pthread_mutex_lock(&target_thread->queue_lock);
    int next_tail = (target_thread->tail + 1) % MAX_QUEUE_SIZE;
    if (next_tail == target_thread->head) {
        pthread_mutex_unlock(&target_thread->queue_lock);
        return NOS_ERR_BUSY;
    }

    size_t full_len = sizeof(nos_service_msg_t) + msg->payload_len;
    nos_service_msg_t *new_msg = malloc(full_len);
    memcpy(new_msg, msg, full_len);

    target_thread->msg_queue[target_thread->tail] = new_msg;
    target_thread->tail = next_tail;
    pthread_mutex_unlock(&target_thread->queue_lock);

    /* 唤醒 epoll_wait */
    char notify_cmd = 'm';
    write(target_thread->notify_fd[1], &notify_cmd, 1);
    
    return NOS_OK;
}

static void process_thread_messages(nos_thread_t *self) {
    char buf[16];
    while (read(self->notify_fd[0], buf, sizeof(buf)) > 0); // 清空管道

    while (1) {
        nos_service_msg_t *msg = NULL;
        pthread_mutex_lock(&self->queue_lock);
        if (self->head != self->tail) {
            msg = self->msg_queue[self->head];
            self->head = (self->head + 1) % MAX_QUEUE_SIZE;
        }
        pthread_mutex_unlock(&self->queue_lock);

        if (!msg) break;

        /* 动态寻址并分发 */
        nos_component_t *target = NULL;
        if (msg->dst_service != 0) {
            for (uint32_t i = 0; i < g_service_count; i++) {
                if (g_service_registry[i].service_id == msg->dst_service) {
                    target = g_service_registry[i].provider;
                    break;
                }
            }
        } else {
            // 点对点回包逻辑：找到 ID 匹配的组件
            for (uint32_t i = 0; i < self->component_count; i++) {
                if (self->components[i]->id == 1) { // 演示简化：回包给 ID=1
                    target = self->components[i];
                    break;
                }
            }
        }

        if (target && target->on_msg) {
            target->on_msg(target, msg);
        }
        free(msg);
    }
}

nos_status_t nos_scheduler_run_loop(nos_thread_t *self) {
    printf("[Scheduler] Thread '%s' (epoll-driven) loop started.\n", self->name);
    struct epoll_event events[MAX_EVENTS];
    
    while (1) {
        int nfds = epoll_wait(self->epoll_fd, events, MAX_EVENTS, -1); 
        if (nfds < 0) break;

        for (int i = 0; i < nfds; i++) {
            /* 
             * 区分唤醒管道和外部 FD：
             * 唤醒管道注册时使用的是 ev.data.fd，
             * 外部 FD 注册时使用的是 ev.data.ptr。
             */
            if (events[i].data.fd == self->notify_fd[0]) {
                process_thread_messages(self);
            } else {
                /* 处理外部 FD 回调 */
                nos_fd_entry_t *entry = (nos_fd_entry_t *)events[i].data.ptr;
                if (entry && entry->callback) {
                    entry->callback(entry->fd, entry->arg);
                }
            }
        }
    }
    return NOS_OK;
}
