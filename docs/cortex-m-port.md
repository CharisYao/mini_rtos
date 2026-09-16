# Cortex-M port (Phase B)

This port implements `kernel/include/os_port.h` for **Cortex-M3/M4 without FPU**
(no lazy VFP stacking). Target boards used in docs and smoke checks:

- STM32F103 (Cortex-M3)
- STM32F407 (Cortex-M4, build with `-mfpu=none` / soft ABI; do not enable FPU)

Scheduling rules stay in the generic kernel. This port only provides:

| HAL hook | Cortex-M mapping |
| --- | --- |
| `os_port_enter/exit_critical` | `cpsid i` / `cpsie i` with nesting (`PRIMASK`) |
| `os_port_pend_context_switch` | `SCB->ICSR` bit `PENDSVSET` |
| `os_port_stack_init` | Descending exception frame + R4–R11; 8-byte aligned SP |
| `os_port_task_request` | Critical section + `*Current` helpers; PendSV if caller left `RUNNING` |
| `os_port_start_scheduler` | `os_SchedStart()`, PendSV priority, SysTick, SVC → first task |
| `SysTick_Handler` | `os_TimeTick()` then PendSV if current changed |
| `PendSV_Handler` | Save/restore R4–R11 and PSP via `g_os_port_psp_owner` / `current->sp` |

Sources:

- `port/cortex-m/os_port_cortex_m.c`
- `port/cortex-m/os_port_cortex_m_asm.S`
- `port/cortex-m/os_port_cortex_m.h` (optional config)

## Toolchain

Use an ARM embedded GCC toolchain, for example:

~~~bash
arm-none-eabi-gcc --version
~~~

Typical flags for this port (no FPU):

~~~text
-mcpu=cortex-m3 -mthumb -mfloat-abi=soft
~~~

or for STM32F407 without FPU use:

~~~text
-mcpu=cortex-m4 -mthumb -mfloat-abi=soft
~~~

Do **not** pass `-mfpu=fpv4-sp-d16` / hard-float with this port: the exception
frame layout assumes no extended stack frame.

## CMake integration

Host builds (Linux/macOS/Windows) keep the stub + `test_kernel` path unchanged.
The Cortex-M library is optional:

~~~cmake
option(MINI_RTOS_PORT_CORTEX_M "Build Cortex-M port library" OFF)
~~~

It is also enabled automatically when `CMAKE_SYSTEM_PROCESSOR` matches ARM
(typical for an `arm-none-eabi` toolchain file).

Example cross-build snippet (board firmware CMake, not the host test tree):

~~~cmake
# toolchain: arm-none-eabi.cmake sets CMAKE_SYSTEM_PROCESSOR to arm
set(MINI_RTOS_PORT_CORTEX_M ON)

add_subdirectory(path/to/mini_rtos)   # or FetchContent

target_link_libraries(your_firmware PRIVATE
    mini_rtos_core
    mini_rtos_port_cortex_m
)

# Board/CMSIS must provide startup + SystemInit and the vector table.
~~~

Configure from the mini_rtos tree with a toolchain file:

~~~bash
cmake -S . -B build-arm \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/arm-none-eabi.cmake \
  -DMINI_RTOS_PORT_CORTEX_M=ON
cmake --build build-arm --target mini_rtos_port_cortex_m
~~~

## Required CMSIS / startup symbols

| Symbol / hook | Role |
| --- | --- |
| `SystemCoreClock` | Core clock in Hz used to program SysTick for `OS_TICK_MS`. The port provides a **weak** default (`72 MHz`) so the library links without a board file; real boards should define the strong CMSIS symbol after `SystemInit`. |
| `SysTick_Handler` | Provided by `os_port_cortex_m.c` — leave the vector entry as the default weak CMSIS alias or point it here. |
| `PendSV_Handler` | Provided by `os_port_cortex_m_asm.S`. |
| `SVC_Handler` | Provided by the asm file (first-task bootstrap only). |
| Vector table / `g_pfnVectors` | Startup must place the three handlers above into the table (CMSIS startup does this when the symbols are strong). |
| `SystemInit` | Board clock setup before `os_Start()`; update `SystemCoreClock`. |

If your vendor startup defines weak empty `SysTick_Handler` / `PendSV_Handler` /
`SVC_Handler`, linking this port’s strong definitions is enough.

## Stack and privilege model

- Tasks run in **Thread mode** using **PSP**.
- Handler mode (SysTick / PendSV / SVC) uses **MSP** (reset MSP from `VTOR[0]` at start).
- Initial LR in the hardware frame points at an exit trampoline that issues
  `OS_PORT_REQ_EXIT`.
- Exception return value: `0xFFFFFFFD` (Thread mode, PSP, no FPU frame).

`OS_MIN_STACK_BYTES` (64) is the kernel’s minimum buffer size. On Cortex-M you
usually want a larger static stack (for example 512–2048 bytes) because the
exception frame alone uses 16 words plus locals.

## STM32F103 / STM32F407 checklist

1. Create tasks with static stacks, call `os_Init()`, then `os_Start(0)`.
2. Ensure `SystemCoreClock` matches the PLL/HSE configuration (`72e6` on many
   F103 setups; `168e6` typical on F407).
3. Link `mini_rtos_core` + `mini_rtos_port_cortex_m` with your startup and
   linker script.
4. Do not also link `mini_rtos_port_stub` or `mini_rtos_win32` into the same
   firmware image (duplicate `os_port_*` symbols).
5. Keep IRQs enabled outside `os_EnterCritical` / port critical sections so
   SysTick and PendSV can run.

## Host regression

On a development PC, leave `MINI_RTOS_PORT_CORTEX_M` off (default). Build and
run the stub tests:

~~~bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
~~~

`test_kernel` links only `mini_rtos_core` + `mini_rtos_port_stub`.
