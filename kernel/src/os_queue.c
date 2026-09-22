/**
 * @file os_queue.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 实现固定容量消息队列、阻塞式收发以及等待任务间的直接消息交付。
 *
 * @details
 * 队列控制块来自静态对象池，消息存储区由调用者提供。队列以 head、tail 和 count
 * 管理固定大小元素的环形缓冲区，因此运行期间不申请动态内存。本文件同时提供
 * 队列占用、容量和只读快照查询，供测试及控制台面板观察。
 *
 * 发送到满队列或接收空队列时，任务可按 timeout 配置进入对应等待队列。发送者
 * 可以把消息直接交给已经阻塞的接收者；接收释放槽位后，也会立即把最高优先级
 * 等待发送者的消息补入队列。阻塞期间消息缓冲区地址保存在任务 TCB 中，因此调用者
 * 必须保证该缓冲区在 API 返回前始终有效。
 */

#include "os_queue_internal.h"
#include "os_list.h"
#include "os_sched.h"

#include <stdint.h>
#include <string.h>

/* 队列控制块固定分配；消息存储区由创建者提供。 */
static os_queue_t g_queues[OS_MAX_QUEUES];

/* 检查环形缓冲区指针、容量、计数和读写索引不变量。 */
static bool os_QueueIsValid(const os_queue_t *queue)
{
    return (queue != NULL) && queue->used && (queue->buffer != NULL) &&
           (queue->item_size > 0U) && (queue->capacity > 0U) &&
           (queue->count <= queue->capacity) &&
           (queue->head < queue->capacity) && (queue->tail < queue->capacity);
}

/* 计算环形缓冲区中指定消息槽位的首地址。 */
static uint8_t *os_QueueSlot(const os_queue_t *queue, size_t index)
{
    return &queue->buffer[index * queue->item_size];
}

/* 清空队列控制块池；不会释放调用方拥有的消息缓冲区。 */
void os_QueueKernelReset(void)
{
    memset(g_queues, 0, sizeof(g_queues));

    for (size_t index = 0U; index < OS_MAX_QUEUES; index++) {
        os_ListInit(&g_queues[index].send_waiters);
        os_ListInit(&g_queues[index].receive_waiters);
    }
}

/**
 * @brief 使用调用方缓冲区初始化固定容量环形消息队列。
 * @param out_queue 返回创建成功的对象；失败时保持为 NULL。
 * @param buffer 至少包含 item_size * capacity 字节的连续存储区。
 * @param item_size 单条消息大小。
 * @param capacity 最大消息数量。
 * @return 初始化结果。
 */
QueueHandle_t xQueueCreate(
    uint32_t uxQueueLength,
    uint32_t uxItemSize,
    void *pucQueueStorage
)
{
    if (!g_os_kernel.initialized || g_os_kernel.started) {
        os_SetLastError(OS_STATUS_BAD_STATE);
        return NULL;
    }
    /* 除基本参数外，还要拒绝 item_size * length 的 size_t 溢出。 */
    if ((pucQueueStorage == NULL) || (uxItemSize == 0U) || (uxQueueLength == 0U) ||
        (uxItemSize > (SIZE_MAX / uxQueueLength))) {
        os_SetLastError(OS_STATUS_INVALID_ARGUMENT);
        return NULL;
    }

    for (size_t index = 0U; index < OS_MAX_QUEUES; index++) {
        os_queue_t *queue = &g_queues[index];

        if (!queue->used) {
            queue->used = true;
            queue->buffer = (uint8_t *)pucQueueStorage;
            queue->item_size = uxItemSize;
            queue->capacity = uxQueueLength;
            queue->head = 0U;
            queue->tail = 0U;
            queue->count = 0U;
            os_ListInit(&queue->send_waiters);
            os_ListInit(&queue->receive_waiters);
            os_SetLastError(OS_STATUS_OK);
            return queue;
        }
    }

    os_SetLastError(OS_STATUS_LIMIT_REACHED);
    return NULL;
}

/* 返回队列当前占用数量，无效对象返回 0。 */
size_t uxQueueMessagesWaiting(QueueHandle_t xQueue)
{
    return os_QueueIsValid(xQueue) ? xQueue->count : 0U;
}

/* 返回队列固定容量，无效对象返回 0。 */
size_t uxQueueCapacity(QueueHandle_t xQueue)
{
    return os_QueueIsValid(xQueue) ? xQueue->capacity : 0U;
}

/**
 * @brief 按逻辑出队顺序复制队列内容，不改变 head、tail 和 count。
 * @param queue 目标队列。
 * @param out_items 接收消息副本的连续缓冲区。
 * @param maximum_items 最多复制的消息数量。
 * @return 实际复制数量。
 */
