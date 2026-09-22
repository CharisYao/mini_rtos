/**
 * @file os_time.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 实现系统 tick、任务延时、等待超时、时间片轮转和到期唤醒。
 *
 * @details
 * os_TimeDelayCurrent() 将当前任务移出运行态，记录绝对唤醒 tick 并切换到下一任务；
 * os_TimeTick() 每推进一个 tick，依次处理延时任务唤醒、内核对象等待超时、
 * 高优先级任务抢占和同优先级时间片耗尽。
 *
 * 时间比较使用有符号差值处理 uint32_t tick 回绕，有限等待被限制在可安全比较的
 * 范围内。对象等待超时后还会触发互斥量优先级重新计算。本文件只处理平台无关的
 * 时间语义，实际周期事件由 Win32 移植层的等待定时器产生。
 */

#include "os_time.h"
#include "os_list.h"
#include "os_sched.h"
#include "os_mutex_internal.h"

#include <stdint.h>

/* 使用有符号差值进行 tick 回绕安全的“已到期”判断。 */
static bool os_TickReached(uint32_t now, uint32_t target)
{
    return (int32_t)(now - target) >= 0;
}

/* 读取 tick。任务可在不进入内核临界区时查询时间。 */
TickType_t xTaskGetTickCount(void)
{
    return g_os_kernel.tick;
}

/**
 * @brief 将当前任务阻塞指定 tick，并立即选择下一任务。
 * @param ticks 延时长度；0 退化为主动让出，超大值会被限制。
 * @return 延时生效后应运行的任务。
 */
os_task_t *os_TimeDelayCurrent(uint32_t ticks)
{
    os_task_t *current = g_os_kernel.current;

    if ((current == NULL) || (current->state != OS_TASK_RUNNING)) {
        return current;
    }

    if (ticks == 0U) {
        return os_SchedYieldCurrent();
    }
    if (ticks > OS_MAX_DELAY_TICKS) {
        ticks = OS_MAX_DELAY_TICKS;
    }

    current->wake_tick = xTaskGetTickCount() + ticks;
    current->state = OS_TASK_BLOCKED_DELAY;
    os_ListPushBack(&g_os_kernel.delayed, &current->schedule_node);
    return os_SchedCurrentBlocked(OS_SWITCH_BLOCKED);
}

/**
 * @brief 推进一个系统 tick，并集中处理所有时间相关状态迁移。
 * @return tick 处理和可能的抢占完成后应运行的任务。
 *
 * 处理顺序为延时唤醒、对象等待超时、当前时间片扣减，最后只执行一次调度判断。
 */
os_task_t *os_TimeTick(void)
{
    g_os_kernel.tick++;
    uint32_t now = g_os_kernel.tick;
    os_list_node_t *node = g_os_kernel.delayed.head;
    bool woke_higher_priority = false;
    bool slice_expired = false;

    /* 延时队列未排序，因此每个 tick 检查全部 BLOCKED_DELAY 任务。 */
    while (node != NULL) {
        os_list_node_t *next = node->next;
        os_task_t *task = node->owner;

        if (os_TickReached(now, task->wake_tick)) {
            os_ListRemove(&g_os_kernel.delayed, node);
            task->state = OS_TASK_READY;
            os_ReadyEnqueue(task);

            if ((g_os_kernel.current == NULL) ||
                (task->effective_priority > g_os_kernel.current->effective_priority)) {
                woke_higher_priority = true;
            }
        }

        node = next;
    }

    /* 对象超时扫描固定 TCB 池，避免为教学版本再增加一套超时节点。 */
    for (size_t index = 0U; index < g_os_kernel.task_count; index++) {
        os_task_t *task = &g_os_kernel.tasks[index];

        if ((task->state == OS_TASK_BLOCKED_OBJECT) &&
            (task->wait_deadline != OS_WAIT_FOREVER) &&
            os_TickReached(now, task->wait_deadline)) {
            const bool waited_for_mutex = task->wait_kind == OS_WAIT_MUTEX;

            os_ListRemove(task->wait_list, &task->schedule_node);
            os_WaitMakeReady(task, OS_STATUS_TIMEOUT);
            /* 等待者离开后，互斥量所有者可能不再需要继承其高优先级。 */
            if (waited_for_mutex) {
                os_MutexWaitersChanged();
            }

            if ((g_os_kernel.current == NULL) ||
                (task->effective_priority > g_os_kernel.current->effective_priority)) {
                woke_higher_priority = true;
            }
        }
    }

    /* 只有 RUNNING 任务消耗时间片；空闲期间不维护虚拟 Idle 时间片。 */
    if (g_os_kernel.current != NULL) {
        if (g_os_kernel.current->slice_remaining > 0U) {
            g_os_kernel.current->slice_remaining--;
        }
        slice_expired = (g_os_kernel.current->slice_remaining == 0U);
    }

    /* 无当前任务或唤醒更高优先级任务时，优先于时间片轮转处理。 */
    if ((g_os_kernel.current == NULL) && (os_ReadyHighestPriority() >= 0)) {
        return os_SchedSchedule(OS_SWITCH_HIGHER_PRIORITY_WAKEUP, false);
    }

    if (woke_higher_priority) {
        return os_SchedSchedule(OS_SWITCH_HIGHER_PRIORITY_WAKEUP, false);
    }

    if (slice_expired) {
        os_task_t *before = g_os_kernel.current;
        os_task_t *after = os_SchedSchedule(OS_SWITCH_TIME_SLICE, true);

        if ((after != NULL) && (after == before)) {
            after->slice_remaining = OS_TIME_SLICE_TICKS;
        }
        return after;
    }

    return g_os_kernel.current;
}
