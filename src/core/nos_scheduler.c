#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <time.h>
#include "nos_scheduler.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_api.h"

#define MAX_QUEUE_SIZE 1024
#define MAX_EVENTS 10

/* 线程局部变量：存储当前线程的调度器指针 */
static __thread nos_thread_t *t_local_scheduler = NULL;

/* 服务注册表项 */
typedef struct {
    uint32_t service_id;
    nos_component_t *local_provider;
    nos_thread_t *local_thread;
    char remote_uds_path[108];
    int is_remote;
} service_entry_t;

static service_entry_t g_service_registry[64];
static uint32_t g_service_count = 0;

static service_entry_t* find_service_entry(uint32_t service_id) {
    for (uint32_t i = 0; i < g_service_count; i++) {
        if (g_service_registry[i].service_id == service_id) return &g_service_registry[i];
    }
    return NULL;
}

/* --- 定时器最小堆私有实现 --- */

static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void heapify_up(nos_thread_t *thread, uint32_t idx) {
    while (idx > 0) {
        uint32_t parent = (idx - 1) / 2;
        if (thread->timer_heap.nodes[idx]->expire_at_ms >= thread->timer_heap.nodes[parent]->expire_at_ms) break;
        nos_timer_node_t *tmp = thread->timer_heap.nodes[idx];
        thread->timer_heap.nodes[idx] = thread->timer_heap.nodes[parent];
        thread->timer_heap.nodes[parent] = tmp;
        idx = parent;
    }
}

static void heapify_down(nos_thread_t *thread, uint32_t idx) {
    while (idx * 2 + 1 < thread->timer_heap.size) {
        uint32_t smallest = idx * 2 + 1;
        uint32_t right = idx * 2 + 2;
        if (right < thread->timer_heap.size && thread->timer_heap.nodes[right]->expire_at_ms < thread->timer_heap.nodes[smallest]->expire_at_ms) {
            smallest = right;
        }
        if (thread->timer_heap.nodes[idx]->expire_at_ms <= thread->timer_heap.nodes[smallest]->expire_at_ms) break;
        nos_timer_node_t *tmp = thread->timer_heap.nodes[idx];
        thread->timer_heap.nodes[idx] = thread->timer_heap.nodes[smallest];
        thread->timer_heap.nodes[smallest] = tmp;
        idx = smallest;
    }
}

static void free_timer_node(nos_timer_node_t *node) {
    if (node->free_arg) node->free_arg(node->arg);
    free(node);
}

static void process_timers(nos_thread_t *self) {
    uint64_t now = get_monotonic_ms();
    while (self->timer_heap.size > 0 && now >= self->timer_heap.nodes[0]->expire_at_ms) {
        nos_timer_node_t *node = self->timer_heap.nodes[0];
        self->timer_heap.nodes[0] = self->timer_heap.nodes[--self->timer_heap.size];
        heapify_down(self, 0);

        if (node->callback) node->callback(node->arg);

        if (node->is_periodic) {
            node->expire_at_ms = get_monotonic_ms() + node->interval_ms;
            self->timer_heap.nodes[self->timer_heap.size] = node;
            heapify_up(self, self->timer_heap.size++);
        } else {
            free_timer_node(node);
        }
        now = get_monotonic_ms();
    }
}

/* --- 调度器核心实现 --- */

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
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = thread->notify_fd[0]};
    epoll_ctl(thread->epoll_fd, EPOLL_CTL_ADD, thread->notify_fd[0], &ev);

    /* 初始化本地堆 */
    thread->timer_heap.capacity = 128;
    thread->timer_heap.size = 0;
    thread->timer_heap.nodes = calloc(thread->timer_heap.capacity, sizeof(nos_timer_node_t*));

    return NOS_OK;
}

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
            thread->components[i] = thread->components[thread->component_count - 1];
            thread->component_count--;
            return NOS_OK;
        }
    }
    return NOS_ERR;
}

