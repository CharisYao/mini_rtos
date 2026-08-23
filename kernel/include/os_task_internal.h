/**
 * @file os_task_internal.h
 * @author Peng RongYao
 * @date 2026-08-22
 * @brief 定义任务模块拥有的 TCB 和对象等待状态。
 *
 * @details
 * 应用层只通过 os.h 中的 os_task_t 不透明句柄访问任务。完整 TCB 归任务模块
 * 管理；调度、时间和内核对象模块通过包含本文件读取或更新调度所需字段。
 */

#ifndef MINI_RTOS_OS_TASK_INTERNAL_H
#define MINI_RTOS_OS_TASK_INTERNAL_H

#include "os.h"
#include "os_list.h"

/* BLOCKED_OBJECT 任务正在等待的内核对象类别。 */
typedef enum {
    OS_WAIT_NONE = 0,         /* 未等待内核对象。 */
    OS_WAIT_SEMAPHORE,        /* 等待获取信号量。 */
    OS_WAIT_QUEUE_SEND,       /* 等待队列出现发送空间。 */
    OS_WAIT_QUEUE_RECEIVE,    /* 等待队列出现可接收消息。 */
    OS_WAIT_MUTEX             /* 等待互斥量所有权。 */
} os_wait_kind_t;

/* TCB任务控制块，保存调度所需的全部平台无关状态。 */
struct os_task {
    uint8_t id;                        /* 静态 TCB 数组下标。 */
    char name[OS_TASK_NAME_MAX];       /* 用于调试和面板显示的任务名。 */
    os_task_entry_t entry;             /* 任务入口函数。 */
    void *argument;                    /* 传给任务入口的用户参数。 */
    uint8_t base_priority;             /* 创建时确定的基础优先级。 */
    uint8_t effective_priority;        /* 考虑互斥量继承后的调度优先级。 */
    os_task_state_t state;             /* 当前生命周期状态。 */
    uint32_t wake_tick;                /* BLOCKED_DELAY 的绝对唤醒 tick。 */
    uint32_t slice_remaining;          /* 当前时间片剩余 tick。 */
    os_wait_kind_t wait_kind;          /* 正在等待的对象操作类型。 */
    void *wait_object;                 /* 正在等待的信号量、队列或互斥量。 */
    void *wait_buffer;                 /* 阻塞式队列操作使用的消息缓冲区。 */
    os_list_t *wait_list;              /* 当前所在的对象等待队列。 */
    uint32_t wait_deadline;            /* 对象等待的绝对超时 tick。 */
    os_status_t wait_result;           /* 任务恢复后由公开 API 返回的结果。 */
    os_list_node_t schedule_node;      /* 挂入一条调度链表的唯一节点。 */
};

#endif
