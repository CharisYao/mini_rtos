# boardVerify — STM32F103 + mini_rtos (arm-none-eabi)

CubeMX/HAL template for STM32F103C8 (`STM32F103xB`) with TIM4 as the HAL
timebase so **SysTick is free for mini_rtos**. Firmware is built with
**arm-none-eabi-gcc** (not Keil MDK).

Phase C board smoke: three LEDs + USART1 status using `os_Init` /
`os_TaskCreate` / `os_Start` plus semaphore, mutex, and queue.

## Prerequisites

```bash
arm-none-eabi-gcc --version   # e.g. 14.x from apt package gcc-arm-none-eabi
cmake --version               # >= 3.20
```

Optional: `ninja-build` (or use the default Unix Makefiles generator).

## Build (CMake + arm-none-eabi — preferred)

From the **repository root**:

```bash
# CMAKE_TOOLCHAIN_FILE must be absolute (or relative to the build dir).
cmake -S boardVerify -B boardVerify/build \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/boardVerify/cmake/arm-none-eabi.cmake" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build boardVerify/build -j
```

Or from `boardVerify/`:

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/arm-none-eabi.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Output ELF:

```text
boardVerify/build/boardVerify.elf
```

Size report (also printed post-build):

```bash
arm-none-eabi-size boardVerify/build/boardVerify.elf
```

## Keil MDK vs CMake

| Path | Role |
| --- | --- |
| **CMake + arm-none-eabi** | Primary build for Linux/CI; produces `boardVerify/build/boardVerify.elf` |
| `MDK-ARM/` | Original CubeMX Keil project kept for reference only |

Prefer CMake. If you open the Keil project, mirror the same USER CODE in
`main.c`, keep TIM4 as HAL timebase, and do **not** redefine
`SVC_Handler` / `PendSV_Handler` / `SysTick_Handler` (those come from
`port/cortex-m`).

## Pin mapping (CubeMX `boardVerify.ioc`)

Preferred Blue-Pill-style pins (PC13 / PA0 / PA1) were **not** used because
this template already wires LEDs differently. Match the CubeMX GPIO labels:

| Signal | Pin | Notes |
| --- | --- | --- |
| LED1 (heartbeat / idle) | **PA2** | Active-low (ON = drive low) |
| LED2 (high-prio worker busy) | **PB10** | Active-low |
| LED3 (sync event) | **PB11** | Active-low; short pulse on sem+queue |
| USART1 TX | **PA9** | 115200 8N1 |
| USART1 RX | **PA10** | 115200 8N1 |
| SWD | PA13 / PA14 | debug |
| HAL tick | TIM4 IRQ | `HAL_IncTick()`; SysTick left to RTOS |

## Expected LED / UART behavior

After reset, the smoke creates five user tasks (plus automatic Idle):

| Task | Prio | Behavior |
| --- | --- | --- |
| `heartbeat` | 1 | Toggles **LED1** every ~500 ms (low-freq heartbeat)
 | `worker` | 5 | **LED2** on during a short busy spin, off while delayed |
 | `producer` | 2 | Sends a queue item and `os_SemGive` ~every 300 ms |
 | `consumer` | 3 | `os_SemTake` + `os_QueueReceive`; pulses **LED3** |
 | `status` | 2 | ~1 Hz USART1 line (mutex-protected TX) |

Example UART line (115200 8N1 on PA9):

```text
boardVerify smoke: LEDs + UART + mini_rtos
tick=100 task=status hb=2 wk=4 prod=3 cons=3 sync=3 q=0
```

Counters: `hb` heartbeat toggles, `wk` worker cycles, `prod`/`cons`/`sync`
queue+sem events, `q` current queue depth. `task=` is the name of whoever was
RUNNING when the status snapshot was taken (often `status` or `worker`).

## What this links

| Piece | Role |
| --- | --- |
| CubeMX `Core/` + `Drivers/` | HAL, CMSIS device, TIM4 tick, USART1, GPIO LEDs |
| `Core/Startup/startup_stm32f103xb.s` | GNU ARM startup / vector table |
| `STM32F103XB_FLASH.ld` | 64K FLASH / 20K RAM (F103C8) |
| `mini_rtos_core` | Kernel from repo `kernel/` + `include/` |
| `mini_rtos_port_cortex_m` | `port/cortex-m` (PendSV / SysTick / SVC) |

CPU flags: `-mcpu=cortex-m3 -mthumb -mfloat-abi=soft`.

## SysTick / vector ownership

- HAL uses **TIM4** (`Core/Src/stm32f1xx_hal_timebase_tim.c`) for `HAL_IncTick()`.
- Empty Cube stubs for `SVC_Handler`, `PendSV_Handler`, and `SysTick_Handler`
  were removed from `Core/Src/stm32f1xx_it.c` so the strong symbols from
  `port/cortex-m` are linked into the vector table.
- If you re-generate from CubeMX, delete those three strong empty handlers
  again (or make them weak). Keep TIM timebase selected in CubeMX.

## Host tests (unchanged)

Repo-root CMake (native gcc) still builds stub/`test_kernel` only:

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Do not point the host build at this board toolchain file.

## Keil (MDK-ARM)

Open `MDK-ARM/boardVerify.uvprojx`. Groups already include:

- app: `Core/Src/*`, `mini_rtos.c`
- `mini_rtos/kernel` → `../../kernel/src/*.c`
- `mini_rtos/port` → `os_port_cortex_m.c` + `os_port_cortex_m_asm_keil.s` (armasm for Keil)

Include paths cover `../../include`, `../../kernel/include`, `../../port/cortex-m`.

CMake/`arm-none-eabi-gcc` uses GNU `os_port_cortex_m_asm.S` instead of the Keil `.s`.

