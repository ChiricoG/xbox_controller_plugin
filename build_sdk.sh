#!/bin/bash
set -e

OO_PS4_TOOLCHAIN="/c/Users/Gianmarco/Projects/video-grab/ps4-store/tools/openorbis/OpenOrbis/PS4Toolchain"
SDK="/c/Users/Gianmarco/Projects/xbox_controller_plugin-master/sdk/GoldHEN_Plugins_SDK"

cd "$SDK"
mkdir -p build

echo "=== Compiling CRT (crtprx.c) ==="
clang -target x86_64-pc-linux-gnu \
    -ffreestanding -nostdlib -fno-builtin -fPIC \
    -isysroot "$OO_PS4_TOOLCHAIN" \
    -isystem "$OO_PS4_TOOLCHAIN/include" \
    -Iinclude \
    -c crt/crtprx.c -o build/crtprx.o
echo "crtprx.o OK"

echo "=== Compiling SDK sources ==="
for f in source/*.c; do
    name=$(basename "$f" .c)
    echo "  Compiling $f -> build/${name}.o"
    clang --target=x86_64-pc-freebsd12-elf \
        -DORBIS=1 -D__ORBIS__=1 -DPS4=1 -D__PS4__=1 \
        -D__BSD_VISIBLE=1 -D_BSD_SOURCE=1 \
        -march=btver2 -mtune=btver2 \
        -fPIC -std=c11 \
        -isysroot "$OO_PS4_TOOLCHAIN" \
        -isystem "$OO_PS4_TOOLCHAIN/include" \
        -Iinclude \
        -c "$f" -o "build/${name}.o"
done
echo "All SDK sources compiled OK"

echo "=== Creating libGoldHEN_Hook.a ==="
llvm-ar --format=bsd rcs libGoldHEN_Hook.a build/*.o
echo "libGoldHEN_Hook.a created OK"

echo ""
echo "SDK build complete!"
ls -lh libGoldHEN_Hook.a build/crtprx.o
