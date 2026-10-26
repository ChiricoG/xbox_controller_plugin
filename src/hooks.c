/*
 * System-Wide Xbox Controller Plugin for PS4 (GoldHEN)
 *
 * Architecture:
 * - Zero static DT_NEEDED dependencies on libScePad, libSceUsbd, or libSceUserService.
 *   All symbols are resolved dynamically at runtime via sys_dynlib_load_prx + sys_dynlib_dlsym
 *   so the PRX can load safely under [default] into Retail Games, Homebrew Apps, and SceShellUI.
 * - Dedicated 250 Hz background thread (scePthreadCreate) with cross-process IPC
 *   (/data/GoldHEN/xbox_ipc.bin) so SceShellUI (Dashboard/Quick Menu) and the active Game
 *   share the controller seamlessly without USB interface conflicts.
 * - Automatic MERGE (1 user logged in -> P1) and SECONDARY (2+ users logged in -> P2) modes
 *   across the entire system, with runtime override combo (LB + RB + Back + Start for 1.5s).
 */

#include "hooks.h"
#include "config.h"
#include "ini_parser.h"
#include "translator.h"
#include "xbox360.h"
#include "xboxone.h"
#include "switch_controller.h"
#include "controller_detect.h"
#include "log.h"
#include "ds4.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/Usbd.h>
#include <orbis/UserService.h>
#include <orbis/_types/user.h>

#include <GoldHEN.h>
#include <Detour.h>
#include <Patcher.h>
#include <Utilities.h>
#include <Syscall.h>
#include <HDE64.h>

extern const char* sceKernelGetFsSandboxRandomWord(void);

// Virtual controller handle for Player 2 in SECONDARY mode
#define XBOX_VIRTUAL_PAD_HANDLE      1001
#define ORBIS_PAD_BUTTON_INTERCEPTED 0x80000000U

// Modes (D-002)
typedef enum {
    PLUGIN_MODE_AUTO = 0,
    PLUGIN_MODE_SECONDARY = 1,
    PLUGIN_MODE_MERGE = 2
} PluginMode;

// Shared state between polling thread and pad hooks within this process
typedef struct {
    ControllerType type;
    int            connected;
    int            has_data;
    uint64_t       last_report_us;
    union {
        Xbox360Report         x360;
        XboxOneReport         xone;
        SwitchInputOnlyReport sw;
        uint8_t               raw[64];
    } rpt;
    OrbisPadData   pad;   // Translated + special-button processed pad state
    uint8_t        guide; // Separate Guide button state (for Xbox One 0x07 report)
    uint8_t        rumble_large;
    uint8_t        rumble_small;
    int            rumble_dirty;
} XboxShared;

// Cross-process IPC file structures (/data/GoldHEN/xbox_ipc.bin)
#define XBOX_IPC_MAGIC      0x58424F58U // 'XBOX'
#define XBOX_IPC_VERSION    3U
#define XBOX_IPC_TIMEOUT_US 250000ULL   // 250 ms sequence stall timeout

typedef struct {
    uint32_t     magic;
    uint32_t     version;
    int32_t      master_pid;
    uint32_t     seq;
    int32_t      connected;
    int32_t      has_data;
    int32_t      controller_type;
    uint32_t     mode;
    int32_t      handoff_active;
    OrbisPadData pad;
} XboxIpcMasterData;

typedef struct {
    uint32_t rumble_seq;
    uint8_t  rumble_large;
    uint8_t  rumble_small;
    uint8_t  reserved[2];
    int32_t  shellui_pid;
    uint32_t shellui_seq;
    int32_t  fg_game_pid;
    uint32_t fg_game_seq;
} XboxIpcClientData;

static XboxShared          g_shared;
static volatile int        g_shared_lock = 0;
static OrbisPthread        g_poll_thread;
static volatile int        g_poll_running = 0;

static int                 g_my_pid = 0;
static int                 g_is_shellui = 0;
static int32_t             g_ipc_fd = -1;
static uint32_t            g_ipc_master_seq = 0;
static uint32_t            g_last_ipc_rumble_seq = 0;
static uint32_t            g_client_rumble_seq = 0;
static uint32_t            g_remote_last_seq = 0;
static uint64_t            g_remote_last_change_us = 0;
static int                 g_using_remote_ipc = 0;

static uint32_t            g_last_seen_shellui_seq = 0;
static uint64_t            g_last_shellui_change_us = 0;
static uint32_t            g_last_seen_fg_game_seq = 0;
static uint64_t            g_last_fg_game_change_us = 0;
static volatile int        g_game_intercepted = 0;
static volatile int        g_intercepted_frames = 0;
static uint8_t             g_prev_guide_state = 0;

static volatile PluginMode g_mode = PLUGIN_MODE_AUTO;
static volatile PluginMode g_effective_mode = PLUGIN_MODE_MERGE;
static volatile int        g_logged_in_users = 1;
static volatile int32_t    g_foreground_user_id = 0;

// Auto-detected user ID and fallback handle for Player 2
static int32_t g_xbox_user_id = 0;
static int32_t g_fallback_p2_handle = 0;
static int32_t g_known_p1_handle = 0;

static inline void xbox_spin_lock(void) {
    while (__atomic_exchange_n(&g_shared_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        __builtin_ia32_pause();
    }
}

static inline void xbox_spin_unlock(void) {
    __atomic_store_n(&g_shared_lock, 0, __ATOMIC_RELEASE);
}

// ============================================
// Dynamically Resolved Function Pointers
// ============================================

// libSceUsbd types & pointers
typedef int32_t (*sceUsbdInit_t)(void);
typedef void    (*sceUsbdExit_t)(void);
typedef ssize_t (*sceUsbdGetDeviceList_t)(libusb_device***);
typedef void    (*sceUsbdFreeDeviceList_t)(libusb_device**);
typedef int32_t (*sceUsbdGetDeviceDescriptor_t)(libusb_device*, struct libusb_device_descriptor*);
typedef int32_t (*sceUsbdGetActiveConfigDescriptor_t)(libusb_device*, struct libusb_config_descriptor**);
typedef void    (*sceUsbdFreeConfigDescriptor_t)(struct libusb_config_descriptor*);
typedef int32_t (*sceUsbdOpen_t)(libusb_device*, libusb_device_handle**);
typedef void    (*sceUsbdClose_t)(libusb_device_handle*);
typedef int32_t (*sceUsbdDetachKernelDriver_t)(libusb_device_handle*, int32_t);
typedef int32_t (*sceUsbdClaimInterface_t)(libusb_device_handle*, int32_t);
typedef int32_t (*sceUsbdReleaseInterface_t)(libusb_device_handle*, int32_t);
typedef int32_t (*sceUsbdSetInterfaceAltSetting_t)(libusb_device_handle*, int, int);
typedef int32_t (*sceUsbdInterruptTransfer_t)(libusb_device_handle*, unsigned char, unsigned char*, int32_t, int32_t*, uint32_t);
typedef int32_t (*sceUsbdCheckConnected_t)(libusb_device_handle*);

static sceUsbdInit_t                      p_sceUsbdInit = NULL;
static sceUsbdExit_t                      p_sceUsbdExit = NULL;
static sceUsbdGetDeviceList_t             p_sceUsbdGetDeviceList = NULL;
static sceUsbdFreeDeviceList_t            p_sceUsbdFreeDeviceList = NULL;
static sceUsbdGetDeviceDescriptor_t       p_sceUsbdGetDeviceDescriptor = NULL;
static sceUsbdGetActiveConfigDescriptor_t p_sceUsbdGetActiveConfigDescriptor = NULL;
static sceUsbdFreeConfigDescriptor_t      p_sceUsbdFreeConfigDescriptor = NULL;
static sceUsbdOpen_t                      p_sceUsbdOpen = NULL;
static sceUsbdClose_t                     p_sceUsbdClose = NULL;
static sceUsbdDetachKernelDriver_t        p_sceUsbdDetachKernelDriver = NULL;
static sceUsbdClaimInterface_t            p_sceUsbdClaimInterface = NULL;
static sceUsbdReleaseInterface_t          p_sceUsbdReleaseInterface = NULL;
static sceUsbdSetInterfaceAltSetting_t    p_sceUsbdSetInterfaceAltSetting = NULL;
static sceUsbdInterruptTransfer_t         p_sceUsbdInterruptTransfer = NULL;
static sceUsbdCheckConnected_t            p_sceUsbdCheckConnected = NULL;

// libScePad types & pointers
typedef int32_t (*scePadReadExt_t)(int32_t, OrbisPadData*, int32_t, int32_t);
typedef int32_t (*scePadReadStateExt_t)(int32_t, OrbisPadData*, int32_t);
typedef int32_t (*scePadRead_t)(int32_t, OrbisPadData*, int32_t);
typedef int32_t (*scePadReadState_t)(int32_t, OrbisPadData*);
typedef int32_t (*scePadOpen_t)(int32_t, int32_t, int32_t, void*);
typedef int32_t (*scePadClose_t)(int32_t);
typedef int32_t (*scePadGetControllerInformation_t)(int32_t, OrbisPadInformation*);
typedef int32_t (*scePadGetHandle_t)(int32_t, uint32_t, uint32_t);
typedef int32_t (*scePadSetLightBar_t)(int32_t, OrbisPadColor*);
typedef int32_t (*scePadResetLightBar_t)(int32_t);
typedef int32_t (*scePadSetMotionSensorState_t)(int32_t, int32_t);
typedef int32_t (*scePadSetVibration_t)(int32_t, const OrbisPadVibeParam*);

static scePadReadExt_t                  p_scePadReadExt = NULL;
static scePadReadStateExt_t             p_scePadReadStateExt = NULL;
static uintptr_t                        addr_scePadRead = 0;
static uintptr_t                        addr_scePadReadState = 0;
static uintptr_t                        addr_scePadOpen = 0;
static uintptr_t                        addr_scePadClose = 0;
static uintptr_t                        addr_scePadGetControllerInformation = 0;
static uintptr_t                        addr_scePadSetVibration = 0;
static uintptr_t                        addr_scePadGetHandle = 0;
static uintptr_t                        addr_scePadSetLightBar = 0;
static uintptr_t                        addr_scePadResetLightBar = 0;
static uintptr_t                        addr_scePadSetMotionSensorState = 0;

// libSceUserService types & pointers
typedef int32_t (*sceUserServiceGetForegroundUser_t)(int32_t*);
typedef int32_t (*sceUserServiceGetLoginUserIdList_t)(OrbisUserServiceLoginUserIdList*);

static sceUserServiceGetForegroundUser_t  p_sceUserServiceGetForegroundUser = NULL;
static sceUserServiceGetLoginUserIdList_t p_sceUserServiceGetLoginUserIdList = NULL;

// Detour & Patcher instances
HOOK_INIT(scePadRead);
HOOK_INIT(scePadReadState);
HOOK_INIT(scePadOpen);
HOOK_INIT(scePadClose);
HOOK_INIT(scePadGetControllerInformation);
HOOK_INIT(sceUserServiceGetLoginUserIdList);
HOOK_INIT(scePadGetHandle);
HOOK_INIT(scePadSetLightBar);
HOOK_INIT(scePadResetLightBar);
HOOK_INIT(scePadSetMotionSensorState);
HOOK_INIT(scePadSetVibration);

static Patcher* g_padReadExtPatcher = NULL;
static Patcher* g_padReadStateExtPatcher = NULL;

// State flags
static int g_hooks_installed = 0;
static int g_usb_initialized = 0;
static int g_pad_prx_loaded = 0;
static int g_usb_prx_loaded = 0;
static int g_user_prx_loaded = 0;

static int g_virtual_pad_open = 0;

// USB device handle for Xbox controller
static libusb_device_handle* g_xbox_handle = NULL;
static uint8_t  g_usb_iface = 0;
static uint8_t  g_ep_in     = 0x81;
static uint8_t  g_ep_out    = 0x01;
static uint16_t g_usb_vid   = 0;
static uint16_t g_usb_pid   = 0;
static uint8_t  g_gip_seq   = 0;

// Special button state machine variables (D-006 & D-008)
static uint64_t g_combo_start_us = 0;
static int      g_combo_triggered = 0;
static uint64_t g_share_start_us = 0;
static int      g_touchpad_active = 0;
static uint64_t g_share_pulse_until_us = 0;

#define COMBO_MASK (DS4_BUTTON_L1 | DS4_BUTTON_R1 | DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)

// Track open real handles to their user IDs
#define MAX_TRACKED_HANDLES 16
static struct {
    int32_t handle;
    int32_t user_id;
} g_open_handles[MAX_TRACKED_HANDLES];

static void register_open_handle(int32_t handle, int32_t user_id) {
    if (handle <= 0) return;
    for (int i = 0; i < MAX_TRACKED_HANDLES; i++) {
        if (g_open_handles[i].handle == handle || g_open_handles[i].handle == 0) {
            g_open_handles[i].handle = handle;
            g_open_handles[i].user_id = user_id;
            return;
        }
    }
}

static void unregister_open_handle(int32_t handle) {
    if (handle <= 0) return;
    if (g_fallback_p2_handle == handle) {
        g_fallback_p2_handle = 0;
    }
    if (g_known_p1_handle == handle) {
        g_known_p1_handle = 0;
    }
    for (int i = 0; i < MAX_TRACKED_HANDLES; i++) {
        if (g_open_handles[i].handle == handle) {
            g_open_handles[i].handle = 0;
            g_open_handles[i].user_id = 0;
            return;
        }
    }
}

// ============================================
// Dynamic Module & IPC Helpers
// ============================================

static int                 g_is_homebrew = 0;

void hooks_set_process_info(int pid, int is_shellui, int is_homebrew) {
    g_my_pid = pid;
    g_is_shellui = is_shellui;
    g_is_homebrew = is_homebrew;
}

static int load_sys_prx(const char* libname, int* out_handle) {
    char path[256];
    int h = 0;
    const char* sb = sceKernelGetFsSandboxRandomWord();
    if (sb != NULL && sb[0] != '\0') {
        snprintf(path, sizeof(path), "/%s/common/lib/%s", sb, libname);
        if (sys_dynlib_load_prx(path, &h) >= 0 && h != 0) {
            *out_handle = h;
            return 0;
        }
    }
    snprintf(path, sizeof(path), "/system/common/lib/%s", libname);
    if (sys_dynlib_load_prx(path, &h) >= 0 && h != 0) {
        *out_handle = h;
        return 0;
    }
    return -1;
}

int32_t usbd_get_active_config_descriptor(libusb_device* dev, struct libusb_config_descriptor** config) {
    if (p_sceUsbdGetActiveConfigDescriptor == NULL) return -1;
    return p_sceUsbdGetActiveConfigDescriptor(dev, config);
}

void usbd_free_config_descriptor(struct libusb_config_descriptor* config) {
    if (p_sceUsbdFreeConfigDescriptor != NULL && config != NULL) {
        p_sceUsbdFreeConfigDescriptor(config);
    }
}

static void open_ipc_file(void) {
    if (g_ipc_fd >= 0) return;

    // 1. Try direct open of /data/GoldHEN/xbox_ipc.bin first
    int32_t fd = sceKernelOpen("/data/GoldHEN/xbox_ipc.bin", O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        g_ipc_fd = fd;
        log_event(LOG_LEVEL_INFO, "IPC file opened at /data/GoldHEN/xbox_ipc.bin (fd=%d)", fd);
        return;
    }

    // 2. Sandboxed process fallback: jailbreak to escape sandbox and access /data/GoldHEN
    // Homebrew processes already have direct filesystem access and must NEVER be unjailed.
    if (!g_is_homebrew) {
        struct jailbreak_backup jb;
        memset(&jb, 0, sizeof(jb));
        if (sys_sdk_jailbreak(&jb) == 0) {
            fd = sceKernelOpen("/data/GoldHEN/xbox_ipc.bin", O_CREAT | O_RDWR, 0666);
            sys_sdk_unjailbreak(&jb);
            if (fd >= 0) {
                g_ipc_fd = fd;
                log_event(LOG_LEVEL_INFO, "IPC file opened via sandbox escape at /data/GoldHEN/xbox_ipc.bin (fd=%d)", fd);
                return;
            }
        }
    }

    // 3. Fallback candidates if /data/GoldHEN cannot be opened
    static const char* const fallbacks[] = {
        "/data/xbox_ipc.bin",
        "/user/data/xbox_ipc.bin"
    };
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        fd = sceKernelOpen(fallbacks[i], O_CREAT | O_RDWR, 0666);
        if (fd >= 0) {
            g_ipc_fd = fd;
            log_event(LOG_LEVEL_INFO, "IPC file opened at fallback %s (fd=%d)", fallbacks[i], fd);
            return;
        }
    }

    log_event(LOG_LEVEL_WARN, "Could not open IPC file; running in standalone USB mode");
}

