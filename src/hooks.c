/*
 * Hybrid approach: Hook scePad like gamepad_helper does
 * Read Xbox controller via USB, inject into scePadReadState
 *
 * STABILITY: All initialization done upfront in plugin_load,
 * hooks only do lightweight data injection
 */

#include "hooks.h"
#include "config.h"
#include "translator.h"
#include "xbox360.h"
#include "xboxone.h"
#include "switch_controller.h"
#include "usb_xbox.h"
#include "controller_detect.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/Usbd.h>
#include <orbis/UserService.h>

#include <GoldHEN.h>
#include <Detour.h>
#include <Patcher.h>
#include <Utilities.h>

// External function declarations
extern int32_t scePadReadExt(int32_t handle, OrbisPadData* pData, int32_t num);
extern int32_t scePadReadStateExt(int32_t handle, OrbisPadData* pData);
extern int sys_dynlib_load_prx(const char* path, int* handle);
extern const char* sceKernelGetFsSandboxRandomWord(void);

// Virtual controller constants
#define XBOX_VIRTUAL_PAD_HANDLE 1001        // Our fake pad handle

// Auto-detected user ID for Player 2 (set during init)
static int32_t g_xbox_user_id = 0;

// Function pointer types for HOOK_CONTINUE
typedef int32_t (*scePadOpen_t)(int32_t, int32_t, int32_t, void*);
typedef int32_t (*scePadClose_t)(int32_t);
typedef int32_t (*scePadGetControllerInformation_t)(int32_t, OrbisPadInformation*);
typedef int32_t (*sceUserServiceGetLoginUserIdList_t)(OrbisUserServiceLoginUserIdList*);

// Hooks for pad and user service functions
HOOK_INIT(scePadRead);
HOOK_INIT(scePadReadState);
HOOK_INIT(scePadOpen);
HOOK_INIT(scePadClose);
HOOK_INIT(scePadGetControllerInformation);
HOOK_INIT(sceUserServiceGetLoginUserIdList);

// Patchers
static Patcher* g_padReadExtPatcher = NULL;
static Patcher* g_padReadStateExtPatcher = NULL;

// State flags
static int g_hooks_installed = 0;
static int g_usb_initialized = 0;
static int g_pad_prx_loaded = 0;
static int g_usb_prx_loaded = 0;
static int g_user_prx_loaded = 0;

// Virtual controller state
static int g_virtual_pad_open = 0;      // Is our virtual pad currently open?
static int g_xbox_connected = 0;        // Is Xbox controller physically connected?
static ControllerType g_controller_type = CONTROLLER_NONE;  // Which controller type

// USB device handle for Xbox controller
static libusb_device_handle* g_xbox_handle = NULL;

// Runtime USB parameters of the active controller (filled during detection).
// Defaults match the official controllers; third-party devices get theirs
// from the USB descriptors.
static uint8_t  g_usb_iface = 0;
static uint8_t  g_ep_in     = 0x81;
static uint8_t  g_ep_out    = 0x01;
static uint16_t g_usb_vid   = 0;
static uint16_t g_usb_pid   = 0;

// Xbox controller VID (Microsoft)
#define XBOX_VID 0x045E

// PDP controller VID (Performance Designed Products)
#define PDP_VID 0x0E6F

// Xbox 360 PIDs
#define XBOX360_PID 0x028E

// Xbox One PIDs (multiple variants)
static const uint16_t XBOXONE_PIDS[] = {
    0x02D1,  // Original Xbox One controller
    0x02DD,  // Xbox One controller (newer)
    0x02E3,  // Xbox Elite controller
    0x02EA,  // Xbox One S controller
    0x0B00,  // Xbox Elite 2 controller
    0x0B12,  // Xbox Series X|S controller (USB)
    0x0B20,  // 2021 Xbox controller
};
#define XBOXONE_PID_COUNT (sizeof(XBOXONE_PIDS) / sizeof(XBOXONE_PIDS[0]))

// Check if PID is an Xbox One controller
static int is_xboxone_pid(uint16_t pid) {
    for (size_t i = 0; i < XBOXONE_PID_COUNT; i++) {
        if (XBOXONE_PIDS[i] == pid) return 1;
    }
    return 0;
}

