# miniRTOS 内核层阅读指南

## 1. 阅读目标与范围

这份文档只讲平台无关的 miniRTOS 内核，目标是看清下面四件事：

1. 内核用哪些结构体保存任务、队列和调度状态；
2. 每个 `kernel/*.c` 文件负责什么；
3. 任务如何在 `READY`、`RUNNING` 和阻塞状态之间迁移；
4. 调度、延时、信号量、消息队列和互斥量之间怎样互相调用。

阅读范围包括：

~~~text
include/os.h
kernel/os_internal.h
kernel/os_list.c
kernel/os_task.c
kernel/os_sched.c
kernel/os_time.c
kernel/os_sem.c
kernel/os_queue.c
kernel/os_mutex.c
~~~

`port/win32/os_port_win32.c` 在本文中只作为内核入口出现。Windows 线程、Event、gate、
TLS 和 `SuspendThread()` 不属于通用内核，不需要先理解它们才能阅读本指南。

## 2. 先建立内核整体图

当前 miniRTOS 可以分成四层：

~~~text
应用任务和 demo
        │
        │ 调用 os.h 公开 API
        ▼
port 层入口
        │
        │ 把请求串行送入内核
        ▼
通用 kernel
├─ os_task.c   创建和管理 TCB
├─ os_sched.c  READY 队列、等待队列和调度决策
├─ os_time.c   tick、延时、超时和时间片
├─ os_sem.c    信号量语义
├─ os_queue.c  消息队列语义
├─ os_mutex.c  互斥量和优先级继承
└─ os_list.c   上述队列共用的侵入式链表
~~~

最重要的边界是：

> kernel 只决定任务状态和“下一任务是谁”，不负责保存、暂停或恢复真实 CPU 现场。

在 Windows 版本中，port 根据 `g_os_kernel.current` 控制工作线程；如果以后移植到 STM32，
则由 SysTick、PendSV 和任务栈完成同样的执行工作。TCB、就绪队列和调度规则仍属于 kernel。

## 3. 两个头文件分别解决什么问题

### 3.1 `include/os.h`：公开接口

[`include/os.h`](../include/os.h) 是任务和 demo 唯一应该直接包含的内核头文件。

它提供：

- 编译期容量和时间配置；
- 对应用隐藏内部字段的不透明对象类型；
- 公共枚举和运行快照；
- 任务、时间、信号量、队列和互斥量 API。

应用只能看到：

~~~c
typedef struct os_task os_task_t;
typedef struct os_sem os_sem_t;
typedef struct os_queue os_queue_t;
typedef struct os_mutex os_mutex_t;
~~~

这表示应用可以保存 `os_task_t *`，但不能直接写 `task->state` 或修改调度链表。

### 3.2 `kernel/os_internal.h`：内核内部定义

[`kernel/os_internal.h`](../kernel/os_internal.h) 展开了所有不透明类型，并声明 kernel
各文件之间需要互相调用的内部接口。

可以包含它的代码是：

- `kernel/*.c`；
- Win32 port；
- `tests/test_kernel.c` 白盒测试。

普通任务不应包含它，否则可以绕过调度规则直接破坏 TCB、ready 队列和等待关系。

## 4. `os.h` 中的核心配置

| 配置 | 当前值 | 内核意义 |
|---|---:|---|
| `OS_MAX_TASKS` | 8 | 静态 TCB 池最多容纳 8 个任务 |
| `OS_PRIORITY_COUNT` | 8 | 优先级编号范围为 0～7 |
| `OS_IDLE_PRIORITY` | 0 | 保留给 Idle 语义，普通任务不能使用 |
| `OS_TICK_MS` | 10 ms | 一个 tick 的标称 Windows 时间 |
| `OS_TIME_SLICE_TICKS` | 20 | 同优先级任务一个时间片为 20 tick |
| `OS_MAX_DELAY_TICKS` | `0x7FFFFFFF` | 保证 tick 回绕比较安全的最大有限等待 |
| `OS_WAIT_FOREVER` | `UINT32_MAX` | 永久等待，不设置超时截止 tick |
| `OS_MAX_SEMAPHORES` | 4 | 静态信号量控制块数量 |
| `OS_MAX_QUEUES` | 4 | 静态消息队列控制块数量 |
| `OS_MAX_MUTEXES` | 4 | 静态互斥量控制块数量 |

当前没有真正创建一个 Idle TCB。没有 READY 任务时，`g_os_kernel.current` 可以为 `NULL`，
Windows 宿主控制线程等待下一个 tick 或任务请求，这就是当前版本的空闲状态。

## 5. 公共枚举的意义

### 5.1 `os_status_t`：一次 API 操作的结果