static void ipc_write_master_state_ex(int active_master, int handoff_active) {
    if (g_ipc_fd < 0) return;

    XboxIpcMasterData m;
    memset(&m, 0, sizeof(m));
    m.magic = XBOX_IPC_MAGIC;
    m.version = XBOX_IPC_VERSION;
    m.master_pid = active_master ? g_my_pid : 0;
    m.seq = ++g_ipc_master_seq;
    m.handoff_active = handoff_active;

    xbox_spin_lock();
    m.connected = handoff_active ? 1 : g_shared.connected;
    m.has_data = g_shared.has_data;
    m.controller_type = (int32_t)g_shared.type;
    m.mode = (uint32_t)g_mode;
    m.pad = g_shared.pad;
    xbox_spin_unlock();

    sceKernelPwrite(g_ipc_fd, &m, sizeof(m), 0);
}

static void ipc_write_master_state(int active_master) {
    ipc_write_master_state_ex(active_master, 0);
}

// ============================================
// Controller Identification & GIP Init
// ============================================

typedef struct {
    const uint8_t* data;
    size_t         len;
} InitPacket;

static const uint8_t XBONE_PKT_POWER_ON[]  = { 0x05, 0x20, 0x00, 0x01, 0x00 };
static const uint8_t XBONE_PKT_PDP_1[]     = { 0x0A, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14 };
static const uint8_t XBONE_PKT_PDP_2[]     = { 0x06, 0x20, 0x00, 0x02, 0x01, 0x00 };
static const uint8_t XBONE_PKT_RUMBLE_ON[] = { 0x09, 0x00, 0x00, 0x09, 0x00, 0x0F, 0x00, 0x00,
                                               0x1D, 0x1D, 0xFF, 0x00, 0x00 };
static const uint8_t XBONE_PKT_RUMBLE_OFF[]= { 0x09, 0x00, 0x00, 0x09, 0x00, 0x0F, 0x00, 0x00,
                                               0x00, 0x00, 0x00, 0x00, 0x00 };

#define VID_POWERA_1   0x24C6
#define VID_POWERA_2   0x20D6
#define VID_PDP        0x0E6F
#define XBOX_VID       0x045E
#define XBOX360_PID    0x028E

static const uint16_t XBOXONE_PIDS[] = {
    0x02D1, 0x02DD, 0x02E3, 0x02EA, 0x0B00, 0x0B0A, 0x0B12, 0x02E0, 0x0B13, 0x0B20
};

static int is_xboxone_pid(uint16_t pid) {
    for (size_t i = 0; i < sizeof(XBOXONE_PIDS) / sizeof(XBOXONE_PIDS[0]); i++) {
        if (pid == XBOXONE_PIDS[i]) return 1;
    }
    return 0;
}

static int xboxone_send_packet(libusb_device_handle* handle, uint8_t ep_out,
                               const InitPacket* pkt, uint8_t* seq) {
    if (p_sceUsbdInterruptTransfer == NULL) return -1;
    uint8_t buf[32];
    if (pkt->len > sizeof(buf)) return -1;
    memcpy(buf, pkt->data, pkt->len);
    if (pkt->len >= 3) {
        buf[2] = (*seq)++;
    }
    int32_t transferred = 0;
    return p_sceUsbdInterruptTransfer(handle, ep_out, buf, (int32_t)pkt->len, &transferred, 100);
}

