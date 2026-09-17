/**
 * @file os_port.h
 * @brief Port HAL: critical sections, context switch, stack frame, scheduler start.
 *
 * @details
 * Platform-agnostic kernel code (including os_api.c) calls only these hooks.
 * Win32 implements them with its control-thread + SuspendThread model; a future
 * Cortex-M port will map them to BASEPRI/PRIMASK, PendSV and exception frames.
 */

#ifndef MINI_RTOS_OS_PORT_H
#define MINI_RTOS_OS_PORT_H

#include "os.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Task-context operations forwarded to the port (Win32 serializes on the
 * control thread; MCU ports execute the matching kernel *Current helper). */
typedef enum {
    OS_PORT_REQ_YIELD = 1,
    OS_PORT_REQ_DELAY,
    OS_PORT_REQ_SEM_TAKE,
    OS_PORT_REQ_SEM_GIVE,
    OS_PORT_REQ_QUEUE_SEND,
    OS_PORT_REQ_QUEUE_RECEIVE,
    OS_PORT_REQ_MUTEX_LOCK,
    OS_PORT_REQ_MUTEX_UNLOCK,
    OS_PORT_REQ_EXIT
} os_port_req_id_t;

void os_port_enter_critical(void);
void os_port_exit_critical(void);
uint32_t os_port_critical_nesting(void);

/* Request a context switch; may be deferred while inside a critical section. */
void os_port_pend_context_switch(void);

/**
 * @brief Build the initial stack frame for a task and return the new SP.
 * @param entry Task entry.
 * @param argument Entry argument.
 * @param stack_memory Base of the caller-provided stack buffer.
 * @param stack_size Size in bytes; must be at least OS_MIN_STACK_BYTES.
 * @return Stack pointer to store in the TCB, or NULL on failure.
 */
void *os_port_stack_init(
    os_task_entry_t entry,
    void *argument,
    void *stack_memory,
    size_t stack_size
);

/**
 * @brief Run a blocking task-context kernel request and return its status.
 *
 * Win32 posts to the host control thread and waits on the task gate. Host stub
 * and future MCU ports execute the kernel helper directly under a critical
 * section and then yield if the caller is no longer RUNNING.
 */
os_status_t os_port_task_request(
    os_port_req_id_t request,
    uint32_t ticks,
    void *object,
    void *buffer
);

/* Start the first task / host scheduler loop. */
os_status_t os_port_start_scheduler(uint32_t run_ticks);

bool os_port_is_running(void);
void os_port_set_observer(os_observer_t observer, void *context);

#ifdef __cplusplus
}
#endif

#endif
