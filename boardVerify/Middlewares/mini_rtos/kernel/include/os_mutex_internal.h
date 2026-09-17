/**
 * @file os_mutex_internal.h
 * @author Peng RongYao
 * @date 2026-08-22
 * @brief 定义互斥量模块拥有的控制块并声明其内核接口。
 */

#ifndef MINI_RTOS_OS_MUTEX_INTERNAL_H
#define MINI_RTOS_OS_MUTEX_INTERNAL_H

#include "os.h"
#include "os_list.h"

/* 带所有者和优先级继承的非递归互斥量。 */
struct os_mutex {
    bool used;             /* 对象池槽位是否已经分配。 */
    os_task_t *owner;      /* 当前所有者，未加锁时为 NULL。 */
    os_list_t waiters;     /* 按有效优先级排列的加锁等待任务。 */
};

/* 复位静态互斥量对象池。 */
void os_MutexKernelReset(void);
/* 在串行内核上下文中获取互斥量并处理优先级继承。 */
void os_MutexLockCurrent(os_mutex_t *mutex, uint32_t timeout_ticks);
/* 在串行内核上下文中释放互斥量并恢复优先级。 */
void os_MutexUnlockCurrent(os_mutex_t *mutex);
/* 根据全部互斥量等待关系重新计算有效优先级。 */
void os_MutexWaitersChanged(void);
/* 任务终止时移交其持有的全部互斥量。 */
void os_MutexReleaseTask(os_task_t *task);

#endif