| 枚举值 | 意义 |
|---|---|
| `OS_STATUS_OK` | 操作成功 |
| `OS_STATUS_INVALID_ARGUMENT` | 指针、数值或参数组合无效 |
| `OS_STATUS_LIMIT_REACHED` | 静态对象池已满，或计数已达到上限 |
| `OS_STATUS_BAD_STATE` | 当前内核状态或任务状态不允许该操作 |
| `OS_STATUS_TIMEOUT` | 有限等待到期，或零超时操作无法立即完成 |
| `OS_STATUS_NOT_OWNER` | 非互斥量所有者尝试解锁 |

对可能阻塞的 API，结果先保存在 TCB 的 `wait_result` 中。任务以后被唤醒并重新运行时，
port 再把这个结果返回给 `os_SemTake()`、`os_QueueReceive()` 等公开 API。

### 5.2 `os_task_state_t`：任务当前处于哪里

| 状态 | 是否可以运行 | 所在位置 |
|---|---|---|
| `OS_TASK_UNUSED` | 否 | 尚未分配的 TCB 槽位 |
| `OS_TASK_READY` | 是 | 对应有效优先级的 ready 队列 |
| `OS_TASK_RUNNING` | 正在运行 | `g_os_kernel.current`，不在任何调度链表中 |
| `OS_TASK_BLOCKED_DELAY` | 否 | `g_os_kernel.delayed` 延时队列 |
| `OS_TASK_BLOCKED_OBJECT` | 否 | 某个信号量、队列或互斥量的等待队列 |
| `OS_TASK_TERMINATED` | 否 | 已退出，不在调度链表中 |

典型状态迁移如下。每一行都按照“原状态 → 触发条件 → 新状态”阅读：

~~~text
UNUSED          -- os_TaskCreate() ----------> READY
READY           -- 被调度器选中 -------------> RUNNING
RUNNING         -- 被抢占、yield 或时间片到期 -> READY
RUNNING         -- os_Delay() ----------------> BLOCKED_DELAY
BLOCKED_DELAY   -- 延时到期 ------------------> READY
RUNNING         -- 等待信号量、队列或互斥量 --> BLOCKED_OBJECT
BLOCKED_OBJECT  -- 对象可用或等待超时 --------> READY
RUNNING         -- 任务入口函数返回 ----------> TERMINATED
~~~

### 5.3 `os_switch_reason_t`：为什么发生了切换

这个枚举不决定任务状态，它用于记录最近一次调度切换的原因，方便面板和测试观察。

| 枚举值 | 触发场景 |
|---|---|
| `OS_SWITCH_START` | 调度器启动并选择首任务 |
| `OS_SWITCH_TIME_SLICE` | 同优先级时间片轮转 |
| `OS_SWITCH_YIELD` | 当前任务主动让出 |
| `OS_SWITCH_HIGHER_PRIORITY_WAKEUP` | 更高优先级任务恢复 READY |
| `OS_SWITCH_BLOCKED` | 当前任务进入延时或对象阻塞 |
| `OS_SWITCH_EXIT` | 当前任务入口返回 |

如果调度后仍是同一个任务，`os_CommitSwitch()`不会增加 `switch_count`，因为没有发生真实
任务切换。

### 5.4 `os_wait_kind_t`：任务在等什么对象操作

`os_wait_kind_t` 是内部枚举，只在任务状态为 `OS_TASK_BLOCKED_OBJECT` 时有意义。

| 枚举值 | 等待原因 |
|---|---|
| `OS_WAIT_NONE` | 当前没有等待对象 |
| `OS_WAIT_SEMAPHORE` | 等待信号量计数 |
| `OS_WAIT_QUEUE_SEND` | 队列满，等待发送空间 |
| `OS_WAIT_QUEUE_RECEIVE` | 队列空，等待消息 |
| `OS_WAIT_MUTEX` | 等待互斥量所有权 |

只看 `BLOCKED_OBJECT` 无法区分具体原因，所以 TCB 还需要 `wait_kind`、`wait_object`、
`wait_list` 和 `wait_buffer`。

## 6. 核心结构体

### 6.1 `os_list_node_t`：嵌入 TCB 的链表节点

~~~c
typedef struct os_list_node {
    struct os_list_node *previous;
    struct os_list_node *next;
    os_task_t *owner;
    bool linked;
} os_list_node_t;
~~~

关键设计是“侵入式链表”：节点不是运行时 `malloc` 出来的，而是直接放在 TCB 中。

`owner` 可以从链表节点反查对应任务，`linked` 防止同一个节点被重复插入。

每个 TCB 只有一个 `schedule_node`，因此一个任务同一时刻只能位于一种调度容器：

- 一个 ready 队列；
- 延时队列；
- 一个对象等待队列；
- 或不在任何链表中，此时通常是 RUNNING、UNUSED 或 TERMINATED。

### 6.2 `os_list_t`：调度链表头

~~~c
typedef struct {
    os_list_node_t *head;
    os_list_node_t *tail;
    size_t size;
} os_list_t;
~~~

`head` 用于取出队首任务，`tail` 用于 FIFO 追加，`size` 用于校验和快速判断空表。

### 6.3 `struct os_task`：TCB 任务控制块

