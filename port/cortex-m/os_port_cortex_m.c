/**
 * @file os_port_cortex_m.c
 * @brief Cortex-M3/M4 (no FPU) port HAL implementing kernel/include/os_port.h.
 *
 * @details
 * Critical sections use PRIMASK with nesting. Context switches are deferred via
 * PendSV (ICSR.PENDSVSET). SysTick drives os_TimeTick() at OS_TICK_MS. Initial
 * task frames are classic descending Cortex-M exception frames; PendSV saves
 * and restores R4-R11 on the process stack (PSP).
 */

#include "os_port.h"
#include "os_port_cortex_m.h"
#include "os_kernel_state.h"
#include "os_sched.h"
#include "os_time.h"
#include "os_sem_internal.h"
#include "os_queue_internal.h"
#include "os_mutex_internal.h"

#include <stddef.h>
#include <stdint.h>

/* ---- NVIC / SCB / SysTick (CMSIS-free register map) --------------------- */

#define OS_PORT_ICSR (*(volatile uint32_t *)0xE000ED04u)
#define OS_PORT_SHPR3 (*(volatile uint32_t *)0xE000ED20u)

#define OS_PORT_SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define OS_PORT_SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define OS_PORT_SYST_CVR (*(volatile uint32_t *)0xE000E018u)

#define OS_PORT_ICSR_PENDSVSET (1u << 28)
#define OS_PORT_SYST_CSR_ENABLE (1u << 0)
#define OS_PORT_SYST_CSR_TICKINT (1u << 1)
#define OS_PORT_SYST_CSR_CLKSOURCE (1u << 2)

#define OS_PORT_INITIAL_XPSR 0x01000000u

/* ---- Port state --------------------------------------------------------- */

static uint32_t g_port_critical_nesting;
static bool g_port_running;
static os_observer_t g_port_observer;
static void *g_port_observer_context;

/*
 * TCB whose context currently resides on PSP. Updated inside PendSV / start.
 * Kernel may update g_os_kernel.current before PendSV runs.
 */
os_task_t *volatile g_os_port_psp_owner;

void os_port_start_first_task(void);
void os_port_pendsv_save_psp(uint32_t psp_after_push);
uint32_t os_port_pendsv_restore_psp(void);
void os_port_enable_systick(void);

#if defined(__CC_ARM)
__weak uint32_t SystemCoreClock = OS_PORT_CORTEX_M_DEFAULT_CORE_CLOCK_HZ;
#else
__attribute__((weak)) uint32_t SystemCoreClock =
    OS_PORT_CORTEX_M_DEFAULT_CORE_CLOCK_HZ;
#endif

/* ---- Intrinsics --------------------------------------------------------- */

#if defined(__CC_ARM)
static __inline void os_port_disable_irq(void)
{
    __disable_irq();
}

static __inline void os_port_enable_irq(void)
{
    __enable_irq();
}

static __inline void os_port_dsb(void)
{
    __dsb(0xF);
}

static __inline void os_port_isb(void)
{
    __isb(0xF);
}

static __inline void os_port_wfi(void)
{
    __wfi();
}
#else
static inline void os_port_disable_irq(void)
{
    __asm volatile("cpsid i" ::: "memory");
}

static inline void os_port_enable_irq(void)
{
    __asm volatile("cpsie i" ::: "memory");
}

static inline void os_port_dsb(void)
{
    __asm volatile("dsb" ::: "memory");
}

static inline void os_port_isb(void)
{
    __asm volatile("isb" ::: "memory");
}

static inline void os_port_wfi(void)
{
    __asm volatile("wfi");
}
#endif

/* ---- Task exit trampoline (stacked LR) ---------------------------------- */

static void os_port_task_exit(void)
{
    (void)os_port_task_request(OS_PORT_REQ_EXIT, 0U, NULL, NULL);
    for (;;) {
        os_port_wfi();
    }
}

/* ---- Critical section --------------------------------------------------- */

void os_port_enter_critical(void)
{
    os_port_disable_irq();
    g_port_critical_nesting++;
}

