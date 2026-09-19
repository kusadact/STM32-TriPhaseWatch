#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BSP_DIR="$ROOT_DIR/firmware/bsp"
APP_DIR="$ROOT_DIR/firmware/app"
BOARD_B_DIR="$ROOT_DIR/firmware/board_b"
BUILD_DIR="$ROOT_DIR/build/board_b"
OBJ_DIR="$BUILD_DIR/obj"
TARGET="$BUILD_DIR/buscomm_board_b"

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
  "-I$BOARD_B_DIR"
  "-I$BSP_DIR/CORE"
  "-I$BSP_DIR/USER"
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
LDFLAGS=(${CPUFLAGS[@]} "-T$ROOT_DIR/firmware/build/stm32f407.ld"
         -nostdlib
         "-Wl,--gc-sections,-Map=$TARGET.map" -Wl,--no-warn-rwx-segments)

mkdir -p "$OBJ_DIR"
find "$OBJ_DIR" -maxdepth 1 -type f -delete
find "$BUILD_DIR" -maxdepth 1 -type f \( -name 'buscomm_board_b.elf' \
  -o -name 'buscomm_board_b.bin' -o -name 'buscomm_board_b.hex' \
  -o -name 'buscomm_board_b.map' \) -delete

sources=(
  "$BOARD_B_DIR/main.c"
  "$BOARD_B_DIR/bridge_core.c"
  "$BOARD_B_DIR/bridge_dispatch.c"
  "$BOARD_B_DIR/board_b_rx_queue.c"
  "$BOARD_B_DIR/board_b_io.c"
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
  "$CC" "${CPPFLAGS[@]}" "${source_cflags[@]}" -MMD -MP -c "$source" -o "$object"
  objects+=("$object")
done

startup_object="$OBJ_DIR/startup.o"
echo "AS  firmware/bsp/startup/startup_stm32f40_41xxx.s"
"$CC" "${CPUFLAGS[@]}" -x assembler-with-cpp -c \
  "$BSP_DIR/startup/startup_stm32f40_41xxx.s" -o "$startup_object"
objects+=("$startup_object")

echo "LD  $TARGET.elf"
"$CC" "${LDFLAGS[@]}" "${objects[@]}" -o "$TARGET.elf" -lgcc
"$OBJCOPY" -O binary "$TARGET.elf" "$TARGET.bin"
"$OBJCOPY" -O ihex "$TARGET.elf" "$TARGET.hex"
"$SIZE" "$TARGET.elf"

if command -v shasum >/dev/null 2>&1; then
  shasum -a 256 "$TARGET.bin"
else
  sha256sum "$TARGET.bin"
fi

echo "Built: $TARGET.bin"
find "$OBJ_DIR" -maxdepth 1 -type f -delete
rmdir "$OBJ_DIR" 2>/dev/null || true