static int xboxone_send_init(libusb_device_handle* handle, uint8_t ep_out, uint16_t vid) {
    InitPacket seq_list[4];
    int count = 0;
    uint8_t seq = 1;

    if (ep_out == 0) ep_out = 0x02;

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
        if (ret != 0) break;
        sceKernelUsleep(10000);
    }
    return ret;
}

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
    } else if (is_switch_input_only_controller(desc->idVendor, desc->idProduct)) {
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

// Recompute g_effective_mode from current g_mode and cached g_logged_in_users
// Safe to call from any thread (no syscalls)
static PluginMode update_effective_mode_cached(void) {
    PluginMode prev = g_effective_mode;
    PluginMode next;
    if (g_mode == PLUGIN_MODE_MERGE) {
        next = PLUGIN_MODE_MERGE;
    } else if (g_mode == PLUGIN_MODE_SECONDARY) {
        next = PLUGIN_MODE_SECONDARY;
    } else {
        int users = g_logged_in_users;
        next = (users >= 2) ? PLUGIN_MODE_SECONDARY : PLUGIN_MODE_MERGE;
    }
    g_effective_mode = next;
    if (next != prev) {
        log_event(LOG_LEVEL_INFO, "Effective mode -> %s (mode=%d, users=%d)",
                  (next == PLUGIN_MODE_MERGE) ? "MERGE" : "SECONDARY",
                  (int)g_mode, g_logged_in_users);
    }
    return next;
}

static int count_valid_users_in_list(const OrbisUserServiceLoginUserIdList* list) {
    if (list == NULL) return 1;
    int count = 0;
    for (int i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        if (list->userId[i] != ORBIS_USER_SERVICE_USER_ID_INVALID && list->userId[i] > 0) {
            count++;
        }
    }
    return (count > 0) ? count : 1;
}

// Refresh user info from UserService
static void refresh_user_service_state(void) {
    if (!g_user_prx_loaded) {
        return;
    }

    int32_t fg = 0;
    if (p_sceUserServiceGetForegroundUser != NULL &&
        p_sceUserServiceGetForegroundUser(&fg) == 0 && fg != 0) {
        g_foreground_user_id = fg;
    }

    OrbisUserServiceLoginUserIdList list;
    memset(&list, 0, sizeof(list));
    int32_t ret = -1;
    if (Detour_sceUserServiceGetLoginUserIdList.StubPtr != NULL) {
        ret = HOOK_CONTINUE(sceUserServiceGetLoginUserIdList, sceUserServiceGetLoginUserIdList_t, &list);
    } else if (p_sceUserServiceGetLoginUserIdList != NULL) {
        ret = p_sceUserServiceGetLoginUserIdList(&list);
    }
    if (ret == 0) {
        g_logged_in_users = count_valid_users_in_list(&list);
    }
    update_effective_mode_cached();
}

static void cycle_mode(void) {
    if (g_mode == PLUGIN_MODE_AUTO) {
        g_mode = PLUGIN_MODE_SECONDARY;
        log_notify("Xbox Mode: SECONDARY (P2)");
        log_event(LOG_LEVEL_INFO, "Mode switch -> SECONDARY");
    } else if (g_mode == PLUGIN_MODE_SECONDARY) {
        g_mode = PLUGIN_MODE_MERGE;
        log_notify("Xbox Mode: MERGE (P1)");
        log_event(LOG_LEVEL_INFO, "Mode switch -> MERGE");
    } else {
        g_mode = PLUGIN_MODE_AUTO;
        log_notify("Xbox Mode: AUTO");
        log_event(LOG_LEVEL_INFO, "Mode switch -> AUTO");
    }
    update_effective_mode_cached();
}

// Send rumble to connected controller (Passo 9)
static void send_rumble_packet(uint8_t large, uint8_t small) {
    if (!g_config.enable_rumble) return;
    if (g_xbox_handle == NULL || g_ep_out == 0 || p_sceUsbdInterruptTransfer == NULL) return;

    int32_t transferred = 0;
    if (g_shared.type == CONTROLLER_XBOX360) {
        Xbox360OutputReport out;
        xbox360_init_rumble(&out, large, small);
        p_sceUsbdInterruptTransfer(g_xbox_handle, g_ep_out, (unsigned char*)&out, sizeof(out), &transferred, 10);
    } else if (g_shared.type == CONTROLLER_XBOXONE) {
        uint8_t pkt[13];
        pkt[0] = 0x09;
        pkt[1] = 0x00;
        pkt[2] = g_gip_seq++;
        pkt[3] = 0x09;
        pkt[4] = 0x00;
        pkt[5] = 0x0F;
        pkt[6] = 0x00;
        pkt[7] = 0x00;
        pkt[8] = 0x00;
        pkt[9] = 0x00;
        pkt[10] = large;
        pkt[11] = small;
        pkt[12] = 0x00;
        p_sceUsbdInterruptTransfer(g_xbox_handle, g_ep_out, pkt, sizeof(pkt), &transferred, 10);
    }
}

// Close controller and reset shared state (F-001)
static void close_controller_ex(int notify, int handoff) {
    int was_usb_owner = (g_xbox_handle != NULL);

    if (g_xbox_handle != NULL) {
        send_rumble_packet(0, 0);
        if (p_sceUsbdReleaseInterface != NULL) {
            p_sceUsbdReleaseInterface(g_xbox_handle, g_usb_iface);
        }
        if (p_sceUsbdClose != NULL) {
            p_sceUsbdClose(g_xbox_handle);
        }
        g_xbox_handle = NULL;
    }

    g_combo_start_us = 0;
    g_combo_triggered = 0;
    g_share_start_us = 0;
    g_touchpad_active = 0;
    g_share_pulse_until_us = 0;

    if (!handoff) {
        xbox_spin_lock();
        g_shared.connected = 0;
        g_shared.has_data = 0;
        g_shared.type = CONTROLLER_NONE;
        g_shared.guide = 0;
        g_shared.rumble_large = 0;
        g_shared.rumble_small = 0;
        g_shared.rumble_dirty = 0;
        memset(&g_shared.rpt, 0, sizeof(g_shared.rpt));
        memset(&g_shared.pad, 0, sizeof(g_shared.pad));
        xbox_spin_unlock();
    }

    if (was_usb_owner) {
        ipc_write_master_state_ex(0, handoff);
        if (handoff) {
            log_event(LOG_LEVEL_INFO, "USB interface released for process handoff (pid=%d)", g_my_pid);
        } else {
            log_event(LOG_LEVEL_INFO, "Xbox controller disconnected and closed");
            if (notify) {
                log_notify("Xbox controller disconnected");
            }
        }
    }
}

static void close_controller(void) {
    close_controller_ex(1, 0);
}

// Hotplug scanner (Passo 3, F-005, F-012)
static int scan_and_open(void) {
    if (g_xbox_handle != NULL) {
        return 0; // Already connected
    }
    if (!g_usb_initialized || p_sceUsbdGetDeviceList == NULL || p_sceUsbdFreeDeviceList == NULL) {
        return -1;
    }

    libusb_device** dev_list = NULL;
    ssize_t dev_count = p_sceUsbdGetDeviceList(&dev_list);
    if (dev_count <= 0 || dev_list == NULL) {
        if (dev_list != NULL) {
            p_sceUsbdFreeDeviceList(dev_list); // F-012 fix
        }
        return -1;
    }

    int was_handoff = 0;
    if (g_ipc_fd >= 0) {
        XboxIpcMasterData m;
        if (sceKernelPread(g_ipc_fd, &m, sizeof(m), 0) == sizeof(m) &&
            m.magic == XBOX_IPC_MAGIC && m.version == XBOX_IPC_VERSION && m.handoff_active != 0) {
            was_handoff = 1;
        }
    }

    for (ssize_t i = 0; i < dev_count; i++) {
        struct libusb_device_descriptor desc;
        if (p_sceUsbdGetDeviceDescriptor(dev_list[i], &desc) != 0) {
            continue;
        }

        DetectedController det;
        if (!identify_controller(dev_list[i], &desc, &det)) {
            continue;
        }

        int ret = p_sceUsbdOpen(dev_list[i], &g_xbox_handle);
        if (ret != 0 || g_xbox_handle == NULL) {
            g_xbox_handle = NULL;
            continue;
        }

        if (p_sceUsbdDetachKernelDriver != NULL) {
            p_sceUsbdDetachKernelDriver(g_xbox_handle, det.interface_num);
        }
        ret = p_sceUsbdClaimInterface(g_xbox_handle, det.interface_num);
        if (ret != 0) {
            p_sceUsbdClose(g_xbox_handle);
            g_xbox_handle = NULL;
            continue;
        }

        g_usb_iface = det.interface_num;
        g_ep_in = det.ep_in;
        g_ep_out = det.ep_out;
        g_usb_vid = det.vid;
        g_usb_pid = det.pid;
        g_gip_seq = 1;

        if (det.type == CONTROLLER_XBOXONE) {
            if (p_sceUsbdSetInterfaceAltSetting != NULL) {
                p_sceUsbdSetInterfaceAltSetting(g_xbox_handle, det.interface_num, det.alt_setting);
            }
            xboxone_send_init(g_xbox_handle, det.ep_out, was_handoff ? 0 : det.vid);
        }

        g_combo_start_us = 0;
        g_combo_triggered = 0;
        g_share_start_us = 0;
        g_touchpad_active = 0;
        g_share_pulse_until_us = 0;

        xbox_spin_lock();
        g_shared.type = det.type;
        g_shared.connected = 1;
        if (!was_handoff) {
            g_shared.has_data = 0;
            g_shared.guide = 0;
            memset(&g_shared.rpt, 0, sizeof(g_shared.rpt));
            memset(&g_shared.pad, 0, sizeof(g_shared.pad));
            g_shared.pad.connected = 1;
            g_shared.pad.count = 1;
            g_shared.pad.leftStick.x = 128;
            g_shared.pad.leftStick.y = 128;
            g_shared.pad.rightStick.x = 128;
            g_shared.pad.rightStick.y = 128;
            g_shared.pad.quat.w = 1.0f;
            g_shared.pad.acell.z = 1.0f;
        }
        xbox_spin_unlock();

        ipc_write_master_state_ex(1, 0);

        p_sceUsbdFreeDeviceList(dev_list);
        refresh_user_service_state();

        const char* type_name =
            (det.type == CONTROLLER_XBOX360) ? "Xbox 360" :
            (det.type == CONTROLLER_XBOXONE) ? "Xbox One" : "Switch";

        if (was_handoff) {
            log_event(LOG_LEVEL_INFO, "USB handoff completed (new USB Master pid=%d): %s (%04X:%04X)",
                      g_my_pid, type_name, det.vid, det.pid);
        } else {
            const char* player_tag = (g_effective_mode == PLUGIN_MODE_SECONDARY) ? " [Player 2]" : " [Player 1]";
            char msg[96];
            if (det.generic) {
                snprintf(msg, sizeof(msg), "%s%s connected!", type_name, player_tag);
            } else {
                snprintf(msg, sizeof(msg), "%s%s connected!", type_name, player_tag);
            }

            log_event(LOG_LEVEL_INFO, "Pad detected & opened (USB Master pid=%d): %s%s (VID=%04X PID=%04X iface=%d in=0x%02X out=0x%02X)",
                      g_my_pid, type_name, player_tag, det.vid, det.pid, det.interface_num, det.ep_in, det.ep_out);
            log_notify("%s", msg);
        }

        return 0;
    }

    p_sceUsbdFreeDeviceList(dev_list); // F-012 fix
    return -1;
}

// Special button processing on translated bitfields (D-006 & D-008)
// Runs inside usb_poll_thread at steady ~250 Hz
static void process_special_buttons(OrbisPadData* pData, uint64_t now_us) {
    uint32_t buttons = pData->buttons;

    // Check mode switch combo (LB + RB + Back + Start held ~1.5s) if enabled
    int combo_pressed = g_config.enable_mode_combo && ((buttons & COMBO_MASK) == COMBO_MASK);
    if (combo_pressed) {
        if (g_combo_start_us == 0) {
            g_combo_start_us = now_us;
        } else if (!g_combo_triggered && (now_us - g_combo_start_us >= 1500000ULL)) {
            g_combo_triggered = 1;
            cycle_mode();
        }
        // Suppress touchpad and share during combo
        g_share_start_us = 0;
        g_share_pulse_until_us = 0;
        g_touchpad_active = 0;
        buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_TOUCHPAD);
    } else {
        g_combo_start_us = 0;
        g_combo_triggered = 0;
    }

    // Touchpad click / Share handling (D-008)
    int raw_share = (buttons & DS4_BUTTON_SHARE) != 0;
    buttons &= ~DS4_BUTTON_SHARE; // Never pass raw share directly

    if (!combo_pressed) {
        if (raw_share) {
            if (g_share_start_us == 0) {
                g_share_start_us = now_us;
            } else if (now_us - g_share_start_us >= 2000000ULL) {
                // Held >= 2 s -> Touchpad click
                g_touchpad_active = 1;
            }
        } else {
            // Share released
            if (g_share_start_us != 0) {
                if (!g_touchpad_active && !g_combo_triggered && (now_us - g_share_start_us < 2000000ULL)) {
                    // Short press released -> trigger ~80 ms pulse
                    g_share_pulse_until_us = now_us + 80000ULL;
                }
                g_share_start_us = 0;
                g_touchpad_active = 0;
            }
        }
    }

    if (g_touchpad_active) {
        buttons |= DS4_BUTTON_TOUCHPAD;
    }

    if (now_us < g_share_pulse_until_us) {
        buttons |= DS4_BUTTON_SHARE;
    }

    pData->buttons = buttons;
}

