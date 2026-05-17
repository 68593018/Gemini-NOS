#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_kv_priv.h"
#include "nos_api.h"

/**
 * @brief MurmurHash3_x86_32 简易实现
 */
static uint32_t murmurhash3_32(const void *key, int len, uint32_t seed) {
    const uint8_t *data = (const uint8_t*)key;
    const int nblocks = len / 4;
    uint32_t h1 = seed;
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;

    const uint32_t *blocks = (const uint32_t *)(data + nblocks * 4);
    for (int i = -nblocks; i; i++) {
        uint32_t k1 = blocks[i];
        k1 *= c1; k1 = (k1 << 15) | (k1 >> 17); k1 *= c2;
        h1 ^= k1; h1 = (h1 << 13) | (h1 >> 19); h1 = h1 * 5 + 0xe6546b64;
    }

    const uint8_t *tail = (const uint8_t*)(data + nblocks * 4);
    uint32_t k1 = 0;
    switch (len & 3) {
        case 3: k1 ^= tail[2] << 16;
        case 2: k1 ^= tail[1] << 8;
        case 1: k1 ^= tail[0]; k1 *= c1; k1 = (k1 << 15) | (k1 >> 17); k1 *= c2; h1 ^= k1;
    }

    h1 ^= len; h1 ^= h1 >> 16; h1 *= 0x85ebca6b; h1 ^= h1 >> 13; h1 *= 0xc2b2ae35; h1 ^= h1 >> 16;
    return h1;
}

#define MAX_TABLES 32
static nos_kv_table_t* g_tables[MAX_TABLES];
static int g_table_count = 0;
static pthread_mutex_t g_table_mgr_lock = PTHREAD_MUTEX_INITIALIZER;

nos_kv_table_t* nos_kv_table_create(const char *name, uint32_t key_size, uint32_t max_val_size, uint32_t capacity) {
    if (!name || key_size == 0 || capacity == 0) return NULL;

    pthread_mutex_lock(&g_table_mgr_lock);
    /* 1. 检查是否已存在同名表 */
    for (int i = 0; i < g_table_count; i++) {
        if (strcmp(g_tables[i]->name, name) == 0) {
            pthread_mutex_unlock(&g_table_mgr_lock);
            return g_tables[i];
        }
    }

    /* 2. 创建新表 */
    if (g_table_count >= MAX_TABLES) {
        pthread_mutex_unlock(&g_table_mgr_lock);
        return NULL;
    }

    nos_kv_table_t *table = calloc(1, sizeof(nos_kv_table_t));
    if (!table) {
        pthread_mutex_unlock(&g_table_mgr_lock);
        return NULL;
    }

    strncpy(table->name, name, 31);
    table->key_size = key_size;
    table->max_val_size = max_val_size;
    table->capacity = capacity;
    table->entry_size = sizeof(nos_kv_entry_t) + key_size + max_val_size;
    
    table->bucket_count = 1024; 
    while (table->bucket_count < capacity) table->bucket_count <<= 1;

    table->buckets = calloc(table->bucket_count, sizeof(nos_kv_bucket_t));
    for (uint32_t i = 0; i < table->bucket_count; i++) {
        pthread_rwlock_init(&table->buckets[i].lock, NULL);
        table->buckets[i].first_idx = KV_INVALID_IDX;
    }

    table->pool_mem = calloc(capacity, table->entry_size);
    table->free_stack = malloc(sizeof(uint32_t) * capacity);
    table->free_top = -1;
    for (uint32_t i = 0; i < capacity; i++) {
        table->free_stack[++table->free_top] = i;
    }

    nos_sys_log_info("KV Table '%s' created: KeySize=%u, MaxVal=%u, Capacity=%u, Mem=%u KB", 
                     name, key_size, max_val_size, capacity, (capacity * table->entry_size) / 1024);

    g_tables[g_table_count++] = table;
    pthread_mutex_unlock(&g_table_mgr_lock);
    return table;
}

static inline nos_kv_entry_t* get_entry(nos_kv_table_t *table, uint32_t idx) {
    if (idx == KV_INVALID_IDX) return NULL;
    return (nos_kv_entry_t *)((uint8_t *)table->pool_mem + (idx * table->entry_size));
}

nos_status_t nos_kv_put(nos_kv_table_t *table, const void *key, const void *val, uint32_t val_len) {
    if (!table || !key || (val_len > table->max_val_size)) return NOS_ERR;

    uint32_t hash = murmurhash3_32(key, table->key_size, 0);
    nos_kv_bucket_t *bucket = &table->buckets[hash % table->bucket_count];

    pthread_rwlock_wrlock(&bucket->lock);

    uint32_t curr_idx = bucket->first_idx;
    while (curr_idx != KV_INVALID_IDX) {
        nos_kv_entry_t *entry = get_entry(table, curr_idx);
        if (memcmp(entry->data, key, table->key_size) == 0) {
            if (val) memcpy(entry->data + table->key_size, val, val_len);
            entry->val_len = val_len;
            pthread_rwlock_unlock(&bucket->lock);
            return NOS_OK;
        }
        curr_idx = entry->next_idx;
    }

    if (table->free_top < 0) {
        pthread_rwlock_unlock(&bucket->lock);
        return NOS_ERR_BUSY;
    }

    uint32_t new_idx = table->free_stack[table->free_top--];
    nos_kv_entry_t *new_entry = get_entry(table, new_idx);
    
    memcpy(new_entry->data, key, table->key_size);
    if (val) memcpy(new_entry->data + table->key_size, val, val_len);
    new_entry->val_len = val_len;
    
    new_entry->next_idx = bucket->first_idx;
    bucket->first_idx = new_idx;
    table->used_count++;

    pthread_rwlock_unlock(&bucket->lock);
    return NOS_OK;
}

