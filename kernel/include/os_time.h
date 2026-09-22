/**
 * @file os_time.h
 * @author Peng RongYao
 * @date 2026-08-22
 * @brief 声明系统 tick、任务延时和时间片处理接口。
 */

#ifndef MINI_RTOS_OS_TIME_H
#define MINI_RTOS_OS_TIME_H

#include "os_kernel_state.h"

/* 返回当前系统 tick。 */
TickType_t xTaskGetTickCount(void);
/* 将当前任务加入延时队列并选择下一任务。 */
os_task_t *os_TimeDelayCurrent(uint32_t ticks);
/* 推进一个系统 tick，处理唤醒、超时和时间片轮转。 */
os_task_t *os_TimeTick(void);

#endif