TCB 是整个内核最重要的结构，可以按职责分成五组字段。

#### 身份和入口

| 字段 | 意义 |
|---|---|
| `id` | 静态 TCB 数组下标，也用于从任务映射到平台槽位 |
| `name` | 调试和控制台显示名称 |
| `entry` | 任务入口函数 |
| `argument` | 传给任务入口的用户参数 |

#### 优先级和状态

| 字段 | 意义 |
|---|---|
| `base_priority` | 创建任务时确定的基础优先级 |
| `effective_priority` | 考虑互斥量优先级继承后的实际调度优先级 |
| `state` | 当前任务状态 |

调度器永远使用 `effective_priority`。没有优先级继承时，它等于 `base_priority`。

#### 时间信息

| 字段 | 意义 |
|---|---|
| `wake_tick` | `BLOCKED_DELAY` 任务的绝对唤醒 tick |
| `slice_remaining` | 当前时间片剩余 tick |

#### 对象等待信息

| 字段 | 意义 |
|---|---|
| `wait_kind` | 正在执行哪种对象等待 |
| `wait_object` | 正在等待哪个信号量、队列或互斥量 |
| `wait_buffer` | 阻塞式队列发送或接收使用的消息缓冲区地址 |
| `wait_list` | 当前所在的对象等待队列 |
| `wait_deadline` | 绝对超时 tick，或 `OS_WAIT_FOREVER` |
| `wait_result` | 任务恢复后公开 API 应返回的结果 |

#### 调度链表节点

| 字段 | 意义 |
|---|---|
| `schedule_node` | 把任务挂入 ready、delayed 或对象等待队列的唯一节点 |

### 6.4 `struct os_sem`：计数信号量

~~~c
struct os_sem {
    bool used;
    uint32_t count;
    uint32_t maximum_count;
    os_list_t waiters;
};
~~~

- `used` 表示静态对象池槽位是否已经分配；
- `count` 是当前可以获取的资源数量；
- `maximum_count` 限制释放后的最大值；
- `waiters` 保存因计数为零而阻塞的任务，按有效优先级降序排列。

### 6.5 `struct os_queue`：固定容量环形消息队列

~~~c
struct os_queue {
    bool used;
    uint8_t *buffer;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    os_list_t send_waiters;
    os_list_t receive_waiters;
};
~~~

`buffer` 由调用者在启动前提供，队列控制块只记录地址，不拥有也不释放这块内存。

- `head` 指向下一条出队消息；
- `tail` 指向下一条入队位置；
- `count` 区分空队列和满队列；
- `send_waiters` 保存队列满时阻塞的发送者；
- `receive_waiters` 保存队列空时阻塞的接收者。

### 6.6 `struct os_mutex`：带所有者的互斥量

~~~c
struct os_mutex {
    bool used;
    os_task_t *owner;
    os_list_t waiters;
};
~~~

互斥量与二值信号量的关键区别是 `owner`。只有所有者可以解锁，等待关系还会参与
优先级继承计算。当前互斥量不可递归获取。

### 6.7 `os_kernel_t`：全局内核状态

项目中只有一个实例：

~~~c
os_kernel_t g_os_kernel;
~~~

核心字段如下：

| 字段 | 意义 |
|---|---|
| `initialized` | 是否已经执行 `os_Init()` |
| `started` | 是否已经启动过调度器 |
| `tasks` | 固定 TCB 池 |
| `task_count` | 已创建任务数量 |
| `ready[]` | 每个优先级一条 FIFO ready 队列 |
| `delayed` | 所有 `BLOCKED_DELAY` 任务组成的延时队列 |
| `ready_bitmap` | 哪些优先级的 ready 队列非空 |
| `tick` | 原子系统 tick |
| `current` | 当前 `RUNNING` 任务，空闲时为 `NULL` |
| `last_from`、`last_to` | 最近一次真实切换的来源和目标 |
| `last_reason` | 最近一次切换原因 |
| `switch_count` | 真实任务切换累计次数 |

## 7. `kernel/os_list.c`：所有调度队列的基础

### 7.1 文件职责

[`kernel/os_list.c`](../kernel/os_list.c) 只实现链表结构，不理解优先级、任务状态、信号量
或消息内容。上层调度代码决定“为什么插入”，链表只完成“怎样连接节点”。

### 7.2 关键函数

| 函数 | 作用 |
|---|---|
| `os_ListInit()` | 清空链表头、尾和数量 |
| `os_ListPushBack()` | 把未链接节点追加到表尾 |
| `os_ListInsertBefore()` | 在指定节点之前插入 |
| `os_ListPopFront()` | 弹出表头节点 |
| `os_ListRemove()` | 删除指定节点并修复前后关系 |
| `os_ListContains()` | 受限遍历，确认节点确实属于链表 |

### 7.3 谁调用它

