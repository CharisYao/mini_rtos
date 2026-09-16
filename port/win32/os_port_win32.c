/**
 * @file os_port_win32.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 使用 Windows 线程、事件和定时器承载任务，并执行通用内核的调度结果。
 *
 * @details
 * 每个 miniRTOS 任务对应一个 Windows 工作线程，Windows 线程保存任务的独立栈和
 * CPU 现场；调用 os_Start() 的线程作为宿主控制线程，等待周期定时器和任务请求事件，
 * 再调用 kernel 层完成 tick 处理和调度决策。
 *
 * os_Yield()、os_Delay()、信号量、队列和互斥量公开接口在此转换为串行内核请求。
 * 工作线程通过 TLS 找到自身请求槽位，并使用事件 gate 等待内核处理结果。时间片到期
 * 时只允许暂停处于用户代码阶段的线程；若线程正在提交内核请求，则暂存 tick，等状态
 * 迁移完成后再处理，避免破坏内核链表。
 *
 * 本文件还负责线程亲和性、优先级、暂停恢复、观察器回调和所有 Windows 句柄的创建
 * 与回收。信号量、队列和互斥量的实际语义仍位于 kernel 层，本文件只负责平台承载。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "os.h"
#include "os_port.h"
#include "os_list.h"
#include "os_task_internal.h"
#include "os_kernel_state.h"
#include "os_sched.h"
#include "os_time.h"
#include "os_sem_internal.h"
#include "os_queue_internal.h"
#include "os_mutex_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Windows 工作线程当前所处的宿主执行阶段 */
typedef enum {
    OS_PORT_PHASE_CREATED = 0, /* 线程已创建但尚未开始执行任务入口。 */
    OS_PORT_PHASE_USER,        /* 正在执行可被 tick 抢占的任务代码。 */
    OS_PORT_PHASE_REQUEST,     /* 已提交内核请求并等待调度结果。 */
    OS_PORT_PHASE_EXITED       /* 任务入口已经返回，线程即将退出。 */
} os_port_phase_t;

/* Mailbox idle value; active codes match os_port_req_id_t. */
enum { OS_PORT_REQUEST_NONE = 0 };

/* 一个 TCB 在 Windows 上对应的线程、门事件和请求邮箱 */
typedef struct {
    os_task_t *task;                 /* 与该槽位绑定的通用 TCB。 */
    HANDLE thread;                   /* 保存任务栈和 CPU 现场的工作线程。 */
    HANDLE gate;                     /* 内核处理完成后放行任务 API 的自动复位事件。 */
    volatile LONG phase;             /* os_port_phase_t，使用原子操作访问。 */
    volatile LONG request;           /* os_port_req_id_t request code. */
    volatile LONG request_ticks;     /* 延时或超时参数。 */
    PVOID volatile request_object;   /* 信号量、队列或互斥量地址。 */
    PVOID volatile request_buffer;   /* 队列发送或接收缓冲区地址。 */
    volatile LONG response;          /* 内核处理后返回给任务的 os_status_t。 */
    bool suspended;                  /* 是否由 SuspendThread 强制暂停。 */
} os_port_task_slot_t;

/* 一次 Windows 宿主调度运行使用的全部平台对象 */
typedef struct {
    os_port_task_slot_t slots[OS_MAX_TASKS]; /* 每个 TCB 对应一个工作线程槽位。 */
    HANDLE request_event;                    /* 通知控制线程处理任务请求。 */
    HANDLE tick_timer;                       /* 提供周期系统 tick 的等待定时器。 */
    DWORD tls_index;                         /* 工作线程查询自身槽位的 TLS 键。 */
    volatile LONG running;                   /* 宿主循环运行标志。 */
    volatile LONG failed;                    /* 不可恢复的移植层错误标志。 */
    uint32_t run_ticks;                      /* 演示有限运行长度，0 表示不限。 */
    uint32_t pending_ticks;                  /* 内核请求期间暂存的到期 tick 数。 */
    DWORD_PTR scheduler_affinity_before;     /* 控制线程原 CPU 亲和掩码。 */
    int scheduler_priority_before;           /* 控制线程原 Windows 优先级。 */
} os_port_runtime_t;