// PDP Switch Input-Only controller PIDs
static const uint16_t PDP_SWITCH_PIDS[] = {
    0x0187,  // PDP Rock Candy Wired Controller
    0x0180,  // PDP Faceoff Wired Pro Controller
    0x0181,  // PDP Faceoff Deluxe Wired Pro Controller
    0x0185,  // PDP Wired Fight Pad Pro
};
#define PDP_SWITCH_PID_COUNT (sizeof(PDP_SWITCH_PIDS) / sizeof(PDP_SWITCH_PIDS[0]))

// Check if PID is a PDP Switch controller
static int is_pdp_switch_pid(uint16_t pid) {
    for (size_t i = 0; i < PDP_SWITCH_PID_COUNT; i++) {
        if (PDP_SWITCH_PIDS[i] == pid) return 1;
    }
    return 0;
}

// ============================================
// Xbox One (GIP) initialization
// ============================================

typedef struct {
    const uint8_t* data;
    uint8_t        len;
} InitPacket;

// Standard "power on" packet - needed by every Xbox One controller to start
// sending input reports.
static const uint8_t XBONE_PKT_POWER_ON[]  = { 0x05, 0x20, 0x00, 0x01, 0x00 };

// Extra packets required by some third-party controllers.
// Taken from the Linux xpad driver (xboxone_*_init); not verified on PS4.
static const uint8_t XBONE_PKT_PDP_1[]     = { 0x0A, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14 };
static const uint8_t XBONE_PKT_PDP_2[]     = { 0x06, 0x20, 0x00, 0x02, 0x01, 0x00 };
static const uint8_t XBONE_PKT_RUMBLE_ON[] = { 0x09, 0x00, 0x00, 0x09, 0x00, 0x0F, 0x00, 0x00,
                                               0x1D, 0x1D, 0xFF, 0x00, 0x00 };
static const uint8_t XBONE_PKT_RUMBLE_OFF[]= { 0x09, 0x00, 0x00, 0x09, 0x00, 0x0F, 0x00, 0x00,
                                               0x00, 0x00, 0x00, 0x00, 0x00 };

// Vendor IDs that need the extra packets
#define VID_POWERA_1   0x24C6
#define VID_POWERA_2   0x20D6
#define VID_PDP        0x0E6F

// Send a single GIP packet; byte [2] is the GIP sequence number.
static int xboxone_send_packet(libusb_device_handle* handle, uint8_t ep_out,
                               const InitPacket* pkt, uint8_t* seq) {
    uint8_t buf[32];
    int32_t transferred = 0;

    if (pkt->len > sizeof(buf)) return -1;
    memcpy(buf, pkt->data, pkt->len);
    if (pkt->len > 2) buf[2] = (*seq)++;

    return sceUsbdInterruptTransfer(handle, ep_out, buf, pkt->len, &transferred, 100);
}

// Send initialization sequence to Xbox One controller
static int xboxone_send_init(libusb_device_handle* handle, uint8_t ep_out,
                             uint16_t vid) {
    InitPacket seq_list[4];
    int count = 0;
    uint8_t seq = 0;

    if (ep_out == 0) {
        ep_out = 0x02;  // official Xbox One OUT endpoint
    }

    seq_list[count++] = (InitPacket){ XBONE_PKT_POWER_ON, sizeof(XBONE_PKT_POWER_ON) };

    if (vid == VID_PDP) {
        seq_list[count++] = (InitPacket){ XBONE_PKT_PDP_1, sizeof(XBONE_PKT_PDP_1) };
        seq_list[count++] = (InitPacket){ XBONE_PKT_PDP_2, sizeof(XBONE_PKT_PDP_2) };
    } else if (vid == VID_POWERA_1 || vid == VID_POWERA_2) {
        seq_list[count++] = (InitPacket){ XBONE_PKT_RUMBLE_ON,  sizeof(XBONE_PKT_RUMBLE_ON) };
        seq_list[count++] = (InitPacket){ XBONE_PKT_RUMBLE_OFF, sizeof(XBONE_PKT_RUMBLE_OFF) };
    }

    int ret = 0;
    for (int i = 0; i < count; i++) {
        ret = xboxone_send_packet(handle, ep_out, &seq_list[i], &seq);
        if (ret != 0) {
            break;
        }
        sceKernelUsleep(10000);  // let the controller digest each packet
    }
    return ret;
}