nos_status_t nos_service_unregister_provider(uint32_t service_id) {
    for (uint32_t i = 0; i < g_service_count; i++) {
        if (g_service_registry[i].service_id == service_id) {
            g_service_registry[i] = g_service_registry[g_service_count - 1];
            g_service_count--;
            return NOS_OK;
        }
    }
    return NOS_ERR;
}

nos_status_t nos_scheduler_add_fd(nos_thread_t *thread, int fd, nos_fd_callback_t callback, void *arg) {
    if (thread->fd_count >= 32) return NOS_ERR;
    nos_fd_entry_t *entry = &thread->fd_entries[thread->fd_count++];
    entry->fd = fd; entry->callback = callback; entry->arg = arg;
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = entry};
    return (epoll_ctl(thread->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) ? NOS_ERR : NOS_OK;
}

nos_status_t nos_scheduler_remove_fd(nos_thread_t *thread, int fd) {
    epoll_ctl(thread->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    for (uint32_t i = 0; i < thread->fd_count; i++) {
        if (thread->fd_entries[i].fd == fd) {
            thread->fd_entries[i] = thread->fd_entries[thread->fd_count - 1];
            thread->fd_count--; return NOS_OK;
        }
    }
    return NOS_ERR;
}

static nos_status_t nos_transport_local_send(nos_buffer_t *buf, nos_thread_t *target_thread) {
    pthread_mutex_lock(&target_thread->queue_lock);
    int next_tail = (target_thread->tail + 1) % MAX_QUEUE_SIZE;
    if (next_tail == target_thread->head) { pthread_mutex_unlock(&target_thread->queue_lock); return NOS_ERR_BUSY; }
    nos_buffer_retain(buf);
    target_thread->msg_queue[target_thread->tail] = buf;
    target_thread->tail = next_tail;
    pthread_mutex_unlock(&target_thread->queue_lock);
    char notify_cmd = 'm'; write(target_thread->notify_fd[1], &notify_cmd, 1);
    return NOS_OK;
}

/* --- 出站连接池 --- */
typedef struct { char uds_path[108]; int fd; } nos_uds_conn_t;
static nos_uds_conn_t g_uds_conn_pool[16];
static uint32_t g_uds_conn_count = 0;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static int get_or_create_uds_conn(const char *uds_path) {
    pthread_mutex_lock(&g_pool_lock);
    for (uint32_t i = 0; i < g_uds_conn_count; i++) {
        if (strcmp(g_uds_conn_pool[i].uds_path, uds_path) == 0) {
            int fd = g_uds_conn_pool[i].fd; pthread_mutex_unlock(&g_pool_lock); return fd;
        }
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) { close(fd); fd = -1; }
        if (fd >= 0 && g_uds_conn_count < 16) {
            strncpy(g_uds_conn_pool[g_uds_conn_count].uds_path, uds_path, 107);
            g_uds_conn_pool[g_uds_conn_count++].fd = fd;
        }
    }
    pthread_mutex_unlock(&g_pool_lock); return fd;
}

static nos_status_t nos_transport_remote_uds_send(nos_buffer_t *buf, const char *uds_path) {
    nos_service_msg_t *header = (nos_service_msg_t *)buf->data;
    size_t full_len = sizeof(nos_service_msg_t) + header->payload_len;
    int fd = get_or_create_uds_conn(uds_path);
    if (fd < 0) return NOS_ERR;
    ssize_t sent = send(fd, buf->data, full_len, MSG_NOSIGNAL);
    if (sent == (ssize_t)full_len) return NOS_OK;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return NOS_ERR_BUSY;
    pthread_mutex_lock(&g_pool_lock);
    for (uint32_t i = 0; i < g_uds_conn_count; i++) {
        if (strcmp(g_uds_conn_pool[i].uds_path, uds_path) == 0) {
            close(g_uds_conn_pool[i].fd);
            g_uds_conn_pool[i] = g_uds_conn_pool[--g_uds_conn_count]; break;
        }
    }
    pthread_mutex_unlock(&g_pool_lock); return NOS_ERR;
}

