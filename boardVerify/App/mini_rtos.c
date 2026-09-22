/**
 * @file mini_rtos.c
 * @brief boardVerify Phase C smoke — mini_rtos tasks + IPC (LEDs + UART).
 *
 * main.c keeps Cube/HAL init only and calls mini_rtos_app_start().
 */

#include "mini_rtos.h"

#include "main.h"
#include "usart.h"
#include "gpio.h"

#include "os.h"

#include <stdio.h>
#include <string.h>

/* Private define ------------------------------------------------------------*/

/* Task stacks: enough for HAL_UART + snprintf on Cortex-M3. */
#define SMOKE_STACK_BYTES 512U
#define QUEUE_CAPACITY    4U

/* OS_TICK_MS is 10 → 100 ticks ≈ 1 s, 50 ≈ 0.5 s, 10 ≈ 100 ms. */
#define HEARTBEAT_PERIOD_TICKS 50U   /* LED1 toggle ~1 Hz (active-low) */
#define WORKER_BUSY_TICKS      5U    /* high-prio busy window */
#define WORKER_IDLE_TICKS      20U
#define PRODUCER_PERIOD_TICKS  30U
#define STATUS_PERIOD_TICKS    100U  /* UART ~1 Hz */

/* User priorities: 1 lowest, 7 highest. Idle is 0 and created by the kernel. */
#define PRIO_HEARTBEAT 1U
#define PRIO_PRODUCER  2U
#define PRIO_STATUS    2U
#define PRIO_CONSUMER  3U
#define PRIO_WORKER    5U

/* Private macro -------------------------------------------------------------*/

/* CubeMX leaves LEDs high at reset; treat as active-low (ON = reset pin). */
#define LED_ON(port, pin)  HAL_GPIO_WritePin((port), (pin), GPIO_PIN_RESET)
#define LED_OFF(port, pin) HAL_GPIO_WritePin((port), (pin), GPIO_PIN_SET)
#define LED_TOGGLE(port, pin) HAL_GPIO_TogglePin((port), (pin))

/* Private variables ---------------------------------------------------------*/

static uint8_t g_stack_idle_hb[SMOKE_STACK_BYTES];
static uint8_t g_stack_worker[SMOKE_STACK_BYTES];
static uint8_t g_stack_producer[SMOKE_STACK_BYTES];
static uint8_t g_stack_consumer[SMOKE_STACK_BYTES];
static uint8_t g_stack_status[SMOKE_STACK_BYTES];

static uint32_t g_queue_storage[QUEUE_CAPACITY];

static SemaphoreHandle_t g_sync_sem;
static MutexHandle_t g_uart_mutex;
static QueueHandle_t g_event_queue;

static volatile uint32_t g_cnt_heartbeat;
static volatile uint32_t g_cnt_worker;
static volatile uint32_t g_cnt_produced;
static volatile uint32_t g_cnt_consumed;
static volatile uint32_t g_cnt_sync;

/* Private function prototypes -----------------------------------------------*/

static void smoke_uart_write(const char *s);
static void task_heartbeat(void *argument);
static void task_worker(void *argument);
static void task_producer(void *argument);
static void task_consumer(void *argument);
static void task_status(void *argument);
/* entry: mini_rtos_app_start in mini_rtos.h */

/* Private user code ---------------------------------------------------------*/


/**
 * @brief Blocking USART1 TX; protected by mutex when scheduler is running.
 */
static void smoke_uart_write(const char *s)
{
  size_t len;

  if (s == NULL) {
    return;
  }
  len = strlen(s);
  if (len == 0U) {
    return;
  }

  if (os_IsRunning() && (g_uart_mutex != NULL)) {
    (void)xMutexLock(g_uart_mutex, OS_WAIT_FOREVER);
  }

  (void)HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)len, 100U);

  if (os_IsRunning() && (g_uart_mutex != NULL)) {
    (void)xMutexUnlock(g_uart_mutex);
  }
}

/* LED1: low-frequency idle/heartbeat blink. */
static void task_heartbeat(void *argument)
{
  (void)argument;
  for (;;) {
    LED_TOGGLE(LED1_GPIO_Port, LED1_Pin);
    g_cnt_heartbeat++;
    vTaskDelay(HEARTBEAT_PERIOD_TICKS);
  }
}

