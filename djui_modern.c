#include <math.h>
#include <string.h>
#include "pc/djui/djui.h"
#include "pc/djui/djui_console.h"
// 面板指针，用于判断文字节点所属面板。
#include "pc/djui/djui_panel_playerlist.h"
#include "pc/djui/djui_panel_modlist.h"
#include "pc/djui/djui_panel_host_mods.h"
#include "pc/djui/djui_theme.h"   // DjuiTheme / gDjuiThemes
#include "pc/configfile.h"        // configDjuiTheme
#include "djui_modern.h"
#include "djui_modern_config.h"
#include "djui_modern_rrect.h"
#include "djui_modern_font.h"
#include "pc/debuglog.h"
#include "engine/math_util.h"

/* 上游 mod 描述面板指针；未创建时为 NULL。 */
extern struct DjuiThreePanel* gDjuiHostModsDescriptionPanel;

static bool sEnabled = false;

// 主题配色

/* 配色以当前 DJUI 主题为基础，只做轻量调整。 */

static struct DjuiTheme* active_theme(void) {
    u32 t = configDjuiTheme;
    if (t >= DJUI_THEME_MAX) { t = 0; }
    return gDjuiThemes[t];
}

/* 在原色上增加少量冷色偏移，保持原有明暗关系。 */
static struct DjuiColor modern_tint(struct DjuiColor c) {
    const s32 mx = max(max((s32)c.r, (s32)c.g), (s32)c.b);
    if (mx < 128) {
        // 暗面：略微提亮并偏冷。
        c.r = (u8)clamp((s32)c.r + 4, 0, 255);
        c.g = (u8)clamp((s32)c.g + 7, 0, 255);
        c.b = (u8)clamp((s32)c.b + 20, 0, 255);
    } else {
        // 亮面：轻微冷色偏移。
        c.r = (u8)clamp((s32)c.r - 3, 0, 255);
        c.b = (u8)clamp((s32)c.b + 6, 0, 255);
    }
    return c;
}

/* 近黑面板使用深蓝灰；彩色面板保持主题色。 */
static struct DjuiColor modern_panel_color(struct DjuiColor base) {
    const s32 mx = max(max((s32)base.r, (s32)base.g), (s32)base.b);
    if (mx < 24) {
        const struct DjuiColor p = { 20, 23, 30, base.a };
        return p;
    }
    return modern_tint(base);
}

// 覆盖层底色。
static const struct DjuiColor kOverlayBg = { 8, 10, 14, 0 };

/* 复选框沿用原版配色。 */

#define COLOR_MATCH_TOLERANCE 24

static bool color_is_neutral(struct DjuiColor c) {
    const s32 mx = max(max((s32)c.r, (s32)c.g), (s32)c.b);
    const s32 mn = min(min((s32)c.r, (s32)c.g), (s32)c.b);
    return (mx - mn) <= 8;
}

static bool color_near(struct DjuiColor a, struct DjuiColor b, s32 tol) {
    return abs((s32)a.r - (s32)b.r) <= tol
        && abs((s32)a.g - (s32)b.g) <= tol
        && abs((s32)a.b - (s32)b.b) <= tol;
}

/* 判断颜色是否来自当前主题。 */
static bool color_is_theme_element(struct DjuiColor c) {
    const struct DjuiInteractableTheme* it = &active_theme()->interactables;
    if (color_near(c, it->defaultRectColor,    COLOR_MATCH_TOLERANCE)) { return true; }
    if (color_near(c, it->hoveredRectColor,    COLOR_MATCH_TOLERANCE)) { return true; }
    if (color_near(c, it->cursorDownRectColor, COLOR_MATCH_TOLERANCE)) { return true; }
    // 禁用态使用 defaultRectColor × 0.6。
    if (color_near(c, djui_theme_shade_color(it->defaultRectColor, 0.6f), COLOR_MATCH_TOLERANCE)) {
        return true;
    }
    return false;
}

/*
 * 通过几何和结构识别复选框外框。
 * 中心块不可见时 comp 为 0，边框宽度也可能被接管，因此两者都不参与判定。
 */
static bool base_is_checkbox_frame(struct DjuiBase* base) {
    if (base == NULL || base->interactable != NULL) { return false; }
    if (base->parent == NULL || base->parent->interactable == NULL) { return false; }
    const f32 w = base->comp.width, h = base->comp.height;
    if (w <= 1.0f || h <= 1.0f) { return false; }
    if (w > 48.0f || h > 48.0f) { return false; }
    if (fabsf(w - h) > 4.0f) { return false; }
    // 有且仅有一个非交互子节点。
    if (base->child == NULL || base->child->next != NULL) { return false; }
    if (base->child->base->interactable != NULL) { return false; }
    return true;
}

/* 通过父节点识别复选框中心块。 */
static bool base_is_checkbox_value(struct DjuiBase* base) {
    if (base == NULL || base->interactable != NULL) { return false; }
    if (base->parent == NULL) { return false; }
    return base_is_checkbox_frame(base->parent);
}

/* 检查节点自身和所有祖先是否可见。 */
static bool subtree_visible(struct DjuiBase* base) {
    for (struct DjuiBase* b = base; b != NULL; b = b->parent) {
        if (!b->visible) { return false; }
    }
    return true;
}

/* base 是否位于 ancestor 的子树里（含自身）。ancestor == NULL，false。 */
static bool base_inside(const struct DjuiBase* base, const struct DjuiBase* ancestor) {
    if (ancestor == NULL) { return false; }
    for (const struct DjuiBase* b = base; b != NULL; b = b->parent) {
        if (b == ancestor) { return true; }
    }
    return false;
}

