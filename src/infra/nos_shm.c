#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include "nos_shm.h"
#include "nos_log_api.h"

#define SHM_MAGIC 0xAABBCCDD

static void *g_shm_base = NULL;
static nos_shm_header_t *g_shm_hdr = NULL;
static int g_local_queue_idx = -1;

uint32_t nos_shm_ptr_to_offset(void *ptr) {
    if (!ptr || !g_shm_base) return 0;
    return (uint32_t)((char*)ptr - (char*)g_shm_base);
}

void* nos_shm_offset_to_ptr(uint32_t offset) {
    if (offset == 0 || !g_shm_base) return NULL;
    return (char*)g_shm_base + offset;
}

/* 初始化一个简单的全局内存池 (仅在创建共享内存时调用一次) */
static void init_shm_pool(void) {
    uint32_t offset = sizeof(nos_shm_header_t);
    // 块大小 = Header + Buffer对象 + 数据区(4096) + 预留指针存放回溯指针
    uint32_t block_size = sizeof(nos_shm_block_t) + sizeof(nos_buffer_t) + 4096 + sizeof(void*);
    uint32_t num_blocks = (NOS_SHM_SIZE - offset) / block_size;
    
    uint32_t first_free = offset;
    for (uint32_t i = 0; i < num_blocks; i++) {
        nos_shm_block_t *blk = (nos_shm_block_t*)nos_shm_offset_to_ptr(offset);
        blk->offset = offset;
        blk->size = 4096;
        atomic_init(&blk->ref_count, 0);
        
        uint32_t next_offset = (i == num_blocks - 1) ? 0 : offset + block_size;
        atomic_init(&blk->next_free, next_offset);
        
        offset += block_size;
    }
    atomic_init(&g_shm_hdr->free_list_head, first_free);
    atomic_init(&g_shm_hdr->total_allocated, 0);
}

