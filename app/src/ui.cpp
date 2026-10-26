#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#define BG_IMAGE_PATH   "/app0/assets/images/bg_native.png"
#define FONT_PATH       "/app0/assets/fonts/font.ttf"

// Layout Constants
static const int SIDEBAR_X = 60;
static const int SIDEBAR_W = 310;
static const int CARD_X = 410;
static const int CARD_W = 1140;
static const int CARD_H = 92;
static const int CARD_Y_START = 280;
static const int CARD_Y_STEP = 104;

// Colors
static const Color COLOR_GREEN_GLOW  = { 8, 85, 25 };
static const Color COLOR_GREEN_MAIN  = { 16, 208, 58 };
static const Color COLOR_GREEN_BRIGHT= { 40, 245, 80 };
static const Color COLOR_WHITE       = { 255, 255, 255 };
static const Color COLOR_TEXT_DIM    = { 135, 148, 158 };
static const Color COLOR_CARD_BG     = { 12, 22, 29 };
static const Color COLOR_CARD_BORDER = { 24, 40, 50 };
static const Color COLOR_DIVIDER     = { 20, 34, 44 };
static const Color COLOR_TOAST_BG    = { 12, 32, 20 };
static const Color COLOR_TOAST_BORDER= { 16, 180, 50 };

UI::UI() : fontTitle(NULL), fontSubtitle(NULL), fontCardTitle(NULL),
           fontCardDesc(NULL), fontValue(NULL), fontFooter(NULL),
           cached_bg(NULL), bg_width(0), bg_height(0) {}

UI::~UI() {
    if (cached_bg) {
        free(cached_bg);
        cached_bg = NULL;
    }
    free_icon(xboxLogo);
    free_icon(iconGear);
    free_icon(iconInfo);
    free_icon(iconSave);
    free_icon(iconMode);
    free_icon(iconGuide);
    free_icon(iconDeadzone);
    free_icon(iconSwap);
    free_icon(iconRumble);
    free_icon(btnArrows);
    free_icon(btnCross);
    free_icon(btnOptions);
    free_icon(btnL1);
    free_icon(btnR1);
}

