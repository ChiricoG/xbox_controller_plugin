#include "config_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>

const std::vector<std::string> ConfigManager::MODE_OPTIONS = {
    "Auto", "Merge (P1)", "Secondary (P2)"
};

const std::vector<std::string> ConfigManager::GUIDE_OPTIONS = {
    "Touchpad", "Share", "Disabled"
};

const std::vector<std::string> ConfigManager::DEADZONE_OPTIONS = {
    "0%", "5%", "8%", "10%", "12%", "15%", "20%", "25%"
};

const std::vector<int> ConfigManager::DEADZONE_VALUES = {
    0, 6, 10, 13, 15, 19, 25, 32
};

const std::vector<std::string> ConfigManager::SWAP_OPTIONS = {
    "Disabled", "Enabled"
};

const std::vector<std::string> ConfigManager::RUMBLE_OPTIONS = {
    "Enabled", "Disabled"
};

const std::vector<std::string> ConfigManager::TRIGGER_OPTIONS = {
    "15 (6%)", "30 (12%)", "45 (18%)", "60 (24%)", "80 (31%)"
};

const std::vector<int> ConfigManager::TRIGGER_VALUES = {
    15, 30, 45, 60, 80
};

const std::vector<std::string> ConfigManager::NOTIF_OPTIONS = {
    "Enabled", "Disabled"
};

const std::vector<std::string> ConfigManager::COMBO_OPTIONS = {
    "Enabled", "Disabled"
};

const std::vector<std::string> ConfigManager::HOMEBREW_OPTIONS = {
    "Enabled", "Disabled"
};

const std::vector<std::string> ConfigManager::SWITCH_OPTIONS = {
    "Positional", "By Letter"
};

ConfigManager::ConfigManager() {
    config.set_defaults();
}

bool ConfigManager::file_exists(const char* path) {
    int fd = sceKernelOpen(path, O_RDONLY, 0);
    if (fd >= 0) {
        sceKernelClose(fd);
        return true;
    }
    return false;
}

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool ConfigManager::load() {
    if (!file_exists(INI_FILE_PATH)) {
        // First run: file does not exist yet! Create it with defaults.
        config.set_defaults();
        save();
        return true;
    }

    // File exists: load and parse current values
    config.set_defaults();

    int fd = sceKernelOpen(INI_FILE_PATH, O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    ssize_t bytes_read = (ssize_t)sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);

    if (bytes_read <= 0) {
        return false;
    }
    buf[bytes_read] = '\0';

    char* line = strtok(buf, "\r\n");
    while (line != NULL) {
        std::string s = trim(line);
        if (!s.empty() && s[0] != '#' && s[0] != ';' && s[0] != '[') {
            size_t eq = s.find('=');
            if (eq != std::string::npos) {
                std::string key = trim(s.substr(0, eq));
                std::string val = trim(s.substr(eq + 1));

                size_t cpos = val.find('#');
                if (cpos == std::string::npos) cpos = val.find(';');
                if (cpos != std::string::npos) val = trim(val.substr(0, cpos));

                if (strcasecmp(key.c_str(), "mode") == 0) {
                    if (strcasecmp(val.c_str(), "merge") == 0 || strcasecmp(val.c_str(), "p1") == 0) {
                        config.mode_index = 1;
                    } else if (strcasecmp(val.c_str(), "secondary") == 0 || strcasecmp(val.c_str(), "p2") == 0) {
                        config.mode_index = 2;
                    } else {
                        config.mode_index = 0;
                    }
                } else if (strcasecmp(key.c_str(), "guide_action") == 0 || strcasecmp(key.c_str(), "guide_button") == 0) {
                    if (strcasecmp(val.c_str(), "share") == 0) {
                        config.guide_index = 1;
                    } else if (strcasecmp(val.c_str(), "none") == 0 || strcasecmp(val.c_str(), "disabled") == 0) {
                        config.guide_index = 2;
                    } else {
                        config.guide_index = 0;
                    }
                } else if (strcasecmp(key.c_str(), "stick_deadzone") == 0 || strcasecmp(key.c_str(), "deadzone") == 0) {
                    int dz = atoi(val.c_str());
                    int best_idx = 4; // default 12%
                    int min_diff = 999;
                    for (size_t i = 0; i < DEADZONE_VALUES.size(); i++) {
                        int diff = abs(DEADZONE_VALUES[i] - dz);
                        if (diff < min_diff) {
                            min_diff = diff;
                            best_idx = (int)i;
                        }
                    }
                    config.deadzone_index = best_idx;
                } else if (strcasecmp(key.c_str(), "swap_triggers_bumpers") == 0) {
                    int v = atoi(val.c_str());
                    config.swap_index = (v != 0) ? 1 : 0;
                } else if (strcasecmp(key.c_str(), "enable_rumble") == 0 || strcasecmp(key.c_str(), "rumble") == 0) {
                    int v = atoi(val.c_str());
                    config.rumble_index = (v != 0) ? 0 : 1; // 0=Enabled, 1=Disabled
                } else if (strcasecmp(key.c_str(), "enable_homebrew") == 0 || strcasecmp(key.c_str(), "homebrew") == 0) {
                    int v = atoi(val.c_str());
                    config.homebrew_index = (v != 0) ? 0 : 1; // 0=Enabled, 1=Disabled
                } else if (strcasecmp(key.c_str(), "enable_notifications") == 0) {
                    int v = atoi(val.c_str());
                    config.notif_index = (v != 0) ? 0 : 1; // 0=Enabled, 1=Disabled
                } else if (strcasecmp(key.c_str(), "enable_mode_combo") == 0) {
                    int v = atoi(val.c_str());
                    config.combo_index = (v != 0) ? 0 : 1; // 0=Enabled, 1=Disabled
                } else if (strcasecmp(key.c_str(), "trigger_threshold") == 0) {
                    int th = atoi(val.c_str());
                    int best_idx = 1; // default 30
                    int min_diff = 999;
                    for (size_t i = 0; i < TRIGGER_VALUES.size(); i++) {
                        int diff = abs(TRIGGER_VALUES[i] - th);
                        if (diff < min_diff) {
                            min_diff = diff;
                            best_idx = (int)i;
                        }
                    }
                    config.trigger_index = best_idx;
                } else if (strcasecmp(key.c_str(), "switch_positional_layout") == 0) {
                    int v = atoi(val.c_str());
                    config.switch_index = (v != 0) ? 0 : 1; // 0=Positional (1), 1=By Letter (0)
                }
            }
        }
        line = strtok(NULL, "\r\n");
    }

    return true;
}

