/**
 * @file os_queue_internal.h
 * @author Peng RongYao
 * @date 2026-08-22
 * @brief 定义消息队列模块拥有的控制块并声明其内核接口。
 */

#ifndef MINI_RTOS_OS_QUEUE_INTERNAL_H
#define MINI_RTOS_OS_QUEUE_INTERNAL_H

#include "os.h"
#include "os_list.h"

/* 使用调用方缓冲区的固定容量环形消息队列。 */
struct os_queue {
    bool used;                     /* 对象池槽位是否已经分配。 */
    uint8_t *buffer;               /* 调用方提供的连续消息存储区。 */
    size_t item_size;              /* 单条消息字节数。 */
    size_t capacity;               /* 最多容纳的消息数量。 */
    size_t head;                   /* 下一条出队消息的索引。 */
    size_t tail;                   /* 下一条入队消息的索引。 */
    size_t count;                  /* 当前已存消息数量。 */
    os_list_t send_waiters;        /* 队列满时阻塞的发送任务。 */
    os_list_t receive_waiters;     /* 队列空时阻塞的接收任务。 */
};

/* 复位静态消息队列对象池。 */
void os_QueueKernelReset(void);
/**
 * @brief 在串行内核上下文中执行当前任务的队列发送。
 * @param queue 目标队列。
 * @param item 待发送消息。
 * @param timeout_ticks 有限等待 tick 或 OS_WAIT_FOREVER。
 */
void os_QueueSendCurrent(
    os_queue_t *queue,
    const void *item,
    uint32_t timeout_ticks
);
/**
 * @brief 在串行内核上下文中执行当前任务的队列接收。
 * @param queue 目标队列。
 * @param out_item 接收消息的任务缓冲区。
 * @param timeout_ticks 有限等待 tick 或 OS_WAIT_FOREVER。
 */
void os_QueueReceiveCurrent(
    os_queue_t *queue,
    void *out_item,
    uint32_t timeout_ticks
);

#endif
