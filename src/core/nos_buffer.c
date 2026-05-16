#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "nos_buffer.h"

/* 内部管理的内存块定义 */
static nos_buffer_t g_buffer_pool[NOS_BUFFER_COUNT];
static uint8_t g_buffer_raw_data[NOS_BUFFER_COUNT][NOS_BUFFER_SIZE];

/* 简单的空闲索引管理栈 */
static uint32_t g_free_stack[NOS_BUFFER_COUNT];
static int g_free_top = -1;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

nos_status_t nos_buffer_init_pool(void) {
    pthread_mutex_lock(&g_pool_lock);
    for (int i = 0; i < NOS_BUFFER_COUNT; i++) {
        g_buffer_pool[i].data = g_buffer_raw_data[i];
        g_buffer_pool[i].capacity = NOS_BUFFER_SIZE;
        g_buffer_pool[i].len = 0;
        g_buffer_pool[i].pool_idx = i;
        atomic_init(&g_buffer_pool[i].ref_cnt, 0);
        
        /* 存入空闲栈 */
        g_free_stack[++g_free_top] = i;
    }
    pthread_mutex_unlock(&g_pool_lock);
    printf("[Buffer] Pool initialized with %d blocks of %d bytes.\n", 
           NOS_BUFFER_COUNT, NOS_BUFFER_SIZE);
    return NOS_OK;
}

nos_buffer_t* nos_buffer_alloc(void) {
    nos_buffer_t *buf = NULL;
    pthread_mutex_lock(&g_pool_lock);
    if (g_free_top >= 0) {
        uint32_t idx = g_free_stack[g_free_top--];
        buf = &g_buffer_pool[idx];
        atomic_store(&buf->ref_cnt, 1);
        buf->len = 0;
    }
    pthread_mutex_unlock(&g_pool_lock);
    return buf;
}

void nos_buffer_retain(nos_buffer_t *buf) {
    if (buf) {
        atomic_fetch_add(&buf->ref_cnt, 1);
    }
}

void nos_buffer_release(nos_buffer_t *buf) {
    if (!buf) return;

    if (atomic_fetch_sub(&buf->ref_cnt, 1) == 1) {
        /* 计数减到 0，归还池 */
        pthread_mutex_lock(&g_pool_lock);
        g_free_stack[++g_free_top] = buf->pool_idx;
        pthread_mutex_unlock(&g_pool_lock);
    }
}
