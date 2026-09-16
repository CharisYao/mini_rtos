/**
 * @file os_port_cortex_m_asm.S
 * @brief PendSV context switch and first-task start for Cortex-M3/M4 (no FPU).
 *
 * GNU assembler, unified Thumb-2 syntax. Task stacks use PSP. Context restore
 * uses EXC_RETURN 0xFFFFFFFD (return to Thread mode, use PSP).
 */

    .syntax unified
    .thumb
    .text

    .extern os_port_pendsv_save_psp
    .extern os_port_pendsv_restore_psp
    .extern os_port_enable_systick

/**
 * PendSV_Handler
 *
 * Hardware has already stacked R0-R3, R12, LR, PC, xPSR on the outgoing PSP.
 * This handler saves R4-R11, stores the resulting SP into the PSP-owner TCB,
 * then restores R4-R11 for g_os_kernel.current and exception-returns.
 */
    .global PendSV_Handler
    .type PendSV_Handler, %function
    .thumb_func
PendSV_Handler:
    cpsid   i
    mrs     r0, psp
    isb
    stmdb   r0!, {r4-r11}
    /* Keep MSP 8-byte aligned across AAPCS calls (push two words). */
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
    .size PendSV_Handler, .-PendSV_Handler

/**
 * SVC_Handler
 *
 * Starts the first task: restore its software context, select PSP, enable
 * SysTick, then EXC_RETURN into the prepared hardware frame.
 */
    .global SVC_Handler
    .type SVC_Handler, %function
    .thumb_func
SVC_Handler:
    bl      os_port_pendsv_restore_psp
    ldmia   r0!, {r4-r11}
    msr     psp, r0
    isb
    movs    r0, #2                 /* CONTROL.SPSEL = PSP */
    msr     control, r0
    isb
    bl      os_port_enable_systick
    ldr     r0, =0xFFFFFFFD
    mov     lr, r0
    bx      lr
    .size SVC_Handler, .-SVC_Handler

/**
 * os_port_start_first_task
 *
 * Reset MSP from VTOR[0], enable IRQs (required for SVC), then SVC into the
 * first-task bootstrap. Does not return.
 */
    .global os_port_start_first_task
    .type os_port_start_first_task, %function
    .thumb_func
os_port_start_first_task:
    ldr     r0, =0xE000ED08        /* SCB->VTOR */
    ldr     r0, [r0]
    ldr     r0, [r0]               /* initial MSP */
    msr     msp, r0
    dsb
    isb
    cpsie   i
    svc     0
1:  b       1b
    .size os_port_start_first_task, .-os_port_start_first_task
