#include <math.h>
#include <string.h>
#include "pc/djui/djui.h"
#include "pc/djui/djui_console.h"
#include "pc/djui/djui_panel_playerlist.h"
#include "pc/djui/djui_panel_modlist.h"
#include "djui_modern.h"
#include "djui_modern_config.h"
#include "djui_modern_rrect.h"
#include "djui_modern_font.h"
#include "pc/debuglog.h"
#include "engine/math_util.h"

static bool sEnabled = false;

static bool color_is_neutral(struct DjuiColor color) {
    const s32 maximum = max(max((s32)color.r, (s32)color.g), (s32)color.b);
    const s32 minimum = min(min((s32)color.r, (s32)color.g), (s32)color.b);
    return (maximum - minimum) <= 8;
}

static bool base_is_checkbox_part(struct DjuiBase* base) {
    return base != NULL
        && (base->type == DJUI_BASE_TYPE_CHECKBOX_FRAME
            || base->type == DJUI_BASE_TYPE_CHECKBOX_VALUE);
}

static struct DjuiBase* slider_fill(struct DjuiBase* base) {
    if (base == NULL) { return NULL; }
    for (struct DjuiBaseChild* child = base->child; child != NULL; child = child->next) {
        if (child->base->type == DJUI_BASE_TYPE_SLIDER_FILL) { return child->base; }
    }
    return NULL;
}

static bool subtree_visible(struct DjuiBase* base) {
    for (struct DjuiBase* b = base; b != NULL; b = b->parent) {
        if (!b->visible) { return false; }
    }
    return true;
}

static bool base_inside(const struct DjuiBase* base, const struct DjuiBase* ancestor) {
    if (ancestor == NULL) { return false; }
    for (const struct DjuiBase* b = base; b != NULL; b = b->parent) {
        if (b == ancestor) { return true; }
    }
    return false;
}

void djui_modern_note_text_created(struct DjuiText* text) {
    if (text == NULL || text->font == NULL) { return; }

    static const s32 kSlots[] = { 0, 3 };
    struct DjuiBase* base = &text->base;

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

    if (!gDjuiModernConfig.fontPlayerList
        && base_inside(base, (gDjuiPlayerList != NULL) ? &gDjuiPlayerList->base : NULL)) {
        want = djui_modern_font_legacy(slot);
    } else if (!gDjuiModernConfig.fontModDescription
               && base_inside(base, (gDjuiModList != NULL) ? &gDjuiModList->base : NULL)) {
        want = djui_modern_font_legacy(slot);
    }

    if (want != NULL && text->font != want) { text->font = want; }
}

enum ThemeScheme {
    SCHEME_NONE = 0,
    SCHEME_ELEM,
    SCHEME_OVERLAY,
    SCHEME_PANEL,
    SCHEME_SECTION,
    SCHEME_TINTED,
    SCHEME_SLIDER_FILL,
    SCHEME_PANEL_ALPHA,
};

#define DJUI_MODERN_MAX_THEMED 1024
static struct {
    struct DjuiBase* base;
    struct DjuiBase* parent;
    u8   scheme;
    u8   srcAlpha;
    u8   lastWrittenAlpha;
    bool seen;
} sThemedMap[DJUI_MODERN_MAX_THEMED];
static u32 sThemedCount = 0;

#define OVERLAY_ALPHA_REF 128

static s32 themed_index_of(struct DjuiBase* base) {
    for (u32 i = 0; i < sThemedCount; i++) {
        if (sThemedMap[i].base == base && sThemedMap[i].parent == base->parent) { return (s32)i; }
    }
    return -1;
}