~~~text
os_task.c   -> 初始化 ready 和 delayed 队列
os_sched.c  -> 管理 ready 队列和对象等待队列
os_time.c   -> 管理 delayed 队列和超时摘除
os_sem.c    -> 初始化信号量等待队列
os_queue.c  -> 初始化发送、接收等待队列
os_mutex.c  -> 初始化互斥量等待队列
~~~

### 7.4 阅读重点

`os_ListPushBack()` 与 `os_ListPopFront()`组合出 FIFO。`os_WaitInsertByPriority()`则使用
`os_ListInsertBefore()`把高优先级等待者插到前面，同优先级仍追加在已有同级任务之后。

## 8. `kernel/os_task.c`：初始化和创建 TCB

### 8.1 文件职责

[`kernel/os_task.c`](../kernel/os_task.c) 定义 `g_os_kernel`，负责把一块全零静态内存
初始化成合法内核，并在启动前从固定 TCB 数组创建任务。

### 8.2 `os_Init()` 调用链

~~~text
os_Init()
├─ memset(g_os_kernel)
├─ os_ListInit(每个 ready 队列)
├─ os_ListInit(delayed)
├─ atomic_init(tick, 0)
├─ os_SemKernelReset()
├─ os_QueueKernelReset()
├─ os_MutexKernelReset()
├─ 初始化每个 TCB 的 id、UNUSED 状态和节点 owner
└─ initialized = true
~~~

这一步只准备平台无关结构，不创建 Windows 线程，也不启动 tick。

### 8.3 `os_TaskCreate()` 调用链

~~~text
os_TaskCreate(...)
├─ 检查已经 os_Init() 且尚未启动
├─ 检查名称、入口、优先级和 TCB 容量
├─ 取 tasks[task_count]
├─ 填写入口、参数、基础/有效优先级
├─ state = READY
├─ 清空时间和对象等待字段
├─ os_ReadyEnqueue(task)
└─ task_count++
~~~

新任务创建后已经进入 ready 队列，但还没有运行。真正的首任务由 `os_SchedStart()`选择。

### 8.4 只读查询函数

`os_TaskGetName()`、`os_TaskGetState()`、`os_TaskGetPriority()`只读取 TCB。
`os_TaskStateName()`和 `os_SwitchReasonName()`把枚举转换为控制台文本，不参与调度。

## 9. `kernel/os_sched.c`：内核调度中心

### 9.1 文件职责

[`kernel/os_sched.c`](../kernel/os_sched.c) 是最核心的文件，负责：

- 每个优先级的 ready 队列；
- 最高优先级任务选择；
- 启动、抢占、yield、阻塞和退出后的重新调度；
- 对象等待队列的统一阻塞和唤醒；
- 有效优先级改变后的重新排队；
- 运行快照、停止清理和内核不变量检查。

### 9.2 ready 队列与位图

`ready[priority]` 是该优先级的 FIFO 队列。`ready_bitmap` 中每一位表示对应 ready 队列
是否非空。

例如：

~~~text
ready[5]: HighA -> HighB
ready[3]: Medium
ready[1]: LowA -> LowB

ready_bitmap = 0b00101010
~~~

`os_ReadyHighestPriority()`从 7 向 0 扫描位图，首先发现 5；
`os_ReadyPopHighest()`再从 `ready[5]` 表头取出 `HighA`。

### 9.3 `os_CommitSwitch()`：统一提交调度结果

所有主要调度路径最终进入这个静态函数。它负责：

1. 把 `next` 改为 `RUNNING`；
2. 为 `next` 重新装载完整时间片；
3. 更新 `g_os_kernel.current`；
4. 如果任务确实发生变化，记录来源、目标、原因和切换次数。

它只提交内核状态，不执行 Windows 线程切换。

### 9.4 首次启动调用链

~~~text
port: os_Start()
└─ kernel: os_SchedStart()
   ├─ os_ReadyPopHighest()
   │  ├─ os_ReadyHighestPriority()
   │  └─ os_ListPopFront()
   └─ os_CommitSwitch(NULL, next, OS_SWITCH_START)
~~~

### 9.5 通用抢占判断

`os_SchedSchedule(reason, rotate_equal)`遵守两条规则：

1. ready 队列中存在更高优先级任务时，无条件抢占当前任务；
2. 最高 READY 优先级与当前任务相同时，只有 `rotate_equal == true` 才轮转。

抢占当前任务时：

~~~text
当前 RUNNING
  -> state = READY
  -> 追加到本优先级队尾
  -> 弹出最高优先级队首
  -> os_CommitSwitch()
~~~

### 9.6 yield、阻塞和退出是三条不同路径

| 函数 | 当前任务怎样处理 | 切换原因 |
|---|---|---|
| `os_SchedYieldCurrent()` | 改为 READY 并放回队尾 | `OS_SWITCH_YIELD` |
| `os_SchedCurrentBlocked()` | 当前任务已经由调用者改成阻塞状态，不放回 ready | `OS_SWITCH_BLOCKED` 等 |
| `os_SchedTerminateCurrent()` | 释放持有互斥量，改为 TERMINATED | `OS_SWITCH_EXIT` |

