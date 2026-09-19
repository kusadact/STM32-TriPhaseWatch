#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BSP_DIR="$ROOT_DIR/firmware/bsp"
APP_DIR="$ROOT_DIR/firmware/app"
BUILD_DIR="$ROOT_DIR/build"
OBJ_DIR="$BUILD_DIR/obj"

TARGET_KIND="${1:-blank}"
case "$TARGET_KIND" in
  blank)
    APP_MAIN="$APP_DIR/main.c"
    TARGET_NAME="buscomm"
    ;;
  sd)
    APP_MAIN="$APP_DIR/sdcard/main.c"
    TARGET_NAME="buscomm_sd"
    ;;
  *)
    echo "usage: $0 [blank|sd]" >&2
    exit 2
    ;;
esac
TARGET_BASE="${TARGET_NAME}${TARGET_SUFFIX:-}"
TARGET="$BUILD_DIR/$TARGET_BASE"

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
  "-I$APP_DIR"
  "-I$BSP_DIR/CORE"
  "-I$BSP_DIR/USER"
  "-I$BSP_DIR/SYSTEM/delay"
  "-I$BSP_DIR/SYSTEM/sys"
  "-I$BSP_DIR/FWLIB/STM32F4xx_StdPeriph_Driver/inc"
  "-I$BSP_DIR/HARDWARE/LED"
  "-I$BSP_DIR/HARDWARE/KEY"
  "-I$BSP_DIR/HARDWARE/BEEP"
)
if [[ "$TARGET_KIND" == "sd" ]]; then
  INCLUDES+=("-I$APP_DIR/sdcard" "-I$BSP_DIR/FATFS")
fi
CPPFLAGS=(-DSTM32F40_41xxx -DUSE_STDPERIPH_DRIVER
          "-include$APP_DIR/gcc_compat.h" "${INCLUDES[@]}")
CFLAGS=(${CPUFLAGS[@]} -Os -g3 -std=gnu11 -ffunction-sections -fdata-sections
        -Wall -Wextra -Wno-unused-parameter)
# Vendor BSP sources predate this toolchain; keep their diagnostics relaxed.
BSP_CFLAGS=(-Wno-unused-variable -Wno-deprecated -Wno-attributes -Wno-uninitialized
        -Wno-pointer-sign -Wno-missing-field-initializers -Wno-missing-braces
        -Wno-misleading-indentation -Wno-unknown-pragmas -Wno-maybe-uninitialized
        -Wno-unused-but-set-variable -Wno-pointer-compare
        -Wno-format -Wno-return-type -Wno-implicit-int -Wno-implicit-function-declaration)
LDFLAGS=(${CPUFLAGS[@]} "-T$SCRIPT_DIR/stm32f407.ld" --specs=nano.specs --specs=nosys.specs
         "-Wl,--gc-sections,-Map=$TARGET.map" -Wl,--no-warn-rwx-segments)

mkdir -p "$OBJ_DIR"
find "$OBJ_DIR" -maxdepth 1 -type f -delete
find "$BUILD_DIR" -maxdepth 1 -type f \( -name "${TARGET_BASE}.elf" -o -name "${TARGET_BASE}.bin" \
  -o -name "${TARGET_BASE}.hex" -o -name "${TARGET_BASE}.map" \) -delete

sources=(
  "$APP_MAIN"
  "$APP_DIR/debug_uart.c"
  "$APP_DIR/delay_port.c"
  "$APP_DIR/syscalls.c"
  "$BSP_DIR/USER/system_stm32f4xx.c"
  "$BSP_DIR/HARDWARE/LED/led.c"
)

if [[ "$TARGET_KIND" == "sd" ]]; then
  sources+=(
    "$APP_DIR/sdcard/sd_spi.c"
    "$APP_DIR/sdcard/sd_spi_bus.c"
    "$APP_DIR/sdcard/sd_diskio.c"
    "$APP_DIR/sdcard/sd_selftest.c"
    "$BSP_DIR/FATFS/ff.c"
  )
fi

for source in \
  misc.c stm32f4xx_gpio.c stm32f4xx_rcc.c stm32f4xx_usart.c; do
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
"$CC" "${LDFLAGS[@]}" "${objects[@]}" -o "$TARGET.elf" -lc -lm -lnosys
"$OBJCOPY" -O binary "$TARGET.elf" "$TARGET.bin"
"$OBJCOPY" -O ihex "$TARGET.elf" "$TARGET.hex"
"$SIZE" "$TARGET.elf"
echo "Built: $TARGET.bin"
find "$OBJ_DIR" -maxdepth 1 -type f -delete
rmdir "$OBJ_DIR" 2>/dev/null || true
