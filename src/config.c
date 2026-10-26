/*
 * Xbox Controller Plugin for PS4 (GoldHEN)
 * INI Configuration Loader
 */

#include "ini_parser.h"
#include "log.h"
#include <fcntl.h>
#include <string.h>
#include <orbis/libkernel.h>

// Global config instance
PluginConfig g_config;

// Initialize defaults replicating 100% of v1.4.0 behavior
void config_init_defaults(PluginConfig *cfg) {
    if (!cfg) return;

    // [General]
    cfg->mode = CONFIG_MODE_AUTO;
    cfg->guide_action = CONFIG_GUIDE_TOUCHPAD;
    cfg->enable_notifications = 1;
    cfg->enable_mode_combo = 1;
    cfg->enable_rumble = 1;
    cfg->enable_homebrew = 1;

    // [Deadzone]
    cfg->stick_deadzone = DEFAULT_STICK_DEADZONE;       // 15
    cfg->trigger_threshold = DEFAULT_TRIGGER_THRESHOLD; // 30

    // [Remap]
    cfg->swap_triggers_bumpers = 0;
    cfg->switch_positional = 1;
}

// Helpers for string trimming & parsing
static char* trim_whitespace(char *str) {
    if (!str) return NULL;
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') {
        str++;
    }
    if (*str == 0) return str;

    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    return str;
}

static int parse_int(const char *str, int default_val) {
    if (!str || *str == '\0') return default_val;
    int sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    int val = 0;
    int has_digits = 0;
    while (*str >= '0' && *str <= '9') {
        has_digits = 1;
        val = val * 10 + (*str - '0');
        str++;
    }
    return has_digits ? (val * sign) : default_val;
}

static int str_case_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a + ('a' - 'A')) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b + ('a' - 'A')) : *b;
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

void config_load(PluginConfig *cfg) {
    if (!cfg) return;

    // Always start with exact defaults
    config_init_defaults(cfg);

    int fd = sceKernelOpen(XBOX_CONFIG_PATH, O_RDONLY, 0);
    if (fd < 0) {
        // Optional file is missing: silently use defaults
        return;
    }

    static char buf[XBOX_CONFIG_BUFFER_SIZE];
    memset(buf, 0, sizeof(buf));
    ssize_t bytes_read = (ssize_t)sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);

    if (bytes_read <= 0) {
        return;
    }
    buf[bytes_read] = '\0';

    char current_section[32] = "general";
    char *cursor = buf;

    while (*cursor != '\0') {
        // Read next line
        char *line_start = cursor;
        while (*cursor != '\0' && *cursor != '\n') {
            cursor++;
        }
        if (*cursor == '\n') {
            *cursor = '\0';
            cursor++;
        }

        // Trim comment & whitespace
        char *line = trim_whitespace(line_start);
        if (*line == '\0' || *line == '#' || *line == ';') {
            continue;
        }

        // Section header [section]
        if (*line == '[') {
            char *end_sec = strchr(line, ']');
            if (end_sec) {
                *end_sec = '\0';
                char *sec_name = trim_whitespace(line + 1);
                size_t len = strlen(sec_name);
                if (len < sizeof(current_section)) {
                    strcpy(current_section, sec_name);
                }
            }
            continue;
        }

        // Key = Value
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim_whitespace(line);
        char *val = trim_whitespace(eq + 1);

        // Strip inline comments if present in val
        char *comment_char = strchr(val, '#');
        if (!comment_char) comment_char = strchr(val, ';');
        if (comment_char) {
            *comment_char = '\0';
            val = trim_whitespace(val);
        }

        if (*key == '\0' || *val == '\0') continue;

        // Parse keys
        if (str_case_eq(key, "mode")) {
            if (str_case_eq(val, "merge") || str_case_eq(val, "p1") || str_case_eq(val, "1")) {
                cfg->mode = CONFIG_MODE_MERGE;
            } else if (str_case_eq(val, "secondary") || str_case_eq(val, "p2") || str_case_eq(val, "2")) {
                cfg->mode = CONFIG_MODE_SECONDARY;
            } else {
                cfg->mode = CONFIG_MODE_AUTO;
            }
        } else if (str_case_eq(key, "guide_action") || str_case_eq(key, "guide_button")) {
            if (str_case_eq(val, "share") || str_case_eq(val, "options")) {
                cfg->guide_action = CONFIG_GUIDE_SHARE;
            } else if (str_case_eq(val, "none") || str_case_eq(val, "disabled") || str_case_eq(val, "0")) {
                cfg->guide_action = CONFIG_GUIDE_NONE;
            } else {
                cfg->guide_action = CONFIG_GUIDE_TOUCHPAD;
            }
        } else if (str_case_eq(key, "enable_notifications") || str_case_eq(key, "notifications")) {
            cfg->enable_notifications = (parse_int(val, 1) != 0);
        } else if (str_case_eq(key, "enable_mode_combo") || str_case_eq(key, "mode_combo")) {
            cfg->enable_mode_combo = (parse_int(val, 1) != 0);
        } else if (str_case_eq(key, "enable_rumble") || str_case_eq(key, "rumble")) {
            cfg->enable_rumble = (parse_int(val, 1) != 0);
        } else if (str_case_eq(key, "enable_homebrew") || str_case_eq(key, "homebrew")) {
            cfg->enable_homebrew = (parse_int(val, 1) != 0);
        } else if (str_case_eq(key, "stick_deadzone") || str_case_eq(key, "deadzone")) {
            int dz = parse_int(val, DEFAULT_STICK_DEADZONE);
            if (dz < 0) dz = 0;
            if (dz > 127) dz = 127;
            cfg->stick_deadzone = (uint8_t)dz;
        } else if (str_case_eq(key, "trigger_threshold")) {
            int th = parse_int(val, DEFAULT_TRIGGER_THRESHOLD);
            if (th < 0) th = 0;
            if (th > 255) th = 255;
            cfg->trigger_threshold = (uint8_t)th;
        } else if (str_case_eq(key, "swap_triggers_bumpers")) {
            cfg->swap_triggers_bumpers = (parse_int(val, 0) != 0);
        } else if (str_case_eq(key, "switch_positional_layout") || str_case_eq(key, "switch_positional")) {
            cfg->switch_positional = (parse_int(val, 1) != 0);
        }
    }

    log_event(LOG_LEVEL_INFO, "Loaded config: mode=%d guide=%d notif=%d combo=%d rumble=%d homebrew=%d dz=%d swap=%d",
              cfg->mode, cfg->guide_action, cfg->enable_notifications,
              cfg->enable_mode_combo, cfg->enable_rumble, cfg->enable_homebrew,
              cfg->stick_deadzone, cfg->swap_triggers_bumpers);
}