void os_port_exit_critical(void)
{
    if (g_port_critical_nesting == 0U) {
        return;
    }

    g_port_critical_nesting--;
    if (g_port_critical_nesting == 0U) {
        os_port_enable_irq();
    }
}

uint32_t os_port_critical_nesting(void)
{
    return g_port_critical_nesting;
}

void os_port_pend_context_switch(void)
{
    OS_PORT_ICSR = OS_PORT_ICSR_PENDSVSET;
    os_port_dsb();
    os_port_isb();
}

/* ---- Stack frame -------------------------------------------------------- */

void *os_port_stack_init(
    os_task_entry_t entry,
    void *argument,
    void *stack_memory,
    size_t stack_size
)
{
    uint32_t *top;
    uintptr_t aligned;

    if ((entry == NULL) || (stack_memory == NULL) ||
        (stack_size < OS_MIN_STACK_BYTES)) {
        return NULL;
    }

    aligned = (uintptr_t)stack_memory + stack_size;
    aligned &= ~(uintptr_t)7u; /* 8-byte AAPCS alignment */
    top = (uint32_t *)aligned;

    /*
     * Exception stack frame (high -> low), then software-saved R4-R11.
     * PendSV restores R4-R11 then exception-returns into the HW frame.
     *
     *   [xPSR][PC][LR][R12][R3][R2][R1][R0] [R11..R4]
     *                                              ^ SP
     */
    *(--top) = OS_PORT_INITIAL_XPSR;
    *(--top) = ((uint32_t)entry) | 1u; /* Thumb bit */
    *(--top) = (uint32_t)os_port_task_exit;
    *(--top) = 0x0000000Cu; /* R12 */
    *(--top) = 0x00000003u; /* R3 */
    *(--top) = 0x00000002u; /* R2 */
    *(--top) = 0x00000001u; /* R1 */
    *(--top) = (uint32_t)argument; /* R0 */

    *(--top) = 0x0000000Bu; /* R11 */
    *(--top) = 0x0000000Au; /* R10 */
    *(--top) = 0x00000009u; /* R9 */
    *(--top) = 0x00000008u; /* R8 */
    *(--top) = 0x00000007u; /* R7 */
    *(--top) = 0x00000006u; /* R6 */
    *(--top) = 0x00000005u; /* R5 */
    *(--top) = 0x00000004u; /* R4 */

    return top;
}

/* ---- Task-context requests ---------------------------------------------- */

os_status_t os_port_task_request(
    os_port_req_id_t request,
    uint32_t ticks,
    void *object,
    void *buffer
)
{
    os_task_t *current = g_os_kernel.current;
    os_status_t result;

    if ((current == NULL) || (current->state != OS_TASK_RUNNING)) {
        return OS_STATUS_BAD_STATE;
    }

    os_port_enter_critical();

    switch (request) {
    case OS_PORT_REQ_YIELD:
        (void)os_SchedYieldCurrent();
        current->wait_result = OS_STATUS_OK;
        break;
    case OS_PORT_REQ_DELAY:
        (void)os_TimeDelayCurrent(ticks);
        break;
    case OS_PORT_REQ_SEM_TAKE:
        os_SemTakeCurrent((os_sem_t *)object, ticks);
        break;
    case OS_PORT_REQ_SEM_GIVE:
        os_SemGiveCurrent((os_sem_t *)object);
        break;
    case OS_PORT_REQ_QUEUE_SEND:
        os_QueueSendCurrent((os_queue_t *)object, buffer, ticks);
        break;
    case OS_PORT_REQ_QUEUE_RECEIVE:
        os_QueueReceiveCurrent((os_queue_t *)object, buffer, ticks);
        break;
    case OS_PORT_REQ_MUTEX_LOCK:
        os_MutexLockCurrent((os_mutex_t *)object, ticks);
        break;
    case OS_PORT_REQ_MUTEX_UNLOCK:
        os_MutexUnlockCurrent((os_mutex_t *)object);
        break;
    case OS_PORT_REQ_EXIT:
        (void)os_SchedTerminateCurrent();
        break;
    default:
        os_port_exit_critical();
        return OS_STATUS_INVALID_ARGUMENT;
    }

    result = current->wait_result;
    if (current->state != OS_TASK_RUNNING) {
        os_port_pend_context_switch();
    }

    os_port_exit_critical();
    return result;
}

