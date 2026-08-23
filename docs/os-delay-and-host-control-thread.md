# `os_Delay()` 调用链与宿主控制线程

## 1. 文档目的

本文结合当前 Windows 版 miniRTOS 的实际代码，回答下面三个问题：

1. 任务调用 `os_Delay()` 后，请求怎样从任务线程进入 `kernel/src/os_time.c`？
2. 宿主控制线程到底是什么，它和 Windows、miniRTOS 调度器是什么关系？
3. `os_Start()` 是否等于启动 RTOS 调度器？

本文只描述当前已经实现的 Win32 移植，不把它泛化为所有 RTOS 的唯一实现方式。

## 2. 先给出结论

### 2.1 `os_Delay()` 和 `time.c` 有直接关系

`os_Delay()` 分为两层：

- `port/win32/os_port_win32.c` 中的 `os_Delay()` 是任务可以调用的公开接口。它负责把延时请求安全地交给宿主控制线程。
- `kernel/src/os_time.c` 中的 `os_TimeDelayCurrent()` 是平台无关的延时内核实现。它负责修改 TCB、加入延时队列并触发调度。

因此，Win32 port 负责“把请求送进去”，`time.c` 负责“真正执行延时状态迁移”。

### 2.2 宿主控制线程不是 RTOS 任务

宿主控制线程就是调用 `os_Start()` 的普通 Windows 线程。在当前两个 demo 中，它最初就是运行 `main()` 的主线程。

进入 `os_Start()` 后，这个线程不再执行业务任务，而是负责：

- 等待任务提交内核请求；
- 等待 Windows 周期定时器产生 tick；
- 调用通用 kernel 更新 TCB、队列和系统时间；
- 根据内核给出的调度结果暂停或恢复任务线程；
- 调用控制台观察器并在运行结束时回收 Windows 对象。

它没有 TCB，不在 ready 队列中，也不会显示为控制台面板中的 `Current`。

### 2.3 `os_Start()` 对外表示“启动 RTOS”，但内部不只是一个调度算法函数

从应用角度看，`os_Start()` 就是启动 miniRTOS 调度器的公开 API。调用它以后，任务开始运行、tick 开始推进、抢占和时间片轮转开始生效。

从内部实现角度看，需要区分：

- `os_Start()`：Win32 移植层的完整启动和宿主控制循环；
- `os_SchedStart()`：通用 kernel 中首次选择最高优先级 READY 任务的调度函数。

也就是说，`os_Start()` 会调用 `os_SchedStart()`，但还会创建 Windows 线程、事件、定时器，执行任务切换并在结束时清理资源。

## 3. 一个进程中实际有哪些执行者

运行 `preemption_demo.exe` 或 `ipc_demo.exe` 后，Windows 创建一个进程。这个进程中主要存在两类线程。

### 3.1 宿主控制线程

宿主控制线程只有一个。它原本执行：

~~~text
main()
~~~

当 `main()` 调用 `os_Start()` 后，它开始执行：

~~~text
main()
└─ os_Start()
   └─ 宿主控制循环
~~~

“宿主”指 Windows 是承载 miniRTOS 的宿主系统；“控制”指这个线程统一处理 tick、内核请求和任务切换。它不是 Windows 提供的一种特殊线程类型，只是项目赋予主线程的职责名称。

### 3.2 任务工作线程

每个 miniRTOS TCB 对应一个 Windows 工作线程：

~~~text
CarTask TCB       <-> Windows 工作线程 A
ComputeTask TCB   <-> Windows 工作线程 B
EmergencyTask TCB <-> Windows 工作线程 C
~~~

Windows 工作线程提供真实的独立栈、程序执行位置和寄存器现场。miniRTOS TCB 保存任务状态、优先级、延时信息和等待关系。

工作线程不是调度决策者。哪个任务应该运行，仍由 `kernel/src/os_sched.c` 根据 TCB 和就绪队列决定。

### 3.3 整体关系

~~~text
Windows 进程
├─ 宿主控制线程
│  └─ os_Start() 控制循环
│     ├─ 处理 request_event
│     ├─ 处理 tick_timer
│     ├─ 调用通用 kernel
│     └─ 暂停或恢复任务线程
│
├─ 任务工作线程 A
│  └─ miniRTOS TaskA
├─ 任务工作线程 B
│  └─ miniRTOS TaskB
└─ 任务工作线程 C
   └─ miniRTOS TaskC
~~~

## 4. Windows 与 miniRTOS 调度器怎样分工

