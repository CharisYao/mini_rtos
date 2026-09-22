/**
 * @file ipc_demo.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 通过温度生产、处理和报警任务展示消息队列、信号量及阻塞唤醒。
 *
 * @details
 * SensorTask 以 P2 周期产生温度并发送到容量为 6 的消息队列；ProcessTask 以 P1
 * 接收并进行较慢处理。生产速度高于消费速度时，演示会先后出现消费者等待空队列
 * 和生产者等待满队列，直观展示阻塞任务不再参与调度以及资源可用后的自动唤醒。
 *
 * ProcessTask 检测到高温后释放报警信号量，等待该信号量的 P3 AlarmTask 随即恢复
 * 并抢占普通任务。宿主观察器显示队列内容、任务状态、最近收发温度和报警进度，
 * 最终还会自动检查空等待、满等待和高优先级报警三个现象是否实际发生。
 */

#include "os.h"

#define DEMO_STACK_BYTES 4096U

static uint8_t g_demo_stacks[OS_MAX_TASKS][DEMO_STACK_BYTES];
static size_t g_demo_stack_cursor;

static void *demoAllocStack(void)
{
    void *stack = g_demo_stacks[g_demo_stack_cursor];
    g_demo_stack_cursor++;
    return stack;
}

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SAMPLE_QUEUE_CAPACITY 6U
#define ALARM_TEMPERATURE 65

/* 温度生产、处理和报警流水线的共享演示上下文 */
typedef struct {
    TaskHandle_t sensorTask;        /* P2 温度生产者。 */
    TaskHandle_t processTask;       /* P1 慢速消费者。 */
    TaskHandle_t alarmTask;         /* P3 信号量等待者。 */
    QueueHandle_t sample_queue;     /* 容量为 6 的温度消息队列。 */
    SemaphoreHandle_t alarm_sem;          /* 高温事件计数信号量。 */
    bool compact;                 /* 是否只输出关键状态变化。 */
    bool saw_empty_wait;          /* 是否观察到消费者等待空队列。 */
    bool saw_full_wait;           /* 是否观察到生产者等待满队列。 */
    bool saw_alarm;               /* 是否观察到高优先级报警运行。 */
    uint32_t last_compact_switch; /* 紧凑模式已显示的切换计数。 */
    size_t last_compact_count;    /* 紧凑模式上次显示的队列占用。 */
} demo_context_t;

/* 工作任务通过原子值向宿主观察器发布进度，不直接执行控制台输出。 */
static atomic_uint g_samples_sent;
static atomic_uint g_samples_processed;
static atomic_uint g_alarm_runs;
static atomic_int g_last_sent;
static atomic_int g_last_processed;
static atomic_bool g_alarm_active;

/**
 * @brief 以持续整数运算占用指定数量的 RTOS tick。
 * @param duration_ticks 期望持续的 tick 数。
 */
static void workForTicks(uint32_t duration_ticks)
{
    const uint32_t start_tick = xTaskGetTickCount();
    volatile uint32_t work = 0x13579BDFU;

    while (os_IsRunning() && ((xTaskGetTickCount() - start_tick) < duration_ticks)) {
        work = (work * 1664525U) + 1013904223U;
    }
}

/**
 * @brief 周期产生温度并发送到有界队列的 P2 任务。
 * @param argument 指向 demo_context_t。
 *
 * 生产速度高于处理速度，最终会因队列满而进入 BLOCKED_OBJECT。
 */
static void sensorTask(void *argument)
{
    static const int temperatures[] = {24, 28, 31, 72, 35, 39, 68, 41, 29, 75};
    demo_context_t *demo = (demo_context_t *)argument;
    size_t sample_index = 0U;

    /* 先延时，让消费者有机会演示空队列接收阻塞。 */
    vTaskDelay(15U);

    while (os_IsRunning()) {
        const int temperature =
            temperatures[sample_index % (sizeof(temperatures) / sizeof(temperatures[0]))];
        const os_status_t status =
            xQueueSend(demo->sample_queue, &temperature, OS_WAIT_FOREVER);

        if (!os_IsRunning()) {
            break;
        }
        if (status != OS_STATUS_OK) {
            break;
        }

        atomic_store_explicit(&g_last_sent, temperature, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_samples_sent, 1U, memory_order_relaxed);
        sample_index++;
        vTaskDelay(12U);
    }
}

/**
 * @brief 接收并慢速处理温度，高温时释放报警信号量的 P1 任务。
 * @param argument 指向 demo_context_t。
 */