/* 按配置设置文字节点使用的字体。 */
void djui_modern_note_text_created(struct DjuiText* text) {
    if (text == NULL || text->font == NULL) { return; }

    static const s32 kSlots[] = { 0, 3 };
    struct DjuiBase* base = &text->base;

    // 查找节点当前字体对应的槽位。
    s32 slot = -1;
    for (u32 i = 0; i < sizeof(kSlots) / sizeof(kSlots[0]); i++) {
        const struct DjuiFont* mod = djui_modern_font_replaced(kSlots[i]);
        const struct DjuiFont* leg = djui_modern_font_legacy(kSlots[i]);
        if ((mod != NULL && text->font == mod) || (leg != NULL && text->font == leg)) {
            slot = kSlots[i];
            break;
        }
    }
    if (slot < 0) { return; }

    const struct DjuiFont* want = djui_modern_font_replaced(slot);
    if (want == NULL) { return; }

    // 面板级字体覆盖。
    if (!gDjuiModernConfig.fontPlayerList
        && base_inside(base, (gDjuiPlayerList != NULL) ? &gDjuiPlayerList->base : NULL)) {
        want = djui_modern_font_legacy(slot);
    } else if (!gDjuiModernConfig.fontModDescription
               && (base_inside(base, (gDjuiModList != NULL) ? &gDjuiModList->base : NULL)
                || base_inside(base, (gDjuiHostModsDescriptionPanel != NULL)
                                        ? &gDjuiHostModsDescriptionPanel->base : NULL))) {
        want = djui_modern_font_legacy(slot);
    }

    if (want != NULL && text->font != want) { text->font = want; }
}

/* 已登记的主题方案，避免重复推导已修改的颜色。 */
enum ThemeScheme {
    SCHEME_NONE = 0,
    SCHEME_ELEM,      // 可交互元素（按钮/滑条底）
    SCHEME_OVERLAY,   // 半透明覆盖层（聊天栏底、弹窗遮罩）
    SCHEME_PANEL,     // 不透明面板底
    SCHEME_SECTION,   // 段落/次级面板底
    SCHEME_TINTED,    // 有彩色背景（进度条填充等），只做半透明化
    SCHEME_PANEL_ALPHA, // 自绘底板容器（三段面板/弹窗），只改不透明度
};

#define DJUI_MODERN_MAX_THEMED 1024
static struct {
    struct DjuiBase* base;
    struct DjuiBase* parent;   // 用于识别地址复用。
    u8   scheme;         // ThemeScheme
    u8   srcAlpha;       // 原版 alpha。
    u8   srcR, srcG, srcB; // 原版颜色。
    u8   lastWrittenAlpha; // 上次写入的 alpha。
    bool hasColor;       // 是否保留原色相。
    bool srcGradient;    // 原版渐变状态。
    bool seen;
} sThemedMap[DJUI_MODERN_MAX_THEMED];
static u32 sThemedCount = 0;

/*
 * 比较当前 alpha 与上次写入值，以保留原版的显隐和淡出效果。
 * 每次写入后同步 lastWrittenAlpha。
 */
#define OVERLAY_ALPHA_REF 128

/* 查找主题表项；同时比对父节点以识别地址复用。 */
static s32 themed_index_of(struct DjuiBase* base) {
    for (u32 i = 0; i < sThemedCount; i++) {
        if (sThemedMap[i].base == base && sThemedMap[i].parent == base->parent) { return (s32)i; }
    }
    return -1;
}

// 登记主题方案；表满时返回 -1。
static s32 themed_register(struct DjuiBase* base, s32 scheme,
                           struct DjuiColor srcColor, f32 srcAlphaHint,
                           bool hasColor) {
    if (base == NULL) { return -1; }
    const s32 found = themed_index_of(base);
    if (found >= 0) {
        sThemedMap[found].seen   = true;
        sThemedMap[found].scheme = (u8)scheme;
        return found;
    }
    if (sThemedCount >= DJUI_MODERN_MAX_THEMED) {
        // 表满时只警告一次。
        static bool sWarnedThemedFull = false;
        if (!sWarnedThemedFull) {
            sWarnedThemedFull = true;
            LOG_ERROR("djui_modern: themed map full (%d), some widgets will lose theming",
                      DJUI_MODERN_MAX_THEMED);
        }
        return -1;
    }
    const s32 idx = (s32)sThemedCount++;
    sThemedMap[idx].base   = base;
    sThemedMap[idx].parent = base->parent;
    sThemedMap[idx].scheme = (u8)scheme;
    sThemedMap[idx].srcAlpha = (u8)clamp((s32)srcAlphaHint, 0, 255);
    sThemedMap[idx].srcR = srcColor.r;
    sThemedMap[idx].srcG = srcColor.g;
    sThemedMap[idx].srcB = srcColor.b;
    sThemedMap[idx].hasColor = hasColor;
    // 保存原版渐变状态。
    sThemedMap[idx].srcGradient = base->gradient;
    sThemedMap[idx].lastWrittenAlpha = (u8)clamp((s32)srcAlphaHint, 0, 255);
    sThemedMap[idx].seen = true;
    return idx;
}

static void themed_sweep(void) {
    u32 n = 0;
    for (u32 i = 0; i < sThemedCount; i++) {
        if (sThemedMap[i].seen) {
            sThemedMap[i].seen = false;
            if (n != i) { sThemedMap[n] = sThemedMap[i]; }
            n++;
        }
    }
    sThemedCount = n;
}