size_t os_QueueSnapshot(
    const os_queue_t *queue,
    void *out_items,
    size_t maximum_items
)
{
    size_t copy_count;

    if (!os_QueueIsValid(queue) || (out_items == NULL) ||
        (maximum_items == 0U)) {
        return 0U;
    }

    copy_count = (queue->count < maximum_items) ? queue->count : maximum_items;
    for (size_t index = 0U; index < copy_count; index++) {
        const size_t source_index = (queue->head + index) % queue->capacity;
        memcpy(
            &((uint8_t *)out_items)[index * queue->item_size],
            os_QueueSlot(queue, source_index),
            queue->item_size
        );
    }

    return copy_count;
}

/**
 * @brief 在串行内核上下文中发送一条消息。
 * @param queue 目标队列。
 * @param item 待发送消息；阻塞期间指针保持在发送任务的 TCB 中。
 * @param timeout_ticks 立即、有限或永久等待配置。
 */
void os_QueueSendCurrent(
    os_queue_t *queue,
    const void *item,
    uint32_t timeout_ticks
)
{
    os_task_t *current = g_os_kernel.current;
    os_task_t *receiver;

    if ((current == NULL) || (current->state != OS_TASK_RUNNING)) {
        return;
    }

    if (!os_QueueIsValid(queue) || (item == NULL)) {
        current->wait_result = OS_STATUS_INVALID_ARGUMENT;
        return;
    }

    /* 已有接收者时直接复制到其缓冲区，消息无需先进入环形队列。 */
    receiver = os_WaitPop(&queue->receive_waiters);
    if (receiver != NULL) {
        memcpy(receiver->wait_buffer, item, queue->item_size);
        current->wait_result = OS_STATUS_OK;
        os_WaitMakeReady(receiver, OS_STATUS_OK);
        os_WaitMaybePreempt(receiver);
        return;
    }

    /* 无等待接收者且仍有空位时，按 tail 写入并环形推进。 */
    if (queue->count < queue->capacity) {
        memcpy(os_QueueSlot(queue, queue->tail), item, queue->item_size);
        queue->tail = (queue->tail + 1U) % queue->capacity;
        queue->count++;
        current->wait_result = OS_STATUS_OK;
        return;
    }

    if (timeout_ticks == 0U) {
        current->wait_result = OS_STATUS_TIMEOUT;
        return;
    }

    /* 队列满时不提前复制，发送任务恢复前必须保留 item 指向的数据。 */
    (void)os_WaitBlockCurrent(
        &queue->send_waiters,
        OS_WAIT_QUEUE_SEND,
        queue,
        (void *)item,
        timeout_ticks
    );
}

/**
 * @brief 在串行内核上下文中接收一条消息。
 * @param queue 目标队列。
 * @param out_item 接收消息的任务缓冲区。
 * @param timeout_ticks 立即、有限或永久等待配置。
 */
void os_QueueReceiveCurrent(
    os_queue_t *queue,
    void *out_item,
    uint32_t timeout_ticks
)
{
    os_task_t *current = g_os_kernel.current;
    os_task_t *sender;

    if ((current == NULL) || (current->state != OS_TASK_RUNNING)) {
        return;
    }

    if (!os_QueueIsValid(queue) || (out_item == NULL)) {
        current->wait_result = OS_STATUS_INVALID_ARGUMENT;
        return;
    }

    /* 非空队列先取出 head 消息，随后再处理可能阻塞的发送者。 */
    if (queue->count > 0U) {
        memcpy(out_item, os_QueueSlot(queue, queue->head), queue->item_size);
        queue->head = (queue->head + 1U) % queue->capacity;
        queue->count--;

        /* 刚释放的槽位立即接收最高优先级阻塞发送者的消息。 */
        sender = os_WaitPop(&queue->send_waiters);
        if (sender != NULL) {
            memcpy(
                os_QueueSlot(queue, queue->tail),
                sender->wait_buffer,
                queue->item_size
            );
            queue->tail = (queue->tail + 1U) % queue->capacity;
            queue->count++;
            current->wait_result = OS_STATUS_OK;
            os_WaitMakeReady(sender, OS_STATUS_OK);
            os_WaitMaybePreempt(sender);
            return;
        }

        current->wait_result = OS_STATUS_OK;
        return;
    }

    if (timeout_ticks == 0U) {
        current->wait_result = OS_STATUS_TIMEOUT;
        return;
    }

    /* 队列空时保存接收缓冲区地址，供后续发送者直接交付消息。 */
    (void)os_WaitBlockCurrent(
        &queue->receive_waiters,
        OS_WAIT_QUEUE_RECEIVE,
        queue,
        out_item,
        timeout_ticks
    );
}
