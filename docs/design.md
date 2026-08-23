# Windows PC 抢占式 miniRTOS 设计说明

## 1. 文档状态

- 当前阶段：五个实现阶段均已完成，本文档同时作为最终设计与验收基线。
- 已实现通用内核、Win32 抢占移植、两个动态演示和自动测试。
- 目标平台：Windows 10/11 本机。
- 当前工具链：MinGW-w64 GCC、CMake、Ninja。
- 禁止访问、依赖或触发 WSL。

本文档是后续实现的设计基线。若实现过程中发现某项底层机制不可行，应先记录证据并重新讨论，不能悄悄把抢占式调度改成协作式调度，也不能擅自改变已经确认的目录和模块边界。

## 2. 项目目标

本项目在 Windows PC 上实现一个教学用、可直接运行、可观察的 miniRTOS。

它要帮助学习和验证：

- TCB 与任务状态。
- 固定优先级抢占调度。
- 同优先级时间片轮转。
- 就绪队列、延时队列和等待队列。
- 任务阻塞与唤醒。
- 信号量、消息队列和互斥量。
- 优先级反转与优先级继承。
- 临界区和延迟调度。

项目必须同时提供两类结果：

1. tests 中的自动测试负责证明调度规则和内核不变量正确。
2. demos 中的动态控制台面板负责让人肉眼看出任务切换、抢占、阻塞、唤醒和任务间通信。

## 3. “纯 C”与“可直接在 PC 运行”的定义

### 3.1 本项目是 C 语言项目

- 所有实现文件使用 .c 和 .h。
- 使用 C11。
- 不使用 C++。
- 不使用 Python、Java、浏览器或 GUI 框架作为运行时。
- 不依赖第三方协程库或现成 RTOS 内核。
- 动态控制台面板也由 C 代码实现。

### 3.2 本项目不是“只使用 ISO C 标准库”

标准 C 没有线程、周期定时器和在任意指令位置暂停任务的能力。为了在 Windows PC 上产生真正的异步抢占，port/win32 层需要调用少量 Win32 API。

因此准确表述是：

> 整个项目使用 C 语言编写；通用内核层尽量保持平台无关；Windows 移植层使用 Win32 API 提供线程、定时器、事件以及暂停和恢复能力。

最终程序编译为普通 Windows .exe，可以在 PC 本机直接运行，不需要 WSL。

### 3.3 Windows API 的隔离边界

windows.h、HANDLE、DWORD、CreateThread 等 Windows 类型和函数只能出现在：

~~~text
port/win32/os_port_win32.c
~~~

kernel、include、tests 和 demos 不直接包含 windows.h。

任务函数只使用 os.h 中公开的 miniRTOS API，不需要理解 Windows 线程 API 才能阅读任务代码。

## 4. 项目性质与非目标

### 4.1 项目性质

本项目是“Windows 宿主版抢占式 miniRTOS 内核实验”。

miniRTOS 自己决定：

- 哪个任务处于 READY。
- 哪个任务应当 RUNNING。
- 为什么切换任务。
- 任务如何阻塞和唤醒。
- 优先级和时间片规则。
- 信号量、队列和互斥量的语义。

Windows 负责：

- 保存和恢复 PC CPU 的实际寄存器现场。
- 为每个宿主线程提供真实栈。
- 提供周期定时器和线程暂停、恢复能力。

### 4.2 明确不做

第一阶段不研究：

- Cortex-M 的 PendSV、SVC、SysTick、PSP 和 MSP。
- ARM 或 x86-64 汇编上下文切换。
- 硬实时响应保证。
- 多核 SMP 调度。
- 网络、文件系统和驱动框架。
- 任务动态删除。
- 调度启动后的动态内存分配。
- 浮点上下文专项处理。
- 图形界面。

Windows 本身不是硬实时操作系统，因此本项目只能验证调度逻辑和可观察行为，不能承诺严格的微秒或毫秒级最坏响应时间。

## 5. 总体架构

### 5.1 固定目录结构

后续实现必须以以下结构为基线：

