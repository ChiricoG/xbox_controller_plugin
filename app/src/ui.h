#pragma once

#include "common/graphics.h"
#include "config_manager.h"
#include <string>

struct ImageRGBA {
    int width;
    int height;
    uint32_t* pixels;
    ImageRGBA() : width(0), height(0), pixels(NULL) {}
};

class UI {
public:
    UI();
    ~UI();

    bool init(Scene2D* scene);
    void render(Scene2D* scene, const AppConfig& cfg, int activeTab, int selectedCard, int toastFrames);

private:
    FT_Face fontTitle;
    FT_Face fontSubtitle;
    FT_Face fontCardTitle;
    FT_Face fontCardDesc;
    FT_Face fontValue;
    FT_Face fontFooter;

    uint32_t* cached_bg;
    int bg_width;
    int bg_height;

    ImageRGBA xboxLogo;
    ImageRGBA iconGear;
    ImageRGBA iconInfo;
    ImageRGBA iconSave;
    ImageRGBA iconMode;
    ImageRGBA iconGuide;
    ImageRGBA iconDeadzone;
    ImageRGBA iconSwap;
    ImageRGBA iconRumble;
    ImageRGBA btnArrows;
    ImageRGBA btnCross;
    ImageRGBA btnOptions;
    ImageRGBA btnL1;
    ImageRGBA btnR1;

    ImageRGBA load_icon(const char* path);
    void free_icon(ImageRGBA& img);
    void draw_image(Scene2D* scene, const ImageRGBA& img, int startX, int startY);

    void draw_rounded_rect(Scene2D* scene, int x, int y, int w, int h, int r, Color color);
    void draw_rounded_outline(Scene2D* scene, int x, int y, int w, int h, int r, Color color, int thickness);
    void draw_text(Scene2D* scene, const std::string& text, FT_Face face, int startX, int baselineY, Color color);
    void draw_text_centered(Scene2D* scene, const std::string& text, FT_Face face, int centerX, int baselineY, Color color);
};
