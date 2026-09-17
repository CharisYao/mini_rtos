/**
 * @file os_sched.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 实现就绪队列管理、固定优先级调度、任务阻塞唤醒和内核状态检查。
 *
 * @details
 * 每个优先级拥有一条 FIFO 就绪队列，并由就绪位图快速判断最高非空优先级。
 * 调度器始终优先选择有效优先级最高的 READY 任务；同优先级任务仅在主动让出
 * 或时间片轮转时重新排队。所有切换都会统一更新当前任务和可观察的切换轨迹。
 *
 * 本文件还负责把任务挂入按优先级排列的对象等待队列、将被唤醒任务恢复为
 * READY，以及在优先级继承改变有效优先级后重新调整任务所在队列。运行结束处理、
 * 快照复制和内核不变量检查也集中在此实现。这里不操作 Windows 线程，具体上下文
 * 承载和暂停恢复由 port 层根据调度结果执行。
 */

#include "os_sched.h"
#include "os_list.h"
#include "os_time.h"
#include "os_mutex_internal.h"
#include "os_port.h"

#include <stddef.h>

/* 将优先级转换为就绪位图中的单个位。 */
static uint8_t os_PriorityMask(uint8_t priority)
{
    return (uint8_t)(1U << priority);
}

/**
 * @brief 提交一次调度结果并更新可观察的切换轨迹。
 * @param previous 切换前任务，可以为 NULL。
 * @param next 切换后任务，可以为 NULL 表示暂时空闲。
 * @param reason 触发本次调度的原因。
 * @return next，便于调用者直接继续处理目标任务。
 */
static os_task_t *os_CommitSwitch(
    os_task_t *previous,
    os_task_t *next,
    os_switch_reason_t reason
)
{
    if (next != NULL) {
        next->state = OS_TASK_RUNNING;
        next->slice_remaining = OS_TIME_SLICE_TICKS;
    }

    g_os_kernel.current = next;

    /* 重新选择到同一任务不算真实上下文切换，也不累计轨迹。 */
    if (previous != next) {
        g_os_kernel.last_from = previous;
        g_os_kernel.last_to = next;
        g_os_kernel.last_reason = reason;
        g_os_kernel.switch_count++;
    }

    return next;
}

/* 进入内核临界区：先屏蔽 port 侧抢占，再增加嵌套计数。 */
void os_EnterCritical(void)
{
    os_port_enter_critical();
    g_os_kernel.critical_nesting++;
}

/* 退出内核临界区；最外层且存在 sched_pending 时请求一次上下文切换。 */
void os_ExitCritical(void)
{
    if (g_os_kernel.critical_nesting == 0U) {
        os_port_exit_critical();
        return;
    }

    g_os_kernel.critical_nesting--;
    if ((g_os_kernel.critical_nesting == 0U) && g_os_kernel.sched_pending) {
        g_os_kernel.sched_pending = false;
        (void)os_SchedSchedule(OS_SWITCH_HIGHER_PRIORITY_WAKEUP, false);
        os_port_pend_context_switch();
    }
    os_port_exit_critical();
}

/**
 * @brief 将 READY 任务追加到其有效优先级 FIFO 队列。
 * @param task 待加入任务；状态或节点关系不合法时不执行操作。
 */
void os_ReadyEnqueue(os_task_t *task)
{
    uint8_t priority;

    if ((task == NULL) || (task->state != OS_TASK_READY)) {
        return;
    }

    priority = task->effective_priority;
    if ((priority >= OS_PRIORITY_COUNT) || task->schedule_node.linked) {
        return;
    }

    os_ListPushBack(&g_os_kernel.ready[priority], &task->schedule_node);
    g_os_kernel.ready_bitmap |= os_PriorityMask(priority);
}

/**
 * @brief 从任务有效优先级对应的就绪队列移除任务。
 * @param task 待移除任务。
 * @return 成功完成移除返回 true，参数或节点状态无效时返回 false。
 */
bool os_ReadyRemove(os_task_t *task)
{
    uint8_t priority;
    os_list_t *list;

    if ((task == NULL) || !task->schedule_node.linked) {
        return false;
    }

    priority = task->effective_priority;
    if (priority >= OS_PRIORITY_COUNT) {
        return false;
    }

    list = &g_os_kernel.ready[priority];
    os_ListRemove(list, &task->schedule_node);
    if (list->size == 0U) {
        g_os_kernel.ready_bitmap &= (uint8_t)~os_PriorityMask(priority);
    }

    return true;
}