### 9.7 对象等待的统一入口

信号量、队列和互斥量不各自重复编写阻塞逻辑，而是调用：

~~~c
os_WaitBlockCurrent(
    wait_list,
    wait_kind,
    object,
    buffer,
    timeout_ticks
);
~~~

它统一完成：

~~~text
填写 wait_kind / wait_object / wait_buffer / wait_list
        ↓
计算 wait_deadline
        ↓
state = BLOCKED_OBJECT
        ↓
按有效优先级插入对象等待队列
        ↓
os_SchedCurrentBlocked()
~~~

### 9.8 对象唤醒的统一出口

对象模块通常按下面的顺序唤醒任务：

~~~text
os_WaitPop(wait_list)
        ↓
对象模块完成资源交付
        ↓
os_WaitMakeReady(task, result)
        ↓
清空等待字段、state = READY、进入 ready 队列
        ↓
os_WaitMaybePreempt(task)
~~~

### 9.9 有效优先级变化为什么要重新排队

`os_TaskSetEffectivePriority()`不能只修改一个数字：

- READY 任务所在 ready 队列由优先级决定，所以必须从旧队列移到新队列；
- BLOCKED_OBJECT 任务的等待队列按有效优先级排序，也必须删除后重新插入；
- RUNNING 或 BLOCKED_DELAY 任务没有位于优先级排序队列中，可以直接修改字段。

### 9.10 `os_KernelValidate()` 检查的核心不变量

- ready 位图必须与各 ready 队列是否为空一致；
- READY 任务必须且只能出现在一个正确优先级 ready 队列中；
- BLOCKED_DELAY 任务必须且只能出现在 delayed 队列中；
- BLOCKED_OBJECT 任务必须在自己的对象等待队列中；
- RUNNING 任务必须等于 `current`，且不能链接在任何调度链表中；
- 最多只能存在一个 RUNNING 任务；
- `effective_priority` 不能低于 `base_priority`；
- 节点的 `owner` 必须指回正确 TCB。

这个函数主要服务于测试和 Win32 port 的防御性检查，不会自动修复错误状态。

## 10. `kernel/os_time.c`：tick、延时和时间片

### 10.1 文件职责

[`kernel/os_time.c`](../kernel/os_time.c) 负责所有与时间相关的内核状态变化：

- 当前系统 tick；
- `os_Delay()` 对应的延时阻塞；
- 延时到期唤醒；
- 信号量、队列和互斥量等待超时；
- 高优先级到期任务抢占；
- 同优先级时间片轮转。

### 10.2 `os_Delay()` 的内核调用链

~~~text
公开 API: os_Delay(ticks)
        ↓ port 将请求送入内核
os_TimeDelayCurrent(ticks)
├─ ticks == 0 -> os_SchedYieldCurrent()
├─ wake_tick = current_tick + ticks
├─ state = BLOCKED_DELAY
├─ os_ListPushBack(delayed, current)
└─ os_SchedCurrentBlocked(OS_SWITCH_BLOCKED)
~~~

延时不是 Windows `Sleep()`，而是把任务从 CPU 竞争中移除。

### 10.3 `os_TimeTick()` 的处理顺序

每次 tick 严格按下面顺序执行：

~~~text
系统 tick 加一
    ↓
扫描 delayed，唤醒到期的 BLOCKED_DELAY 任务
    ↓
扫描固定 TCB 池，处理 BLOCKED_OBJECT 超时
    ↓
若互斥量等待超时，重新计算优先级继承
    ↓
扣减当前 RUNNING 任务时间片
    ↓
只执行一次最终调度判断
~~~

最终调度优先级是：

1. 当前没有任务但已有 READY 任务；
2. 唤醒了比当前任务优先级更高的任务；
3. 当前任务时间片耗尽，需要允许同级轮转；
4. 否则继续运行当前任务。

### 10.4 tick 回绕

`uint32_t` 最终会从 `UINT32_MAX` 回到 0。内核使用：

~~~c
(int32_t)(now - target) >= 0
~~~

判断截止时间是否到达，同时把最大有限等待限制为 `0x7FFFFFFF`，保证差值不会跨越
超过半个计数空间。

### 10.5 当前实现的简化

- delayed 队列没有按到期时间排序，每个 tick 扫描全部延时任务；
- 对象超时没有单独超时链表，每个 tick 扫描固定 TCB 池；
- 任务最多 8 个，因此这种 O(n) 教学实现简单且可控。

## 11. `kernel/os_sem.c`：计数信号量

### 11.1 文件职责

[`kernel/os_sem.c`](../kernel/os_sem.c) 从固定对象池分配信号量，并实现计数获取、阻塞、
直接唤醒和最大计数限制。

### 11.2 初始化调用链

~~~text
os_Init()
└─ os_SemKernelReset()
   └─ 清空对象池并初始化每个 waiters 链表

