/**
 * @file os_api.c
 * @brief Platform-agnostic public task/IPC/startup API wrappers.
 *
 * @details
 * Scheduling and IPC semantics stay in the kernel modules. This file owns the
 * public entry points formerly implemented inside the Win32 port, and routes
 * blocking task-context work through the port HAL.
 */

#include "os.h"
#include "os_kernel_state.h"
#include "os_task_internal.h"
#include "os_sched.h"
#include "os_sem_internal.h"
#include "os_port.h"

os_status_t os_Start(uint32_t run_ticks)
{
    os_status_t status = os_EnsureIdleTask();

    if (status != OS_STATUS_OK) {
        return status;
    }

    return os_port_start_scheduler(run_ticks);
}

void os_Yield(void)
{
    (void)os_port_task_request(OS_PORT_REQ_YIELD, 0U, NULL, NULL);
}

void os_Delay(uint32_t ticks)
{
    if (ticks == 0U) {
        os_Yield();
        return;
    }

    (void)os_port_task_request(OS_PORT_REQ_DELAY, ticks, NULL, NULL);
}

bool os_IsRunning(void)
{
    return os_port_is_running();
}

void os_SetObserver(os_observer_t observer, void *context)
{
    os_port_set_observer(observer, context);
}

os_status_t os_SemTake(os_sem_t *sem, uint32_t timeout_ticks)
{
    return os_port_task_request(
        OS_PORT_REQ_SEM_TAKE,
        timeout_ticks,
        sem,
        NULL
    );
}

os_status_t os_SemGive(os_sem_t *sem)
{
    return os_port_task_request(OS_PORT_REQ_SEM_GIVE, 0U, sem, NULL);
}

os_status_t os_SemGiveFromISR(os_sem_t *sem)
{
    os_task_t *woken = NULL;
    os_status_t status;

    os_EnterCritical();
    status = os_SemGiveFromIsrContext(sem, &woken);
    if (woken != NULL) {
        os_WaitMaybePreempt(woken);
    }
    os_ExitCritical();
    return status;
}

os_status_t os_QueueSend(
    os_queue_t *queue,
    const void *item,
    uint32_t timeout_ticks
)
{
    return os_port_task_request(
        OS_PORT_REQ_QUEUE_SEND,
        timeout_ticks,
        queue,
        (void *)item
    );
}

os_status_t os_QueueReceive(
    os_queue_t *queue,
    void *out_item,
    uint32_t timeout_ticks
)
{
    return os_port_task_request(
        OS_PORT_REQ_QUEUE_RECEIVE,
        timeout_ticks,
        queue,
        out_item
    );
}

os_status_t os_MutexLock(os_mutex_t *mutex, uint32_t timeout_ticks)
{
    return os_port_task_request(
        OS_PORT_REQ_MUTEX_LOCK,
        timeout_ticks,
        mutex,
        NULL
    );
}

os_status_t os_MutexUnlock(os_mutex_t *mutex)
{
    return os_port_task_request(OS_PORT_REQ_MUTEX_UNLOCK, 0U, mutex, NULL);
}