bool ConfigManager::save() {
    mkdir("/data", 0777);
    mkdir("/data/GoldHEN", 0777);

    int fd = sceKernelOpen(INI_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return false;
    }

    char out[2048];
    const char* mode_str = (config.mode_index == 1) ? "merge" : ((config.mode_index == 2) ? "secondary" : "auto");
    const char* guide_str = (config.guide_index == 1) ? "share" : ((config.guide_index == 2) ? "none" : "touchpad");
    int deadzone_val = DEADZONE_VALUES[config.deadzone_index];
    int trigger_val = TRIGGER_VALUES[config.trigger_index];
    int swap_val = (config.swap_index != 0) ? 1 : 0;
    int rumble_val = (config.rumble_index == 0) ? 1 : 0;
    int notif_val = (config.notif_index == 0) ? 1 : 0;
    int combo_val = (config.combo_index == 0) ? 1 : 0;
    int homebrew_val = (config.homebrew_index == 0) ? 1 : 0;
    int switch_val = (config.switch_index == 0) ? 1 : 0;

    int len = snprintf(out, sizeof(out),
        "# Xbox Controller Plugin - Configuration File\n"
        "# Generated by Xbox Controller Plugin Configurator\n"
        "# Location on PS4: /data/GoldHEN/xbox_controller.ini\n"
        "\n"
        "[General]\n"
        "mode = %s\n"
        "guide_action = %s\n"
        "enable_notifications = %d\n"
        "enable_mode_combo = %d\n"
        "enable_rumble = %d\n"
        "enable_homebrew = %d\n"
        "\n"
        "[Deadzone]\n"
        "stick_deadzone = %d\n"
        "trigger_threshold = %d\n"
        "\n"
        "[Remap]\n"
        "swap_triggers_bumpers = %d\n"
        "switch_positional_layout = %d\n",
        mode_str,
        guide_str,
        notif_val,
        combo_val,
        rumble_val,
        homebrew_val,
        deadzone_val,
        trigger_val,
        swap_val,
        switch_val
    );

    if (len > 0) {
        sceKernelWrite(fd, out, (size_t)len);
    }
    sceKernelClose(fd);
    return true;
}