/* port 运行状态只在 os_Start() 生命周期内有效。 */
static os_port_runtime_t g_port;
/* 观察器由宿主控制线程调用，不属于 miniRTOS 任务。 */
static os_observer_t g_observer;
static void *g_observer_context;
static volatile LONG g_port_critical_nesting;
static volatile LONG g_port_switch_pended;

/* 根据稳定的 TCB id 找到对应 Windows 工作线程槽位。 */
static os_port_task_slot_t *os_PortSlotForTask(const os_task_t *task)
{
    if ((task == NULL) || (task->id >= OS_MAX_TASKS)) {
        return NULL;
    }

    return &g_port.slots[task->id];
}

/* 使用 InterlockedCompareExchange 实现不修改值的原子 LONG 读取。 */
static LONG os_PortReadLong(volatile LONG *value)
{
    return InterlockedCompareExchange(value, 0L, 0L);
}

/* 记录不可恢复错误并要求宿主循环尽快退出。 */
static void os_PortFail(void)
{
    InterlockedExchange(&g_port.failed, 1L);
    InterlockedExchange(&g_port.running, 0L);
}

/* 原子读取移植层失败标志。 */
static bool os_PortFailed(void)
{
    return os_PortReadLong(&g_port.failed) != 0L;
}

/* ---- port HAL ---------------------------------------------------------- */

void os_port_enter_critical(void)
{
    InterlockedIncrement(&g_port_critical_nesting);
}

void os_port_exit_critical(void)
{
    LONG nesting = InterlockedDecrement(&g_port_critical_nesting);
    if (nesting < 0L) {
        InterlockedExchange(&g_port_critical_nesting, 0L);
    }
}

uint32_t os_port_critical_nesting(void)
{
    return (uint32_t)os_PortReadLong(&g_port_critical_nesting);
}

void os_port_pend_context_switch(void)
{
    InterlockedExchange(&g_port_switch_pended, 1L);
    if (g_port.request_event != NULL) {
        (void)SetEvent(g_port.request_event);
    }
}

void *os_port_stack_init(
    os_task_entry_t entry,
    void *argument,
    void *stack_memory,
    size_t stack_size
)
{
    (void)entry;
    (void)argument;

    if ((stack_memory == NULL) || (stack_size < OS_MIN_STACK_BYTES)) {
        return NULL;
    }

    /* Real context lives on the worker thread stack; record a descending top. */
    return (uint8_t *)stack_memory + stack_size;
}

bool os_port_is_running(void)
{
    return os_PortReadLong(&g_port.running) != 0L;
}

void os_port_set_observer(os_observer_t observer, void *context)
{
    g_observer = observer;
    g_observer_context = context;
}

os_status_t os_port_task_request(
    os_port_req_id_t request,
    uint32_t ticks,
    void *object,
    void *buffer
)
{
    os_port_task_slot_t *slot;
    const bool wait = (request != OS_PORT_REQ_EXIT);

    if (!os_port_is_running() || (g_port.tls_index == TLS_OUT_OF_INDEXES)) {
        return OS_STATUS_BAD_STATE;
    }

    slot = (os_port_task_slot_t *)TlsGetValue(g_port.tls_index);
    if (slot == NULL) {
        return OS_STATUS_BAD_STATE;
    }

    os_port_enter_critical();

    InterlockedExchange(&slot->response, (LONG)OS_STATUS_BAD_STATE);
    (void)InterlockedExchangePointer(&slot->request_object, object);
    (void)InterlockedExchangePointer(&slot->request_buffer, buffer);
    InterlockedExchange(&slot->request_ticks, (LONG)ticks);
    InterlockedExchange(&slot->request, (LONG)request);
    InterlockedExchange(&slot->phase, (LONG)OS_PORT_PHASE_REQUEST);

    os_port_exit_critical();

    if (!SetEvent(g_port.request_event)) {
        os_PortFail();
        return OS_STATUS_BAD_STATE;
    }

    if (wait) {
        if (WaitForSingleObject(slot->gate, INFINITE) != WAIT_OBJECT_0) {
            os_PortFail();
            return OS_STATUS_BAD_STATE;
        }
        InterlockedExchange(&slot->phase, (LONG)OS_PORT_PHASE_USER);
        return (os_status_t)os_PortReadLong(&slot->response);
    }

    return OS_STATUS_OK;
}


