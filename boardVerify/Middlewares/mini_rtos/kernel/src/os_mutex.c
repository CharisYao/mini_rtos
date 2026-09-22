/**
 * @file os_mutex.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 实现非递归互斥量、所有权移交以及链式优先级继承。
 *
 * @details
 * 互斥量来自固定对象池并记录当前所有者。未加锁时由调用任务直接获得；已被占用时，
 * 请求任务根据 timeout 参数立即失败或进入按有效优先级排列的等待队列。互斥量不可
 * 递归获取，且只有当前所有者可以正常解锁。
 *
 * 当高优先级任务等待低优先级所有者时，本文件通过多轮传播重新计算所有任务的
 * 有效优先级，能够处理多个互斥量形成的链式继承。等待超时、正常解锁或任务终止
 * 都会重新计算优先级；释放时所有权直接移交给最高优先级且最早等待的任务。
 */

#include "os_mutex_internal.h"
#include "os_list.h"
#include "os_sched.h"

#include <stddef.h>
#include <string.h>

/* 互斥量控制块来自固定对象池。 */
static os_mutex_t g_mutexes[OS_MAX_MUTEXES];

/* 互斥量只需确认对象池槽位已经分配。 */
static bool os_MutexIsValid(const os_mutex_t *mutex)
{
    return (mutex != NULL) && mutex->used;
}

/* 清空互斥量对象池及各自等待队列。 */
void os_MutexKernelReset(void)
{
    memset(g_mutexes, 0, sizeof(g_mutexes));

    for (size_t index = 0U; index < OS_MAX_MUTEXES; index++) {
        os_ListInit(&g_mutexes[index].waiters);
    }
}

/**
 * @brief 从静态对象池初始化一个未加锁的非递归互斥量。
 * @param out_mutex 返回创建成功的对象；失败时保持为 NULL。
 * @return 初始化结果。
 */
MutexHandle_t xMutexCreate(void)
{
    if (!g_os_kernel.initialized || g_os_kernel.started) {
        os_SetLastError(OS_STATUS_BAD_STATE);
        return NULL;
    }

    for (size_t index = 0U; index < OS_MAX_MUTEXES; index++) {
        os_mutex_t *mutex = &g_mutexes[index];

        if (!mutex->used) {
            mutex->used = true;
            mutex->owner = NULL;
            os_ListInit(&mutex->waiters);
            os_SetLastError(OS_STATUS_OK);
            return mutex;
        }
    }

    os_SetLastError(OS_STATUS_LIMIT_REACHED);
    return NULL;
}

/**
 * @brief 根据全部互斥量所有权和等待关系重新计算任务有效优先级。
 *
 * 先从每个任务的基础优先级开始，再进行有限次传播，支持“高优先级任务等待
 * 中间任务、中间任务又等待低优先级所有者”的链式继承。最后统一移动受影响任务
 * 的就绪或等待队列位置。
 */
void os_MutexWaitersChanged(void)
{
    uint8_t desired[OS_MAX_TASKS] = {0U};

    /* 每次都从基础优先级重算，确保解锁或超时后能够正确降低优先级。 */
    for (size_t index = 0U; index < g_os_kernel.task_count; index++) {
        desired[index] = g_os_kernel.tasks[index].base_priority;
    }

    /* 每轮沿一层等待关系传播，OS_MAX_TASKS 轮足以覆盖有限任务链。 */
    for (size_t pass = 0U; pass < OS_MAX_TASKS; pass++) {
        bool changed = false;

        for (size_t index = 0U; index < OS_MAX_MUTEXES; index++) {
            const os_mutex_t *mutex = &g_mutexes[index];
            os_list_node_t *node;
            uint8_t inherited;

            if (!mutex->used || (mutex->owner == NULL)) {
                continue;
            }

            inherited = desired[mutex->owner->id];
            node = mutex->waiters.head;
            while (node != NULL) {
                const uint8_t waiter_priority = desired[node->owner->id];

                if (waiter_priority > inherited) {
                    inherited = waiter_priority;
                }
                node = node->next;
            }

            if (inherited > desired[mutex->owner->id]) {
                desired[mutex->owner->id] = inherited;
                changed = true;
            }
        }

        if (!changed) {
            break;
        }
    }

    /* 计算稳定后再统一应用，避免传播过程依赖临时队列顺序。 */
    for (size_t index = 0U; index < g_os_kernel.task_count; index++) {
        os_TaskSetEffectivePriority(&g_os_kernel.tasks[index], desired[index]);
    }
}

