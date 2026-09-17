cmake_minimum_required(VERSION 3.20)

# Board-local firmware build: CubeMX/HAL under this tree + mini_rtos core/port.
# Does not replace the host stub tests at the repo root.
project(boardVerify C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # CMSIS/HAL use GNU extensions

set(MINI_RTOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# ---- mini_rtos core + Cortex-M port (same sources as root CMake) ------------
# Enable the port library explicitly (also auto-on when CMAKE_SYSTEM_PROCESSOR
# is arm via the toolchain file).
set(MINI_RTOS_PORT_CORTEX_M ON CACHE BOOL "Build Cortex-M port library" FORCE)

add_library(mini_rtos_core STATIC
    ${MINI_RTOS_ROOT}/kernel/src/os_list.c
    ${MINI_RTOS_ROOT}/kernel/src/os_task.c
    ${MINI_RTOS_ROOT}/kernel/src/os_sched.c
    ${MINI_RTOS_ROOT}/kernel/src/os_time.c
    ${MINI_RTOS_ROOT}/kernel/src/os_sem.c
    ${MINI_RTOS_ROOT}/kernel/src/os_queue.c
    ${MINI_RTOS_ROOT}/kernel/src/os_mutex.c
    ${MINI_RTOS_ROOT}/kernel/src/os_api.c
)

target_include_directories(mini_rtos_core
    PUBLIC
        ${MINI_RTOS_ROOT}/include
    PRIVATE
        ${MINI_RTOS_ROOT}/kernel/include
)

target_compile_options(mini_rtos_core PRIVATE -Wall -Wextra)

add_library(mini_rtos_port_cortex_m STATIC
    ${MINI_RTOS_ROOT}/port/cortex-m/os_port_cortex_m.c
    ${MINI_RTOS_ROOT}/port/cortex-m/os_port_cortex_m_asm.S
)

target_include_directories(mini_rtos_port_cortex_m
    PUBLIC
        ${MINI_RTOS_ROOT}/include
        ${MINI_RTOS_ROOT}/port/cortex-m
    PRIVATE
        ${MINI_RTOS_ROOT}/kernel/include
)

target_link_libraries(mini_rtos_port_cortex_m PUBLIC mini_rtos_core)

target_compile_options(mini_rtos_port_cortex_m PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra>
)

# ---- CubeMX / HAL / CMSIS board application ---------------------------------
set(BOARD_HAL_SRC
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_cortex.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_dma.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_exti.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash_ex.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio_ex.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_pwr.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc_ex.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim_ex.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_uart.c
)

set(BOARD_APP_SRC
    Core/Src/main.c
    mini_rtos.c
    Core/Src/gpio.c
    Core/Src/usart.c
    Core/Src/stm32f1xx_it.c
    Core/Src/stm32f1xx_hal_msp.c
    Core/Src/stm32f1xx_hal_timebase_tim.c
    Core/Src/system_stm32f1xx.c
    Core/Startup/startup_stm32f103xb.s
    syscalls.c
)

add_executable(boardVerify.elf
    ${BOARD_APP_SRC}
    ${BOARD_HAL_SRC}
)

target_include_directories(boardVerify.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    Core/Inc
    Drivers/STM32F1xx_HAL_Driver/Inc
    Drivers/STM32F1xx_HAL_Driver/Inc/Legacy
    Drivers/CMSIS/Device/ST/STM32F1xx/Include
    Drivers/CMSIS/Include
    ${MINI_RTOS_ROOT}/include
    ${MINI_RTOS_ROOT}/kernel/include
)

target_compile_definitions(boardVerify.elf PRIVATE
    USE_HAL_DRIVER
    STM32F103xB
)

target_compile_options(boardVerify.elf PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -ffunction-sections -fdata-sections>
    $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp>
)

target_link_libraries(boardVerify.elf PRIVATE
    mini_rtos_core
    mini_rtos_port_cortex_m
)

target_link_options(boardVerify.elf PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/STM32F103XB_FLASH.ld
    -Wl,--gc-sections
    -Wl,-Map=$<TARGET_FILE_DIR:boardVerify.elf>/boardVerify.map
    --specs=nano.specs
    --specs=nosys.specs
)

# Post-build size report
add_custom_command(TARGET boardVerify.elf POST_BUILD
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:boardVerify.elf>
    COMMENT "arm-none-eabi-size boardVerify.elf"
)
