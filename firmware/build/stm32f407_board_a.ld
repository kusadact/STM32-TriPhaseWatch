/* Board A keeps the common memory layout but aligns the initial MSP to ABI 8. */
INCLUDE stm32f407.ld

_estack = ORIGIN(RAM) + LENGTH(RAM);
