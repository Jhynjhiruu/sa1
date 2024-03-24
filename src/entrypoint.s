.include "macro.inc"

#include "stack.h"

.section .text, "ax"

glabel entrypoint
    la  $t0, __bss_start
    la  $t1, __bss_size

clear_loop:
    sw $zero, 0($t0)
    sw $zero, 4($t0)

    addi $t0, $t0, 8
    addi $t1, $t1, -8

    bnez $t1, clear_loop

done:
    la $sp, (bootStack + STACK_SIZE)
    la $t2, boot
    jalr $t2

infloop:
    b .