static void update_translated_pad_in_poll_thread(void) {
    if (!g_shared.connected || !g_shared.has_data) {
        return;
    }

    OrbisPadData translated;
    memset(&translated, 0, sizeof(translated));

    if (g_shared.type == CONTROLLER_XBOX360) {
        xbox360_to_ds4(&g_shared.rpt.x360, &translated);
    } else if (g_shared.type == CONTROLLER_XBOXONE) {
        xboxone_to_ds4(&g_shared.rpt.xone, &translated);
    } else if (g_shared.type == CONTROLLER_SWITCH) {
        switch_to_ds4(&g_shared.rpt.sw, &translated);
    } else {
        return;
    }

    if (g_shared.guide) {
        if (g_config.guide_action == CONFIG_GUIDE_TOUCHPAD) {
            translated.buttons |= DS4_BUTTON_TOUCHPAD;
        } else if (g_config.guide_action == CONFIG_GUIDE_SHARE) {
            translated.buttons |= DS4_BUTTON_SHARE;
        }
    }

    uint64_t now_us = sceKernelGetProcessTime();
    process_special_buttons(&translated, now_us);

    xbox_spin_lock();
    g_shared.pad = translated;
    xbox_spin_unlock();
}

// Check if another process is currently acting as USB Master in xbox_ipc.bin
static int poll_remote_ipc_master(uint64_t now_us, int* out_handoff_pending) {
    if (out_handoff_pending != NULL) {
        *out_handoff_pending = 0;
    }
    if (g_ipc_fd < 0) return 0;

    XboxIpcMasterData m;
    ssize_t n = (ssize_t)sceKernelPread(g_ipc_fd, &m, sizeof(m), 0);
    if (n != (ssize_t)sizeof(m) || m.magic != XBOX_IPC_MAGIC || m.version != XBOX_IPC_VERSION) {
        return 0;
    }

    if (m.master_pid == 0) {
        if (m.handoff_active != 0 && out_handoff_pending != NULL) {
            *out_handoff_pending = 1;
        }
        return 0;
    }

    if (m.master_pid == g_my_pid) {
        return 0;
    }

    if (m.seq != g_remote_last_seq) {
        g_remote_last_seq = m.seq;
        g_remote_last_change_us = now_us;
    }

    if (g_remote_last_change_us == 0 || (now_us - g_remote_last_change_us) >= XBOX_IPC_TIMEOUT_US) {
        return 0;
    }

    if (m.pad.timestamp != 0 && now_us > m.pad.timestamp && (now_us - m.pad.timestamp) >= XBOX_IPC_TIMEOUT_US) {
        return 0;
    }

    // Remote USB Master is actively updating IPC!
    xbox_spin_lock();
    g_shared.connected = m.connected;
    g_shared.has_data = m.has_data;
    g_shared.type = (ControllerType)m.controller_type;
    g_shared.pad = m.pad;
    int need_rumble_forward = g_shared.rumble_dirty;
    uint8_t r_large = g_shared.rumble_large;
    uint8_t r_small = g_shared.rumble_small;
    g_shared.rumble_dirty = 0;
    xbox_spin_unlock();

    if (!g_is_homebrew && (PluginMode)m.mode != g_mode) {
        g_mode = (PluginMode)m.mode;
        update_effective_mode_cached();
    }

    if (need_rumble_forward) {
        XboxIpcClientData client;
        memset(&client, 0, sizeof(client));
        sceKernelPread(g_ipc_fd, &client, sizeof(client), (off_t)sizeof(XboxIpcMasterData));
        client.rumble_large = r_large;
        client.rumble_small = r_small;
        client.rumble_seq = ++g_client_rumble_seq;
        if (g_is_shellui) {
            client.shellui_pid = g_my_pid;
        }
        sceKernelPwrite(g_ipc_fd, &client, sizeof(client), (off_t)sizeof(XboxIpcMasterData));
    }

    if (!g_using_remote_ipc) {
        g_using_remote_ipc = 1;
        log_event(LOG_LEVEL_INFO, "Attached as IPC client to USB Master pid=%d", m.master_pid);
    }

    return 1;
}

// Dedicated USB & IPC polling thread (Passo 2, F-002, F-003)
static void* usb_poll_thread(void* arg) {
    (void)arg;
    int scan_counter = 0;
    int flush_counter = 0;
    int ipc_counter = 0;
    int heartbeat_counter = 0;
    int user_refresh_counter = 0;
    int consecutive_errors = 0;
    int should_yield_usb = 0;

    log_event(LOG_LEVEL_INFO, "Polling thread started (pid=%d, is_shellui=%d)", g_my_pid, g_is_shellui);
    log_flush();

    if (g_ipc_fd >= 0) {
        XboxIpcClientData client;
        memset(&client, 0, sizeof(client));
        sceKernelPread(g_ipc_fd, &client, sizeof(client), (off_t)sizeof(XboxIpcMasterData));
        if (g_is_shellui) {
            client.shellui_pid = g_my_pid;
            client.shellui_seq++;
        } else {
            client.fg_game_pid = g_my_pid;
            client.fg_game_seq++;
        }
        sceKernelPwrite(g_ipc_fd, &client, sizeof(client), (off_t)sizeof(XboxIpcMasterData));
    }

    while (g_poll_running) {
        uint64_t now_us = sceKernelGetProcessTime();

        if (++flush_counter >= 12) {
            flush_counter = 0;
            log_flush();
        }

        // Coordinate SceShellUI <-> Foreground Game heartbeat & USB ownership every 4 ticks (~16 ms)
        if (g_ipc_fd >= 0 && ++heartbeat_counter >= 4) {
            heartbeat_counter = 0;
            XboxIpcClientData client;
            memset(&client, 0, sizeof(client));
            sceKernelPread(g_ipc_fd, &client, sizeof(client), (off_t)sizeof(XboxIpcMasterData));

            if (client.shellui_pid > 0 && client.shellui_seq != g_last_seen_shellui_seq) {
                g_last_seen_shellui_seq = client.shellui_seq;
                g_last_shellui_change_us = now_us;
            }
            int shellui_alive = (g_last_shellui_change_us != 0 &&
                                 (now_us - g_last_shellui_change_us) < XBOX_IPC_TIMEOUT_US);

            if (client.fg_game_pid > 0 && client.fg_game_seq != g_last_seen_fg_game_seq) {
                g_last_seen_fg_game_seq = client.fg_game_seq;
                g_last_fg_game_change_us = now_us;
            }
            int fg_game_active = (client.fg_game_pid > 0 &&
                                  g_last_fg_game_change_us != 0 &&
                                  (now_us - g_last_fg_game_change_us) < XBOX_IPC_TIMEOUT_US);

            int client_dirty = 0;
            if (g_is_shellui) {
                client.shellui_pid = g_my_pid;
                client.shellui_seq++;
                client_dirty = 1;
            } else {
                if (!g_game_intercepted) {
                    client.fg_game_pid = g_my_pid;
                    client.fg_game_seq++;
                    client_dirty = 1;
                } else if (client.fg_game_pid == g_my_pid) {
                    client.fg_game_pid = 0;
                    client_dirty = 1;
                }
            }

            if (client.rumble_seq != g_last_ipc_rumble_seq && g_xbox_handle != NULL) {
                g_last_ipc_rumble_seq = client.rumble_seq;
                xbox_spin_lock();
                g_shared.rumble_large = client.rumble_large;
                g_shared.rumble_small = client.rumble_small;
                g_shared.rumble_dirty = 1;
                xbox_spin_unlock();
            }

            if (client_dirty) {
                sceKernelPwrite(g_ipc_fd, &client, sizeof(client), (off_t)sizeof(XboxIpcMasterData));
            }

            should_yield_usb = (g_is_shellui && fg_game_active) ||
                               (!g_is_shellui && g_game_intercepted && shellui_alive);

            if (should_yield_usb && g_xbox_handle != NULL) {
                close_controller_ex(0, 1);
            }
        }

        // Periodically refresh logged-in user count (~2 s) so AUTO mode reacts to user login/logout system-wide
        if (++user_refresh_counter >= 500) {
            user_refresh_counter = 0;
            refresh_user_service_state();
        }

        if (g_xbox_handle == NULL) {
            int handoff_pending = 0;
            // Check if another process is the active USB Master via IPC
            if (poll_remote_ipc_master(now_us, &handoff_pending)) {
                sceKernelUsleep(USB_POLL_INTERVAL_US);
                continue;
            }

            if (g_using_remote_ipc && !handoff_pending) {
                g_using_remote_ipc = 0;
                xbox_spin_lock();
                g_shared.connected = 0;
                g_shared.has_data = 0;
                xbox_spin_unlock();
                log_event(LOG_LEVEL_INFO, "Remote USB Master gone; switching to local USB scan");
            }

            if (!should_yield_usb) {
                // Claim immediately if handoff_pending, otherwise hotplug scan every ~1 s (250 * 4 ms)
                if (handoff_pending || ++scan_counter >= 250) {
                    scan_counter = 0;
                    scan_and_open();
                }
            }
            sceKernelUsleep(USB_POLL_INTERVAL_US);
            continue;
        }

        // We are the USB Master! Read from physical USB endpoint
        uint8_t rx_buf[64];
        int32_t transferred = 0;
        int ret = p_sceUsbdInterruptTransfer(g_xbox_handle, g_ep_in, rx_buf, sizeof(rx_buf), &transferred, 4);

        if (ret == 0 && transferred > 0) {
            consecutive_errors = 0;

            if (g_shared.type == CONTROLLER_XBOX360) {
                if (transferred >= 20 && xbox360_report_valid((const Xbox360Report*)rx_buf)) {
                    xbox_spin_lock();
                    memcpy(&g_shared.rpt.x360, rx_buf, sizeof(Xbox360Report));
                    g_shared.has_data = 1;
                    g_shared.last_report_us = now_us;
                    xbox_spin_unlock();
                }
            } else if (g_shared.type == CONTROLLER_XBOXONE) {
                if (rx_buf[0] == XBOXONE_REPORT_INPUT && transferred >= 18) {
                    xbox_spin_lock();
                    memcpy(&g_shared.rpt.xone, rx_buf, sizeof(XboxOneReport));
                    g_shared.has_data = 1;
                    g_shared.last_report_us = now_us;
                    xbox_spin_unlock();
                } else if (rx_buf[0] == XBOXONE_REPORT_GUIDE && transferred >= 6) {
                    // Passo 8 (F-006): Guide button report 0x07
                    xbox_spin_lock();
                    g_shared.guide = (rx_buf[4] & 0x01) ? 1 : 0;
                    g_shared.last_report_us = now_us;
                    xbox_spin_unlock();
                }
            } else if (g_shared.type == CONTROLLER_SWITCH) {
                if (transferred >= 7) {
                    xbox_spin_lock();
                    memcpy(&g_shared.rpt.sw, rx_buf, sizeof(SwitchInputOnlyReport));
                    g_shared.has_data = 1;
                    g_shared.last_report_us = now_us;
                    xbox_spin_unlock();
                }
            }
        } else if (ret < 0) {
            // Note: on PS4 sceUsbdInterruptTransfer, timeout also returns < 0 (SCE_USBD_ERROR_TIMEOUT).
            // Only disconnect if sceUsbdCheckConnected reports disconnected!
            if (p_sceUsbdCheckConnected != NULL && p_sceUsbdCheckConnected(g_xbox_handle) != 0) {
                consecutive_errors++;
                if (consecutive_errors >= 3) {
                    log_event(LOG_LEVEL_WARN, "USB controller disconnected (ret=%d)", ret);
                    close_controller();
                    consecutive_errors = 0;
                    continue;
                }
            } else {
                consecutive_errors = 0;
            }
        }

        // Update translated state & special button timers (Share pulse / Touchpad / Mode combo)
        update_translated_pad_in_poll_thread();

        uint8_t cur_guide = (g_shared.pad.buttons & DS4_BUTTON_TOUCHPAD) ? 1 : 0;
        int guide_edge = (cur_guide != g_prev_guide_state);
        g_prev_guide_state = cur_guide;

        // Publish Master state to IPC every 4 ticks (~16 ms = 62.5 Hz) or immediately on Guide/PS button edge
        if (guide_edge || ++ipc_counter >= 4) {
            ipc_counter = 0;
            ipc_write_master_state(1);
        }

        // Check if rumble output is pending
        int need_rumble = 0;
        uint8_t r_large = 0, r_small = 0;
        xbox_spin_lock();
        if (g_shared.rumble_dirty) {
            need_rumble = 1;
            r_large = g_shared.rumble_large;
            r_small = g_shared.rumble_small;
            g_shared.rumble_dirty = 0;
        }
        xbox_spin_unlock();

        if (need_rumble) {
            send_rumble_packet(r_large, r_small);
        }

        sceKernelUsleep(USB_POLL_INTERVAL_US);
    }

    if (g_ipc_fd >= 0 && !g_is_shellui) {
        XboxIpcClientData client;
        memset(&client, 0, sizeof(client));
        sceKernelPread(g_ipc_fd, &client, sizeof(client), (off_t)sizeof(XboxIpcMasterData));
        if (client.fg_game_pid == g_my_pid) {
            client.fg_game_pid = 0;
            sceKernelPwrite(g_ipc_fd, &client, sizeof(client), (off_t)sizeof(XboxIpcMasterData));
        }
    }

    if (g_xbox_handle != NULL) {
        ipc_write_master_state_ex(0, 1);
    }

    log_event(LOG_LEVEL_INFO, "Polling thread stopped");
    log_flush();
    return NULL;
}

