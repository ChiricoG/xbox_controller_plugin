# Changelog

All notable changes to this project are documented in this file.

## [1.4.2] - 2026-10-26

### Added
- **Live Input Tester & Stick Drift Auto-Calibration (Companion App .pkg)**:
  - Added dedicated **Input Tester & Calibration Tab** (Tab 1), effortlessly navigable via **L1 / R1** alongside Settings (Tab 0) and Info & Help (Tab 2).
  - **Live Analog Stick Reticles**: Real-time X/Y coordinate crosshairs for Left and Right sticks with dynamic dashed inner deadzone radius circle overlay, giving instant visual feedback on stick movement, centering, and potential drift.
  - **Analog Triggers & Bumpers**: High-visibility progress bars for LT and RT showing 0-255 raw values and percentages with digital threshold activation tick markers, plus dedicated state pills for LB and RB.
  - **Vibration Feedback Test**: Press **Square** to initiate a 3-second motor verification sequence (Phase 1: Left heavy low-frequency motor, Phase 2: Right light high-frequency motor, Phase 3: Both motors at full power) with live on-screen status updates.
  - **Stick Drift Auto-Calibration**: Press **Triangle** to perform a 3-second (180-frame) rest-position noise measurement. The app automatically computes the lowest optimal deadzone setting required to eliminate drift, allowing one-click application and instant saving to `/data/GoldHEN/xbox_controller.ini` by pressing **Cross**.
  - **Native PlayStation Button Icons**: Included authentic PNG button glyphs (`btn_square.png`, `btn_triangle.png`, `btn_cross_blue.png`, `btn_options.png`) for seamless visual presentation across the UI.
- **Pure Xbox Hardware Isolation via Virtual Handle `1001`**:
  - Introduced dedicated virtual handle `1001` (`XBOX_VIRTUAL_PAD_HANDLE`), accessible by opening user `0x58424F58` or specifying controller type `99` via `scePadOpen`.
  - Enables diagnostic and configuration homebrew tools to read raw, pristine Xbox controller telemetry isolated completely from DualShock 4 inputs, even when running in `MERGE (P1)` mode.
  - Built-in disconnected guard: gracefully displays a warning banner if no Xbox controller is connected to the USB ports.
- **DualShock 4 Rumble Isolation**:
  - Intercepted `scePadSetVibration` on virtual handle `1001`: sending rumble commands to handle `1001` vibrates strictly the Xbox controller without triggering DualShock 4 vibration motors.

### Fixed
- **Homebrew Virtual Handle Detours**:
  - Installed `safe_install_detour32` on `scePadOpen` and `scePadClose` in homebrew process initialization (`g_is_homebrew`), allowing homebrew applications to acquire virtual pad handles like `1001`.
- **Xbox One / Series GIP Rumble Protocol Offsets**:
  - Corrected motor payload byte offsets in `send_rumble_packet()`: Left heavy motor mapped to byte 8, Right light motor mapped to byte 9, and continuous duration mapped to 0xFF.
- **libScePad 32-bit Syscall Trampolines**:
  - Replaced 14-byte 64-bit jump detours with safe 5-byte relative jump trampolines (`safe_install_detour32` within ±2GB allocator) on 11-byte `scePadSetVibration` syscall stubs, resolving skipped hooks on PS4 firmware.
- **UI Footer Label Cleanup**:
  - Removed duplicate "Exit" text in the Input Tester footer.

---

## [1.4.1] - 2026-10-26