~~~text
mini_rtos/
├── CMakeLists.txt
├── README.md
├── docs/
│   └── design.md
├── include/
│   └── os.h
├── kernel/
│   ├── include/
│   │   ├── os_list.h
│   │   ├── os_task_internal.h
│   │   ├── os_kernel_state.h
│   │   ├── os_sched.h
│   │   ├── os_time.h
│   │   ├── os_sem_internal.h
│   │   ├── os_queue_internal.h
│   │   └── os_mutex_internal.h
│   └── src/
│       ├── os_task.c
│       ├── os_sched.c
│       ├── os_list.c
│       ├── os_time.c
│       ├── os_sem.c
│       ├── os_queue.c
│       └── os_mutex.c
├── port/
│   └── win32/
│       └── os_port_win32.c
├── tests/
│   └── test_kernel.c
└── demos/
    ├── preemption_demo.c
    └── ipc_demo.c
~~~

分阶段实现时可以只完成其中一部分功能，但不能用缩略目录代替这份完整架构，也不能因此把模块职责挪到其他文件。

### 5.2 模块职责

#### include/os.h

唯一公共 API 入口，直接声明应用可使用的配置、公共枚举、不透明句柄和全部公开
服务接口：

- 内核初始化与启动。
- 任务创建、让出、延时和退出。
- tick 查询。
- 信号量 API。
- 消息队列 API。
- 互斥量 API。
- 只读任务状态和调度轨迹接口。

当前项目规模下没有再拆分出多个 public `api.h`；应用只需包含 `os.h`。
`os.h` 不暴露 Win32 线程句柄，也不让应用直接修改 TCB。

#### kernel/include/*.h

内核头文件按职责拆分：`os_list.h` 拥有链表类型，`os_task_internal.h` 拥有 TCB，
`os_sem_internal.h`、`os_queue_internal.h` 和 `os_mutex_internal.h` 分别拥有对应
内核对象控制块；`os_kernel_state.h` 只组合全局内核状态。Win32 port 和白盒测试
显式包含所需模块头，不再依赖一个汇总全部内部类型的总头文件。

#### kernel/src/os_task.c

- TCB 管理。
- 任务创建参数检查。
- 任务状态迁移。
- 当前任务信息。
- 任务入口和任务返回处理。

#### kernel/src/os_sched.c

- 选择最高优先级 READY 任务。
- 同优先级时间片轮转。
- 判断是否需要抢占。
- 记录任务切换原因。
- 检查调度不变量。

#### kernel/src/os_list.c

- 就绪队列。
- 延时队列。
- 内核对象等待队列。
- 节点插入、移除和一致性检查。

第一版任务数量很少，优先选择容易检查和讲解的数据结构，不为了理论复杂度过早引入复杂容器。

#### kernel/src/os_time.c

- 系统 tick。
- 任务延时。
- 延时到期唤醒。
- tick 回绕安全比较。
- 时间片计数。

#### kernel/src/os_sem.c

- 二值信号量。
- 计数信号量。
- 获取、释放、阻塞和超时。
- 按调度规则唤醒等待任务。

#### kernel/src/os_queue.c

- 固定容量环形消息缓冲区。
- 发送等待队列。
- 接收等待队列。
- 队列满、队列空和超时语义。

#### kernel/src/os_mutex.c

- 互斥量所有者。
- 非所有者释放检查。
- 优先级反转演示。
- 优先级继承。

递归互斥量不属于第一版范围。

#### port/win32/os_port_win32.c

- 创建 Windows 宿主线程。
- 为任务保存 Windows 线程句柄。
- 创建调度器线程。
- 创建周期定时器和调度事件。
- 暂停当前任务并恢复目标任务。
- 实现宿主临界区和调度请求握手。
- 提供控制台显示所需的安全输出通道。
- 隔离全部 Windows 专用代码。

#### tests/test_kernel.c