/*
 * 原版是否在上一帧之后主动改写过这个控件的 alpha？
 *
 * 判据：base->color.a 与模块上次写入的值是否不同。
 * 相等表示模块写入后未被修改，不等表示原版或其他代码更新过该值。
 *
 * 只有整块颜色由模块接管的方案需要此检测。SCHEME_ELEM 的颜色每帧根据
 * hover 和按下状态计算，不依赖原版 alpha，因此不参与检测。
 */
static bool native_alpha_changed(struct DjuiBase* base, s32 idx) {
    if (base == NULL || idx < 0 || idx >= (s32)sThemedCount) { return false; }
    return base->color.a != sThemedMap[idx].lastWrittenAlpha;
}


/* 按登记方案更新颜色。 */
static void apply_scheme(struct DjuiBase* base, s32 idx) {
    if (base == NULL || idx < 0 || idx >= (s32)sThemedCount) { return; }
    const s32 scheme = sThemedMap[idx].scheme;

    // 查找当前交互宿主。
    struct DjuiBase* host = (base->interactable != NULL) ? base
                          : (base->parent != NULL && base->parent->interactable != NULL) ? base->parent
                          : NULL;

    // 保留原版 alpha 控制，包括显隐和淡出。
    if (native_alpha_changed(base, idx)) {
        const u8 nativeA = base->color.a;
        if (nativeA == 0) {
            // 原版要求隐藏。
            sThemedMap[idx].srcAlpha = 0;
            sThemedMap[idx].lastWrittenAlpha = 0;
            base->color.a = 0;
            return;
        }
        // 更新原版 alpha。
        sThemedMap[idx].srcAlpha = nativeA;
    }

    switch (scheme) {
        case SCHEME_ELEM: {
            // 交互元素颜色取自当前主题。
            const struct DjuiInteractableTheme* it = &active_theme()->interactables;
            const bool enabled = (host != NULL) ? (host->interactable->enabled && host->enabled) : true;

            struct DjuiColor bg;
            if (!enabled) {
                bg = djui_theme_shade_color(it->defaultRectColor, 0.6f);   // 原版禁用态
            } else if (host == gDjuiCursorDownOn || host == gInteractableFocus) {
                bg = it->cursorDownRectColor;                              // 原版按下 / 键盘焦点
            } else if (host == gDjuiHovered) {
                bg = it->hoveredRectColor;                                 // 原版悬停
            } else {
                bg = it->defaultRectColor;                                 // 原版常态
            }
            bg = modern_tint(bg);

            // 按原版 alpha 等比缩放，保留实心控件与轨道高光的差异。
            const u8 srcA = sThemedMap[idx].srcAlpha;
            bg.a = (u8)(((s32)srcA * gDjuiModernConfig.elementAlpha) / 255);

            // 低 alpha 轨道再降低一档。
            if (srcA < 128) { bg.a = (u8)(bg.a / 2); }
            if (!enabled) { bg.a = (u8)(((s32)bg.a * 2) / 3); }

            // 保留原版设置的自定义色相。
            if (sThemedMap[idx].hasColor) {
                bg.r = sThemedMap[idx].srcR;
                bg.g = sThemedMap[idx].srcG;
                bg.b = sThemedMap[idx].srcB;
            }
            base->color = bg;
            base->gradient = gDjuiModernConfig.gradients;
        } break;

        case SCHEME_OVERLAY: {
            struct DjuiColor bg = kOverlayBg;
            // 按原版 alpha 比例缩放。
            const s32 scaled = ((s32)gDjuiModernConfig.overlayAlpha * (s32)sThemedMap[idx].srcAlpha) / OVERLAY_ALPHA_REF;
            bg.a = (u8)clamp(scaled, 0, 255);
            base->color = bg;
            base->gradient = gDjuiModernConfig.gradients;
        } break;

        case SCHEME_PANEL: {
            // 面板底：原版 = theme->threePanels.rectColor（djui_panel_menu.c:72）
            struct DjuiColor bg = modern_panel_color(active_theme()->threePanels.rectColor);
            bg.a = gDjuiModernConfig.panelAlpha;
            base->color = bg;
            base->gradient = gDjuiModernConfig.gradients;
        } break;

        case SCHEME_SECTION: {
            // 保留中性灰的色相，只调整透明度。
            const u8 srcA = sThemedMap[idx].srcAlpha;
            struct DjuiColor bg;
            bg.r = sThemedMap[idx].srcR;
            bg.g = sThemedMap[idx].srcG;
            bg.b = sThemedMap[idx].srcB;
            bg = modern_tint(bg);
            bg.a = (u8)(((s32)srcA * gDjuiModernConfig.panelAlpha) / 255);
            base->color = bg;
            base->gradient = gDjuiModernConfig.gradients;
        } break;

        case SCHEME_TINTED: {
            // 保留彩色背景的色相，只调整透明度。
            struct DjuiColor bg;
            bg.r = sThemedMap[idx].srcR;
            bg.g = sThemedMap[idx].srcG;
            bg.b = sThemedMap[idx].srcB;
            bg.a = (u8)(((s32)sThemedMap[idx].srcAlpha * gDjuiModernConfig.elementAlpha) / 255);
            base->color = bg;
            base->gradient = false;
        } break;

        case SCHEME_PANEL_ALPHA: {
            // 自绘底板只调整透明度，保留原版 RGB 与渐变。
            base->color.a = (u8)(((s32)sThemedMap[idx].srcAlpha * (s32)gDjuiModernConfig.panelAlpha) / 255);
        } break;

        default:
            return;   // 未识别方案：不写，也不需要更新 lastWrittenAlpha
    }

    // 记录本次写入值。
    sThemedMap[idx].lastWrittenAlpha = base->color.a;
}