// Identify a USB device as a supported controller.
// 1) Known official / PDP VID+PID: use the original hardcoded parameters
//    (keeps previous behaviour unchanged).
// 2) Anything else: look at the USB interface descriptor, which also matches
//    third-party Xbox 360 / Xbox One compatible controllers.
// Returns 1 if the device is supported.
static int identify_controller(libusb_device* dev,
                               const struct libusb_device_descriptor* desc,
                               DetectedController* out) {
    memset(out, 0, sizeof(*out));

    if (desc->idVendor == XBOX_VID && desc->idProduct == XBOX360_PID) {
        out->type = CONTROLLER_XBOX360;
        out->ep_in = 0x81;
        out->ep_out = 0x01;
    } else if (desc->idVendor == XBOX_VID && is_xboxone_pid(desc->idProduct)) {
        out->type = CONTROLLER_XBOXONE;
        out->ep_in = 0x82;
        out->ep_out = 0x02;
    } else if (desc->idVendor == PDP_VID && is_pdp_switch_pid(desc->idProduct)) {
        out->type = CONTROLLER_SWITCH;
        out->ep_in = 0x81;
        out->ep_out = 0;
    } else if (controller_detect_by_descriptor(dev, out)) {
        out->generic = 1;
    } else {
        return 0;
    }

    out->vid = desc->idVendor;
    out->pid = desc->idProduct;
    return 1;
}

