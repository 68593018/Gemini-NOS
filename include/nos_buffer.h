#ifndef __NOS_BUFFER_H__
#define __NOS_BUFFER_H__

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "nos_types.h"

#define NOS_BUFFER_SIZE 2048   /* 每个 Buffer 块固定 2KB */
#define NOS_BUFFER_COUNT 256   /* 预分配 256 个块 */

/**
 * @brief NOS 统一内存 Buffer 结构
 */
typedef struct {
    uint8_t *data;             /**< 实际数据区域 */
    uint32_t capacity;         /**< 总容量 */
    uint32_t len;              /**< 当前有效数据长度 */
    _Atomic int ref_cnt;       /**< 引用计数 (原子操作) */
    uint32_t pool_idx;         /**< 在池中的索引，内部使用 */
} nos_buffer_t;

/**
 * @brief 初始化全局 Buffer 池
 */
nos_status_t nos_buffer_init_pool(void);

/**
 * @brief 从池中申请一个 Buffer
 * @return nos_buffer_t* 成功返回指针（引用计数初始化为 1），失败返回 NULL
 */
nos_buffer_t* nos_buffer_alloc(void);

/**
 * @brief 增加 Buffer 的引用计数
 */
void nos_buffer_retain(nos_buffer_t *buf);

/**
 * @brief 释放/减少 Buffer 的引用计数
 * @note 当计数减至 0 时，Buffer 自动归还至池中
 */
void nos_buffer_release(nos_buffer_t *buf);

#endif /* __NOS_BUFFER_H__ */
