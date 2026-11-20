#include "pc/configfile.h"
#include "pc/djui/djui_theme.h"
#include "djui_modern.h"

#define COLOR_MATCH_TOLERANCE 24
#define NEUTRAL_CHANNEL_SPREAD 8

static const struct DjuiModernPalette sPalettes[DJUI_THEME_MAX] = {
    [DJUI_THEME_LIGHT] = {
        .text = { 18, 18, 21, 255 },
        .disabledText = { 68, 68, 71, 255 },
        .rect = { 236, 238, 242, 255 },
        .hoveredRect = { 242, 246, 251, 255 },
        .cursorDownRect = { 221, 232, 244, 255 },
        .border = { 82, 82, 85, 255 },
        .hoveredBorder = { 42, 124, 194, 255 },
        .cursorDownBorder = { 34, 92, 151, 255 },
        .panel = { 20, 24, 31, 224 },
        .panelBorder = { 16, 18, 23, 192 },
    },
    [DJUI_THEME_DARK] = {
        .text = { 226, 226, 230, 255 },
        .disabledText = { 148, 150, 155, 255 },
        .rect = { 46, 50, 58, 255 },
        .hoveredRect = { 82, 92, 108, 255 },
        .cursorDownRect = { 108, 120, 137, 255 },
        .border = { 82, 84, 89, 255 },
        .hoveredBorder = { 48, 128, 199, 255 },
        .cursorDownBorder = { 36, 94, 151, 255 },
        .panel = { 20, 24, 31, 226 },
        .panelBorder = { 17, 20, 26, 196 },
    },
    [DJUI_THEME_FILE_SELECT] = {
        .text = { 18, 20, 18, 255 },
        .disabledText = { 68, 70, 68, 255 },
        .rect = { 226, 234, 222, 255 },
        .hoveredRect = { 240, 246, 250, 255 },
        .cursorDownRect = { 218, 233, 244, 255 },
        .border = { 80, 85, 80, 255 },
        .hoveredBorder = { 44, 126, 195, 255 },
        .cursorDownBorder = { 35, 96, 153, 255 },
        .panel = { 222, 183, 65, 248 },
        .panelBorder = { 188, 151, 56, 196 },
    },
    [DJUI_THEME_MARIO] = {
        .text = { 20, 18, 16, 255 },
        .disabledText = { 70, 66, 62, 255 },
        .rect = { 246, 219, 79, 255 },
        .hoveredRect = { 241, 247, 251, 255 },
        .cursorDownRect = { 220, 233, 245, 255 },
        .border = { 200, 169, 12, 255 },
        .hoveredBorder = { 45, 127, 196, 255 },
        .cursorDownBorder = { 36, 97, 154, 255 },
        .panel = { 91, 126, 196, 228 },
        .panelBorder = { 229, 111, 111, 196 },
    },
    [DJUI_THEME_ODYSSEY] = {
        .text = { 18, 17, 20, 255 },
        .disabledText = { 68, 66, 70, 255 },
        .rect = { 247, 247, 249, 255 },
        .hoveredRect = { 241, 246, 251, 255 },
        .cursorDownRect = { 220, 233, 245, 255 },
        .border = { 203, 203, 206, 255 },
        .hoveredBorder = { 45, 126, 196, 255 },
        .cursorDownBorder = { 35, 97, 155, 255 },
        .panel = { 226, 55, 78, 198 },
        .panelBorder = { 246, 235, 238, 188 },
    },
};

static const struct DjuiModernUiColors sUiColors = {
    .overlay = { 17, 21, 28, 255 },
    .panel = { 30, 35, 44, 255 },
    .panelShadow = { 8, 11, 17, 44 },
    .detailTitle = { 252, 252, 253, 255 },
    .detailText = { 222, 223, 227, 255 },
    .statusError = { 236, 116, 125, 255 },
    .statusNotice = { 238, 202, 116, 255 },
    .textShadow = { 64, 64, 64, 100 },
    .transparent = { 0, 0, 0, 0 },
};

static u32 palette_index(void) {
    const u32 theme = configDjuiTheme;
    return (theme < DJUI_THEME_MAX) ? theme : DJUI_THEME_LIGHT;
}