- 使用断言、状态快照和计数器验证内核。
- 测试调度选择、队列操作和状态迁移。
- 测试 tick 回绕和超时。
- 检查任意时刻最多一个任务处于 RUNNING。
- 检查一个 TCB 不能同时存在于多个调度容器。

计数器主要属于自动测试，不作为正式演示的可视化主体。

#### demos/preemption_demo.c

动态展示：

- 同优先级任务时间片轮转。
- 不主动调用 os_Yield 的任务仍被抢占。
- 高优先级任务到期后抢占低优先级任务。
- 低优先级任务稍后从原位置继续运行。

#### demos/ipc_demo.c

动态展示：

- 传感器生产数据。
- 消息进入和离开队列。
- 队列满、队列空导致任务阻塞。
- 信号量唤醒报警任务。
- 高优先级报警任务立即抢占。

## 6. Windows 宿主运行模型

### 6.1 线程角色

第一版使用：

- 每个 miniRTOS 任务对应一个 Windows 工作线程。
- 一个 Windows 调度器线程负责处理 tick 和内核请求。
- 一个只负责显示的观察通道读取内核轨迹并刷新控制台。

工作线程只是任务运行现场的宿主。miniRTOS 的 TCB、任务状态和调度规则仍然由本项目维护。

### 6.2 单核运行约束

miniRTOS 工作线程和调度器线程应绑定到同一个逻辑 CPU，降低多个任务真正并行运行的可能，使运行模型接近单核 MCU：

~~~text
任意时刻：
最多一个 miniRTOS 任务为 RUNNING
其他任务只能是 READY、BLOCKED 或 TERMINATED
~~~

控制台观察通道不属于 miniRTOS 任务，不参与优先级竞争，也不能修改内核调度结果。

### 6.3 tick 与抢占

初始建议参数：

- 内核 tick：10 ms。
- 动态面板刷新：100 ms。
- 同优先级时间片：200 ms。
- 紧急事件演示周期：约 3 s。

这些值以后通过编译期配置调整。

tick 流程：

~~~text
Windows 周期定时器到期
        ↓
调度器线程获得执行机会
        ↓
更新 tick 和延时任务
        ↓
检查高优先级唤醒或时间片到期
        ↓
调用通用调度器选择 next
        ↓
必要时暂停 current、恢复 next
~~~

如果高优先级任务从阻塞态变成 READY，应在下一次可安全调度的位置抢占低优先级任务。

### 6.4 任务调用内核 API

任务调用 os_Delay、os_SemTake 或 os_QueueReceive 时，不能直接自行修改多条内核队列。

计划采用统一内核请求流程：

~~~text
任务提交请求
        ↓
任务进入内核边界并暂时禁止抢占
        ↓
通知调度器
        ↓
调度器集中修改 TCB 和队列
        ↓
任务阻塞或重新进入 READY
        ↓
调度器选择下一个任务
~~~

这样可以让内核状态主要由调度器串行修改，减少 tick 与任务 API 同时修改链表的风险。

### 6.5 强制暂停的风险边界

Win32 的 SuspendThread 和 ResumeThread 能让教学程序在任务不调用 os_Yield 时仍产生异步抢占，但 SuspendThread 主要面向调试用途。若目标线程正持有 C 运行库或 Windows 内部锁，强制暂停后可能造成死锁。

因此第一版必须遵守：

- 任务启动后不直接调用 malloc 和 free。
- 任务不直接调用 printf、fprintf、puts 等控制台输出函数。
- 任务不直接调用 Sleep、文件 I/O、套接字和其他可能阻塞的宿主 API。
- 任务之间只能通过 miniRTOS API 通信。
- 所有可视化输出通过预分配的轨迹或状态通道交给观察模块。
- 调度器不依赖被任务持有的锁。
- 内核临界区内禁止阻塞。
- 临界区内到达的 tick 记为 pending，退出临界区后补处理。

这不是说 RTOS 永远禁止 malloc 或 printf，而是当前 Windows 强制抢占移植方案需要这一受控边界。

实现前应参考 Microsoft 对 SuspendThread 的风险说明：

https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-suspendthread