应用初始化
└─ os_SemInit(initial_count, maximum_count)
   └─ 找到第一个 used == false 的槽位
~~~

信号量只能在调度器启动前创建。

### 11.3 获取信号量

~~~text
os_SemTake()
        ↓ port 请求
os_SemTakeCurrent()
├─ count > 0
│  └─ count--，结果 OK
├─ count == 0 且 timeout == 0
│  └─ 结果 TIMEOUT，不阻塞
└─ count == 0 且允许等待
   └─ os_WaitBlockCurrent(sem->waiters, OS_WAIT_SEMAPHORE, ...)
~~~

### 11.4 释放信号量

~~~text
os_SemGive()
        ↓ port 请求
os_SemGiveCurrent()
├─ waiters 非空
│  ├─ os_WaitPop()
│  ├─ 直接把本次资源交给等待者，count 仍为 0
│  ├─ os_WaitMakeReady()
│  └─ os_WaitMaybePreempt()
└─ waiters 为空
   ├─ count < maximum_count -> count++
   └─ 已到上限 -> LIMIT_REACHED
~~~

“直接交付”避免先 `count++` 再让另一个任务竞争，保证已经等待的高优先级任务优先获得资源。

## 12. `kernel/os_queue.c`：固定容量消息队列

### 12.1 文件职责

[`kernel/os_queue.c`](../kernel/os_queue.c) 管理静态队列控制块和调用者提供的环形缓冲区，
实现立即收发、满/空阻塞、直接消息交付和快照查询。

### 12.2 初始化

~~~text
os_QueueInit(out_queue, buffer, item_size, capacity)
├─ 检查 buffer、元素大小、容量
├─ 检查 item_size * capacity 不发生 size_t 溢出
├─ 分配一个静态队列控制块
├─ head = tail = count = 0
└─ 初始化 send_waiters 和 receive_waiters
~~~

内核不分配消息缓冲区，也不会在结束时释放它。

### 12.3 普通环形缓冲区路径

发送时：

~~~text
写入 buffer[tail]
tail = (tail + 1) % capacity
count++
~~~

接收时：

~~~text
读取 buffer[head]
head = (head + 1) % capacity
count--
~~~

### 12.4 已有阻塞接收者时的直接交付

~~~text
发送任务调用 os_QueueSendCurrent()
        ↓
receive_waiters 非空
        ↓
取出最高优先级接收者
        ↓
直接 memcpy 到 receiver->wait_buffer
        ↓
接收者 READY，必要时立即抢占
~~~

消息不需要先进入环形缓冲区，所以 `queue->count` 保持不变。

### 12.5 队列满时的发送阻塞

~~~text
count == capacity
        ↓
timeout == 0 -> TIMEOUT
        ↓ 否则
发送任务进入 send_waiters
wait_buffer 保存 item 地址
~~~

以后接收者取走一条消息，刚释放的槽位会立即接收最高优先级阻塞发送者的数据，再将
发送者恢复 READY。因此接收一次后，队列可能仍然保持满状态，但等待发送者已经减少。

### 12.6 队列空时的接收阻塞

~~~text
count == 0
        ↓
timeout == 0 -> TIMEOUT
        ↓ 否则
接收任务进入 receive_waiters
wait_buffer 保存 out_item 地址
~~~

阻塞期间发送和接收缓冲区地址保存在 TCB 中，所以调用者必须保证对应内存一直有效，
直到 API 返回。任务自己的栈在阻塞期间不会消失，因此任务局部变量可以满足这个条件，
但不能提前离开其作用域或复用该内存。

### 12.7 查询接口

`os_QueueGetCount()`和 `os_QueueGetCapacity()`读取队列状态；`os_QueueSnapshot()`按逻辑
出队顺序复制消息，但不修改 `head`、`tail` 或 `count`，主要供控制台观察和测试使用。

## 13. `kernel/os_mutex.c`：所有权与优先级继承

### 13.1 文件职责

[`kernel/os_mutex.c`](../kernel/os_mutex.c) 实现非递归互斥量、所有者检查、等待者移交、
任务退出清理以及链式优先级继承。

### 13.2 加锁路径

~~~text
os_MutexLock()
        ↓ port 请求
os_MutexLockCurrent()
├─ owner == NULL
│  └─ owner = current，结果 OK
├─ owner == current
│  └─ BAD_STATE，当前互斥量不可递归
├─ 已被其他任务持有且 timeout == 0
│  └─ TIMEOUT
└─ 已被其他任务持有且允许等待
   ├─ os_WaitBlockCurrent(..., OS_WAIT_MUTEX, ...)
   ├─ os_MutexWaitersChanged()
   └─ 所有者提升后必要时重新调度
~~~

### 13.3 基础优先级和有效优先级

假设：

~~~text
Low    基础优先级 P1，持有 mutex
Medium 基础优先级 P3，一直 READY
High   基础优先级 P5，等待 mutex
~~~

