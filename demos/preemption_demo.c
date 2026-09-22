/**
 * @file preemption_demo.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 通过动态控制台面板展示同优先级时间片轮转和高优先级任务抢占。
 *
 * @details
 * CarTask 和 ComputeTask 都是 P1 CPU 密集任务，它们在循环中故意不调用 vTaskYield()
 * 或 vTaskDelay()。两者仍能交替推进，说明周期 tick 可以强制结束当前时间片并让同级
 * 任务轮转。EmergencyTask 为 P3，平时通过延时进入阻塞态，到期后立即抢占 P1 任务，
 * 完成紧急处理后再次延时。
 *
 * 工作任务只更新原子观察值，不直接调用 printf；宿主观察器读取运行快照并绘制小车、
 * 计算进度、紧急任务状态、当前任务和切换原因。普通、quick 和 stress 模式分别用于
 * 肉眼观察、自动测试和较长时间抢占稳定性验证。
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

#define TRACK_WIDTH 30U
#define PROGRESS_WIDTH 30U

/* 抢占演示使用的任务句柄和紧凑输出状态 */
typedef struct {
    TaskHandle_t carTask;           /* 文本小车任务。 */
    TaskHandle_t computeTask;       /* 持续计算任务。 */
    TaskHandle_t emergencyTask;     /* 周期唤醒的高优先级任务。 */
    bool compact;                 /* 是否只打印切换轨迹。 */
    uint32_t last_compact_switch; /* 紧凑模式已显示的切换计数。 */
} demo_context_t;

/* 任务只更新原子观察值，控制台输出统一留给宿主观察器。 */
static atomic_uint g_car_position;
static atomic_uint g_car_steps;
static atomic_uint g_compute_progress;
static atomic_uint g_compute_steps;
static atomic_uint g_emergency_active;
static atomic_uint g_emergency_progress;
static atomic_uint g_emergency_runs;

/**
 * @brief 持续推进文本小车位置的 P1 CPU 密集任务。
 * @param argument 本演示不使用任务参数。
 *
 * 任务故意不调用 vTaskYield() 或 vTaskDelay()，用于证明时间片能够异步抢占。
 */
static void carTask(void *argument)
{
    uint32_t position = 0U;
    volatile uint32_t work = 1U;

    (void)argument;

    while (os_IsRunning()) {
        /* 固定整数运算延长单轮工作，使暂停和恢复效果更容易观察。 */
        for (uint32_t index = 0U; index < 500000U; index++) {
            work = (work * 1664525U) + 1013904223U;
        }

        position = (position + 1U) % TRACK_WIDTH;
        atomic_store_explicit(&g_car_position, position, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_car_steps, 1U, memory_order_relaxed);
    }
}

/**
 * @brief 持续计算校验值并更新循环进度条的 P1 任务。
 * @param argument 本演示不使用任务参数。
 *
 * 与 carTask 同优先级且从不主动让出，用来展示同级时间片轮转。
 */
static void computeTask(void *argument)
{
    uint32_t progress = 0U;
    volatile uint32_t checksum = 0x12345678U;

    (void)argument;

    while (os_IsRunning()) {
        for (uint32_t index = 1U; index <= 350000U; index++) {
            checksum ^= (index * 2654435761U);
            checksum = (checksum << 5U) | (checksum >> 27U);
        }

        progress = (progress + 3U) % 101U;
        atomic_store_explicit(&g_compute_progress, progress, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_compute_steps, 1U, memory_order_relaxed);
    }
}

/**
 * @brief 周期延时后以 P3 抢占普通任务的紧急处理任务。
 * @param argument 本演示不使用任务参数。
 */
static void emergencyTask(void *argument)
{
    volatile uint32_t work = 0U;

    (void)argument;

    while (os_IsRunning()) {
        uint32_t start_tick;

        /* 延时期间处于 BLOCKED_DELAY，不参与 CPU 竞争。 */
        vTaskDelay(250U);
        if (!os_IsRunning()) {
            break;
        }

        atomic_store_explicit(&g_emergency_active, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_emergency_runs, 1U, memory_order_relaxed);
        start_tick = xTaskGetTickCount();

        /* 持续运行 60 tick，面板可观察低优先级任务在此期间停止推进。 */
        while (os_IsRunning()) {
            const uint32_t elapsed = xTaskGetTickCount() - start_tick;

            if (elapsed >= 60U) {
                break;
            }

            atomic_store_explicit(
                &g_emergency_progress,
                (elapsed * 100U) / 60U,
                memory_order_relaxed
            );
            work = (work * 1103515245U) + 12345U;
        }

        atomic_store_explicit(&g_emergency_progress, 100U, memory_order_relaxed);
        atomic_store_explicit(&g_emergency_active, 0U, memory_order_relaxed);
        atomic_store_explicit(&g_emergency_progress, 0U, memory_order_relaxed);
    }
}

/* 将空闲状态的 NULL current 显示为 IDLE。 */
static const char *taskNameOrIdle(const os_task_t *task)
{
    return (task != NULL) ? pcTaskGetName(task) : "IDLE";
}

/**
 * @brief 将百分比转换为固定宽度 ASCII 进度条。
 * @param bar 输出字符数组，容量至少为 width + 1。
 * @param width 进度条字符宽度。
 * @param value 0～100 的完成比例。
 * @param marker 已完成区间使用的字符。
 */
static void fillBar(char *bar, uint32_t width, uint32_t value, char marker)
{
    uint32_t filled = (value * width) / 100U;

    for (uint32_t index = 0U; index < width; index++) {
        bar[index] = (index < filled) ? marker : '-';
    }
    bar[width] = '\0';
}