如果强制暂停方案无法在这些边界下稳定工作，必须停止实现并重新评审移植层，不能把 os_checkpoint 或隐藏的主动让出包装成“抢占式”。

## 7. 调度规则

### 7.1 优先级

- 初始支持 8 个优先级。
- 数值越大，优先级越高。
- 优先级 0 保留给 Idle。
- 任务创建后基础优先级固定。
- 互斥量阶段允许有效优先级因优先级继承临时提高。

### 7.2 就绪选择

调度器总是选择最高的非空就绪优先级。

同优先级任务使用 FIFO 就绪队列：

~~~text
TaskA -> TaskB -> TaskC -> TaskA
~~~

### 7.3 抢占条件

以下情况应触发调度判断：

- 周期 tick 到达。
- 当前任务时间片耗尽。
- 高优先级延时任务到期。
- 信号量或队列唤醒高优先级任务。
- 当前任务调用 os_Yield。
- 当前任务调用阻塞式 API。
- 当前任务返回或退出。
- 退出最外层临界区时存在 pending 调度。

### 7.4 调度结果

- 更高优先级 READY 任务必须优先。
- 同优先级任务只有在时间片到期、主动 yield 或当前任务阻塞时轮转。
- 低优先级任务不能因为时间片轮转越过仍处于 READY 的高优先级任务。
- 若最高优先级只有当前任务一个，不做无意义切换。

### 7.5 任务状态

第一版状态：

~~~text
UNUSED
READY
RUNNING
BLOCKED_DELAY
BLOCKED_OBJECT
TERMINATED
~~~

SUSPENDED 和动态删除不属于第一版。

## 8. 内核不变量

每次状态迁移和调度后都应能检查：

1. 任意时刻最多一个任务是 RUNNING。
2. current 指针为空或指向 RUNNING 任务。
3. READY 任务必须且只能出现在一条就绪队列中。
4. BLOCKED_DELAY 任务必须且只能出现在延时队列中。
5. BLOCKED_OBJECT 任务必须且只能出现在对应对象等待队列中。
6. TERMINATED 任务不能出现在任何可调度队列中。
7. 就绪优先级位图必须和就绪队列实际内容一致。
8. 同一队列节点不能重复插入。
9. 当前任务进入阻塞态后不能继续执行任务代码。
10. 临界区嵌套计数不能为负，退出最外层后必须处理 pending 调度。

自动测试在每个构造的状态迁移后调用 `os_KernelValidate`；Win32 运行时在每次 tick 和任务内核请求完成后也检查这些条件。发现破坏时立即让运行失败，而不是等链表损坏后才观察崩溃。

## 9. 动态控制台面板

### 9.1 实现方式

动态面板使用 C 语言实现。

计划使用：

- 固定大小的可观察状态结构。
- C11 原子值或 Win32 移植层提供的原子快照。
- 固定大小的调度轨迹环形缓冲区。
- ANSI 控制台转义序列或等效的 Windows 控制台刷新能力。
- printf 只由观察模块统一调用。

不使用图形库，不创建窗口，不依赖 Python。

### 9.2 面板目的

面板不是装饰，它必须直接显示 RTOS 因果关系：

- 当前运行任务。
- 每个任务的优先级和状态。
- 最近一次从谁切换到谁。
- 切换原因。
- 时间片剩余量。
- 延时任务还有多久唤醒。
- 消息队列占用量。
- 信号量等待状态。

### 9.3 抢占演示任务

preemption_demo.c 固定包含三类任务。

#### CarTask，优先级 1

- 在文本轨道上移动一个字符。
- 不调用 os_Yield。
- 被切走后位置停止变化。
- 再次运行时从原位置继续。

#### ComputeTask，优先级 1

- 执行持续的 CPU 计算。
- 在文本进度条上显示阶段进度。
- 不调用 os_Yield。
- 与 CarTask 按时间片轮转。

#### EmergencyTask，优先级 3

