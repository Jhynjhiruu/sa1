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

#ifdef PATCHED_SK
glabel skMemCopy
/* ABF0 8000BBF0 24020014 */  addiu      $v0, $zero, 0x15
/* ABF4 8000BBF4 3C08A430 */  lui        $t0, (0xA4300014 >> 16)
/* ABF8 8000BBF8 35080014 */  ori        $t0, $t0, (0xA4300014 & 0xFFFF)
/* ABFC 8000BBFC 8D090000 */  lw         $t1, 0x0($t0)
/* AC00 8000BC00 00000000 */  nop
/* AC04 8000BC04 03E00008 */  jr         $ra
/* AC08 8000BC08 00000000 */   nop
/* AC0C 8000BC0C 00000000 */  nop
#endif