// Snapshot translated Xbox input (safe, non-blocking, hook-callable, ~15ns spinlock)
static int get_xbox_translated_pad(OrbisPadData* out) {
    int ok = 0;
    xbox_spin_lock();
    if (g_shared.connected && g_shared.has_data) {
        *out = g_shared.pad;
        ok = 1;
    }
    xbox_spin_unlock();
    return ok;
}

// Axis merge rule: value furthest from center (128)
static inline uint8_t merge_stick_axis(uint8_t ds4_val, uint8_t xbox_val) {
    int diff_ds4 = abs((int)ds4_val - 128);
    int diff_xbox = abs((int)xbox_val - 128);
    return (diff_xbox > diff_ds4) ? xbox_val : ds4_val;
}

// Trigger merge rule: maximum
static inline uint8_t merge_trigger_val(uint8_t ds4_val, uint8_t xbox_val) {
    return (xbox_val > ds4_val) ? xbox_val : ds4_val;
}

// Merge Xbox pad data onto DS4 pad data (Passo 5, Section 3.2)
static void merge_xbox_into_ds4(OrbisPadData* ds4, const OrbisPadData* xbox) {
    ds4->buttons |= xbox->buttons;
    ds4->leftStick.x = merge_stick_axis(ds4->leftStick.x, xbox->leftStick.x);
    ds4->leftStick.y = merge_stick_axis(ds4->leftStick.y, xbox->leftStick.y);
    ds4->rightStick.x = merge_stick_axis(ds4->rightStick.x, xbox->rightStick.x);
    ds4->rightStick.y = merge_stick_axis(ds4->rightStick.y, xbox->rightStick.y);
    ds4->analogButtons.l2 = merge_trigger_val(ds4->analogButtons.l2, xbox->analogButtons.l2);
    ds4->analogButtons.r2 = merge_trigger_val(ds4->analogButtons.r2, xbox->analogButtons.r2);
    ds4->connected = 1;
}

// Public interface
int hooks_init_usb(void) {
    if (g_usb_initialized) return 0;

    // In homebrew, operate purely in MERGE (Player 1) mode
    if (g_is_homebrew) {
        g_mode = PLUGIN_MODE_MERGE;
        g_effective_mode = PLUGIN_MODE_MERGE;
    } else {
        // Apply configured initial mode (auto, merge, secondary)
        if (g_config.mode == CONFIG_MODE_MERGE) {
            g_mode = PLUGIN_MODE_MERGE;
        } else if (g_config.mode == CONFIG_MODE_SECONDARY) {
            g_mode = PLUGIN_MODE_SECONDARY;
        } else {
            g_mode = PLUGIN_MODE_AUTO;
        }
        update_effective_mode_cached();
    }

    g_shared_lock = 0;
    memset(&g_shared, 0, sizeof(g_shared));

    open_ipc_file();

    int h = 0;
    if (load_sys_prx("libSceUsbd.sprx", &h) < 0 || h == 0) {
        log_event(LOG_LEVEL_WARN, "libSceUsbd.sprx load failed; IPC-only client mode");
        return -1;
    }
    g_usb_prx_loaded = 1;

    sys_dynlib_dlsym(h, "sceUsbdInit", (void**)&p_sceUsbdInit);
    sys_dynlib_dlsym(h, "sceUsbdExit", (void**)&p_sceUsbdExit);
    sys_dynlib_dlsym(h, "sceUsbdGetDeviceList", (void**)&p_sceUsbdGetDeviceList);
    sys_dynlib_dlsym(h, "sceUsbdFreeDeviceList", (void**)&p_sceUsbdFreeDeviceList);
    sys_dynlib_dlsym(h, "sceUsbdGetDeviceDescriptor", (void**)&p_sceUsbdGetDeviceDescriptor);
    sys_dynlib_dlsym(h, "sceUsbdGetActiveConfigDescriptor", (void**)&p_sceUsbdGetActiveConfigDescriptor);
    sys_dynlib_dlsym(h, "sceUsbdFreeConfigDescriptor", (void**)&p_sceUsbdFreeConfigDescriptor);
    sys_dynlib_dlsym(h, "sceUsbdOpen", (void**)&p_sceUsbdOpen);
    sys_dynlib_dlsym(h, "sceUsbdClose", (void**)&p_sceUsbdClose);
    sys_dynlib_dlsym(h, "sceUsbdDetachKernelDriver", (void**)&p_sceUsbdDetachKernelDriver);
    sys_dynlib_dlsym(h, "sceUsbdClaimInterface", (void**)&p_sceUsbdClaimInterface);
    sys_dynlib_dlsym(h, "sceUsbdReleaseInterface", (void**)&p_sceUsbdReleaseInterface);
    sys_dynlib_dlsym(h, "sceUsbdSetInterfaceAltSetting", (void**)&p_sceUsbdSetInterfaceAltSetting);
    sys_dynlib_dlsym(h, "sceUsbdInterruptTransfer", (void**)&p_sceUsbdInterruptTransfer);
    sys_dynlib_dlsym(h, "sceUsbdCheckConnected", (void**)&p_sceUsbdCheckConnected);

    if (p_sceUsbdInit == NULL || p_sceUsbdGetDeviceList == NULL ||
        p_sceUsbdOpen == NULL || p_sceUsbdClaimInterface == NULL ||
        p_sceUsbdInterruptTransfer == NULL) {
        log_event(LOG_LEVEL_ERROR, "Failed to resolve core libSceUsbd symbols");
        return -1;
    }

    int ret = p_sceUsbdInit();
    if (ret != 0) {
        log_event(LOG_LEVEL_ERROR, "sceUsbdInit failed: %d", ret);
        return -1;
    }
    g_usb_initialized = 1;

    // Only scan USB immediately if no remote USB Master is already active in IPC
    if (!poll_remote_ipc_master(sceKernelGetProcessTime(), NULL)) {
        scan_and_open();
    }

    return 0;
}

// ============================================
// User Service Hook
// ============================================

int32_t sceUserServiceGetLoginUserIdList_hook(OrbisUserServiceLoginUserIdList* userIdList) {
    int32_t ret = HOOK_CONTINUE(sceUserServiceGetLoginUserIdList, sceUserServiceGetLoginUserIdList_t, userIdList);
    if (ret == 0 && userIdList != NULL) {
        g_logged_in_users = count_valid_users_in_list(userIdList);
        update_effective_mode_cached();
    }
    return ret;
}

// ============================================
// Pad Open / Close Hooks (Passo 4 & 5)
// ============================================

int32_t scePadOpen_hook(int32_t userId, int32_t type, int32_t index, void* param) {
    refresh_user_service_state();
    PluginMode eff = g_effective_mode;

    if (eff == PLUGIN_MODE_SECONDARY && g_shared.connected) {
        int32_t fg_user = g_foreground_user_id;
        if (fg_user != 0 && userId != fg_user && userId > 0) {
            if (!g_virtual_pad_open || g_xbox_user_id == userId) {
                g_xbox_user_id = userId;
                g_virtual_pad_open = 1;
                log_event(LOG_LEVEL_INFO, "Assigned virtual pad 1001 to P2 user 0x%X (fg=0x%X)", userId, fg_user);
                log_notify("Xbox -> Player 2 (0x%X)", userId);
                return XBOX_VIRTUAL_PAD_HANDLE;
            }
        }
    }

    // In MERGE mode (or P1 in SECONDARY mode), use the real DS4 handle
    int32_t ret = HOOK_CONTINUE(scePadOpen, scePadOpen_t, userId, type, index, param);
    if (ret >= 0) {
        register_open_handle(ret, userId);
        log_event(LOG_LEVEL_INFO, "scePadOpen(user=0x%X, type=%d, idx=%d) -> handle %d (mode=%s)",
                  userId, type, index, ret, (eff == PLUGIN_MODE_MERGE) ? "MERGE" : "SECONDARY");
    }
    return ret;
}