- 平时调用 os_Delay，处于 BLOCKED_DELAY。
- 约每 3 秒到期一次。
- 到期后抢占 CarTask 或 ComputeTask。
- 显示紧急事件处理阶段。
- 处理完成后再次延时。

示意面板：

~~~text
================ miniRTOS PREEMPTION ================
Tick       : 0300
Current    : EmergencyTask (P3)
Last switch: CarTask -> EmergencyTask
Reason     : higher_priority_wakeup

[P1][READY  ] CarTask
Track      : |----------->------------------|

[P1][READY  ] ComputeTask
Progress   : [##############--------------] 51%

[P3][RUNNING] EmergencyTask
Emergency  : [######----------------------] 2/8
======================================================
~~~

肉眼验收：

- CarTask 运行时轨道位置连续变化。
- ComputeTask 运行时进度连续变化。
- 两者不调用 os_Yield，仍会轮流获得 CPU。
- EmergencyTask 唤醒时，另外两个任务的画面立即停止变化。
- EmergencyTask 阻塞后，原任务从旧位置继续。

### 9.4 IPC 演示任务

ipc_demo.c 固定使用“温度采集和报警”场景。

#### SensorTask，优先级 2

- 周期产生温度样本。
- 通过 os_QueueSend 发送。
- 生产速度阶段性快于消费者，用来展示队列满和发送阻塞。

#### ProcessTask，优先级 1

- 通过 os_QueueReceive 等待样本。
- 处理速度阶段性慢于生产者。
- 检测到超过阈值的数据时释放报警信号量。

#### AlarmTask，优先级 3

- 平时阻塞在信号量上。
- 获得信号量后立即抢占。
- 显示高温报警处理。
- 处理完成后重新等待信号量。

示意面板：

~~~text
==================== miniRTOS IPC ====================
Current     : AlarmTask (P3)
Last switch: ProcessTask -> AlarmTask
Reason      : higher_priority_wakeup

SensorTask  [READY  ] latest = 76 C
Queue       [42][48][55][63][76][  ]
Usage       5 / 6

ProcessTask [READY  ] last processed = 63 C
AlarmTask   [RUNNING] HIGH TEMP: 76 C
======================================================
~~~

肉眼验收：

- 可以看到样本进入和离开消息队列。
- 队列满时 SensorTask 进入 BLOCKED_OBJECT。
- 队列空时 ProcessTask 进入 BLOCKED_OBJECT。
- 高温出现后 AlarmTask 从 BLOCKED_OBJECT 变成 RUNNING。
- 面板明确显示 `higher_priority_wakeup`；在该场景中它由 ProcessTask 释放报警信号量触发。

## 10. 输出、内存和任务代码边界

### 10.1 输出

任务不能直接刷新控制台。

任务只更新预分配的演示状态或写入固定大小轨迹记录。观察模块统一显示，避免：

- 多个任务输出相互穿插。
- 任务被暂停在 C 运行库输出锁内部。
- 控制台速度改变调度结果。

### 10.2 内存

第一版采用静态或启动前一次性分配：

- 固定最大任务数，初始值 8。
- 固定优先级数量，初始值 8。
- 固定 TCB 数组。
- 固定队列控制块数量。
- 消息队列缓冲区由调用方提供。
- 固定调度轨迹缓冲区。

调度器启动后不调用 malloc 和 free。

### 10.3 任务允许做什么

任务可以：

- 进行普通 C 计算。
- 修改自己的任务局部变量。
- 更新约定的演示状态。
- 调用公开的 os_* API。

任务不可以：

- 直接修改 TCB 或内核链表。
- 直接调用 Win32 线程和同步 API。
- 绕过 os_queue 和 os_sem 共享复杂数据。
- 在第一版中执行不可控的第三方库函数。

## 11. 计划公开 API

当前公开 API 由 `include/os.h` 定义，核心功能边界为：

~~~text
os_Init
os_TaskCreate
os_Start
os_Yield
os_Delay
os_TickGet
os_TaskGetState

os_SemInit
os_SemTake
os_SemGive

os_QueueInit
os_QueueSend
os_QueueReceive

os_MutexInit
os_MutexLock
os_MutexUnlock
~~~

阻塞式信号量、队列和互斥量 API 最终应支持超时，但可以分阶段实现“永久等待”和“有限超时”。

## 12. 实现阶段

### 阶段 0：设计冻结

- 状态：已完成。
- 已创建固定目录结构并冻结 Windows 本机、纯 C11 和 Win32 隔离边界。

### 阶段 1：可测试的通用内核模型

- 状态：已完成并审查。
- TCB 和任务状态。
- 链表和就绪队列。
- 固定优先级选择。
- 同优先级轮转。
- 内核不变量断言。
- test_kernel 自动测试。

这一阶段可以直接调用调度函数测试状态，不把它冒充为已经实现了 PC 抢占。

### 阶段 2：Windows 抢占移植和动态演示

- 状态：已完成并审查。
- Windows 工作线程。
- 调度器线程和周期 tick。
- 强制暂停和恢复。
- os_Yield 和 os_Delay。
- preemption_demo 动态面板。

阶段验收必须包含两个从不调用 os_Yield 的同优先级任务。如果它们不能被时间片轮转，就不能声称完成抢占。

### 阶段 3：信号量和消息队列

- 状态：已完成并审查。
- 二值、计数信号量。
- 固定长度消息队列。
- 阻塞、唤醒和超时。
- ipc_demo 动态面板。

### 阶段 4：互斥量和优先级继承

- 状态：已完成并审查。
- 所有者检查。
- 优先级反转复现实验。
- 优先级继承。
- 继承结束后的优先级恢复。

### 阶段 5：压力测试与说明

- 状态：已完成并审查。
- 长时间调度压力测试。
- tick 回绕测试。
- 随机状态迁移测试。
- 调度轨迹检查。
- 整理运行说明和已知限制。

## 13. 最终验收标准

### 13.1 构建

- 使用 Windows 本机 MinGW-w64 GCC 编译。
- 使用 CMake 和 Ninja 构建。
- 不进入 WSL。
- 生成可直接运行的 Windows .exe。

### 13.2 抢占

- 两个不调用 os_Yield 的同优先级任务都能持续推进可见行为。
- 时间片到期时面板显示任务切换。
- 高优先级任务到期时抢占低优先级任务。
- 被抢占任务稍后从原状态继续。

### 13.3 阻塞与通信

- os_Delay 期间任务行为停止变化。
- 队列空和队列满能让正确任务阻塞。
- 信号量释放能唤醒等待任务。
- 高优先级任务被唤醒后优先执行。

### 13.4 稳定性

- Debug 构建不触发内核不变量断言。
- 调度轨迹不存在两个任务同时为 RUNNING。
- 正常演示运行期间不死锁、不崩溃。
- 控制台显示不会直接修改调度状态。

## 14. 当前结论

方案固定为：

> 在 Windows PC 上使用 C11 编写 miniRTOS。通用内核自行实现 TCB、任务状态、优先级调度、队列和内核对象；Win32 移植层提供宿主线程、定时器以及暂停和恢复能力；动态控制台面板使用 C 代码直接在 Windows 控制台运行，并用可见任务行为展示时间片轮转、高优先级抢占、阻塞、唤醒和 IPC。

当前实现保持了最初冻结的模块边界：通用内核不包含 Windows 类型，所有 Win32 API 集中在 `port/win32/os_port_win32.c`，动态面板和任务行为均由 C11 代码实现。

实现已经具备：

- 8 个静态任务和 8 个固定优先级。
- 10 ms tick 与 20 tick 同优先级时间片。
- Windows 周期定时器驱动的异步抢占。
- 延时、信号量、固定容量消息队列及有限/永久等待。
- 非递归互斥量、所有者检查、优先级继承及超时/解锁后的优先级恢复。
- 两个可直接运行的动态控制台演示。
- 调度、IPC、tick 回绕、优先级反转和随机状态迁移自动测试。

具体构建命令、演示观察方法和已知限制见项目根目录 `README.md`。