### Added
- **Full Homebrew Navigation & Process Classification Engine**:
  - Implemented advanced multi-tier process classification in `src/main.c`, extending Xbox controller support from retail games (`CUSA`, `PCAS`, `PLAS`) to PS4 Homebrew applications (`HBST*`, `APOL*`, `ITEM*`, `EXPL*`, `XBOX*`, etc.).
  - Enables full controller navigation inside Apollo Save Tool, Itemzflow, PS4-Explorer, Homebrew Store, GoldHEN Cheats Manager, DiPlay, and the Xbox Controller Configurator.
  - Resolved `libScePad` internal calling conventions (`scePadReadStateExt`/`scePadReadExt` flags register `flags = 0` and prologue patching) and SDL2 joystick detection via `scePadGetControllerInformation`, allowing seamless simultaneous DualShock 4 and Xbox navigation in `MERGE` mode even if the DualShock 4 is disconnected.
  - Zero-risk architecture: strictly blacklists system daemons and helper processes (`mini-syscore`, `SceSysCore`, `WebProcess`, `compiler`, `ScePartyDaemon`, `SceShellCore`, `NPXS*`), validating `libScePad` availability before hooking to guarantee 100% system stability.
  - Added `enable_homebrew` configuration toggle (`1` = enabled by default, `0` = retail games only).
  - Ensured complete isolation from USB Mass Storage (Class 0x08) so homebrew file managers and PKG installers experience zero drive conflicts.
- **Xbox Controller Configurator (Optional Companion PS4 Homebrew App)**:
  - Developed an optional Fake PKG (FPKG) companion application (`UP9000-XBOX00001_00-XBOXCONFIGURATOR`) for PS4 consoles to graphically customize, save, and reload plugin settings directly on TV without requiring PC or FTP text editing. (The app is completely optional; the plugin works normally without it).
  - Full support for all 10 plugin parameters within a responsive 5-card vertical scrollable viewport with native green scrollbar and position tracking indicator (`1/10` to `10/10`).
  - Automated first-boot initialization: detects and creates `/data/GoldHEN/xbox_controller.ini` with all default settings if missing, or reads and edits the existing file.
  - Real-time on-screen toast confirmation banner on saving.
- **Optional INI Configuration Engine in `xbox_controller.prx`**:
  - Added optional persistent configuration support via `/data/GoldHEN/xbox_controller.ini`.
  - Zero-allocation, static-buffer INI parser (`include/ini_parser.h`, `src/config.c`) ensuring zero dynamic heap fragmentation.
  - 100% backward compatible: if the INI file does not exist, the plugin preserves exact v1.4.0 behavior by default.
  - Configurable settings:
    - `mode`: `auto` (default), `merge`, `secondary`
    - `guide_action`: `touchpad` (default), `share`, `none`
    - `enable_notifications`: `1` (default) or `0`
    - `enable_mode_combo`: `1` (default) or `0`
    - `enable_rumble`: `1` (default) or `0`
    - `enable_homebrew`: `1` (default) or `0` (controller navigation in homebrew apps)
    - `stick_deadzone`: configurable inner analog stick deadzone (`0` to `32`, default `15` = ~`12%`)
    - `trigger_threshold`: configurable digital trigger activation threshold (`0` to `255`, default `30` = ~`12%`)
    - `swap_triggers_bumpers`: inverts bumper and trigger mappings (`0` default, `1` inverted)
    - `switch_positional_layout`: swaps A/B and X/Y for Switch positional layouts (`1` default, `0` by letter)
- **Extended Controller & XInput Compatibility**:
  - Added explicit support and documentation for all standard XInput controllers in addition to Xbox 360, Xbox One, and Xbox Series X|S.
- **Demonstration Media & Hardware Verification**:
  - Updated live PS4 hardware screenshots demonstrating dashboard XMB integration, 10-parameter scrollable configuration editor, save toast notification, and developer information panel.

---

## [1.4.0] - 2026-10-25

### Added
- **Global Game Support under `[default]` with Homebrew Protection**:
  - The plugin can now be placed globally under `[default]` in `/data/GoldHEN/plugins.ini`, automatically activating for any PlayStation 4 retail game without needing per-title `[CUSAXXXXX]` entries.
  - Implemented `is_game_process()` title-id filtering (`CUSA`, `PCAS`, `PLAS`) which cleanly bypasses non-game processes (Homebrew utilities like Apollo Save Tool, GoldHEN Cheats Manager, Itemzflow, PS4-Explorer, as well as background daemons). Homebrew apps launch instantaneously without any crashes.
- **Central Xbox Guide / Home Button Mapped to Touchpad Click**:
  - Pressing the central Xbox Guide button (glowing Xbox logo) or Switch Home button instantly triggers **`DS4_BUTTON_TOUCHPAD`** (Touchpad Click). Players can now open Maps, Inventories, Quest Logs, and Journals with a single natural button press on Xbox controllers without needing to hold View for 2 seconds.
