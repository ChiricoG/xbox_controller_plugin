#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>

#include "common/graphics.h"
#include "common/log.h"
#include "config_manager.h"
#include "ui.h"

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);

    // Initialize 2D Graphics (1920x1080 @ 60 Hz)
    Scene2D* scene = new Scene2D(1920, 1080, 4);
    if (!scene->Init(0xC000000, 2)) {
        return -1;
    }

    // Initialize Config Manager
    // NOTE: If /data/GoldHEN/xbox_controller.ini does not exist,
    // configMgr.load() automatically creates it with all default settings!
    ConfigManager configMgr;
    configMgr.load();

    // Initialize UI
    UI* ui = new UI();
    if (!ui->init(scene)) {
        return -1;
    }

    // Initialize Controller via OpenOrbis scePad
    scePadInit();
    int userId = -1;
    OrbisUserServiceInitializeParams uParam;
    uParam.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&uParam);
    sceUserServiceGetInitialUser(&userId);

    int padHandle = scePadOpen(userId, 0, 0, NULL);

    int activeTab = 0;
    int selectedCard = 0;
    int toastFrames = 0;
    int frameID = 0;
    uint32_t prevButtons = 0;

    // Main Interaction Loop
    for (;;) {
        OrbisPadData padData;
        memset(&padData, 0, sizeof(padData));
        if (padHandle >= 0) {
            scePadReadState(padHandle, &padData);
        }

        uint32_t curButtons = padData.buttons;
        uint32_t justPressed = curButtons & ~prevButtons;
        prevButtons = curButtons;

        AppConfig& cfg = configMgr.getConfig();

        // Exit on OPTIONS or CIRCLE
        if ((justPressed & ORBIS_PAD_BUTTON_OPTIONS) || (justPressed & ORBIS_PAD_BUTTON_CIRCLE)) {
            break;
        }

        // Tab Switching via L1 / R1
        if (justPressed & ORBIS_PAD_BUTTON_L1) {
            activeTab = 0;
        }
        if (justPressed & ORBIS_PAD_BUTTON_R1) {
            activeTab = 1;
        }

        if (activeTab == 0) {
            const int TOTAL_CARDS = 10;

            // Navigate Cards in Settings
            if (justPressed & ORBIS_PAD_BUTTON_UP) {
                selectedCard = (selectedCard + TOTAL_CARDS - 1) % TOTAL_CARDS;
            }
            if (justPressed & ORBIS_PAD_BUTTON_DOWN) {
                selectedCard = (selectedCard + 1) % TOTAL_CARDS;
            }

            // Value changes (LEFT)
            if (justPressed & ORBIS_PAD_BUTTON_LEFT) {
                if (selectedCard == 0) {
                    cfg.mode_index = (cfg.mode_index + 2) % 3;
                } else if (selectedCard == 1) {
                    cfg.guide_index = (cfg.guide_index + 2) % 3;
                } else if (selectedCard == 2) {
                    if (cfg.deadzone_index > 0) cfg.deadzone_index--;
                } else if (selectedCard == 3) {
                    if (cfg.trigger_index > 0) cfg.trigger_index--;
                } else if (selectedCard == 4) {
                    cfg.swap_index ^= 1;
                } else if (selectedCard == 5) {
                    cfg.rumble_index ^= 1;
                } else if (selectedCard == 6) {
                    cfg.notif_index ^= 1;
                } else if (selectedCard == 7) {
                    cfg.combo_index ^= 1;
                } else if (selectedCard == 8) {
                    cfg.homebrew_index ^= 1;
                } else if (selectedCard == 9) {
                    cfg.switch_index ^= 1;
                }
            }

            // Value changes (RIGHT)
            if (justPressed & ORBIS_PAD_BUTTON_RIGHT) {
                if (selectedCard == 0) {
                    cfg.mode_index = (cfg.mode_index + 1) % 3;
                } else if (selectedCard == 1) {
                    cfg.guide_index = (cfg.guide_index + 1) % 3;
                } else if (selectedCard == 2) {
                    if (cfg.deadzone_index < (int)ConfigManager::DEADZONE_OPTIONS.size() - 1) {
                        cfg.deadzone_index++;
                    }
                } else if (selectedCard == 3) {
                    if (cfg.trigger_index < (int)ConfigManager::TRIGGER_OPTIONS.size() - 1) {
                        cfg.trigger_index++;
                    }
                } else if (selectedCard == 4) {
                    cfg.swap_index ^= 1;
                } else if (selectedCard == 5) {
                    cfg.rumble_index ^= 1;
                } else if (selectedCard == 6) {
                    cfg.notif_index ^= 1;
                } else if (selectedCard == 7) {
                    cfg.combo_index ^= 1;
                } else if (selectedCard == 8) {
                    cfg.homebrew_index ^= 1;
                } else if (selectedCard == 9) {
                    cfg.switch_index ^= 1;
                }
            }

            // Save on CROSS (X)
            if (justPressed & ORBIS_PAD_BUTTON_CROSS) {
                if (configMgr.save()) {
                    toastFrames = 180; // ~3 seconds toast feedback
                }
            }
        } else {
            // In Info tab: CROSS or LEFT returns to Settings
            if ((justPressed & ORBIS_PAD_BUTTON_CROSS) || (justPressed & ORBIS_PAD_BUTTON_LEFT)) {
                activeTab = 0;
            }
        }

        if (toastFrames > 0) {
            toastFrames--;
        }

        // Render UI
        ui->render(scene, cfg, activeTab, selectedCard, toastFrames);

        // Flip & VSync
        scene->SubmitFlip(frameID);
        scene->FrameWait(frameID);
        scene->FrameBufferSwap();
        frameID++;
    }

    if (padHandle >= 0) {
        scePadClose(padHandle);
    }

    // Gracefully exit back to PS4 shell without CE-34878-0
    sceSystemServiceLoadExec("exit", NULL);
    exit(0);

    return 0;
}
