/* SPDX-License-Identifier: 0BSD */
/* Host ARMv7 hard-float AAPCS -> guest base AAPCS with a managed stack.
 * Save Linux callee-saved integer registers (including r9), LR and d8-d15.
 * Both stack tops stay 8-byte aligned. Guest fn keeps its Thumb bit for BLX.
 */
.syntax unified
.arch armv7-a
.fpu vfpv3-d16
.arm
.text
.align 2
.global arm_guest_call
.type arm_guest_call, %function
arm_guest_call:
    push {r4-r11, lr}
    sub sp, sp, #4
    vpush {d8-d15}
    mov r5, sp
    mov r6, r0
    mov r7, r1
    mov r8, r2
    mov r9, r3
    ldr r10, [sp, #104]
    mov sp, r7
    mov r0, r8
    mov r1, r9
    mov r2, r10
    blx r6
    mov sp, r5
    vpop {d8-d15}
    add sp, sp, #4
    pop {r4-r11, pc}
.size arm_guest_call, . - arm_guest_call
.section .note.GNU-stack,"",%progbits