“Windows 调度器”和“miniRTOS 调度器”不是同一个东西。

| 组成 | 负责什么 |
|---|---|
| Windows 调度器 | 真正决定某个 Windows 原生线程什么时候获得 CPU，由 Windows 内核实现 |
| miniRTOS 调度器 | 根据优先级、任务状态和时间片决定哪个 TCB 逻辑上应当 RUNNING |
| Win32 port | 把 miniRTOS 的逻辑决定落实为 Windows 线程的等待、暂停、恢复和放行 |
| 宿主控制线程 | 串行调用 kernel 并执行 port 操作，是 kernel 与 Windows 对象之间的控制入口 |

miniRTOS 并没有替换 Windows 调度器。它采用下面的方式约束 Windows 线程：

1. 所有任务线程和宿主控制线程绑定到逻辑 CPU 0；
2. 未被 miniRTOS 选中的任务线程停在 `gate` 或被 `SuspendThread()` 暂停；
3. 只有 `g_os_kernel.current` 对应的任务线程被 port 放行；
4. Windows 调度器再从当前可运行的原生线程中分配 CPU。

所以最终执行仍依赖 Windows，但任务状态和“谁应该运行”的规则由 miniRTOS 自己维护。

## 5. `os_Start()` 的完整启动过程

公开声明位于 `include/os.h`，实现位于 `port/win32/os_port_win32.c` 的 `os_Start()`。

### 5.1 创建宿主运行对象

`os_Start()` 首先调用 `os_PortCreateRuntimeObjects()`。该函数创建：

- 所有任务共用的 `request_event`；
- 产生周期 tick 的 `tick_timer`；
- 用于识别当前工作线程所属任务的 TLS 索引；
- 每个任务自己的 `gate`；
- 每个 TCB 对应的 Windows 工作线程。

工作线程使用 `CREATE_SUSPENDED` 创建，所以此时虽然线程对象已经存在，任务入口还没有开始运行。

### 5.2 启动周期 tick

`os_PortStartTimer()` 按 `OS_TICK_MS` 启动 Windows 等待定时器。当前配置中：

~~~c
#define OS_TICK_MS 10U
~~~

因此标称每 10 ms 产生一个 miniRTOS tick。Windows 不是硬实时系统，所以实际墙上时间可能存在抖动，但内核内部仍按离散 tick 推进。

### 5.3 通用调度器选择第一个任务

`os_Start()` 调用：

~~~c
os_SchedStart();
~~~

`os_SchedStart()` 位于 `kernel/src/os_sched.c`，它从最高优先级非空 ready 队列取出第一个任务，并把该任务设为 `RUNNING`。

### 5.4 port 放行第一个任务线程

内核只改变 TCB 和 `g_os_kernel.current`，不会自己恢复 Windows 线程。随后 `os_Start()` 调用：

~~~c
os_PortActivate(g_os_kernel.current);
~~~

第一次运行时，目标工作线程仍处于 `CREATE_SUSPENDED` 状态，因此 `os_PortActivate()` 使用 `ResumeThread()` 启动它。

### 5.5 主线程进入宿主控制循环

`os_Start()` 随后同时等待两个 Windows 对象：

~~~c
wait_handles[0] = g_port.request_event;
wait_handles[1] = g_port.tick_timer;

WaitForMultipleObjects(2U, wait_handles, FALSE, INFINITE);
~~~

如果 `request_event` 到达，就调用 `os_PortProcessRequest()`；如果 `tick_timer` 到达，就调用 `os_PortProcessTimer()`。

`os_Start()` 不会在启动任务后立即返回。它会一直占据主线程，直到运行时间到达、任务全部结束或移植层出现错误，然后回收线程、事件、定时器和 TLS。

## 6. `os_Delay(5)` 的两条调用栈

下面假设一个正在运行的任务调用：

~~~c
os_Delay(5U);
~~~

这一过程不是一条从任务函数一直调用到 `time.c` 的普通嵌套调用栈。它在 `SetEvent()` 处跨越了两个 Windows 线程。

### 6.1 任务工作线程的调用栈

~~~text
任务函数
└─ os_Delay(5)
   └─ os_PortSubmitRequest(OS_PORT_REQUEST_DELAY, 5, ...)
      ├─ 写入任务请求槽
      ├─ SetEvent(request_event)
      └─ WaitForSingleObject(slot->gate, INFINITE)
         └─ 当前任务工作线程停止等待
~~~

