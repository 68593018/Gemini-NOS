#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include "nos_log.h"

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static nos_log_level_t g_filter_level = NOS_LOG_LEVEL_DEBUG; // 修改默认级别为 DEBUG 以便于看到初始化信息

static const char* level_to_str(nos_log_level_t level) {
    switch(level) {
        case NOS_LOG_LEVEL_DEBUG: return "DEBUG";
        case NOS_LOG_LEVEL_INFO:  return "INFO ";
        case NOS_LOG_LEVEL_WARN:  return "WARN ";
        case NOS_LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKN ";
    }
}

static void nos_log_impl(nos_log_level_t level, const char *comp_name, const char *fmt, ...) {
    if (level < g_filter_level) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", t); // 简化时间戳显示

    pthread_mutex_lock(&g_log_lock);
    
    printf("[%s] [%s] [%s] ", time_str, level_to_str(level), comp_name ? comp_name : "System");
    
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
    
    pthread_mutex_unlock(&g_log_lock);
}

static void nos_set_level_impl(nos_log_level_t level) {
    g_filter_level = level;
    nos_log_impl(NOS_LOG_LEVEL_INFO, "Log", "Global filter level set to %s", level_to_str(level));
}

static nos_log_ops_t g_log_ops = {
    .log = nos_log_impl,
    .set_filter_level = nos_set_level_impl
};

void nos_log_init(void) {
    extern nos_status_t nos_embedded_service_register(const char *name, void *ops);
    nos_embedded_service_register("SVC_LOG", &g_log_ops);
}
