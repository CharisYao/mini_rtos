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

static os_status_t g_last_error = OS_STATUS_OK;

void os_SetLastError(os_status_t status)
{
    g_last_error = status;
}

os_status_t osGetLastError(void)
{
    return g_last_error;
}

os_status_t os_Start(uint32_t run_ticks)
{
    os_status_t status = os_EnsureIdleTask();

    if (status != OS_STATUS_OK) {
        return status;
    }

    return os_port_start_scheduler(run_ticks);
}

void vTaskYield(void)
{
    (void)os_port_task_request(OS_PORT_REQ_YIELD, 0U, NULL, NULL);
}

void vTaskDelay(TickType_t xTicksToDelay)
{
    if (xTicksToDelay == 0U) {
        vTaskYield();
        return;
    }

    (void)os_port_task_request(OS_PORT_REQ_DELAY, xTicksToDelay, NULL, NULL);
}

bool os_IsRunning(void)
{
    return os_port_is_running();
}

void os_SetObserver(os_observer_t observer, void *context)
{
    os_port_set_observer(observer, context);
}

os_status_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
    return os_port_task_request(
        OS_PORT_REQ_SEM_TAKE,
        xTicksToWait,
        xSemaphore,
        NULL
    );
}

os_status_t xSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
    return os_port_task_request(OS_PORT_REQ_SEM_GIVE, 0U, xSemaphore, NULL);
}

os_status_t xSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore)
{
    os_task_t *woken = NULL;
    os_status_t status;

    os_EnterCritical();
    status = os_SemGiveFromIsrContext(xSemaphore, &woken);
    if (woken != NULL) {
        os_WaitMaybePreempt(woken);
    }
    os_ExitCritical();
    return status;
}

os_status_t xQueueSend(
    QueueHandle_t xQueue,
    const void *pvItemToQueue,
    TickType_t xTicksToWait
)
{
    return os_port_task_request(
        OS_PORT_REQ_QUEUE_SEND,
        xTicksToWait,
        xQueue,
        (void *)pvItemToQueue
    );
}

os_status_t xQueueReceive(
    QueueHandle_t xQueue,
    void *pvBuffer,
    TickType_t xTicksToWait
)
{
    return os_port_task_request(
        OS_PORT_REQ_QUEUE_RECEIVE,
        xTicksToWait,
        xQueue,
        pvBuffer
    );
}

os_status_t xMutexLock(MutexHandle_t xMutex, TickType_t xTicksToWait)
{
    return os_port_task_request(
        OS_PORT_REQ_MUTEX_LOCK,
        xTicksToWait,
        xMutex,
        NULL
    );
}

os_status_t xMutexUnlock(MutexHandle_t xMutex)
{
    return os_port_task_request(OS_PORT_REQ_MUTEX_UNLOCK, 0U, xMutex, NULL);
}
