/**
 * @file os_internal.h
 * @author Peng RongYao
 * @date 2026-07-17
 * @brief 定义通用内核数据结构以及 kernel 与 port 之间共用的内部接口。
 *
 * @details
 * 本文件在公开 API 之下描述 miniRTOS 的内部组织，包括完整 TCB、侵入式链表、
 * 信号量、消息队列、互斥量和全局内核状态。它还声明任务、调度、时间管理和
 * 内核对象模块之间需要互相调用的内部函数。
 *
 * kernel 目录、Win32 移植层和白盒测试可以包含本文件；普通任务和 demo 不应
 * 包含它，以免绕过公开 API 直接修改任务状态、调度链表或对象等待关系。
 */

#ifndef MINI_RTOS_OS_INTERNAL_H
#define MINI_RTOS_OS_INTERNAL_H

#include "os.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

/* TCB 内嵌的侵入式双向链表节点 */
typedef struct os_list_node {
    struct os_list_node *previous; /* 前一个节点，表头为 NULL。 */
    struct os_list_node *next;     /* 后一个节点，表尾为 NULL。 */
    os_task_t *owner;              /* 拥有该节点的 TCB。 */
    bool linked;                   /* 节点当前是否属于某一条内核链表。 */
} os_list_node_t;

/* 就绪、延时和对象等待队列共用的链表头 */
typedef struct {
    os_list_node_t *head; /* 表头节点。 */
    os_list_node_t *tail; /* 表尾节点。 */
    size_t size;          /* 当前节点数量。 */
} os_list_t;

/* BLOCKED_OBJECT 任务正在等待的内核对象类别 */
typedef enum {
    OS_WAIT_NONE = 0,         /* 未等待内核对象。 */
    OS_WAIT_SEMAPHORE,        /* 等待获取信号量。 */
    OS_WAIT_QUEUE_SEND,       /* 等待队列出现发送空间。 */
    OS_WAIT_QUEUE_RECEIVE,    /* 等待队列出现可接收消息。 */
    OS_WAIT_MUTEX             /* 等待互斥量所有权。 */
} os_wait_kind_t;

/* TCB任务控制块，保存调度所需的全部平台无关状态 */
struct os_task {
    uint8_t id;                        /* 静态 TCB 数组下标。 */
    char name[OS_TASK_NAME_MAX];       /* 用于调试和面板显示的任务名。 */
    os_task_entry_t entry;             /* 任务入口函数。 */
    void *argument;                    /* 传给任务入口的用户参数。 */
    uint8_t base_priority;             /* 创建时确定的基础优先级。 */
    uint8_t effective_priority;        /* 考虑互斥量继承后的调度优先级。 */
    os_task_state_t state;             /* 当前生命周期状态。 */
    uint32_t wake_tick;                /* BLOCKED_DELAY 的绝对唤醒 tick。 */
    uint32_t slice_remaining;          /* 当前时间片剩余 tick。 */
    os_wait_kind_t wait_kind;          /* 正在等待的对象操作类型。 */
    void *wait_object;                 /* 正在等待的信号量、队列或互斥量。 */
    void *wait_buffer;                 /* 阻塞式队列操作使用的消息缓冲区。 */
    os_list_t *wait_list;              /* 当前所在的对象等待队列。 */
    uint32_t wait_deadline;            /* 对象等待的绝对超时 tick。 */
    os_status_t wait_result;           /* 任务恢复后由公开 API 返回的结果。 */
    os_list_node_t schedule_node;      /* 挂入一条调度链表的唯一节点。 */
};

/* 静态计数信号量控制块 */
struct os_sem {
    bool used;                 /* 对象池槽位是否已经分配。 */
    uint32_t count;            /* 当前可获取计数。 */
    uint32_t maximum_count;    /* 计数允许达到的上限。 */
    os_list_t waiters;         /* 按有效优先级排列的等待任务。 */
};

/* 使用调用方缓冲区的固定容量环形消息队列 */
struct os_queue {
    bool used;                     /* 对象池槽位是否已经分配。 */
    uint8_t *buffer;               /* 调用方提供的连续消息存储区。 */
    size_t item_size;              /* 单条消息字节数。 */
    size_t capacity;               /* 最多容纳的消息数量。 */
    size_t head;                   /* 下一条出队消息的索引。 */
    size_t tail;                   /* 下一条入队消息的索引。 */
    size_t count;                  /* 当前已存消息数量。 */
    os_list_t send_waiters;        /* 队列满时阻塞的发送任务。 */
    os_list_t receive_waiters;     /* 队列空时阻塞的接收任务。 */
};

/* 带所有者和优先级继承的非递归互斥量 */
struct os_mutex {
    bool used;             /* 对象池槽位是否已经分配。 */
    os_task_t *owner;      /* 当前所有者，未加锁时为 NULL。 */
    os_list_t waiters;     /* 按有效优先级排列的加锁等待任务。 */
};

/* miniRTOS 唯一的全局内核状态 */
typedef struct {
    bool initialized;                          /* os_Init() 是否已经完成。 */
    bool started;                              /* 是否已经启动过一次调度器。 */
    os_task_t tasks[OS_MAX_TASKS];             /* 静态 TCB 池。 */
    size_t task_count;                         /* 已创建任务数量。 */
    os_list_t ready[OS_PRIORITY_COUNT];        /* 每个优先级一条 FIFO 就绪队列。 */
    os_list_t delayed;                         /* 等待 wake_tick 的任务集合。 */
    uint8_t ready_bitmap;                      /* 非空就绪队列的优先级位图。 */
    atomic_uint_least32_t tick;                /* 可由任务安全读取的系统 tick。 */
    os_task_t *current;                        /* 当前 RUNNING 任务。 */
    os_task_t *last_from;                      /* 最近一次切换的来源任务。 */
    os_task_t *last_to;                        /* 最近一次切换的目标任务。 */
    os_switch_reason_t last_reason;            /* 最近一次切换原因。 */
    uint32_t switch_count;                     /* 实际任务切换累计次数。 */
} os_kernel_t;