int32_t scePadClose_hook(int32_t handle) {
    xbox_spin_lock();
    g_shared.rumble_large = 0;
    g_shared.rumble_small = 0;
    g_shared.rumble_dirty = 1;
    xbox_spin_unlock();

    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        g_virtual_pad_open = 0;
        g_xbox_user_id = 0;
        log_event(LOG_LEVEL_INFO, "Virtual pad 1001 closed");
        return 0;
    }

    unregister_open_handle(handle);
    return HOOK_CONTINUE(scePadClose, scePadClose_t, handle);
}

// ============================================
// Controller Info & Virtual Handle Stubs (Passo 6, F-020)
// ============================================

int32_t scePadGetControllerInformation_hook(int32_t handle, OrbisPadInformation* info) {
    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        if (info != NULL) {
            memset(info, 0, sizeof(OrbisPadInformation));
            info->connected = g_shared.connected ? 1 : 0;
            info->connectionType = ORBIS_PAD_CONNECTION_TYPE_STANDARD;
            info->deviceClass = ORBIS_PAD_DEVICE_CLASS_PAD;
            info->touchpadDensity = 1.0f;
            info->touchResolutionX = 1920;
            info->touchResolutionY = 943;
        }
        return 0;
    }

    int32_t ret = HOOK_CONTINUE(scePadGetControllerInformation, scePadGetControllerInformation_t, handle, info);
    if (info != NULL) {
        if (ret == 0 && info->connected != 0 && handle != g_fallback_p2_handle) {
            g_known_p1_handle = handle;
        }
        if (g_shared.connected) {
            PluginMode eff = g_effective_mode;
            if (eff == PLUGIN_MODE_MERGE || g_is_homebrew) {
                if (g_known_p1_handle == 0 || handle == g_known_p1_handle) {
                    if (g_known_p1_handle == 0) {
                        g_known_p1_handle = handle;
                    }
                    info->connected = 1;
                    if (info->count == 0) info->count = 1;
                    if (ret != 0) {
                        info->connectionType = ORBIS_PAD_CONNECTION_TYPE_STANDARD;
                        info->deviceClass = ORBIS_PAD_DEVICE_CLASS_PAD;
                        info->touchpadDensity = 1.0f;
                        info->touchResolutionX = 1920;
                        info->touchResolutionY = 943;
                        ret = 0;
                    }
                }
            } else if (eff == PLUGIN_MODE_SECONDARY && !g_virtual_pad_open &&
                       handle == g_fallback_p2_handle && g_fallback_p2_handle > 0) {
                info->connected = 1;
                if (info->count == 0) info->count = 1;
            }
        }
    }
    return ret;
}

int32_t scePadGetHandle_hook(int32_t userID, uint32_t controller_type, uint32_t controller_index) {
    if (g_effective_mode == PLUGIN_MODE_SECONDARY && g_shared.connected &&
        g_xbox_user_id != 0 && userID == g_xbox_user_id) {
        return XBOX_VIRTUAL_PAD_HANDLE;
    }
    return HOOK_CONTINUE(scePadGetHandle, scePadGetHandle_t, userID, controller_type, controller_index);
}

int32_t scePadSetLightBar_hook(int32_t handle, OrbisPadColor *inputColor) {
    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        return 0;
    }
    return HOOK_CONTINUE(scePadSetLightBar, scePadSetLightBar_t, handle, inputColor);
}

int32_t scePadResetLightBar_hook(int32_t handle) {
    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        return 0;
    }
    return HOOK_CONTINUE(scePadResetLightBar, scePadResetLightBar_t, handle);
}

int32_t scePadSetMotionSensorState_hook(int32_t handle, int32_t enable) {
    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        return 0;
    }
    return HOOK_CONTINUE(scePadSetMotionSensorState, scePadSetMotionSensorState_t, handle, enable);
}

int32_t scePadSetVibration_hook(int32_t handle, const OrbisPadVibeParam *param) {
    if (param != NULL && g_shared.connected && g_config.enable_rumble) {
        xbox_spin_lock();
        if (g_shared.rumble_large != param->lgMotor || g_shared.rumble_small != param->smMotor) {
            g_shared.rumble_large = param->lgMotor;
            g_shared.rumble_small = param->smMotor;
            g_shared.rumble_dirty = 1;
        }
        xbox_spin_unlock();
    }

    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        return 0;
    }
    return HOOK_CONTINUE(scePadSetVibration, scePadSetVibration_t, handle, param);
}

// ============================================
// Pad Read Hooks (Passo 5, Merge & Secondary)
// ============================================

static int should_inject_into_disconnected_p2_handle(int32_t handle) {
    if (handle <= 0 || handle == g_known_p1_handle) {
        return 0;
    }
    if (g_fallback_p2_handle == 0 || g_fallback_p2_handle == handle) {
        g_fallback_p2_handle = handle;
        return 1;
    }
    return 0;
}

static void update_game_focus_state(const OrbisPadData* pData) {
    if (g_is_shellui || pData == NULL) return;
    if (pData->buttons & ORBIS_PAD_BUTTON_INTERCEPTED) {
        int frames = g_intercepted_frames + 1;
        if (frames > 100) frames = 100;
        g_intercepted_frames = frames;
        if (frames >= 8) {
            g_game_intercepted = 1;
        }
    } else {
        g_intercepted_frames = 0;
        g_game_intercepted = 0;
    }
}

int32_t scePadRead_hook(int32_t handle, OrbisPadData* pData, int32_t num) {
    PluginMode eff = g_effective_mode;

    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        if (!g_virtual_pad_open || pData == NULL || num <= 0) {
            return 0;
        }

        OrbisPadData xbox_pad;
        int has_data = (eff == PLUGIN_MODE_SECONDARY) ? get_xbox_translated_pad(&xbox_pad) : 0;
        uint64_t now_us = sceKernelGetProcessTime();

        for (int i = 0; i < num; i++) {
            if (has_data) {
                pData[i] = xbox_pad;
                pData[i].timestamp = now_us;
            } else {
                memset(&pData[i], 0, sizeof(OrbisPadData));
                pData[i].connected = g_shared.connected ? 1 : 0;
                pData[i].timestamp = now_us;
                pData[i].count = 1;
                pData[i].leftStick.x = 128;
                pData[i].leftStick.y = 128;
                pData[i].rightStick.x = 128;
                pData[i].rightStick.y = 128;
                pData[i].quat.w = 1.0f;
                pData[i].acell.z = 1.0f;
            }
        }
        return num;
    }

    // Real handle read
    int32_t ret = p_scePadReadExt(handle, pData, num, 0);
    if (pData == NULL || num <= 0) {
        return ret;
    }

    OrbisPadData xbox_pad;
    int has_xbox = get_xbox_translated_pad(&xbox_pad);

    if (g_is_homebrew) {
        if (ret > 0) {
            for (int i = 0; i < ret && i < num; i++) {
                pData[i].buttons &= ~ORBIS_PAD_BUTTON_INTERCEPTED;
                if (has_xbox) {
                    if (pData[i].connected != 0) {
                        merge_xbox_into_ds4(&pData[i], &xbox_pad);
                    } else {
                        pData[i] = xbox_pad;
                        pData[i].timestamp = sceKernelGetProcessTime();
                        pData[i].connected = 1;
                        pData[i].count = 1;
                    }
                }
            }
            return ret;
        } else if (has_xbox) {
            pData[0] = xbox_pad;
            pData[0].timestamp = sceKernelGetProcessTime();
            pData[0].connected = 1;
            pData[0].count = 1;
            pData[0].buttons &= ~ORBIS_PAD_BUTTON_INTERCEPTED;
            g_known_p1_handle = handle;
            return 1;
        }
        return ret;
    }

    if (ret < 0) {
        return ret;
    }

    if (ret > 0) {
        update_game_focus_state(&pData[0]);
        if (pData[0].connected != 0 && handle != g_fallback_p2_handle) {
            g_known_p1_handle = handle;
        }
    }

    if (!has_xbox) {
        return ret;
    }

    // 1. Check SECONDARY mode disconnected P2 handle first (both in Games and in SceShellUI)
    if (eff == PLUGIN_MODE_SECONDARY && !g_virtual_pad_open) {
        if ((ret == 0 || pData[0].connected == 0) && should_inject_into_disconnected_p2_handle(handle)) {
            int count = (ret > 0) ? ret : 1;
            uint64_t now_us = sceKernelGetProcessTime();
            for (int i = 0; i < count && i < num; i++) {
                if (pData[i].buttons & ORBIS_PAD_BUTTON_INTERCEPTED) {
                    continue;
                }
                pData[i] = xbox_pad;
                pData[i].timestamp = now_us;
                pData[i].connected = 1;
                pData[i].count = 1;
            }
            return count;
        }
    }

    // 2. In MERGE mode (or in SceShellUI when no secondary handle is active), overlay onto P1 real handle
    if (eff == PLUGIN_MODE_MERGE || (g_is_shellui && !g_virtual_pad_open && g_fallback_p2_handle == 0)) {
        if (ret > 0) {
            for (int i = 0; i < ret && i < num; i++) {
                if (pData[i].buttons & ORBIS_PAD_BUTTON_INTERCEPTED) {
                    continue;
                }
                if (pData[i].connected != 0) {
                    merge_xbox_into_ds4(&pData[i], &xbox_pad);
                } else if (g_known_p1_handle == 0 || handle == g_known_p1_handle) {
                    g_known_p1_handle = handle;
                    pData[i] = xbox_pad;
                    pData[i].timestamp = sceKernelGetProcessTime();
                    pData[i].connected = 1;
                    pData[i].count = 1;
                }
            }
        } else if (g_known_p1_handle == 0 || handle == g_known_p1_handle) {
            g_known_p1_handle = handle;
            pData[0] = xbox_pad;
            pData[0].timestamp = sceKernelGetProcessTime();
            pData[0].connected = 1;
            pData[0].count = 1;
            return 1;
        }
    }

    return ret;
}

