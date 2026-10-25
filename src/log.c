/*
 * Logging and Notification Subsystem for xbox_controller
 *
 * Implements D-009:
 * - Two channels: on-screen notifications (rate-limited) and disk log file.
 * - Lock-protected ring buffer so hooks never perform file I/O or notification syscalls.
 * - Direct kernel file I/O (sceKernelOpen/Write/Close) to avoid libc FILE* issues in PRX.
 * - Log rotation at 64 KB with .old backup.
 */

#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <orbis/libkernel.h>
#include <GoldHEN.h>

#define LOG_BUFFER_SIZE 128
#define MAX_LOG_MSG_LEN 256
#define MAX_LOG_FILE_SIZE (64 * 1024)
#define NOTIFY_GLOBAL_MIN_INTERVAL_US  500000ULL // 0.5 s between notifications
#define NOTIFY_SAME_MIN_INTERVAL_US   3000000ULL // 3.0 s duplicate suppression
#define NOTIFY_HISTORY_COUNT 8

typedef struct {
    uint64_t timestamp_us;
    LogLevel level;
    char     message[MAX_LOG_MSG_LEN];
    int      notify;
} LogEntry;

static LogEntry     g_entries[LOG_BUFFER_SIZE];
static int          g_head = 0;
static int          g_tail = 0;
static int          g_count = 0;
static volatile int g_log_lock = 0;
static volatile int g_log_initialized = 0;

static int32_t      g_log_fd = -1;
static char         g_active_log_path[128] = {0};
static char         g_active_old_path[136] = {0};
static long         g_current_file_size = 0;

typedef struct {
    char     message[64];
    uint64_t time_us;
} NotifyHistory;

static NotifyHistory g_notify_history[NOTIFY_HISTORY_COUNT];
static int           g_notify_history_idx = 0;
static uint64_t      g_last_notify_time_us = 0;

static inline void log_spin_lock(void) {
    while (__atomic_exchange_n(&g_log_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        __builtin_ia32_pause();
    }
}

static inline void log_spin_unlock(void) {
    __atomic_store_n(&g_log_lock, 0, __ATOMIC_RELEASE);
}

static const char* level_to_str(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default:              return "UNKNOWN";
    }
}