/* ---- SysTick / observer ------------------------------------------------- */

static void os_port_maybe_observe(void)
{
    if (g_port_observer != NULL) {
        os_runtime_snapshot_t snapshot;
        os_GetRuntimeSnapshot(&snapshot);
        g_port_observer(&snapshot, g_port_observer_context);
    }
}

void SysTick_Handler(void)
{
    os_task_t *previous = g_os_kernel.current;
    os_task_t *next = os_TimeTick();

    if (next != previous) {
        os_port_pend_context_switch();
    }

    if ((xTaskGetTickCount() % OS_OBSERVER_PERIOD_TICKS) == 0U) {
        os_port_maybe_observe();
    }
}

/* ---- Scheduler start ---------------------------------------------------- */

static void os_port_prepare_systick(void)
{
    uint32_t ticks;
    uint32_t clock = SystemCoreClock;

    if (clock == 0U) {
        clock = OS_PORT_CORTEX_M_DEFAULT_CORE_CLOCK_HZ;
    }

    ticks = (clock / 1000u) * (uint32_t)OS_TICK_MS;
    if (ticks < 2u) {
        ticks = 2u;
    }

    OS_PORT_SYST_CSR = 0u;
    OS_PORT_SYST_RVR = ticks - 1u;
    OS_PORT_SYST_CVR = 0u;
}

void os_port_enable_systick(void)
{
    OS_PORT_SYST_CSR = OS_PORT_SYST_CSR_CLKSOURCE | OS_PORT_SYST_CSR_TICKINT |
                       OS_PORT_SYST_CSR_ENABLE;
}

static void os_port_configure_pendsv_priority(void)
{
    /* SHPR3 bits [23:16] = PendSV priority; lowest = 0xFF. */
    uint32_t shpr3 = OS_PORT_SHPR3;
    shpr3 &= ~(0xFFu << 16);
    shpr3 |= ((uint32_t)OS_PORT_CORTEX_M_PENDSV_PRIORITY << 16);
    OS_PORT_SHPR3 = shpr3;
}

os_status_t os_port_start_scheduler(uint32_t run_ticks)
{
    os_task_t *first;

    (void)run_ticks; /* Soft limit unused on bare metal; typically never returns. */

    if (!g_os_kernel.initialized || g_os_kernel.started) {
        return OS_STATUS_BAD_STATE;
    }

    first = os_SchedStart();
    if ((first == NULL) || (first->sp == NULL)) {
        return OS_STATUS_BAD_STATE;
    }

    g_os_port_psp_owner = first;
    g_port_running = true;

    os_port_disable_irq();
    os_port_configure_pendsv_priority();
    os_port_prepare_systick();

    /* Does not return: SVC starts the first task and enables SysTick. */
    os_port_start_first_task();

    g_port_running = false;
    return OS_STATUS_BAD_STATE;
}

bool os_port_is_running(void)
{
    return g_port_running;
}

void os_port_set_observer(os_observer_t observer, void *context)
{
    g_port_observer = observer;
    g_port_observer_context = context;
}

/* ---- PendSV helpers ----------------------------------------------------- */

void os_port_pendsv_save_psp(uint32_t psp_after_push)
{
    os_task_t *owner = g_os_port_psp_owner;
    if (owner != NULL) {
        owner->sp = (void *)(uintptr_t)psp_after_push;
    }
}

uint32_t os_port_pendsv_restore_psp(void)
{
    os_task_t *next = g_os_kernel.current;
    g_os_port_psp_owner = next;
    if ((next == NULL) || (next->sp == NULL)) {
        for (;;) {
            os_port_wfi();
        }
    }
    return (uint32_t)(uintptr_t)next->sp;
}