// 主题化

/* 按节点类型应用主题颜色；不修改布局、可见性或回调。 */
static void apply_theme(struct DjuiBase* base, bool fillContainer) {
    if (base == NULL) { return; }

    // 自绘底板只调整 panel_alpha，不进入交互元素分支。
    if (fillContainer) {
        const s32 idxOld = themed_index_of(base);
        if (idxOld >= 0 && sThemedMap[idxOld].scheme == SCHEME_PANEL_ALPHA) {
            sThemedMap[idxOld].seen = true;
            apply_scheme(base, idxOld);
            return;
        }
        // 全透明节点不登记。
        if (base->color.a == 0) { return; }
        const s32 idx = themed_register(base, SCHEME_PANEL_ALPHA, base->color, (f32)base->color.a, false);
        if (idx >= 0) { apply_scheme(base, idx); }
        return;
    }

    // 复选框保持原版颜色；首帧误登记时恢复保存的原色。
    if (base_is_checkbox_frame(base) || base_is_checkbox_value(base)) {
        const s32 idx = themed_index_of(base);
        if (idx >= 0 && sThemedMap[idx].scheme != SCHEME_NONE) {
            base->color.r = sThemedMap[idx].srcR;
            base->color.g = sThemedMap[idx].srcG;
            base->color.b = sThemedMap[idx].srcB;
            base->color.a = sThemedMap[idx].srcAlpha;
            // 恢复原版渐变状态。
            base->gradient = sThemedMap[idx].srcGradient;
            sThemedMap[idx].scheme = SCHEME_NONE;
        }
        return;
    }

    // 查找交互宿主；按钮的交互通常位于父节点。
    struct DjuiBase* host = (base->interactable != NULL) ? base
                          : (base->parent != NULL && base->parent->interactable != NULL) ? base->parent
                          : NULL;

    // 已登记节点按记录方案更新。
    if (host != NULL) {
        const s32 idx = themed_index_of(base);
        if (idx >= 0 && sThemedMap[idx].scheme == SCHEME_ELEM) {
            sThemedMap[idx].seen = true;
            apply_scheme(base, idx);
            return;
        }
    } else {
        const s32 idx = themed_index_of(base);
        if (idx >= 0 && sThemedMap[idx].scheme != SCHEME_NONE) {
            sThemedMap[idx].seen = true;
            apply_scheme(base, idx);
            return;
        }
    }

    // 首次登记时读取原版颜色。
    const struct DjuiColor cur = base->color;

    // 全透明节点不登记。
    if (cur.a == 0) { return; }

    if (host != NULL) {
        // 自定义色相写入快照；主题色继续跟随主题。
        const bool hasColor = !color_is_neutral(cur) && !color_is_theme_element(cur);
        const s32 idx = themed_register(base, SCHEME_ELEM, cur, (f32)cur.a, hasColor);
        if (idx >= 0) { apply_scheme(base, idx); }
        return;
    }

    if (color_is_neutral(cur)) {
        if (cur.r < 40 && cur.g < 40 && cur.b < 40) {
            // 近黑：面板底或覆盖层。
            const s32 scheme = (cur.a <= 200) ? SCHEME_OVERLAY : SCHEME_PANEL;
            const s32 idx = themed_register(base, scheme, cur, (f32)cur.a, false);
            if (idx >= 0) { apply_scheme(base, idx); }
        } else {
            // 中性灰：段落背景。
            const s32 idx = themed_register(base, SCHEME_SECTION, cur, (f32)cur.a, false);
            if (idx >= 0) { apply_scheme(base, idx); }
        }
        return;
    }

    // 彩色背景保留色相。
    const s32 idx = themed_register(base, SCHEME_TINTED, cur, (f32)cur.a, true);
    if (idx >= 0) { apply_scheme(base, idx); }
}

/* 矩形渲染接管入口。定义必须保持强符号，以覆盖上游弱兜底。 */
bool djui_modern_rect_override(struct DjuiBase* base);

/* 一次性自检统计。 */
static u32  sDiagScanned = 0;
static u32  sDiagAdopted = 0;
static u32  sDiagRectCalls = 0;
static u32  sDiagRectEmits = 0;
static u32  sDiagFrames = 0;
static bool sDiagLogged = false;

/* 遍历统计，不参与渲染决策。 */
static void modern_adopt(struct DjuiBase* base) {
    sDiagScanned++;
    if (base->render == djui_rect_render) { sDiagAdopted++; }
}


/* 交互基座边框接管：保存原宽度，并由圆角边框替代原版直角边框。 */

#define DJUI_MODERN_MAX_HOST_BORDER 64
static struct {
    struct DjuiBase* base;
    f32 width;
    bool seen;
} sHostBorderMap[DJUI_MODERN_MAX_HOST_BORDER];
static u32 sHostBorderCount = 0;