/* 从高到低扫描位图，返回最高非空就绪优先级。 */
int os_ReadyHighestPriority(void)
{
    int priority;

    for (priority = (int)OS_PRIORITY_COUNT - 1; priority >= 0; priority--) {
        if ((g_os_kernel.ready_bitmap & os_PriorityMask((uint8_t)priority)) != 0U) {
            return priority;
        }
    }

    return -1;
}

/* 弹出最高优先级队列的表头，实现同优先级 FIFO 选择。 */
os_task_t *os_ReadyPopHighest(void)
{
    int priority = os_ReadyHighestPriority();
    os_list_t *list;
    os_list_node_t *node;

    if (priority < 0) {
        return NULL;
    }

    list = &g_os_kernel.ready[(uint8_t)priority];
    node = os_ListPopFront(list);
    if (list->size == 0U) {
        g_os_kernel.ready_bitmap &= (uint8_t)~os_PriorityMask((uint8_t)priority);
    }

    return (node != NULL) ? node->owner : NULL;
}

/* 首次启动时选择最高优先级 READY 任务。 */
os_task_t *os_SchedStart(void)
{
    os_task_t *next;

    if (!g_os_kernel.initialized || g_os_kernel.started) {
        return g_os_kernel.current;
    }

    next = os_ReadyPopHighest();
    if (next == NULL) {
        return NULL;
    }

    g_os_kernel.started = true;
    return os_CommitSwitch(NULL, next, OS_SWITCH_START);
}

/**
 * @brief 按固定优先级和可选的同优先级轮转规则执行调度判断。
 * @param reason 若发生切换，记录到运行轨迹中的原因。
 * @param rotate_equal 是否允许当前任务与同优先级 READY 任务轮转。
 * @return 调度后应运行的任务；没有任务可运行时返回 NULL。
 */
os_task_t *os_SchedSchedule(os_switch_reason_t reason, bool rotate_equal)
{
    os_task_t *current = g_os_kernel.current;
    os_task_t *next;
    int highest_ready;

    if (!g_os_kernel.started) {
        return NULL;
    }

    if ((current == NULL) || (current->state != OS_TASK_RUNNING)) {
        next = os_ReadyPopHighest();
        return os_CommitSwitch(current, next, reason);
    }

    highest_ready = os_ReadyHighestPriority();
    if (highest_ready < 0) {
        return current;
    }

    /* 更高优先级 READY 任务无条件抢占当前任务。 */
    if ((uint8_t)highest_ready > current->effective_priority) {
        current->state = OS_TASK_READY;
        os_ReadyEnqueue(current);
        next = os_ReadyPopHighest();
        return os_CommitSwitch(current, next, reason);
    }

    /* 只有显式允许轮转时，同优先级任务才会替换当前任务。 */
    if (rotate_equal && ((uint8_t)highest_ready == current->effective_priority)) {
        current->state = OS_TASK_READY;
        os_ReadyEnqueue(current);
        next = os_ReadyPopHighest();
        return os_CommitSwitch(current, next, reason);
    }

    return current;
}

/* 主动让出时把当前任务放回队尾，再从最高优先级重新选择。 */
os_task_t *os_SchedYieldCurrent(void)
{
    os_task_t *previous = g_os_kernel.current;
    os_task_t *next;

    if ((previous == NULL) || (previous->state != OS_TASK_RUNNING)) {
        return previous;
    }

    previous->state = OS_TASK_READY;
    os_ReadyEnqueue(previous);
    next = os_ReadyPopHighest();
    return os_CommitSwitch(previous, next, OS_SWITCH_YIELD);
}

/* 当前任务已不再 RUNNING 时，选择新的最高优先级任务。 */
os_task_t *os_SchedCurrentBlocked(os_switch_reason_t reason)
{
    os_task_t *previous = g_os_kernel.current;
    os_task_t *next;

    if ((previous == NULL) || (previous->state == OS_TASK_RUNNING)) {
        return previous;
    }

    next = os_ReadyPopHighest();
    return os_CommitSwitch(previous, next, reason);
}

/* 终止当前任务，并在切换前移交其仍持有的互斥量。 */
os_task_t *os_SchedTerminateCurrent(void)
{
    os_task_t *previous = g_os_kernel.current;
    os_task_t *next;

    if ((previous == NULL) || (previous->state != OS_TASK_RUNNING)) {
        return previous;
    }

    os_MutexReleaseTask(previous);
    previous->state = OS_TASK_TERMINATED;
    next = os_ReadyPopHighest();
    return os_CommitSwitch(previous, next, OS_SWITCH_EXIT);
}