当前实现中的关键位置如下。行号用于对应当前版本；后续增删代码后应以函数名为准。

| 作用 | 源文件 | 函数或语句 | 当前起始行 |
|---|---|---|---:|
| 任务工作线程统一入口 | [`os_port_win32.c`](../port/win32/os_port_win32.c) | `os_PortTaskEntry()` | 330 |
| 公开延时入口 | [`os_port_win32.c`](../port/win32/os_port_win32.c) | `os_Delay()` | 214 |
| 请求封装及 gate 等待 | [`os_port_win32.c`](../port/win32/os_port_win32.c) | `os_PortSubmitRequest()` | 156 |
| 恢复被选中的任务线程 | [`os_port_win32.c`](../port/win32/os_port_win32.c) | `os_PortActivate()` | 367 |
| 查找请求者 | [`os_port_win32.c`](../port/win32/os_port_win32.c) | `os_PortFindRequester()` | 455 |
| 请求分发 | [`os_port_win32.c`](../port/win32/os_port_win32.c) | `os_PortProcessRequest()` | 475 |
| Win32 运行环境和控制循环 | [`os_port_win32.c`](../port/win32/os_port_win32.c) | `os_Start()` | 767 |
| 延时状态迁移 | [`os_time.c`](../kernel/src/os_time.c) | `os_TimeDelayCurrent()` | 38 |
| tick、超时和到期唤醒 | [`os_time.c`](../kernel/src/os_time.c) | `os_TimeTick()` | 65 |
| 阻塞后选择下一任务 | [`os_sched.c`](../kernel/src/os_sched.c) | `os_SchedCurrentBlocked()` | 222 |

### 6.2 宿主控制线程的调用栈

~~~text
main()
└─ os_Start()
   └─ WaitForMultipleObjects()
      └─ request_event 到达
         └─ os_PortProcessRequest()
            └─ os_TimeDelayCurrent(5)
               ├─ 设置 wake_tick
               ├─ 状态改为 BLOCKED_DELAY
               ├─ 加入 delayed 队列
               └─ os_SchedCurrentBlocked()
                  ├─ os_ReadyPopHighest()
                  └─ os_CommitSwitch()
~~~

这两条调用栈通过 Windows 事件连接，而不是通过普通 C 函数调用直接连接。

## 7. 任务线程怎样提交延时请求

### 7.1 `os_Delay()` 进入 port

`os_Delay()` 对非零延时调用：

~~~c
os_PortSubmitRequest(
    OS_PORT_REQUEST_DELAY,
    ticks,
    NULL,
    NULL,
    true
);
~~~

最后一个参数为 `true`，表示任务必须停在 `gate` 上等待，直到它以后再次被 miniRTOS 选中。

`ticks == 0` 时不进入延时队列，而是退化为 `os_Yield()`。

### 7.2 TLS 找到当前任务槽位

每个 Windows 工作线程启动时，`os_PortTaskEntry()` 执行：

~~~c
TlsSetValue(g_port.tls_index, slot);
~~~

任务以后调用内核 API 时，`os_PortSubmitRequest()` 使用：

~~~c
slot = (os_port_task_slot_t *)TlsGetValue(g_port.tls_index);
~~~

从而知道请求来自哪个 miniRTOS 任务。TLS 可以理解为每个 Windows 线程独有的“身份标签”。

### 7.3 写入单槽请求邮箱

`os_PortSubmitRequest()` 原子写入：

~~~text
slot->request       = OS_PORT_REQUEST_DELAY
slot->request_ticks = 5
slot->phase         = OS_PORT_PHASE_REQUEST
~~~

`InterlockedExchange()` 用于保证宿主控制线程不会读到只写了一半的请求状态。

### 7.4 `request_event` 只负责通知

任务线程随后调用：

~~~c
SetEvent(g_port.request_event);
~~~

这句话的含义只是：

> 请求参数已经写好，请唤醒宿主控制线程处理。

`SetEvent()` 不会直接调用 `os_PortProcessRequest()`，也不会在任务线程中执行 `time.c`。

### 7.5 任务停在自己的 `gate`

任务线程接着执行：

~~~c
WaitForSingleObject(slot->gate, INFINITE);
~~~

此时 `os_Delay()` 尚未返回，任务函数也不会继续执行。Windows 把这个工作线程置于等待状态，它不会在这里空转消耗 CPU。

## 8. 宿主控制线程怎样处理延时请求

### 8.1 `request_event` 唤醒 `os_Start()`