static void host_border_takeover(struct DjuiBase* base) {
    // 已记录基座使用保存的宽度。
    f32 w = -1.0f;
    for (u32 i = 0; i < sHostBorderCount; i++) {
        if (sHostBorderMap[i].base == base) {
            sHostBorderMap[i].seen = true;
            w = sHostBorderMap[i].width;
            break;
        }
    }
    if (w < 0.0f) {
        // 仅接管可见边框。
        if (base->borderWidth.value <= 0.001f || base->borderColor.a == 0) { return; }
        if (sHostBorderCount >= DJUI_MODERN_MAX_HOST_BORDER) { return; }
        // 隐藏节点不占用表项。
        if (!subtree_visible(base)) { return; }
        sHostBorderMap[sHostBorderCount].base = base;
        sHostBorderMap[sHostBorderCount].width = base->borderWidth.value;
        sHostBorderMap[sHostBorderCount].seen = true;
        sHostBorderCount++;
        w = base->borderWidth.value;
    }
    // 阻止原版重复绘制边框。
    base->borderWidth.value = 0;
    (void)w;
}

// 清理两帧未见的条目。
static void host_border_sweep(void) {
    u32 n = 0;
    for (u32 i = 0; i < sHostBorderCount; i++) {
        if (sHostBorderMap[i].seen) {
            sHostBorderMap[i].seen = false;
            if (n != i) { sHostBorderMap[n] = sHostBorderMap[i]; }
            n++;
        }
    }
    sHostBorderCount = n;
}

// 查询交互基座边框宽度。
static f32 host_border_width(struct DjuiBase* rectNode) {
    struct DjuiBase* host = rectNode->parent;
    if (host == NULL || host->interactable == NULL) { return -1.0f; }
    for (u32 i = 0; i < sHostBorderCount; i++) {
        if (sHostBorderMap[i].base == host) { return sHostBorderMap[i].width; }
    }
    return -1.0f;
}

/* 非交互基座边框接管表。 */
static struct {
    struct DjuiBase* base;
    f32 width;
    bool seen;
} sFrameBorderMap[DJUI_MODERN_MAX_HOST_BORDER];
static u32 sFrameBorderCount = 0;

/* 判断基座是否需要圆角边框接管。 */
static bool base_wants_rounded_border(struct DjuiBase* base) {
    if (base == NULL) { return false; }
    if (base->interactable != NULL) { return false; }   // 交互基座由 host_border_takeover 负责
    if (base->parent == NULL) { return false; }
    // comp 尚未计算时延后处理。
    if (base->comp.width <= 1.0f || base->comp.height <= 1.0f) { return false; }
    return true;
}

static void frame_border_takeover(struct DjuiBase* base) {
    f32 w = -1.0f;
    for (u32 i = 0; i < sFrameBorderCount; i++) {
        if (sFrameBorderMap[i].base == base) {
            sFrameBorderMap[i].seen = true;
            w = sFrameBorderMap[i].width;
            break;
        }
    }
    if (w < 0.0f) {
        if (base->borderWidth.value <= 0.001f) { return; }
        if (base->borderColor.a == 0) { return; }
        if (sFrameBorderCount >= DJUI_MODERN_MAX_HOST_BORDER) { return; }
        if (!subtree_visible(base)) { return; }
        sFrameBorderMap[sFrameBorderCount].base = base;
        sFrameBorderMap[sFrameBorderCount].width = base->borderWidth.value;
        sFrameBorderMap[sFrameBorderCount].seen = true;
        sFrameBorderCount++;
    }
    base->borderWidth.value = 0;
}

static void frame_border_sweep(void) {
    u32 n = 0;
    for (u32 i = 0; i < sFrameBorderCount; i++) {
        if (sFrameBorderMap[i].seen) {
            sFrameBorderMap[i].seen = false;
            if (n != i) { sFrameBorderMap[n] = sFrameBorderMap[i]; }
            n++;
        }
    }
    sFrameBorderCount = n;
}

/* 查某基座自己是否被收编了边框；有则返回宽度（>=0）。 */
static f32 frame_border_width(struct DjuiBase* base) {
    for (u32 i = 0; i < sFrameBorderCount; i++) {
        if (sFrameBorderMap[i].base == base) { return sFrameBorderMap[i].width; }
    }
    return -1.0f;
}

/* 判断节点是否为自己绘制底板的容器。 */
static bool base_is_fill_container(struct DjuiBase* base) {
    if (base == NULL) { return false; }
    if (base->render == NULL) { return false; }
    if (base->render == djui_rect_render) { return false; }
    if (base->interactable != NULL) { return false; }
    if (base->child == NULL) { return false; }
    if (base->color.a == 0) { return false; }
    return true;
}

/* 保持自绘底板容器的边框记录存活。 */
static void frame_border_keep_alive(struct DjuiBase* base) {
    for (u32 i = 0; i < sFrameBorderCount; i++) {
        if (sFrameBorderMap[i].base == base) {
            sFrameBorderMap[i].seen = true;
            base->borderWidth.value = 0;
            return;
        }
    }
}

