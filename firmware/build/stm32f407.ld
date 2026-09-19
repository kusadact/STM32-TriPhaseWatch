/* GNU ld layout matching the STM32F407ZG target in USER/LWIP.uvprojx. */

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM) - 4; /* 0x2001FFFC: 4-byte aligned and satisfies legacy (msp & 0x2FFE0000 == 0x20000000) check */
_Min_Heap_Size = 0;
_Min_Stack_Size = 0x1000;

MEMORY
{
  FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  RAM      (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  CCM      (xrw) : ORIGIN = 0x10000000, LENGTH = 64K
  EXT_SRAM (xrw) : ORIGIN = 0x68000000, LENGTH = 1M
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text .text*)
    *(.rodata .rodata*)
    *(.glue_7 .glue_7t)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
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
    KEEP(*(.fini_array*))
    KEEP(*(SORT(.fini_array.*)))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } > FLASH

  /* Copy initialized data from the next available flash address at reset. */
  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data .data*)
    *(.ramfunc .ramfunc*)
    *(.data.ramfunc*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  /* End of the flash-resident image (used by /api/firmware/hash readback). */
  PROVIDE(__firmware_image_end = LOADADDR(.data) + SIZEOF(.data));

  /* Upload diagnostics intentionally survive a software reset so a later
   * HTTP request can explain where a stalled transfer stopped. */
  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.noinit .noinit*)
    . = ALIGN(4);
  } > RAM

  /* These pools are initialized explicitly by mymem_init() after the FSMC
   * external SRAM controller is configured. They must not be copied from
   * flash or cleared by the reset handler. */
  .ccm (NOLOAD) :
  {
    . = ALIGN(4);
    *(.ccm .ccm*)
    . = ALIGN(4);
  } > CCM

  .ccm_map (NOLOAD) :
  {
    . = ALIGN(4);
    *(.ccm_map .ccm_map*)
    . = ALIGN(4);
  } > CCM

  .ext_sram (NOLOAD) :
  {
    . = ALIGN(4);
    *(.ext_sram .ext_sram*)
    . = ALIGN(4);
  } > EXT_SRAM

  .ext_sram_map (NOLOAD) :
  {
    . = ALIGN(4);
    *(.ext_sram_map .ext_sram_map*)
    . = ALIGN(4);
  } > EXT_SRAM

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss .bss* COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } > RAM

  ._user_heap_stack (NOLOAD) :
  {
    . = ALIGN(8);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } > RAM

  PROVIDE(end = _ebss);
  PROVIDE(_end = _ebss);

  /DISCARD/ :
  {
    libc.a(*)
    libm.a(*)
    libgcc.a(*)
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
