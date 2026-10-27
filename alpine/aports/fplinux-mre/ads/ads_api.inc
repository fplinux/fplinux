/* SPDX-License-Identifier: 0BSD */
/* Include from an ARM assembly unit alongside ads_call.S.
 * ADS_API public_wrapper, base_aapcs_host_function, exact_stack_argument_words
 * E.g. ADS_API ads_vm_graphic_fill_rect, host_fill_rect, 2
 * Host functions must use pcs("aapcs") for the original soft/base ABI.
 * Every API address returned to ADS (including its resolver) needs a wrapper.
 * Stack words must match the SDK signature; avoid varargs until a dedicated
 * adapter exists. The wrapper preserves guest SB/r4-r11 and calls C on the
 * aligned saved Linux stack with the original Linux r9 restored.
 */
.macro ADS_API wrapper, host_function, stack_words=0
    .if \stack_words < 0 || \stack_words > 16
        .error "ADS API wrapper supports 0..16 explicit stack words"
    .endif
    .arm
    .align 2
    .global \wrapper
    .type \wrapper,%function
\wrapper:
    push {r4-r11,lr}
    sub sp,sp,#4
    mov r4,sp
    ldr r6,=ads_active_api_sp
    ldr r5,[r6]
    str r4,[r6]
    ldr r12,=ads_active_frame
    ldr r12,[r12]
    sub sp,r12,#(((\stack_words*4)+7)&~7)
    ldr r9,[r12,#96]
    .set ads_arg_word,0
    .rept \stack_words
        ldr r12,[r4,#(40+ads_arg_word*4)]
        str r12,[sp,#(ads_arg_word*4)]
        .set ads_arg_word,ads_arg_word+1
    .endr
    bl \host_function
    ldr r6,=ads_active_api_sp
    str r5,[r6]
    mov sp,r4
    add sp,sp,#4
    pop {r4-r11,pc}
    .size \wrapper,.-\wrapper
.endm
