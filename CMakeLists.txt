cmake_minimum_required(VERSION 3.20)

project(mini_rtos VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(mini_rtos_core STATIC
    kernel/src/os_list.c
    kernel/src/os_task.c
    kernel/src/os_sched.c
    kernel/src/os_time.c
    kernel/src/os_sem.c
    kernel/src/os_queue.c
    kernel/src/os_mutex.c
    kernel/src/os_api.c
)

target_include_directories(mini_rtos_core
    PUBLIC
        include
    PRIVATE
        kernel/include
)

if(MSVC)
    target_compile_options(mini_rtos_core PRIVATE /W4 /WX)
else()
    target_compile_options(mini_rtos_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()

add_library(mini_rtos_port_stub STATIC
    port/stub/os_port_stub.c
)

target_include_directories(mini_rtos_port_stub
    PUBLIC
        include
    PRIVATE
        kernel/include
)

if(MSVC)
    target_compile_options(mini_rtos_port_stub PRIVATE /W4 /WX)
else()
    target_compile_options(mini_rtos_port_stub PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(test_kernel
    tests/test_kernel.c
)

target_include_directories(test_kernel PRIVATE
    kernel/include
)

target_link_libraries(test_kernel PRIVATE mini_rtos_core mini_rtos_port_stub)

if(MSVC)
    target_compile_options(test_kernel PRIVATE /W4 /WX)
else()
    target_compile_options(test_kernel PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME kernel_core COMMAND test_kernel)

if(WIN32)
    add_library(mini_rtos_win32 STATIC
        port/win32/os_port_win32.c
    )

    target_include_directories(mini_rtos_win32
        PUBLIC
            include
        PRIVATE
            kernel/include
    )

    target_link_libraries(mini_rtos_win32 PUBLIC mini_rtos_core)

    if(MSVC)
        target_compile_options(mini_rtos_win32 PRIVATE /W4 /WX)
    else()
        target_compile_options(mini_rtos_win32 PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()

    add_executable(preemption_demo
        demos/preemption_demo.c
    )

    target_link_libraries(preemption_demo PRIVATE mini_rtos_win32)

    if(MSVC)
        target_compile_options(preemption_demo PRIVATE /W4 /WX)
    else()
        target_compile_options(preemption_demo PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()

    add_test(NAME preemption_runtime COMMAND preemption_demo --quick)
    set_tests_properties(preemption_runtime PROPERTIES TIMEOUT 20)

    add_test(NAME preemption_stress COMMAND preemption_demo --stress)
    set_tests_properties(preemption_stress PROPERTIES TIMEOUT 40 LABELS stress)

    add_executable(ipc_demo
        demos/ipc_demo.c
    )

    target_link_libraries(ipc_demo PRIVATE mini_rtos_win32)

    if(MSVC)
        target_compile_options(ipc_demo PRIVATE /W4 /WX)
    else()
        target_compile_options(ipc_demo PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()

    add_test(NAME ipc_runtime COMMAND ipc_demo --quick)
    set_tests_properties(ipc_runtime PROPERTIES TIMEOUT 25)
endif()