/* LED2: high-prio worker — blink while "busy", off while delayed. */
static void task_worker(void *argument)
{
  (void)argument;
  for (;;) {
    LED_ON(LED2_GPIO_Port, LED2_Pin);
    /* Busy-wait a few ticks so preemption is visible vs lower tasks. */
    {
      const uint32_t start = xTaskGetTickCount();
      while ((xTaskGetTickCount() - start) < WORKER_BUSY_TICKS) {
        /* spin */
      }
    }
    LED_OFF(LED2_GPIO_Port, LED2_Pin);
    g_cnt_worker++;
    vTaskDelay(WORKER_IDLE_TICKS);
  }
}

/* Producer: queue item + give sync semaphore (drives LED3 via consumer). */
static void task_producer(void *argument)
{
  uint32_t seq = 0U;
  (void)argument;

  for (;;) {
    seq++;
    if (xQueueSend(g_event_queue, &seq, OS_WAIT_FOREVER) == OS_STATUS_OK) {
      g_cnt_produced++;
      (void)xSemaphoreGive(g_sync_sem);
    }
    vTaskDelay(PRODUCER_PERIOD_TICKS);
  }
}

/* Consumer: take sem, receive queue — pulse LED3 on each sync event. */
static void task_consumer(void *argument)
{
  (void)argument;

  for (;;) {
    if (xSemaphoreTake(g_sync_sem, OS_WAIT_FOREVER) != OS_STATUS_OK) {
      continue;
    }

    {
      uint32_t item = 0U;
      if (xQueueReceive(g_event_queue, &item, 0U) == OS_STATUS_OK) {
        g_cnt_consumed++;
        g_cnt_sync++;
        LED_ON(LED3_GPIO_Port, LED3_Pin);
        vTaskDelay(5U); /* short visible pulse */
        LED_OFF(LED3_GPIO_Port, LED3_Pin);
        (void)item;
      }
    }
  }
}

/* UART ~1 Hz status: tick, current task name, counters. */
static void task_status(void *argument)
{
  char line[128];
  (void)argument;

  for (;;) {
    os_runtime_snapshot_t snap;
    const char *name;

    os_GetRuntimeSnapshot(&snap);
    name = (snap.current != NULL) ? pcTaskGetName(snap.current) : "idle";

    (void)snprintf(
        line,
        sizeof(line),
        "tick=%lu task=%s hb=%lu wk=%lu prod=%lu cons=%lu sync=%lu q=%u\r\n",
        (unsigned long)snap.tick,
        name,
        (unsigned long)g_cnt_heartbeat,
        (unsigned long)g_cnt_worker,
        (unsigned long)g_cnt_produced,
        (unsigned long)g_cnt_consumed,
        (unsigned long)g_cnt_sync,
        (unsigned)uxQueueMessagesWaiting(g_event_queue));
    smoke_uart_write(line);
    vTaskDelay(STATUS_PERIOD_TICKS);
  }
}

void mini_rtos_app_start(void)
{
  LED_OFF(LED1_GPIO_Port, LED1_Pin);
  LED_OFF(LED2_GPIO_Port, LED2_Pin);
  LED_OFF(LED3_GPIO_Port, LED3_Pin);

  os_Init();

  g_sync_sem = xSemaphoreCreateCounting(8U, 0U);
  g_uart_mutex = xMutexCreate();
  g_event_queue = xQueueCreate(QUEUE_CAPACITY, sizeof(uint32_t), g_queue_storage);
  if ((g_sync_sem == NULL) || (g_uart_mutex == NULL) || (g_event_queue == NULL)) {
    Error_Handler();
  }

  if (xTaskCreate(task_heartbeat, "heartbeat", SMOKE_STACK_BYTES, NULL,
                  PRIO_HEARTBEAT, g_stack_idle_hb) == NULL ||
      xTaskCreate(task_producer, "producer", SMOKE_STACK_BYTES, NULL,
                  PRIO_PRODUCER, g_stack_producer) == NULL ||
      xTaskCreate(task_consumer, "consumer", SMOKE_STACK_BYTES, NULL,
                  PRIO_CONSUMER, g_stack_consumer) == NULL ||
      xTaskCreate(task_status, "status", SMOKE_STACK_BYTES, NULL,
                  PRIO_STATUS, g_stack_status) == NULL ||
      xTaskCreate(task_worker, "worker", SMOKE_STACK_BYTES, NULL,
                  PRIO_WORKER, g_stack_worker) == NULL) {
    Error_Handler();
  }

  smoke_uart_write("boardVerify smoke: LEDs + UART + mini_rtos\r\n");

  /* run_ticks=0 → run until only Idle remains (never, for this smoke). */
  (void)os_Start(0U);

  /* os_Start should not return on Cortex-M; park if it does. */
  for (;;) {
  }
}