/**
 * @brief 在串行内核上下文中为当前任务获取互斥量。
 * @param mutex 目标互斥量。
 * @param timeout_ticks 立即、有限或永久等待配置。
 *
 * 当前所有者重复加锁会返回 BAD_STATE；发生竞争时阻塞请求者并提升所有者优先级。
 */
void os_MutexLockCurrent(os_mutex_t *mutex, uint32_t timeout_ticks)
{
    os_task_t *current = g_os_kernel.current;

    if ((current == NULL) || (current->state != OS_TASK_RUNNING)) {
        return;
    }

    if (!os_MutexIsValid(mutex)) {
        current->wait_result = OS_STATUS_INVALID_ARGUMENT;
        return;
    }

    if (mutex->owner == NULL) {
        mutex->owner = current;
        current->wait_result = OS_STATUS_OK;
        return;
    }

    if (mutex->owner == current) {
        current->wait_result = OS_STATUS_BAD_STATE;
        return;
    }

    if (timeout_ticks == 0U) {
        current->wait_result = OS_STATUS_TIMEOUT;
        return;
    }

    /* 请求者先进入等待队列，随后才能从完整关系中计算继承优先级。 */
    (void)os_WaitBlockCurrent(
        &mutex->waiters,
        OS_WAIT_MUTEX,
        mutex,
        NULL,
        timeout_ticks
    );
    os_MutexWaitersChanged();

    /* 所有者被提升后可能越过刚选出的中优先级任务，需要立即重新调度。 */
    if ((g_os_kernel.current != NULL) &&
        (mutex->owner->effective_priority >
         g_os_kernel.current->effective_priority)) {
        (void)os_SchedSchedule(OS_SWITCH_HIGHER_PRIORITY_WAKEUP, false);
    }
}

/**
 * @brief 由当前所有者释放互斥量并移交给最高优先级等待者。
 * @param mutex 目标互斥量。
 *
 * 所有权改变后重新计算继承关系，再决定新所有者是否立即抢占。
 */
void os_MutexUnlockCurrent(os_mutex_t *mutex)
{
    os_task_t *current = g_os_kernel.current;
    os_task_t *waiter;

    if ((current == NULL) || (current->state != OS_TASK_RUNNING)) {
        return;
    }

    if (!os_MutexIsValid(mutex)) {
        current->wait_result = OS_STATUS_INVALID_ARGUMENT;
        return;
    }

    if (mutex->owner != current) {
        current->wait_result = OS_STATUS_NOT_OWNER;
        return;
    }

    /* 等待者存在时直接移交所有权，避免其醒来后再次竞争同一把锁。 */
    waiter = os_WaitPop(&mutex->waiters);
    mutex->owner = waiter;
    current->wait_result = OS_STATUS_OK;
    os_MutexWaitersChanged();

    if (waiter != NULL) {
        os_WaitMakeReady(waiter, OS_STATUS_OK);
        os_WaitMaybePreempt(waiter);
    }
}

/**
 * @brief 任务终止时释放并移交它持有的全部互斥量。
 * @param task 即将终止的任务。
 *
 * 该清理防止任务入口意外返回后留下永久占用的互斥量。
 */
void os_MutexReleaseTask(os_task_t *task)
{
    bool released_any = false;

    if (task == NULL) {
        return;
    }

    for (size_t index = 0U; index < OS_MAX_MUTEXES; index++) {
        os_mutex_t *mutex = &g_mutexes[index];

        if (mutex->used && (mutex->owner == task)) {
            os_task_t *waiter = os_WaitPop(&mutex->waiters);

            mutex->owner = waiter;
            if (waiter != NULL) {
                os_WaitMakeReady(waiter, OS_STATUS_OK);
            }
            released_any = true;
        }
    }

    if (released_any) {
        os_MutexWaitersChanged();
    }
}
