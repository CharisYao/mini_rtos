/**
 * @file os_task.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 实现内核初始化、静态任务创建以及任务信息查询功能。
 *
 * @details
 * 本文件定义唯一的全局内核实例。os_Init() 负责复位 TCB、就绪队列、延时队列、
 * 系统 tick 以及信号量、消息队列和互斥量对象池；os_TaskCreate() 校验任务参数，
 * 从固定 TCB 数组分配槽位，初始化 port 栈帧，并把新任务加入就绪队列。
 *
 * 自动 Idle 由 os_EnsureIdleTask() 在 os_Start() 前创建。真正的宿主线程和运行
 * 对象由移植层在 os_port_start_scheduler() 中建立。
 */

#include "os_kernel_state.h"
#include "os_task_internal.h"
#include "os_list.h"
#include "os_sched.h"
#include "os_sem_internal.h"
#include "os_queue_internal.h"
#include "os_mutex_internal.h"
#include "os_port.h"

#include <string.h>

os_kernel_t g_os_kernel;

static uint8_t g_idle_stack[OS_IDLE_STACK_BYTES];
static os_task_t *g_idle_task = NULL;

static void os_IdleTaskEntry(void *argument)
{
    (void)argument;

    for (;;) {
        if (!os_port_is_running()) {
            return;
        }
        os_Yield();
    }
}

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

static os_status_t os_TaskCreateInternal(
    os_task_t **out_task,
    const char *name,
    os_task_entry_t entry,
    void *argument,
    uint8_t priority,
    void *stack_memory,
    size_t stack_size,
    bool allow_idle_priority
)
{
    size_t name_length;
    os_task_t *task;
    void *sp;

    if (out_task != NULL) {
        *out_task = NULL;
    }

    if (!g_os_kernel.initialized || g_os_kernel.started) {
        return OS_STATUS_BAD_STATE;
    }

    if ((out_task == NULL) || (name == NULL) || (entry == NULL) ||
        (stack_memory == NULL)) {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    name_length = os_TaskNameLength(name);
    if ((name_length == 0U) || (name_length >= OS_TASK_NAME_MAX)) {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (allow_idle_priority) {
        if (priority >= OS_PRIORITY_COUNT) {
            return OS_STATUS_INVALID_ARGUMENT;
        }
    } else if ((priority <= OS_IDLE_PRIORITY) ||
               (priority >= OS_PRIORITY_COUNT)) {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (stack_size < OS_MIN_STACK_BYTES) {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (g_os_kernel.task_count >= OS_MAX_TASKS) {
        return OS_STATUS_LIMIT_REACHED;
    }

    sp = os_port_stack_init(entry, argument, stack_memory, stack_size);
    if (sp == NULL) {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    task = &g_os_kernel.tasks[g_os_kernel.task_count];
    memcpy(task->name, name, name_length + 1U);
    task->entry = entry;
    task->argument = argument;
    task->sp = sp;
    task->stack_memory = stack_memory;
    task->stack_size = stack_size;
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

/**
 * @brief 复位通用内核、内核对象池和全部静态 TCB 槽位。
 */
void os_Init(void)
{
    size_t index;

    memset(&g_os_kernel, 0, sizeof(g_os_kernel));
    g_idle_task = NULL;

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

os_status_t os_TaskCreate(
    os_task_t **out_task,
    const char *name,
    os_task_entry_t entry,
    void *argument,
    uint8_t priority,
    void *stack_memory,
    size_t stack_size
)
{
    return os_TaskCreateInternal(
        out_task,
        name,
        entry,
        argument,
        priority,
        stack_memory,
        stack_size,
        false
    );
}

os_status_t os_EnsureIdleTask(void)
{
    if (g_idle_task != NULL) {
        return OS_STATUS_OK;
    }

    return os_TaskCreateInternal(
        &g_idle_task,
        "Idle",
        os_IdleTaskEntry,
        NULL,
        OS_IDLE_PRIORITY,
        g_idle_stack,
        sizeof(g_idle_stack),
        true
    );
}

bool os_TaskIsIdle(const os_task_t *task)
{
    return (task != NULL) && (task->base_priority == OS_IDLE_PRIORITY);
}

const char *os_TaskGetName(const os_task_t *task)
{
    return (task != NULL) ? task->name : NULL;
}

os_task_state_t os_TaskGetState(const os_task_t *task)
{
    return (task != NULL) ? task->state : OS_TASK_UNUSED;
}

uint8_t os_TaskGetPriority(const os_task_t *task)
{
    return (task != NULL) ? task->effective_priority : OS_IDLE_PRIORITY;
}

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
