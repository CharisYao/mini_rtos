/**
 * @file os_sched.h
 * @author Peng RongYao
 * @date 2026-08-22
 * @brief 声明就绪队列、调度、等待队列和内核状态检查接口。
 */

#ifndef MINI_RTOS_OS_SCHED_H
#define MINI_RTOS_OS_SCHED_H

#include "os_kernel_state.h"

/* 进入/退出内核临界区；最外层退出时若 sched_pending 则 pend 上下文切换。 */
void os_EnterCritical(void);
void os_ExitCritical(void);

/* 将 READY 任务加入其有效优先级队列末尾。 */
void os_ReadyEnqueue(os_task_t *task);
/* 将任务从其有效优先级就绪队列移除。 */
bool os_ReadyRemove(os_task_t *task);
/* 弹出最高非空优先级队列的表头任务。 */
os_task_t *os_ReadyPopHighest(void);
/* 返回最高非空就绪优先级；没有 READY 任务时返回 -1。 */
int os_ReadyHighestPriority(void);

/* 启动调度并选择首个最高优先级任务。 */
os_task_t *os_SchedStart(void);
/**
 * @brief 根据当前任务与最高 READY 优先级决定是否切换。
 * @param reason 本次调度判断对应的触发原因。
 * @param rotate_equal 为 true 时允许与同优先级 READY 任务轮转。
 * @return 调度后应当运行的任务；无可运行任务时为 NULL。
 */
os_task_t *os_SchedSchedule(os_switch_reason_t reason, bool rotate_equal);
/* 将当前任务放回就绪队列并重新选择任务。 */
os_task_t *os_SchedYieldCurrent(void);
/* 当前任务已完成阻塞状态迁移后选择下一任务。 */
os_task_t *os_SchedCurrentBlocked(os_switch_reason_t reason);
/* 终止当前任务、释放其互斥量并选择下一任务。 */
os_task_t *os_SchedTerminateCurrent(void);

/**
 * @brief 将当前任务挂入一个按优先级排列的对象等待队列。
 * @param wait_list 目标对象的等待队列。
 * @param kind 等待操作类型。
 * @param object 正在等待的内核对象。
 * @param buffer 队列发送或接收使用的任务缓冲区。
 * @param timeout_ticks 有限等待 tick 或 OS_WAIT_FOREVER。
 * @return 当前任务阻塞后选出的下一任务。
 */
os_task_t *os_WaitBlockCurrent(
    os_list_t *wait_list,
    os_wait_kind_t kind,
    void *object,
    void *buffer,
    uint32_t timeout_ticks
);
/* 弹出对象等待队列中优先级最高且最早到达的任务。 */
os_task_t *os_WaitPop(os_list_t *wait_list);
/* 清除对象等待信息并将任务重新加入就绪队列。 */
void os_WaitMakeReady(os_task_t *task, os_status_t result);
/* 被唤醒任务优先级更高时立即执行一次抢占调度（临界区内改为 sched_pending）。 */
void os_WaitMaybePreempt(os_task_t *woken_task);
/* 修改任务有效优先级，并同步调整其所在调度队列。 */
void os_TaskSetEffectivePriority(os_task_t *task, uint8_t priority);

/* 判断是否仍存在未终止的非 Idle 任务。 */
bool os_KernelHasLiveTasks(void);
/* 宿主运行结束后清空调度容器并终止全部任务。 */
void os_KernelStopAll(void);
/* 检查任务状态、链表成员关系和就绪位图等内核不变量。 */
bool os_KernelValidate(void);

#endif
