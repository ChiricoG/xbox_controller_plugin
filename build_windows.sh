#!/bin/bash
# Build script for Windows (MSYS2 / msys2_shell)
#
# Prerequisites:
#   - MSYS2 with clang + lld installed (msys/clang, msys/lld, msys/llvm)
#   - OpenOrbis PS4 Toolchain (set OO_PS4_TOOLCHAIN env var)
#   - create-fself built from https://github.com/OpenOrbis/create-fself
#     and placed at C:/tools/create-fself.exe  (or edit CREATE_FSELF below)
#
# Usage (from MSYS2 bash):
#   export OO_PS4_TOOLCHAIN=/c/path/to/OpenOrbis/PS4Toolchain
#   export CREATE_FSELF=/c/path/to/create-fself.exe
#   bash build_windows.sh
#
# Output: bin/xbox_controller.prx

set -e

: "${OO_PS4_TOOLCHAIN:?Please set OO_PS4_TOOLCHAIN to the OpenOrbis toolchain path}"
: "${CREATE_FSELF:?Please set CREATE_FSELF to the path of create-fself.exe}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GOLDHEN_SDK="$SCRIPT_DIR/sdk/GoldHEN_Plugins_SDK"
PLUGIN_NAME="xbox_controller"

# Convert script dir to Windows path for create-fself.exe
WIN_PROJECT="$(cygpath -w "$SCRIPT_DIR")"

# ─── Build GoldHEN SDK (if not already built) ───────────────────────────────

if [ ! -f "$GOLDHEN_SDK/libGoldHEN_Hook.a" ]; then
    echo "=== Building GoldHEN SDK ==="
    mkdir -p "$GOLDHEN_SDK/build"

    clang -target x86_64-pc-linux-gnu \
        -ffreestanding -nostdlib -fno-builtin -fPIC \
        -isysroot "$OO_PS4_TOOLCHAIN" \
        -isystem "$OO_PS4_TOOLCHAIN/include" \
        -I"$GOLDHEN_SDK/include" \
        -c "$GOLDHEN_SDK/crt/crtprx.c" -o "$GOLDHEN_SDK/build/crtprx.o"

    for f in "$GOLDHEN_SDK/source/"*.c; do
        name="$(basename "$f" .c)"
        clang --target=x86_64-pc-freebsd12-elf \
            -DORBIS=1 -D__ORBIS__=1 -DPS4=1 -D__PS4__=1 \
            -D__BSD_VISIBLE=1 -D_BSD_SOURCE=1 \
            -march=btver2 -mtune=btver2 -fPIC -std=c11 \
            -isysroot "$OO_PS4_TOOLCHAIN" \
            -isystem "$OO_PS4_TOOLCHAIN/include" \
            -I"$GOLDHEN_SDK/include" \
            -c "$f" -o "$GOLDHEN_SDK/build/${name}.o"
    done

    llvm-ar --format=bsd rcs "$GOLDHEN_SDK/libGoldHEN_Hook.a" "$GOLDHEN_SDK/build/"*.o
    echo "GoldHEN SDK built OK"
fi

# ─── Compile plugin sources ──────────────────────────────────────────────────

mkdir -p "$SCRIPT_DIR/bin" "$SCRIPT_DIR/obj"

CFLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c -Wall"
CFLAGS="$CFLAGS -isysroot $OO_PS4_TOOLCHAIN"
CFLAGS="$CFLAGS -isystem $OO_PS4_TOOLCHAIN/include"
CFLAGS="$CFLAGS -isystem $OO_PS4_TOOLCHAIN/include/orbis/_types"
CFLAGS="$CFLAGS -I$GOLDHEN_SDK/include"
CFLAGS="$CFLAGS -I$SCRIPT_DIR/include"
CFLAGS="$CFLAGS -D__PS4__ -D__ORBIS__"

echo "=== Compiling plugin sources ==="
for f in "$SCRIPT_DIR/src/"*.c; do
    name="$(basename "$f" .c)"
    echo "  $name.c"
    clang $CFLAGS -o "$SCRIPT_DIR/obj/${name}.o" "$f"
done

# ─── Link ELF ────────────────────────────────────────────────────────────────

LDFLAGS="-m elf_x86_64 -pie --script $OO_PS4_TOOLCHAIN/link.x -e _init --eh-frame-hdr"
LDFLAGS="$LDFLAGS -L$OO_PS4_TOOLCHAIN/lib -L$GOLDHEN_SDK"
LIBS="-lSceLibcInternal -lGoldHEN_Hook -lkernel -lScePad -lSceUsbd -lSceSysmodule -lSceUserService"

echo "=== Linking ELF ==="
ld.lld "$GOLDHEN_SDK/build/crtprx.o" \
    "$SCRIPT_DIR/obj/"*.o \
    -o "$SCRIPT_DIR/bin/${PLUGIN_NAME}.elf" \
    $LDFLAGS $LIBS

# ─── Generate PRX ────────────────────────────────────────────────────────────

echo "=== Generating PRX ==="
"$CREATE_FSELF" \
    -in "$(cygpath -w "$SCRIPT_DIR/bin/${PLUGIN_NAME}.elf")" \
    -out "$(cygpath -w "$SCRIPT_DIR/bin/${PLUGIN_NAME}.oelf")" \
    -lib "$(cygpath -w "$SCRIPT_DIR/bin/${PLUGIN_NAME}.prx")" \
    -paid 0x3800000000000011

echo ""
echo "=== Build complete: bin/${PLUGIN_NAME}.prx ==="
ls -lh "$SCRIPT_DIR/bin/${PLUGIN_NAME}.prx"
