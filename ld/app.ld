/*
 * SIU application: app slot, 0x08002000, 26 KB (minus the 4-byte CRC trailer).
 * Flash/RAM layout: common/fw_layout.h. Works with ST's startup_stm32f030x8.s.
 *
 * RAM starts at 0x20000100: the first 192 bytes hold the app's vector table (copied there at
 * start-up, Cortex-M0 has no VTOR) and 0x200000C0 is the bootloader -> app "boot info" word.
 * No heap: static allocation only (ARCHITECTURE.md §5).
 */

ENTRY(Reset_Handler)

_estack = 0x20002000;
_Min_Heap_Size  = 0x000;
_Min_Stack_Size = 0x400;   /* 1 KB main stack — checked at link time below */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08002000, LENGTH = 26K - 4
    RAM   (xrw) : ORIGIN = 0x20000100, LENGTH = 8K - 0x100
}

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
    } > FLASH
    ASSERT(SIZEOF(.isr_vector) == 0xC0, "vector table must be 0xC0 bytes (FW_VECTOR_TABLE_SIZE)")

    /* image header at offset 0xC0 (common/fw_image.h), filled in by tools/mkimage.py */
    .app_header ORIGIN(FLASH) + 0xC0 :
    {
        KEEP(*(.app_header))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH
    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH
    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    /* Fails the link if static data + stack don't fit in RAM. */
    ._user_heap_stack (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > RAM

    /DISCARD/ :
    {
        libc.a(*)
        libm.a(*)
        libgcc.a(*)
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
