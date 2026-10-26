/*
 * Xbox Controller Plugin for PS4 (GoldHEN)
 * Configuration and constants
 */

#ifndef CONFIG_H
#define CONFIG_H

// Plugin version
#define PLUGIN_VERSION_MAJOR 1
#define PLUGIN_VERSION_MINOR 4
#define PLUGIN_VERSION_PATCH 2

// Xbox 360 Controller USB identifiers
#define XBOX360_VID             0x045E  // Microsoft Corporation
#define XBOX360_PID_WIRED       0x028E  // Xbox 360 Controller
#define XBOX360_PID_WIRELESS    0x0719  // Xbox 360 Wireless Receiver

// USB endpoints
#define XBOX360_ENDPOINT_IN     0x81    // Input endpoint (controller -> host)
#define XBOX360_ENDPOINT_OUT    0x01    // Output endpoint (host -> controller)

// Report sizes
#define XBOX360_REPORT_SIZE     20      // Input report size in bytes

// Controller limits
#define MAX_XBOX_CONTROLLERS    4       // Maximum simultaneous controllers

// Virtual handle management
#define VIRTUAL_HANDLE_BASE     1000    // Virtual handles start at 1000
#define VIRTUAL_USER_BASE       0x20000000  // Virtual user ID base

// Timing
#define USB_POLL_INTERVAL_US    4000    // 4ms = 250Hz polling rate
#define USB_TRANSFER_TIMEOUT_MS 16      // USB transfer timeout

// Deadzone defaults (0-127 range, applied to 0-128 half-axis)
#define DEFAULT_STICK_DEADZONE  15      // ~12% deadzone
#define DEFAULT_TRIGGER_THRESHOLD 30    // Digital trigger activation point

// Debug
#ifndef DEBUG_NOTIFICATIONS
#define DEBUG_NOTIFICATIONS     0       // Set to 1 for verbose notifications
#endif

// Helper macros
#define IS_VIRTUAL_HANDLE(h)    ((h) >= VIRTUAL_HANDLE_BASE && (h) < VIRTUAL_HANDLE_BASE + MAX_XBOX_CONTROLLERS)
#define HANDLE_TO_INDEX(h)      ((h) - VIRTUAL_HANDLE_BASE)
#define INDEX_TO_HANDLE(i)      ((i) + VIRTUAL_HANDLE_BASE)

#define IS_VIRTUAL_USER(u)      ((u) >= VIRTUAL_USER_BASE && (u) < VIRTUAL_USER_BASE + MAX_XBOX_CONTROLLERS)
#define USER_TO_INDEX(u)        ((u) - VIRTUAL_USER_BASE)
#define INDEX_TO_USER(i)        ((i) + VIRTUAL_USER_BASE)

#endif // CONFIG_H