/* 每帧遍历控件树，应用主题并接管边框。 */
static void modern_theme_and_adopt(struct DjuiBase* base, u32 depth) {
    if (base == NULL || depth > 64) { return; }

    modern_adopt(base);

    // 保持自绘底板容器的边框记录存活。
    if (base_is_fill_container(base)) { frame_border_keep_alive(base); }

    // 接管边框几何，颜色仍由原版维护。
    if (base->interactable != NULL) {
        host_border_takeover(base);
    } else if (base_is_checkbox_frame(base)) {
        // 复选框保持原版方框边框。
        const f32 w = frame_border_width(base);
        if (w > 0.0f && base->borderWidth.value <= 0.001f) {
            base->borderWidth.value = w;
        }
    } else if (base_wants_rounded_border(base)) {
        // 非交互基座改由圆角边框绘制。
        frame_border_takeover(base);
    }

    // 仅处理绘制矩形填充的节点和自绘底板容器。
    // 文字颜色与图片染色层不能按主题覆盖。
    if (base->render == djui_rect_render) {
        apply_theme(base, false);
    } else if (base_is_fill_container(base)) {
        apply_theme(base, true);
    } else if (base->render != NULL && gDjuiModernConfig.showStats) {
        // 诊断输出非矩形节点信息。
        static u32 sDiagOther = 0;
        if (sDiagOther < 10) {
            sDiagOther++;
            LOG_INFO("djui_modern: node#%u render=%p color(%u,%u,%u,%u) comp(%.0f,%.0f %.0fx%.0f) vis=%d inter=%d",
                     sDiagOther, (const void*)base->render,
                     base->color.r, base->color.g, base->color.b, base->color.a,
                     base->comp.x, base->comp.y, base->comp.width, base->comp.height,
                     (int)base->visible, (int)(base->interactable != NULL));
        }
    }

    for (struct DjuiBaseChild* child = base->child; child != NULL; child = child->next) {
        modern_theme_and_adopt(child->base, depth + 1);
    }
}

/* 遍历包含矩形控件的两棵 UI 树。 */
static void djui_modern_theme_pass(void) {
    if (!sEnabled) { return; }

    // 清理已销毁节点的记录。
    host_border_sweep();
    frame_border_sweep();
    themed_sweep();

    if (gDjuiRoot != NULL) {
        modern_theme_and_adopt(&gDjuiRoot->base, 0);
    }
    if (gDjuiConsole != NULL) {
        modern_theme_and_adopt(&gDjuiConsole->base, 0);
    }
}

/*
 * 圆角矩形渲染接管入口。
 * true：已由本模块绘制；false：交回原版。
 */

static enum DjuiModernRadius radius_for(struct DjuiBase* base, struct DjuiBaseRect* draw) {
    // 交互状态可能位于父节点。
    const bool isElement = (base->interactable != NULL)
                        || (base->parent != NULL && base->parent->interactable != NULL);
    if (isElement) {
        // 较矮的交互元素使用胶囊圆角。
        if (draw->height <= 22.0f) { return DJUI_MODERN_RADIUS_PILL; }
        return DJUI_MODERN_RADIUS_ELEMENT;
    }

    // 大面积使用面板圆角。
    if (draw->width >= 96.0f && draw->height >= 56.0f) { return DJUI_MODERN_RADIUS_PANEL; }

    return DJUI_MODERN_RADIUS_ELEMENT;
}

/* 大矩形诊断输出。 */
#define DIAG_BIG_AREA 8000.0f
#define DIAG_BIG_MAX  32
static struct DjuiBase* sDiagBigSeen[DIAG_BIG_MAX];
static u32 sDiagBigSeenCount = 0;

static bool diag_big_already(struct DjuiBase* base) {
    for (u32 i = 0; i < sDiagBigSeenCount; i++) {
        if (sDiagBigSeen[i] == base) { return true; }
    }
    if (sDiagBigSeenCount < DIAG_BIG_MAX) { sDiagBigSeen[sDiagBigSeenCount++] = base; }
    return false;
}

static const char* diag_node_tag(struct DjuiBase* b) {
    if (b == NULL) { return "?"; }
    if (gDjuiChatBox != NULL && b == &gDjuiChatBox->chatContainer->base) { return "chatContainer"; }
    if (gDjuiChatBox != NULL && b == &gDjuiChatBox->chatFlow->base) { return "chatFlow"; }
    if (gDjuiChatBox != NULL && b == &gDjuiChatBox->base) { return "chatBox"; }
    if (gDjuiChatBox != NULL && b == &gDjuiChatBox->chatInput->base) { return "chatInput"; }
    if (gDjuiConsole != NULL && b == &gDjuiConsole->base) { return "console"; }
    if (gDjuiConsole != NULL && b == &gDjuiConsole->flow->base) { return "consoleFlow"; }
    if (gDjuiRoot != NULL && b == &gDjuiRoot->base) { return "root"; }
    return "-";
}

static void diag_big_rect(struct DjuiBase* base, struct DjuiBaseRect* draw, s32 radius) {
    if (!gDjuiModernConfig.showStats) { return; }
    if (draw->width * draw->height < DIAG_BIG_AREA) { return; }
    if (diag_big_already(base)) { return; }

    LOG_INFO("djui_modern[big]: self=%p(%s) color(%u,%u,%u,%u) draw(%.0f,%.0f %.0fx%.0f) radius=%d border=%.1f",
             (void*)base, diag_node_tag(base),
             base->color.r, base->color.g, base->color.b, base->color.a,
             draw->x, draw->y, draw->width, draw->height, (int)radius, base->borderWidth.value);

    u32 depth = 0;
    for (struct DjuiBase* b = base->parent; b != NULL && depth < 8; b = b->parent, depth++) {
        LOG_INFO("djui_modern[big]:   ^%u %p(%s) render=%p vis=%d inter=%d color(%u,%u,%u,%u) comp(%.0f,%.0f %.0fx%.0f)",
                 depth, (void*)b, diag_node_tag(b), (const void*)b->render,
                 (int)b->visible, (int)(b->interactable != NULL),
                 b->color.r, b->color.g, b->color.b, b->color.a,
                 b->comp.x, b->comp.y, b->comp.width, b->comp.height);
    }
}

