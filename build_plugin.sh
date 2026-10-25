#!/bin/bash
set -e

OO_PS4_TOOLCHAIN="/c/Users/Gianmarco/Projects/video-grab/ps4-store/tools/openorbis/OpenOrbis/PS4Toolchain"
GOLDHEN_SDK="/c/Users/Gianmarco/Projects/xbox_controller_plugin-master/sdk/GoldHEN_Plugins_SDK"
PROJECT="/c/Users/Gianmarco/Projects/xbox_controller_plugin-master"
PLUGIN_NAME="xbox_controller"

# Windows-native paths for create-fself.exe
WIN_PROJECT="C:\\Users\\Gianmarco\\Projects\\xbox_controller_plugin-master"
WIN_TOOLCHAIN="C:\\Users\\Gianmarco\\Projects\\video-grab\\ps4-store\\tools\\openorbis\\OpenOrbis\\PS4Toolchain"

CFLAGS="--target=x86_64-pc-freebsd12-elf"
CFLAGS="$CFLAGS -fPIC -funwind-tables -fno-delete-null-pointer-checks -c -Wall"
CFLAGS="$CFLAGS -isysroot $OO_PS4_TOOLCHAIN"
CFLAGS="$CFLAGS -isystem $OO_PS4_TOOLCHAIN/include"
CFLAGS="$CFLAGS -isystem $OO_PS4_TOOLCHAIN/include/orbis/_types"
CFLAGS="$CFLAGS -I$GOLDHEN_SDK/include"
CFLAGS="$CFLAGS -I$PROJECT/include"
CFLAGS="$CFLAGS -D__PS4__ -D__ORBIS__"

LDFLAGS="-m elf_x86_64 -pie"
LDFLAGS="$LDFLAGS --script $OO_PS4_TOOLCHAIN/link.x"
LDFLAGS="$LDFLAGS -e _init --eh-frame-hdr"
LDFLAGS="$LDFLAGS -L$OO_PS4_TOOLCHAIN/lib"
LDFLAGS="$LDFLAGS -L$GOLDHEN_SDK"

LIBS="-lSceLibcInternal -lGoldHEN_Hook -lkernel"

mkdir -p "$PROJECT/bin"
mkdir -p "$PROJECT/obj"

echo "=== Compiling plugin sources ==="
for f in "$PROJECT/src/"*.c; do
    name=$(basename "$f" .c)
    echo "  Compiling $name.c -> obj/${name}.o"
    clang $CFLAGS -o "$PROJECT/obj/${name}.o" "$f"
done
echo "All sources compiled OK"

echo "=== Linking ELF ==="
ld.lld $GOLDHEN_SDK/build/crtprx.o \
    "$PROJECT/obj/"*.o \
    -o "$PROJECT/bin/${PLUGIN_NAME}.elf" \
    $LDFLAGS $LIBS
echo "ELF linked OK"

echo "=== Creating PRX (via create-fself.exe) ==="
# Use freshly compiled create-fself to avoid Windows file permission issues
"/c/tmp/create-fself-new.exe" \
    -in "${WIN_PROJECT}\\bin\\${PLUGIN_NAME}.elf" \
    -out "${WIN_PROJECT}\\bin\\${PLUGIN_NAME}.oelf" \
    -lib "${WIN_PROJECT}\\bin\\${PLUGIN_NAME}.prx" \
    -paid 0x3800000000000011
echo "PRX created!"

echo ""
echo "=== Build complete! ==="
ls -lh "$PROJECT/bin/"
