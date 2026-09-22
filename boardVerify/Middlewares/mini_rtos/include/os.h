/**
 * @file os.h
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 声明应用层可使用的 miniRTOS 配置、数据类型和公开接口。
 *
 * @details
 * 本文件是任务代码和演示程序访问 miniRTOS 的统一入口，主要包含：
 * - 内核容量、tick 周期、时间片长度等编译期配置；
 * - 任务、信号量、消息队列和互斥量的不透明句柄；
 * - 任务状态、API 返回状态和任务切换原因等公共数据类型；
 * - 任务创建与运行、延时与主动让出、IPC、互斥量和运行快照接口。
 *
 * 应用层只需要包含本文件，不应直接访问 TCB、就绪队列或 Windows 线程对象。
 * 内核内部结构和跨模块接口分别位于 kernel/include 下的模块私有头文件中。
 */

#ifndef MINI_RTOS_OS_H
#define MINI_RTOS_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 内核采用静态资源配置，所有上限均在调度器启动前确定。 */
#define OS_MAX_TASKS 8U                 /**< 最大任务数量（含自动 Idle）。 */
#define OS_PRIORITY_COUNT 8U            /**< 优先级数量，取值范围为 0～7。 */
#define OS_IDLE_PRIORITY 0U             /**< Idle 任务优先级；用户任务不可使用。 */
#define OS_TASK_NAME_MAX 24U            /**< 任务名缓冲区大小，包含结尾 '\0'。 */
#define OS_TICK_MS 10U                  /**< 一个系统 tick 的标称时长，单位 ms。 */
#define OS_TIME_SLICE_TICKS 20U         /**< 同优先级任务的时间片长度。 */
#define OS_OBSERVER_PERIOD_TICKS 10U    /**< 控制台观察器的刷新周期。 */
#define OS_MAX_DELAY_TICKS 0x7FFFFFFFU  /**< 可安全进行回绕比较的最大有限等待。 */
#define OS_WAIT_FOREVER UINT32_MAX      /**< 永久等待，不设置超时截止 tick。 */
#define OS_MAX_SEMAPHORES 4U            /**< 静态信号量控制块数量。 */
#define OS_MAX_QUEUES 4U                /**< 静态消息队列控制块数量。 */
#define OS_MAX_MUTEXES 4U               /**< 静态互斥量控制块数量。 */
#define OS_MIN_STACK_BYTES 64U          /**< 任务栈缓冲区最小字节数。 */
#define OS_IDLE_STACK_BYTES 256U        /**< 自动 Idle 任务静态栈大小。 */

/* 对应用隐藏内部字段的控制块。句柄是指向它们的指针。 */
typedef struct os_task os_task_t;
typedef struct os_sem os_sem_t;
typedef struct os_queue os_queue_t;
typedef struct os_mutex os_mutex_t;

/* FreeRTOS 风格句柄。互斥量单独用 MutexHandle_t，因为它不是信号量。 */
typedef os_task_t *TaskHandle_t;
typedef os_sem_t *SemaphoreHandle_t;
typedef os_queue_t *QueueHandle_t;
typedef os_mutex_t *MutexHandle_t;
typedef uint32_t TickType_t;

/**
 * @brief 任务入口函数类型。
 * @param argument 创建任务时传入的用户参数。
 */
typedef void (*TaskFunction_t)(void *argument);
typedef TaskFunction_t os_task_entry_t;

/* 所有公开 API 共用的返回状态 */
typedef enum {
    OS_STATUS_OK = 0,             /* 操作成功。 */
    OS_STATUS_INVALID_ARGUMENT,   /* 参数为空、越界或组合无效。 */
    OS_STATUS_LIMIT_REACHED,      /* 静态对象池已满或计数达到上限。 */
    OS_STATUS_BAD_STATE,          /* 当前内核或任务状态不允许该操作。 */
    OS_STATUS_TIMEOUT,            /* 在指定 tick 数内未获得资源。 */
    OS_STATUS_NOT_OWNER           /* 非互斥量所有者尝试解锁。 */
} os_status_t;