bool djui_modern_rect_override(struct DjuiBase* base) {
    sDiagRectCalls++;

    // 任何一环没就绪就交回原实现（返回 false，djui_rect_render 继续执行）。
    if (!sEnabled || base == NULL || !djui_modern_rrect_ready()) { return false; }

    // 自身或祖先隐藏时不绘制。
    if (!subtree_visible(base)) { return true; }

    // 复选框完全交由原版绘制。
    if (base_is_checkbox_frame(base) || base_is_checkbox_value(base)) { return false; }

    struct DjuiBaseRect* clip = &base->clip;
    struct DjuiBaseRect* comp = &base->comp;

    // 无效尺寸不绘制。
    if (comp->width <= 0.0f || comp->height <= 0.0f) { return true; }
    if (clip->width <= 0.0f || clip->height <= 0.0f) { return true; }

    const f32 borderWidth = base->borderWidth.value;
    const bool hasFill   = (base->color.a > 0);

    // 原版矩形填充直接使用 clip。
    struct DjuiBaseRect draw = *clip;
    if (draw.width <= 0.0f || draw.height <= 0.0f) { return true; }

    // 半径按实际绘制矩形判定（而非 comp），避免被父级裁掉后仍按原尺寸算圆角。
    const enum DjuiModernRadius radius = radius_for(base, &draw);

    // 使用 radius_for 计算的半径。
    const f32 explicitRadius = -1.0f;

    // 全透明控件仍可能需要绘制边框。
    if (!hasFill) {
        const f32 bw = frame_border_width(base);
        if (gDjuiModernConfig.showBorders && bw > 0.0f && base->borderColor.a > 0) {
            const struct DjuiColor none = { 0, 0, 0, 0 };
            djui_modern_rrect_draw_framed(clip, &draw, radius, explicitRadius, bw,
                                          base->borderColor, none, none, 255);
        }
        return true;
    }

    sDiagRectEmits++;

    // 诊断：把"大号填充"的归属打印出来（仅 show_stats，按地址去重）
    diag_big_rect(base, &draw, (s32)radius);

    // 诊断转储（仅 show_stats=true 时输出前 16 个）：判断"按钮是没画还是被裁剪"
    if (gDjuiModernConfig.showStats && sDiagRectEmits <= 16) {
        LOG_INFO("djui_modern: rect#%u comp(%.0f,%.0f %.0fx%.0f) clip(%.0f,%.0f %.0fx%.0f) color(%u,%u,%u,%u) border(%.1f,%u,%u,%u) radius=%d",
                 sDiagRectEmits, comp->x, comp->y, comp->width, comp->height,
                 clip->x, clip->y, clip->width, clip->height,
                 base->color.r, base->color.g, base->color.b, base->color.a,
                 borderWidth, base->borderColor.r, base->borderColor.g, base->borderColor.a,
                 (int)radius);
    }

    struct DjuiColor top = base->color;
    struct DjuiColor bottom = base->color;

    if (base->gradient) {
        const f32 topBoost = gDjuiModernConfig.gradientTopBoost;
        const f32 botScale = gDjuiModernConfig.gradientBotScale;
        top.r    = (u8)clamp((s32)(top.r * topBoost), 0, 255);
        top.g    = (u8)clamp((s32)(top.g * topBoost), 0, 255);
        top.b    = (u8)clamp((s32)(top.b * topBoost), 0, 255);
        bottom.r = (u8)clamp((s32)(bottom.r * botScale), 0, 255);
        bottom.g = (u8)clamp((s32)(bottom.g * botScale), 0, 255);
        bottom.b = (u8)clamp((s32)(bottom.b * botScale), 0, 255);
    }

    // 获取边框宽度和颜色。
    f32 hostBorderW = frame_border_width(base);
    struct DjuiColor borderCol;
    if (hostBorderW >= 0.0f) {
        borderCol = base->borderColor;
    } else {
        hostBorderW = host_border_width(base);
        borderCol = (base->parent != NULL) ? base->parent->borderColor : base->borderColor;
    }
    const bool hasBorder = gDjuiModernConfig.showBorders && hostBorderW > 0.0f && borderCol.a > 0;

    // 大面积面板添加轻量投影。
    if (radius == DJUI_MODERN_RADIUS_PANEL && draw.height >= 40.0f) {
        struct DjuiBaseRect shadow = draw;
        shadow.y     += 2.0f;
        shadow.x     -= 1.0f;
        shadow.width += 2.0f;
        const f32 shadowRadius = hasBorder
            ? djui_modern_rrect_outer_radius(&draw, radius, explicitRadius, hostBorderW)
            : explicitRadius;
        struct DjuiColor shadowCol = { 0, 0, 0, 56 };
        djui_modern_rrect_draw(clip, &shadow, radius, shadowRadius, shadowCol, shadowCol, 255);
    }

    if (!hasBorder) {
        djui_modern_rrect_draw(clip, &draw, radius, explicitRadius, top, bottom, 255);
    } else {
        djui_modern_rrect_draw_framed(clip, &draw, radius, explicitRadius, hostBorderW,
                                      borderCol, top, bottom, 255);
    }

    return true;
}

// 生命周期

/* 扫描 display list 的矩阵栈和命令数量。 */
static Gfx* sDiagSegStart = NULL;

static u32   sDiagLastBytes = 0;
static u32   sDiagLastPush = 0, sDiagLastPop = 0, sDiagLastNoPush = 0;
static u32   sDiagLastVtx = 0, sDiagLastTri = 0, sDiagLastTri3d = 0, sDiagLastExec = 0;