static void hook_notify(const char* message) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.targetId = -1;
    strncpy(req.message, message, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// Initialize USB subsystem - call ONCE from plugin_load, NOT from hooks
int hooks_init_usb(void) {
    if (g_usb_initialized) return 0;

    // Load USB module
    char module[256];
    snprintf(module, 256, "/%s/common/lib/%s", sceKernelGetFsSandboxRandomWord(), "libSceUsbd.sprx");
    int h = 0;
    int ret = sys_dynlib_load_prx(module, &h);
    if (ret < 0 || h == 0) {
        // USB PRX failed to load - not fatal, just no Xbox support
        return -1;
    }
    g_usb_prx_loaded = 1;

    // Init USB subsystem
    ret = sceUsbdInit();
    if (ret != 0) {
        return -1;
    }
    g_usb_initialized = 1;

    // Scan for Xbox controllers (360 and One)
    libusb_device** dev_list = NULL;
    int32_t dev_count = sceUsbdGetDeviceList(&dev_list);
    if (dev_count <= 0 || dev_list == NULL) {
        // No USB devices - that's okay
        return 0;
    }

    // Look for supported controllers (official, PDP Switch and third-party)
    for (int i = 0; i < dev_count; i++) {
        struct libusb_device_descriptor desc;
        if (sceUsbdGetDeviceDescriptor(dev_list[i], &desc) != 0) {
            continue;
        }

        DetectedController det;
        if (!identify_controller(dev_list[i], &desc, &det)) {
            continue;
        }

        ret = sceUsbdOpen(dev_list[i], &g_xbox_handle);
        if (ret != 0 || g_xbox_handle == NULL) {
            g_xbox_handle = NULL;
            continue;
        }

        // Try to detach kernel driver if attached
        sceUsbdDetachKernelDriver(g_xbox_handle, det.interface_num);

        ret = sceUsbdClaimInterface(g_xbox_handle, det.interface_num);
        if (ret != 0) {
            sceUsbdClose(g_xbox_handle);
            g_xbox_handle = NULL;
            continue;
        }

        g_xbox_connected = 1;
        g_controller_type = det.type;
        g_usb_iface = det.interface_num;
        g_ep_in = det.ep_in;
        g_ep_out = det.ep_out;
        g_usb_vid = det.vid;
        g_usb_pid = det.pid;

        // Xbox One needs init command(s) to start sending input
        if (det.type == CONTROLLER_XBOXONE) {
            sceUsbdSetInterfaceAltSetting(g_xbox_handle, det.interface_num, det.alt_setting);
            xboxone_send_init(g_xbox_handle, det.ep_out, det.vid);
        }

        const char* type_name =
            (det.type == CONTROLLER_XBOX360) ? "Xbox 360" :
            (det.type == CONTROLLER_XBOXONE) ? "Xbox One" : "Switch";

        char msg[96];
        if (det.generic) {
            snprintf(msg, sizeof(msg), "%s (3rd party) %04X:%04X connected!",
                     type_name, det.vid, det.pid);
        } else {
            snprintf(msg, sizeof(msg), "%s connected!", type_name);
        }
        hook_notify(msg);

        sceUsbdFreeDeviceList(dev_list);
        return 0;
    }

    sceUsbdFreeDeviceList(dev_list);
    return 0;  // USB init succeeded even if no Xbox found
}

// Cached foreground user ID (Player 1)
static int32_t g_foreground_user_id = 0;

// Get the foreground user (lazy, cached)
static int32_t get_foreground_user(void) {
    if (g_foreground_user_id == 0) {
        sceUserServiceGetForegroundUser(&g_foreground_user_id);
    }
    return g_foreground_user_id;
}

// Auto-detect second user for Player 2
// Called lazily when scePadOpen is called for a non-foreground user
int hooks_detect_second_user(void) {
    // Just cache the foreground user at init time
    int32_t fg = 0;
    int32_t ret = sceUserServiceGetForegroundUser(&fg);
    if (ret == 0 && fg != 0) {
        g_foreground_user_id = fg;
        return 0;
    }
    return -1;
}

// Cached controller reports - updated by polling, read by hooks
static union {
    Xbox360Report xbox360;
    XboxOneReport xboxone;
    SwitchInputOnlyReport switch_report;
    uint8_t raw[64];  // Large enough for any report
} g_cached_xbox_report;
static volatile int g_has_xbox_data = 0;
static uint64_t g_last_read_time = 0;
static int g_xbox_active = 0;

// Helper to inject Xbox data into pad data
// STABILITY: No initialization here - just read if connected
static void inject_xbox_input(OrbisPadData* pData) {
    // Only try to read if Xbox handle is valid (set during plugin_load)
    if (g_xbox_handle == NULL || g_controller_type == CONTROLLER_NONE) {
        return;  // No Xbox connected, pass through DS4 data unchanged
    }

    uint64_t now = sceKernelGetProcessTime();

    // Read every 1ms (1000Hz)
    if (now - g_last_read_time > 1000) {
        g_last_read_time = now;

        int32_t transferred = 0;

        // Read from the interrupt IN endpoint found during detection
        // (0x81 for Xbox 360 / Switch, 0x82 for official Xbox One, anything
        // for third-party devices).
        //
        // Read into a temporary buffer and only copy to the cache when the
        // report is valid: Xbox One controllers also send non-input packets
        // (Guide button, acks...) that must not overwrite the cached state.
        uint8_t rx[sizeof(g_cached_xbox_report.raw)];

        int ret = sceUsbdInterruptTransfer(
            g_xbox_handle,
            g_ep_in,
            rx,
            sizeof(rx),
            &transferred,
            2  // 2ms timeout
        );

        if (ret == 0 && transferred > 0) {
            int valid_report = 0;

            if (g_controller_type == CONTROLLER_XBOX360 && transferred >= 20) {
                // Xbox 360: msg_type=0x00, msg_length=0x14
                if (rx[0] == 0x00) {
                    valid_report = 1;
                }
            } else if (g_controller_type == CONTROLLER_XBOXONE && transferred >= 18) {
                // Xbox One: report_type=0x20 for input
                if (rx[0] == XBOXONE_REPORT_INPUT) {
                    valid_report = 1;
                }
            } else if (g_controller_type == CONTROLLER_SWITCH && transferred >= SWITCH_INPUT_ONLY_REPORT_SIZE) {
                // Switch Input-Only: 7 bytes, no report ID filtering needed
                valid_report = 1;
            }

            if (valid_report) {
                memcpy(g_cached_xbox_report.raw, rx, (size_t)transferred);
                if (!g_xbox_active) {
                    g_xbox_active = 1;
                    hook_notify("Controller input active!");
                }
                g_has_xbox_data = 1;
            }
        }
    }

    // Use cached data for translation based on controller type
    if (g_has_xbox_data) {
        if (g_controller_type == CONTROLLER_XBOX360) {
            xbox360_to_ds4(&g_cached_xbox_report.xbox360, pData);
        } else if (g_controller_type == CONTROLLER_XBOXONE) {
            xboxone_to_ds4(&g_cached_xbox_report.xboxone, pData);
        } else if (g_controller_type == CONTROLLER_SWITCH) {
            switch_to_ds4(&g_cached_xbox_report.switch_report, pData);
        }
    }
}

// ============================================
// User Service Hook - Inject virtual user
// ============================================

int32_t sceUserServiceGetLoginUserIdList_hook(OrbisUserServiceLoginUserIdList* userIdList) {
    // Call original first via HOOK_CONTINUE
    int32_t ret = HOOK_CONTINUE(sceUserServiceGetLoginUserIdList, sceUserServiceGetLoginUserIdList_t, userIdList);
    if (ret != 0 || userIdList == NULL) {
        return ret;
    }

    // With auto-detect, we use an already logged-in user
    // No injection needed - just pass through
    return ret;
}

// ============================================
// Pad Open/Close Hooks - Handle virtual controller
// ============================================

int32_t scePadOpen_hook(int32_t userId, int32_t type, int32_t index, void* param) {
    // Dynamic detection: if Xbox is connected and this is NOT the foreground user,
    // this must be Player 2 - give them the Xbox controller
    int32_t fg_user = get_foreground_user();

    if (g_xbox_connected && fg_user != 0 && userId != fg_user) {
        // This is a non-foreground user requesting a controller
        // Assign Xbox controller to them
        if (g_xbox_user_id == 0) {
            g_xbox_user_id = userId;  // Remember this user for future calls
        }

        if (userId == g_xbox_user_id) {
            g_virtual_pad_open = 1;
            hook_notify("Xbox Player 2 ready!");
            return XBOX_VIRTUAL_PAD_HANDLE;
        }
    }

    // Pass through to real scePadOpen via HOOK_CONTINUE
    return HOOK_CONTINUE(scePadOpen, scePadOpen_t, userId, type, index, param);
}

int32_t scePadClose_hook(int32_t handle) {
    // Check if closing our virtual pad
    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        g_virtual_pad_open = 0;
        g_xbox_user_id = 0;  // Reset so it can be reassigned
        return 0;
    }

    // Pass through to real scePadClose via HOOK_CONTINUE
    return HOOK_CONTINUE(scePadClose, scePadClose_t, handle);
}