static u8 channel_delta(u8 a, u8 b) {
    return (a > b) ? (u8)(a - b) : (u8)(b - a);
}

static bool color_near(struct DjuiColor a, struct DjuiColor b) {
    return channel_delta(a.r, b.r) <= COLOR_MATCH_TOLERANCE
        && channel_delta(a.g, b.g) <= COLOR_MATCH_TOLERANCE
        && channel_delta(a.b, b.b) <= COLOR_MATCH_TOLERANCE;
}

static u8 shade_channel(u8 channel, f32 scale) {
    const s32 value = (s32)((f32)channel * scale);
    return (u8)((value < 0) ? 0 : (value > 255) ? 255 : value);
}

static u8 maximum_channel(struct DjuiColor color) {
    return (color.r > color.g)
        ? ((color.r > color.b) ? color.r : color.b)
        : ((color.g > color.b) ? color.g : color.b);
}

static u8 minimum_channel(struct DjuiColor color) {
    return (color.r < color.g)
        ? ((color.r < color.b) ? color.r : color.b)
        : ((color.g < color.b) ? color.g : color.b);
}

static struct DjuiColor shade(struct DjuiColor color, f32 scale) {
    color.r = shade_channel(color.r, scale);
    color.g = shade_channel(color.g, scale);
    color.b = shade_channel(color.b, scale);
    return color;
}

const struct DjuiModernPalette* djui_modern_palette(void) {
    return &sPalettes[palette_index()];
}

struct DjuiModernControlColors djui_modern_control_colors(
    enum DjuiModernControlState state, bool isBackButton) {
    const struct DjuiModernPalette* palette = djui_modern_palette();
    const bool darkTheme = palette_index() == DJUI_THEME_DARK;
    struct DjuiModernControlColors colors = {
        .rect = palette->rect,
        .border = palette->border,
    };
    f32 scale = 1.0f;

    switch (state) {
        case DJUI_MODERN_CONTROL_HOVERED:
            colors.rect = palette->hoveredRect;
            colors.border = palette->hoveredBorder;
            scale = 1.0f;
            break;
        case DJUI_MODERN_CONTROL_CURSOR_DOWN:
            colors.rect = palette->cursorDownRect;
            colors.border = palette->cursorDownBorder;
            scale = 1.0f;
            break;
        case DJUI_MODERN_CONTROL_DISABLED:
            scale = isBackButton ? (darkTheme ? 0.3f : 0.55f) : 0.6f;
            break;
        case DJUI_MODERN_CONTROL_DEFAULT:
        default:
            scale = isBackButton ? (darkTheme ? 0.6f : 0.84f) : 1.0f;
            break;
    }

    colors.rect = shade(colors.rect, scale);
    colors.border = shade(colors.border, scale);
    return colors;
}

const struct DjuiModernUiColors* djui_modern_ui_colors(void) {
    return &sUiColors;
}

struct DjuiColor djui_modern_panel_color(struct DjuiColor base) {
    if (maximum_channel(base) < 24) {
        struct DjuiColor color = sUiColors.panel;
        color.a = base.a;
        return color;
    }
    return djui_modern_tint_color(base);
}

struct DjuiColor djui_modern_tint_color(struct DjuiColor color) {
    if (maximum_channel(color) < 128) {
        color.r = (u8)((color.r + 4 > 255) ? 255 : color.r + 4);
        color.g = (u8)((color.g + 7 > 255) ? 255 : color.g + 7);
        color.b = (u8)((color.b + 20 > 255) ? 255 : color.b + 20);
    } else {
        color.r = (u8)((color.r < 3) ? 0 : color.r - 3);
        color.b = (u8)((color.b + 6 > 255) ? 255 : color.b + 6);
    }
    return color;
}

bool djui_modern_is_neutral_color(struct DjuiColor color) {
    return (maximum_channel(color) - minimum_channel(color)) <= NEUTRAL_CHANNEL_SPREAD;
}

bool djui_modern_is_theme_control_color(struct DjuiColor color) {
    const struct DjuiModernPalette* palette = djui_modern_palette();
    if (color_near(color, palette->rect)
        || color_near(color, palette->hoveredRect)
        || color_near(color, palette->cursorDownRect)) {
        return true;
    }
    return color_near(color, shade(palette->rect, 0.6f));
}
