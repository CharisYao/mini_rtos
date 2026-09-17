; os_port_cortex_m_asm_keil.s — PendSV / SVC / first-task start (Cortex-M3/M4, no FPU)
; ARM Assembler (armasm / armclang -masm=armasm) for Keil MDK.
; GNU twin: os_port_cortex_m_asm.S (used by CMake + arm-none-eabi-gcc).

                AREA    |.text|, CODE, READONLY, ALIGN=2
                THUMB
                REQUIRE8
                PRESERVE8

                EXTERN  os_port_pendsv_save_psp
                EXTERN  os_port_pendsv_restore_psp
                EXTERN  os_port_enable_systick

                EXPORT  PendSV_Handler
                EXPORT  SVC_Handler
                EXPORT  os_port_start_first_task

PendSV_Handler  PROC
                cpsid   i
                mrs     r0, psp
                isb
                stmdb   r0!, {r4-r11}
                ; Keep MSP 8-byte aligned across AAPCS calls (push two words).
                push    {r3, lr}
                bl      os_port_pendsv_save_psp
                bl      os_port_pendsv_restore_psp
                ldmia   r0!, {r4-r11}
                msr     psp, r0
                isb
                pop     {r3, lr}
                cpsie   i
                ldr     r0, =0xFFFFFFFD
                mov     lr, r0
                bx      lr
                ENDP

SVC_Handler     PROC
                bl      os_port_pendsv_restore_psp
                ldmia   r0!, {r4-r11}
                msr     psp, r0
                isb
                movs    r0, #2                 ; CONTROL.SPSEL = PSP
                msr     control, r0
                isb
                bl      os_port_enable_systick
                ldr     r0, =0xFFFFFFFD
                mov     lr, r0
                bx      lr
                ENDP

os_port_start_first_task PROC
                ldr     r0, =0xE000ED08        ; SCB->VTOR
                ldr     r0, [r0]
                ldr     r0, [r0]               ; initial MSP
                msr     msp, r0
                dsb
                isb
                cpsie   i
                svc     #0
os_port_start_first_task_hang
                b       os_port_start_first_task_hang
                ENDP

                ALIGN
                END
