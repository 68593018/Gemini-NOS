#ifndef __NOS_SHM_H__
#define __NOS_SHM_H__

#include <stdint.h>
#include <stdatomic.h>
#include "nos_types.h"
#include "nos_buffer.h"

#define NOS_SHM_NAME "/nos_global_shm"
#define NOS_SHM_SIZE (16 * 1024 * 1024) // 暂定 16MB
#define NOS_SHM_QUEUE_SIZE 4096         // 每个节点的队列深度

/**
 * @brief SHM 里的 MPSC 无锁队列
 */
typedef struct {
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    uint32_t offsets[NOS_SHM_QUEUE_SIZE];
} nos_shm_mpsc_queue_t;

/**
 * @brief 共享内存 Buffer 管理头
 * @note 紧挨着这个头的是实际的数据区 (nos_buffer_t)
 */
typedef struct {
    uint32_t offset;          // 自身相对于 SHM 基址的偏移
    uint32_t size;            // 块大小
    _Atomic uint32_t ref_count; // 跨进程原子引用计数
    _Atomic uint32_t next_free; // 空闲链表中的下一个偏移
} nos_shm_block_t;

/**
 * @brief 全局 SHM 布局头
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    
    /* 简易服务名注册表 (Node Name -> Queue Index) */
    struct {
        char node_name[32];
        int in_use;
    } nodes[16];

    nos_shm_mpsc_queue_t rx_queues[16]; // 每个节点专属的接收队列

    /* 简易全局内存池 (基于空闲链表) */
    _Atomic uint32_t free_list_head;    
    _Atomic uint32_t total_allocated;
} nos_shm_header_t;

/**
 * @brief 初始化或映射共享内存
 */
nos_status_t nos_shm_init(const char *node_name);

/**
 * @brief 获取当前进程在 SHM 中的专属 RX 队列
 */
nos_shm_mpsc_queue_t* nos_shm_get_local_queue(void);

/**
 * @brief 根据节点名称获取其 RX 队列
 */
nos_shm_mpsc_queue_t* nos_shm_get_remote_queue(const char *node_name);

/**
 * @brief SHM 专用 Buffer 申请
 */
nos_buffer_t* nos_shm_buffer_alloc(uint32_t size);

/**
 * @brief SHM 专用 Buffer 释放
 */
void nos_shm_buffer_release(nos_buffer_t *buf);

/**
 * @brief 将指针转换为偏移量
 */
uint32_t nos_shm_ptr_to_offset(void *ptr);

/**
 * @brief 将偏移量转换为指针
 */
void* nos_shm_offset_to_ptr(uint32_t offset);

/**
 * @brief 向 MPSC 队列无锁压入偏移量
 */
nos_status_t nos_shm_mpsc_enqueue(nos_shm_mpsc_queue_t *q, uint32_t offset);

/**
 * @brief 从 MPSC 队列出队偏移量 (单消费者)
 */
uint32_t nos_shm_mpsc_dequeue(nos_shm_mpsc_queue_t *q);

#endif
