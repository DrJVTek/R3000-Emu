OUTPUT_FORMAT("elf32-littlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS
{
    /* Typical PS1 homebrew entry in KSEG0 */
    . = 0x80010000;

    .text :
    {
        *(.text.start)
        *(.text*)
    }

    .rodata :
    {
        *(.rodata*)
    }

    .data :
    {
        *(.data*)
    }

    .bss (NOLOAD) :
    {
        *(.bss*)
        *(COMMON)
    }
}

