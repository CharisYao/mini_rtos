/**
 * @file test_kernel.c
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 对调度、时间管理、IPC、互斥量和内核不变量执行白盒与随机测试。
 *
 * @details
 * 本文件显式包含 kernel 内部模块头，在不创建 Windows 工作线程的情况下驱动通用内核，
 * 用于验证链表、最高优先级选择、同优先级轮转、延时与 tick 回绕、任务创建边界、
 * 信号量阻塞唤醒、队列直接交付、互斥量所有权和优先级继承等行为。
 *
 * 26 个测试用例在关键状态迁移后调用 os_KernelValidate() 检查 TCB、链表和就绪位图
 * 是否一致。其中随机测试使用固定伪随机序列执行一万次操作并跨越 tick 回绕，使结果
 * 可重复。CHECK 宏在首个失败位置打印条件、文件和行号，便于定位内核错误。
 */

#include "os.h"
#include "os_list.h"
#include "os_task_internal.h"
#include "os_kernel_state.h"
#include "os_sched.h"
#include "os_time.h"
#include "os_sem_internal.h"
#include "os_queue_internal.h"
#include "os_mutex_internal.h"

#include <stdio.h>
#include <string.h>

#define TEST_STACK_BYTES 128U
static uint8_t g_test_stacks[64][TEST_STACK_BYTES];
static size_t g_test_stack_cursor;

static void *testStack(void)
{
    if (g_test_stack_cursor >= 64U) {
        g_test_stack_cursor = 0U;
    }
    return g_test_stacks[g_test_stack_cursor++];
}

static void resetTestStacks(void)
{
    g_test_stack_cursor = 0U;
}

/* 任一条件失败就打印源位置并让当前测试立即返回 false。 */
#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                           \
            printf("    CHECK failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
            return false;                                                            \
        }                                                                            \
    } while (0)

/* 无参数内核测试函数类型，true 表示全部检查通过。 */
typedef bool (*test_function_t)(void);

/* 测试显示名称与函数入口的对应关系 */
typedef struct {
    const char *name;         /* 控制台显示的测试名称。 */
    test_function_t function; /* 对应测试函数。 */
} test_case_t;

/* 任务调度逻辑测试只需要一个不会真正执行的入口占位符。 */
static void dummyTask(void *argument)
{
    (void)argument;
}

/**
 * @brief 验证初始化前拒绝创建任务，以及空内核初始化后的基本不变量。
 * @return 全部检查通过时返回 true。
 */
