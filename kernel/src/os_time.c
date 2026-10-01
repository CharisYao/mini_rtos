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

/* 原子读取 tick，使正在计算的任务无需进入内核请求即可查询时间。 */
uint32_t os_TickGet(void)
{
    return atomic_load_explicit(&g_os_kernel.tick, memory_order_relaxed);
}

/**
 * @brief 将任务按绝对唤醒时间升序插入对应延时链表（当前周期或溢出周期）。
 * @param task 待延时/超时任务。
 * @param target 目标唤醒 tick。
 */
void os_TimeDelayListInsert(os_task_t *task, uint32_t target)
{
    uint32_t now;
    os_list_t *list;
    os_list_node_t *node;

    if (task == NULL) {
        return;
    }

    if (task->delay_node.linked) {
        os_TimeDelayListRemove(task);
    }

    now = os_TickGet();
    task->wake_tick = target;

    /* 若目标唤醒时间小于当前时间，说明跨越了 uint32_t 溢出点，放入溢出链表 */
    if (target < now) {
        list = g_os_kernel.px_overflow_delayed;
    } else {
        list = g_os_kernel.px_delayed;
    }

    task->delay_list = list;

    /* 在目标链表中按 target 升序插入；相同 target 追加在其后，保持 FIFO */
    node = list->head;
    while (node != NULL) {
        if (target < node->owner->wake_tick) {
            os_ListInsertBefore(list, node, &task->delay_node);
            return;
        }
        node = node->next;
    }

    os_ListPushBack(list, &task->delay_node);
}

/**
 * @brief 将任务从其当前所在的延时链表中安全移除。
 * @param task 目标任务。
 */
void os_TimeDelayListRemove(os_task_t *task)
{
    if ((task != NULL) && task->delay_node.linked && (task->delay_list != NULL)) {
        os_ListRemove(task->delay_list, &task->delay_node);
        task->delay_list = NULL;
    }
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

    current->state = OS_TASK_BLOCKED_DELAY;
    os_TimeDelayListInsert(current, os_TickGet() + ticks);
    return os_SchedCurrentBlocked(OS_SWITCH_BLOCKED);
}

/**
 * @brief 推进一个系统 tick，并集中处理所有时间相关状态迁移。
 * @return tick 处理和可能的抢占完成后应运行的任务。
 *
 * 仅检查有序延时链表表头任务，无人到期时 O(1) 立即返回；并在回绕时乒乓对调双链表。
 */
os_task_t *os_TimeTick(void)
{
    uint32_t now =
        atomic_fetch_add_explicit(&g_os_kernel.tick, 1U, memory_order_relaxed) + 1U;
    bool woke_higher_priority = false;
    bool slice_expired = false;

    /* 1. 检测 Tick 溢出（从 UINT32_MAX 回绕到 0），对调双延时链表指针 */
    if (now == 0U) {
        os_list_t *temp = g_os_kernel.px_delayed;
        g_os_kernel.px_delayed = g_os_kernel.px_overflow_delayed;
        g_os_kernel.px_overflow_delayed = temp;
    }

    /* 2. 仅检查当前周期的延时链表表头！ */
    while (g_os_kernel.px_delayed->head != NULL) {
        os_task_t *task = g_os_kernel.px_delayed->head->owner;

        /* 表头任务尚未到期，后续任务必定未到期，O(1) 立即跳出 */
        if (!os_TickReached(now, task->wake_tick)) {
            break;
        }

        /* 弹出到期任务并清理 delay_list */
        os_ListPopFront(g_os_kernel.px_delayed);
        task->delay_list = NULL;

        if (task->state == OS_TASK_BLOCKED_DELAY) {
            task->state = OS_TASK_READY;
            os_ReadyEnqueue(task);
        } else if (task->state == OS_TASK_BLOCKED_OBJECT) {
            const bool waited_for_mutex = (task->wait_kind == OS_WAIT_MUTEX);

            if ((task->wait_list != NULL) && task->schedule_node.linked) {
                os_ListRemove(task->wait_list, &task->schedule_node);
            }
            os_WaitMakeReady(task, OS_STATUS_TIMEOUT);

            /* 等待者离开后，互斥量所有者可能不再需要继承其高优先级 */
            if (waited_for_mutex) {
                os_MutexWaitersChanged();
            }
        }

        if ((g_os_kernel.current == NULL) ||
            (task->effective_priority > g_os_kernel.current->effective_priority)) {
            woke_higher_priority = true;
        }
    }

    /* 3. 只有 RUNNING 任务消耗时间片；空闲期间不维护虚拟 Idle 时间片 */
    if (g_os_kernel.current != NULL) {
        if (g_os_kernel.current->slice_remaining > 0U) {
            g_os_kernel.current->slice_remaining--;
        }
        slice_expired = (g_os_kernel.current->slice_remaining == 0U);
    }

    /* 4. 无当前任务或唤醒更高优先级任务时，优先于时间片轮转处理 */
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
