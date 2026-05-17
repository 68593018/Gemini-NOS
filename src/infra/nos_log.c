#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <string.h>
#include "nos_log.h"

#define LOG_BUFFER_SIZE 1024
#define LOG_MSG_MAX 256
#define MAX_COMP_FILTERS 32

typedef struct {
    nos_log_level_t level;
    char comp_name[32];
    char time_str[16];
    char message[LOG_MSG_MAX];
    atomic_int ready;
} nos_log_entry_t;

typedef struct {
    const char *name;
    nos_log_level_t level;
} nos_comp_filter_t;

/**
 * @brief 日志系统内部上下文
 */
typedef struct {
    nos_log_entry_t ring[LOG_BUFFER_SIZE];
    _Atomic unsigned int write_idx;
    uint32_t read_idx;

    nos_log_level_t global_filter_level;
    nos_comp_filter_t comp_filters[MAX_COMP_FILTERS];
    int comp_filter_count;
    pthread_mutex_t filter_lock;

    pthread_t consumer_tid;
    int initialized;
} nos_log_context_t;

static nos_log_context_t g_log_ctx = {
    .write_idx = 0,
    .read_idx = 0,
    .global_filter_level = NOS_LOG_LEVEL_DEBUG,
    .comp_filter_count = 0,
    .filter_lock = PTHREAD_MUTEX_INITIALIZER,
    .initialized = 0
};

static const char* level_to_str(nos_log_level_t level) {
    switch(level) {
        case NOS_LOG_LEVEL_DEBUG: return "DEBUG";
        case NOS_LOG_LEVEL_INFO:  return "INFO ";
        case NOS_LOG_LEVEL_WARN:  return "WARN ";
        case NOS_LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKN ";
    }
}

static void* nos_log_consumer_thread(void *arg) {
    nos_log_context_t *ctx = &g_log_ctx;
    while (1) {
        nos_log_entry_t *entry = &ctx->ring[ctx->read_idx % LOG_BUFFER_SIZE];
        if (atomic_load(&entry->ready) == 1) {
            printf("[%s] [%s] [%-8s] %s\n", 
                   entry->time_str, level_to_str(entry->level), entry->comp_name, entry->message);
            fflush(stdout);
            atomic_store(&entry->ready, 0);
            ctx->read_idx++;
        } else {
            usleep(1000); // 1ms sleep to yield CPU
        }
    }
    return NULL;
}

static void nos_log_impl(nos_log_level_t level, const char *comp_name, const char *fmt, ...) {
    nos_log_context_t *ctx = &g_log_ctx;
    nos_log_level_t filter_level = ctx->global_filter_level;

    if (comp_name) {
        pthread_mutex_lock(&ctx->filter_lock);
        for (int i = 0; i < ctx->comp_filter_count; i++) {
            if (strcmp(ctx->comp_filters[i].name, comp_name) == 0) {
                filter_level = ctx->comp_filters[i].level;
                break;
            }
        }
        pthread_mutex_unlock(&ctx->filter_lock);
    }

    if (level < filter_level) return;

    unsigned int idx = atomic_fetch_add(&ctx->write_idx, 1) % LOG_BUFFER_SIZE;
    nos_log_entry_t *entry = &ctx->ring[idx];

    // Wait for the slot to be consumed if we wrap around
    while (atomic_load(&entry->ready) == 1) {
        usleep(100);
    }

    entry->level = level;
    strncpy(entry->comp_name, comp_name ? comp_name : "System", sizeof(entry->comp_name) - 1);
    
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(entry->time_str, sizeof(entry->time_str), "%H:%M:%S", t);

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry->message, sizeof(entry->message), fmt, args);
    va_end(args);

    atomic_store(&entry->ready, 1);
}

static void nos_set_level_impl(nos_log_level_t level) {
    g_log_ctx.global_filter_level = level;
    nos_log_impl(NOS_LOG_LEVEL_INFO, "Log", "Global filter level set to %s", level_to_str(level));
}

static void nos_set_comp_level_impl(const char *comp_name, nos_log_level_t level) {
    if (!comp_name) return;
    nos_log_context_t *ctx = &g_log_ctx;

    pthread_mutex_lock(&ctx->filter_lock);
    int found = 0;
    for (int i = 0; i < ctx->comp_filter_count; i++) {
        if (strcmp(ctx->comp_filters[i].name, comp_name) == 0) {
            ctx->comp_filters[i].level = level;
            found = 1;
            break;
        }
    }
    if (!found && ctx->comp_filter_count < MAX_COMP_FILTERS) {
        ctx->comp_filters[ctx->comp_filter_count].name = strdup(comp_name);
        ctx->comp_filters[ctx->comp_filter_count].level = level;
        ctx->comp_filter_count++;
    }
    pthread_mutex_unlock(&ctx->filter_lock);
    
    nos_log_impl(NOS_LOG_LEVEL_INFO, "Log", "Component [%s] filter level set to %s", comp_name, level_to_str(level));
}

static nos_log_ops_t g_log_ops = {
    .log = nos_log_impl,
    .set_filter_level = nos_set_level_impl,
    .set_comp_level = nos_set_comp_level_impl
};

void nos_log_init(void) {
    nos_log_context_t *ctx = &g_log_ctx;
    if (ctx->initialized) return;

    for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
        atomic_init(&ctx->ring[i].ready, 0);
    }

    pthread_create(&ctx->consumer_tid, NULL, nos_log_consumer_thread, NULL);
    pthread_detach(ctx->consumer_tid);

    extern nos_status_t nos_embedded_service_register(const char *name, void *ops);
    nos_embedded_service_register("SVC_LOG", &g_log_ops);
    
    ctx->initialized = 1;
}