static s32 themed_register(struct DjuiBase* base, s32 scheme,
                           f32 srcAlphaHint) {
    if (base == NULL) { return -1; }
    const s32 found = themed_index_of(base);
    if (found >= 0) {
        sThemedMap[found].seen   = true;
        sThemedMap[found].scheme = (u8)scheme;
        return found;
    }
    if (sThemedCount >= DJUI_MODERN_MAX_THEMED) {
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

static bool native_alpha_changed(struct DjuiBase* base, s32 idx) {
    if (base == NULL || idx < 0 || idx >= (s32)sThemedCount) { return false; }
    return base->color.a != sThemedMap[idx].lastWrittenAlpha;
}


static void apply_scheme(struct DjuiBase* base, s32 idx) {
    if (base == NULL || idx < 0 || idx >= (s32)sThemedCount) { return; }
    const s32 scheme = sThemedMap[idx].scheme;

    struct DjuiBase* host = (base->interactable != NULL) ? base
                          : (base->parent != NULL && base->parent->interactable != NULL) ? base->parent
                          : NULL;

    if (native_alpha_changed(base, idx)) {
        const u8 nativeA = base->color.a;
        if (nativeA == 0) {
            sThemedMap[idx].srcAlpha = 0;
            sThemedMap[idx].lastWrittenAlpha = 0;
            base->color.a = 0;
            return;
        }
        sThemedMap[idx].srcAlpha = nativeA;
    }

    switch (scheme) {
        case SCHEME_ELEM: {
            const bool enabled = (host != NULL) ? (host->interactable->enabled && host->enabled) : true;
            const u8 srcA = sThemedMap[idx].srcAlpha;
            base->color.a = (u8)(((s32)srcA * gDjuiModernConfig.elementAlpha) / 255);

            if (srcA < 128) { base->color.a = (u8)(base->color.a / 2); }
            if (!enabled) { base->color.a = (u8)(((s32)base->color.a * 2) / 3); }
        } break;

        case SCHEME_OVERLAY: {
            const s32 scaled = ((s32)gDjuiModernConfig.overlayAlpha * (s32)sThemedMap[idx].srcAlpha) / OVERLAY_ALPHA_REF;
            base->color.a = (u8)clamp(scaled, 0, 255);
        } break;

        case SCHEME_PANEL: {
            base->color.a = gDjuiModernConfig.panelAlpha;
        } break;

        case SCHEME_SECTION: {
            const u8 srcA = sThemedMap[idx].srcAlpha;
            base->color.a = (u8)(((s32)srcA * gDjuiModernConfig.panelAlpha) / 255);
        } break;

        case SCHEME_TINTED: {
            base->color.a = (u8)(((s32)sThemedMap[idx].srcAlpha * gDjuiModernConfig.elementAlpha) / 255);
        } break;

        case SCHEME_SLIDER_FILL: {
            base->color.a = (u8)(((s32)sThemedMap[idx].srcAlpha * gDjuiModernConfig.sliderAlpha) / 255);
        } break;

        case SCHEME_PANEL_ALPHA: {
            base->color.a = (u8)(((s32)sThemedMap[idx].srcAlpha * (s32)gDjuiModernConfig.panelAlpha) / 255);
        } break;

        default:
            return;
    }

    sThemedMap[idx].lastWrittenAlpha = base->color.a;
    base->gradient = gDjuiModernConfig.gradients;
}

static void apply_theme(struct DjuiBase* base, bool fillContainer) {
    if (base == NULL) { return; }

    if (fillContainer) {
        const s32 idxOld = themed_index_of(base);
        if (idxOld >= 0 && sThemedMap[idxOld].scheme == SCHEME_PANEL_ALPHA) {
            sThemedMap[idxOld].seen = true;
            apply_scheme(base, idxOld);
            return;
        }
        if (base->color.a == 0) { return; }
        const s32 idx = themed_register(base, SCHEME_PANEL_ALPHA, (f32)base->color.a);
        if (idx >= 0) { apply_scheme(base, idx); }
        return;
    }

    if (base_is_checkbox_part(base)) {
        const s32 idx = themed_index_of(base);
        if (idx >= 0 && sThemedMap[idx].scheme != SCHEME_NONE) {
            base->color.a = sThemedMap[idx].srcAlpha;
            sThemedMap[idx].scheme = SCHEME_NONE;
        }
        return;
    }

    if (base->type == DJUI_BASE_TYPE_SLIDER_FILL) {
        const s32 idx = themed_index_of(base);
        if (idx >= 0) {
            sThemedMap[idx].seen = true;
            apply_scheme(base, idx);
            return;
        }
        if (base->color.a != 0) {
            const s32 created = themed_register(base, SCHEME_SLIDER_FILL, (f32)base->color.a);
            if (created >= 0) { apply_scheme(base, created); }
        }
        return;
    }

    const bool isControlSurface = base->type == DJUI_BASE_TYPE_BUTTON_SURFACE
                               || base->type == DJUI_BASE_TYPE_SELECTIONBOX_BOX
                               || base->type == DJUI_BASE_TYPE_BIND_SURFACE;
    struct DjuiBase* host = NULL;
    if (base->interactable != NULL) {
        host = base;
    } else if (isControlSurface && base->parent != NULL && base->parent->interactable != NULL) {
        host = base->parent;
    }

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

    const struct DjuiColor cur = base->color;

    if (cur.a == 0) { return; }

    if (host != NULL) {
        const s32 idx = themed_register(base, SCHEME_ELEM, (f32)cur.a);
        if (idx >= 0) { apply_scheme(base, idx); }
        return;
    }

    if (color_is_neutral(cur)) {
        if (cur.r < 40 && cur.g < 40 && cur.b < 40) {
            const s32 scheme = (cur.a <= 200) ? SCHEME_OVERLAY : SCHEME_PANEL;
            const s32 idx = themed_register(base, scheme, (f32)cur.a);
            if (idx >= 0) { apply_scheme(base, idx); }
        } else {
            const s32 idx = themed_register(base, SCHEME_SECTION, (f32)cur.a);
            if (idx >= 0) { apply_scheme(base, idx); }
        }
        return;
    }

    const s32 idx = themed_register(base, SCHEME_TINTED, (f32)cur.a);
    if (idx >= 0) { apply_scheme(base, idx); }
}

/* 强符号覆盖上游弱定义。 */
bool djui_modern_rect_override(struct DjuiBase* base);

static u32  sDiagScanned = 0;
static u32  sDiagAdopted = 0;
static u32  sDiagRectCalls = 0;
static u32  sDiagRectEmits = 0;
static u32  sDiagFrames = 0;
static bool sDiagLogged = false;

static void modern_adopt(struct DjuiBase* base) {
    sDiagScanned++;
    if (base->render == djui_rect_render) { sDiagAdopted++; }
}

#define DJUI_MODERN_MAX_HOST_BORDER 64
static struct {
    struct DjuiBase* base;
    f32 width;
    bool seen;
} sHostBorderMap[DJUI_MODERN_MAX_HOST_BORDER];
static u32 sHostBorderCount = 0;

static void host_border_takeover(struct DjuiBase* base) {
    f32 w = -1.0f;
    for (u32 i = 0; i < sHostBorderCount; i++) {
        if (sHostBorderMap[i].base == base) {
            sHostBorderMap[i].seen = true;
            w = sHostBorderMap[i].width;
            break;
        }
    }
    if (w < 0.0f) {
        if (base->borderWidth.value <= 0.001f || base->borderColor.a == 0) { return; }
        if (sHostBorderCount >= DJUI_MODERN_MAX_HOST_BORDER) { return; }
        if (!subtree_visible(base)) { return; }
        sHostBorderMap[sHostBorderCount].base = base;
        sHostBorderMap[sHostBorderCount].width = base->borderWidth.value;
        sHostBorderMap[sHostBorderCount].seen = true;
        sHostBorderCount++;
        w = base->borderWidth.value;
    }
    base->borderWidth.value = 0;
    (void)w;
}

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

static f32 host_border_width(struct DjuiBase* rectNode) {
    struct DjuiBase* host = rectNode->parent;
    if (host == NULL || host->interactable == NULL) { return -1.0f; }
    for (u32 i = 0; i < sHostBorderCount; i++) {
        if (sHostBorderMap[i].base == host) { return sHostBorderMap[i].width; }
    }
    return -1.0f;
}

static struct {
    struct DjuiBase* base;
    f32 width;
    bool seen;
} sFrameBorderMap[DJUI_MODERN_MAX_HOST_BORDER];
static u32 sFrameBorderCount = 0;

static bool base_wants_rounded_border(struct DjuiBase* base) {
    if (base == NULL) { return false; }
    if (base->interactable != NULL) { return false; }
    if (base->parent == NULL) { return false; }
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

static f32 frame_border_width(struct DjuiBase* base) {
    for (u32 i = 0; i < sFrameBorderCount; i++) {
        if (sFrameBorderMap[i].base == base) { return sFrameBorderMap[i].width; }
    }
    return -1.0f;
}

static bool base_is_fill_container(struct DjuiBase* base) {
    if (base == NULL) { return false; }
    if (base->render == NULL) { return false; }
    if (base->render == djui_rect_render) { return false; }
    if (base->interactable != NULL) { return false; }
    if (base->child == NULL) { return false; }
    if (base->color.a == 0) { return false; }
    return true;
}

static void frame_border_keep_alive(struct DjuiBase* base) {
    for (u32 i = 0; i < sFrameBorderCount; i++) {
        if (sFrameBorderMap[i].base == base) {
            sFrameBorderMap[i].seen = true;
            base->borderWidth.value = 0;
            return;
        }
    }
}

static void modern_theme_and_adopt(struct DjuiBase* base, u32 depth) {
    if (base == NULL || depth > 64) { return; }

    modern_adopt(base);

    if (base_is_fill_container(base)) { frame_border_keep_alive(base); }

    if (base->interactable != NULL) {
        host_border_takeover(base);
    } else if (base->type == DJUI_BASE_TYPE_CHECKBOX_FRAME) {
        const f32 w = frame_border_width(base);
        if (w > 0.0f && base->borderWidth.value <= 0.001f) {
            base->borderWidth.value = w;
        }
    } else if (base_wants_rounded_border(base)) {
        frame_border_takeover(base);
    }

    if (base->render == djui_rect_render) {
        apply_theme(base, false);
    } else if (base_is_fill_container(base)) {
        apply_theme(base, true);
    } else if (base->render != NULL && gDjuiModernConfig.showStats) {
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

static void djui_modern_theme_pass(void) {
    if (!sEnabled) { return; }

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

static enum DjuiModernRadius radius_for(struct DjuiBase* base) {
    return (base->type == DJUI_BASE_TYPE_PANEL)
        ? DJUI_MODERN_RADIUS_PANEL
        : DJUI_MODERN_RADIUS_ELEMENT;
}

static void gradient_colors(struct DjuiBase* base,
                            struct DjuiColor* top, struct DjuiColor* bottom) {
    *top = base->color;
    *bottom = base->color;
    if (!base->gradient) { return; }

    const f32 topBoost = gDjuiModernConfig.gradientTopBoost;
    const f32 botScale = gDjuiModernConfig.gradientBotScale;
    top->r = (u8)clamp((s32)(top->r * topBoost), 0, 255);
    top->g = (u8)clamp((s32)(top->g * topBoost), 0, 255);
    top->b = (u8)clamp((s32)(top->b * topBoost), 0, 255);
    bottom->r = (u8)clamp((s32)(bottom->r * botScale), 0, 255);
    bottom->g = (u8)clamp((s32)(bottom->g * botScale), 0, 255);
    bottom->b = (u8)clamp((s32)(bottom->b * botScale), 0, 255);
}

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

    if (!sEnabled || base == NULL || !djui_modern_rrect_ready()) { return false; }

    if (!subtree_visible(base)) { return true; }

    if (base_is_checkbox_part(base)) { return false; }

    if (base->type == DJUI_BASE_TYPE_SLIDER_FILL) { return true; }

    struct DjuiBaseRect* clip = &base->clip;
    struct DjuiBaseRect* comp = &base->comp;

    if (comp->width <= 0.0f || comp->height <= 0.0f) { return true; }
    if (clip->width <= 0.0f || clip->height <= 0.0f) { return true; }

    const f32 borderWidth = base->borderWidth.value;
    const bool hasFill   = (base->color.a > 0);

    struct DjuiBaseRect draw = *clip;
    if (draw.width <= 0.0f || draw.height <= 0.0f) { return true; }

    const enum DjuiModernRadius radius = radius_for(base);

    const f32 explicitRadius = -1.0f;

    struct DjuiBase* fill = slider_fill(base);
    if (fill != NULL) {
        f32 sliderBorderW = -1.0f;
        if (gDjuiModernConfig.showBorders) {
            sliderBorderW = frame_border_width(base);
            if (sliderBorderW < 0.0f) { sliderBorderW = base->borderWidth.value; }
        }
        if (base->borderColor.a == 0) { sliderBorderW = 0.0f; }

        struct DjuiColor fillTop;
        struct DjuiColor fillBottom;
        gradient_colors(fill, &fillTop, &fillBottom);
        const f32 fillFraction = clamp(fill->width.value, 0.0f, 1.0f);
        djui_modern_rrect_draw_slider(clip, &draw, radius, explicitRadius,
                                      fmaxf(sliderBorderW, 0.0f), fillFraction,
                                      base->borderColor, fillTop, fillBottom, 255);
        return true;
    }

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

    diag_big_rect(base, &draw, (s32)radius);

    if (gDjuiModernConfig.showStats && sDiagRectEmits <= 16) {
        LOG_INFO("djui_modern: rect#%u comp(%.0f,%.0f %.0fx%.0f) clip(%.0f,%.0f %.0fx%.0f) color(%u,%u,%u,%u) border(%.1f,%u,%u,%u) radius=%d",
                 sDiagRectEmits, comp->x, comp->y, comp->width, comp->height,
                 clip->x, clip->y, clip->width, clip->height,
                 base->color.r, base->color.g, base->color.b, base->color.a,
                 borderWidth, base->borderColor.r, base->borderColor.g, base->borderColor.a,
                 (int)radius);
    }

    struct DjuiColor top;
    struct DjuiColor bottom;
    gradient_colors(base, &top, &bottom);

    f32 hostBorderW = frame_border_width(base);
    struct DjuiColor borderCol;
    if (hostBorderW >= 0.0f) {
        borderCol = base->borderColor;
    } else {
        hostBorderW = host_border_width(base);
        borderCol = (base->parent != NULL) ? base->parent->borderColor : base->borderColor;
    }
    const bool hasBorder = gDjuiModernConfig.showBorders && hostBorderW > 0.0f && borderCol.a > 0;

    if (radius == DJUI_MODERN_RADIUS_PANEL) {
        struct DjuiBaseRect shadow = draw;
        shadow.y     += 2.0f;
        shadow.x     -= 1.0f;
        shadow.width += 2.0f;
        const f32 shadowRadius = hasBorder
            ? djui_modern_rrect_outer_radius(&draw, radius, explicitRadius, hostBorderW)
            : explicitRadius;
        const struct DjuiColor shadowCol = { 0, 0, 0, 56 };
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
            triCmds++;
        } else if (op == G_TRI2) {
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

    extern bool sDjuiRendered60fps;
    const bool isSecondPass = sDjuiRendered60fps;

    djui_modern_rrect_frame_begin();
    if (isSecondPass) {
        djui_modern_font_pass_begin();
    } else {
        djui_modern_font_frame_begin();
    }
    djui_modern_theme_pass();

    if (!gDjuiModernConfig.showStats) { return; }

    sDiagSegStart = gDisplayListHead;

    {
        static u32 sDiagPre = 0;
        if (sDiagPre < 8) {
            sDiagPre++;
            LOG_INFO("djui_modern: pre#%u head=%p", sDiagPre, (void*)gDisplayListHead);
        }
    }

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

void djui_modern_post_render(void) {
    if (!sEnabled) { return; }
    if (!gDjuiModernConfig.showStats) { return; }

    djui_modern_scan_frame_dl();

    static u32 sDiagPostCount = 0;
    if (sDiagPostCount < 12) {
        sDiagPostCount++;
        LOG_INFO("djui_modern: dl#%u bytes=%u push=%u pop=%u nopush=%u vtx=%u tri=%u tri3d=%u exec=%u %s",
                 sDiagPostCount, sDiagLastBytes, sDiagLastPush, sDiagLastPop, sDiagLastNoPush,
                 sDiagLastVtx, sDiagLastTri, sDiagLastTri3d, sDiagLastExec,
                 (sDiagLastPush == sDiagLastPop) ? "BALANCED" : "!!! UNBALANCED !!!");
        if (sDiagLastTri3d != 0) {
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