宿主控制线程原本停在：

~~~c
WaitForMultipleObjects(...);
~~~

当 `request_event` 被设置后，该函数返回 `WAIT_OBJECT_0`，`os_Start()` 随即调用：

~~~c
os_PortProcessRequest();
~~~

### 8.2 找出请求者并取走请求

`os_PortFindRequester()` 扫描固定任务槽位，找到 `request != OS_PORT_REQUEST_NONE` 的任务。

`os_PortProcessRequest()` 再使用 `InterlockedExchange()` 读取并清空请求码，保证同一个请求只被处理一次。它还会验证请求者必须等于 `g_os_kernel.current`，防止未运行任务修改内核。

### 8.3 分发到 `time.c`

延时请求进入下面的分支：

~~~c
case OS_PORT_REQUEST_DELAY:
    (void)os_TimeDelayCurrent(
        (uint32_t)os_PortReadLong(&slot->request_ticks)
    );
    break;
~~~

从这里开始进入平台无关的通用 kernel。

### 8.4 `os_TimeDelayCurrent()` 修改 TCB

`kernel/src/os_time.c` 中的核心状态迁移是：

~~~c
current->wake_tick = os_TickGet() + ticks;
current->state = OS_TASK_BLOCKED_DELAY;
os_ListPushBack(&g_os_kernel.delayed, &current->schedule_node);
return os_SchedCurrentBlocked(OS_SWITCH_BLOCKED);
~~~

假设当前 tick 为 100，调用 `os_Delay(5)` 后：

~~~text
wake_tick = 100 + 5 = 105

任务状态：RUNNING -> BLOCKED_DELAY
任务位置：当前任务 -> delayed 延时队列
~~~

### 8.5 调度器选择下一任务

`os_SchedCurrentBlocked()` 调用 `os_ReadyPopHighest()`，从最高优先级非空 ready 队列选择下一任务，再通过 `os_CommitSwitch()` 更新：

~~~text
g_os_kernel.current
last_from
last_to
last_reason
switch_count
~~~

调度器只改变内核数据。返回 port 后，`os_PortProcessRequest()` 调用：

~~~c
os_PortActivate(g_os_kernel.current);
~~~

把新的调度结果落实到对应 Windows 任务线程。

## 9. 原延时任务怎样恢复

原任务仍停在自己的 `gate` 上。处理完 DELAY 请求时，port 放行的是新的 `current`，不会立即放行刚刚进入 `BLOCKED_DELAY` 的原任务。

之后每次 Windows 定时器到期，执行：

~~~text
os_Start()
└─ os_PortProcessTimer()
   └─ os_PortApplyTick()
      └─ os_TimeTick()
~~~

`os_TimeTick()` 将系统 tick 加一并扫描延时队列。当 `now` 到达 `wake_tick` 时：

~~~c
os_ListRemove(&g_os_kernel.delayed, node);
task->state = OS_TASK_READY;
os_ReadyEnqueue(task);
~~~

这一步只表示任务重新具备运行条件，不保证它马上运行。如果有更高优先级任务，它仍要留在 ready 队列等待。

当调度器最终选中该任务时，`os_PortActivate()` 执行：

~~~c
SetEvent(slot->gate);
~~~

原任务线程中的 `WaitForSingleObject()` 返回，调用栈依次恢复：

~~~text
WaitForSingleObject() 返回
└─ os_PortSubmitRequest() 返回
   └─ os_Delay() 返回
      └─ 任务从 os_Delay() 后面的语句继续运行
~~~

## 10. `request_event` 和 `gate` 的区别

| 对象 | 数量 | 谁等待 | 谁设置 | 作用 |
|---|---:|---|---|---|
| `request_event` | 全局 1 个 | 宿主控制线程 | 提交 API 请求的任务线程 | 通知控制线程“有请求待处理” |
| `gate` | 每个任务 1 个 | 对应任务工作线程 | 宿主控制线程 | 允许被调度选中的任务继续执行 |

可以把它们理解为两种方向相反的通知：

~~~text
任务线程 --request_event--> 宿主控制线程
任务线程 <--gate----------- 宿主控制线程
~~~

`gate` 不只是“请求处理完毕”的通知。对于 `os_Delay()`、阻塞式信号量和阻塞式队列操作，它还表示：

> 当前任务已经重新成为 miniRTOS 选中的 RUNNING 任务，可以从 API 内部返回了。

## 11. 宿主控制线程是不是 RTOS 调度器

