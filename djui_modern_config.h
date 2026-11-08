#pragma once
#include "pc/djui/djui.h"

enum {
    DJUI_MODERN_FONT_SLOT_COUNT = 7,
    DJUI_MODERN_FONT_PATH_MAX = 260,
};

struct DjuiModernConfig {
    bool  enabled;

    // 半径使用 320x240 DJUI 坐标。
    f32   radiusPanel;
    f32   radiusElement;
    // 0 为羊角螺线过渡，1 为正圆弧。
    f32   cornerRoundness;

    u8    panelAlpha;
    u8    elementAlpha;
    u8    overlayAlpha;

    bool  gradients;
    f32   gradientTopBoost;
    f32   gradientBotScale;

    bool  showBorders;

    bool  fontEnabled;
    char  fontPath[DJUI_MODERN_FONT_PATH_MAX];
    f32   fontRasterBoostCJK;
    bool  fontBilinear;
    bool  fontNativeMetrics;
    bool  fontFallbackKerning;
    // 槽号：0=Normal 1=Title 2=HUD 3=Aliased 4=CustomHUD 5=RecolorHUD 6=Special
    u8    fontSlots[DJUI_MODERN_FONT_SLOT_COUNT];
    u8    fontSlotCount;
    bool  emojiModern;
    f32   textScale;

    bool  fontPlayerList;
    bool  fontModDescription;

    bool  showStats;
};

extern struct DjuiModernConfig gDjuiModernConfig;

/* 填充默认值并清零其余字段。 */
void djui_modern_config_set_defaults(struct DjuiModernConfig* cfg);

/* 启动时读取配置；重复调用只生效一次。 */
void djui_modern_config_load(void);

/* 从磁盘读取配置，不修改 gDjuiModernConfig。 */
bool djui_modern_config_read(struct DjuiModernConfig* cfg, const char** error);

/* 将给定配置写回磁盘。 */
bool djui_modern_config_save(const struct DjuiModernConfig* cfg, const char** error);
