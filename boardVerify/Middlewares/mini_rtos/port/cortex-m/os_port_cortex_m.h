/**
 * @file os_port_cortex_m.h
 * @brief Optional configuration for the Cortex-M3/M4 (no FPU) port.
 *
 * @details
 * Override these macros from the board CMake target (compile definitions) or
 * by providing SystemCoreClock from CMSIS startup/system files.
 *
 * Required vector-table hooks implemented by this port:
 *   - SysTick_Handler
 *   - PendSV_Handler
 *   - SVC_Handler (first-task start only)
 */

#ifndef MINI_RTOS_OS_PORT_CORTEX_M_H
#define MINI_RTOS_OS_PORT_CORTEX_M_H

#include "os.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SysTick reload uses SystemCoreClock / 1000 * OS_TICK_MS. */
#ifndef OS_PORT_CORTEX_M_DEFAULT_CORE_CLOCK_HZ
#define OS_PORT_CORTEX_M_DEFAULT_CORE_CLOCK_HZ 72000000u
#endif

/* Lowest configurable exception priority byte for PendSV. */
#ifndef OS_PORT_CORTEX_M_PENDSV_PRIORITY
#define OS_PORT_CORTEX_M_PENDSV_PRIORITY 0xFFu
#endif

/* CMSIS / board-provided core clock; weak default exists in the port .c. */
extern uint32_t SystemCoreClock;

void SysTick_Handler(void);
void PendSV_Handler(void);
void SVC_Handler(void);

#ifdef __cplusplus
}
#endif

#endif