如果 Low 保持 P1，Medium 会一直压住 Low，High 就无法获得锁。优先级继承把 Low 的
`effective_priority` 临时提升为 P5，使 Low 越过 Medium，尽快运行并释放互斥量。

### 13.4 `os_MutexWaitersChanged()` 为什么从头重算

每次等待关系改变时，它先把每个任务的目标优先级重置为 `base_priority`，再沿全部互斥量
等待关系传播高优先级，最多迭代 `OS_MAX_TASKS` 轮直到稳定。

这不仅支持直接继承，还支持链式继承：

~~~text
High(P5) 等待 M2，M2 由 Medium(P3) 持有
Medium(P3) 又等待 M1，M1 由 Low(P1) 持有

第一轮：Medium 继承 P5
下一轮：Low 再从 Medium 继承 P5
~~~

从基础优先级重新计算也保证等待超时或解锁后，任务可以正确降低回原优先级。

### 13.5 解锁路径

~~~text
os_MutexUnlockCurrent()
├─ 检查 current 是 owner
├─ os_WaitPop() 取最高优先级等待者
├─ owner = waiter，直接移交所有权
├─ os_MutexWaitersChanged() 重算继承
└─ waiter 存在时
   ├─ os_WaitMakeReady(waiter, OK)
   └─ os_WaitMaybePreempt(waiter)
~~~

### 13.6 任务入口返回时的清理

`os_SchedTerminateCurrent()`在把任务改为 TERMINATED 前调用 `os_MutexReleaseTask()`。
后者检查全部互斥量，把该任务仍持有的锁移交给等待者，防止任务退出后永久占锁。

## 14. port 公开接口与 kernel 内部函数的对应关系

为了保持内核状态串行修改，当前 Windows 版本把一部分公开 API 放在 port 中作为请求外壳。
阅读内核时只需要知道下面的对应关系：

| 任务调用的公开 API | 进入内核后的函数 |
|---|---|
| `os_Yield()` | `os_SchedYieldCurrent()` |
| `os_Delay(ticks)` | `os_TimeDelayCurrent(ticks)` |
| 周期 tick | `os_TimeTick()` |
| `os_SemTake()` | `os_SemTakeCurrent()` |
| `os_SemGive()` | `os_SemGiveCurrent()` |
| `os_QueueSend()` | `os_QueueSendCurrent()` |
| `os_QueueReceive()` | `os_QueueReceiveCurrent()` |
| `os_MutexLock()` | `os_MutexLockCurrent()` |
| `os_MutexUnlock()` | `os_MutexUnlockCurrent()` |
| 任务入口返回 | `os_SchedTerminateCurrent()` |

`Current` 后缀表示：该函数操作的对象是 `g_os_kernel.current` 指向的当前任务，而且只能在
串行内核上下文中调用。它不是给普通任务直接调用的 API。

## 15. 六条完整调用链

### 15.1 初始化并启动首任务

~~~text
main()
├─ os_Init()
├─ os_TaskCreate(TaskA) -> os_ReadyEnqueue(TaskA)
├─ os_TaskCreate(TaskB) -> os_ReadyEnqueue(TaskB)
├─ os_SemInit() / os_QueueInit() / os_MutexInit()
└─ os_Start()
   └─ os_SchedStart()
      └─ os_ReadyPopHighest()
         └─ os_CommitSwitch(NULL, highest, START)
~~~

### 15.2 tick 触发同优先级轮转

~~~text
周期 tick
└─ os_TimeTick()
   ├─ current->slice_remaining--
   └─ 时间片耗尽
      └─ os_SchedSchedule(TIME_SLICE, true)
         ├─ current -> READY，追加到同级队尾
         ├─ 弹出同级队首任务
         └─ os_CommitSwitch()
~~~

### 15.3 高优先级延时任务到期抢占

~~~text
High 调用 os_Delay(5)
└─ os_TimeDelayCurrent(5)
   ├─ High -> BLOCKED_DELAY
   └─ Low 成为 current

第 5 个 tick
└─ os_TimeTick()
   ├─ High 从 delayed 移除
   ├─ High -> READY
   ├─ os_ReadyEnqueue(High)
   └─ os_SchedSchedule(HIGHER_PRIORITY_WAKEUP, false)
      ├─ Low -> READY
      └─ High -> RUNNING
~~~

### 15.4 信号量唤醒

~~~text
High: os_SemTake(sem, FOREVER)
└─ os_SemTakeCurrent()
   └─ os_WaitBlockCurrent() -> High BLOCKED_OBJECT

Low: os_SemGive(sem)
└─ os_SemGiveCurrent()
   ├─ os_WaitPop() -> High
   ├─ os_WaitMakeReady(High, OK)
   └─ os_WaitMaybePreempt(High)
      └─ High 抢占 Low
~~~

### 15.5 队列空等待与直接交付

~~~text
Receiver: os_QueueReceive(queue, &value, FOREVER)
└─ queue 空
   └─ Receiver 进入 receive_waiters，保存 &value

