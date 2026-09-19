*** Variables ***
${UART}                       sysbus.usart1
${ELF}                        ${CURDIR}/../../build/buscomm.elf

*** Test Cases ***
Blank Firmware Should Print Banner And Alive Counter
    Execute Command           mach create
    Execute Command           machine LoadPlatformDescription @platforms/cpus/stm32f4.repl
    Execute Command           sysbus LoadELF @${ELF}

    Create Terminal Tester    ${UART}

    Start Emulation

    Wait For Line On Uart     blank firmware
    Wait For Line On Uart     alive 0
    Wait For Line On Uart     alive 1