int32_t scePadReadState_hook(int32_t handle, OrbisPadData* pData) {
    PluginMode eff = g_effective_mode;

    if (handle == XBOX_VIRTUAL_PAD_HANDLE) {
        if (!g_virtual_pad_open || pData == NULL) {
            return -1;
        }

        OrbisPadData xbox_pad;
        int has_data = (eff == PLUGIN_MODE_SECONDARY) ? get_xbox_translated_pad(&xbox_pad) : 0;
        if (has_data) {
            *pData = xbox_pad;
            pData->timestamp = sceKernelGetProcessTime();
        } else {
            memset(pData, 0, sizeof(OrbisPadData));
            pData->connected = g_shared.connected ? 1 : 0;
            pData->timestamp = sceKernelGetProcessTime();
            pData->count = 1;
            pData->leftStick.x = 128;
            pData->leftStick.y = 128;
            pData->rightStick.x = 128;
            pData->rightStick.y = 128;
            pData->quat.w = 1.0f;
            pData->acell.z = 1.0f;
        }
        return 0;
    }

    // Real handle read
    int32_t ret = p_scePadReadStateExt(handle, pData, 0);
    if (pData == NULL) {
        return ret;
    }

    OrbisPadData xbox_pad;
    int has_xbox = get_xbox_translated_pad(&xbox_pad);

    if (g_is_homebrew) {
        if (ret == 0) {
            pData->buttons &= ~ORBIS_PAD_BUTTON_INTERCEPTED;
            if (has_xbox) {
                if (pData->connected != 0) {
                    merge_xbox_into_ds4(pData, &xbox_pad);
                } else {
                    *pData = xbox_pad;
                    pData->timestamp = sceKernelGetProcessTime();
                    pData->connected = 1;
                    pData->count = 1;
                }
            }
            return 0;
        } else if (has_xbox) {
            *pData = xbox_pad;
            pData->timestamp = sceKernelGetProcessTime();
            pData->connected = 1;
            pData->count = 1;
            pData->buttons &= ~ORBIS_PAD_BUTTON_INTERCEPTED;
            g_known_p1_handle = handle;
            return 0;
        }
        return ret;
    }

    if (ret != 0) {
        return ret;
    }

    update_game_focus_state(pData);

    if (pData->connected != 0 && handle != g_fallback_p2_handle) {
        g_known_p1_handle = handle;
    }

    if (pData->buttons & ORBIS_PAD_BUTTON_INTERCEPTED) {
        return ret;
    }

    if (!has_xbox) {
        return ret;
    }

    // 1. Check SECONDARY mode disconnected P2 handle first (both in Games and in SceShellUI)
    if (eff == PLUGIN_MODE_SECONDARY && !g_virtual_pad_open) {
        if (pData->connected == 0 && should_inject_into_disconnected_p2_handle(handle)) {
            *pData = xbox_pad;
            pData->timestamp = sceKernelGetProcessTime();
            pData->connected = 1;
            pData->count = 1;
            return ret;
        }
    }

    // 2. In MERGE mode (or in SceShellUI when no secondary handle is active), overlay onto P1 real handle
    if (eff == PLUGIN_MODE_MERGE || (g_is_shellui && !g_virtual_pad_open && g_fallback_p2_handle == 0)) {
        if (pData->connected != 0) {
            merge_xbox_into_ds4(pData, &xbox_pad);
        } else if (g_known_p1_handle == 0 || handle == g_known_p1_handle) {
            g_known_p1_handle = handle;
            *pData = xbox_pad;
            pData->timestamp = sceKernelGetProcessTime();
            pData->connected = 1;
            pData->count = 1;
        }
    }

    return ret;
}

// ============================================
// Safe Optional Hook Helper & Hook Installation
// ============================================

static int g_hooked_scePadGetHandle = 0;
static int g_hooked_scePadSetLightBar = 0;
static int g_hooked_scePadResetLightBar = 0;
static int g_hooked_scePadSetMotionSensorState = 0;
static int g_hooked_scePadSetVibration = 0;

static int is_within_rel32(uintptr_t from_next_ip, uintptr_t dest) {
    int64_t diff = (int64_t)dest - (int64_t)from_next_ip;
    return (diff >= -0x7FFFFFF0LL && diff <= 0x7FFFFFF0LL);
}

static void* alloc_executable_trampoline(uintptr_t target_addr) {
    static void* s_page = NULL;
    static size_t s_offset = 0;
    uintptr_t next_ip = target_addr + sizeof(Detour_scePadRead.JumpInstructions32); // target_addr + 5

    // If current allocated executable page is within +-2GB and has space for a 16-byte trampoline:
    if (s_page != NULL && (s_offset + 16 <= 4096)) {
        uintptr_t cand = (uintptr_t)s_page + s_offset;
        if (is_within_rel32(next_ip, cand)) {
            s_offset += 16;
            return (void*)cand;
        }
    }

    // Allocate a new executable page near target_addr using hinted mmap
    intptr_t deltas[] = {
        -0x10000, 0x10000,
        -0x100000, 0x100000,
        -0x1000000, 0x1000000,
        -0x10000000, 0x10000000
    };
    for (size_t i = 0; i < sizeof(deltas) / sizeof(deltas[0]); i++) {
        uintptr_t hint = (target_addr + deltas[i]) & ~4095ULL;
        void* mapped = NULL;
        if (sceKernelMmap((void*)hint, 4096, VM_PROT_ALL, 0x1000 | 0x2, -1, 0, &mapped) >= 0 && mapped != NULL) {
            if (is_within_rel32(next_ip, (uintptr_t)mapped)) {
                s_page = mapped;
                s_offset = 16;
                return mapped;
            }
            sceKernelMunmap(mapped, 4096);
        }
    }
    return NULL;
}

