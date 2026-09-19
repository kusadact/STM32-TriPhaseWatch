#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BSP_DIR="$ROOT_DIR/firmware/bsp"
APP_DIR="$ROOT_DIR/firmware/app"
BOARD_DIR="$ROOT_DIR/firmware/board_a"
CORE_DIR="$BOARD_DIR/core"
PLATFORM_DIR="$BOARD_DIR/platform"
BUILD_DIR="$ROOT_DIR/build/board_a"
OBJ_DIR="$BUILD_DIR/obj"
TARGET="$BUILD_DIR/board_a"

TOOLCHAIN_ROOT="${ARM_GNU_TOOLCHAIN_ROOT:-/Applications/ArmGNUToolchain/15.3.rel1/arm-none-eabi/bin}"
if [[ -x "$TOOLCHAIN_ROOT/arm-none-eabi-gcc" ]]; then
  DEFAULT_CC="$TOOLCHAIN_ROOT/arm-none-eabi-gcc"
  DEFAULT_OBJCOPY="$TOOLCHAIN_ROOT/arm-none-eabi-objcopy"
  DEFAULT_SIZE="$TOOLCHAIN_ROOT/arm-none-eabi-size"
else
  DEFAULT_CC="arm-none-eabi-gcc"
  DEFAULT_OBJCOPY="arm-none-eabi-objcopy"
  DEFAULT_SIZE="arm-none-eabi-size"
fi
CC="${CC:-$DEFAULT_CC}"
OBJCOPY="${OBJCOPY:-$DEFAULT_OBJCOPY}"
SIZE="${SIZE:-$DEFAULT_SIZE}"

CPUFLAGS=(-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
INCLUDES=(
  "-I$CORE_DIR"
  "-I$PLATFORM_DIR"
  "-I$APP_DIR"
  "-I$BSP_DIR/CORE"
  "-I$BSP_DIR/USER"
  "-I$BSP_DIR/SYSTEM/sys"
  "-I$BSP_DIR/FWLIB/STM32F4xx_StdPeriph_Driver/inc"
)
CPPFLAGS=(-DSTM32F40_41xxx -DUSE_STDPERIPH_DRIVER
          "-include$APP_DIR/gcc_compat.h" "${INCLUDES[@]}")
CFLAGS=(${CPUFLAGS[@]} -Os -g3 -std=gnu11 -ffunction-sections -fdata-sections
        -Wall -Wextra -Wno-unused-parameter)
BSP_CFLAGS=(-Wno-unused-variable -Wno-deprecated -Wno-attributes -Wno-uninitialized
        -Wno-pointer-sign -Wno-missing-field-initializers -Wno-missing-braces
        -Wno-misleading-indentation -Wno-unknown-pragmas -Wno-maybe-uninitialized
        -Wno-unused-but-set-variable -Wno-pointer-compare
        -Wno-format -Wno-return-type -Wno-implicit-int -Wno-implicit-function-declaration)
LDFLAGS=(${CPUFLAGS[@]} "-T$SCRIPT_DIR/stm32f407.ld"
         --specs=nano.specs --specs=nosys.specs
         "-Wl,--gc-sections,-Map=$TARGET.map" -Wl,--no-warn-rwx-segments)

mkdir -p "$OBJ_DIR"
find "$OBJ_DIR" -maxdepth 1 -type f -delete
find "$BUILD_DIR" -maxdepth 1 -type f \
  \( -name 'board_a.elf' -o -name 'board_a.bin' \
  -o -name 'board_a.hex' -o -name 'board_a.map' \) -delete

sources=(
  "$CORE_DIR/modbus_crc.c"
  "$CORE_DIR/modbus_rtu.c"
  "$CORE_DIR/modbus_rtu_rx.c"
  "$CORE_DIR/board_a_log_schedule.c"
  "$CORE_DIR/board_a_model.c"
  "$CORE_DIR/board_a_slave.c"
  "$PLATFORM_DIR/board_a_main.c"
  "$APP_DIR/debug_uart.c"
  "$APP_DIR/syscalls.c"
  "$BSP_DIR/USER/system_stm32f4xx.c"
)

for source in misc.c stm32f4xx_gpio.c stm32f4xx_rcc.c stm32f4xx_tim.c \
              stm32f4xx_usart.c; do
  sources+=("$BSP_DIR/FWLIB/STM32F4xx_StdPeriph_Driver/src/$source")
done

objects=()
index=0
for source in "${sources[@]}"; do
  object="$OBJ_DIR/$(printf '%04d' "$index").o"
  index=$((index + 1))
  echo "CC  ${source#$ROOT_DIR/}"
  source_cflags=("${CFLAGS[@]}")
  if [[ "$source" == "$BSP_DIR/"* ]]; then
    source_cflags+=("${BSP_CFLAGS[@]}")
  fi
  "$CC" "${CPPFLAGS[@]}" "${source_cflags[@]}" -MMD -MP -c \
    "$source" -o "$object"
  objects+=("$object")
done

startup_object="$OBJ_DIR/startup.o"
echo "AS  firmware/bsp/startup/startup_stm32f40_41xxx.s"
"$CC" "${CPUFLAGS[@]}" -x assembler-with-cpp -c \
  "$BSP_DIR/startup/startup_stm32f40_41xxx.s" -o "$startup_object"
objects+=("$startup_object")

echo "LD  ${TARGET#$ROOT_DIR/}.elf"
"$CC" "${LDFLAGS[@]}" "${objects[@]}" -o "$TARGET.elf" -lc -lm -lnosys
"$OBJCOPY" -O binary "$TARGET.elf" "$TARGET.bin"
"$OBJCOPY" -O ihex "$TARGET.elf" "$TARGET.hex"
"$SIZE" "$TARGET.elf"
echo "Built: ${TARGET#$ROOT_DIR/}.bin"

find "$OBJ_DIR" -maxdepth 1 -type f -delete
rmdir "$OBJ_DIR" 2>/dev/null || true