- **Automatic `MERGE` and `SECONDARY` Modes**:
  - `AUTO` mode (default): automatically selects **`MERGE` (Player 1)** when 1 user profile is logged in on the PS4, or **`SECONDARY` (Player 2)** on virtual handle `1001` when 2 or more users are logged in.
  - `MERGE` mode: overlays Xbox controller inputs onto the primary DualShock 4 handle (logical `OR` on buttons, furthest analog deflection from center `128`, maximum trigger pressure) while preserving physical DS4 motion sensors, lightbar, and orientation. Allows playing with the Xbox controller even if the DualShock 4 is turned off after launching the game.
  - `SECONDARY` mode: assigns virtual handle `1001` to the secondary user profile for native split-screen and local multiplayer / co-op gaming.
- **Runtime Mode Switch Combo**:
  - Hold **`LB + RB + Back/View + Start/Menu`** for **1.5 seconds** to cycle modes at runtime (`AUTO` → `SECONDARY (P2)` → `MERGE (P1)` → `AUTO`) with on-screen GoldHEN notifications without editing any files.
- **Explicit Player Connection Notifications**:
  - On controller connection, the notification explicitly reports player assignment based on active user state (e.g., `"Xbox One [Player 1] connected!"` or `"Xbox One [Player 2] connected!"`).
- **USB Hotplugging & Dedicated 250 Hz Polling**:
  - Dedicated background thread (`scePthreadCreate`, 250 Hz) decouples USB transfers from game rendering hooks, providing ultra-low input latency (~4 ms) and seamless hotplugging (controllers can be plugged in, unplugged, or swapped at any time mid-game).
- **Rumble / Vibration Forwarding**:
  - Intercepts `scePadSetVibration` across both `MERGE` and `SECONDARY` modes and sends vibration reports to Xbox 360 and Xbox One/Series (GIP) controllers on value change, automatically clearing vibration upon controller disconnect or pad closure.
- **Dynamic Controller & Descriptor Detection**:
  - Dynamic detection based on USB interface class, subclass, and protocol in addition to explicit PID tables, supporting official Microsoft Xbox 360, Xbox One, Xbox Series X|S (including 2021 model `0x0B20`), PDP, PowerA, and Nintendo Switch (PDP) controllers.
- **Dynamic Symbol Resolution & Zero Static Relocations**:
  - Eliminated static `-lScePad -lSceUsbd -lSceSysmodule -lSceUserService` link dependencies. All symbols are resolved dynamically at runtime via `sys_dynlib_load_prx` and `sys_dynlib_dlsym`, eliminating ELF `.got` `R_X86_64_GLOB_DAT` relocations.
- **Safe Executable Trampoline Allocator**:
  - Implemented `alloc_executable_trampoline()` using `sceKernelMmap(..., VM_PROT_ALL, ...)` with ±2 GB hints, ensuring all 32-bit hook detours execute strictly from executable memory pages.
- **Non-Blocking Diagnostic Logging**:
  - Spinlock-protected ring buffer logger in `src/log.c` writing directly to `/data/GoldHEN/xbox_controller.log` (with 64 KB `.old` rotation) and rate-limited on-screen notifications.
- **Project Documentation & License**:
  - Added MIT `LICENSE`, comprehensive `README.md`, and technical architectural documentation.

### Fixed
- Fixed USB polling error handling where PS4 `SCE_USBD_ERROR_TIMEOUT` was misinterpreted as a disconnect, eliminating the connect/disconnect notification loop.
- Fixed stuck inputs on controller disconnection by immediately zeroing state and stopping rumble when `sceUsbdCheckConnected` indicates the device was removed.
- Fixed device list memory leak by ensuring `sceUsbdFreeDeviceList` is always called.
- Fixed crash caused by calling `sceSystemServiceNavigateToGoHome()` from game processes by removing the call and mapping Guide to Touchpad Click.

---

## [1.3.0] - Previous Release

### Added
- Support for wired USB Xbox 360, Xbox One/Series (including PDP & PowerA third-party controllers), and Switch (PDP) controllers as a secondary (Player 2) controller via virtual handle `1001`.