/**
 * @brief 按有效优先级降序插入对象等待队列。
 * @param wait_list 目标等待队列。
 * @param task 待阻塞任务。
 *
 * 相同优先级不会插到已有任务之前，因此保持先到先服务。
 */
static void os_WaitInsertByPriority(os_list_t *wait_list, os_task_t *task)
{
    os_list_node_t *node = wait_list->head;

    while (node != NULL) {
        if (task->effective_priority > node->owner->effective_priority) {
            os_ListInsertBefore(wait_list, node, &task->schedule_node);
            return;
        }
        node = node->next;
    }

    os_ListPushBack(wait_list, &task->schedule_node);
}

/**
 * @brief 修改有效优先级并保持任务所在调度队列的排序正确。
 * @param task 目标任务。
 * @param priority 新的有效优先级。
 */
void os_TaskSetEffectivePriority(os_task_t *task, uint8_t priority)
{
    os_list_t *wait_list;

    if ((task == NULL) || (priority <= OS_IDLE_PRIORITY) ||
        (priority >= OS_PRIORITY_COUNT) ||
        (task->effective_priority == priority)) {
        return;
    }

    /* READY 任务必须从旧优先级队列迁移到新优先级队列。 */
    if (task->state == OS_TASK_READY) {
        if (!os_ReadyRemove(task)) {
            return;
        }
        task->effective_priority = priority;
        os_ReadyEnqueue(task);
        return;
    }

    /* 对象等待队列按有效优先级排序，继承变化后也要重新插入。 */
    if ((task->state == OS_TASK_BLOCKED_OBJECT) &&
        task->schedule_node.linked && (task->wait_list != NULL)) {
        wait_list = task->wait_list;
        os_ListRemove(wait_list, &task->schedule_node);
        task->effective_priority = priority;
        os_WaitInsertByPriority(wait_list, task);
        return;
    }

    task->effective_priority = priority;
}

/**
 * @brief 记录当前任务的对象等待条件并切换到下一任务。
 * @param wait_list 目标对象等待队列。
 * @param kind 等待操作类型。
 * @param object 目标内核对象。
 * @param buffer 队列操作在阻塞期间保留的消息缓冲区。
 * @param timeout_ticks 有限等待 tick 或 OS_WAIT_FOREVER。
 * @return 当前任务阻塞后选出的任务。
 */
os_task_t *os_WaitBlockCurrent(
    os_list_t *wait_list,
    os_wait_kind_t kind,
    void *object,
    void *buffer,
    uint32_t timeout_ticks
)
{
    os_task_t *current = g_os_kernel.current;

    if ((current == NULL) || (current->state != OS_TASK_RUNNING) ||
        (wait_list == NULL) || (kind == OS_WAIT_NONE)) {
        return current;
    }

    if ((timeout_ticks != OS_WAIT_FOREVER) &&
        (timeout_ticks > OS_MAX_DELAY_TICKS)) {
        timeout_ticks = OS_MAX_DELAY_TICKS;
    }

    current->wait_kind = kind;
    current->wait_object = object;
    current->wait_buffer = buffer;
    current->wait_list = wait_list;
    current->wait_deadline =
        (timeout_ticks == OS_WAIT_FOREVER)
            ? OS_WAIT_FOREVER
            : (os_TickGet() + timeout_ticks);
    current->wait_result = OS_STATUS_BAD_STATE;
    current->state = OS_TASK_BLOCKED_OBJECT;

    os_WaitInsertByPriority(wait_list, current);
    return os_SchedCurrentBlocked(OS_SWITCH_BLOCKED);
}

/* 返回等待队列中优先级最高且同级最早到达的任务。 */
os_task_t *os_WaitPop(os_list_t *wait_list)
{
    os_list_node_t *node;

    if (wait_list == NULL) {
        return NULL;
    }

    node = os_ListPopFront(wait_list);
    return (node != NULL) ? node->owner : NULL;
}

/**
 * @brief 将已经从对象等待队列摘除的任务恢复为 READY。
 * @param task 被唤醒或超时的任务。
 * @param result 阻塞 API 恢复后应返回的结果。
 */
void os_WaitMakeReady(os_task_t *task, os_status_t result)
{
    if ((task == NULL) || (task->state != OS_TASK_BLOCKED_OBJECT) ||
        task->schedule_node.linked) {
        return;
    }

    task->wait_kind = OS_WAIT_NONE;
    task->wait_object = NULL;
    task->wait_buffer = NULL;
    task->wait_list = NULL;
    task->wait_deadline = 0U;
    task->wait_result = result;
    task->state = OS_TASK_READY;
    os_ReadyEnqueue(task);
}