/* TCB 中记录的任务生命周期状态 */
typedef enum {
    OS_TASK_UNUSED = 0,       /* TCB 槽位尚未分配。 */
    OS_TASK_READY,            /* 已具备运行条件，正在就绪队列中等待。 */
    OS_TASK_RUNNING,          /* 当前获得 CPU 的唯一任务。 */
    OS_TASK_BLOCKED_DELAY,    /* 等待延时到期。 */
    OS_TASK_BLOCKED_OBJECT,   /* 等待信号量、队列或互斥量。 */
    OS_TASK_TERMINATED        /* 任务入口已经返回，不再参与调度。 */
} os_task_state_t;

/* 最近一次任务切换的触发原因 */
typedef enum {
    OS_SWITCH_NONE = 0,                 /* 尚未发生任务切换。 */
    OS_SWITCH_START,                    /* 调度器启动并选择首个任务。 */
    OS_SWITCH_TIME_SLICE,               /* 同优先级任务时间片耗尽。 */
    OS_SWITCH_YIELD,                    /* 当前任务主动让出 CPU。 */
    OS_SWITCH_HIGHER_PRIORITY_WAKEUP,   /* 更高优先级任务变为 READY。 */
    OS_SWITCH_BLOCKED,                  /* 当前任务因延时或资源进入阻塞。 */
    OS_SWITCH_EXIT                      /* 当前任务入口返回并终止。 */
} os_switch_reason_t;

/* 观察器读取的只读调度快照 */
typedef struct {
    uint32_t tick;                          /* 当前系统 tick。 */
    const os_task_t *current;               /* 当前 RUNNING 任务，空闲时为 NULL。 */
    const os_task_t *last_from;             /* 最近一次切换离开的任务。 */
    const os_task_t *last_to;               /* 最近一次切换进入的任务。 */
    os_switch_reason_t last_reason;         /* 最近一次切换原因。 */
    uint32_t switch_count;                  /* 实际发生的任务切换累计次数。 */
    uint32_t current_slice_remaining;       /* 当前任务剩余时间片。 */
} os_runtime_snapshot_t;

/**
 * @brief 调度器周期调用的只读观察回调。
 * @param snapshot 当前调度快照，只在本次回调期间有效。
 * @param context 注册观察器时提供的用户上下文。
 */
typedef void (*os_observer_t)(const os_runtime_snapshot_t *snapshot, void *context);

/* 初始化全部静态内核对象，创建任务前必须调用。 */
void os_Init(void);

/**
 * @brief 从静态 TCB 池创建任务，成功返回句柄，失败返回 NULL。
 *
 * 调用方提供静态栈，单位是字节，不是 FreeRTOS 的字。失败原因见 osGetLastError()。
 */
TaskHandle_t xTaskCreate(
    TaskFunction_t pxTaskCode,
    const char *pcName,
    uint32_t ulStackBytes,
    void *pvParameters,
    uint8_t uxPriority,
    void *puxStackBuffer
);

/* 最近一次公开创建/操作 API 的状态。句柄 API 失败时用来区分原因。 */
os_status_t osGetLastError(void);

/* 返回任务名称；xTask 为 NULL 时返回 NULL。 */
const char *pcTaskGetName(const os_task_t *xTask);
/* 返回任务当前状态；xTask 为 NULL 时返回 OS_TASK_UNUSED。 */
os_task_state_t eTaskGetState(const os_task_t *xTask);
/* 返回任务有效优先级；xTask 为 NULL 时返回 OS_IDLE_PRIORITY。 */
uint8_t uxTaskPriorityGet(const os_task_t *xTask);
/* 返回便于显示的任务状态名称。 */
const char *os_TaskStateName(os_task_state_t state);
/* 返回便于显示的任务切换原因名称。 */
const char *os_SwitchReasonName(os_switch_reason_t reason);

