#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "pc/djui/djui.h"
#include "pc/djui/djui_hud_utils.h"
#include "pc/djui/djui_unicode.h"
#include "djui_modern.h"
#include "djui_modern_config.h"
#include "djui_modern_font_atlas.h"
#include "pc/debuglog.h"
#include "engine/math_util.h"

/* 将 TTF/OTF 字形后端接入 DJUI 字体系统。 */

typedef struct FontSlot {
    s8   slot;
    f32  legacyScale;              // 原版该字体的 defaultFontScale
    f32  widthCalibration;         // 西文宽度校准系数。
    f32  latinCalibration;         // 兼容模式的西文宽度系数。
    const struct DjuiFont* legacy; // 原版字体回退。
    struct DjuiFont* self;
} FontSlot;

#define kLatinCalibRelax 0.5f

static inline f32 font_clampf(f32 v, f32 lo, f32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static FontSlot sSlots[8];
static s32 sSlotCount = 0;
static FontSlot* sActive = NULL;
static bool sInstalled = false;

/* djui_gfx_size_translate() 上报的矩阵字号。 */
static f32 sReportedFontSize = 0.0f;

/* 接收当前矩阵字号。 */
void djui_modern_note_font_size(f32 size) {
    sReportedFontSize = size;
}

/* 将上报字号换算为屏幕像素 em 高度。 */
#define kDefaultEmPixelsFallback 32.0f

static f32 current_em_pixels(void) {
    if (sReportedFontSize <= 0.0001f) { return kDefaultEmPixelsFallback; }

    u32 windowWidth = 0, winH = 0;
    gfx_get_dimensions(&windowWidth, &winH);
    if (winH == 0) { winH = SCREEN_HEIGHT; }

    f32 em = sReportedFontSize * ((f32)winH / (f32)SCREEN_HEIGHT);
    if (em < 4.0f) { em = 4.0f; }
    // 上限高于最高光栅档，避免超大 HUD 文字被额外放大。
    if (em > 1024.0f) { em = 1024.0f; }
    return em;
}

/* 标定样例：覆盖大小写、数字与常用标点。 */
static const char* kSample =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789 .,:;!?'\"()[]{}-_/\\|@#$%^&*+=<>~";

static f32 legacy_average_width(const struct DjuiFont* font) {
    if (font == NULL || font->char_width == NULL) { return 0.5f; }

    f32 total = 0.0f;
    s32 count = 0;
    for (const char* c = kSample; *c != '\0'; c++) {
        if (*c == ' ') { continue; }
        char buf[2] = { *c, '\0' };
        const f32 w = font->char_width(buf);
        if (w > 0.0f && w < 4.0f) { total += w; count++; }
    }
    return (count > 0) ? (total / (f32)count) : 0.5f;
}

static f32 modern_average_width(f32 fontScale) {
    f32 total = 0.0f;
    s32 count = 0;
    for (const char* c = kSample; *c != '\0'; c++) {
        if (*c == ' ') { continue; }
        char buf[2] = { *c, '\0' };
        const f32 w = djui_modern_font_atlas_char_width(buf, fontScale);
        if (w > 0.0f && w < 4.0f) { total += w; count++; }
    }
    return (count > 0) ? (total / (f32)count) : 0.5f;
}

// DjuiFont 回调：每槽一套薄包装，用于把 sActive 指向自己的槽位。

/*
 * 该字符是否属于原版 emoji 表（spriteIndex 带 0x020000 标志）。原版 emoji
 * 只有位图、没有轮廓，矢量字体渲染不了。
 */
static bool is_vanilla_emoji(const char* c) {
    return (djui_unicode_get_sprite_index(c) & 0x020000u) != 0;
}

/* 该字符是否要交回原版渲染器（原版自带 32x32 单色 emoji 贴图）。 */
static bool emoji_needs_legacy(const char* c) {
    return is_vanilla_emoji(c) && !djui_modern_emoji_available(c);
}

/* 崩溃屏使用原版等宽字体。 */
static bool crash_fallback_active(void) {
    return gDjuiDisabled;
}

static f32 slot_char_width(const char* c) {
    if (sActive == NULL) { return 0.5f; }

    if (crash_fallback_active() || !djui_modern_font_atlas_ready()) {
        return (sActive->legacy && sActive->legacy->char_width)
             ? sActive->legacy->char_width(c) : 0.5f;
    }

    // 不可用的 Emoji 交由原版字体测量。
    if (emoji_needs_legacy(c)) {
        return (sActive->legacy && sActive->legacy->char_width)
             ? sActive->legacy->char_width(c) : 1.0f;
    }

    // 原生模式直接使用字体度量；兼容模式只校准西文宽度。
    const f32 raw = djui_modern_font_atlas_char_width(c, sActive->legacyScale);

    if (gDjuiModernConfig.fontNativeMetrics) { return raw; }

    s32 bytes = 1;
    const u32 cp = djui_modern_font_utf8_decode(c, &bytes);
    if (codepoint_is_cjk(cp)) { return raw; }

    return raw * sActive->latinCalibration;
}

static void slot_render_begin(void) {
    if (sActive == NULL || crash_fallback_active() || !djui_modern_font_atlas_ready()) {
        if (sActive && sActive->legacy && sActive->legacy->render_begin) { sActive->legacy->render_begin(); }
        return;
    }
    // 获取本次文字的屏幕 em 像素高度。
    const f32 emPixels = current_em_pixels();

    // 与 render_end 成对压栈。
    djui_modern_font_atlas_push_scale_hint(emPixels, 1.0f);
    djui_modern_font_atlas_render_begin();

    // 同步名义字格高。
    if (sActive->self != NULL) {
        djui_modern_font_atlas_set_cell_height(sActive->self->charHeight);
    }
}

static void slot_render_char(const char* c) {
    if (sActive == NULL) { return; }

    if (crash_fallback_active() || !djui_modern_font_atlas_ready()) {
        if (sActive->legacy && sActive->legacy->render_char) { sActive->legacy->render_char(c); }
        return;
    }

    // Emoji 使用原版 RDP 状态，绘制后恢复本模块状态。
    if (emoji_needs_legacy(c)) {
        if (sActive->legacy && sActive->legacy->render_begin) { sActive->legacy->render_begin(); }
        if (sActive->legacy && sActive->legacy->render_char) { sActive->legacy->render_char(c); }
        djui_modern_font_atlas_render_begin();   // 恢复本模块的状态（并让纹理绑定失效）
        return;
    }

    // 测量与绘制共用同一套字号分档。
    djui_modern_font_atlas_render_char(c);
}

static void slot_render_end(void) {
    if (sActive == NULL || crash_fallback_active() || !djui_modern_font_atlas_ready()) {
        if (sActive && sActive->legacy && sActive->legacy->render_end) { sActive->legacy->render_end(); }
        return;
    }
    djui_modern_font_atlas_render_end();
    djui_modern_font_atlas_pop_scale_hint();
}

#define DECLARE_SLOT(N)                                            \
    static f32  slot_width_##N(const char* c) { sActive = &sSlots[N]; return slot_char_width(c); }  \
    static void slot_begin_##N(void)          { sActive = &sSlots[N]; slot_render_begin(); }        \
    static void slot_char_##N(const char* c)  { sActive = &sSlots[N]; slot_render_char(c); }        \
    static void slot_end_##N(void)            { sActive = &sSlots[N]; slot_render_end(); }

DECLARE_SLOT(0)
DECLARE_SLOT(1)
DECLARE_SLOT(2)
DECLARE_SLOT(3)
DECLARE_SLOT(4)
DECLARE_SLOT(5)
DECLARE_SLOT(6)
DECLARE_SLOT(7)

typedef struct {
    f32  (*char_width)(const char*);
    void (*render_begin)(void);
    void (*render_char)(const char*);
    void (*render_end)(void);
} SlotCallbacks;

static const SlotCallbacks sSlotCallbacks[8] = {
    { slot_width_0, slot_begin_0, slot_char_0, slot_end_0 },
    { slot_width_1, slot_begin_1, slot_char_1, slot_end_1 },
    { slot_width_2, slot_begin_2, slot_char_2, slot_end_2 },
    { slot_width_3, slot_begin_3, slot_char_3, slot_end_3 },
    { slot_width_4, slot_begin_4, slot_char_4, slot_end_4 },
    { slot_width_5, slot_begin_5, slot_char_5, slot_end_5 },
    { slot_width_6, slot_begin_6, slot_char_6, slot_end_6 },
    { slot_width_7, slot_begin_7, slot_char_7, slot_end_7 },
};

// 安装。

static bool install_slot(s32 slot) {
    if (slot < 0 || slot >= FONT_COUNT || slot >= 8) { return false; }

    const struct DjuiFont* legacy = gDjuiFonts[slot];
    if (legacy == NULL) { return false; }

    // sSlots 按原版槽号索引，必须与槽回调一一对应。
    FontSlot* inst = &sSlots[slot];
    memset(inst, 0, sizeof(*inst));
    inst->slot = (s8)slot;
    inst->legacy = legacy;
    inst->legacyScale = legacy->defaultFontScale;

    // 兼容模式下的西文宽度校准。
    {
        const f32 legacyAvg = legacy_average_width(legacy);
        const f32 modernAvg = modern_average_width(inst->legacyScale);
        inst->widthCalibration = (modernAvg > 0.0001f) ? (legacyAvg / modernAvg) : 1.0f;
        inst->widthCalibration = font_clampf(inst->widthCalibration, 0.5f, 2.0f);
        inst->latinCalibration = 1.0f - (1.0f - inst->widthCalibration) * kLatinCalibRelax;
        inst->latinCalibration = font_clampf(inst->latinCalibration, 0.5f, 2.0f);
    }

    struct DjuiFont* f = calloc(1, sizeof(struct DjuiFont));
    if (f == NULL) { return false; }
    inst->self = f;

    // 名义字格宽必须与图集层的裁剪换算一致。
    f->charWidth            = DJUI_MODERN_FONT_CELL_W;
    f->charHeight           = legacy->charHeight;
    f->lineHeight           = legacy->lineHeight;
    f->xOffset              = legacy->xOffset;
    f->yOffset              = legacy->yOffset;
    f->defaultFontScale     = legacy->defaultFontScale;
    f->textBeginDisplayList = legacy->textBeginDisplayList;
    f->render_begin         = sSlotCallbacks[slot].render_begin;
    f->render_char          = sSlotCallbacks[slot].render_char;
    f->render_end           = sSlotCallbacks[slot].render_end;
    f->char_width           = sSlotCallbacks[slot].char_width;

    gDjuiFonts[slot] = f;
    sSlotCount++;
    LOG_INFO("djui_modern: font slot %d -> TTF (scale=%.1f, nativeMetrics=%d, latinCalib=%.3f raw=%.3f)",
             slot, inst->legacyScale, (int)gDjuiModernConfig.fontNativeMetrics,
             inst->latinCalibration, inst->widthCalibration);
    return true;
}

void djui_modern_font_install(void) {
    if (sInstalled) { return; }
    sInstalled = true;

    if (!gDjuiModernConfig.fontEnabled) {
        LOG_INFO("djui_modern: TTF fonts disabled by config");
        return;
    }

    if (!djui_modern_font_atlas_init()) {
        LOG_INFO("djui_modern: no TTF available, keeping original bitmap fonts");
        return;
    }

    if (gDjuiModernConfig.fontSlotCount == 0) {
        LOG_INFO("djui_modern: font_slots empty, nothing replaced");
        return;
    }

    for (u8 i = 0; i < gDjuiModernConfig.fontSlotCount; i++) {
        if (sSlotCount >= (s32)(sizeof(sSlots) / sizeof(sSlots[0]))) { break; }
        install_slot((s32)gDjuiModernConfig.fontSlots[i]);
    }
}

/* 由 djui_modern.c 的生命周期钩子调用。 */
void djui_modern_font_relinquish(void) {
    // 还原 gDjuiFonts 指针并释放槽位数据。
    for (s32 i = 0; i < (s32)(sizeof(sSlots) / sizeof(sSlots[0])); i++) {
        if (sSlots[i].legacy != NULL && sSlots[i].slot >= 0) {
            gDjuiFonts[sSlots[i].slot] = sSlots[i].legacy;
        }
        if (sSlots[i].self != NULL) { free(sSlots[i].self); sSlots[i].self = NULL; }
    }
    sSlotCount = 0;
    sActive = NULL;
    sInstalled = false;
}

/* 字体接口封装。 */

/* 字体（TTF/OTF）后端是否已就绪。 */
bool djui_modern_font_ready(void) {
    return djui_modern_font_atlas_ready();
}

/* 当前字体文件名，用于日志（未就绪时返回 "none"）。 */
const char* djui_modern_font_name(void) {
    return djui_modern_font_atlas_ready() ? djui_modern_font_atlas_name() : "none";
}

/* 槽位被替换后的模块字体对象；未替换返回 NULL。 */
const struct DjuiFont* djui_modern_font_replaced(s32 slot) {
    if (slot < 0 || slot >= (s32)(sizeof(sSlots) / sizeof(sSlots[0]))) { return NULL; }
    return sSlots[slot].self;
}

/* 槽位被替换之前的原版字体对象；未替换返回 NULL。 */
const struct DjuiFont* djui_modern_font_legacy(s32 slot) {
    if (slot < 0 || slot >= (s32)(sizeof(sSlots) / sizeof(sSlots[0]))) { return NULL; }
    return sSlots[slot].legacy;
}

/* 图集统计：像素数 / 字形数 / 上传次数。 */
void djui_modern_font_get_stats(u32* atlasPixels, u32* glyphCount, u32* uploads) {
    if (atlasPixels != NULL) { *atlasPixels = 0; }
    if (glyphCount  != NULL) { *glyphCount  = 0; }
    if (uploads     != NULL) { *uploads     = 0; }
    if (!djui_modern_font_atlas_ready()) { return; }
    djui_modern_font_atlas_stats(atlasPixels, glyphCount, uploads);
}

// 每帧开始：重置每帧顶点游标（图集常驻，无需每帧维护）。
void djui_modern_font_frame_begin(void) {
    if (!djui_modern_font_atlas_ready()) { return; }
    djui_modern_font_atlas_frame_begin();
}

/* 第二遍渲染时保留顶点池。 */
void djui_modern_font_pass_begin(void) {
    if (!djui_modern_font_atlas_ready()) { return; }
    djui_modern_font_atlas_pass_begin();
}

void djui_modern_font_shutdown(void) {
    djui_modern_font_relinquish();
    djui_modern_font_atlas_shutdown();
}
