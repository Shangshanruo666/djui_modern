#pragma once

#include "pc/djui/djui.h"

enum DjuiModernControlState {
    DJUI_MODERN_CONTROL_DEFAULT,
    DJUI_MODERN_CONTROL_HOVERED,
    DJUI_MODERN_CONTROL_CURSOR_DOWN,
    DJUI_MODERN_CONTROL_DISABLED,
};

struct DjuiModernPalette {
    struct DjuiColor text;
    struct DjuiColor disabledText;
    struct DjuiColor rect;
    struct DjuiColor hoveredRect;
    struct DjuiColor cursorDownRect;
    struct DjuiColor border;
    struct DjuiColor hoveredBorder;
    struct DjuiColor cursorDownBorder;
    struct DjuiColor panel;
    struct DjuiColor panelBorder;
};

struct DjuiModernControlColors {
    struct DjuiColor rect;
    struct DjuiColor border;
};

struct DjuiModernUiColors {
    struct DjuiColor overlay;
    struct DjuiColor panel;
    struct DjuiColor panelShadow;
    struct DjuiColor detailTitle;
    struct DjuiColor detailText;
    struct DjuiColor statusError;
    struct DjuiColor statusNotice;
    struct DjuiColor textShadow;
    struct DjuiColor transparent;
};

const struct DjuiModernPalette* djui_modern_palette(void);

struct DjuiModernControlColors djui_modern_control_colors(
    enum DjuiModernControlState state, bool isBackButton);

const struct DjuiModernUiColors* djui_modern_ui_colors(void);
struct DjuiColor djui_modern_panel_color(struct DjuiColor base);
struct DjuiColor djui_modern_tint_color(struct DjuiColor color);
bool djui_modern_is_neutral_color(struct DjuiColor color);

bool djui_modern_is_theme_control_color(struct DjuiColor color);

// 在 DJUI 首次创建文字节点前调用。
void djui_modern_init(void);

// 在 DJUI 每帧绘制前调用。
void djui_modern_pre_render(void);

// 在 DJUI 每帧绘制后调用。
void djui_modern_post_render(void);

// 在 DJUI 关闭时调用。
void djui_modern_shutdown(void);