/**
 * @brief 确保自动 Idle 任务存在，然后启动移植层调度器。
 * @param run_ticks 为 0 时运行到仅剩 Idle；非 0 时到达该 tick 后停止。
 * @return OS_STATUS_OK 表示正常结束，宿主对象或运行不变量失败时返回错误。
 */
os_status_t os_Start(uint32_t run_ticks);
/* 当前任务保持 READY 并主动让出一次 CPU。 */
void vTaskYield(void);
#define taskYIELD() vTaskYield()
/* 当前任务阻塞指定 tick；0 等价于 taskYIELD()。 */
void vTaskDelay(TickType_t xTicksToDelay);
/* 返回当前系统 tick。 */
TickType_t xTaskGetTickCount(void);
/* 返回调度器是否仍在运行。 */
bool os_IsRunning(void);

/**
 * @brief 注册动态面板使用的只读观察器。
 * @param observer 周期回调；传入 NULL 可关闭观察。
 * @param context 原样传给观察回调的用户上下文。
 */
void os_SetObserver(os_observer_t observer, void *context);
/* 将当前调度状态复制到 out_snapshot；空指针不会执行操作。 */
void os_GetRuntimeSnapshot(os_runtime_snapshot_t *out_snapshot);

/**
 * @brief 从静态池创建计数信号量。参数顺序与 FreeRTOS 相同：先最大计数，再初始计数。
 * @return 成功返回句柄，失败返回 NULL。原因见 osGetLastError()。
 */
SemaphoreHandle_t xSemaphoreCreateCounting(
    uint32_t uxMaxCount,
    uint32_t uxInitialCount
);
/**
 * @brief 获取一次信号量，计数为 0 时阻塞当前任务。
 * @param xTicksToWait 0 表示不等待，OS_WAIT_FOREVER 表示永久等待。
 */
os_status_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);
/* 释放一次信号量，并优先唤醒最高优先级等待者。 */
os_status_t xSemaphoreGive(SemaphoreHandle_t xSemaphore);
/* 在中断上下文释放信号量；不阻塞。 */
os_status_t xSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore);
/* 返回当前计数；无效句柄返回 0。 */
uint32_t uxSemaphoreGetCount(SemaphoreHandle_t xSemaphore);

/**
 * @brief 用调用方缓冲区创建队列。uxQueueLength 是消息条数，uxItemSize 是每条字节数。
 * @return 成功返回句柄，失败返回 NULL。
 */
QueueHandle_t xQueueCreate(
    uint32_t uxQueueLength,
    uint32_t uxItemSize,
    void *pucQueueStorage
);
os_status_t xQueueSend(
    QueueHandle_t xQueue,
    const void *pvItemToQueue,
    TickType_t xTicksToWait
);
os_status_t xQueueReceive(
    QueueHandle_t xQueue,
    void *pvBuffer,
    TickType_t xTicksToWait
);
/* 返回队列里当前的消息条数。 */
size_t uxQueueMessagesWaiting(QueueHandle_t xQueue);
/* 返回队列容量（条数）。 */
size_t uxQueueCapacity(QueueHandle_t xQueue);
/**
 * @brief 按出队顺序复制队列内容，但不改变读写位置。
 * @param queue 目标消息队列。
 * @param out_items 接收连续消息副本的缓冲区。
 * @param maximum_items 最多复制的消息数量。
 * @return 实际复制的消息数量。
 */
size_t os_QueueSnapshot(
    const os_queue_t *queue,
    void *out_items,
    size_t maximum_items
);

/* 从静态池创建一个非递归互斥量。失败返回 NULL。 */
MutexHandle_t xMutexCreate(void);
/**
 * @brief 获取互斥量。被占用时阻塞，并做优先级继承。
 * @return OS_STATUS_OK、OS_STATUS_TIMEOUT 或参数/状态错误。
 */
os_status_t xMutexLock(MutexHandle_t xMutex, TickType_t xTicksToWait);
/* 由所有者释放互斥量；非所有者返回 OS_STATUS_NOT_OWNER。 */
os_status_t xMutexUnlock(MutexHandle_t xMutex);

#ifdef __cplusplus
}
#endif

#endif