nos_status_t _nos_shm_create_and_init(void) {
    int fd = shm_open(NOS_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return NOS_ERR;

    if (ftruncate(fd, NOS_SHM_SIZE) < 0) { close(fd); return NOS_ERR; }

    g_shm_base = mmap(NULL, NOS_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (g_shm_base == MAP_FAILED) return NOS_ERR;

    g_shm_hdr = (nos_shm_header_t *)g_shm_base;
    memset(g_shm_hdr, 0, sizeof(nos_shm_header_t));
    init_shm_pool();
    
    /* 最后写入 Magic，作为初始化完成的标志 */
    atomic_store_explicit((_Atomic uint32_t*)&g_shm_hdr->magic, SHM_MAGIC, memory_order_release);
    return NOS_OK;
}

nos_status_t nos_shm_init(const char *node_name) {
    int fd = shm_open(NOS_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) {
        nos_sys_log_error("SHM not created by Daemon yet.");
        return NOS_ERR;
    }

    g_shm_base = mmap(NULL, NOS_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (g_shm_base == MAP_FAILED) return NOS_ERR;

    g_shm_hdr = (nos_shm_header_t *)g_shm_base;

    /* 等待 Daemon 初始化完成 */
    int wait_count = 0;
    while (atomic_load_explicit((_Atomic uint32_t*)&g_shm_hdr->magic, memory_order_acquire) != SHM_MAGIC) {
        if (++wait_count > 50) return NOS_ERR; // 等待超时 500ms
        usleep(10000);
    }

    /* 注册本地节点 */
    for (int i = 0; i < 16; i++) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_shm_hdr->nodes[i].in_use, &expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            strncpy(g_shm_hdr->nodes[i].node_name, node_name, 31);
            g_local_queue_idx = i;
            atomic_init(&g_shm_hdr->rx_queues[i].head, 0);
            atomic_init(&g_shm_hdr->rx_queues[i].tail, 0);
            break;
        } else if (strcmp(g_shm_hdr->nodes[i].node_name, node_name) == 0) {
            /* 可能是异常重启，重用旧槽位 */
            g_local_queue_idx = i;
            break;
        }
    }

    if (g_local_queue_idx == -1) return NOS_ERR; // Node 满了
    return NOS_OK;
}

nos_shm_mpsc_queue_t* nos_shm_get_local_queue(void) {
    if (g_local_queue_idx >= 0) return &g_shm_hdr->rx_queues[g_local_queue_idx];
    return NULL;
}

nos_shm_mpsc_queue_t* nos_shm_get_remote_queue(const char *node_name) {
    if (!g_shm_hdr) return NULL;
    for (int i = 0; i < 16; i++) {
        if (g_shm_hdr->nodes[i].in_use && strcmp(g_shm_hdr->nodes[i].node_name, node_name) == 0) {
            return &g_shm_hdr->rx_queues[i];
        }
    }
    return NULL;
}

nos_buffer_t* nos_shm_buffer_alloc(uint32_t size) {
    if (size > 4096) return NULL; // 目前简单实现只支持最大4K
    
    uint32_t head;
    uint32_t next;
    nos_shm_block_t *blk = NULL;

    /* 无锁空闲链表 Pop */
    do {
        head = atomic_load(&g_shm_hdr->free_list_head);
        if (head == 0) return NULL; // OOM
        blk = (nos_shm_block_t*)nos_shm_offset_to_ptr(head);
        next = atomic_load(&blk->next_free);
    } while (!atomic_compare_exchange_weak(&g_shm_hdr->free_list_head, &head, next));

    atomic_fetch_add(&g_shm_hdr->total_allocated, 1);
    atomic_store(&blk->ref_count, 1);
    
    nos_buffer_t *buf = (nos_buffer_t*)(blk + 1);
    buf->raw_data = (uint8_t*)(buf + 1);
    buf->data = buf->raw_data;
    buf->capacity = 4096;
    buf->len = 0;
    buf->headroom = 0;
    buf->tailroom = 4096;
    atomic_init(&buf->ref_cnt, 0); // 内部 ref_cnt 也可以保留，但主要靠 SHM 的 ref_count
    buf->flags = 1; // 1 表示这是一个 SHM Buffer

    return buf;
}

void nos_shm_buffer_release(nos_buffer_t *buf) {
    if (!buf || buf->flags != 1) return;

    /* 绝对安全的回溯：因为 buf 就是从 blk 后面分配的 */
    nos_shm_block_t *blk = (nos_shm_block_t*)buf - 1;
    if (!blk) return;

    if (atomic_fetch_sub(&blk->ref_count, 1) == 1) {
        /* 引用计数归零，无锁 Push 回空闲链表 */
        uint32_t head;
        do {
            head = atomic_load(&g_shm_hdr->free_list_head);
            atomic_store(&blk->next_free, head);
        } while (!atomic_compare_exchange_weak(&g_shm_hdr->free_list_head, &head, blk->offset));
        
        atomic_fetch_sub(&g_shm_hdr->total_allocated, 1);
    }
}

void nos_shm_buffer_retain(nos_buffer_t *buf) {
    if (!buf || buf->flags != 1) return;

    nos_shm_block_t *blk = (nos_shm_block_t*)buf - 1;
    if (!blk) return;
    
    atomic_fetch_add(&blk->ref_count, 1);
}

nos_status_t nos_shm_mpsc_enqueue(nos_shm_mpsc_queue_t *q, uint32_t offset) {
    if (!q || offset == 0) return NOS_ERR;
    uint32_t tail, next_tail;
    do {
        tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
        next_tail = (tail + 1) % NOS_SHM_QUEUE_SIZE;
        if (next_tail == atomic_load_explicit(&q->head, memory_order_acquire)) {
            return NOS_ERR_BUSY; // Queue full
        }
    } while (!atomic_compare_exchange_weak_explicit(&q->tail, &tail, next_tail, 
                                                    memory_order_acquire, 
                                                    memory_order_relaxed));
    
    /* 抢到 tail 槽位后，写入数据。由于 tail 已经推进，消费者可能会立刻读到这个槽位。
       为了防止消费者读到旧数据，我们在初始化时保证所有 offset 为 0。
       写入时，我们用 release 语义写入真正的 offset。 */
    atomic_store_explicit((_Atomic uint32_t*)&q->offsets[tail], offset, memory_order_release);
    return NOS_OK;
}

uint32_t nos_shm_mpsc_dequeue(nos_shm_mpsc_queue_t *q) {
    if (!q) return 0;
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (head == atomic_load_explicit(&q->tail, memory_order_acquire)) {
        return 0; // Queue empty
    }
    
    /* 自旋等待生产者完成数据写入 (offset 不为 0) */
    uint32_t offset;
    while ((offset = atomic_load_explicit((_Atomic uint32_t*)&q->offsets[head], memory_order_acquire)) == 0) {
        /* 如果生产者抢到了槽位但还没来得及写数据，我们在这里稍微等一下 */
        // cpu_relax();
    }
    
    /* 读完数据后，必须将槽位清零，为下一轮循环做准备 */
    atomic_store_explicit((_Atomic uint32_t*)&q->offsets[head], 0, memory_order_relaxed);
    atomic_store_explicit(&q->head, (head + 1) % NOS_SHM_QUEUE_SIZE, memory_order_release);
    return offset;
}