Sender: os_QueueSend(queue, &sample, FOREVER)
└─ 找到 Receiver
   ├─ memcpy(&value, &sample, item_size)
   ├─ Receiver -> READY
   └─ 必要时抢占 Sender
~~~

### 15.6 互斥量优先级继承

~~~text
Low(P1) 获得 mutex
        ↓
High(P5) 请求 mutex
        ↓
High 进入 mutex->waiters
        ↓
os_MutexWaitersChanged()
        ↓
Low effective_priority: P1 -> P5
        ↓
Low 越过 Medium(P3) 运行并解锁
        ↓
mutex 所有权直接交给 High
        ↓
Low 恢复 P1，High READY 并抢占
~~~

## 16. 推荐阅读顺序

### 第一遍：只看数据，不追调用

1. `include/os.h` 中四个配置组和三个公共枚举；
2. `os_internal.h` 中 `os_task` 和 `os_kernel_t`；
3. 对照任务状态迁移图理解一个任务可能位于哪里。

### 第二遍：只看最小调度主线

1. `os_Init()`；
2. `os_TaskCreate()`；
3. `os_ReadyEnqueue()`；
4. `os_SchedStart()`；
5. `os_SchedSchedule()`；
6. `os_CommitSwitch()`。

读完这一遍，应能回答“最高优先级任务怎样成为 current”。

### 第三遍：加入时间

1. `os_TimeDelayCurrent()`；
2. `os_TimeTick()`；
3. `os_SchedCurrentBlocked()`；
4. 时间片耗尽时的 `os_SchedSchedule(..., true)`。

读完这一遍，应能解释延时唤醒和同优先级轮转。

### 第四遍：加入统一对象等待

先读 `os_WaitBlockCurrent()`、`os_WaitMakeReady()`和 `os_WaitMaybePreempt()`，再读
`os_sem.c`。信号量路径最短，适合理解对象阻塞框架。

### 第五遍：阅读队列

重点比较三条路径：

- 正常写入环形缓冲区；
- 直接交付给阻塞接收者；
- 满队列发送者在接收释放槽位后立即补入消息。

### 第六遍：最后阅读互斥量

先理解 `base_priority` 和 `effective_priority`，再看 `os_MutexWaitersChanged()`的多轮传播。
互斥量建立在任务等待、优先级排序和调度重新排队都已经理解的基础上。

## 17. 阅读时容易混淆的边界

### 17.1 kernel 调度器不是 Windows 线程

`os_sched.c` 是调度算法和数据结构，不是一个单独线程。Win32 宿主控制线程调用它，
STM32 port 也可以在内核 API、SysTick 或 PendSV 前后调用同类逻辑。

### 17.2 `RUNNING` 任务不在 ready 队列

ready 表示“可以运行但尚未获得 CPU”。任务被选中后从 ready 队列弹出，再改为 RUNNING。

### 17.3 阻塞不是线程睡眠语义本身

kernel 所谓阻塞，是 TCB 改成阻塞状态并离开 ready 队列。Windows gate 或 STM32 上下文
切换只是 port 层落实这个决定的方式。

### 17.4 信号量与互斥量不同

信号量没有所有者，只管理计数；互斥量记录所有者，限制解锁者，并参与优先级继承。

### 17.5 `base_priority` 不直接代表当前调度优先级

正常情况下两者相同；持锁任务发生优先级继承后，调度器使用更高的
`effective_priority`。

### 17.6 当前实现不支持的内容

- 调度器启动后创建任务；
- 任务删除 API；
- 真正的 Idle Task；
- 动态内存对象；
- ISR 专用 API；
- 多核并行；
- 硬实时响应保证。

这些是当前教学边界，不影响理解 TCB、优先级调度、阻塞唤醒和内核对象的主干机制。

## 18. 如何使用测试辅助阅读

[`tests/test_kernel.c`](../tests/test_kernel.c) 直接包含 `os_internal.h`，可以绕过 Win32 port
逐步驱动内核。阅读某个功能时，可以对应查看下面的测试：

| 想验证的机制 | 对应测试方向 |
|---|---|
| 最高优先级启动 | `highest priority starts first` |
| 同优先级 FIFO 轮转 | `equal priority round robin` |
| 延时阻塞和唤醒 | `delay blocks and wakes task` |
| tick 回绕 | `delay wake handles tick wrap` |
| 信号量阻塞与抢占 | `semaphore blocks and wakes high priority` |
| 队列直接交付 | `queue direct receiver handoff` |
| 满队列发送者补位 | `queue full sender refill` |
| 互斥量优先级继承 | `mutex priority inheritance` |
| 超时后恢复优先级 | `mutex timeout restores priority` |
| 任务退出移交互斥量 | `task exit transfers owned mutex` |

测试中的每一步通常都紧跟 `os_KernelValidate()`，因此可以把测试看成“合法状态迁移示例”，
而不仅是最后判断成功或失败的自动脚本。
