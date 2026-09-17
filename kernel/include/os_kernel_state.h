/**
 * @file os_kernel_state.h
 * @author Peng RongYao
 * @date 2026-08-22
 * @brief 定义由各内核模块共同组成的全局内核状态。
 *
 * @details
 * 本文件只负责组合 TCB 池、ready/delayed 链表和调度轨迹。具体控制块仍归任务、
 * 信号量、队列和互斥量模块所有，不在这里重复定义。
 */

#ifndef MINI_RTOS_OS_KERNEL_STATE_H
#define MINI_RTOS_OS_KERNEL_STATE_H

#include "os.h"
#include "os_list.h"
#include "os_task_internal.h"

/* miniRTOS 唯一的全局内核状态。 */
typedef struct {
    bool initialized;                          /* os_Init() 是否已经完成。 */
    bool started;                              /* 是否已经启动过一次调度器。 */
    os_task_t tasks[OS_MAX_TASKS];             /* 静态 TCB 池。 */
    size_t task_count;                         /* 已创建任务数量。 */
    os_list_t ready[OS_PRIORITY_COUNT];        /* 每个优先级一条 FIFO 就绪队列。 */
    os_list_t delayed;                         /* 等待 wake_tick 的任务集合。 */
    uint8_t ready_bitmap;                      /* 非空就绪队列的优先级位图。 */
    /* ARMCC 5 无 stdatomic.h。Cortex-M 对齐 32 位读写本身原子，任务可读 tick。 */
    volatile uint32_t tick;
    os_task_t *current;                        /* 当前 RUNNING 任务。 */
    os_task_t *last_from;                      /* 最近一次切换的来源任务。 */
    os_task_t *last_to;                        /* 最近一次切换的目标任务。 */
    os_switch_reason_t last_reason;            /* 最近一次切换原因。 */
    uint32_t switch_count;                     /* 实际任务切换累计次数。 */
    uint32_t critical_nesting;                 /* 内核临界区嵌套深度。 */
    bool sched_pending;                        /* 临界区内推迟的调度请求。 */
} os_kernel_t;

/* 全局内核实例，由 kernel/src/os_task.c 定义。 */
extern os_kernel_t g_os_kernel;

#endif
