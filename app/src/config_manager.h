#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#define INI_FILE_PATH "/data/GoldHEN/xbox_controller.ini"

struct AppConfig {
    int mode_index;         // 0 = Auto, 1 = Merge (P1), 2 = Secondary (P2)
    int guide_index;        // 0 = Touchpad, 1 = Share, 2 = Disabled
    int deadzone_index;     // 0..7 (0%, 5%, 8%, 10%, 12%, 15%, 20%, 25%)
    int trigger_index;      // 0..4 (15, 30, 45, 60, 80)
    int swap_index;         // 0 = Disabled, 1 = Enabled
    int rumble_index;       // 0 = Enabled, 1 = Disabled
    int notif_index;        // 0 = Enabled, 1 = Disabled
    int combo_index;        // 0 = Enabled, 1 = Disabled
    int homebrew_index;     // 0 = Enabled, 1 = Disabled
    int switch_index;       // 0 = Positional, 1 = By Letter

    AppConfig() {
        set_defaults();
    }

    void set_defaults() {
        mode_index = 0;         // Auto
        guide_index = 0;        // Touchpad
        deadzone_index = 4;     // 12% (15/127)
        trigger_index = 1;      // 30 (12%)
        swap_index = 0;         // Disabled
        rumble_index = 0;       // Enabled
        notif_index = 0;        // Enabled
        combo_index = 0;        // Enabled
        homebrew_index = 0;     // Enabled
        switch_index = 0;       // Positional
    }
};

class ConfigManager {
public:
    static const std::vector<std::string> MODE_OPTIONS;
    static const std::vector<std::string> GUIDE_OPTIONS;
    static const std::vector<std::string> DEADZONE_OPTIONS;
    static const std::vector<int>         DEADZONE_VALUES;
    static const std::vector<std::string> TRIGGER_OPTIONS;
    static const std::vector<int>         TRIGGER_VALUES;
    static const std::vector<std::string> SWAP_OPTIONS;
    static const std::vector<std::string> RUMBLE_OPTIONS;
    static const std::vector<std::string> NOTIF_OPTIONS;
    static const std::vector<std::string> COMBO_OPTIONS;
    static const std::vector<std::string> HOMEBREW_OPTIONS;
    static const std::vector<std::string> SWITCH_OPTIONS;

    ConfigManager();
    bool load();
    bool save();

    AppConfig& getConfig() { return config; }

private:
    AppConfig config;
    bool file_exists(const char* path);
};
