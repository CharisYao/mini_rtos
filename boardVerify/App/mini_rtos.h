/**
 * @file mini_rtos.h
 * @brief boardVerify Phase C smoke entry (tasks + IPC).
 *
 * Charis: RTOS smoke lives in mini_rtos.c; main.c is HAL-only.
 */

#ifndef BOARDVERIFY_MINI_RTOS_H
#define BOARDVERIFY_MINI_RTOS_H

#ifdef __cplusplus
extern "C" {
#endif

/** Init IPC/tasks and call os_Start (does not return on success). */
void mini_rtos_app_start(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARDVERIFY_MINI_RTOS_H */
