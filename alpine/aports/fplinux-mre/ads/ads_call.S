/* SPDX-License-Identifier: 0BSD */
/* Host AAPCS/hard-float <-> original ADS/APCS guest, one OS thread.
 * Return uses a linked host frame, not a register the ADS startup can clobber.
 * Layout at ads_active_frame: previous(4), pad(4), d8-d15(64), pad(4),
 * r4-r11(32), lr(4). Saved Linux r9 is therefore at frame+96.
 */
.syntax unified
.arch armv7-a
.fpu vfpv3-d16
.arm
.text
.align 2
.global ads_guest_call
.type ads_guest_call,%function
ads_guest_call:
    push {r4-r11,lr}
    sub sp,sp,#4
    vpush {d8-d15}
    sub sp,sp,#8
    ldr r12,=ads_active_frame
    ldr r4,[r12]
    str r4,[sp]
    str sp,[r12]
    mov r6,r0
    mov r7,r1
    mov r9,r2
    mov r0,r3
    ldr r1,[sp,#112]
    ldr r2,[sp,#116]
    ldr r3,[sp,#120]
    sub r7,r7,#8
    ldr lr,=ads_guest_return
    mov r12,#0
    str r12,[r7]
    str lr,[r7,#4]
    mov sp,r7
    bx r6
.size ads_guest_call,.-ads_guest_call

.global ads_guest_return
.type ads_guest_return,%function
ads_guest_return:
    ldr r12,=ads_active_frame
    ldr sp,[r12]
    ldr r1,[sp]
    str r1,[r12]
    add sp,sp,#8
    vpop {d8-d15}
    add sp,sp,#4
    pop {r4-r11,pc}
.size ads_guest_return,.-ads_guest_return

.bss
.align 2
.global ads_active_frame
.type ads_active_frame,%object
.size ads_active_frame,4
ads_active_frame:
    .word 0
.global ads_active_api_sp
.type ads_active_api_sp,%object
.size ads_active_api_sp,4
ads_active_api_sp:
    .word 0
.section .note.GNU-stack,"",%progbits
