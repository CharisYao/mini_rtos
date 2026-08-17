/**
 * @file os_task.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 实现内核初始化、静态任务创建以及任务信息查询功能。
 *
 * @details
 * 本文件定义唯一的全局内核实例。os_Init() 负责复位 TCB、就绪队列、延时队列、
 * 系统 tick 以及信号量、消息队列和互斥量对象池；os_TaskCreate() 校验任务参数，
 * 从固定 TCB 数组分配槽位，并把新任务加入对应优先级的就绪队列。
 *
 * 任务创建阶段只建立平台无关的内核状态，不创建 Windows 线程。真正的宿主线程
 * 和运行对象由 Win32 移植层在 os_Start() 中建立。本文件还提供任务名称、状态、
 * 有效优先级以及状态文本等只读查询功能。
 */

#include "os_internal.h"

#include <string.h>

os_kernel_t g_os_kernel;

/* 在固定上限内检查任务名长度，避免读取未终止字符串。 */
static size_t os_TaskNameLength(const char *name)
{
    size_t length = 0U;

    if (name == NULL) {
        return 0U;
    }

    while ((length < OS_TASK_NAME_MAX) && (name[length] != '\0')) {
        length++;
    }

    return length;
}

/**
 * @brief 复位通用内核、内核对象池和全部静态 TCB 槽位。
 *
 * 初始化不会创建 Windows 线程；宿主运行对象统一留到 os_Start() 创建。
 */
void os_Init(void)
{
    size_t index;

    memset(&g_os_kernel, 0, sizeof(g_os_kernel));

    for (index = 0U; index < OS_PRIORITY_COUNT; index++) {
        os_ListInit(&g_os_kernel.ready[index]);
    }
    os_ListInit(&g_os_kernel.delayed);
    atomic_init(&g_os_kernel.tick, 0U);
    os_SemKernelReset();
    os_QueueKernelReset();
    os_MutexKernelReset();

    for (index = 0U; index < OS_MAX_TASKS; index++) {
        os_task_t *task = &g_os_kernel.tasks[index];

        task->id = (uint8_t)index;
        task->state = OS_TASK_UNUSED;
        task->schedule_node.owner = task;
    }

    g_os_kernel.initialized = true;
}

/**
 * @brief 校验任务参数并从静态数组中分配下一个 TCB。
 * @param out_task 返回创建成功的任务；任何失败路径都保持为 NULL。
 * @param name 任务显示名称。
 * @param entry 任务入口函数。
 * @param argument 传给任务入口的用户参数。
 * @param priority 基础优先级，0 为保留值。
 * @return 创建结果。
 */
os_status_t os_TaskCreate(
    os_task_t **out_task,
    const char *name,
    os_task_entry_t entry,
    void *argument,
    uint8_t priority
)
{
    size_t name_length;
    os_task_t *task;

    if (out_task != NULL) {
        *out_task = NULL;
    }

    if (!g_os_kernel.initialized || g_os_kernel.started) {
        return OS_STATUS_BAD_STATE;
    }

    if ((out_task == NULL) || (name == NULL) || (entry == NULL)) {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    name_length = os_TaskNameLength(name);
    if ((name_length == 0U) || (name_length >= OS_TASK_NAME_MAX)) {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if ((priority <= OS_IDLE_PRIORITY) || (priority >= OS_PRIORITY_COUNT)) {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (g_os_kernel.task_count >= OS_MAX_TASKS) {
        return OS_STATUS_LIMIT_REACHED;
    }

    /* TCB 池按创建顺序连续使用，不涉及运行期动态内存。 */
    task = &g_os_kernel.tasks[g_os_kernel.task_count];
    memcpy(task->name, name, name_length + 1U);
    task->entry = entry;
    task->argument = argument;
    task->base_priority = priority;
    task->effective_priority = priority;
    task->state = OS_TASK_READY;
    task->wake_tick = 0U;
    task->slice_remaining = 0U;
    task->wait_kind = OS_WAIT_NONE;
    task->wait_object = NULL;
    task->wait_buffer = NULL;
    task->wait_list = NULL;
    task->wait_deadline = 0U;
    task->wait_result = OS_STATUS_BAD_STATE;

    os_ReadyEnqueue(task);
    g_os_kernel.task_count++;
    *out_task = task;
    return OS_STATUS_OK;
}

/* 返回只读任务名，NULL 输入不解引用。 */
const char *os_TaskGetName(const os_task_t *task)
{
    return (task != NULL) ? task->name : NULL;
}

/* 返回任务状态，NULL 统一映射为未使用槽位。 */
os_task_state_t os_TaskGetState(const os_task_t *task)
{
    return (task != NULL) ? task->state : OS_TASK_UNUSED;
}

/* 返回用于当前调度的有效优先级，而不是固定基础优先级。 */
uint8_t os_TaskGetPriority(const os_task_t *task)
{
    return (task != NULL) ? task->effective_priority : OS_IDLE_PRIORITY;
}

/* 将任务状态枚举转换为控制台可读文本。 */
const char *os_TaskStateName(os_task_state_t state)
{
    switch (state) {
    case OS_TASK_UNUSED:
        return "UNUSED";
    case OS_TASK_READY:
        return "READY";
    case OS_TASK_RUNNING:
        return "RUNNING";
    case OS_TASK_BLOCKED_DELAY:
        return "BLOCKED_DELAY";
    case OS_TASK_BLOCKED_OBJECT:
        return "BLOCKED_OBJECT";
    case OS_TASK_TERMINATED:
        return "TERMINATED";
    default:
        return "INVALID";
    }
}

/* 将最近一次调度原因转换为控制台可读文本。 */
const char *os_SwitchReasonName(os_switch_reason_t reason)
{
    switch (reason) {
    case OS_SWITCH_NONE:
        return "none";
    case OS_SWITCH_START:
        return "start";
    case OS_SWITCH_TIME_SLICE:
        return "time_slice";
    case OS_SWITCH_YIELD:
        return "yield";
    case OS_SWITCH_HIGHER_PRIORITY_WAKEUP:
        return "higher_priority_wakeup";
    case OS_SWITCH_BLOCKED:
        return "blocked";
    case OS_SWITCH_EXIT:
        return "exit";
    default:
        return "invalid";
    }
}