static void processTask(void *argument)
{
    demo_context_t *demo = (demo_context_t *)argument;

    while (os_IsRunning()) {
        int temperature = 0;
        const os_status_t status =
            xQueueReceive(demo->sample_queue, &temperature, OS_WAIT_FOREVER);

        if (!os_IsRunning()) {
            break;
        }
        if (status != OS_STATUS_OK) {
            break;
        }

        workForTicks(30U);
        if (!os_IsRunning()) {
            break;
        }

        atomic_store_explicit(&g_last_processed, temperature, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_samples_processed, 1U, memory_order_relaxed);

        if (temperature >= ALARM_TEMPERATURE) {
            (void)xSemaphoreGive(demo->alarm_sem);
        }
    }
}

/**
 * @brief 等待报警信号量并以 P3 执行高温处理的任务。
 * @param argument 指向 demo_context_t。
 */
static void alarmTask(void *argument)
{
    demo_context_t *demo = (demo_context_t *)argument;

    while (os_IsRunning()) {
        const os_status_t status = xSemaphoreTake(demo->alarm_sem, OS_WAIT_FOREVER);

        if (!os_IsRunning()) {
            break;
        }
        if (status != OS_STATUS_OK) {
            break;
        }

        atomic_store_explicit(&g_alarm_active, true, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_alarm_runs, 1U, memory_order_relaxed);
        workForTicks(22U);
        atomic_store_explicit(&g_alarm_active, false, memory_order_relaxed);
    }
}

/* 将没有 RUNNING 任务的快照显示为 IDLE。 */
static const char *taskNameOrIdle(const os_task_t *task)
{
    return (task != NULL) ? pcTaskGetName(task) : "IDLE";
}

/**
 * @brief 从任务状态和队列占用中记录三个肉眼验收条件。
 * @param demo 演示上下文。
 * @param queue_count 当前队列消息数量。
 */
static void observeBehaviors(demo_context_t *demo, size_t queue_count)
{
    if ((queue_count == 0U) &&
        (eTaskGetState(demo->processTask) == OS_TASK_BLOCKED_OBJECT)) {
        demo->saw_empty_wait = true;
    }
    if ((queue_count == SAMPLE_QUEUE_CAPACITY) &&
        (eTaskGetState(demo->sensorTask) == OS_TASK_BLOCKED_OBJECT)) {
        demo->saw_full_wait = true;
    }
    if (atomic_load_explicit(&g_alarm_active, memory_order_relaxed)) {
        demo->saw_alarm = true;
    }
}

/**
 * @brief 将队列快照渲染为六个固定 ASCII 槽位。
 * @param items 按出队顺序排列的温度副本。
 * @param count 有效温度数量。
 */
static void renderQueue(const int *items, size_t count)
{
    for (size_t index = 0U; index < SAMPLE_QUEUE_CAPACITY; index++) {
        if (index < count) {
            printf("[%2d]", items[index]);
        } else {
            printf("[--]");
        }
    }
}

/**
 * @brief 在宿主控制线程中显示任务状态、队列内容和报警因果关系。
 * @param snapshot 当前内核运行快照。
 * @param context 指向 demo_context_t。
 */
static void renderDashboard(const os_runtime_snapshot_t *snapshot, void *context)
{
    demo_context_t *demo = (demo_context_t *)context;
    int items[SAMPLE_QUEUE_CAPACITY] = {0};
    const size_t count = os_QueueSnapshot(
        demo->sample_queue,
        items,
        SAMPLE_QUEUE_CAPACITY
    );

    observeBehaviors(demo, count);

    if (demo->compact) {
        if ((snapshot->switch_count != demo->last_compact_switch) ||
            (count != demo->last_compact_count)) {
            printf(
                "tick=%4u  %-11s -> %-11s  queue=%u/%u  reason=%s\n",
                snapshot->tick,
                taskNameOrIdle(snapshot->last_from),
                taskNameOrIdle(snapshot->last_to),
                (unsigned)count,
                (unsigned)SAMPLE_QUEUE_CAPACITY,
                os_SwitchReasonName(snapshot->last_reason)
            );
            demo->last_compact_switch = snapshot->switch_count;
            demo->last_compact_count = count;
        }
        return;
    }

    printf("\x1b[2J\x1b[H");
    printf("================ miniRTOS IPC PIPELINE ================\n");
    printf("Tick/Current : %-5u %s (P%u)\n", snapshot->tick,
           taskNameOrIdle(snapshot->current),
           uxTaskPriorityGet(snapshot->current));
    printf("Last switch  : %s -> %s (%s)\n\n",
           taskNameOrIdle(snapshot->last_from),
           taskNameOrIdle(snapshot->last_to),
           os_SwitchReasonName(snapshot->last_reason));

    printf("Sensor P2    : %-14s sent=%-3u last=%d C\n",
           os_TaskStateName(eTaskGetState(demo->sensorTask)),
           atomic_load_explicit(&g_samples_sent, memory_order_relaxed),
           atomic_load_explicit(&g_last_sent, memory_order_relaxed));
    printf("                | produces every 12 ticks\n");
    printf("                v\n");
    printf("Queue         : ");
    renderQueue(items, count);
    printf("  %u/%u\n", (unsigned)count, (unsigned)SAMPLE_QUEUE_CAPACITY);
    printf("                | bounded buffer\n");
    printf("                v\n");
    printf("Processor P1 : %-14s done=%-3u last=%d C\n\n",
           os_TaskStateName(eTaskGetState(demo->processTask)),
           atomic_load_explicit(&g_samples_processed, memory_order_relaxed),
           atomic_load_explicit(&g_last_processed, memory_order_relaxed));

    printf("Alarm P3     : %-14s runs=%-3u %s\n",
           os_TaskStateName(eTaskGetState(demo->alarmTask)),
           atomic_load_explicit(&g_alarm_runs, memory_order_relaxed),
           atomic_load_explicit(&g_alarm_active, memory_order_relaxed)
               ? "!!! HANDLING HIGH TEMPERATURE !!!"
               : "waiting for semaphore");
    printf("\nObserved     : empty-wait=%s  full-wait=%s  alarm-preempt=%s\n",
           demo->saw_empty_wait ? "YES" : "no",
           demo->saw_full_wait ? "YES" : "no",
           demo->saw_alarm ? "YES" : "no");
    printf("========================================================\n");
    fflush(stdout);
}