/**
 * @brief 在宿主控制线程中渲染只读调度快照和任务观察值。
 * @param snapshot 当前内核运行快照。
 * @param context 指向 demo_context_t。
 *
 * 本回调不是 miniRTOS 任务，因此 printf 不会在被强制暂停的任务线程中持有运行库锁。
 */
static void renderDashboard(const os_runtime_snapshot_t *snapshot, void *context)
{
    demo_context_t *demo = (demo_context_t *)context;
    char track[TRACK_WIDTH + 1U];
    char compute_bar[PROGRESS_WIDTH + 1U];
    char emergency_bar[PROGRESS_WIDTH + 1U];
    uint32_t car_position;
    uint32_t compute_progress;
    uint32_t emergency_progress;

    if (demo->compact) {
        if (snapshot->switch_count != demo->last_compact_switch) {
            printf(
                "tick=%4u  %-13s -> %-13s  reason=%s\n",
                snapshot->tick,
                taskNameOrIdle(snapshot->last_from),
                taskNameOrIdle(snapshot->last_to),
                os_SwitchReasonName(snapshot->last_reason)
            );
            demo->last_compact_switch = snapshot->switch_count;
        }
        return;
    }

    car_position = atomic_load_explicit(&g_car_position, memory_order_relaxed);
    compute_progress =
        atomic_load_explicit(&g_compute_progress, memory_order_relaxed);
    emergency_progress =
        atomic_load_explicit(&g_emergency_progress, memory_order_relaxed);

    memset(track, '-', TRACK_WIDTH);
    track[car_position % TRACK_WIDTH] = '>';
    track[TRACK_WIDTH] = '\0';
    fillBar(compute_bar, PROGRESS_WIDTH, compute_progress, '#');
    fillBar(emergency_bar, PROGRESS_WIDTH, emergency_progress, '!');

    printf("\x1b[2J\x1b[H");
    printf("================ miniRTOS PREEMPTION ================\n");
    printf("Tick        : %u\n", snapshot->tick);
    printf(
        "Current     : %s (P%u)\n",
        taskNameOrIdle(snapshot->current),
        uxTaskPriorityGet(snapshot->current)
    );
    printf(
        "Last switch : %s -> %s\n",
        taskNameOrIdle(snapshot->last_from),
        taskNameOrIdle(snapshot->last_to)
    );
    printf("Reason      : %s\n\n", os_SwitchReasonName(snapshot->last_reason));
    printf(
        "Slice left  : %u / %u ticks\n\n",
        snapshot->current_slice_remaining,
        OS_TIME_SLICE_TICKS
    );

    printf(
        "[P1][%-13s] CarTask\nTrack       : |%s|\n\n",
        os_TaskStateName(eTaskGetState(demo->carTask)),
        track
    );
    printf(
        "[P1][%-13s] ComputeTask\nProgress    : [%s] %3u%%\n\n",
        os_TaskStateName(eTaskGetState(demo->computeTask)),
        compute_bar,
        compute_progress
    );
    printf(
        "[P3][%-13s] EmergencyTask\nEmergency   : [%s] %s\n",
        os_TaskStateName(eTaskGetState(demo->emergencyTask)),
        emergency_bar,
        atomic_load_explicit(&g_emergency_active, memory_order_relaxed)
            ? "HANDLING"
            : "WAITING"
    );
    printf("======================================================\n");
    fflush(stdout);
}

/**
 * @brief 创建三个演示任务并按普通、快速或压力模式运行调度器。
 * @param argc 命令行参数数量。
 * @param argv 可选的 --quick 或 --stress 模式。
 * @return 演示行为满足验收条件时返回 0。
 */
int main(int argc, char **argv)
{
    demo_context_t demo = {0};
    os_runtime_snapshot_t final_snapshot;
    uint32_t run_ticks = 900U;
    os_status_t status;

    if ((argc > 1) && (strcmp(argv[1], "--quick") == 0)) {
        demo.compact = true;
        run_ticks = 420U;
    } else if ((argc > 1) && (strcmp(argv[1], "--stress") == 0)) {
        demo.compact = true;
        run_ticks = 1200U;
    }

    os_Init();
    if ((((demo.carTask = xTaskCreate(carTask, "CarTask", DEMO_STACK_BYTES, NULL, 1U, demoAllocStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) !=
            OS_STATUS_OK ||
        (((demo.computeTask = xTaskCreate(computeTask, "ComputeTask", DEMO_STACK_BYTES, NULL, 1U, demoAllocStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) !=
            OS_STATUS_OK ||
        (((demo.emergencyTask = xTaskCreate(emergencyTask, "EmergencyTask", DEMO_STACK_BYTES, NULL, 3U, demoAllocStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) != OS_STATUS_OK) {
        fprintf(stderr, "failed to create demo tasks\n");
        return 1;
    }

    os_SetObserver(renderDashboard, &demo);
    status = os_Start(run_ticks);
    os_GetRuntimeSnapshot(&final_snapshot);

    printf(
        "\nsummary: car_steps=%u compute_steps=%u emergency_runs=%u switches=%u\n",
        atomic_load_explicit(&g_car_steps, memory_order_relaxed),
        atomic_load_explicit(&g_compute_steps, memory_order_relaxed),
        atomic_load_explicit(&g_emergency_runs, memory_order_relaxed),
        final_snapshot.switch_count
    );

    if ((status != OS_STATUS_OK) ||
        (atomic_load_explicit(&g_car_steps, memory_order_relaxed) == 0U) ||
        (atomic_load_explicit(&g_compute_steps, memory_order_relaxed) == 0U) ||
        (atomic_load_explicit(&g_emergency_runs, memory_order_relaxed) == 0U) ||
        (final_snapshot.switch_count < 4U)) {
        fprintf(stderr, "preemption demo verification failed\n");
        return 1;
    }

    printf("preemption demo verification passed\n");
    return 0;
}