ImageRGBA UI::load_icon(const char* path) {
    ImageRGBA img;
    int channels = 0;
    unsigned char* data = stbi_load(path, &img.width, &img.height, &channels, 4);
    if (data) {
        int total = img.width * img.height;
        img.pixels = (uint32_t*)malloc(total * sizeof(uint32_t));
        if (img.pixels) {
            uint32_t* src32 = (uint32_t*)data;
            for (int i = 0; i < total; i++) {
                uint32_t p = src32[i];
                uint8_t r = (uint8_t)(p >> 0);
                uint8_t g = (uint8_t)(p >> 8);
                uint8_t b = (uint8_t)(p >> 16);
                uint8_t a = (uint8_t)(p >> 24);
                img.pixels[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
        stbi_image_free(data);
    }
    return img;
}

void UI::free_icon(ImageRGBA& img) {
    if (img.pixels) {
        free(img.pixels);
        img.pixels = NULL;
    }
}

void UI::draw_image(Scene2D* scene, const ImageRGBA& img, int startX, int startY) {
    if (!scene || !img.pixels) return;
    for (int y = 0; y < img.height; y++) {
        int py = startY + y;
        if (py < 0 || py >= 1080) continue;
        for (int x = 0; x < img.width; x++) {
            int px = startX + x;
            if (px < 0 || px >= 1920) continue;
            uint32_t p = img.pixels[y * img.width + x];
            uint8_t a = (uint8_t)(p >> 24);
            if (a == 0) continue;
            uint8_t r = (uint8_t)(p >> 16);
            uint8_t g = (uint8_t)(p >> 8);
            uint8_t b = (uint8_t)(p >> 0);
            Color c = { r, g, b };
            scene->DrawPixelBlend(px, py, c, a);
        }
    }
}

bool UI::init(Scene2D* scene) {
    if (!scene) return false;

    // 1. Load clean native background (deep slate-black wallpaper with ambient green glow & controller silhouette)
    int channels = 0;
    unsigned char* data = stbi_load(BG_IMAGE_PATH, &bg_width, &bg_height, &channels, 4);
    if (data && bg_width == 1920 && bg_height == 1080) {
        cached_bg = (uint32_t*)malloc(1920 * 1080 * 4);
        if (cached_bg) {
            uint32_t* src32 = (uint32_t*)data;
            for (int i = 0; i < 1920 * 1080; i++) {
                uint32_t pixel = src32[i];
                uint8_t r = (uint8_t)(pixel >> 0);
                uint8_t g = (uint8_t)(pixel >> 8);
                uint8_t b = (uint8_t)(pixel >> 16);
                cached_bg[i] = 0x80000000 | (r << 16) | (g << 8) | b;
            }
        }
        stbi_image_free(data);
    }

    // 2. Load separate clean vector-crisp icons
    xboxLogo      = load_icon("/app0/assets/icons/xbox_logo.png");
    iconGear      = load_icon("/app0/assets/icons/icon_gear.png");
    iconInfo      = load_icon("/app0/assets/icons/icon_info.png");
    iconSave      = load_icon("/app0/assets/icons/icon_save.png");
    iconMode      = load_icon("/app0/assets/icons/card_icon_mode.png");
    iconGuide     = load_icon("/app0/assets/icons/card_icon_guide.png");
    iconDeadzone  = load_icon("/app0/assets/icons/card_icon_deadzone.png");
    iconSwap      = load_icon("/app0/assets/icons/card_icon_swap.png");
    iconRumble    = load_icon("/app0/assets/icons/card_icon_rumble.png");
    btnArrows     = load_icon("/app0/assets/icons/btn_arrows.png");
    btnCross      = load_icon("/app0/assets/icons/btn_cross_blue.png");
    btnOptions    = load_icon("/app0/assets/icons/btn_options.png");
    btnL1         = load_icon("/app0/assets/icons/btn_l1.png");
    btnR1         = load_icon("/app0/assets/icons/btn_r1.png");

    // 3. Initialize FreeType fonts with legible TV sizes
    scene->InitFont(&fontTitle,     FONT_PATH, 38);
    scene->InitFont(&fontSubtitle,  FONT_PATH, 20);
    scene->InitFont(&fontCardTitle, FONT_PATH, 24);
    scene->InitFont(&fontCardDesc,  FONT_PATH, 18);
    scene->InitFont(&fontValue,     FONT_PATH, 22);
    scene->InitFont(&fontFooter,    FONT_PATH, 18);

    return true;
}

void UI::draw_rounded_rect(Scene2D* scene, int x, int y, int w, int h, int r, Color color) {
    if (!scene) return;
    for (int row = 0; row < h; row++) {
        int dx = r;
        if (row < r) {
            int dy = r - 1 - row;
            dx = (int)round(sqrt((double)(r * r - dy * dy)));
        } else if (row >= h - r) {
            int dy = row - (h - r);
            dx = (int)round(sqrt((double)(r * r - dy * dy)));
        }
        int startX = x + (r - dx);
        int spanW = w - 2 * (r - dx);
        if (spanW > 0) {
            scene->DrawRectangle(startX, y + row, spanW, 1, color);
        }
    }
}

void UI::draw_rounded_outline(Scene2D* scene, int x, int y, int w, int h, int r, Color color, int thickness) {
    if (!scene) return;
    for (int t = 0; t < thickness; t++) {
        int curX = x + t;
        int curY = y + t;
        int curW = w - 2 * t;
        int curH = h - 2 * t;
        int curR = (r > t) ? (r - t) : 1;
        for (int row = 0; row < curH; row++) {
            int dx = curR;
            if (row < curR) {
                int dy = curR - 1 - row;
                dx = (int)round(sqrt((double)(curR * curR - dy * dy)));
            } else if (row >= curH - curR) {
                int dy = row - (curH - curR);
                dx = (int)round(sqrt((double)(curR * curR - dy * dy)));
            }
            int startX = curX + (curR - dx);
            int spanW = curW - 2 * (curR - dx);
            if (spanW > 0) {
                scene->DrawPixel(startX, curY + row, color);
                scene->DrawPixel(startX + spanW - 1, curY + row, color);
            }
        }
        // Ensure continuous top and bottom borders
        scene->DrawRectangle(curX + curR, curY, curW - 2 * curR, 1, color);
        scene->DrawRectangle(curX + curR, curY + curH - 1, curW - 2 * curR, 1, color);
    }
}

void UI::draw_text(Scene2D* scene, const std::string& text, FT_Face face, int startX, int baselineY, Color color) {
    if (!scene || !face) return;
    Color dummyBg = { 0, 0, 0 };
    scene->DrawText((char*)text.c_str(), face, startX, baselineY, dummyBg, color);
}

void UI::draw_text_centered(Scene2D* scene, const std::string& text, FT_Face face, int centerX, int baselineY, Color color) {
    if (!scene || !face) return;
    int totalWidth = 0;
    for (size_t i = 0; i < text.length(); i++) {
        FT_UInt glyph_idx = FT_Get_Char_Index(face, text[i]);
        if (FT_Load_Glyph(face, glyph_idx, FT_LOAD_DEFAULT) == 0) {
            totalWidth += (face->glyph->advance.x >> 6);
        }
    }
    int startX = centerX - (totalWidth / 2);
    Color dummyBg = { 0, 0, 0 };
    scene->DrawText((char*)text.c_str(), face, startX, baselineY, dummyBg, color);
}

void UI::render(Scene2D* scene, const AppConfig& cfg, int activeTab, int selectedCard, int toastFrames) {
    if (!scene) return;

    // 1. Blit clean native background (pure wallpaper)
    if (cached_bg) {
        scene->CopyToActiveBuffer(cached_bg);
    }

    // 2. Header
    draw_image(scene, xboxLogo, 60, 120);
    draw_text(scene, "Xbox Controller", fontTitle, 175, 145, COLOR_WHITE);
    draw_text(scene, "Plugin", fontTitle, 175, 188, COLOR_GREEN_MAIN);
    draw_text(scene, "Configurator", fontTitle, 295, 188, COLOR_WHITE);
    draw_text(scene, "Configure Xbox controller plugin settings for PS4", fontSubtitle, 175, 222, COLOR_TEXT_DIM);

    // 3. Left Sidebar
    Color sidebarGreen = { 14, 80, 28 };
    Color sidebarDark  = { 12, 22, 29 };
    Color borderDim    = { 24, 40, 50 };

    // Tab 0: Settings
    if (activeTab == 0) {
        draw_rounded_rect(scene, SIDEBAR_X, 280, SIDEBAR_W, 58, 10, sidebarGreen);
        draw_rounded_outline(scene, SIDEBAR_X, 280, SIDEBAR_W, 58, 10, COLOR_GREEN_MAIN, 2);
        draw_image(scene, iconGear, SIDEBAR_X + 16, 285);
        draw_text(scene, "Settings", fontCardTitle, SIDEBAR_X + 72, 317, COLOR_WHITE);
    } else {
        draw_rounded_rect(scene, SIDEBAR_X, 280, SIDEBAR_W, 58, 10, sidebarDark);
        draw_rounded_outline(scene, SIDEBAR_X, 280, SIDEBAR_W, 58, 10, borderDim, 1);
        draw_image(scene, iconGear, SIDEBAR_X + 16, 285);
        draw_text(scene, "Settings", fontCardTitle, SIDEBAR_X + 72, 317, COLOR_TEXT_DIM);
    }
    draw_image(scene, btnL1, SIDEBAR_X + 250, 295);

    // Tab 1: Info
    if (activeTab == 1) {
        draw_rounded_rect(scene, SIDEBAR_X, 355, SIDEBAR_W, 58, 10, sidebarGreen);
        draw_rounded_outline(scene, SIDEBAR_X, 355, SIDEBAR_W, 58, 10, COLOR_GREEN_MAIN, 2);
        draw_image(scene, iconInfo, SIDEBAR_X + 16, 360);
        draw_text(scene, "Info", fontCardTitle, SIDEBAR_X + 72, 392, COLOR_WHITE);
    } else {
        draw_rounded_rect(scene, SIDEBAR_X, 355, SIDEBAR_W, 58, 10, sidebarDark);
        draw_rounded_outline(scene, SIDEBAR_X, 355, SIDEBAR_W, 58, 10, borderDim, 1);
        draw_image(scene, iconInfo, SIDEBAR_X + 16, 360);
        draw_text(scene, "Info", fontCardTitle, SIDEBAR_X + 72, 392, COLOR_TEXT_DIM);
    }
    draw_image(scene, btnR1, SIDEBAR_X + 250, 370);

    // 4. Main Content Area
    if (activeTab == 0) {
        // --- SETTINGS VIEW (Scrollable 10 Cards Viewport) ---
        const int TOTAL_CARDS = 10;
        const int VISIBLE_CARDS = 5;

        int scrollOffset = 0;
        if (selectedCard >= VISIBLE_CARDS) {
            scrollOffset = selectedCard - VISIBLE_CARDS + 1;
        }
        if (scrollOffset > TOTAL_CARDS - VISIBLE_CARDS) {
            scrollOffset = TOTAL_CARDS - VISIBLE_CARDS;
        }

        const char* cardTitles[TOTAL_CARDS] = {
            "Operating Mode",
            "Guide Button",
            "Stick Deadzone",
            "Trigger Threshold",
            "Swap Bumpers & Triggers",
            "Vibration",
            "GoldHEN Notifications",
            "Mode Toggle Combo",
            "Homebrew Navigation",
            "Switch Controller Layout"
        };

        const char* cardDescs[TOTAL_CARDS] = {
            "Select plugin operating behavior and player assignment.",
            "Choose in-game action for the central Xbox Guide button.",
            "Adjust analog stick inner deadzone to counter stick drift.",
            "Digital press activation threshold for analog triggers.",
            "Invert bumper (LB/RB) and trigger (LT/RT) functions.",
            "Enable or disable controller rumble feedback.",
            "Show on-screen popups on controller connect or mode change.",
            "Switch mode at runtime by holding LB+RB+Back+Start for 1.5s.",
            "Enable controller support inside homebrew apps and menus.",
            "Positional (bottom=X) or by letter (A=Circle, B=Cross)."
        };

        ImageRGBA cardIcons[TOTAL_CARDS] = {
            iconMode, iconGuide, iconDeadzone, iconDeadzone, iconSwap,
            iconRumble, iconInfo, iconMode, iconGear, iconSwap
        };

        std::string vals[TOTAL_CARDS];
        vals[0] = ConfigManager::MODE_OPTIONS[cfg.mode_index];
        vals[1] = ConfigManager::GUIDE_OPTIONS[cfg.guide_index];
        vals[2] = ConfigManager::DEADZONE_OPTIONS[cfg.deadzone_index];
        vals[3] = ConfigManager::TRIGGER_OPTIONS[cfg.trigger_index];
        vals[4] = ConfigManager::SWAP_OPTIONS[cfg.swap_index];
        vals[5] = ConfigManager::RUMBLE_OPTIONS[cfg.rumble_index];
        vals[6] = ConfigManager::NOTIF_OPTIONS[cfg.notif_index];
        vals[7] = ConfigManager::COMBO_OPTIONS[cfg.combo_index];
        vals[8] = ConfigManager::HOMEBREW_OPTIONS[cfg.homebrew_index];
        vals[9] = ConfigManager::SWITCH_OPTIONS[cfg.switch_index];

        int selectorCenterX = 1380;

        for (int i = 0; i < VISIBLE_CARDS; i++) {
            int cardIndex = scrollOffset + i;
            int cardY = CARD_Y_START + (i * CARD_Y_STEP);
            bool isSelected = (cardIndex == selectedCard);

            // Procedural card background
            draw_rounded_rect(scene, CARD_X, cardY, CARD_W, CARD_H, 12, COLOR_CARD_BG);

            if (isSelected) {
                draw_rounded_outline(scene, CARD_X - 1, cardY - 1, CARD_W + 2, CARD_H + 2, 12, COLOR_GREEN_GLOW, 2);
                draw_rounded_outline(scene, CARD_X, cardY, CARD_W, CARD_H, 12, COLOR_GREEN_MAIN, 2);
            } else {
                draw_rounded_outline(scene, CARD_X, cardY, CARD_W, CARD_H, 12, COLOR_CARD_BORDER, 1);
            }

            // Draw card icon
            draw_image(scene, cardIcons[cardIndex], CARD_X + 20, cardY + 22);

            // Draw text
            draw_text(scene, cardTitles[cardIndex], fontCardTitle, CARD_X + 85, cardY + 38, COLOR_WHITE);
            draw_text(scene, cardDescs[cardIndex],  fontCardDesc,  CARD_X + 85, cardY + 68, COLOR_TEXT_DIM);

            // Draw value selector
            Color textColor  = isSelected ? COLOR_GREEN_BRIGHT : COLOR_WHITE;
            Color arrowColor = isSelected ? COLOR_WHITE : COLOR_TEXT_DIM;

            std::string fullStr = "[   " + vals[cardIndex] + "   ]";
            draw_text_centered(scene, fullStr, fontValue, selectorCenterX, cardY + 54, textColor);

            draw_text(scene, "<", fontValue, selectorCenterX - 130, cardY + 54, arrowColor);
            draw_text(scene, ">", fontValue, selectorCenterX + 120, cardY + 54, arrowColor);
        }

        // Vertical Scrollbar on the right of cards
        int scrollTrackX = CARD_X + CARD_W + 16;
        int scrollTrackY = CARD_Y_START;
        int scrollTrackH = (VISIBLE_CARDS * CARD_Y_STEP) - 12; // 508px
        int scrollTrackW = 6;
        draw_rounded_rect(scene, scrollTrackX, scrollTrackY, scrollTrackW, scrollTrackH, 3, COLOR_CARD_BORDER);

        int thumbH = (scrollTrackH * VISIBLE_CARDS) / TOTAL_CARDS; // ~254px
        int maxThumbTravel = scrollTrackH - thumbH;
        int maxScrollOffset = TOTAL_CARDS - VISIBLE_CARDS;
        int thumbY = scrollTrackY + (maxScrollOffset > 0 ? (scrollOffset * maxThumbTravel) / maxScrollOffset : 0);
        draw_rounded_rect(scene, scrollTrackX, thumbY, scrollTrackW, thumbH, 3, COLOR_GREEN_MAIN);

        // Position Indicator text below scrollbar: e.g. "7 / 10"
        char posStr[16];
        snprintf(posStr, sizeof(posStr), "%d / %d", selectedCard + 1, TOTAL_CARDS);
        draw_text_centered(scene, posStr, fontFooter, scrollTrackX + 3, scrollTrackY + scrollTrackH + 24, COLOR_TEXT_DIM);

        // 5. Footer for Settings
        scene->DrawRectangle(60, 865, 1800, 2, COLOR_DIVIDER);

        draw_image(scene, iconSave, 60, 890);
        draw_text(scene, "Save Settings", fontCardTitle, 120, 912, COLOR_WHITE);
        draw_text(scene, "Configuration will be written to /data/GoldHEN/xbox_controller.ini", fontFooter, 120, 938, COLOR_TEXT_DIM);

        // Navigation Hints
        draw_image(scene, btnArrows, 1370, 901);
        draw_text(scene, "Change", fontFooter, 1424, 922, COLOR_TEXT_DIM);

        draw_image(scene, btnCross, 1540, 897);
        draw_text(scene, "Save", fontFooter, 1585, 922, COLOR_GREEN_MAIN);

        draw_image(scene, btnOptions, 1690, 900);
        draw_text(scene, "Exit", fontFooter, 1770, 922, COLOR_TEXT_DIM);

    } else {
        // --- INFO VIEW ---
        // Info Card 1: Overview & Version
        draw_rounded_rect(scene, CARD_X, 280, CARD_W, 160, 12, COLOR_CARD_BG);
        draw_rounded_outline(scene, CARD_X, 280, CARD_W, 160, 12, COLOR_CARD_BORDER, 1);
        draw_image(scene, iconInfo, CARD_X + 24, 305);
        draw_text(scene, "Xbox Controller Plugin & Configurator", fontCardTitle, CARD_X + 90, 318, COLOR_WHITE);
        draw_text(scene, "Plugin Version: v1.4.1 (with INI Configuration Support)", fontCardDesc, CARD_X + 90, 350, COLOR_GREEN_BRIGHT);
        draw_text(scene, "Configuration File: /data/GoldHEN/xbox_controller.ini", fontCardDesc, CARD_X + 90, 380, COLOR_TEXT_DIM);
        draw_text(scene, "Developer: ChiricoG - github.com/ChiricoG/xbox_controller_plugin", fontCardDesc, CARD_X + 90, 410, COLOR_TEXT_DIM);

        // Info Card 2: Supported Controllers & Connectivity
        draw_rounded_rect(scene, CARD_X, 460, CARD_W, 160, 12, COLOR_CARD_BG);
        draw_rounded_outline(scene, CARD_X, 460, CARD_W, 160, 12, COLOR_CARD_BORDER, 1);
        draw_image(scene, iconMode, CARD_X + 24, 485);
        draw_text(scene, "Supported Hardware & Connection", fontCardTitle, CARD_X + 90, 498, COLOR_WHITE);
        draw_text(scene, "- Compatible with Xbox One, Series X|S, 360, and every XInput controller.", fontCardDesc, CARD_X + 90, 530, COLOR_TEXT_DIM);
        draw_text(scene, "- Connect to any USB port on your PS4 console using a standard USB cable.", fontCardDesc, CARD_X + 90, 560, COLOR_TEXT_DIM);
        draw_text(scene, "- Plug-and-play: recognized immediately by GoldHEN without restarting games.", fontCardDesc, CARD_X + 90, 590, COLOR_TEXT_DIM);

        // Info Card 3: Modes & Shortcuts
        draw_rounded_rect(scene, CARD_X, 640, CARD_W, 190, 12, COLOR_CARD_BG);
        draw_rounded_outline(scene, CARD_X, 640, CARD_W, 190, 12, COLOR_CARD_BORDER, 1);
        draw_image(scene, iconGuide, CARD_X + 24, 665);
        draw_text(scene, "Modes & Quick In-Game Shortcuts", fontCardTitle, CARD_X + 90, 678, COLOR_WHITE);
        draw_text(scene, "- Auto: Automatically assigns to Player 1 or Player 2 depending on connection.", fontCardDesc, CARD_X + 90, 710, COLOR_TEXT_DIM);
        draw_text(scene, "- Merge: Mirrors Xbox controller inputs directly into Player 1 DualShock 4.", fontCardDesc, CARD_X + 90, 740, COLOR_TEXT_DIM);
        draw_text(scene, "- Secondary: Operates as an independent Player 2 controller.", fontCardDesc, CARD_X + 90, 770, COLOR_TEXT_DIM);
        draw_text(scene, "- Quick Shortcut: Press Guide + View + Menu simultaneously to toggle modes.", fontCardDesc, CARD_X + 90, 800, COLOR_GREEN_BRIGHT);

        // 5. Footer for Info
        scene->DrawRectangle(60, 865, 1800, 2, COLOR_DIVIDER);

        draw_image(scene, iconInfo, 60, 890);
        draw_text(scene, "Information & Help", fontCardTitle, 120, 912, COLOR_WHITE);
        draw_text(scene, "Press [L1] to return to Settings", fontFooter, 120, 938, COLOR_TEXT_DIM);

        // Navigation Hints
        draw_image(scene, btnL1, 1540, 901);
        draw_text(scene, "Settings", fontFooter, 1595, 922, COLOR_TEXT_DIM);

        draw_image(scene, btnOptions, 1690, 900);
        draw_text(scene, "Exit", fontFooter, 1770, 922, COLOR_TEXT_DIM);
    }

    // 6. Toast Notification if active
    if (toastFrames > 0) {
        int boxW = 860;
        int boxH = 56;
        int boxX = (1920 - boxW) / 2;
        int boxY = 800;

        draw_rounded_rect(scene, boxX, boxY, boxW, boxH, 10, COLOR_TOAST_BG);
        draw_rounded_outline(scene, boxX, boxY, boxW, boxH, 10, COLOR_TOAST_BORDER, 2);
        draw_text_centered(scene, "Settings saved to /data/GoldHEN/xbox_controller.ini", fontCardTitle, 960, boxY + 36, COLOR_GREEN_BRIGHT);
    }
}