nos_status_t nos_service_msg_send(nos_buffer_t *buf) {
    nos_service_msg_t *header = (nos_service_msg_t *)buf->data;
    service_entry_t *entry = find_service_entry(header->dst_service);
    if (entry) {
        return entry->is_remote ? nos_transport_remote_uds_send(buf, entry->remote_uds_path) : nos_transport_local_send(buf, entry->local_thread);
    }
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
        service_entry_t *entry = find_service_entry(header->dst_service);
        if (entry && entry->local_provider && entry->local_provider->on_msg) entry->local_provider->on_msg(entry->local_provider, header);
        nos_buffer_release(buf);
    }
}

void nos_scheduler_stop(nos_thread_t *thread) {
    char notify_cmd = 'q'; write(thread->notify_fd[1], &notify_cmd, 1);
}

nos_status_t nos_scheduler_run_loop(nos_thread_t *self) {
    t_local_scheduler = self;
    for (uint32_t i = 0; i < self->component_count; i++) {
        nos_component_t *comp = self->components[i];
        if (comp->start && comp->status != NOS_COMP_ST_ACTIVE) {
            if (comp->start(comp) == NOS_OK) comp->status = NOS_COMP_ST_ACTIVE;
        }
    }
    nos_sys_log_info("Thread '%s' (TID: %lu) loop started.", self->name, pthread_self());
    struct epoll_event events[MAX_EVENTS];
    int running = 1;
    while (running) {
        int timeout = -1;
        if (self->timer_heap.size > 0) {
            uint64_t now = get_monotonic_ms();
            timeout = (self->timer_heap.nodes[0]->expire_at_ms <= now) ? 0 : (int)(self->timer_heap.nodes[0]->expire_at_ms - now);
        }
        int nfds = epoll_wait(self->epoll_fd, events, MAX_EVENTS, timeout);
        process_timers(self);
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == self->notify_fd[0]) {
                char cmd; if (read(self->notify_fd[0], &cmd, 1) > 0) {
                    if (cmd == 'q') { running = 0; nos_sys_log_info("Thread '%s' exit command received.", self->name); }
                    else if (cmd == 'm') process_thread_messages(self);
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

/* --- 公开定时器接口实现 --- */
static uint32_t g_next_timer_id = 1;

nos_status_t nos_timer_start(uint32_t interval_ms, int is_periodic, nos_timer_cb_t cb, void *arg, nos_timer_free_arg_t free_arg, uint32_t *out_id) {
    nos_thread_t *self = t_local_scheduler;
    if (!self || !cb || self->timer_heap.size >= self->timer_heap.capacity) return NOS_ERR;
    nos_timer_node_t *node = malloc(sizeof(nos_timer_node_t));
    node->timer_id = __atomic_fetch_add(&g_next_timer_id, 1, __ATOMIC_RELAXED);
    node->interval_ms = interval_ms; node->expire_at_ms = get_monotonic_ms() + interval_ms;
    node->is_periodic = is_periodic; node->callback = cb; node->arg = arg; node->free_arg = free_arg;
    self->timer_heap.nodes[self->timer_heap.size] = node;
    heapify_up(self, self->timer_heap.size++);
    if (out_id) *out_id = node->timer_id;
    char notify_cmd = 't'; write(self->notify_fd[1], &notify_cmd, 1);
    return NOS_OK;
}

nos_status_t nos_timer_stop(uint32_t timer_id) {
    nos_thread_t *self = t_local_scheduler;
    if (!self) return NOS_ERR;
    for (uint32_t i = 0; i < self->timer_heap.size; i++) {
        if (self->timer_heap.nodes[i]->timer_id == timer_id) {
            free_timer_node(self->timer_heap.nodes[i]);
            self->timer_heap.nodes[i] = self->timer_heap.nodes[--self->timer_heap.size];
            heapify_down(self, i); return NOS_OK;
        }
    }
    return NOS_ERR;
}