/* 唤醒任务比当前任务优先级高时重新调度；临界区内仅置 sched_pending。 */
void os_WaitMaybePreempt(os_task_t *woken_task)
{
    bool need_switch;

    if (woken_task == NULL) {
        return;
    }

    need_switch = (g_os_kernel.current == NULL) ||
                  (woken_task->effective_priority >
                   g_os_kernel.current->effective_priority);
    if (!need_switch) {
        return;
    }

    if (g_os_kernel.critical_nesting > 0U) {
        g_os_kernel.sched_pending = true;
        return;
    }

    (void)os_SchedSchedule(OS_SWITCH_HIGHER_PRIORITY_WAKEUP, false);
}

/* 复制只读运行轨迹，避免观察器直接修改内核。 */
void os_GetRuntimeSnapshot(os_runtime_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return;
    }

    out_snapshot->tick = g_os_kernel.tick;
    out_snapshot->current = g_os_kernel.current;
    out_snapshot->last_from = g_os_kernel.last_from;
    out_snapshot->last_to = g_os_kernel.last_to;
    out_snapshot->last_reason = g_os_kernel.last_reason;
    out_snapshot->switch_count = g_os_kernel.switch_count;
    out_snapshot->current_slice_remaining =
        (g_os_kernel.current != NULL) ? g_os_kernel.current->slice_remaining : 0U;
}

