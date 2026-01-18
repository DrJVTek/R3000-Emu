.set noreorder
.set noat

.globl _start
_start:
    // Set up a simple stack in KSEG0 near top of 2MB RAM.
    lui   $sp, 0x801F
    ori   $sp, $sp, 0xF000

    jal   main
    nop

1:
    j     1b
    nop

