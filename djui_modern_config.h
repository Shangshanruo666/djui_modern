#pragma once
#include "pc/djui/djui.h"

/* djui_modern 运行期配置。 */

struct DjuiModernConfig {
    // 总开关。
    bool  enabled;

    // 形状。
    f32   radiusPanel;      // 大面板圆角，320x240 坐标。
    f32   radiusElement;    // 普通控件圆角。
    f32   radiusPill;       // 胶囊圆角；< 0 时使用高度的一半。
    // 0：羊角螺线过渡；1：正圆弧。
    f32   cornerRoundness;

    // 不透明度，0-255。
    u8    panelAlpha;
    u8    elementAlpha;
    u8    overlayAlpha;

    // 渐变。
    bool  gradients;
    f32   gradientTopBoost;
    f32   gradientBotScale;

    // 边框。
    bool  showBorders;

    // 字体。
    bool  fontEnabled;
    char  fontPath[260];
    f32   fontRasterBoostCJK;
    bool  fontBilinear;
    // true：字体原生字距；false：对齐原版点阵字体宽度。
    bool  fontNativeMetrics;
    // 字体缺少字偶间距数据时使用内置兜底。
    bool  fontFallbackKerning;
    // 替换字体槽：
    // 0 正文，1 标题艺术字，2 HUD，3 ALIASED，
    // 4 custom HUD，5 recolor HUD，6 special。
    u8    fontSlots[8];
    u8    fontSlotCount;
    // true：现代彩色 Emoji；false：原版 Emoji。
    bool  emojiModern;

    // 字形光栅尺寸倍率；不改变排版尺寸。
    f32   textScale;

    // 指定面板是否使用矢量字体。
    bool  fontPlayerList;
    bool  fontModDescription;

    // 调试统计。
    bool  showStats;
};

extern struct DjuiModernConfig gDjuiModernConfig;

/* 读取配置并填充默认值；重复调用只生效一次。 */
void djui_modern_config_load(void);
