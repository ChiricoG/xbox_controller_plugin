# Xbox Controller Plugin for PS4

A GoldHEN plugin that enables Xbox and Nintendo Switch USB controllers on jailbroken PS4 consoles.

## Status: WORKING (Single Player + Local Multiplayer)

Xbox controllers work as Player 2 in local multiplayer games! DS4 remains Player 1.

### Supported Controllers

**Xbox Controllers:**
- Xbox 360 wired USB controller
- Xbox One wired USB controller
- Xbox Series X|S wired USB controller
- Xbox Elite controllers (wired)

**Third-Party Xbox Controllers (PowerA, Hori, Razer, 8BitDo, Mad Catz, PDP, etc.):**
- Any wired controller in **XInput / Xbox mode** is detected automatically from its USB interface descriptor:
  - Xbox 360 compatible: class `FF/5D/01`
  - Xbox One compatible: class `FF/47/D0`
- Detection does not rely on a static VID/PID list. A notification will display the detected `VID:PID`.
- Controllers equipped with a physical mode switch (X / D / S) must be set to **X (XInput)** mode. DInput/HID modes are not supported.
- Supported third-party Xbox One pads (including PDP and PowerA models) automatically receive their required vendor initialization packets.

**Nintendo Switch Controllers (PDP):**
- PDP Rock Candy Wired Controller
- PDP Faceoff Wired Pro Controller
- PDP Faceoff Deluxe Wired Pro Controller
- PDP Wired Fight Pad Pro

**Note for Xbox One/Series controllers:** The controller must be **off** (Xbox button unlit) when launching the game. The plugin will detect and initialize it automatically. If the controller is already turned on, unplug and reconnect it, or turn it off before starting the game.

### What Works
- Full button mapping (A/B/X/Y, bumpers, triggers, sticks, D-pad)
- Low-latency input (1ms polling, 2ms timeout)
- **Local multiplayer** - Xbox controller as Player 2, DS4 as Player 1
- Dynamic descriptor-based detection for official and third-party controllers

### Not Supported
- Rumble/vibration feedback
- Wireless/Bluetooth Xbox controllers

---

## Requirements

- Jailbroken PS4 (tested on 9.00)
- GoldHEN 2.3 or newer
- Supported **wired** USB controller (Xbox 360/One/Series or PDP Switch controller)
- DualShock 4 controller (required for system menu navigation and Player 1)

---

## Installation

1. Connect to your PS4 via FTP (port 2121)
2. Upload `xbox_controller.prx` to `/data/GoldHEN/plugins/`
3. Create or edit `/data/GoldHEN/plugins.ini`:
   ```ini
   [default]
   /data/GoldHEN/plugins/xbox_controller.prx
   ```
4. Launch a game — the plugin will load automatically.

To disable: remove or comment out the plugin line in `plugins.ini`.

---

## Multiplayer Setup

For local multiplayer:

1. **Create a second PS4 user profile** (`Settings` > `Users` > `Create User`)
2. **Log in both users** before launching the game:
   - Profile 1 (Player 1 — DS4)
   - Profile 2 (Player 2 — Xbox controller)
3. Launch the game.
4. Notifications will confirm controller detection and Player 2 assignment.
5. Both controllers now operate independently.

> **Note:** Both users must be signed in at the PS4 system level before starting the game. The plugin automatically detects and assigns the Xbox controller to the second logged-in user.

### Drop-in Games (Diablo III, etc.)

For games where Player 2 joins mid-game by pressing Start:
1. **Create your Player 2 character on the second profile first** (launch the game once as Player 2 to create save data before attempting co-op).
2. Once the character exists, press Start on the Xbox controller to drop into the session.

---

## Button Mapping

### Xbox Controllers

| Xbox | DualShock 4 |
|------|-------------|
| A | Cross (X) |
| B | Circle |
| X | Square |
| Y | Triangle |
| LB | L1 |
| RB | R1 |
| LT | L2 |
| RT | R2 |
| Back | Share |
| Start | Options |
| Guide | PS Button |
| L3 | L3 |
| R3 | R3 |

### Switch Controllers (PDP)

| Switch | DualShock 4 |
|--------|-------------|
| A | Circle |
| B | Cross (X) |
| X | Triangle |
| Y | Square |
| L | L1 |
| R | R1 |
| ZL | L2 |
| ZR | R2 |
| - | Share |
| + | Options |
| Home | PS Button |
| L3 | L3 |
| R3 | R3 |

*Note: Switch controllers follow positional mapping (A corresponds to Circle, B to Cross).*

---

## Limitations

- Touchpad input is not supported on third-party controllers.
- Motion controls (gyroscope/accelerometer) are unavailable.
- Lightbar color feedback is not supported.
- Controller headphone audio passthrough is not supported.
- System menu operation requires an original DS4.
- Switch controller triggers operate digitally (full press only).

---

## Building

### Prerequisites

- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
- [GoldHEN Plugins SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK) (included as submodule)

Clone the repository recursively:
```bash
git clone --recursive https://github.com/ChiricoG/xbox_controller_plugin.git
cd xbox_controller_plugin
```
If already cloned without `--recursive`, initialize submodules:
```bash
git submodule update --init --recursive
```

### Linux / macOS

Set the `OO_PS4_TOOLCHAIN` environment variable and run `make`:
```bash
export OO_PS4_TOOLCHAIN=/path/to/openorbis-toolchain
make
```

Output: `bin/xbox_controller.prx`

### Windows (MSYS2)

1. Install MSYS2 with Clang and LLD:
   ```bash
   pacman -S msys/clang msys/lld msys/llvm
   ```
2. Build or obtain `create-fself.exe` from [OpenOrbis create-fself](https://github.com/OpenOrbis/create-fself).
3. Set your environment variables and execute the build script from an MSYS2 shell:
   ```bash
   export OO_PS4_TOOLCHAIN=/c/path/to/OpenOrbis/PS4Toolchain
   export CREATE_FSELF=/c/path/to/create-fself.exe
   bash build_windows.sh
   ```

Output: `bin/xbox_controller.prx`

---

## Technical Details

This plugin hooks several PS4 system APIs:
- `scePadOpen` / `scePadClose` — Virtual controller registration
- `scePadRead` / `scePadReadState` — Controller input injection into application space
- `scePadGetControllerInformation` — Pad status and capability reporting
- `sceUserServiceGetLoginUserIdList` — User ID assignment for local co-op
- `sceUsbd` — USB enumeration, descriptor parsing, and interrupt transfer polling
- Endpoint and interface configuration resolved dynamically via USB descriptors
- GIP / XInput HID report parsing translated into PS4 `OrbisPadData` structures

---

## Roadmap

### PSN Spoof for Local Multiplayer (Planned)

Certain titles (e.g. Minecraft Bedrock) verify PSN availability even for offline split-screen co-op. A planned update will hook `libSceNpManager` availability routines (`sceNpCheckNpAvailability`, `sceNpGetState`, `sceNpHasSignedUp`) to return offline spoofed status.

---

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.

---

## Credits

- GoldHEN team for the plugin SDK and `gamepad_helper` reference
- OpenOrbis team for the PS4 toolchain and reverse engineering tooling
- `remotePad` plugin for virtual controller architecture inspiration