static void send_os_notification(const char* message) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.targetId = -1;
    strncpy(req.message, message, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void try_send_notification(const char* message, uint64_t now_us) {
    if (g_last_notify_time_us != 0 &&
        (now_us - g_last_notify_time_us < NOTIFY_GLOBAL_MIN_INTERVAL_US)) {
        return; // Global rate limit
    }

    // Check same-message rate limit
    for (int i = 0; i < NOTIFY_HISTORY_COUNT; i++) {
        if (g_notify_history[i].message[0] != '\0' &&
            strncmp(g_notify_history[i].message, message, sizeof(g_notify_history[i].message) - 1) == 0) {
            if (now_us - g_notify_history[i].time_us < NOTIFY_SAME_MIN_INTERVAL_US) {
                return; // Suppress duplicate notification
            }
        }
    }

    send_os_notification(message);
    g_last_notify_time_us = now_us;

    strncpy(g_notify_history[g_notify_history_idx].message, message,
            sizeof(g_notify_history[g_notify_history_idx].message) - 1);
    g_notify_history[g_notify_history_idx].message[sizeof(g_notify_history[g_notify_history_idx].message) - 1] = '\0';
    g_notify_history[g_notify_history_idx].time_us = now_us;
    g_notify_history_idx = (g_notify_history_idx + 1) % NOTIFY_HISTORY_COUNT;
}

static void check_rotation(void) {
    if (g_log_fd < 0 || g_active_log_path[0] == '\0') {
        return;
    }

    if (g_current_file_size >= MAX_LOG_FILE_SIZE) {
        sceKernelClose(g_log_fd);
        g_log_fd = -1;

        sceKernelUnlink(g_active_old_path);
        sceKernelRename(g_active_log_path, g_active_old_path);

        g_log_fd = sceKernelOpen(g_active_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (g_log_fd < 0) {
            struct jailbreak_backup jb;
            memset(&jb, 0, sizeof(jb));
            if (sys_sdk_jailbreak(&jb) == 0) {
                sceKernelUnlink(g_active_old_path);
                sceKernelRename(g_active_log_path, g_active_old_path);
                g_log_fd = sceKernelOpen(g_active_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                sys_sdk_unjailbreak(&jb);
            }
        }
        if (g_log_fd >= 0) {
            const char* hdr = "=== Log rotated (previous saved to .old) ===\n";
            size_t len = strlen(hdr);
            sceKernelWrite(g_log_fd, hdr, len);
            g_current_file_size = (long)len;
        }
    }
}

static int open_candidate_log_file(void) {
    static const char* const log_primary = "/data/GoldHEN/xbox_controller.log";
    static const char* const old_primary = "/data/GoldHEN/xbox_controller.log.old";

    // 1. Direct open of primary log path
    int32_t fd = sceKernelOpen(log_primary, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        strncpy(g_active_log_path, log_primary, sizeof(g_active_log_path) - 1);
        strncpy(g_active_old_path, old_primary, sizeof(g_active_old_path) - 1);
        off_t pos = sceKernelLseek(fd, 0, SEEK_END);
        g_current_file_size = (pos >= 0) ? (long)pos : 0;
        g_log_fd = fd;
        return 0;
    }

    // 2. Sandboxed process fallback: escape sandbox to write to /data/GoldHEN
    struct jailbreak_backup jb;
    memset(&jb, 0, sizeof(jb));
    if (sys_sdk_jailbreak(&jb) == 0) {
        fd = sceKernelOpen(log_primary, O_WRONLY | O_CREAT | O_APPEND, 0666);
        sys_sdk_unjailbreak(&jb);
        if (fd >= 0) {
            strncpy(g_active_log_path, log_primary, sizeof(g_active_log_path) - 1);
            strncpy(g_active_old_path, old_primary, sizeof(g_active_old_path) - 1);
            off_t pos = sceKernelLseek(fd, 0, SEEK_END);
            g_current_file_size = (pos >= 0) ? (long)pos : 0;
            g_log_fd = fd;
            return 0;
        }
    }

    // 3. Fallbacks if /data/GoldHEN is unavailable
    static const char* const fallbacks[] = {
        "/data/xbox_controller.log",
        "/user/data/xbox_controller.log"
    };
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        fd = sceKernelOpen(fallbacks[i], O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0) {
            strncpy(g_active_log_path, fallbacks[i], sizeof(g_active_log_path) - 1);
            snprintf(g_active_old_path, sizeof(g_active_old_path), "%s.old", fallbacks[i]);
            off_t pos = sceKernelLseek(fd, 0, SEEK_END);
            g_current_file_size = (pos >= 0) ? (long)pos : 0;
            g_log_fd = fd;
            return 0;
        }
    }

    return -1;
}

void log_flush(void) {
    if (!g_log_initialized) {
        return;
    }

    LogEntry entry;
    int has_entry = 0;

    while (1) {
        has_entry = 0;
        log_spin_lock();
        if (g_count > 0) {
            entry = g_entries[g_head];
            g_head = (g_head + 1) % LOG_BUFFER_SIZE;
            g_count--;
            has_entry = 1;
        }
        log_spin_unlock();

        if (!has_entry) {
            break;
        }

        uint64_t sec = entry.timestamp_us / 1000000ULL;
        uint32_t ms = (uint32_t)((entry.timestamp_us % 1000000ULL) / 1000ULL);

        if (g_log_fd >= 0) {
            check_rotation();
            if (g_log_fd >= 0) {
                char line[MAX_LOG_MSG_LEN + 64];
                int len = snprintf(line, sizeof(line), "[%5llu.%03u] [%-5s] %s\n",
                                   (unsigned long long)sec, ms,
                                   level_to_str(entry.level), entry.message);
                if (len > 0) {
                    if ((size_t)len >= sizeof(line)) {
                        len = (int)(sizeof(line) - 1);
                    }
                    sceKernelWrite(g_log_fd, line, (size_t)len);
                    g_current_file_size += len;
                }
            }
        }

        if (entry.notify) {
            try_send_notification(entry.message, entry.timestamp_us);
        }
    }
}

void log_init(void) {
    if (g_log_initialized) {
        return;
    }

    g_log_lock = 0;
    g_head = 0;
    g_tail = 0;
    g_count = 0;
    g_last_notify_time_us = 0;
    memset(g_notify_history, 0, sizeof(g_notify_history));

    int res = open_candidate_log_file();
    g_log_initialized = 1;

    if (res == 0) {
        log_event(LOG_LEVEL_INFO, "=== Xbox Controller Plugin Started (Log: %s) ===", g_active_log_path);
    } else {
        log_event(LOG_LEVEL_WARN, "=== Xbox Controller Plugin Started (File log disabled: unwritable paths) ===");
    }
    log_flush();
}

void log_shutdown(void) {
    if (!g_log_initialized) {
        return;
    }

    log_event(LOG_LEVEL_INFO, "=== Xbox Controller Plugin Unloading ===");
    log_flush();

    if (g_log_fd >= 0) {
        sceKernelClose(g_log_fd);
        g_log_fd = -1;
    }

    g_log_initialized = 0;
}

void log_event(LogLevel level, const char* fmt, ...) {
    if (!g_log_initialized) {
        return;
    }

    LogEntry entry;
    entry.timestamp_us = sceKernelGetProcessTime();
    entry.level = level;
    entry.notify = 0;

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry.message, sizeof(entry.message), fmt, args);
    va_end(args);

    log_spin_lock();
    if (g_count >= LOG_BUFFER_SIZE) {
        g_head = (g_head + 1) % LOG_BUFFER_SIZE;
        g_count--;
    }
    g_entries[g_tail] = entry;
    g_tail = (g_tail + 1) % LOG_BUFFER_SIZE;
    g_count++;
    log_spin_unlock();
}

void log_notify(const char* fmt, ...) {
    if (!g_log_initialized) {
        return;
    }

    LogEntry entry;
    entry.timestamp_us = sceKernelGetProcessTime();
    entry.level = LOG_LEVEL_INFO;
    entry.notify = 1;

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry.message, sizeof(entry.message), fmt, args);
    va_end(args);

    log_spin_lock();
    if (g_count >= LOG_BUFFER_SIZE) {
        g_head = (g_head + 1) % LOG_BUFFER_SIZE;
        g_count--;
    }
    g_entries[g_tail] = entry;
    g_tail = (g_tail + 1) % LOG_BUFFER_SIZE;
    g_count++;
    log_spin_unlock();
}
