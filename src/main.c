/*
 * Xbox Controller Plugin for PS4 (GoldHEN)
 * System-Wide Game Support under [default]
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "config.h"
#include "hooks.h"
#include "log.h"

// OpenOrbis headers
#include <orbis/libkernel.h>

// GoldHEN SDK
#include <GoldHEN.h>

#define attr_public __attribute__((visibility("default")))

attr_public const char *g_pluginName = "xbox_controller";
attr_public const char *g_pluginDesc = "Xbox Controller Support for PS4 Games (P1 Merge & P2 Secondary)";
attr_public const char *g_pluginAuth = "xbox_controller_plugin";
attr_public uint32_t g_pluginVersion = 0x00000140;

static volatile int g_plugin_loaded = 0;
static volatile int g_plugin_active = 0;

/*
 * Check if the current process is a retail PlayStation 4 game title.
 * Retail games and official titles on PS4:
 * - Title IDs begin with "CUSA" (Americas/Europe/Worldwide)
 * - Or "PCAS", "PLAS" (Asia)
 *
 * Skipping non-game processes (Homebrew, system daemons, ShellUI) ensures that:
 * 1. Homebrew apps (Apollo, Cheats Manager, Store, Itemzflow, PS4-Explorer) NEVER crash
 *    when the plugin is loaded globally under [default] in plugins.ini.
 * 2. All retail games automatically load the plugin and have full Xbox controller support.
 */
static int is_game_process(const struct proc_info* info) {
    if (info == NULL) return 0;

    // Check game Title ID prefix
    if (strncmp(info->titleid, "CUSA", 4) == 0 ||
        strncmp(info->titleid, "PCAS", 4) == 0 ||
        strncmp(info->titleid, "PLAS", 4) == 0) {

        // Exclude background helper processes spawned by games or browser
        if (strstr(info->name, "WebProcess") != NULL ||
            strstr(info->name, "compiler") != NULL ||
            strstr(info->name, "Daemon") != NULL ||
            strstr(info->name, "Dialog") != NULL ||
            strstr(info->name, "SceCdlg") != NULL ||
            strstr(info->name, "SceParty") != NULL ||
            strstr(info->name, "SceVoice") != NULL ||
            strstr(info->name, "SceAvCapture") != NULL) {
            return 0;
        }

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

    // Under [default], only activate in retail games. Non-game processes
    // (Homebrew utilities, system daemons) are skipped cleanly so they never crash.
    if (!has_pinfo || !is_game_process(&pinfo)) {
        log_event(LOG_LEVEL_INFO, "Skipping non-game process '%s' titleid='%s' pid=%d",
                  has_pinfo ? pinfo.name : "unknown",
                  has_pinfo ? pinfo.titleid : "unknown",
                  has_pinfo ? pinfo.pid : -1);
        log_shutdown();
        return 0;
    }

    hooks_set_process_info(pinfo.pid, 0);

    log_event(LOG_LEVEL_INFO, "Plugin load (v%d.%d.%d) in game '%s' titleid='%s' pid=%d",
              PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH,
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