static int safe_hook_optional(Detour* detour, const char* name, volatile uintptr_t fn_addr, void* hook_fn) {
    if (fn_addr == 0) {
        log_event(LOG_LEVEL_WARN, "Optional hook %s skipped: symbol is NULL", name);
        return 0;
    }

    const uint8_t* p = (const uint8_t*)fn_addr;
    uintptr_t target = fn_addr;
    if (p[0] == 0xE9) {
        int32_t rel = *(const int32_t*)(fn_addr + 1);
        target = (uintptr_t)((intptr_t)(fn_addr + 5) + (intptr_t)rel);
        p = (const uint8_t*)target;
    }

    size_t offset = 0;
    while (offset < 14) {
        hde64s hs;
        memset(&hs, 0, sizeof(hs));
        unsigned int len = hde64_disasm((const void*)(target + offset), &hs);
        if (len == 0 || (hs.flags & F_ERROR)) {
            log_event(LOG_LEVEL_WARN, "Optional hook %s skipped: disasm error at +%zu", name, offset);
            return 0;
        }
        if (hs.flags & F_RELATIVE) {
            log_event(LOG_LEVEL_WARN, "Optional hook %s skipped: relative branch/call (op=0x%02X) at +%zu",
                      name, hs.opcode, offset);
            return 0;
        }
        if ((hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5) {
            log_event(LOG_LEVEL_WARN, "Optional hook %s skipped: RIP-relative memory operand at +%zu",
                      name, offset);
            return 0;
        }
        offset += len;
    }

    Detour_Construct(detour, DetourMode_x64);
    void* stub = Detour_DetourFunction(detour, (uint64_t)target, hook_fn);
    if (stub == NULL) {
        log_event(LOG_LEVEL_WARN, "Optional hook %s failed in Detour_DetourFunction64", name);
        return 0;
    }

    log_event(LOG_LEVEL_INFO, "Optional hook %s installed (64-bit detour @%p, stub=%p)",
              name, (void*)target, stub);
    return 1;
}

extern size_t Detour_GetInstructionSize(Detour *This, uint64_t Address, size_t MinSize);
extern void   Detour_WriteJump64(Detour *This, void *Address, uint64_t Destination);
extern void   Detour_WriteJump32(Detour *This, void *Address, uint64_t Destination);
extern void*  Detour_DetourFunction64(Detour *This, uint64_t FunctionPtr, void *HookPtr);

static int safe_install_detour32(Detour* detour, const char* name, uintptr_t target_addr, void* hook_fn) {
    if (target_addr == 0 || hook_fn == NULL) {
        log_event(LOG_LEVEL_ERROR, "Detour32 %s failed: NULL address", name);
        return -1;
    }

    Detour_Construct(detour, DetourMode_x32);
    size_t inst_size = Detour_GetInstructionSize(detour, (uint64_t)target_addr, sizeof(detour->JumpInstructions32));
    if (inst_size < sizeof(detour->JumpInstructions32)) {
        log_event(LOG_LEVEL_ERROR, "Detour32 %s failed: inst_size=%zu < 5", name, inst_size);
        return -1;
    }

    // If the function starts with a 5-byte E9 jump stub, follow it with proper signed 32-bit offset
    const uint8_t* p = (const uint8_t*)target_addr;
    if (p[0] == 0xE9 && inst_size == sizeof(detour->JumpInstructions32)) {
        int32_t rel = *(const int32_t*)(target_addr + 1);
        uintptr_t dest = (uintptr_t)((intptr_t)(target_addr + 5) + (intptr_t)rel);
        detour->Mode = DetourMode_x64;
        void* stub = Detour_DetourFunction64(detour, (uint64_t)dest, hook_fn);
        return (stub != NULL) ? 0 : -1;
    }

    void* tramp = alloc_executable_trampoline(target_addr);

    // If no ±2 GB trampoline is available, fall back to 64-bit detour if function is >= 14 bytes
    if (tramp == NULL) {
        if (Detour_GetInstructionSize(detour, (uint64_t)target_addr, sizeof(detour->JumpInstructions64)) >= sizeof(detour->JumpInstructions64)) {
            detour->Mode = DetourMode_x64;
            void* stub = Detour_DetourFunction64(detour, (uint64_t)target_addr, hook_fn);
            return (stub != NULL) ? 0 : -1;
        }
        log_event(LOG_LEVEL_ERROR, "Detour32 %s aborted: no executable trampoline within +-2GB of %p", name, (void*)target_addr);
        return -1;
    }

    detour->TrampolinePtr = tramp;
    Detour_WriteJump64(detour, detour->TrampolinePtr, (uint64_t)hook_fn);

    detour->FunctionPtr = (void*)target_addr;
    detour->HookPtr = hook_fn;

    sceKernelMprotect((void*)target_addr, inst_size, VM_PROT_ALL);

    size_t stub_alloc_size = inst_size + sizeof(detour->JumpInstructions64);
    if (sceKernelMmap(0, stub_alloc_size, VM_PROT_ALL, 0x1000 | 0x2, -1, 0, &detour->StubPtr) < 0 ||
        detour->StubPtr == NULL) {
        log_event(LOG_LEVEL_ERROR, "Detour32 %s failed: stub mmap failed", name);
        return -1;
    }

    // Set StubSize to inst_size + 5 so Detour_RestoreFunction restores exactly inst_size bytes (not inst_size + 9)
    detour->StubSize = inst_size + sizeof(detour->JumpInstructions32);

    memcpy(detour->StubPtr, (const void*)target_addr, inst_size);
    Detour_WriteJump64(detour, (void*)((uintptr_t)detour->StubPtr + inst_size), (uint64_t)(target_addr + inst_size));

    memset((void*)target_addr, 0x90, inst_size);
    Detour_WriteJump32(detour, (void*)target_addr, (uint64_t)detour->TrampolinePtr);

    return 0;
}

int hooks_install(void) {
    if (g_hooks_installed) return 0;

    int pad_h = 0;
    if (load_sys_prx("libScePad.sprx", &pad_h) < 0 || pad_h == 0) {
        log_event(LOG_LEVEL_ERROR, "libScePad.sprx load failed");
        return -1;
    }
    g_pad_prx_loaded = 1;

    sys_dynlib_dlsym(pad_h, "scePadReadExt", (void**)&p_scePadReadExt);
    sys_dynlib_dlsym(pad_h, "scePadReadStateExt", (void**)&p_scePadReadStateExt);
    sys_dynlib_dlsym(pad_h, "scePadRead", (void**)&addr_scePadRead);
    sys_dynlib_dlsym(pad_h, "scePadReadState", (void**)&addr_scePadReadState);
    sys_dynlib_dlsym(pad_h, "scePadOpen", (void**)&addr_scePadOpen);
    sys_dynlib_dlsym(pad_h, "scePadClose", (void**)&addr_scePadClose);
    sys_dynlib_dlsym(pad_h, "scePadGetControllerInformation", (void**)&addr_scePadGetControllerInformation);
    sys_dynlib_dlsym(pad_h, "scePadSetVibration", (void**)&addr_scePadSetVibration);
    sys_dynlib_dlsym(pad_h, "scePadGetHandle", (void**)&addr_scePadGetHandle);
    sys_dynlib_dlsym(pad_h, "scePadSetLightBar", (void**)&addr_scePadSetLightBar);
    sys_dynlib_dlsym(pad_h, "scePadResetLightBar", (void**)&addr_scePadResetLightBar);
    sys_dynlib_dlsym(pad_h, "scePadSetMotionSensorState", (void**)&addr_scePadSetMotionSensorState);

    if (p_scePadReadExt == NULL || p_scePadReadStateExt == NULL) {
        log_event(LOG_LEVEL_ERROR, "p_scePadReadExt or p_scePadReadStateExt symbol missing");
        return -1;
    }

    // Prologue patchers (xor ecx, ecx / xor edx, edx) installed for both homebrew and retail
    if (!g_padReadExtPatcher) {
        g_padReadExtPatcher = (Patcher*)malloc(sizeof(Patcher));
        if (g_padReadExtPatcher) {
            Patcher_Construct(g_padReadExtPatcher);
            uint8_t xor_ecx_ecx[5] = {0x31, 0xC9, 0x90, 0x90, 0x90};
            Patcher_Install_Patch(g_padReadExtPatcher, (uint64_t)p_scePadReadExt, xor_ecx_ecx, sizeof(xor_ecx_ecx));
        }
    }

    if (!g_padReadStateExtPatcher) {
        g_padReadStateExtPatcher = (Patcher*)malloc(sizeof(Patcher));
        if (g_padReadStateExtPatcher) {
            Patcher_Construct(g_padReadStateExtPatcher);
            uint8_t xor_edx_edx[5] = {0x31, 0xD2, 0x90, 0x90, 0x90};
            Patcher_Install_Patch(g_padReadStateExtPatcher, (uint64_t)p_scePadReadStateExt, xor_edx_edx, sizeof(xor_edx_edx));
        }
    }

    if (g_is_homebrew) {
        log_event(LOG_LEVEL_INFO, "Installing lightweight Homebrew hooks (MERGE only)...");
        log_flush();

        if (addr_scePadRead == 0 || addr_scePadReadState == 0) {
            log_event(LOG_LEVEL_ERROR, "Required pad read symbols missing in homebrew");
            log_flush();
            return -1;
        }

        if (safe_install_detour32(&Detour_scePadRead, "scePadRead", addr_scePadRead, (void*)&scePadRead_hook) < 0 ||
            safe_install_detour32(&Detour_scePadReadState, "scePadReadState", addr_scePadReadState, (void*)&scePadReadState_hook) < 0) {
            log_event(LOG_LEVEL_ERROR, "Homebrew pad read detours failed");
            log_flush();
            return -1;
        }

        if (addr_scePadGetControllerInformation != 0) {
            safe_install_detour32(&Detour_scePadGetControllerInformation, "scePadGetControllerInformation",
                                  addr_scePadGetControllerInformation, (void*)&scePadGetControllerInformation_hook);
        }

        g_hooked_scePadSetVibration = safe_hook_optional(
            &Detour_scePadSetVibration, "scePadSetVibration",
            addr_scePadSetVibration, (void*)&scePadSetVibration_hook);

        g_hooks_installed = 1;
        log_event(LOG_LEVEL_INFO, "Homebrew pad hooks installed successfully (pid=%d, MERGE mode)", g_my_pid);
        log_flush();

        // Start background polling thread
        if ((g_usb_initialized || g_ipc_fd >= 0) && !g_poll_running) {
            g_poll_running = 1;
            if (scePthreadCreate(&g_poll_thread, NULL, usb_poll_thread, NULL, "xbox_usb_poll") != 0) {
                g_poll_running = 0;
                log_event(LOG_LEVEL_ERROR, "Failed to create USB poll thread in homebrew");
                log_flush();
            }
        }
        return 0;
    }

    if (addr_scePadRead == 0 || addr_scePadReadState == 0 ||
        addr_scePadOpen == 0 || addr_scePadClose == 0 ||
        addr_scePadGetControllerInformation == 0) {
        log_event(LOG_LEVEL_ERROR, "Required libScePad symbols missing");
        return -1;
    }

    int user_h = 0;
    if (load_sys_prx("libSceUserService.sprx", &user_h) >= 0 && user_h != 0) {
        g_user_prx_loaded = 1;
        sys_dynlib_dlsym(user_h, "sceUserServiceGetForegroundUser", (void**)&p_sceUserServiceGetForegroundUser);
        sys_dynlib_dlsym(user_h, "sceUserServiceGetLoginUserIdList", (void**)&p_sceUserServiceGetLoginUserIdList);
        if (p_sceUserServiceGetForegroundUser != NULL) {
            int32_t fg = 0;
            if (p_sceUserServiceGetForegroundUser(&fg) == 0 && fg != 0) {
                g_foreground_user_id = fg;
            }
        }
    }



    // Install core pad hooks using dynamically resolved addresses & safe +-2GB trampoline verification
    if (safe_install_detour32(&Detour_scePadRead, "scePadRead", addr_scePadRead, (void*)&scePadRead_hook) < 0 ||
        safe_install_detour32(&Detour_scePadReadState, "scePadReadState", addr_scePadReadState, (void*)&scePadReadState_hook) < 0 ||
        safe_install_detour32(&Detour_scePadOpen, "scePadOpen", addr_scePadOpen, (void*)&scePadOpen_hook) < 0 ||
        safe_install_detour32(&Detour_scePadClose, "scePadClose", addr_scePadClose, (void*)&scePadClose_hook) < 0 ||
        safe_install_detour32(&Detour_scePadGetControllerInformation, "scePadGetControllerInformation",
                              addr_scePadGetControllerInformation, (void*)&scePadGetControllerInformation_hook) < 0) {
        log_event(LOG_LEVEL_ERROR, "Core pad detour installation failed");
        return -1;
    }

    if (g_user_prx_loaded && p_sceUserServiceGetLoginUserIdList != NULL) {
        safe_install_detour32(&Detour_sceUserServiceGetLoginUserIdList,
                              "sceUserServiceGetLoginUserIdList",
                              (uintptr_t)p_sceUserServiceGetLoginUserIdList,
                              (void*)&sceUserServiceGetLoginUserIdList_hook);
    }

    // Install additional pad hooks ONLY if prologue disassembly is verified safe (Passo 6)
    g_hooked_scePadSetVibration = safe_hook_optional(
        &Detour_scePadSetVibration, "scePadSetVibration",
        addr_scePadSetVibration, (void*)&scePadSetVibration_hook);
    g_hooked_scePadGetHandle = safe_hook_optional(
        &Detour_scePadGetHandle, "scePadGetHandle",
        addr_scePadGetHandle, (void*)&scePadGetHandle_hook);
    g_hooked_scePadSetLightBar = safe_hook_optional(
        &Detour_scePadSetLightBar, "scePadSetLightBar",
        addr_scePadSetLightBar, (void*)&scePadSetLightBar_hook);
    g_hooked_scePadResetLightBar = safe_hook_optional(
        &Detour_scePadResetLightBar, "scePadResetLightBar",
        addr_scePadResetLightBar, (void*)&scePadResetLightBar_hook);
    g_hooked_scePadSetMotionSensorState = safe_hook_optional(
        &Detour_scePadSetMotionSensorState, "scePadSetMotionSensorState",
        addr_scePadSetMotionSensorState, (void*)&scePadSetMotionSensorState_hook);

    refresh_user_service_state();

    g_hooks_installed = 1;
    log_event(LOG_LEVEL_INFO, "Pad hooks installed successfully (pid=%d, is_shellui=%d)", g_my_pid, g_is_shellui);

    // Start background polling thread (runs if either USB or IPC is available)
    if ((g_usb_initialized || g_ipc_fd >= 0) && !g_poll_running) {
        g_poll_running = 1;
        if (scePthreadCreate(&g_poll_thread, NULL, usb_poll_thread, NULL, "xbox_usb_poll") != 0) {
            g_poll_running = 0;
            log_event(LOG_LEVEL_ERROR, "Failed to create USB poll thread");
        }
    }

    return 0;
}

void hooks_remove(void) {
    // 1. Stop polling thread FIRST before touching USB
    if (g_poll_running) {
        g_poll_running = 0;
        scePthreadJoin(g_poll_thread, NULL);
    }

    if (g_is_homebrew) {
        if (g_hooks_installed) {
            UNHOOK(scePadRead);
            UNHOOK(scePadReadState);
            UNHOOK(scePadGetControllerInformation);
            if (g_hooked_scePadSetVibration) {
                UNHOOK(scePadSetVibration);
                g_hooked_scePadSetVibration = 0;
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
        if (g_usb_initialized) {
            close_controller_ex(0, 1);
            if (p_sceUsbdExit != NULL) {
                p_sceUsbdExit();
            }
            g_usb_initialized = 0;
        }
        if (g_ipc_fd >= 0) {
            sceKernelClose(g_ipc_fd);
            g_ipc_fd = -1;
        }
        return;
    }

    // 2. Unhook functions
    if (g_hooks_installed) {
        UNHOOK(scePadRead);
        UNHOOK(scePadReadState);
        UNHOOK(scePadOpen);
        UNHOOK(scePadClose);
        UNHOOK(scePadGetControllerInformation);

        if (g_hooked_scePadSetVibration) {
            UNHOOK(scePadSetVibration);
            g_hooked_scePadSetVibration = 0;
        }
        if (g_hooked_scePadGetHandle) {
            UNHOOK(scePadGetHandle);
            g_hooked_scePadGetHandle = 0;
        }
        if (g_hooked_scePadSetLightBar) {
            UNHOOK(scePadSetLightBar);
            g_hooked_scePadSetLightBar = 0;
        }
        if (g_hooked_scePadResetLightBar) {
            UNHOOK(scePadResetLightBar);
            g_hooked_scePadResetLightBar = 0;
        }
        if (g_hooked_scePadSetMotionSensorState) {
            UNHOOK(scePadSetMotionSensorState);
            g_hooked_scePadSetMotionSensorState = 0;
        }

        if (g_user_prx_loaded && p_sceUserServiceGetLoginUserIdList != NULL) {
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

    // 3. Release USB controller (silently hand off to SceShellUI) & exit USB
    if (g_usb_initialized) {
        close_controller_ex(0, 1);
        if (p_sceUsbdExit != NULL) {
            p_sceUsbdExit();
        }
        g_usb_initialized = 0;
    }

    if (g_ipc_fd >= 0) {
        sceKernelClose(g_ipc_fd);
        g_ipc_fd = -1;
    }

    g_virtual_pad_open = 0;
}

int hooks_is_virtual_handle(int handle) {
    return (handle == XBOX_VIRTUAL_PAD_HANDLE);
}

int hooks_handle_to_index(int handle) {
    return (handle == XBOX_VIRTUAL_PAD_HANDLE) ? 0 : -1;
}