/* 全局内核实例，由 kernel/os_task.c 定义。 */
extern os_kernel_t g_os_kernel;

/* 将链表初始化为空表。 */
void os_ListInit(os_list_t *list);
/* 将未链接节点追加到表尾。 */
void os_ListPushBack(os_list_t *list, os_list_node_t *node);
/**
 * @brief 在指定节点之前插入一个未链接节点。
 * @param list 目标链表。
 * @param position 已属于 list 的定位节点。
 * @param node 待插入节点。
 */
void os_ListInsertBefore(
    os_list_t *list,
    os_list_node_t *position,
    os_list_node_t *node
);
/* 弹出并返回表头节点；空表返回 NULL。 */
os_list_node_t *os_ListPopFront(os_list_t *list);
/* 从链表移除指定节点。 */
void os_ListRemove(os_list_t *list, os_list_node_t *node);
/* 判断节点是否属于给定链表。 */
bool os_ListContains(const os_list_t *list, const os_list_node_t *node);

/* 将 READY 任务加入其有效优先级队列末尾。 */
void os_ReadyEnqueue(os_task_t *task);
/* 将任务从其有效优先级就绪队列移除。 */
bool os_ReadyRemove(os_task_t *task);
/* 弹出最高非空优先级队列的表头任务。 */
os_task_t *os_ReadyPopHighest(void);
/* 返回最高非空就绪优先级；没有 READY 任务时返回 -1。 */
int os_ReadyHighestPriority(void);

/* 启动调度并选择首个最高优先级任务。 */
os_task_t *os_SchedStart(void);
/**
 * @brief 根据当前任务与最高 READY 优先级决定是否切换。
 * @param reason 本次调度判断对应的触发原因。
 * @param rotate_equal 为 true 时允许与同优先级 READY 任务轮转。
 * @return 调度后应当运行的任务；无可运行任务时为 NULL。
 */
os_task_t *os_SchedSchedule(os_switch_reason_t reason, bool rotate_equal);
/* 将当前任务放回就绪队列并重新选择任务。 */
os_task_t *os_SchedYieldCurrent(void);
/* 当前任务已完成阻塞状态迁移后选择下一任务。 */
os_task_t *os_SchedCurrentBlocked(os_switch_reason_t reason);
/* 终止当前任务、释放其互斥量并选择下一任务。 */
os_task_t *os_SchedTerminateCurrent(void);

/**
 * @brief 将当前任务挂入一个按优先级排列的对象等待队列。
 * @param wait_list 目标对象的等待队列。
 * @param kind 等待操作类型。
 * @param object 正在等待的内核对象。
 * @param buffer 队列发送或接收使用的任务缓冲区。
 * @param timeout_ticks 有限等待 tick 或 OS_WAIT_FOREVER。
 * @return 当前任务阻塞后选出的下一任务。
 */
os_task_t *os_WaitBlockCurrent(
    os_list_t *wait_list,
    os_wait_kind_t kind,
    void *object,
    void *buffer,
    uint32_t timeout_ticks
);
/* 弹出对象等待队列中优先级最高且最早到达的任务。 */
os_task_t *os_WaitPop(os_list_t *wait_list);
/* 清除对象等待信息并将任务重新加入就绪队列。 */
void os_WaitMakeReady(os_task_t *task, os_status_t result);
/* 被唤醒任务优先级更高时立即执行一次抢占调度。 */
void os_WaitMaybePreempt(os_task_t *woken_task);
/* 修改任务有效优先级，并同步调整其所在调度队列。 */
void os_TaskSetEffectivePriority(os_task_t *task, uint8_t priority);

/* 将当前任务加入延时队列并选择下一任务。 */
os_task_t *os_TimeDelayCurrent(uint32_t ticks);
/* 推进一个系统 tick，处理唤醒、超时和时间片轮转。 */
os_task_t *os_TimeTick(void);

/* 复位静态信号量对象池。 */
void os_SemKernelReset(void);
/* 复位静态消息队列对象池。 */
void os_QueueKernelReset(void);
/* 在串行内核上下文中执行当前任务的信号量获取。 */
void os_SemTakeCurrent(os_sem_t *sem, uint32_t timeout_ticks);
/* 在串行内核上下文中执行当前任务的信号量释放。 */
void os_SemGiveCurrent(os_sem_t *sem);
/**
 * @brief 在串行内核上下文中执行当前任务的队列发送。
 * @param queue 目标队列。
 * @param item 待发送消息。
 * @param timeout_ticks 有限等待 tick 或 OS_WAIT_FOREVER。
 */
void os_QueueSendCurrent(
    os_queue_t *queue,
    const void *item,
    uint32_t timeout_ticks
);
/**
 * @brief 在串行内核上下文中执行当前任务的队列接收。
 * @param queue 目标队列。
 * @param out_item 接收消息的任务缓冲区。
 * @param timeout_ticks 有限等待 tick 或 OS_WAIT_FOREVER。
 */
void os_QueueReceiveCurrent(
    os_queue_t *queue,
    void *out_item,
    uint32_t timeout_ticks
);
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

/* 判断是否仍存在未终止任务。 */
bool os_KernelHasLiveTasks(void);
/* 宿主运行结束后清空调度容器并终止全部任务。 */
void os_KernelStopAll(void);
/* 检查任务状态、链表成员关系和就绪位图等内核不变量。 */
bool os_KernelValidate(void);

#endif
