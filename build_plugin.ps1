$TOOLCHAIN = "C:/Users/Gianmarco/Projects/video-grab/ps4-store/tools/openorbis/OpenOrbis/PS4Toolchain"
$GOLDHEN_SDK = "C:/Users/Gianmarco/Projects/xbox_controller_plugin-master/sdk/GoldHEN_Plugins_SDK"
$PROJECT = "C:/Users/Gianmarco/Projects/xbox_controller_plugin-master"
$PLUGIN_NAME = "xbox_controller"
$CREATE_FSELF = "C:/tmp/create-fself-new.exe"

if (-not (Test-Path "$PROJECT/obj")) { New-Item -ItemType Directory -Path "$PROJECT/obj" | Out-Null }
if (-not (Test-Path "$PROJECT/bin")) { New-Item -ItemType Directory -Path "$PROJECT/bin" | Out-Null }

$CFLAGS = @(
    "--target=x86_64-pc-freebsd12-elf",
    "-fPIC",
    "-funwind-tables",
    "-fno-delete-null-pointer-checks",
    "-c",
    "-Wall",
    "-isysroot", $TOOLCHAIN,
    "-isystem", "$TOOLCHAIN/include",
    "-isystem", "$TOOLCHAIN/include/orbis/_types",
    "-I$GOLDHEN_SDK/include",
    "-I$PROJECT/include",
    "-D__PS4__",
    "-D__ORBIS__"
)

Write-Host "=== Compiling plugin sources ==="
$srcFiles = Get-ChildItem "$PROJECT/src/*.c"
foreach ($f in $srcFiles) {
    $name = $f.BaseName
    Write-Host "  Compiling $name.c -> obj/$name.o"
    & clang @CFLAGS -o "$PROJECT/obj/$name.o" $f.FullName
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to compile $($f.Name)"
        exit 1
    }
}
Write-Host "All sources compiled OK"

Write-Host "=== Linking ELF ==="
$LDFLAGS = @(
    "-m", "elf_x86_64",
    "-pie",
    "--script", "$TOOLCHAIN/link.x",
    "-e", "_init",
    "--eh-frame-hdr",
    "-L$TOOLCHAIN/lib",
    "-L$GOLDHEN_SDK"
)

$LIBS = @("-lSceLibcInternal", "-lGoldHEN_Hook", "-lkernel")

$objFiles = Get-ChildItem "$PROJECT/obj/*.o" | ForEach-Object { $_.FullName }
$crt = "$GOLDHEN_SDK/build/crtprx.o"
$ELF = "$PROJECT/bin/$PLUGIN_NAME.elf"
$OELF = "$PROJECT/bin/$PLUGIN_NAME.oelf"
$PRX = "$PROJECT/bin/$PLUGIN_NAME.prx"

& ld.lld $crt @objFiles -o $ELF @LDFLAGS @LIBS
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to link ELF"
    exit 1
}
Write-Host "ELF linked OK"

Write-Host "=== Creating PRX (via create-fself) ==="
& $CREATE_FSELF -in "$ELF" -out "$OELF" -lib "$PRX" -paid 0x3800000000000011
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to create PRX"
    exit 1
}

Write-Host "=== Build complete: bin/$PLUGIN_NAME.prx ==="
