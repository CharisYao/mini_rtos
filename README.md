# miniRTOS：Windows PC 上可观察的抢占式调度实验

这是一个完全使用 C11 编写、直接运行在 Windows PC 上的教学型 miniRTOS。它不需要开发板，也不使用 WSL。

通用内核自己维护任务状态、优先级、就绪/阻塞队列、时间片、信号量、消息队列和互斥量。Windows 移植层只负责提供真实任务栈、周期定时器以及线程暂停/恢复，所以你可以先理解 RTOS 的调度规则，再学习 Cortex-M 的 PendSV 和汇编移植。

## 直接构建和运行

在项目根目录打开 PowerShell：

~~~powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
~~~

运行两个动态面板：

~~~powershell
.\build\preemption_demo.exe
.\build\ipc_demo.exe
~~~

程序都是普通的 Windows `.exe`。按其设定时长运行结束后，会打印自动验收结果。

## 肉眼能看到什么

`preemption_demo` 有三个任务：

- `CarTask` 在文本轨道上移动。
- `ComputeTask` 推进计算进度条。
- `EmergencyTask` 平时延时，唤醒后以更高优先级立即抢占。

前两个任务都没有调用 `os_Yield`，但画面仍按时间片轮流推进。这证明切换不是任务主动配合伪装出来的。紧急任务运行时，两个低优先级画面都会暂停；紧急任务再次阻塞后，原任务从旧位置继续。

`ipc_demo` 展示一条温度处理流水线：

~~~text
SensorTask(P2) -> 6格消息队列 -> ProcessTask(P1)
                                      |
                                      v
                                AlarmTask(P3)
~~~

你会看到：

- 队列空时，消费者进入 `BLOCKED_OBJECT`，不空转占 CPU。
- 生产速度大于处理速度后，6 个格子逐渐填满。
- 队列满时，生产者进入 `BLOCKED_OBJECT`。
- 处理到高温样本时，信号量唤醒 P3 报警任务并立即抢占 P1。

面板中的 `Current`、任务状态、队列格子、最近切换方向和 `reason` 把这些因果关系直接显示出来。

## 为什么任务里不直接 printf 或 malloc

这不是所有 RTOS 的永久禁令，而是当前 Windows 教学移植的安全边界。

为了在任意计算位置抢占，移植层会暂停任务对应的 Windows 线程。如果线程恰好被暂停在 `printf` 或 `malloc` 的 C 运行库内部，它可能正持有运行库锁；此时观察器或其他代码再次进入运行库就可能死锁。因此：

- 任务只做普通计算、维护局部变量、更新 C11 原子演示状态并调用 `os_*` API。
- 控制台输出只由调度器的观察回调统一执行。
- 所有任务、内核对象和队列缓冲区都在启动前静态准备，运行中不分配内存。

这也很接近小型嵌入式系统常用的确定性设计习惯，但根本原因仍是 Win32 `SuspendThread` 移植方案的限制。

## 当前内核边界

- 最多 8 个任务，优先级 1 到 7；0 保留给 Idle 语义。
- 10 ms 系统 tick，同优先级时间片为 20 tick。
- 最多 4 个信号量、4 个队列和 4 个互斥量。
- 队列缓冲区由调用方在启动前提供。
- 互斥量不可递归，支持所有者检查和优先级继承。
- 不支持调度启动后创建/删除任务、SMP、多核并行或硬实时保证。
- 任务不能直接调用 Win32 阻塞 API、文件/网络 I/O 或不可控第三方库。
- `SuspendThread` 适合本教学实验，不是生产级用户态 RTOS 移植方案。

## 代码从哪里读起

建议按这条顺序：

1. `include/os.h`：先看任务能调用哪些 API。
2. `demos/preemption_demo.c`：看不主动让出的任务行为。
3. `kernel/os_sched.c` 和 `kernel/os_time.c`：看优先级选择、时间片和唤醒。
4. `port/win32/os_port_win32.c`：看 Windows 如何提供真正的异步打断。
5. `kernel/os_sem.c`、`os_queue.c`、`os_mutex.c`：看阻塞、唤醒和优先级继承。
6. `tests/test_kernel.c`：用测试场景核对每条调度规则。

更完整的架构、状态迁移、模块职责和验收标准见 `docs/design.md`。