// ============================================
// Controller Info Hook - Report virtual controller
// ============================================

int32_t scePadGetControllerInformation_hook(int32_t handle, OrbisPadInformation* info) {
    // Check if querying our virtual pad
    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        if (info != NULL) {
            memset(info, 0, sizeof(OrbisPadInformation));
            info->connected = g_xbox_connected ? 1 : 0;
            info->connectionType = ORBIS_PAD_CONNECTION_TYPE_STANDARD;
            info->deviceClass = ORBIS_PAD_DEVICE_CLASS_PAD;
            // Fake touchpad info (Xbox has none)
            info->touchpadDensity = 1.0f;
            info->touchResolutionX = 1920;
            info->touchResolutionY = 943;
        }
        return 0;
    }

    // Pass through to real function via HOOK_CONTINUE
    return HOOK_CONTINUE(scePadGetControllerInformation, scePadGetControllerInformation_t, handle, info);
}

// ============================================
// Pad Read Hooks - Only inject on virtual handle
// ============================================

int32_t scePadRead_hook(int32_t handle, OrbisPadData* pData, int32_t num) {
    // Check if this is our virtual pad
    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        if (!g_virtual_pad_open || pData == NULL || num <= 0) {
            return 0;
        }

        // Fill with Xbox data
        for (int i = 0; i < num; i++) {
            memset(&pData[i], 0, sizeof(OrbisPadData));
            pData[i].connected = g_xbox_connected ? 1 : 0;
            pData[i].timestamp = sceKernelGetProcessTime();
            // Neutral stick positions
            pData[i].leftStick.x = 128;
            pData[i].leftStick.y = 128;
            pData[i].rightStick.x = 128;
            pData[i].rightStick.y = 128;

            if (g_xbox_connected) {
                inject_xbox_input(&pData[i]);
            }
        }
        return num;
    }

    // For real DS4 handle - pass through unchanged
    int32_t ret = scePadReadExt(handle, pData, num);
    return ret;
}

int32_t scePadReadState_hook(int32_t handle, OrbisPadData* pData) {
    // Check if this is our virtual pad
    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        if (!g_virtual_pad_open || pData == NULL) {
            return -1;
        }

        // Fill with Xbox data
        memset(pData, 0, sizeof(OrbisPadData));
        pData->connected = g_xbox_connected ? 1 : 0;
        pData->timestamp = sceKernelGetProcessTime();
        // Neutral stick positions
        pData->leftStick.x = 128;
        pData->leftStick.y = 128;
        pData->rightStick.x = 128;
        pData->rightStick.y = 128;

        if (g_xbox_connected) {
            inject_xbox_input(pData);
        }
        return 0;
    }

    // For real DS4 handle - pass through unchanged
    int32_t ret = scePadReadStateExt(handle, pData);
    return ret;
}