不能直接画等号。

### 11.1 miniRTOS 调度器是一组内核数据和函数

调度器的核心位于 `kernel/src/os_sched.c`，主要包括：

- 每个优先级的 ready 队列；
- ready 位图；
- `g_os_kernel.current`；
- `os_SchedStart()`；
- `os_SchedSchedule()`；
- `os_SchedYieldCurrent()`；
- `os_SchedCurrentBlocked()`；
- `os_SchedTerminateCurrent()`。

这些函数按照任务状态和优先级算出“下一任务是谁”，但它们本身不是一个线程。

### 11.2 宿主控制线程执行调度器函数

宿主控制线程收到请求或 tick 后调用这些 kernel 函数，然后使用 Win32 API 执行结果。因此更准确的关系是：

~~~text
宿主控制线程
    ├─ 驱动 miniRTOS 调度器作决定
    └─ 调用 Win32 port 执行决定
~~~

它相当于 Windows 教学移植中的“内核运行环境”，而不是一个可被 miniRTOS 调度的业务任务。

## 12. 与 STM32 Cortex-M 的对应关系

STM32 上没有 Windows，也没有这个形式的宿主控制线程。相同职责由硬件异常机制和移植层共同完成。

| Windows PC 版本 | STM32 Cortex-M 版本 |
|---|---|
| Windows 工作线程保存任务栈和执行位置 | 每个任务自己的栈和 PSP 保存执行现场 |
| Windows 等待定时器 | SysTick 硬件定时中断 |
| `request_event` 通知宿主控制线程 | 任务进入内核 API 或触发 SVC/PendSV |
| 宿主控制线程调用 `os_TimeTick()` | SysTick 处理函数调用内核 tick 逻辑 |
| `SuspendThread()`、`ResumeThread()`、`gate` | PendSV 保存和恢复 CPU 寄存器及 PSP |
| Win32 port | Cortex-M port 和少量汇编 |
| `kernel/src/os_sched.c` | 仍然是平台无关的调度决策逻辑 |

因此，宿主控制线程没有一个完全对应的 STM32“线程”。在 Cortex-M 上，它承担的职责被拆分到：

- 当前任务调用的内核 API；
- SysTick 中断；
- PendSV 上下文切换；
- SVC 或启动首任务代码。

如果把当前通用 kernel 移植到 STM32，`kernel/src/os_sched.c`、`kernel/src/os_time.c` 和内核对象的核心逻辑可以继续使用；`port/win32/os_port_win32.c` 则必须替换为 Cortex-M 移植层。

## 13. 常见误解

### 13.1 `os_Delay()` 等于 Windows `Sleep()`

不是。当前实现没有用 `Sleep()` 完成任务延时。任务进入 miniRTOS 延时队列，tick 到期后重新变为 READY，是否运行仍由 miniRTOS 优先级调度决定。

### 13.2 宿主控制线程就是最高优先级 RTOS 任务

不是。它没有 TCB，不在 ready 队列中，不受 miniRTOS 优先级和时间片调度。

### 13.3 miniRTOS 完全绕过了 Windows 调度器

不是。所有代码最终仍运行在线程和 Windows 内核之上。miniRTOS 通过暂停、等待和恢复任务线程，限制哪些原生线程有资格运行。

### 13.4 `os_Start()` 只调用一次调度算法然后返回

不是。它负责建立 Win32 运行环境、选择首任务，并一直运行宿主控制循环，直到系统结束后才返回。

## 14. 建议阅读顺序

按照下面的顺序对照代码最容易理解：

1. `include/os.h`：查看 `os_Start()`、`os_Delay()` 和 tick 配置。
2. `port/win32/os_port_win32.c` 中的 `os_Start()`：看宿主控制循环。
3. 同文件中的 `os_Delay()` 和 `os_PortSubmitRequest()`：看任务怎样提交请求并停在 gate。
4. 同文件中的 `os_PortProcessRequest()`：看请求怎样分发到 kernel。
5. `kernel/src/os_time.c` 中的 `os_TimeDelayCurrent()`：看任务怎样进入延时队列。
6. `kernel/src/os_sched.c` 中的 `os_SchedCurrentBlocked()`：看下一任务怎样选出。
7. 回到 port 的 `os_PortActivate()`：看内核选择怎样落实为 Windows 线程恢复。
8. `kernel/src/os_time.c` 中的 `os_TimeTick()`：看延时任务怎样到期并重新变为 READY。