/* 只要存在未终止的非 Idle 任务，宿主循环就仍有工作。 */
bool os_KernelHasLiveTasks(void)
{
    for (size_t index = 0U; index < g_os_kernel.task_count; index++) {
        const os_task_t *task = &g_os_kernel.tasks[index];
        const os_task_state_t state = task->state;

        if (os_TaskIsIdle(task)) {
            continue;
        }

        if ((state != OS_TASK_UNUSED) && (state != OS_TASK_TERMINATED)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief 宿主运行结束后清空所有调度容器并终止剩余任务。
 *
 * started 故意保持为 true，要求再次运行前重新 os_Init()，避免复用旧线程状态。
 */
void os_KernelStopAll(void)
{
    for (size_t priority = 0U; priority < OS_PRIORITY_COUNT; priority++) {
        os_ListInit(&g_os_kernel.ready[priority]);
    }
    os_ListInit(&g_os_kernel.delayed);

    g_os_kernel.ready_bitmap = 0U;
    g_os_kernel.current = NULL;
    /* 旧任务已经没有可恢复的 Windows 线程，禁止直接再次 os_Start()。 */
    g_os_kernel.started = true;

    for (size_t index = 0U; index < g_os_kernel.task_count; index++) {
        os_task_t *task = &g_os_kernel.tasks[index];

        task->state = OS_TASK_TERMINATED;
        task->wait_kind = OS_WAIT_NONE;
        task->wait_object = NULL;
        task->wait_buffer = NULL;
        task->wait_list = NULL;
        task->wait_deadline = 0U;
        task->schedule_node.previous = NULL;
        task->schedule_node.next = NULL;
        task->schedule_node.linked = false;
    }
}

/**
 * @brief 验证 TCB 状态、链表成员关系和就绪位图之间的一致性。
 * @return 全部内核不变量成立时返回 true。
 *
 * 验证函数用于单元测试和每次 Windows 内核请求后的防御性检查，不修复状态。
 */
bool os_KernelValidate(void)
{
    size_t ready_membership[OS_MAX_TASKS] = {0U};
    size_t delay_membership[OS_MAX_TASKS] = {0U};
    size_t used_tasks = 0U;
    size_t running_tasks = 0U;
    uint8_t expected_bitmap = 0U;

    if (!g_os_kernel.initialized || (g_os_kernel.task_count > OS_MAX_TASKS)) {
        return false;
    }

    /* 第一遍统计每个 TCB 在就绪队列中的出现次数，并重建期望位图。 */
    for (size_t priority = 0U; priority < OS_PRIORITY_COUNT; priority++) {
        const os_list_t *list = &g_os_kernel.ready[priority];
        const os_list_node_t *node = list->head;
        const os_list_node_t *previous = NULL;
        size_t count = 0U;

        while (node != NULL) {
            const os_task_t *task = node->owner;

            if ((count >= OS_MAX_TASKS) || !node->linked || (node->previous != previous)) {
                return false;
            }
            if ((task == NULL) || (task->id >= OS_MAX_TASKS)) {
                return false;
            }
            if (task != &g_os_kernel.tasks[task->id]) {
                return false;
            }
            if ((task->state != OS_TASK_READY) || (task->effective_priority != priority)) {
                return false;
            }

            ready_membership[task->id]++;
            previous = node;
            node = node->next;
            count++;
        }

        if ((count != list->size) || (previous != list->tail)) {
            return false;
        }

        if (count > 0U) {
            expected_bitmap |= os_PriorityMask((uint8_t)priority);
        }
    }

    if (expected_bitmap != g_os_kernel.ready_bitmap) {
        return false;
    }

    /* 第二遍验证延时队列的双向关系和 BLOCKED_DELAY 状态。 */
    {
        const os_list_node_t *node = g_os_kernel.delayed.head;
        const os_list_node_t *previous = NULL;
        size_t count = 0U;

        while (node != NULL) {
            const os_task_t *task = node->owner;

            if ((count >= OS_MAX_TASKS) || !node->linked || (node->previous != previous)) {
                return false;
            }
            if ((task == NULL) || (task->id >= OS_MAX_TASKS)) {
                return false;
            }
            if ((task != &g_os_kernel.tasks[task->id]) ||
                (task->state != OS_TASK_BLOCKED_DELAY)) {
                return false;
            }

            delay_membership[task->id]++;
            previous = node;
            node = node->next;
            count++;
        }

        if ((count != g_os_kernel.delayed.size) || (previous != g_os_kernel.delayed.tail)) {
            return false;
        }
    }

    /* 最后按任务状态核对它应当且只能属于哪一种调度容器。 */
    for (size_t index = 0U; index < OS_MAX_TASKS; index++) {
        const os_task_t *task = &g_os_kernel.tasks[index];

        if ((task->id != index) || (task->schedule_node.owner != task)) {
            return false;
        }

        if (task->state == OS_TASK_UNUSED) {
            if ((ready_membership[index] != 0U) || (delay_membership[index] != 0U)) {
                return false;
            }
            continue;
        }

        used_tasks++;
        if (task->entry == NULL) {
            return false;
        }
        if (os_TaskIsIdle(task)) {
            if ((task->base_priority != OS_IDLE_PRIORITY) ||
                (task->effective_priority != OS_IDLE_PRIORITY)) {
                return false;
            }
        } else if ((task->base_priority <= OS_IDLE_PRIORITY) ||
                   (task->base_priority >= OS_PRIORITY_COUNT) ||
                   (task->effective_priority < task->base_priority) ||
                   (task->effective_priority >= OS_PRIORITY_COUNT)) {
            return false;
        }

        switch (task->state) {
        case OS_TASK_READY:
            if ((ready_membership[index] != 1U) || (delay_membership[index] != 0U)) {
                return false;
            }
            if ((task->wait_kind != OS_WAIT_NONE) || (task->wait_list != NULL)) {
                return false;
            }
            break;
        case OS_TASK_BLOCKED_DELAY:
            if ((ready_membership[index] != 0U) || (delay_membership[index] != 1U)) {
                return false;
            }
            break;
        case OS_TASK_RUNNING:
            running_tasks++;
            if ((g_os_kernel.current != task) ||
                (ready_membership[index] != 0U) ||
                (delay_membership[index] != 0U) ||
                task->schedule_node.linked) {
                return false;
            }
            if ((task->wait_kind != OS_WAIT_NONE) || (task->wait_list != NULL)) {
                return false;
            }
            break;
        case OS_TASK_BLOCKED_OBJECT:
            if ((ready_membership[index] != 0U) || (delay_membership[index] != 0U) ||
                !task->schedule_node.linked || (task->wait_kind == OS_WAIT_NONE) ||
                (task->wait_list == NULL) ||
                !os_ListContains(task->wait_list, &task->schedule_node)) {
                return false;
            }
            break;
        case OS_TASK_TERMINATED:
            if ((ready_membership[index] != 0U) || (delay_membership[index] != 0U) ||
                task->schedule_node.linked) {
                return false;
            }
            break;
        case OS_TASK_UNUSED:
        default:
            return false;
        }
    }

    if (used_tasks != g_os_kernel.task_count) {
        return false;
    }
    if (running_tasks > 1U) {
        return false;
    }
    if ((g_os_kernel.current != NULL) && (g_os_kernel.current->state != OS_TASK_RUNNING)) {
        return false;
    }
    if ((g_os_kernel.current == NULL) && (running_tasks != 0U)) {
        return false;
    }

    return true;
}
