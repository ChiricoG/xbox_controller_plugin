/*
 * Xbox Controller Plugin for PS4 (GoldHEN)
 * System-Wide Game Support under [default]
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "config.h"
#include "ini_parser.h"
#include "hooks.h"
#include "log.h"

// OpenOrbis headers
#include <orbis/libkernel.h>

// GoldHEN SDK
#include <GoldHEN.h>

#define attr_public __attribute__((visibility("default")))

attr_public const char *g_pluginName = "xbox_controller";
attr_public const char *g_pluginDesc = "Xbox Controller Support for PS4 Games & Homebrew (P1 Merge & P2 Secondary)";
attr_public const char *g_pluginAuth = "xbox_controller_plugin";
attr_public uint32_t g_pluginVersion = 0x00000141;

static volatile int g_plugin_loaded = 0;
static volatile int g_plugin_active = 0;

/*
 * Check if the Title ID matches standard retail game prefixes:
 * - "CUSA" (Americas / Europe / Worldwide)
 * - "PCAS", "PLAS" (Asia)
 */
static int is_retail_game(const struct proc_info* info) {
    if (info == NULL || info->titleid[0] == '\0') return 0;
    return (strncmp(info->titleid, "CUSA", 4) == 0 ||
            strncmp(info->titleid, "PCAS", 4) == 0 ||
            strncmp(info->titleid, "PLAS", 4) == 0);
}

/*
 * Check if the Title ID matches a homebrew application.
 * Homebrew packages use standard 9-character Title IDs (e.g. "XBOX00001",
 * "HBST00001", "APOL00004", "ITEM00001", "EXPL00001", "GOLD00777", etc.).
 * System applications and background daemons ("NPXS*", "SYSTEM") are excluded.
 */
static int is_homebrew_app(const struct proc_info* info) {
    if (info == NULL || info->titleid[0] == '\0') return 0;

    // Must be the main interactive application executable (never a child worker/helper)
    if (strcasecmp(info->name, "eboot.bin") != 0 &&
        strcasecmp(info->name, "eboot") != 0) {
        return 0;
    }

    // Must not be a retail game (handled by is_retail_game)
    if (is_retail_game(info)) return 0;

    // Exclude Sony system titles and shells
    if (strncmp(info->titleid, "NPXS", 4) == 0 ||
        strncmp(info->titleid, "SYSTEM", 6) == 0) {
        return 0;
    }

    // Validate 9-character alphanumeric Title ID
    size_t len = strlen(info->titleid);
    if (len != 9) return 0;

    for (size_t i = 0; i < 9; i++) {
        char c = info->titleid[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            return 0;
        }
    }

    return 1;
}

/*
 * Check if the process is a background daemon, system service, or compiler/helper.
 * These processes must NEVER be hooked to guarantee absolute system stability.
 */
static int is_blacklisted_process(const struct proc_info* info) {
    if (info == NULL || info->name[0] == '\0') return 1;

    const char* blacklisted[] = {
        "WebProcess", "compiler", "Daemon", "daemon",
        "Dialog", "SceCdlg", "SceParty", "SceVoice",
        "SceAvCapture", "SceSysCore", "mini-syscore",
        "SceShellCore", "SceNotificationDaemon",
        "SceGnmDriverDaemon", "SceVdecDaemon", "SceWebKitProcess"
    };

    for (size_t i = 0; i < sizeof(blacklisted) / sizeof(blacklisted[0]); i++) {
        if (strstr(info->name, blacklisted[i]) != NULL) {
            return 1;
        }
    }

    return 0;
}

/*
 * Check if the current process is an interactive target (retail game or homebrew).
 */
static int is_supported_process(const struct proc_info* info, int enable_homebrew) {
    if (info == NULL) return 0;
    if (is_blacklisted_process(info)) return 0;

    if (is_retail_game(info)) {
        return 1;
    }

    if (enable_homebrew && is_homebrew_app(info)) {
        return 1;
    }

    return 0;
}

int32_t attr_public plugin_load(int32_t argc, const char* argv[]) {
    (void)argc;
    (void)argv;

    // Ensure plugin_load runs at most once per process
    if (__atomic_exchange_n(&g_plugin_loaded, 1, __ATOMIC_ACQUIRE) != 0) {
        return 0;
    }

    struct proc_info pinfo;
    memset(&pinfo, 0, sizeof(pinfo));
    int has_pinfo = (sys_sdk_proc_info(&pinfo) == 0);

    log_init();

    // Load optional configuration early (fast static read, falls back to defaults)
    config_load(&g_config);

    // Validate process eligibility (retail game or homebrew)
    if (!has_pinfo || !is_supported_process(&pinfo, g_config.enable_homebrew)) {
        log_event(LOG_LEVEL_INFO, "Skipping process '%s' titleid='%s' pid=%d",
                  has_pinfo ? pinfo.name : "unknown",
                  has_pinfo ? pinfo.titleid : "unknown",
                  has_pinfo ? pinfo.pid : -1);
        log_shutdown();
        return 0;
    }

    int is_hb = is_homebrew_app(&pinfo);
    hooks_set_process_info(pinfo.pid, 0, is_hb);

    log_event(LOG_LEVEL_INFO, "Plugin load (v%d.%d.%d) in %s '%s' titleid='%s' pid=%d",
              PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH,
              is_retail_game(&pinfo) ? "game" : "homebrew",
              pinfo.name, pinfo.titleid, pinfo.pid);
    log_flush();

    // Initialize USB & IPC (non-fatal if direct USB fails - IPC client or hotplug will handle it)
    hooks_init_usb();
    log_flush();

    // Install pad hooks
    if (hooks_install() < 0) {
        log_event(LOG_LEVEL_WARN, "Hooks not installed in '%s'; cleaning up", pinfo.name);
        hooks_remove();
        log_shutdown();
        return 0;
    }

    g_plugin_active = 1;
    log_flush();
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char* argv[]) {
    (void)argc;
    (void)argv;
    if (g_plugin_active) {
        hooks_remove();
        g_plugin_active = 0;
    }
    log_shutdown();
    return 0;
}

int module_start(size_t argc, const void* argv) {
    (void)argc;
    (void)argv;
    return 0;
}

int module_stop(size_t argc, const void* argv) {
    (void)argc;
    (void)argv;
    return 0;
}