/* Enable ANSI escapes when possible; failure only affects the dashboard. */
static void os_PortEnableVirtualTerminal(void)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;

    if ((output != INVALID_HANDLE_VALUE) && GetConsoleMode(output, &mode)) {
        (void)SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

/**
 * @brief 所有 Windows 工作线程共用的入口包装。
 * @param parameter 指向该任务的 os_port_task_slot_t。
 * @return 0 表示正常退出，TLS 初始化失败返回非 0。
 *
 * 包装器先登记 TLS，再调用真正的 miniRTOS 任务入口。任务函数返回时提交 EXIT
 * 请求，使通用内核能够把对应 TCB 迁移到 TERMINATED。
 */
static DWORD WINAPI os_PortTaskEntry(LPVOID parameter)
{
    os_port_task_slot_t *slot = (os_port_task_slot_t *)parameter;

    if ((slot == NULL) || !TlsSetValue(g_port.tls_index, slot)) {
        os_PortFail();
        return 1U;
    }

    InterlockedExchange(&slot->phase, (LONG)OS_PORT_PHASE_USER);

    if (os_port_is_running()) {
        slot->task->entry(slot->task->argument);
    }

    if (os_port_is_running()) {
        (void)os_port_task_request(OS_PORT_REQ_EXIT, 0U, NULL, NULL);
    }

    InterlockedExchange(&slot->phase, (LONG)OS_PORT_PHASE_EXITED);
    return 0U;
}

/**
 * @brief 让内核选中的任务线程恢复执行。
 * @param task 目标 TCB；NULL 表示当前没有任务可运行。
 * @return 恢复线程或触发 gate 成功时返回 true。
 *
 * 被 tick 强制暂停的任务通过 ResumeThread 恢复；停在内核 API gate 上的任务通过
 * SetEvent 恢复。写入 response 必须先于放行任务。
 */
static bool os_PortActivate(os_task_t *task)
{
    os_port_task_slot_t *slot;

    if (task == NULL) {
        return true;
    }

    slot = os_PortSlotForTask(task);
    if ((slot == NULL) || (slot->thread == NULL)) {
        return false;
    }

    InterlockedExchange(&slot->response, (LONG)task->wait_result);

    if (slot->suspended) {
        if (ResumeThread(slot->thread) == (DWORD)-1) {
            return false;
        }
        slot->suspended = false;
        return true;
    }

    return SetEvent(slot->gate) != 0;
}

/**
 * @brief 在 tick 边界强制暂停当前处于用户阶段的任务线程。
 * @return 成功暂停或当前本来为空闲状态时返回 true。
 *
 * 仅允许暂停 OS_PORT_PHASE_USER，避免把线程冻结在请求握手或退出流程中。
 */
static bool os_PortSuspendCurrent(void)
{
    os_port_task_slot_t *slot = os_PortSlotForTask(g_os_kernel.current);
    DWORD previous_count;

    if (slot == NULL) {
        return g_os_kernel.current == NULL;
    }

    if (os_PortReadLong(&slot->phase) != (LONG)OS_PORT_PHASE_USER) {
        return false;
    }

    previous_count = SuspendThread(slot->thread);
    if (previous_count == (DWORD)-1) {
        return false;
    }
    if (previous_count != 0U) {
        (void)ResumeThread(slot->thread);
        return false;
    }

    slot->suspended = true;
    return true;
}

/* 按观察周期刷新面板，并在有限演示达到 run_ticks 时请求停止。 */
static void os_PortObserveTick(void)
{
    const uint32_t tick = os_TickGet();

    if ((g_observer != NULL) && ((tick % OS_OBSERVER_PERIOD_TICKS) == 0U)) {
        os_runtime_snapshot_t snapshot;

        os_GetRuntimeSnapshot(&snapshot);
        g_observer(&snapshot, g_observer_context);
    }

    if ((g_port.run_ticks != 0U) && (tick >= g_port.run_ticks)) {
        InterlockedExchange(&g_port.running, 0L);
    }
}

/* 执行一个通用内核 tick、验证不变量并通知观察器。 */
static bool os_PortApplyTick(void)
{
    (void)os_TimeTick();
    if (!os_KernelValidate()) {
        os_PortFail();
        return false;
    }
    os_PortObserveTick();
    return os_port_is_running();
}

/* 扫描固定任务槽位，找到唯一的待处理请求邮箱。 */
static os_port_task_slot_t *os_PortFindRequester(void)
{
    for (size_t index = 0U; index < g_os_kernel.task_count; index++) {
        os_port_task_slot_t *slot = &g_port.slots[index];

        if (os_PortReadLong(&slot->request) != (LONG)OS_PORT_REQUEST_NONE) {
            return slot;
        }
    }

    return NULL;
}

/**
 * @brief 在宿主控制线程中取出并执行一个任务内核请求。
 * @return 请求合法、内核状态有效且下一任务成功激活时返回 true。
 *
 * switch 只负责把平台请求分发到通用 kernel。请求期间积累的 tick 会在状态迁移
 * 完整后补处理，防止 tick 与链表修改交叉执行。
 */
static bool os_PortProcessRequest(void)
{
    os_port_task_slot_t *slot = os_PortFindRequester();
    os_port_req_id_t request;
    void *object;
    void *buffer;

    if (slot == NULL) {
        if (os_PortReadLong(&g_port_switch_pended) != 0L) {
            InterlockedExchange(&g_port_switch_pended, 0L);
            if (!os_KernelValidate()) {
                return false;
            }
            if (os_port_is_running() && !os_PortActivate(g_os_kernel.current)) {
                return false;
            }
            return true;
        }
        return false;
    }

    /* 原子取走请求码和指针，清空邮箱后再验证请求者就是 current。 */
    request = (os_port_req_id_t)InterlockedExchange(
        &slot->request,
        (LONG)OS_PORT_REQUEST_NONE
    );
    object = InterlockedExchangePointer(&slot->request_object, NULL);
    buffer = InterlockedExchangePointer(&slot->request_buffer, NULL);

    if (slot->task != g_os_kernel.current) {
        return false;
    }

    /* 所有 TCB、就绪队列和对象等待队列修改都在控制线程中串行发生。 */
    switch (request) {
    case OS_PORT_REQ_YIELD:
        (void)os_SchedYieldCurrent();
        break;
    case OS_PORT_REQ_DELAY:
        (void)os_TimeDelayCurrent((uint32_t)os_PortReadLong(&slot->request_ticks));
        break;
    case OS_PORT_REQ_SEM_TAKE:
        os_SemTakeCurrent(
            (os_sem_t *)object,
            (uint32_t)os_PortReadLong(&slot->request_ticks)
        );
        break;
    case OS_PORT_REQ_SEM_GIVE:
        os_SemGiveCurrent((os_sem_t *)object);
        break;
    case OS_PORT_REQ_QUEUE_SEND:
        os_QueueSendCurrent(
            (os_queue_t *)object,
            buffer,
            (uint32_t)os_PortReadLong(&slot->request_ticks)
        );
        break;
    case OS_PORT_REQ_QUEUE_RECEIVE:
        os_QueueReceiveCurrent(
            (os_queue_t *)object,
            buffer,
            (uint32_t)os_PortReadLong(&slot->request_ticks)
        );
        break;
    case OS_PORT_REQ_MUTEX_LOCK:
        os_MutexLockCurrent(
            (os_mutex_t *)object,
            (uint32_t)os_PortReadLong(&slot->request_ticks)
        );
        break;
    case OS_PORT_REQ_MUTEX_UNLOCK:
        os_MutexUnlockCurrent((os_mutex_t *)object);
        break;
    case OS_PORT_REQ_EXIT:
        (void)os_SchedTerminateCurrent();
        break;
    case OS_PORT_REQUEST_NONE:
    default:
        return false;
    }

    if (!os_KernelValidate()) {
        return false;
    }

    /* 请求握手期间没有丢弃时间，只把 tick 推迟到安全边界。 */
    while ((g_port.pending_ticks > 0U) && os_port_is_running()) {
        g_port.pending_ticks--;
        (void)os_PortApplyTick();
    }

    if (!os_KernelHasLiveTasks()) {
        InterlockedExchange(&g_port.running, 0L);
    }

    if (os_port_is_running() && !os_PortActivate(g_os_kernel.current)) {
        return false;
    }

    return true;
}

/**
 * @brief 处理一次 Windows 等待定时器到期事件。
 * @return tick 已处理或安全延期时返回 true，线程暂停/恢复失败时返回 false。
 *
 * 当前任务只有在 USER 阶段才允许强制暂停；若它正在提交请求，则累计 pending_ticks，
 * 等请求处理完成后再推进内核时间。
 */
static bool os_PortProcessTimer(void)
{
    os_port_task_slot_t *slot = os_PortSlotForTask(g_os_kernel.current);

    if ((slot != NULL) &&
        (os_PortReadLong(&slot->phase) != (LONG)OS_PORT_PHASE_USER)) {
        g_port.pending_ticks++;
        return true;
    }

    if ((g_os_kernel.current != NULL) && !os_PortSuspendCurrent()) {
        return false;
    }

    if (!os_PortApplyTick()) {
        return true;
    }

    return os_PortActivate(g_os_kernel.current);
}

/**
 * @brief 停止全部任务线程、等待退出并关闭线程与 gate 句柄。
 *
 * 先清除 running，再恢复所有被强制暂停或等待 gate 的线程，使任务有机会观察停止
 * 标志并返回。等待超时会记录移植层失败，但仍继续关闭已创建句柄。
 */
static void os_PortReleaseThreads(void)
{
    HANDLE handles[OS_MAX_TASKS];
    DWORD handle_count = 0U;

    InterlockedExchange(&g_port.running, 0L);

    for (size_t index = 0U; index < g_os_kernel.task_count; index++) {
        os_port_task_slot_t *slot = &g_port.slots[index];

        if (slot->thread == NULL) {
            continue;
        }

        if (slot->suspended) {
            (void)ResumeThread(slot->thread);
            slot->suspended = false;
        }
        if (slot->gate != NULL) {
            (void)SetEvent(slot->gate);
        }

        handles[handle_count++] = slot->thread;
    }

    if (handle_count > 0U) {
        const DWORD wait_result =
            WaitForMultipleObjects(handle_count, handles, TRUE, 5000U);
        if (wait_result == WAIT_FAILED || wait_result == WAIT_TIMEOUT) {
            InterlockedExchange(&g_port.failed, 1L);
        }
    }

    for (size_t index = 0U; index < g_os_kernel.task_count; index++) {
        os_port_task_slot_t *slot = &g_port.slots[index];

        if (slot->thread != NULL) {
            CloseHandle(slot->thread);
            slot->thread = NULL;
        }
        if (slot->gate != NULL) {
            CloseHandle(slot->gate);
            slot->gate = NULL;
        }
    }
}

/**
 * @brief 创建请求事件、周期定时器、TLS 和每个任务的 Windows 工作线程。
 * @return 全部宿主对象创建并配置成功时返回 true。
 *
 * 工作线程和调用 os_Start() 的控制线程都绑定到逻辑 CPU 0，使任意时刻只有一个
 * miniRTOS 任务真实执行，更接近单核 MCU 的运行模型。
 */
static bool os_PortCreateRuntimeObjects(void)
{
    const DWORD_PTR affinity = (DWORD_PTR)1U;

    g_port.request_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_port.tick_timer = CreateWaitableTimerA(NULL, FALSE, NULL);
    g_port.tls_index = TlsAlloc();

    if ((g_port.request_event == NULL) || (g_port.tick_timer == NULL) ||
        (g_port.tls_index == TLS_OUT_OF_INDEXES)) {
        return false;
    }

    for (size_t index = 0U; index < g_os_kernel.task_count; index++) {
        os_port_task_slot_t *slot = &g_port.slots[index];

        slot->task = &g_os_kernel.tasks[index];
        slot->gate = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (slot->gate == NULL) {
            return false;
        }

        /* 线程以暂停状态创建，只有通用调度器选中后才首次恢复。 */
        slot->thread = CreateThread(
            NULL,
            0U,
            os_PortTaskEntry,
            slot,
            CREATE_SUSPENDED,
            NULL
        );
        if (slot->thread == NULL) {
            return false;
        }

        slot->suspended = true;
        InterlockedExchange(&slot->phase, (LONG)OS_PORT_PHASE_CREATED);
        InterlockedExchange(&slot->request, (LONG)OS_PORT_REQUEST_NONE);
        InterlockedExchange(&slot->response, (LONG)OS_STATUS_BAD_STATE);

        if ((SetThreadAffinityMask(slot->thread, affinity) == 0U) ||
            !SetThreadPriority(slot->thread, THREAD_PRIORITY_NORMAL)) {
            return false;
        }
    }

    g_port.scheduler_affinity_before = SetThreadAffinityMask(GetCurrentThread(), affinity);
    if (g_port.scheduler_affinity_before == 0U) {
        return false;
    }

    g_port.scheduler_priority_before = GetThreadPriority(GetCurrentThread());
    if ((g_port.scheduler_priority_before == THREAD_PRIORITY_ERROR_RETURN) ||
        !SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
        return false;
    }

    return true;
}

/* 启动以 OS_TICK_MS 为周期的 Windows 等待定时器。 */
static bool os_PortStartTimer(void)
{
    LARGE_INTEGER due_time;

    due_time.QuadPart = -((LONGLONG)OS_TICK_MS * 10000LL);
    return SetWaitableTimer(
               g_port.tick_timer,
               &due_time,
               (LONG)OS_TICK_MS,
               NULL,
               NULL,
               FALSE
           ) != 0;
}

/* 关闭全局宿主对象，并恢复控制线程原来的优先级和 CPU 亲和性。 */
static void os_PortCloseRuntimeObjects(void)
{
    if (g_port.tick_timer != NULL) {
        (void)CancelWaitableTimer(g_port.tick_timer);
        CloseHandle(g_port.tick_timer);
        g_port.tick_timer = NULL;
    }
    if (g_port.request_event != NULL) {
        CloseHandle(g_port.request_event);
        g_port.request_event = NULL;
    }
    if (g_port.tls_index != TLS_OUT_OF_INDEXES) {
        (void)TlsFree(g_port.tls_index);
        g_port.tls_index = TLS_OUT_OF_INDEXES;
    }

    if (g_port.scheduler_priority_before != THREAD_PRIORITY_ERROR_RETURN) {
        (void)SetThreadPriority(GetCurrentThread(), g_port.scheduler_priority_before);
    }
    if (g_port.scheduler_affinity_before != 0U) {
        (void)SetThreadAffinityMask(
            GetCurrentThread(),
            g_port.scheduler_affinity_before
        );
    }
}

/**
 * @brief 在调用线程中启动并运行 Windows 宿主控制循环。
 * @param run_ticks 0 表示运行到任务全部结束，非 0 表示达到该 tick 后停止。
 * @return 正常运行结束返回 OS_STATUS_OK，任一宿主操作失败返回 BAD_STATE。
 *
 * 本函数本身就是宿主控制线程：它等待“任务请求事件”和“周期定时器”两类对象，
 * 调用通用内核做调度决定，再通过暂停、恢复或 gate 执行该决定。
 */
os_status_t os_port_start_scheduler(uint32_t run_ticks)
{
    HANDLE wait_handles[2];

    if (!g_os_kernel.initialized || g_os_kernel.started ||
        (g_os_kernel.task_count == 0U)) {
        return OS_STATUS_BAD_STATE;
    }

    memset(&g_port, 0, sizeof(g_port));
    g_port.tls_index = TLS_OUT_OF_INDEXES;
    g_port.scheduler_priority_before = THREAD_PRIORITY_ERROR_RETURN;
    g_port.run_ticks = run_ticks;

    os_PortEnableVirtualTerminal();

    if (!os_PortCreateRuntimeObjects() || !os_PortStartTimer()) {
        InterlockedExchange(&g_port.failed, 1L);
        os_PortReleaseThreads();
        os_PortCloseRuntimeObjects();
        return OS_STATUS_BAD_STATE;
    }

    InterlockedExchange(&g_port.running, 1L);
    if ((os_SchedStart() == NULL) || !os_KernelValidate() ||
        !os_PortActivate(g_os_kernel.current)) {
        os_PortFail();
    }

    wait_handles[0] = g_port.request_event;
    wait_handles[1] = g_port.tick_timer;

    /* WaitForMultipleObjects 将任务 API 请求和 tick 串行交给同一控制循环。 */
    while (os_port_is_running()) {
        const DWORD wait_result =
            WaitForMultipleObjects(2U, wait_handles, FALSE, INFINITE);

        if (wait_result == WAIT_OBJECT_0) {
            if (!os_PortProcessRequest()) {
                os_PortFail();
            }
        } else if (wait_result == (WAIT_OBJECT_0 + 1U)) {
            if (!os_PortProcessTimer()) {
                os_PortFail();
            }
        } else {
            os_PortFail();
        }
    }

    os_PortReleaseThreads();
    os_PortCloseRuntimeObjects();
    os_KernelStopAll();

    return os_PortFailed() ? OS_STATUS_BAD_STATE : OS_STATUS_OK;
}
