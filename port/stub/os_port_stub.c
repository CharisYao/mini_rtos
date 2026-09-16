/**
 * @file os_port_stub.c
 * @brief Host stub port for white-box kernel tests (no real preemption).
 */

#include "os_port.h"
#include "os_kernel_state.h"
#include "os_sched.h"
#include "os_time.h"
#include "os_sem_internal.h"
#include "os_queue_internal.h"
#include "os_mutex_internal.h"

#include <stdint.h>
#include <string.h>

static uint32_t g_stub_critical_nesting;
static os_observer_t g_stub_observer;
static void *g_stub_observer_context;
static bool g_stub_switch_pending;

void os_port_enter_critical(void)
{
    g_stub_critical_nesting++;
}

void os_port_exit_critical(void)
{
    if (g_stub_critical_nesting > 0U) {
        g_stub_critical_nesting--;
    }
}

uint32_t os_port_critical_nesting(void)
{
    return g_stub_critical_nesting;
}

void os_port_pend_context_switch(void)
{
    g_stub_switch_pending = true;
}

void *os_port_stack_init(
    os_task_entry_t entry,
    void *argument,
    void *stack_memory,
    size_t stack_size
)
{
    uint8_t *top;

    (void)entry;
    (void)argument;

    if ((stack_memory == NULL) || (stack_size < OS_MIN_STACK_BYTES)) {
        return NULL;
    }

    /* Descending stack bookkeeping only; stub never switches stacks. */
    top = (uint8_t *)stack_memory + stack_size;
    return top;
}

os_status_t os_port_task_request(
    os_port_req_id_t request,
    uint32_t ticks,
    void *object,
    void *buffer
)
{
    os_task_t *current = g_os_kernel.current;

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

    os_port_exit_critical();
    g_stub_switch_pending = false;
    return (current->state == OS_TASK_RUNNING) ? current->wait_result
                                               : current->wait_result;
}

os_status_t os_port_start_scheduler(uint32_t run_ticks)
{
    (void)run_ticks;
    return OS_STATUS_BAD_STATE;
}

bool os_port_is_running(void)
{
    return false;
}

void os_port_set_observer(os_observer_t observer, void *context)
{
    g_stub_observer = observer;
    g_stub_observer_context = context;
}
