# Xbox Controller Plugin for PS4 (GoldHEN)
# Makefile for OpenOrbis Toolchain

# ============================================
# Configuration
# ============================================

PLUGIN_NAME := xbox_controller

# OpenOrbis toolchain path
OO_PS4_TOOLCHAIN ?= /home/doug/openorbis-toolchain

# GoldHEN SDK path
GOLDHEN_SDK ?= /home/doug/xbox_controller_plugin/sdk/GoldHEN_Plugins_SDK

# ============================================
# Toolchain
# ============================================

CC      := clang
CCX     := clang++
LD      := ld.lld

ifeq ($(OS),Windows_NT)
    CDIR := windows
    EXE  := .exe
else
    CDIR := linux
    EXE  :=
endif

# ============================================
# Directories
# ============================================

SRC_DIR     := src
INC_DIR     := include
BIN_DIR     := bin
OBJ_DIR     := obj

# ============================================
# Source Files
# ============================================

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

# ============================================
# Output Files
# ============================================

TARGET_ELF  := $(BIN_DIR)/$(PLUGIN_NAME).elf
TARGET_OELF := $(BIN_DIR)/$(PLUGIN_NAME).oelf
TARGET_PRX  := $(BIN_DIR)/$(PLUGIN_NAME).prx

# ============================================
# Compiler Flags (matching working GoldHEN plugins)
# ============================================

CFLAGS := --target=x86_64-pc-freebsd12-elf
CFLAGS += -fPIC
CFLAGS += -funwind-tables
CFLAGS += -c
CFLAGS += -Wall
CFLAGS += -isysroot $(OO_PS4_TOOLCHAIN)
CFLAGS += -isystem $(OO_PS4_TOOLCHAIN)/include
CFLAGS += -isystem $(OO_PS4_TOOLCHAIN)/include/orbis/_types
CFLAGS += -I$(GOLDHEN_SDK)/include
CFLAGS += -I$(INC_DIR)

# PS4-specific defines
CFLAGS += -D__PS4__
CFLAGS += -D__ORBIS__
CFLAGS += -fno-delete-null-pointer-checks
CFLAGS += -MMD -MP

# ============================================
# Linker Flags (matching working GoldHEN plugins)
# ============================================

LDFLAGS := -m elf_x86_64
LDFLAGS += -pie
LDFLAGS += --script $(OO_PS4_TOOLCHAIN)/link.x
LDFLAGS += -e _init
LDFLAGS += --eh-frame-hdr
LDFLAGS += -L$(OO_PS4_TOOLCHAIN)/lib
LDFLAGS += -L$(GOLDHEN_SDK)

# Libraries (libScePad, libSceUsbd, libSceUserService are loaded & resolved dynamically via sys_dynlib_dlsym)
LIBS := -lSceLibcInternal
LIBS += -lGoldHEN_Hook
LIBS += -lkernel

# ============================================
# Build Rules
# ============================================

.PHONY: all clean install dirs sdk

all: dirs sdk $(TARGET_PRX)
	@echo ""
	@echo "Build complete: $(TARGET_PRX)"
	@echo ""

dirs:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(OBJ_DIR)

# Build SDK if needed
sdk:
	@if [ ! -f $(GOLDHEN_SDK)/libGoldHEN_Hook.a ]; then \
		echo "Building GoldHEN SDK..."; \
		cd $(GOLDHEN_SDK) && OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) make; \
	fi

# Link ELF (using pre-built crtprx.o from SDK)
$(TARGET_ELF): $(OBJS)
	@echo "Linking $(TARGET_ELF)..."
	$(LD) $(GOLDHEN_SDK)/build/crtprx.o $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

# Create PRX
$(TARGET_PRX): $(TARGET_ELF)
	@echo "Creating PRX..."
	$(OO_PS4_TOOLCHAIN)/bin/$(CDIR)/create-fself$(EXE) -in=$< -out=$(TARGET_OELF) --lib=$@ --paid 0x3800000000000011

# Compile plugin source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -o $@ $<

# ============================================
# Clean
# ============================================

clean:
	@echo "Cleaning..."
	rm -rf $(OBJ_DIR)
	rm -rf $(BIN_DIR)

# ============================================
# Install (FTP to PS4)
# ============================================

PS4_IP ?= 192.168.1.123
PS4_FTP_PORT ?= 2121
PS4_FTP_USER ?= ps4
PS4_FTP_PASS ?= ps4

install: $(TARGET_PRX)
	@echo "Uploading to PS4 at $(PS4_IP)..."
	curl -u $(PS4_FTP_USER):$(PS4_FTP_PASS) -T $(TARGET_PRX) ftp://$(PS4_IP):$(PS4_FTP_PORT)/data/GoldHEN/plugins/
	@echo ""
	@echo "Upload complete!"

# ============================================
# Debug build
# ============================================

debug: CFLAGS := $(filter-out -O2,$(CFLAGS)) -DDEBUG_NOTIFICATIONS=1 -g -O0
debug: all

-include $(wildcard $(OBJ_DIR)/*.d)

# ============================================
# Help
# ============================================

help:
	@echo "Xbox Controller Plugin Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build the plugin (default)"
	@echo "  clean    - Remove build artifacts"
	@echo "  install  - Upload to PS4 via FTP"
	@echo "  debug    - Build with debug output"
	@echo "  help     - Show this help"
	@echo ""
	@echo "Environment Variables:"
	@echo "  OO_PS4_TOOLCHAIN  - OpenOrbis path (default: /home/doug/openorbis-toolchain)"
	@echo "  GOLDHEN_SDK       - GoldHEN SDK path"
	@echo "  PS4_IP            - PS4 IP for install (default: 192.168.1.123)"