static bool testEmptyKernel(void)
{
    TaskHandle_t task = (TaskHandle_t )1;

    memset(&g_os_kernel, 0, sizeof(g_os_kernel));
    CHECK((((task = xTaskCreate(dummyTask, "early", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_BAD_STATE);
    CHECK(task == NULL);

    resetTestStacks();
    os_Init();

    CHECK(g_os_kernel.initialized);
    CHECK(g_os_kernel.task_count == 0U);
    CHECK(g_os_kernel.current == NULL);
    CHECK(os_SchedStart() == NULL);
    CHECK(!g_os_kernel.started);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证侵入式链表的追加、中间删除和表头弹出关系。
 * @return 全部检查通过时返回 true。
 */
static bool testIntrusiveListOperations(void)
{
    os_list_t list;
    os_task_t tasks[3] = {0};

    os_ListInit(&list);
    for (size_t index = 0U; index < 3U; index++) {
        tasks[index].schedule_node.owner = &tasks[index];
        os_ListPushBack(&list, &tasks[index].schedule_node);
    }

    CHECK(list.size == 3U);
    CHECK(list.head == &tasks[0].schedule_node);
    CHECK(list.tail == &tasks[2].schedule_node);

    os_ListRemove(&list, &tasks[1].schedule_node);
    CHECK(list.size == 2U);
    CHECK(list.head->next == list.tail);
    CHECK(list.tail->previous == list.head);
    CHECK(!tasks[1].schedule_node.linked);

    CHECK(os_ListPopFront(&list) == &tasks[0].schedule_node);
    CHECK(os_ListPopFront(&list) == &tasks[2].schedule_node);
    CHECK(os_ListPopFront(&list) == NULL);
    CHECK(list.size == 0U);
    CHECK((list.head == NULL) && (list.tail == NULL));
    return true;
}

/**
 * @brief 验证启动调度器时首先选择最高优先级 READY 任务。
 * @return 全部检查通过时返回 true。
 */
static bool testHighestPriorityStartsFirst(void)
{
    TaskHandle_t low;
    TaskHandle_t medium;
    TaskHandle_t high;

    resetTestStacks();
    os_Init();
    CHECK((((low = xTaskCreate(dummyTask, "low", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((high = xTaskCreate(dummyTask, "high", TEST_STACK_BYTES, NULL, 6U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((medium = xTaskCreate(dummyTask, "medium", TEST_STACK_BYTES, NULL, 3U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);

    CHECK(os_SchedStart() == high);
    CHECK(eTaskGetState(high) == OS_TASK_RUNNING);
    CHECK(eTaskGetState(medium) == OS_TASK_READY);
    CHECK(eTaskGetState(low) == OS_TASK_READY);
    CHECK(g_os_kernel.last_reason == OS_SWITCH_START);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证三个同优先级任务按 FIFO 顺序循环轮转。
 * @return 全部检查通过时返回 true。
 */
static bool testEqualPriorityRoundRobin(void)
{
    TaskHandle_t task_a;
    TaskHandle_t task_b;
    TaskHandle_t task_c;

    resetTestStacks();
    os_Init();
    CHECK((((task_a = xTaskCreate(dummyTask, "task_a", TEST_STACK_BYTES, NULL, 2U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((task_b = xTaskCreate(dummyTask, "task_b", TEST_STACK_BYTES, NULL, 2U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((task_c = xTaskCreate(dummyTask, "task_c", TEST_STACK_BYTES, NULL, 2U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);

    CHECK(os_SchedStart() == task_a);
    CHECK(os_SchedSchedule(OS_SWITCH_TIME_SLICE, true) == task_b);
    CHECK(os_SchedSchedule(OS_SWITCH_TIME_SLICE, true) == task_c);
    CHECK(os_SchedSchedule(OS_SWITCH_TIME_SLICE, true) == task_a);
    CHECK(g_os_kernel.switch_count == 4U);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证只有一个任务时，时间片判断不会记录无意义的自切换。
 * @return 全部检查通过时返回 true。
 */
static bool testSingleTaskDoesNotSwitchToItself(void)
{
    TaskHandle_t only_task;
    uint32_t switch_count;

    resetTestStacks();
    os_Init();
    CHECK((((only_task = xTaskCreate(dummyTask, "only", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == only_task);
    switch_count = g_os_kernel.switch_count;

    CHECK(os_SchedSchedule(OS_SWITCH_TIME_SLICE, true) == only_task);
    CHECK(g_os_kernel.switch_count == switch_count);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证更高优先级任务恢复 READY 后立即抢占当前低优先级任务。
 * @return 全部检查通过时返回 true。
 */
static bool testHigherPriorityReadyTaskPreempts(void)
{
    TaskHandle_t low;
    TaskHandle_t high;

    resetTestStacks();
    os_Init();
    CHECK((((low = xTaskCreate(dummyTask, "low", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((high = xTaskCreate(dummyTask, "high", TEST_STACK_BYTES, NULL, 5U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);

    CHECK(os_ReadyRemove(high));
    high->state = OS_TASK_BLOCKED_DELAY;
    high->wake_tick = 100U;
    os_ListPushBack(&g_os_kernel.delayed, &high->schedule_node);
    CHECK(os_SchedStart() == low);
    CHECK(os_KernelValidate());

    os_ListRemove(&g_os_kernel.delayed, &high->schedule_node);
    high->state = OS_TASK_READY;
    os_ReadyEnqueue(high);
    CHECK(os_SchedSchedule(OS_SWITCH_HIGHER_PRIORITY_WAKEUP, false) == high);
    CHECK(eTaskGetState(low) == OS_TASK_READY);
    CHECK(eTaskGetState(high) == OS_TASK_RUNNING);
    CHECK(g_os_kernel.last_from == low);
    CHECK(g_os_kernel.last_to == high);
    CHECK(g_os_kernel.last_reason == OS_SWITCH_HIGHER_PRIORITY_WAKEUP);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证 os_Delay 对应的阻塞、逐 tick 等待和到期抢占流程。
 * @return 全部检查通过时返回 true。
 */
static bool testDelayBlocksAndWakesTask(void)
{
    TaskHandle_t low;
    TaskHandle_t high;

    resetTestStacks();
    os_Init();
    CHECK((((low = xTaskCreate(dummyTask, "low", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((high = xTaskCreate(dummyTask, "high", TEST_STACK_BYTES, NULL, 5U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == high);

    CHECK(os_TimeDelayCurrent(3U) == low);
    CHECK(eTaskGetState(high) == OS_TASK_BLOCKED_DELAY);
    CHECK(os_KernelValidate());

    CHECK(os_TimeTick() == low);
    CHECK(os_TimeTick() == low);
    CHECK(eTaskGetState(high) == OS_TASK_BLOCKED_DELAY);

    CHECK(os_TimeTick() == high);
    CHECK(eTaskGetState(high) == OS_TASK_RUNNING);
    CHECK(eTaskGetState(low) == OS_TASK_READY);
    CHECK(g_os_kernel.last_reason == OS_SWITCH_HIGHER_PRIORITY_WAKEUP);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证时间片最后一个 tick 触发同优先级任务轮转。
 * @return 全部检查通过时返回 true。
 */
static bool testTickRotatesEqualPriorityTasks(void)
{
    TaskHandle_t task_a;
    TaskHandle_t task_b;

    resetTestStacks();
    os_Init();
    CHECK((((task_a = xTaskCreate(dummyTask, "task_a", TEST_STACK_BYTES, NULL, 2U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((task_b = xTaskCreate(dummyTask, "task_b", TEST_STACK_BYTES, NULL, 2U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == task_a);

    for (uint32_t tick = 1U; tick < OS_TIME_SLICE_TICKS; tick++) {
        CHECK(os_TimeTick() == task_a);
    }
    CHECK(os_TimeTick() == task_b);
    CHECK(g_os_kernel.last_reason == OS_SWITCH_TIME_SLICE);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证 wake_tick 穿越 UINT32_MAX 后仍能在正确 tick 唤醒。
 * @return 全部检查通过时返回 true。
 */
static bool testDelayWakeHandlesTickWrap(void)
{
    TaskHandle_t task;

    resetTestStacks();
    os_Init();
    CHECK((((task = xTaskCreate(dummyTask, "task", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == task);
    g_os_kernel.tick = UINT32_MAX - 1U;

    CHECK(os_TimeDelayCurrent(3U) == NULL);
    CHECK(task->wake_tick == 1U);
    CHECK(os_TimeTick() == NULL);
    CHECK(os_TimeTick() == NULL);
    CHECK(os_TimeTick() == task);
    CHECK(eTaskGetState(task) == OS_TASK_RUNNING);
    CHECK(xTaskGetTickCount() == 1U);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证 READY 的低优先级任务不能因时间片判断替换高优先级当前任务。
 * @return 全部检查通过时返回 true。
 */
static bool testLowerPriorityDoesNotReplaceCurrent(void)
{
    TaskHandle_t low;
    TaskHandle_t high;
    uint32_t switch_count;

    resetTestStacks();
    os_Init();
    CHECK((((high = xTaskCreate(dummyTask, "high", TEST_STACK_BYTES, NULL, 5U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((low = xTaskCreate(dummyTask, "low", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == high);
    switch_count = g_os_kernel.switch_count;

    CHECK(os_SchedSchedule(OS_SWITCH_TIME_SLICE, true) == high);
    CHECK(g_os_kernel.switch_count == switch_count);
    CHECK(eTaskGetState(low) == OS_TASK_READY);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证任务名、入口、输出指针和优先级等创建参数边界。
 * @return 全部检查通过时返回 true。
 */
static bool testTaskCreationValidation(void)
{
    TaskHandle_t task = NULL;
    char long_name[OS_TASK_NAME_MAX + 1U];

    memset(long_name, 'x', sizeof(long_name));
    long_name[sizeof(long_name) - 1U] = '\0';

    resetTestStacks();
    os_Init();
    CHECK((((task = xTaskCreate(dummyTask, NULL, TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_INVALID_ARGUMENT);
    CHECK((((task = xTaskCreate(dummyTask, "", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_INVALID_ARGUMENT);
    CHECK((((task = xTaskCreate(dummyTask, long_name, TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_INVALID_ARGUMENT);
    CHECK((((task = xTaskCreate(NULL, "task", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_INVALID_ARGUMENT);
    CHECK((((task = xTaskCreate(dummyTask, "idle", TEST_STACK_BYTES, NULL, 0U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_INVALID_ARGUMENT);
    CHECK((((task = xTaskCreate(dummyTask, "nostack", TEST_STACK_BYTES, NULL, 1U, NULL)) != NULL) ? OS_STATUS_OK : osGetLastError()) ==
          OS_STATUS_INVALID_ARGUMENT);
    CHECK((((task = xTaskCreate(dummyTask, "smallstack", 8U, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) ==
          OS_STATUS_INVALID_ARGUMENT);
    CHECK((((task = xTaskCreate(dummyTask, "too_high", TEST_STACK_BYTES, NULL, OS_PRIORITY_COUNT, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) ==
          OS_STATUS_INVALID_ARGUMENT);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证静态 TCB 数量上限以及任务只读查询接口。
 * @return 全部检查通过时返回 true。
 */
static bool testTaskLimitAndGetters(void)
{
    TaskHandle_t task = NULL;
    TaskHandle_t last_task = NULL;
    char names[OS_MAX_TASKS][OS_TASK_NAME_MAX];

    resetTestStacks();
    os_Init();

    for (size_t index = 0U; index < OS_MAX_TASKS; index++) {
        (void)snprintf(names[index], sizeof(names[index]), "task_%u", (unsigned)index);
        CHECK((((task = xTaskCreate(dummyTask, names[index], TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
        last_task = task;
    }

    CHECK((((task = xTaskCreate(dummyTask, "overflow", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_LIMIT_REACHED);
    CHECK(task == NULL);
    CHECK(strcmp(pcTaskGetName(last_task), "task_7") == 0);
    CHECK(uxTaskPriorityGet(last_task) == 1U);
    CHECK(eTaskGetState(last_task) == OS_TASK_READY);
    CHECK(pcTaskGetName(NULL) == NULL);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证不变量检查器能够发现调度节点与 TCB 所有者不一致。
 * @return 全部检查通过时返回 true。
 */
static bool testInvariantCheckerRejectsBadOwner(void)
{
    TaskHandle_t task;

    resetTestStacks();
    os_Init();
    CHECK((((task = xTaskCreate(dummyTask, "task", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_KernelValidate());

    task->schedule_node.owner = NULL;
    CHECK(!os_KernelValidate());

    task->schedule_node.owner = task;
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证调度器启动后不允许继续向静态任务池创建任务。
 * @return 全部检查通过时返回 true。
 */
static bool testCreationAfterStartIsRejected(void)
{
    TaskHandle_t first;
    TaskHandle_t late = NULL;

    resetTestStacks();
    os_Init();
    CHECK((((first = xTaskCreate(dummyTask, "first", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == first);
    CHECK((((late = xTaskCreate(dummyTask, "late", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_BAD_STATE);
    CHECK(late == NULL);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证高优先级信号量等待者阻塞，并在释放时立即被唤醒抢占。
 * @return 全部检查通过时返回 true。
 */
static bool testSemaphoreBlocksAndWakesHighPriorityTask(void)
{
    TaskHandle_t low;
    TaskHandle_t high;
    SemaphoreHandle_t sem;

    resetTestStacks();
    os_Init();
    CHECK((((sem = xSemaphoreCreateCounting(1U, 0U)) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((low = xTaskCreate(dummyTask, "low", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((high = xTaskCreate(dummyTask, "high", TEST_STACK_BYTES, NULL, 5U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == high);

    os_SemTakeCurrent(sem, OS_WAIT_FOREVER);
    CHECK(g_os_kernel.current == low);
    CHECK(high->state == OS_TASK_BLOCKED_OBJECT);
    CHECK(high->wait_kind == OS_WAIT_SEMAPHORE);
    CHECK(uxSemaphoreGetCount(sem) == 0U);
    CHECK(os_KernelValidate());

    os_SemGiveCurrent(sem);
    CHECK(g_os_kernel.current == high);
    CHECK(high->wait_result == OS_STATUS_OK);
    CHECK(low->wait_result == OS_STATUS_OK);
    CHECK(uxSemaphoreGetCount(sem) == 0U);
    CHECK(g_os_kernel.last_reason == OS_SWITCH_HIGHER_PRIORITY_WAKEUP);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证信号量有限等待超时以及最大计数限制。
 * @return 全部检查通过时返回 true。
 */
static bool testSemaphoreTimeoutAndCountLimit(void)
{
    TaskHandle_t task;
    SemaphoreHandle_t sem;

    resetTestStacks();
    os_Init();
    CHECK((((sem = xSemaphoreCreateCounting(1U, 0U)) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((task = xTaskCreate(dummyTask, "waiter", TEST_STACK_BYTES, NULL, 2U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == task);

    os_SemTakeCurrent(sem, 3U);
    CHECK(g_os_kernel.current == NULL);
    CHECK(os_TimeTick() == NULL);
    CHECK(os_TimeTick() == NULL);
    CHECK(os_TimeTick() == task);
    CHECK(task->wait_result == OS_STATUS_TIMEOUT);
    CHECK(os_KernelValidate());

    os_SemGiveCurrent(sem);
    CHECK(task->wait_result == OS_STATUS_OK);
    CHECK(uxSemaphoreGetCount(sem) == 1U);
    os_SemGiveCurrent(sem);
    CHECK(task->wait_result == OS_STATUS_LIMIT_REACHED);
    CHECK(uxSemaphoreGetCount(sem) == 1U);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证发送消息直接交付给已阻塞接收者而不占用环形队列。
 * @return 全部检查通过时返回 true。
 */
static bool testQueueDirectHandoffToWaitingReceiver(void)
{
    int storage[2] = {0};
    int received = 0;
    const int sent = 42;
    TaskHandle_t low;
    TaskHandle_t high;
    QueueHandle_t queue;

    resetTestStacks();
    os_Init();
    CHECK((((queue = xQueueCreate(2U, sizeof(storage[0]), storage)) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((low = xTaskCreate(dummyTask, "producer", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((high = xTaskCreate(dummyTask, "consumer", TEST_STACK_BYTES, NULL, 5U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == high);

    os_QueueReceiveCurrent(queue, &received, OS_WAIT_FOREVER);
    CHECK(g_os_kernel.current == low);
    CHECK(high->wait_kind == OS_WAIT_QUEUE_RECEIVE);
    CHECK(uxQueueMessagesWaiting(queue) == 0U);
    CHECK(os_KernelValidate());

    os_QueueSendCurrent(queue, &sent, OS_WAIT_FOREVER);
    CHECK(g_os_kernel.current == high);
    CHECK(received == sent);
    CHECK(high->wait_result == OS_STATUS_OK);
    CHECK(uxQueueMessagesWaiting(queue) == 0U);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证满队列发送者阻塞，并在接收释放槽位后立即补入待发消息。
 * @return 全部检查通过时返回 true。
 */
static bool testQueueFullSenderRefillsFreedSlot(void)
{
    int storage[1] = {0};
    int received = 0;
    int queued = 0;
    const int first = 11;
    const int second = 22;
    TaskHandle_t low;
    TaskHandle_t high;
    QueueHandle_t queue;

    resetTestStacks();
    os_Init();
    CHECK((((queue = xQueueCreate(1U, sizeof(storage[0]), storage)) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((low = xTaskCreate(dummyTask, "consumer", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((high = xTaskCreate(dummyTask, "producer", TEST_STACK_BYTES, NULL, 5U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == high);

    os_QueueSendCurrent(queue, &first, 0U);
    CHECK(high->wait_result == OS_STATUS_OK);
    CHECK(uxQueueMessagesWaiting(queue) == 1U);
    os_QueueSendCurrent(queue, &second, OS_WAIT_FOREVER);
    CHECK(g_os_kernel.current == low);
    CHECK(high->wait_kind == OS_WAIT_QUEUE_SEND);
    CHECK(os_KernelValidate());

    os_QueueReceiveCurrent(queue, &received, OS_WAIT_FOREVER);
    CHECK(received == first);
    CHECK(g_os_kernel.current == high);
    CHECK(high->wait_result == OS_STATUS_OK);
    CHECK(uxQueueMessagesWaiting(queue) == 1U);
    CHECK(os_QueueSnapshot(queue, &queued, 1U) == 1U);
    CHECK(queued == second);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证空队列接收和满队列发送的有限等待超时。
 * @return 全部检查通过时返回 true。
 */
static bool testQueueSendAndReceiveTimeouts(void)
{
    int storage[1] = {0};
    int received = 0;
    const int first = 7;
    const int second = 9;
    TaskHandle_t task;
    QueueHandle_t queue;

    resetTestStacks();
    os_Init();
    CHECK((((queue = xQueueCreate(1U, sizeof(storage[0]), storage)) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((task = xTaskCreate(dummyTask, "queue_user", TEST_STACK_BYTES, NULL, 2U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == task);

    os_QueueReceiveCurrent(queue, &received, 2U);
    CHECK(g_os_kernel.current == NULL);
    CHECK(os_TimeTick() == NULL);
    CHECK(os_TimeTick() == task);
    CHECK(task->wait_result == OS_STATUS_TIMEOUT);

    os_QueueSendCurrent(queue, &first, 0U);
    CHECK(task->wait_result == OS_STATUS_OK);
    os_QueueSendCurrent(queue, &second, 2U);
    CHECK(g_os_kernel.current == NULL);
    CHECK(os_TimeTick() == NULL);
    CHECK(os_TimeTick() == task);
    CHECK(task->wait_result == OS_STATUS_TIMEOUT);
    CHECK(uxQueueMessagesWaiting(queue) == 1U);

    os_QueueReceiveCurrent(queue, &received, 0U);
    CHECK(task->wait_result == OS_STATUS_OK);
    CHECK(received == first);
    CHECK(uxQueueMessagesWaiting(queue) == 0U);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证信号量、队列初始化参数以及静态对象池容量上限。
 * @return 全部检查通过时返回 true。
 */
static bool testIpcInitializationValidationAndLimits(void)
{
    int storage[OS_MAX_QUEUES + 1U] = {0};
    SemaphoreHandle_t sem = (SemaphoreHandle_t )1;
    QueueHandle_t queue = (QueueHandle_t )1;

    resetTestStacks();
    os_Init();
    CHECK((((sem = xSemaphoreCreateCounting(1U, 2U)) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_INVALID_ARGUMENT);
    CHECK(sem == NULL);
    CHECK((((queue = xQueueCreate(1U, sizeof(storage[0]), NULL)) != NULL) ? OS_STATUS_OK : osGetLastError()) ==
          OS_STATUS_INVALID_ARGUMENT);
    CHECK(queue == NULL);

    for (size_t index = 0U; index < OS_MAX_SEMAPHORES; index++) {
        CHECK((((sem = xSemaphoreCreateCounting(1U, 0U)) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    }
    CHECK((((sem = xSemaphoreCreateCounting(1U, 0U)) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_LIMIT_REACHED);
    CHECK(sem == NULL);

    for (size_t index = 0U; index < OS_MAX_QUEUES; index++) {
        CHECK((((queue = xQueueCreate(1U, sizeof(storage[index]), &storage[index])) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    }
    CHECK((((queue = xQueueCreate(1U, sizeof(storage[0]), &storage[OS_MAX_QUEUES])) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_LIMIT_REACHED);
    CHECK(queue == NULL);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证互斥量不可递归，以及非所有者无法释放互斥量。
 * @return 全部检查通过时返回 true。
 */
static bool testMutexOwnershipAndNonRecursiveRule(void)
{
    TaskHandle_t owner;
    TaskHandle_t intruder;
    MutexHandle_t mutex;

    resetTestStacks();
    os_Init();
    CHECK((((mutex = xMutexCreate()) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((owner = xTaskCreate(dummyTask, "owner", TEST_STACK_BYTES, NULL, 2U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((intruder = xTaskCreate(dummyTask, "intruder", TEST_STACK_BYTES, NULL, 2U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK(os_SchedStart() == owner);

    os_MutexLockCurrent(mutex, OS_WAIT_FOREVER);
    CHECK(owner->wait_result == OS_STATUS_OK);
    CHECK(mutex->owner == owner);

    os_MutexLockCurrent(mutex, OS_WAIT_FOREVER);
    CHECK(owner->wait_result == OS_STATUS_BAD_STATE);
    CHECK(mutex->owner == owner);

    CHECK(os_SchedYieldCurrent() == intruder);
    os_MutexUnlockCurrent(mutex);
    CHECK(intruder->wait_result == OS_STATUS_NOT_OWNER);
    CHECK(mutex->owner == owner);

    CHECK(os_SchedYieldCurrent() == owner);
    os_MutexUnlockCurrent(mutex);
    CHECK(owner->wait_result == OS_STATUS_OK);
    CHECK(mutex->owner == NULL);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证 P1 所有者继承 P5 优先级，从而越过 P3 并解除优先级反转。
 * @return 全部检查通过时返回 true。
 */
static bool testMutexPriorityInheritancePreventsInversion(void)
{
    TaskHandle_t low;
    TaskHandle_t medium;
    TaskHandle_t high;
    MutexHandle_t mutex;

    resetTestStacks();
    os_Init();
    CHECK((((mutex = xMutexCreate()) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((low = xTaskCreate(dummyTask, "low", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((medium = xTaskCreate(dummyTask, "medium", TEST_STACK_BYTES, NULL, 3U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((high = xTaskCreate(dummyTask, "high", TEST_STACK_BYTES, NULL, 5U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);

    CHECK(os_ReadyRemove(medium));
    medium->state = OS_TASK_BLOCKED_DELAY;
    medium->wake_tick = 1000U;
    os_ListPushBack(&g_os_kernel.delayed, &medium->schedule_node);
    CHECK(os_ReadyRemove(high));
    high->state = OS_TASK_BLOCKED_DELAY;
    high->wake_tick = 1000U;
    os_ListPushBack(&g_os_kernel.delayed, &high->schedule_node);

    CHECK(os_SchedStart() == low);
    os_MutexLockCurrent(mutex, OS_WAIT_FOREVER);
    CHECK(mutex->owner == low);

    os_ListRemove(&g_os_kernel.delayed, &medium->schedule_node);
    medium->state = OS_TASK_READY;
    os_ReadyEnqueue(medium);
    os_ListRemove(&g_os_kernel.delayed, &high->schedule_node);
    high->state = OS_TASK_READY;
    os_ReadyEnqueue(high);
    CHECK(os_SchedSchedule(OS_SWITCH_HIGHER_PRIORITY_WAKEUP, false) == high);

    os_MutexLockCurrent(mutex, OS_WAIT_FOREVER);
    CHECK(high->state == OS_TASK_BLOCKED_OBJECT);
    CHECK(high->wait_kind == OS_WAIT_MUTEX);
    CHECK(low->effective_priority == high->base_priority);
    CHECK(g_os_kernel.current == low);
    CHECK(medium->state == OS_TASK_READY);
    CHECK(os_KernelValidate());

    os_MutexUnlockCurrent(mutex);
    CHECK(mutex->owner == high);
    CHECK(low->effective_priority == low->base_priority);
    CHECK(g_os_kernel.current == high);
    CHECK(high->wait_result == OS_STATUS_OK);

    os_MutexUnlockCurrent(mutex);
    CHECK(mutex->owner == NULL);
    CHECK(high->effective_priority == high->base_priority);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证高优先级等待者超时后，互斥量所有者恢复基础优先级。
 * @return 全部检查通过时返回 true。
 */
static bool testMutexTimeoutRestoresOwnerPriority(void)
{
    TaskHandle_t low;
    TaskHandle_t high;
    MutexHandle_t mutex;

    resetTestStacks();
    os_Init();
    CHECK((((mutex = xMutexCreate()) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((low = xTaskCreate(dummyTask, "low", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((high = xTaskCreate(dummyTask, "high", TEST_STACK_BYTES, NULL, 5U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);

    CHECK(os_ReadyRemove(high));
    high->state = OS_TASK_BLOCKED_DELAY;
    high->wake_tick = 1000U;
    os_ListPushBack(&g_os_kernel.delayed, &high->schedule_node);
    CHECK(os_SchedStart() == low);
    os_MutexLockCurrent(mutex, OS_WAIT_FOREVER);

    os_ListRemove(&g_os_kernel.delayed, &high->schedule_node);
    high->state = OS_TASK_READY;
    os_ReadyEnqueue(high);
    CHECK(os_SchedSchedule(OS_SWITCH_HIGHER_PRIORITY_WAKEUP, false) == high);
    os_MutexLockCurrent(mutex, 2U);
    CHECK(g_os_kernel.current == low);
    CHECK(low->effective_priority == 5U);

    CHECK(os_TimeTick() == low);
    CHECK(os_TimeTick() == high);
    CHECK(high->wait_result == OS_STATUS_TIMEOUT);
    CHECK(low->effective_priority == low->base_priority);
    CHECK(mutex->owner == low);

    CHECK(os_SchedTerminateCurrent() == low);
    os_MutexUnlockCurrent(mutex);
    CHECK(mutex->owner == NULL);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证静态互斥量对象池的初始化参数和数量上限。
 * @return 全部检查通过时返回 true。
 */
static bool testMutexInitializationLimit(void)
{
    MutexHandle_t mutex = (MutexHandle_t )1;

    resetTestStacks();
    os_Init();
    for (size_t index = 0U; index < OS_MAX_MUTEXES; index++) {
        CHECK((((mutex = xMutexCreate()) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    }
    CHECK((((mutex = xMutexCreate()) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_LIMIT_REACHED);
    CHECK(mutex == NULL);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 验证持锁任务终止时把互斥量所有权移交给等待者。
 * @return 全部检查通过时返回 true。
 */
static bool testTaskExitTransfersOwnedMutex(void)
{
    TaskHandle_t low;
    TaskHandle_t high;
    MutexHandle_t mutex;

    resetTestStacks();
    os_Init();
    CHECK((((mutex = xMutexCreate()) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((low = xTaskCreate(dummyTask, "low", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((high = xTaskCreate(dummyTask, "high", TEST_STACK_BYTES, NULL, 5U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);

    CHECK(os_ReadyRemove(high));
    high->state = OS_TASK_BLOCKED_DELAY;
    high->wake_tick = 1000U;
    os_ListPushBack(&g_os_kernel.delayed, &high->schedule_node);
    CHECK(os_SchedStart() == low);
    os_MutexLockCurrent(mutex, OS_WAIT_FOREVER);

    os_ListRemove(&g_os_kernel.delayed, &high->schedule_node);
    high->state = OS_TASK_READY;
    os_ReadyEnqueue(high);
    CHECK(os_SchedSchedule(OS_SWITCH_HIGHER_PRIORITY_WAKEUP, false) == high);
    os_MutexLockCurrent(mutex, OS_WAIT_FOREVER);
    CHECK(g_os_kernel.current == low);

    CHECK(os_SchedTerminateCurrent() == high);
    CHECK(low->state == OS_TASK_TERMINATED);
    CHECK(mutex->owner == high);
    CHECK(high->wait_result == OS_STATUS_OK);
    CHECK(high->state == OS_TASK_RUNNING);

    os_MutexUnlockCurrent(mutex);
    CHECK(mutex->owner == NULL);
    CHECK(os_KernelValidate());
    return true;
}


/**
 * @brief Verify SemGiveFromISR wakes a waiter and respects critical deferral.
 */
static bool testSemGiveFromIsrAndCriticalDefer(void)
{
    SemaphoreHandle_t sem;
    TaskHandle_t low;
    TaskHandle_t high;

    resetTestStacks();
    os_Init();
    CHECK((((sem = xSemaphoreCreateCounting(1U, 0U)) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((low = xTaskCreate(dummyTask, "low", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) ==
          OS_STATUS_OK);
    CHECK((((high = xTaskCreate(dummyTask, "high", TEST_STACK_BYTES, NULL, 5U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) ==
          OS_STATUS_OK);
    CHECK(os_SchedStart() == high);

    os_SemTakeCurrent(sem, OS_WAIT_FOREVER);
    CHECK(g_os_kernel.current == low);
    CHECK(high->state == OS_TASK_BLOCKED_OBJECT);

    os_EnterCritical();
    CHECK(xSemaphoreGiveFromISR(sem) == OS_STATUS_OK);
    CHECK(g_os_kernel.sched_pending);
    CHECK(g_os_kernel.current == low);
    CHECK(high->state == OS_TASK_READY);
    os_ExitCritical();
    CHECK(!g_os_kernel.sched_pending);
    CHECK(g_os_kernel.current == high);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief Auto Idle occupies one TCB slot and rejects a full pool.
 */
static bool testEnsureIdleTask(void)
{
    TaskHandle_t task = NULL;
    char names[OS_MAX_TASKS][OS_TASK_NAME_MAX];

    resetTestStacks();
    os_Init();
    CHECK(os_EnsureIdleTask() == OS_STATUS_OK);
    CHECK(g_os_kernel.task_count == 1U);
    CHECK(os_TaskIsIdle(&g_os_kernel.tasks[0]));
    CHECK(g_os_kernel.tasks[0].sp != NULL);

    for (size_t index = 0U; index < (OS_MAX_TASKS - 1U); index++) {
        (void)snprintf(names[index], sizeof(names[index]), "u_%u", (unsigned)index);
        CHECK((((task = xTaskCreate(dummyTask, names[index], TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    }
    CHECK((((task = xTaskCreate(dummyTask, "overflow", TEST_STACK_BYTES, NULL, 1U, testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) ==
          OS_STATUS_LIMIT_REACHED);
    CHECK(os_EnsureIdleTask() == OS_STATUS_OK);
    CHECK(os_KernelValidate());
    return true;
}

/* 生成可重复的线性同余伪随机序列，使压力测试结果可复现。 */
static uint32_t nextRandom(uint32_t *state)
{
    *state = (*state * 1664525U) + 1013904223U;
    return *state;
}

/**
 * @brief 跨 tick 回绕执行一万次随机调度和内核对象状态迁移。
 * @return 每一步内核不变量均成立时返回 true。
 */
static bool testRandomizedStateTransitionsAcrossTickWrap(void)
{
    int queue_storage[3] = {0};
    int send_values[OS_MAX_TASKS] = {0};
    int receive_values[OS_MAX_TASKS] = {0};
    SemaphoreHandle_t sem;
    QueueHandle_t queue;
    MutexHandle_t mutex;
    TaskHandle_t task;
    uint32_t random_state = 0xC0FFEEU;

    resetTestStacks();
    os_Init();
    CHECK((((sem = xSemaphoreCreateCounting(2U, 1U)) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    CHECK((((queue = xQueueCreate(3U, sizeof(queue_storage[0]), queue_storage)) != NULL) ? OS_STATUS_OK : osGetLastError()) ==
          OS_STATUS_OK);
    CHECK((((mutex = xMutexCreate()) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);

    for (size_t index = 0U; index < 5U; index++) {
        char name[OS_TASK_NAME_MAX];

        (void)snprintf(name, sizeof(name), "stress_%u", (unsigned)index);
        CHECK((((task = xTaskCreate(dummyTask, name, TEST_STACK_BYTES, NULL, (uint8_t)((index % 5U) + 1U), testStack())) != NULL) ? OS_STATUS_OK : osGetLastError()) == OS_STATUS_OK);
    }

    CHECK(os_SchedStart() != NULL);
    g_os_kernel.tick = UINT32_MAX - 100U;

    /* 固定种子覆盖让出、延时、IPC、互斥量和空闲 tick，失败可稳定复现。 */
    for (size_t step = 0U; step < 10000U; step++) {
        TaskHandle_t current = g_os_kernel.current;
        const uint32_t random_value = nextRandom(&random_state);

        if (current == NULL) {
            (void)os_TimeTick();
            CHECK(os_KernelValidate());
            continue;
        }

        switch (random_value % 9U) {
        case 0U:
            (void)os_SchedYieldCurrent();
            break;
        case 1U:
            (void)os_TimeDelayCurrent((random_value % 5U) + 1U);
            break;
        case 2U:
            os_SemTakeCurrent(sem, (random_value % 5U) + 1U);
            break;
        case 3U:
            os_SemGiveCurrent(sem);
            break;
        case 4U:
            send_values[current->id] = (int)random_value;
            os_QueueSendCurrent(
                queue,
                &send_values[current->id],
                (random_value % 5U) + 1U
            );
            break;
        case 5U:
            os_QueueReceiveCurrent(
                queue,
                &receive_values[current->id],
                (random_value % 5U) + 1U
            );
            break;
        case 6U:
            os_MutexLockCurrent(mutex, (random_value % 5U) + 1U);
            break;
        case 7U:
            os_MutexUnlockCurrent(mutex);
            break;
        case 8U:
        default:
            (void)os_TimeTick();
            break;
        }

        if ((step % 3U) == 0U) {
            (void)os_TimeTick();
        }
        CHECK(os_KernelValidate());
    }

    CHECK(xTaskGetTickCount() < 10000U);
    CHECK(os_KernelValidate());
    return true;
}

/**
 * @brief 依次执行全部内核测试，任一失败立即返回非 0。
 * @return 全部测试通过时返回 0。
 */
int main(void)
{
    const test_case_t tests[] = {
        {"empty kernel", testEmptyKernel},
        {"intrusive list operations", testIntrusiveListOperations},
        {"highest priority starts first", testHighestPriorityStartsFirst},
        {"equal priority round robin", testEqualPriorityRoundRobin},
        {"single task no self switch", testSingleTaskDoesNotSwitchToItself},
        {"higher priority preempts", testHigherPriorityReadyTaskPreempts},
        {"delay blocks and wakes task", testDelayBlocksAndWakesTask},
        {"tick rotates equal priority tasks", testTickRotatesEqualPriorityTasks},
        {"delay wake handles tick wrap", testDelayWakeHandlesTickWrap},
        {"lower priority cannot replace current", testLowerPriorityDoesNotReplaceCurrent},
        {"task creation validation", testTaskCreationValidation},
        {"task limit and getters", testTaskLimitAndGetters},
        {"invariant checker rejects bad owner", testInvariantCheckerRejectsBadOwner},
        {"creation after start rejected", testCreationAfterStartIsRejected},
        {"semaphore blocks and wakes high priority", testSemaphoreBlocksAndWakesHighPriorityTask},
        {"semaphore timeout and count limit", testSemaphoreTimeoutAndCountLimit},
        {"queue direct receiver handoff", testQueueDirectHandoffToWaitingReceiver},
        {"queue full sender refill", testQueueFullSenderRefillsFreedSlot},
        {"queue send and receive timeouts", testQueueSendAndReceiveTimeouts},
        {"IPC initialization validation", testIpcInitializationValidationAndLimits},
        {"mutex ownership and non-recursive rule", testMutexOwnershipAndNonRecursiveRule},
        {"mutex priority inheritance", testMutexPriorityInheritancePreventsInversion},
        {"mutex timeout restores priority", testMutexTimeoutRestoresOwnerPriority},
        {"mutex initialization limit", testMutexInitializationLimit},
        {"task exit transfers owned mutex", testTaskExitTransfersOwnedMutex},
        {"randomized transitions across tick wrap", testRandomizedStateTransitionsAcrossTickWrap},
    { "sem_give_from_isr_and_critical_defer", testSemGiveFromIsrAndCriticalDefer },
    { "ensure_idle_task", testEnsureIdleTask },
    };
    size_t passed = 0U;

    printf("miniRTOS kernel tests\n");

    for (size_t index = 0U; index < (sizeof(tests) / sizeof(tests[0])); index++) {
        const bool result = tests[index].function();
        printf("  [%s] %s\n", result ? "PASS" : "FAIL", tests[index].name);
        if (!result) {
            return 1;
        }
        passed++;
    }

    printf("%u/%u tests passed\n", (unsigned)passed, (unsigned)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
