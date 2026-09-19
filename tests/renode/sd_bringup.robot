*** Variables ***
${UART}                       sysbus.usart1
${ELF}                        ${CURDIR}/../../build/buscomm_sd.elf

*** Test Cases ***
SD Bring-Up Reports The Missing Card Without Hanging
    Execute Command           mach create
    Execute Command           machine LoadPlatformDescription @platforms/cpus/stm32f4.repl
    Execute Command           sysbus LoadELF @${ELF}

    Create Terminal Tester    ${UART}

    Start Emulation

    Wait For Line On Uart     SD SPI bring-up
    Wait For Line On Uart     [sd] init FAIL: no card or no R1 after CMD0

    # The STM32F4 model advances SysTick-based delays far slower than real
    # time (~1600x observed), so the 5 s retry window costs thousands of
    # virtual seconds and is not asserted here; see the local SD bring-up notes.
