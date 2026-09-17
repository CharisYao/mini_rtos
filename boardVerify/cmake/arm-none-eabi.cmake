# Cross-compile toolchain for STM32F103 (Cortex-M3, soft-float).
# Usage:
#   cmake -S boardVerify -B boardVerify/build \
#     -DCMAKE_TOOLCHAIN_FILE=boardVerify/cmake/arm-none-eabi.cmake

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
set(CMAKE_OBJDUMP arm-none-eabi-objdump)
set(CMAKE_SIZE arm-none-eabi-size)
set(CMAKE_AR arm-none-eabi-ar)
set(CMAKE_RANLIB arm-none-eabi-ranlib)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Soft-float Cortex-M3 (no FPU) — required by mini_rtos port/cortex-m.
set(BOARD_CPU_FLAGS "-mcpu=cortex-m3 -mthumb -mfloat-abi=soft")
set(CMAKE_C_FLAGS_INIT "${BOARD_CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${BOARD_CPU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${BOARD_CPU_FLAGS}")
