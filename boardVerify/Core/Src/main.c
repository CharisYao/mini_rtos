/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body — mini_rtos board smoke (LEDs + UART)
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "os.h"

#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Task stacks: enough for HAL_UART + snprintf on Cortex-M3. */
#define SMOKE_STACK_BYTES 512U
#define QUEUE_CAPACITY    4U

/* OS_TICK_MS is 10 → 100 ticks ≈ 1 s, 50 ≈ 0.5 s, 10 ≈ 100 ms. */
#define HEARTBEAT_PERIOD_TICKS 50U   /* LED1 toggle ~1 Hz (active-low) */
#define WORKER_BUSY_TICKS      5U    /* high-prio busy window */
#define WORKER_IDLE_TICKS      20U
#define PRODUCER_PERIOD_TICKS  30U
#define STATUS_PERIOD_TICKS    100U  /* UART ~1 Hz */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* CubeMX leaves LEDs high at reset; treat as active-low (ON = reset pin). */
#define LED_ON(port, pin)  HAL_GPIO_WritePin((port), (pin), GPIO_PIN_RESET)
#define LED_OFF(port, pin) HAL_GPIO_WritePin((port), (pin), GPIO_PIN_SET)
#define LED_TOGGLE(port, pin) HAL_GPIO_TogglePin((port), (pin))
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static uint8_t g_stack_idle_hb[SMOKE_STACK_BYTES];
static uint8_t g_stack_worker[SMOKE_STACK_BYTES];
static uint8_t g_stack_producer[SMOKE_STACK_BYTES];
static uint8_t g_stack_consumer[SMOKE_STACK_BYTES];
static uint8_t g_stack_status[SMOKE_STACK_BYTES];

static uint32_t g_queue_storage[QUEUE_CAPACITY];

static os_sem_t *g_sync_sem;
static os_mutex_t *g_uart_mutex;
static os_queue_t *g_event_queue;

static volatile uint32_t g_cnt_heartbeat;
static volatile uint32_t g_cnt_worker;
static volatile uint32_t g_cnt_produced;
static volatile uint32_t g_cnt_consumed;
static volatile uint32_t g_cnt_sync;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void smoke_uart_write(const char *s);
static void task_heartbeat(void *argument);
static void task_worker(void *argument);
static void task_producer(void *argument);
static void task_consumer(void *argument);
static void task_status(void *argument);
static void board_smoke_start(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

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
    (void)os_MutexLock(g_uart_mutex, OS_WAIT_FOREVER);
  }

  (void)HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)len, 100U);

  if (os_IsRunning() && (g_uart_mutex != NULL)) {
    (void)os_MutexUnlock(g_uart_mutex);
  }
}

/* LED1: low-frequency idle/heartbeat blink. */
static void task_heartbeat(void *argument)
{
  (void)argument;
  for (;;) {
    LED_TOGGLE(LED1_GPIO_Port, LED1_Pin);
    g_cnt_heartbeat++;
    os_Delay(HEARTBEAT_PERIOD_TICKS);
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
      const uint32_t start = os_TickGet();
      while ((os_TickGet() - start) < WORKER_BUSY_TICKS) {
        /* spin */
      }
    }
    LED_OFF(LED2_GPIO_Port, LED2_Pin);
    g_cnt_worker++;
    os_Delay(WORKER_IDLE_TICKS);
  }
}

/* Producer: queue item + give sync semaphore (drives LED3 via consumer). */
static void task_producer(void *argument)
{
  uint32_t seq = 0U;
  (void)argument;

  for (;;) {
    seq++;
    if (os_QueueSend(g_event_queue, &seq, OS_WAIT_FOREVER) == OS_STATUS_OK) {
      g_cnt_produced++;
      (void)os_SemGive(g_sync_sem);
    }
    os_Delay(PRODUCER_PERIOD_TICKS);
  }
}

/* Consumer: take sem, receive queue — pulse LED3 on each sync event. */
static void task_consumer(void *argument)
{
  (void)argument;

  for (;;) {
    if (os_SemTake(g_sync_sem, OS_WAIT_FOREVER) != OS_STATUS_OK) {
      continue;
    }

    {
      uint32_t item = 0U;
      if (os_QueueReceive(g_event_queue, &item, 0U) == OS_STATUS_OK) {
        g_cnt_consumed++;
        g_cnt_sync++;
        LED_ON(LED3_GPIO_Port, LED3_Pin);
        os_Delay(5U); /* short visible pulse */
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
    name = (snap.current != NULL) ? os_TaskGetName(snap.current) : "idle";

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
        (unsigned)os_QueueGetCount(g_event_queue));
    smoke_uart_write(line);
    os_Delay(STATUS_PERIOD_TICKS);
  }
}

static void board_smoke_start(void)
{
  os_task_t *t;

  LED_OFF(LED1_GPIO_Port, LED1_Pin);
  LED_OFF(LED2_GPIO_Port, LED2_Pin);
  LED_OFF(LED3_GPIO_Port, LED3_Pin);

  os_Init();

  if (os_SemInit(&g_sync_sem, 0U, 8U) != OS_STATUS_OK) {
    Error_Handler();
  }
  if (os_MutexInit(&g_uart_mutex) != OS_STATUS_OK) {
    Error_Handler();
  }
  if (os_QueueInit(
          &g_event_queue, g_queue_storage, sizeof(uint32_t), QUEUE_CAPACITY) !=
      OS_STATUS_OK) {
    Error_Handler();
  }

  /* Priorities: 1=lowest user … 7=highest. Idle is 0 (auto). */
  if (os_TaskCreate(
          &t, "heartbeat", task_heartbeat, NULL, 1U, g_stack_idle_hb,
          SMOKE_STACK_BYTES) != OS_STATUS_OK) {
    Error_Handler();
  }
  if (os_TaskCreate(
          &t, "producer", task_producer, NULL, 2U, g_stack_producer,
          SMOKE_STACK_BYTES) != OS_STATUS_OK) {
    Error_Handler();
  }
  if (os_TaskCreate(
          &t, "consumer", task_consumer, NULL, 3U, g_stack_consumer,
          SMOKE_STACK_BYTES) != OS_STATUS_OK) {
    Error_Handler();
  }
  if (os_TaskCreate(
          &t, "status", task_status, NULL, 2U, g_stack_status,
          SMOKE_STACK_BYTES) != OS_STATUS_OK) {
    Error_Handler();
  }
  if (os_TaskCreate(
          &t, "worker", task_worker, NULL, 5U, g_stack_worker,
          SMOKE_STACK_BYTES) != OS_STATUS_OK) {
    Error_Handler();
  }

  smoke_uart_write("boardVerify smoke: LEDs + UART + mini_rtos\r\n");

  /* run_ticks=0 → run until only Idle remains (never, for this smoke). */
  (void)os_Start(0U);

  /* os_Start should not return on Cortex-M; park if it does. */
  for (;;) {
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  board_smoke_start();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM4 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM4)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