static void djui_modern_scan_frame_dl(void) {
    Gfx* start = sDiagSegStart;
    Gfx* end   = gDisplayListHead;

    if (start == NULL || end == NULL || end <= start) { return; }

    u32 push = 0, pop = 0, nopush = 0;
    u32 vtxCmds = 0, triCmds = 0, djuiExec = 0;
    u32 tri3dCmds = 0;
    for (Gfx* p = start; p < end; p++) {
        const u32 op = (p->words.w0 >> 24) & 0xFF;
        if (op == G_MTX) {
            if ((p->words.w0 & G_MTX_PUSH) == 0) { push++; } else { nopush++; }
        } else if (op == G_POPMTX) {
            pop++;
        } else if (op == G_VTX_EXT) {
            vtxCmds++;
        } else if (op == G_TRI2_EXT) {
            // DJUI 2D 三角形。
            triCmds++;
        } else if (op == G_TRI2) {
            // 标准 3D 三角形；DJUI 几何不应使用此路径。
            tri3dCmds++;
        } else if (op == G_EXECUTE_DJUI) {
            djuiExec++;
        }
    }

    sDiagLastBytes = (u32)((end - start) * sizeof(Gfx));
    sDiagLastPush  = push;
    sDiagLastPop   = pop;
    sDiagLastNoPush = nopush;
    sDiagLastVtx = vtxCmds;
    sDiagLastTri = triCmds;
    sDiagLastTri3d = tri3dCmds;
    sDiagLastExec = djuiExec;
}

void djui_modern_pre_render(void) {
    if (!sEnabled) { return; }

    // sDjuiRendered60fps 表示当前是否为插值重渲的第二遍。
    extern bool sDjuiRendered60fps;
    const bool isSecondPass = sDjuiRendered60fps;

    // 每帧初始化与主题更新。
    djui_modern_rrect_frame_begin();
    if (isSecondPass) {
        djui_modern_font_pass_begin();
    } else {
        djui_modern_font_frame_begin();
    }
    djui_modern_theme_pass();

    // 诊断扫描。
    if (!gDjuiModernConfig.showStats) { return; }

    // 记录本段起点，post_render 扫描完整区间。
    sDiagSegStart = gDisplayListHead;

    // 前 8 帧输出边界信息。
    {
        static u32 sDiagPre = 0;
        if (sDiagPre < 8) {
            sDiagPre++;
            LOG_INFO("djui_modern: pre#%u head=%p", sDiagPre, (void*)gDisplayListHead);
        }
    }

    // 第 3 帧输出一次自检信息。
    sDiagFrames++;
    if (!sDiagLogged && sDiagFrames >= 3) {
        sDiagLogged = true;
        u32 atlasPixels = 0, glyphCount = 0, uploads = 0;
        djui_modern_font_get_stats(&atlasPixels, &glyphCount, &uploads);
        LOG_INFO("djui_modern: selfcheck frames=%u scanned=%u adopted=%u rectCalls=%u rectDrawn=%u",
                 sDiagFrames, sDiagScanned, sDiagAdopted, sDiagRectCalls, sDiagRectEmits);
        LOG_INFO("djui_modern: selfcheck font=%s ready=%d atlasPixels=%u glyphs=%u uploads=%u",
                 djui_modern_font_name(), (int)djui_modern_font_ready(), atlasPixels, glyphCount, uploads);
    }
}

/* 帧末诊断。 */
void djui_modern_post_render(void) {
    if (!sEnabled) { return; }
    if (!gDjuiModernConfig.showStats) { return; }

    djui_modern_scan_frame_dl();

    // 最多输出 12 帧。
    static u32 sDiagPostCount = 0;
    if (sDiagPostCount < 12) {
        sDiagPostCount++;
        LOG_INFO("djui_modern: dl#%u bytes=%u push=%u pop=%u nopush=%u vtx=%u tri=%u tri3d=%u exec=%u %s",
                 sDiagPostCount, sDiagLastBytes, sDiagLastPush, sDiagLastPop, sDiagLastNoPush,
                 sDiagLastVtx, sDiagLastTri, sDiagLastTri3d, sDiagLastExec,
                 (sDiagLastPush == sDiagLastPop) ? "BALANCED" : "!!! UNBALANCED !!!");
        if (sDiagLastTri3d != 0) {
            // 标准 3D 三角形不应出现在 DJUI 渲染段。
            LOG_ERROR("djui_modern: %u standard G_TRI2 (3D path) found in DJUI segment -- "
                      "an element likely used gsSP2Triangles instead of gsSP2TrianglesDjui",
                      sDiagLastTri3d);
        }
    }
}

void djui_modern_init(void) {
    djui_modern_config_load();

    if (!gDjuiModernConfig.enabled) {
        LOG_INFO("djui_modern: disabled by config");
        return;
    }

    djui_modern_rrect_init();
    if (!djui_modern_rrect_ready()) {
        LOG_ERROR("djui_modern: rounded-rect mask unavailable, staying on the original UI");
        return;
    }

    // 安装字体并启用主模块。
    djui_modern_font_install();
    sEnabled = true;

    LOG_INFO("djui_modern: enabled (radius panel=%.1f element=%.1f)",
             gDjuiModernConfig.radiusPanel, gDjuiModernConfig.radiusElement);
}

void djui_modern_shutdown(void) {
    djui_modern_rrect_shutdown();
    djui_modern_font_shutdown();
    sEnabled = false;
}