nos_status_t nos_kv_get(nos_kv_table_t *table, const void *key, void *val_buf, uint32_t *val_len) {
    if (!table || !key || !val_buf || !val_len) return NOS_ERR;

    uint32_t hash = murmurhash3_32(key, table->key_size, 0);
    nos_kv_bucket_t *bucket = &table->buckets[hash % table->bucket_count];

    pthread_rwlock_rdlock(&bucket->lock);

    uint32_t curr_idx = bucket->first_idx;
    while (curr_idx != KV_INVALID_IDX) {
        nos_kv_entry_t *entry = get_entry(table, curr_idx);
        if (memcmp(entry->data, key, table->key_size) == 0) {
            uint32_t copy_len = (*val_len < entry->val_len) ? *val_len : entry->val_len;
            memcpy(val_buf, entry->data + table->key_size, copy_len);
            *val_len = entry->val_len;
            pthread_rwlock_unlock(&bucket->lock);
            return NOS_OK;
        }
        curr_idx = entry->next_idx;
    }

    pthread_rwlock_unlock(&bucket->lock);
    return NOS_ERR;
}

nos_status_t nos_kv_get_ptr(nos_kv_table_t *table, const void *key, const void **out_ptr, uint32_t *out_len, void **out_handle) {
    if (!table || !key || !out_ptr || !out_len || !out_handle) return NOS_ERR;

    uint32_t hash = murmurhash3_32(key, table->key_size, 0);
    nos_kv_bucket_t *bucket = &table->buckets[hash % table->bucket_count];

    pthread_rwlock_rdlock(&bucket->lock);

    uint32_t curr_idx = bucket->first_idx;
    while (curr_idx != KV_INVALID_IDX) {
        nos_kv_entry_t *entry = get_entry(table, curr_idx);
        if (memcmp(entry->data, key, table->key_size) == 0) {
            *out_ptr = entry->data + table->key_size;
            *out_len = entry->val_len;
            
            nos_kv_lock_handle_t *h = malloc(sizeof(nos_kv_lock_handle_t));
            h->lock = &bucket->lock;
            *out_handle = h;
            
            return NOS_OK;
        }
        curr_idx = entry->next_idx;
    }

    pthread_rwlock_unlock(&bucket->lock);
    return NOS_ERR;
}

void nos_kv_release_ptr(void *handle) {
    if (!handle) return;
    nos_kv_lock_handle_t *h = (nos_kv_lock_handle_t *)handle;
    pthread_rwlock_unlock(h->lock);
    free(h);
}

nos_status_t nos_kv_del(nos_kv_table_t *table, const void *key) {
    if (!table || !key) return NOS_ERR;

    uint32_t hash = murmurhash3_32(key, table->key_size, 0);
    nos_kv_bucket_t *bucket = &table->buckets[hash % table->bucket_count];

    pthread_rwlock_wrlock(&bucket->lock);

    uint32_t prev_idx = KV_INVALID_IDX;
    uint32_t curr_idx = bucket->first_idx;

    while (curr_idx != KV_INVALID_IDX) {
        nos_kv_entry_t *entry = get_entry(table, curr_idx);
        if (memcmp(entry->data, key, table->key_size) == 0) {
            if (prev_idx == KV_INVALID_IDX) bucket->first_idx = entry->next_idx;
            else get_entry(table, prev_idx)->next_idx = entry->next_idx;

            table->free_stack[++table->free_top] = curr_idx;
            table->used_count--;

            pthread_rwlock_unlock(&bucket->lock);
            return NOS_OK;
        }
        prev_idx = curr_idx;
        curr_idx = entry->next_idx;
    }

    pthread_rwlock_unlock(&bucket->lock);
    return NOS_ERR;
}

void nos_kv_table_dump(nos_kv_table_t *table) {
    if (!table) return;
    nos_sys_log_info("KV Table '%s' Stats: Capacity=%u, Used=%u (%.1f%%)", 
                     table->name, table->capacity, table->used_count, 
                     (float)table->used_count * 100 / table->capacity);
}

static nos_kv_ops_t g_kv_ops = {
    .table_create = nos_kv_table_create,
    .put = nos_kv_put,
    .get = nos_kv_get,
    .get_ptr = nos_kv_get_ptr,
    .release_ptr = nos_kv_release_ptr,
    .del = nos_kv_del
};

void nos_kv_db_init(void) {
    extern nos_status_t nos_embedded_service_register(const char *name, void *ops);
    nos_embedded_service_register("SVC_KV_DB", &g_kv_ops);
}