int hooks_install(void) {
    if (g_hooks_installed) {
        return 0;
    }

    char module[256];
    int h = 0;
    int ret;

    // Load libScePad - MUST succeed for hooks to work
    snprintf(module, 256, "/%s/common/lib/%s", sceKernelGetFsSandboxRandomWord(), "libScePad.sprx");
    ret = sys_dynlib_load_prx(module, &h);
    if (ret < 0 || h == 0) {
        hook_notify("Xbox: Pad lib failed");
        return -1;
    }
    g_pad_prx_loaded = 1;

    // Load libSceUserService for user detection
    snprintf(module, 256, "/%s/common/lib/%s", sceKernelGetFsSandboxRandomWord(), "libSceUserService.sprx");
    h = 0;
    ret = sys_dynlib_load_prx(module, &h);
    if (ret >= 0 && h != 0) {
        g_user_prx_loaded = 1;

        // Cache the foreground user (Player 1) for later comparison
        hooks_detect_second_user();
    }

    // Verify scePadReadExt exists before patching
    if ((uint64_t)scePadReadExt == 0) {
        hook_notify("Xbox: No PadReadExt");
        return -1;
    }

    // Set up patcher for scePadReadExt (xor ecx, ecx)
    g_padReadExtPatcher = (Patcher*)malloc(sizeof(Patcher));
    if (g_padReadExtPatcher) {
        Patcher_Construct(g_padReadExtPatcher);
        uint8_t xor_ecx_ecx[5] = {0x31, 0xC9, 0x90, 0x90, 0x90};
        Patcher_Install_Patch(g_padReadExtPatcher, (uint64_t)scePadReadExt, xor_ecx_ecx, sizeof(xor_ecx_ecx));
    }

    // Set up patcher for scePadReadStateExt (xor edx, edx)
    g_padReadStateExtPatcher = (Patcher*)malloc(sizeof(Patcher));
    if (g_padReadStateExtPatcher) {
        Patcher_Construct(g_padReadStateExtPatcher);
        uint8_t xor_edx_edx[5] = {0x31, 0xD2, 0x90, 0x90, 0x90};
        Patcher_Install_Patch(g_padReadStateExtPatcher, (uint64_t)scePadReadStateExt, xor_edx_edx, sizeof(xor_edx_edx));
    }

    // Install pad hooks
    HOOK32(scePadRead);
    HOOK32(scePadReadState);
    HOOK32(scePadOpen);
    HOOK32(scePadClose);
    HOOK32(scePadGetControllerInformation);

    // Install user service hook if library loaded
    if (g_user_prx_loaded) {
        HOOK32(sceUserServiceGetLoginUserIdList);
    }

    g_hooks_installed = 1;
    return 0;
}

void hooks_remove(void) {
    // Unhook first to stop any hook calls
    if (g_hooks_installed) {
        UNHOOK(scePadRead);
        UNHOOK(scePadReadState);
        UNHOOK(scePadOpen);
        UNHOOK(scePadClose);
        UNHOOK(scePadGetControllerInformation);

        if (g_user_prx_loaded) {
            UNHOOK(sceUserServiceGetLoginUserIdList);
        }

        if (g_padReadExtPatcher) {
            Patcher_Destroy(g_padReadExtPatcher);
            free(g_padReadExtPatcher);
            g_padReadExtPatcher = NULL;
        }

        if (g_padReadStateExtPatcher) {
            Patcher_Destroy(g_padReadStateExtPatcher);
            free(g_padReadStateExtPatcher);
            g_padReadStateExtPatcher = NULL;
        }

        g_hooks_installed = 0;
    }

    // Clean up USB resources
    if (g_xbox_handle != NULL) {
        sceUsbdReleaseInterface(g_xbox_handle, g_usb_iface);
        sceUsbdClose(g_xbox_handle);
        g_xbox_handle = NULL;
    }

    if (g_usb_initialized) {
        sceUsbdExit();
        g_usb_initialized = 0;
    }

    // Reset state
    g_has_xbox_data = 0;
    g_xbox_active = 0;
    g_xbox_connected = 0;
    g_virtual_pad_open = 0;
    g_controller_type = CONTROLLER_NONE;
}

int hooks_is_virtual_handle(int handle) {
    return (handle == XBOX_VIRTUAL_PAD_HANDLE) ? 1 : 0;
}

int hooks_handle_to_index(int handle) {
    return (handle == XBOX_VIRTUAL_PAD_HANDLE) ? 0 : -1;
}
