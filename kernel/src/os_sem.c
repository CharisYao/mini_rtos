/**
 * @file os_sem.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 实现基于静态对象池的计数信号量及其任务阻塞和唤醒语义。
 *
 * @details
 * 信号量控制块来自固定对象池，初始化时指定初始计数和最大计数，运行期间不使用
 * malloc。获取信号量时，有可用计数则立即消费；计数为零时根据 timeout 参数选择
 * 立即失败、有限等待或永久阻塞。
 *
 * 释放信号量时，若存在等待任务，本次资源直接交给最高优先级且最早等待的任务，
 * 不先增加计数；没有等待者时才增加计数并限制在最大值内。若被唤醒任务优先级
 * 更高，通用调度器会立即作出抢占决定。
 */

#include "os_sem_internal.h"
#include "os_list.h"
#include "os_sched.h"

#include <stddef.h>
#include <string.h>

/* 信号量控制块来自固定对象池，运行期间不申请动态内存。 */
static os_sem_t g_semaphores[OS_MAX_SEMAPHORES];

/* 检查对象已分配且计数没有破坏最大值不变量。 */
static bool os_SemIsValid(const os_sem_t *sem)
{
    return (sem != NULL) && sem->used && (sem->maximum_count > 0U) &&
           (sem->count <= sem->maximum_count);
}

/* 清空对象池；只由 os_Init() 在调度启动前调用。 */
void os_SemKernelReset(void)
{
    memset(g_semaphores, 0, sizeof(g_semaphores));

    for (size_t index = 0U; index < OS_MAX_SEMAPHORES; index++) {
        os_ListInit(&g_semaphores[index].waiters);
    }
}

/**
 * @brief 从静态对象池初始化一个计数信号量。
 * @param out_sem 返回创建成功的对象；失败时保持为 NULL。
 * @param initial_count 初始可用计数。
 * @param maximum_count 允许的最大计数，必须大于 0。
 * @return 初始化结果。
 */
os_status_t os_SemInit(
    os_sem_t **out_sem,
    uint32_t initial_count,
    uint32_t maximum_count
)
{
    if (out_sem == NULL) {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    *out_sem = NULL;

    if (!g_os_kernel.initialized || g_os_kernel.started) {
        return OS_STATUS_BAD_STATE;
    }
    if ((maximum_count == 0U) || (initial_count > maximum_count)) {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    for (size_t index = 0U; index < OS_MAX_SEMAPHORES; index++) {
        os_sem_t *sem = &g_semaphores[index];

        if (!sem->used) {
            sem->used = true;
            sem->count = initial_count;
            sem->maximum_count = maximum_count;
            os_ListInit(&sem->waiters);
            *out_sem = sem;
            return OS_STATUS_OK;
        }
    }

    return OS_STATUS_LIMIT_REACHED;
}

/* 返回当前可用计数，无效对象统一返回 0。 */
uint32_t os_SemGetCount(const os_sem_t *sem)
{
    return os_SemIsValid(sem) ? sem->count : 0U;
}

/**
 * @brief 在串行内核上下文中为当前任务获取一次信号量。
 * @param sem 目标信号量。
 * @param timeout_ticks 立即、有限或永久等待配置。
 *
 * 可用时直接减计数；不可用且允许等待时，当前任务进入信号量等待队列。
 */
void os_SemTakeCurrent(os_sem_t *sem, uint32_t timeout_ticks)
{
    os_task_t *current = g_os_kernel.current;

    if ((current == NULL) || (current->state != OS_TASK_RUNNING)) {
        return;
    }

    if (!os_SemIsValid(sem)) {
        current->wait_result = OS_STATUS_INVALID_ARGUMENT;
        return;
    }

    if (sem->count > 0U) {
        sem->count--;
        current->wait_result = OS_STATUS_OK;
        return;
    }

    if (timeout_ticks == 0U) {
        current->wait_result = OS_STATUS_TIMEOUT;
        return;
    }

    (void)os_WaitBlockCurrent(
        &sem->waiters,
        OS_WAIT_SEMAPHORE,
        sem,
        NULL,
        timeout_ticks
    );
}

/**
 * @brief 在串行内核上下文中释放一次信号量。
 * @param sem 目标信号量。
 *
 * 已有等待者时，本次释放直接交给最高优先级等待任务，不先增加再减少计数。
 */
os_status_t os_SemGiveFromIsrContext(os_sem_t *sem, os_task_t **out_woken)
{
    os_task_t *waiter;

    if (out_woken != NULL) {
        *out_woken = NULL;
    }

    if (!os_SemIsValid(sem)) {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    waiter = os_WaitPop(&sem->waiters);
    if (waiter != NULL) {
        os_WaitMakeReady(waiter, OS_STATUS_OK);
        if (out_woken != NULL) {
            *out_woken = waiter;
        }
        return OS_STATUS_OK;
    }

    if (sem->count >= sem->maximum_count) {
        return OS_STATUS_LIMIT_REACHED;
    }

    sem->count++;
    return OS_STATUS_OK;
}

void os_SemGiveCurrent(os_sem_t *sem)
{
    os_task_t *current = g_os_kernel.current;
    os_task_t *woken = NULL;
    os_status_t status;

    if ((current == NULL) || (current->state != OS_TASK_RUNNING)) {
        return;
    }

    status = os_SemGiveFromIsrContext(sem, &woken);
    current->wait_result = status;
    if (woken != NULL) {
        os_WaitMaybePreempt(woken);
    }
}