/**
 * @brief 初始化 IPC 对象和三个任务，运行并自动检查演示现象。
 * @param argc 命令行参数数量。
 * @param argv 可选的 --quick 模式。
 * @return 空等待、满等待和报警抢占均出现时返回 0。
 */
int main(int argc, char **argv)
{
    demo_context_t demo = {0};
    int queue_storage[SAMPLE_QUEUE_CAPACITY] = {0};
    os_runtime_snapshot_t final_snapshot;
    uint32_t run_ticks = 900U;
    os_status_t status;

    atomic_store_explicit(&g_last_sent, -1, memory_order_relaxed);
    atomic_store_explicit(&g_last_processed, -1, memory_order_relaxed);

    if ((argc > 1) && (strcmp(argv[1], "--quick") == 0)) {
        demo.compact = true;
        run_ticks = 420U;
    }

    os_Init();
    if (((((demo.sample_queue = xQueueCreate(SAMPLE_QUEUE_CAPACITY, sizeof(queue_storage[0]), queue_storage)) != NULL) ? OS_STATUS_OK : osGetLastError()) != OS_STATUS_OK) ||
        ((((demo.alarm_sem = xSemaphoreCreateCounting(1U, 0U)) != NULL) ? OS_STATUS_OK : osGetLastError()) != OS_STATUS_OK) ||
        ((((demo.sensorTask = xTaskCreate(sensorTask, "SensorTask", DEMO_STACK_BYTES, &demo, 2U, demoAllocStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) != OS_STATUS_OK) ||
        ((((demo.processTask = xTaskCreate(processTask, "ProcessTask", DEMO_STACK_BYTES, &demo, 1U, demoAllocStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) != OS_STATUS_OK) ||
        ((((demo.alarmTask = xTaskCreate(alarmTask, "AlarmTask", DEMO_STACK_BYTES, &demo, 3U, demoAllocStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) != OS_STATUS_OK)) {
        fprintf(stderr, "failed to initialize IPC demo\n");
        return 1;
    }

    os_SetObserver(renderDashboard, &demo);
    status = os_Start(run_ticks);
    os_GetRuntimeSnapshot(&final_snapshot);

    printf(
        "\nsummary: sent=%u processed=%u alarms=%u switches=%u "
        "empty_wait=%s full_wait=%s\n",
        atomic_load_explicit(&g_samples_sent, memory_order_relaxed),
        atomic_load_explicit(&g_samples_processed, memory_order_relaxed),
        atomic_load_explicit(&g_alarm_runs, memory_order_relaxed),
        final_snapshot.switch_count,
        demo.saw_empty_wait ? "yes" : "no",
        demo.saw_full_wait ? "yes" : "no"
    );

    if ((status != OS_STATUS_OK) ||
        (atomic_load_explicit(&g_samples_sent, memory_order_relaxed) == 0U) ||
        (atomic_load_explicit(&g_samples_processed, memory_order_relaxed) == 0U) ||
        (atomic_load_explicit(&g_alarm_runs, memory_order_relaxed) == 0U) ||
        !demo.saw_empty_wait || !demo.saw_full_wait || !demo.saw_alarm) {
        fprintf(stderr, "IPC demo verification failed\n");
        return 1;
    }

    printf("IPC demo verification passed\n");
    return 0;
}
