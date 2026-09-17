/**
 * @file os_sem_internal.h
 * @author Peng RongYao
 * @date 2026-08-22
 * @brief 定义信号量模块拥有的控制块并声明其内核接口。
 */

#ifndef MINI_RTOS_OS_SEM_INTERNAL_H
#define MINI_RTOS_OS_SEM_INTERNAL_H

#include "os.h"
#include "os_list.h"

/* 静态计数信号量控制块。 */
struct os_sem {
    bool used;                 /* 对象池槽位是否已经分配。 */
    uint32_t count;            /* 当前可获取计数。 */
    uint32_t maximum_count;    /* 计数允许达到的上限。 */
    os_list_t waiters;         /* 按有效优先级排列的等待任务。 */
};

/* 复位静态信号量对象池。 */
void os_SemKernelReset(void);
/* 在串行内核上下文中执行当前任务的信号量获取。 */
void os_SemTakeCurrent(os_sem_t *sem, uint32_t timeout_ticks);
/* 在串行内核上下文中执行当前任务的信号量释放。 */
void os_SemGiveCurrent(os_sem_t *sem);
/**
 * @brief 在 ISR/tick 上下文释放信号量（不依赖 current，不阻塞）。
 * @param sem 目标信号量。
 * @param out_woken 可选；被直接交付的等待任务，无人等待时为 NULL。
 * @return 操作状态。
 */
os_status_t os_SemGiveFromIsrContext(os_sem_t *sem, os_task_t **out_woken);

#endif
