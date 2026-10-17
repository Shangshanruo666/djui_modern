#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pc/djui/djui.h"
#include "djui_modern.h"
#include "djui_modern_config.h"
#include "djui_modern_font_atlas.h"
#include "djui_modern_gbi.h"   // gsSP2TrianglesDjui（本模块私有，不改上游 gbi_extension.h）
#include "pc/debuglog.h"
#include "pc/fs/fs.h"
#include "engine/math_util.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* stb_image 实现已由 gfx_pc.c 提供。 */
#include <stb/stb_image.h>

/*
 * 内嵌字体、Emoji 与国旗数据。
 * Emoji 和国旗使用两张布局相同的 512x512 图集。
 */
#if defined(__has_include)
#  if __has_include("djui_modern_font_latin_data.h")
#    include "djui_modern_font_latin_data.h"
#  endif
#  if __has_include("djui_modern_font_cjk_data.h")
#    include "djui_modern_font_cjk_data.h"
#  endif
#  if __has_include("djui_modern_emoji_data.h")
#    include "djui_modern_emoji_data.h"
#  endif
#  if __has_include("djui_modern_flag_data.h")
#    include "djui_modern_flag_data.h"
#  endif
#endif

/* 图集边长必须不超过 S10.5 UV 的安全范围。 */
#if defined(DJUI_MODERN_EMOJI_COUNT)
typedef char djui_modern_emoji_atlas_dim_ok[
    (DJUI_MODERN_EMOJI_DIM_X <= 1023 && DJUI_MODERN_EMOJI_DIM_Y <= 1023) ? 1 : -1];
#endif

/*
 * 每个字符和光栅档位使用一张不可变纹理。
 * 连续相同纹理跳过重复绑定；未知字符使用回退字形。
 */

// 常量

/* 光栅像素高度档位。 */
static const s32 kRasterBuckets[] = { 12, 16, 20, 26, 32, 40, 52, 64, 88, 128, 192, 320, 512 };
#define BUCKET_COUNT ((s32)(sizeof(kRasterBuckets) / sizeof(kRasterBuckets[0])))

/* 字符集上限。字形位图按需生成。 */
#define CHARSET_MAX 8192

// 回退槽在槽位表里的下标（每个档位各一个）
#define FALLBACK_INDEX 0

// 字形纹理边长下限（2 的幂）。太小会让 gfx_pc 的 tile 计算退化。
#define GLYPH_TEX_MIN 4
/* 字形纹理边长上限。受 S10.5 UV 范围限制。 */
#define GLYPH_TEX_MAX 512

/* 字形纹理总字节预算。 */
#define GLYPH_BYTES_LIMIT (128u * 1024u * 1024u)

// 字形槽位

typedef struct GlyphSlot {
    u8*  tex;         // RGBA32 纹理，生成后不再修改。
    s32  tw, th;      // 纹理边长。
    s32  twLog2, thLog2;
    s32  bw, bh;      // 位图尺寸。
    s32  xoff, yoff;  // 位图偏移。
    f32  advanceEm;   // advance / unitsPerEm。
    f32  rasterEm;    // 本次光栅化使用的 em 尺寸。
    u8   state;       // 0 未生成，1 已生成，2 无法生成。
} GlyphSlot;

/* 字体链按注册顺序选择第一个包含目标码点的字体。 */
#define FONT_MAX 3

typedef struct {
    stbtt_fontinfo info;
    u8*  data;
    u32  size;
    bool owned;
    f32  unitsPerEm;
    const char* name;
} FontFace;

static FontFace sFaces[FONT_MAX];
static s32      sFaceCount = 0;

/* 当前字形所属字体。 */
static FontFace* sCur = NULL;
#define sFont        (sCur->info)
#define sUnitsPerEm  (sCur->unitsPerEm)

static bool sReady = false;
static char sFontName[200] = "none";

static void detect_latin_monospace(void);   // 定义见下方"宽度查询"一节

/* 返回含该码点的字体下标（按注册顺序取第一个）；没有则 -1。 */
static s32 font_face_for(u32 cp) {
    for (s32 i = 0; i < sFaceCount; i++) {
        if (stbtt_FindGlyphIndex(&sFaces[i].info, (s32)cp) != 0) { return i; }
    }
    return -1;
}

static f32  sAscentEm   = 0.8f;
static f32  sDescentEm  = -0.2f;
static f32  sBaselineY  = -0.85f;

// 字符集：下标 -> codepoint
static u32   sCharset[CHARSET_MAX];
static s32   sCharsetCount = 0;
static s32   sAsciiIndex[128];      // ASCII 快速索引（-1 = 不在集合里）

/* 字符槽位按行惰性分配。 */
static GlyphSlot** sSlotGrid = NULL;   // [CHARSET_MAX]，行按需分配
static s32   sSlotRows = 0;            // = sCharsetCount
static s32   sSlotRowsUsed = 0;        // 实际分配过的行数（诊断用）

/* 取槽位；该字符首次被用到时才为它分配一整行（分配失败返回 NULL）。 */
static GlyphSlot* slot_at(s32 ci, s32 bi) {
    if (sSlotGrid == NULL || ci < 0 || ci >= sSlotRows || bi < 0 || bi >= BUCKET_COUNT) {
        return NULL;
    }
    GlyphSlot* row = sSlotGrid[ci];
    if (row == NULL) {
        row = (GlyphSlot*)calloc(BUCKET_COUNT, sizeof(GlyphSlot));
        if (row == NULL) { return NULL; }
        sSlotGrid[ci] = row;
        sSlotRowsUsed++;
    }
    return &row[bi];
}

// 字形顶点池。
#define GLYPH_VERTS 4
#define GLYPH_POOL_BLOCKS 16384
static Vtx*  sVtxPool = NULL;
static u32   sVtxUsed = 0;

// 当前已绑定的字形纹理（用于跳过重复绑定）
static const u8* sBoundTex = NULL;

/* 把字格裁剪比例换算到字形墨迹盒坐标。 */
static const Gfx* sGlyphDlMark = NULL;

/* 聊天框使用垂直硬裁剪，其余控件允许文字上下溢出。 */
static bool sTextClipHard = false;

/* 当前名义字格高。 */
static f32 sCellHeight = 1.0f;

/* 聊天框硬裁剪的下边界余量。 */
static f32 sClipPadBottomEm = 0.0f;

void djui_modern_font_atlas_set_cell_height(f32 h) {
    if (h > 0.01f) { sCellHeight = h; }
}

static bool base_is_chat_box_part(const struct DjuiBase* base) {
    if (gDjuiChatBox == NULL) { return false; }
    if (base == &gDjuiChatBox->base) { return true; }
    if (gDjuiChatBox->chatContainer != NULL && base == &gDjuiChatBox->chatContainer->base) { return true; }
    if (gDjuiChatBox->chatFlow      != NULL && base == &gDjuiChatBox->chatFlow->base)      { return true; }
    if (gDjuiChatBox->chatInput     != NULL && base == &gDjuiChatBox->chatInput->base)     { return true; }
    return false;
}

/* djui_gfx.c 的 djui_gfx_add_clipping_specific() 每字形调用一次（强定义覆盖弱空兜底）。 */
void djui_modern_note_text_base(struct DjuiBase* base) {
    sTextClipHard = false;
    for (const struct DjuiBase* b = base; b != NULL; b = b->parent) {
        if (base_is_chat_box_part(b)) { sTextClipHard = true; return; }
    }
}

static f32 clip01(f32 v) { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); }

/* 将字格裁剪比例换算到当前墨迹盒。 */
static void glyph_fix_pending_clip(f32 inkX0, f32 inkW, f32 inkY0, f32 inkH) {
    if (sGlyphDlMark == NULL) { return; }
    for (const Gfx* p = sGlyphDlMark; p < gDisplayListHead; p++) {
        if ((u32)(p->words.w0 >> 24) != (u32)G_TEXCLIP_DJUI) { continue; }

        const f32 f1 = (f32)((p->words.w0 >> 16) & 0xFFu) / 255.0f;   // 从左边裁掉
        const f32 f2 = (f32)((p->words.w1 >> 16) & 0xFFu) / 255.0f;   // 从右边裁掉
        const f32 g1 = (f32)((p->words.w0 >>  8) & 0xFFu) / 255.0f;   // 从上边裁掉
        const f32 g2 = (f32)((p->words.w1 >>  8) & 0xFFu) / 255.0f;   // 从下边裁掉
        const f32 cw = DJUI_MODERN_FONT_CELL_W;

        // 水平裁剪。
        f32 q1 = 0.0f, q2 = 0.0f;
        if (inkW > 0.001f) {
            q1 = (f1 * cw - inkX0) / inkW;
            q2 = (inkX0 + inkW - (cw - f2 * cw)) / inkW;
        }

        // 只有聊天框保留垂直裁剪。
        f32 ky1 = 0.0f, ky2 = 0.0f;
        if (sTextClipHard && inkH > 0.001f) {
            // 下边界额外放宽字形下伸部。
            const f32 clipTop = -g1 * sCellHeight;
            const f32 clipBot = (g2 - 1.0f) * sCellHeight - sClipPadBottomEm;
            ky1 = (inkY0 - clipTop) / inkH;
            ky2 = (clipBot - (inkY0 - inkH)) / inkH;
        }

        q1 = clip01(q1); q2 = clip01(q2);
        ky1 = clip01(ky1); ky2 = clip01(ky2);

        // 只改写本次字形的裁剪命令。
        Gfx* w = (Gfx*)p;
        w->words.w0 = _SHIFTL((u32)G_TEXCLIP_DJUI, 24, 8)
                    | _SHIFTL((u32)(q1  * 255.0f + 0.5f), 16, 8)
                    | _SHIFTL((u32)(ky1 * 255.0f + 0.5f),  8, 8);
        w->words.w1 = _SHIFTL((u32)(q2  * 255.0f + 0.5f), 16, 8)
                    | _SHIFTL((u32)(ky2 * 255.0f + 0.5f),  8, 8);
    }
}

/* 在不产生几何时消费待处理的纹理裁剪状态。 */
static void glyph_consume_pending_clip(void) {
    // 使用恒等裁剪。
    glyph_fix_pending_clip(0.0f, DJUI_MODERN_FONT_CELL_W, 0.0f, sCellHeight);
    *(gDisplayListHead++) = (Gfx) gsSPExecuteDjui(G_TEXCLIP_DJUI);
    sGlyphDlMark = gDisplayListHead;
}

/* Emoji 图集的运行时 RGBA32 缓冲。 */
#if defined(DJUI_MODERN_EMOJI_COUNT)
static const u8* sEmojiAtlasRGBA = NULL;
static u8* sEmojiAtlasOwned = NULL;
#endif

/* 国旗图集缓冲，布局与 Emoji 图集一致。 */
#if defined(DJUI_MODERN_FLAG_COUNT)
static const u8* sFlagAtlasRGBA = NULL;
static u8* sFlagAtlasOwned = NULL;
#endif

/* 前向声明。 */
static s32  flag_tile_at(const char* c);
static bool flag_consumes(const char* c);
#if defined(DJUI_MODERN_EMOJI_COUNT)
static void emoji_atlas_prepare(void);
#endif
#if defined(DJUI_MODERN_FLAG_COUNT)
static void flag_atlas_prepare(void);
#endif

/* 国旗配对状态。 */
#define FLAG_KIND_NONE 0
#define FLAG_KIND_HEAD 1
#define FLAG_KIND_TAIL 2

#if defined(DJUI_MODERN_FLAG_COUNT)
static const char* sFlagCurPtr  = NULL;   // 上一次求值的字符指针（幂等缓存键）
static s32         sFlagCurKind = FLAG_KIND_NONE;
static const char* sFlagPrevEnd = NULL;   // 上一个字符的结束指针（判连续性）
static u32         sFlagPrevCp  = 0;      // 上一个字符的码点
static bool        sFlagPrevWasHead = false;
#endif

// 统计
static u32   sGlyphRasterized = 0;
static u32   sGlyphBytes = 0;
static bool  sBudgetWarned = false;

// 渲染缩放提示栈，单位为屏幕像素。

#define SCALE_HINT_STACK_MAX 32
static f32 sScaleHintFont[SCALE_HINT_STACK_MAX];
static s32 sScaleHintTop = 0;
static f32 sScaleHintCurrentFont = 0.0f;   // 0 = 尚未设置
static f32 sScaleHintCurrentHud  = 1.0f;

#define kFallbackEmPixels 32.0f

// 工具

static inline u16 read_be_u16(const u8* p) {
    return (u16)((p[0] << 8) | p[1]);
}

u32 djui_modern_font_utf8_decode(const char* s, s32* bytes) {
    const u8* p = (const u8*)s;
    if (p[0] < 0x80) { if (bytes) { *bytes = 1; } return p[0]; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        if (bytes) { *bytes = 2; }
        return ((u32)(p[0] & 0x1F) << 6) | (u32)(p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        if (bytes) { *bytes = 3; }
        return ((u32)(p[0] & 0x0F) << 12) | ((u32)(p[1] & 0x3F) << 6) | (u32)(p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        if (bytes) { *bytes = 4; }
        return ((u32)(p[0] & 0x07) << 18) | ((u32)(p[1] & 0x3F) << 12) | ((u32)(p[2] & 0x3F) << 6) | (u32)(p[3] & 0x3F);
    }
    if (bytes) { *bytes = 1; }
    return p[0];
}

bool codepoint_is_cjk(u32 cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF);
}

/* 将 Coopdx 的星星控制字符 0x7F 重定向到 U+2605。 */
static inline u32 codepoint_remap(u32 cp) {
    return (cp == 0x7F) ? 0x2605u : cp;
}

/* 取 >= v 的最小 2 的幂，并钳在 [GLYPH_TEX_MIN, GLYPH_TEX_MAX]。 */
static s32 pot_ceil(s32 v) {
    s32 p = GLYPH_TEX_MIN;
    while (p < v && p < GLYPH_TEX_MAX) { p <<= 1; }
    return p;
}

static s32 log2_of(s32 pot) {
    s32 n = 0;
    while ((1 << n) < pot) { n++; }
    return n;
}

// 字符集构建

/* 已收录码点去重位图。 */
static u8 sCharsetSeen[0x110000 / 8];

/* 与 sCharset 一一对应：每个码点由哪一份字体提供（决定度量与光栅用哪份）。 */
static u8 sCharsetFace[CHARSET_MAX];

/* 收录一个码点；face < 0 时自动选择字体。 */
static bool charset_add_face(u32 cp, s32 face) {
    if (cp >= 0x110000u) { return false; }
    const u32 byte = cp >> 3;
    const u8  bit  = (u8)(1u << (cp & 7u));
    if (sCharsetSeen[byte] & bit) { return true; }   // 已收录（去重）
    if (face < 0) { face = font_face_for(cp); }
    if (face < 0) { return false; }                  // 没有任何字体含这个字
    if (sCharsetCount >= CHARSET_MAX) { return false; }
    sCharsetSeen[byte] |= bit;
    sCharsetFace[sCharsetCount] = (u8)face;
    sCharset[sCharsetCount++] = cp;
    return true;
}

static int charset_cmp(const void* a, const void* b) {
    const u32 x = *(const u32*)a;
    const u32 y = *(const u32*)b;
    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

/* 扫描 [lo, hi]，把字体里实际存在的码点收进字符集。 */
static void charset_scan_range(u32 lo, u32 hi) {
    for (u32 cp = lo; cp <= hi; cp++) {
        if (sCharsetCount >= CHARSET_MAX) { return; }
        charset_add_face(cp, -1);   // 自动挑字体；没有字体含它就跳过
    }
}

/* 上游 GB2312 字表的本地只读副本。 */
typedef struct {
    char unicode[4];
    char base;
    f32  width;
    u32  spriteIndex;
} DjuiModernCnGlyph;

static const DjuiModernCnGlyph kCnGlyphs[] = {
#include "pc/djui/cn_glyphs.h"
};

/* 把"游戏真正会用到的汉字"收进字符集（字体没有的跳过）。 */
static void charset_add_needed_cn(void) {
    const s32 n = (s32)(sizeof(kCnGlyphs) / sizeof(kCnGlyphs[0]));
    for (s32 i = 0; i < n; i++) {
        const char* u = kCnGlyphs[i].unicode;
        if (u[0] == '\0') { continue; }
        s32 bytes = 1;
        const u32 cp = djui_modern_font_utf8_decode(u, &bytes);
        if (cp < 128) { continue; }        // ASCII 已单独收
        charset_add_face(cp, -1);          // 自动挑字体；没有字体含它就跳过
    }
}

/* 构建字符集。先收录游戏文本常用字符，再扫描其余范围。 */
static void charset_build(void) {
    memset(sCharsetSeen, 0, sizeof(sCharsetSeen));
    for (s32 i = 0; i < 128; i++) { sAsciiIndex[i] = -1; }

    // 下标 0 留给回退字形；实际码点从 1 开始放。
    sCharsetCount = 1;
    sCharset[0] = '?';
    sCharsetFace[0] = (u8)((font_face_for('?') >= 0) ? font_face_for('?') : 0);

    // ASCII。
    charset_scan_range(0x20, 0x7E);

    // 游戏使用的汉字。
    charset_add_needed_cn();

    // 五角星可由程序生成，因此无条件收录。
    if (!charset_add_face(0x2605, -1)) { charset_add_face(0x2605, 0); }

    // 其余 Unicode 范围。
    charset_scan_range(0x4E00, 0x9FFF);
    charset_scan_range(0xA0, 0x4DFF);
    charset_scan_range(0xA000, 0xFFFD);

    // charset_lookup 依赖升序数组。
    if (sCharsetCount > 2) {
        qsort(&sCharset[1], (size_t)(sCharsetCount - 1), sizeof(u32), charset_cmp);
    }

    // 重建 ASCII 索引。
    for (s32 i = 1; i < sCharsetCount; i++) {
        if (sCharset[i] < 128) { sAsciiIndex[sCharset[i]] = i; }
    }

    s32 cjk = 0;
    for (s32 i = 1; i < sCharsetCount; i++) {
        if (codepoint_is_cjk(sCharset[i])) { cjk++; }
    }
    LOG_INFO("djui_modern: charset built (%d rows, %d cjk, capped at %d)",
             sCharsetCount, cjk, CHARSET_MAX);
}

/* 返回 codepoint 在字符集里的下标。ASCII 走 O(1) 快速表；其余走二分查找。
 * 找不到返回 FALLBACK_INDEX（0，画 '?'）。 */
static s32 charset_lookup(u32 cp) {
    if (cp < 128) {
        const s32 idx = sAsciiIndex[cp];
        return (idx >= 0) ? idx : FALLBACK_INDEX;
    }
    s32 lo = 1, hi = sCharsetCount - 1;
    while (lo <= hi) {
        const s32 mid = (lo + hi) >> 1;
        const u32 v = sCharset[mid];
        if (v == cp) { return mid; }
        if (v < cp) { lo = mid + 1; } else { hi = mid - 1; }
    }
    return FALLBACK_INDEX;
}

// 字形纹理（每字形一张，不可变）

/* 程序化生成 U+2605 五角星；字体缺少该字形时使用。 */
#define STAR_OUTER_INNER_RATIO 0.381966f

/* 点在凸/凹多边形内（射线法，支持凹多边形，五角星是凹的）。 */
static bool point_in_polygon(f32 px, f32 py, const f32* xs, const f32* ys, s32 n) {
    bool inside = false;
    for (s32 i = 0, j = n - 1; i < n; j = i++) {
        if (((ys[i] > py) != (ys[j] > py)) &&
            (px < (xs[j] - xs[i]) * (py - ys[i]) / (ys[j] - ys[i] + 1e-9f) + xs[i])) {
            inside = !inside;
        }
    }
    return inside;
}

/* 生成五角星覆盖度位图。 */
static u8* star_build_bitmap(f32 scale, s32* outW, s32* outH, s32* outXoff, s32* outYoff) {
    // em 像素高度（scale 是像素/单位，em = upem * scale）
    const f32 emPx = sUnitsPerEm * scale;
    s32 size = (s32)(emPx * 0.78f + 0.5f);
    if (size < 4) { size = 4; }
    if (size > GLYPH_TEX_MAX) { size = GLYPH_TEX_MAX; }

    const f32 cx = size * 0.5f;
    const f32 cy = size * 0.5f;
    const f32 R  = size * 0.5f - 1.0f;          // 外顶点半径（留 1px 边距给抗锯齿）
    const f32 r  = R * STAR_OUTER_INNER_RATIO;  // 内顶点半径

    f32 xs[10], ys[10];
    for (s32 i = 0; i < 10; i++) {
        const f32 ang = -1.5707963f + (f32)i * 0.6283185f;   // -90° + i*36°
        const f32 rad = (i & 1) ? r : R;
        xs[i] = cx + cosf(ang) * rad;
        ys[i] = cy + sinf(ang) * rad;
    }

    u8* bmp = (u8*)malloc((size_t)size * (size_t)size);
    if (bmp == NULL) { return NULL; }

    const s32 SS = 4;   // 每个像素 4x4 超采样
    for (s32 y = 0; y < size; y++) {
        for (s32 x = 0; x < size; x++) {
            s32 hits = 0;
            for (s32 sy = 0; sy < SS; sy++) {
                for (s32 sx = 0; sx < SS; sx++) {
                    const f32 px = (f32)x + ((f32)sx + 0.5f) / (f32)SS;
                    const f32 py = (f32)y + ((f32)sy + 0.5f) / (f32)SS;
                    if (point_in_polygon(px, py, xs, ys, 10)) { hits++; }
                }
            }
            bmp[y * size + x] = (u8)((hits * 255) / (SS * SS));
        }
    }

    *outW = size;
    *outH = size;
    // 水平居中，顶边对齐到约 0.72em。
    *outXoff = (s32)(emPx * 0.5f) - size / 2;
    *outYoff = -(s32)(emPx * 0.72f);
    return bmp;
}

/* 将覆盖度位图转换为 RGBA32 纹理。 */
static bool glyph_make_texture(GlyphSlot* g, const u8* bmp, s32 bw, s32 bh) {
    const s32 tw = pot_ceil(bw);
    const s32 th = pot_ceil(bh);
    // 纹理必须能容纳完整位图。
    if (tw < bw || th < bh || tw > GLYPH_TEX_MAX || th > GLYPH_TEX_MAX) {
        return false;
    }

    const u32 bytes = (u32)tw * (u32)th * 4u;
    if (sGlyphBytes + bytes > GLYPH_BYTES_LIMIT) {
        if (!sBudgetWarned) {
            sBudgetWarned = true;
            LOG_ERROR("djui_modern: glyph texture budget exhausted (%u MB), further glyphs dropped",
                      GLYPH_BYTES_LIMIT / (1024u * 1024u));
        }
        return false;
    }

    u8* tex = (u8*)malloc(bytes);
    if (tex == NULL) { return false; }

    // 未使用的纹理区域保持透明。
    memset(tex, 0, bytes);

    for (s32 y = 0; y < bh; y++) {
        const u8* src = bmp + (size_t)y * bw;
        u8* dst = tex + ((size_t)y * tw) * 4;
        for (s32 x = 0; x < bw; x++) {
            dst[0] = 255; dst[1] = 255; dst[2] = 255; dst[3] = src[x];
            dst += 4;
        }
    }

    g->tex     = tex;
    g->tw      = tw;
    g->th      = th;
    g->twLog2  = log2_of(tw);
    g->thLog2  = log2_of(th);
    g->bw      = bw;
    g->bh      = bh;
    sGlyphBytes += bytes;
    return true;
}

/* 按需生成字形纹理；内容生成后不再修改。 */
static void slot_rasterize(GlyphSlot* g, u32 cp, s32 bucket, s32 face) {
    if (g->state != 0) { return; }

    // 使用该字形所属字体。
    sCur = &sFaces[(face >= 0 && face < sFaceCount) ? face : 0];

    const s32 gi = stbtt_FindGlyphIndex(&sFont, (s32)cp);
    if (gi == 0) {
        // 字体缺少字形时使用程序化五角星或空字形。
        if (cp == 0x2605) {
            const f32 cjkBoost = gDjuiModernConfig.fontRasterBoostCJK;
            g->rasterEm = (f32)bucket * cjkBoost;
            const f32 s = g->rasterEm / sUnitsPerEm;
            s32 bw = 0, bh = 0, xo = 0, yo = 0;
            u8* bmp = star_build_bitmap(s, &bw, &bh, &xo, &yo);
            if (bmp != NULL && bw > 0 && bh > 0) {
                g->advanceEm = 1.0f;
                g->xoff = xo;
                g->yoff = yo;
                if (glyph_make_texture(g, bmp, bw, bh)) {
                    g->state = 1;
                    sGlyphRasterized++;
                    free(bmp);
                    return;
                }
                free(bmp);
            }
            g->bw = 0; g->bh = 0;
            g->state = 1;
            return;
        }
        g->state = 1;
        return;
    }

    const f32 cjkBoost = codepoint_is_cjk(cp) ? gDjuiModernConfig.fontRasterBoostCJK : 1.0f;
    g->rasterEm = (f32)bucket * cjkBoost;
    const f32 scale = g->rasterEm / sUnitsPerEm;

    s32 advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&sFont, gi, &advance, &lsb);
    g->advanceEm = (f32)advance / sUnitsPerEm;

    s32 bw = 0, bh = 0;
    u8* bmp = stbtt_GetGlyphBitmap(&sFont, scale, scale, gi, &bw, &bh, &g->xoff, &g->yoff);
    if (bmp == NULL || bw <= 0 || bh <= 0) {
        g->bw = 0; g->bh = 0;
        g->state = 1;
        if (bmp != NULL) { stbtt_FreeBitmap(bmp, NULL); }
        return;
    }

    if (!glyph_make_texture(g, bmp, bw, bh)) {
        stbtt_FreeBitmap(bmp, NULL);
        g->bw = 0; g->bh = 0;
        g->state = 2;
        return;
    }

    g->state = 1;
    sGlyphRasterized++;
    stbtt_FreeBitmap(bmp, NULL);
}

// 字体数据加载

/* 用一段已经在内存里的字体数据初始化一个 face。 */
static bool font_face_setup(FontFace* face, u8* data, u32 size, bool own, const char* name) {
    if (face == NULL || data == NULL) { return false; }

    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&face->info, data, offset)) {
        if (own) { free(data); }
        return false;
    }

    face->data = data;
    face->size = size;
    face->owned = own;
    face->name = name;

    f32 upem = (f32)read_be_u16(data + face->info.head + 18);
    if (upem < 16.0f || upem > 16384.0f) { upem = 1000.0f; }
    face->unitsPerEm = upem;
    return true;
}

/* 从磁盘读一个字体文件并加入字体链。 */
static bool font_add_path(const char* path) {
    if (path == NULL || path[0] == '\0') { return false; }
    if (sFaceCount >= FONT_MAX) { return false; }

    FILE* f = fopen(path, "rb");
    if (f == NULL) { return false; }

    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return false; }

    u8* data = (u8*)malloc((size_t)sz);
    if (data == NULL) { fclose(f); return false; }

    const size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(data); return false; }

    if (!font_face_setup(&sFaces[sFaceCount], data, (u32)sz, true, path)) { return false; }
    sFaceCount++;
    return true;
}

/* 路径是否以 .ttf / .otf 结尾（大小写不敏感）。 */
static bool path_is_font(const char* p) {
    const size_t n = (p != NULL) ? strlen(p) : 0;
    if (n < 5) { return false; }
    const char* ext = p + n - 4;
    return (strcasecmp(ext, ".ttf") == 0) || (strcasecmp(ext, ".otf") == 0);
}

typedef struct {
    char best[SYS_MAX_PATH];   // 名字最小的那个候选
    bool found;
} FontScan;

static bool font_scan_cb(void* user, const char* path) {
    FontScan* s = (FontScan*)user;
    if (path_is_font(path) && (!s->found || strcmp(path, s->best) < 0)) {
        snprintf(s->best, sizeof(s->best), "%s", path);
        s->found = true;
    }
    return true;   // 必须继续遍历完，才能确定"名字最小"的那个
}

/*
 * 在游戏目录中查找字体。
 * 优先使用 djui_modern/，其次使用游戏根目录；多个字体按文件名排序。
 */
static bool font_try_dir(const char* dir) {
    if (dir == NULL || dir[0] == '\0') { return false; }
    if (!fs_sys_dir_exists(dir)) { return false; }

    FontScan scan;
    memset(&scan, 0, sizeof(scan));
    fs_sys_walk(dir, font_scan_cb, &scan, false);
    if (!scan.found) { return false; }
    return font_add_path(scan.best);
}

/*
 * 装配字体链：font_path、游戏目录字体、内嵌西文与中文字体。
 * 外部字体位于链首，内嵌字体用于缺字回退。
 */
static void font_load_chain(void) {
    bool haveExternal = false;

    if (gDjuiModernConfig.fontPath[0] != '\0') {
        haveExternal = font_add_path(gDjuiModernConfig.fontPath);
        if (!haveExternal) {
            LOG_ERROR("djui_modern: font_path \"%s\" not readable, falling back", gDjuiModernConfig.fontPath);
        }
    }

    if (!haveExternal) {
        const char* exeDir = sys_exe_path_dir();
        if (exeDir != NULL && exeDir[0] != '\0') {
            char dir[SYS_MAX_PATH];
            // 优先使用 djui_modern 文件夹。
            if (snprintf(dir, sizeof(dir), "%s/djui_modern", exeDir) > 0 && font_try_dir(dir)) {
                haveExternal = true;
            // 其次使用游戏根目录。
            } else if (font_try_dir(exeDir)) {
                haveExternal = true;
            }
        }
    }
    if (haveExternal) {
        LOG_INFO("djui_modern: external font registered first; embedded chain appended as fallback");
    }

    // 内嵌字体作为回退链。
#if defined(DJUI_MODERN_LATIN_FONT_PRESENT)
    if (sFaceCount < FONT_MAX &&
        font_face_setup(&sFaces[sFaceCount], (u8*)sDjuiModernLatinFont,
                        sDjuiModernLatinFontSize, false, DJUI_MODERN_LATIN_FONT_NAME)) {
        sFaceCount++;
    }
#endif
#if defined(DJUI_MODERN_CJK_FONT_PRESENT)
    if (sFaceCount < FONT_MAX &&
        font_face_setup(&sFaces[sFaceCount], (u8*)sDjuiModernCjkFont,
                        sDjuiModernCjkFontSize, false, DJUI_MODERN_CJK_FONT_NAME)) {
        sFaceCount++;
    }
#endif
}

// 公共 API

/* 释放字体链里所有 face。 */
static void font_free_all(void) {
    for (s32 i = 0; i < sFaceCount; i++) {
        if (sFaces[i].owned && sFaces[i].data != NULL) { free(sFaces[i].data); }
        sFaces[i].data = NULL;
        sFaces[i].size = 0;
        sFaces[i].owned = false;
    }
    sFaceCount = 0;
    sCur = NULL;
}

bool djui_modern_font_atlas_init(void) {
    if (sReady) { return true; }

    font_load_chain();
    if (sFaceCount == 0) {
        LOG_ERROR("djui_modern: no usable TTF/OTF font found, keeping bitmap fonts");
        return false;
    }

    // 基线与行高使用字体链首的度量。
    sCur = &sFaces[0];
    detect_latin_monospace();

    s32 ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&sFont, &ascent, &descent, &lineGap);
    sAscentEm  = (f32)ascent  / sUnitsPerEm;
    sDescentEm = (f32)descent / sUnitsPerEm;

    f32 desc = -sDescentEm;
    if (desc < 0.0f) { desc = 0.0f; }
    if (desc > 0.15f) { desc = 0.15f; }
    sBaselineY = -(1.0f - desc);

    // 计算聊天框硬裁剪需要保留的下伸余量。
    {
        const f32 descUsed = 1.0f + sBaselineY;
        const f32 descNeed = -sDescentEm;
        sClipPadBottomEm = (descNeed > descUsed) ? (descNeed - descUsed) : 0.0f;
    }

    charset_build();

    sSlotRows = sCharsetCount;
    sSlotGrid = (GlyphSlot**)calloc((size_t)CHARSET_MAX, sizeof(GlyphSlot*));
    if (sSlotGrid == NULL) {
        LOG_ERROR("djui_modern: slot grid alloc failed (%d rows)", sSlotRows);
        font_free_all();
        return false;
    }

    // 回退槽使用 '?'。
    {
        const s32 qIdx = charset_lookup('?');
        if (qIdx > 0) { sCharset[FALLBACK_INDEX] = sCharset[qIdx]; }
        else          { sCharset[FALLBACK_INDEX] = '?'; }
    }

    sGlyphRasterized = 0;
    sGlyphBytes = 0;
    sBudgetWarned = false;
    sBoundTex = NULL;

#if defined(DJUI_MODERN_EMOJI_COUNT)
    // 解码 Emoji 图集。
    emoji_atlas_prepare();
#endif
#if defined(DJUI_MODERN_FLAG_COUNT)
    // 解码国旗图集。
    flag_atlas_prepare();
#endif

    // 生成字体链名称。
    {
        sFontName[0] = '\0';
        for (s32 i = 0; i < sFaceCount; i++) {
            const char* nm = (sFaces[i].name != NULL) ? sFaces[i].name : "?";
            const char* slash = strrchr(nm, '/');
            const char* bslash = strrchr(nm, '\\');
            if (bslash != NULL && (slash == NULL || bslash > slash)) { slash = bslash; }
            if (slash != NULL) { nm = slash + 1; }
            if (i > 0) { strncat(sFontName, " + ", sizeof(sFontName) - strlen(sFontName) - 1); }
            strncat(sFontName, nm, sizeof(sFontName) - strlen(sFontName) - 1);
        }
    }

    sReady = true;
    LOG_INFO("djui_modern: font chain ready (%d face(s): %s)",
             sFaceCount, sFontName);
    LOG_INFO("djui_modern: metrics from face0 (unitsPerEm=%.0f, ascent=%.3fem, descent=%.3fem, baseline=%.2f, clipPad=%.3fem)",
             sUnitsPerEm, sAscentEm, sDescentEm, sBaselineY, sClipPadBottomEm);
    LOG_INFO("djui_modern: font slots = %d chars x %d buckets, one immutable texture per glyph",
             sSlotRows, BUCKET_COUNT);
    // 输出惰性槽位表和顶点池的内存上限。
    LOG_INFO("djui_modern: heap budget: slot grid lazy %d x %d x %u B (<= %.1f MB if all used), "
             "vtx pool %.0f KB",
             sSlotRows, BUCKET_COUNT, (unsigned)sizeof(GlyphSlot),
             (double)sSlotRows * BUCKET_COUNT * sizeof(GlyphSlot) / (1024.0 * 1024.0),
             (double)(sizeof(Vtx) * GLYPH_VERTS * GLYPH_POOL_BLOCKS) / 1024.0);

    // 输出字符覆盖抽检信息。
    {
        static const u32 kProbe[] = {
            '1', '5', 'A', 'g', '?',
            0x4F60, 0x597D, 0x623F, 0x95F4, 0x963F, 0x7684, 0x300C,
            0x2605,
        };
        char buf[320] = { 0 };
        for (size_t i = 0; i < sizeof(kProbe) / sizeof(kProbe[0]); i++) {
            const s32 fi = font_face_for(kProbe[i]);
            const s32 gi = (fi >= 0) ? stbtt_FindGlyphIndex(&sFaces[fi].info, (s32)kProbe[i]) : 0;
            const s32 ci = charset_lookup(kProbe[i]);
            char piece[44];
            snprintf(piece, sizeof(piece), " U+%04X:g%u/i%d/f%d", kProbe[i], (unsigned)gi, ci, fi);
            strncat(buf, piece, sizeof(buf) - strlen(buf) - 1);
        }
        LOG_INFO("djui_modern: charset probe (g=glyph, i=charset index, f=font face, -1=absent)%s", buf);
    }
}

void djui_modern_font_atlas_shutdown(void) {
    // 释放字形纹理与惰性槽位表。
    if (sSlotGrid != NULL) {
        LOG_INFO("djui_modern: slot grid actually used %d / %d rows (%.0f KB)",
                 sSlotRowsUsed, sSlotRows,
                 (double)sSlotRowsUsed * BUCKET_COUNT * sizeof(GlyphSlot) / 1024.0);
        for (s32 ci = 0; ci < sSlotRows; ci++) {
            GlyphSlot* row = sSlotGrid[ci];
            if (row == NULL) { continue; }
            for (s32 bi = 0; bi < BUCKET_COUNT; bi++) {
                if (row[bi].tex != NULL) { free(row[bi].tex); row[bi].tex = NULL; }
            }
            free(row);
        }
        free(sSlotGrid);
    }
    sSlotGrid = NULL;
    sSlotRows = 0;
    sSlotRowsUsed = 0;
    sCharsetCount = 0;

    font_free_all();
    sReady = false;

    sScaleHintTop = 0;
    sScaleHintCurrentFont = 0.0f;
    sScaleHintCurrentHud  = 1.0f;

#if defined(DJUI_MODERN_EMOJI_COUNT)
    // 释放 Emoji 图集缓冲。
    if (sEmojiAtlasOwned != NULL) { free(sEmojiAtlasOwned); sEmojiAtlasOwned = NULL; }
    sEmojiAtlasRGBA = NULL;
#endif
#if defined(DJUI_MODERN_FLAG_COUNT)
    if (sFlagAtlasOwned != NULL) { free(sFlagAtlasOwned); sFlagAtlasOwned = NULL; }
    sFlagAtlasRGBA = NULL;
#endif

    if (sVtxPool != NULL) { free(sVtxPool); sVtxPool = NULL; }
    sVtxUsed = 0;
    sBoundTex = NULL;

    sGlyphRasterized = 0;
    sGlyphBytes = 0;
    sBudgetWarned = false;
}

/* 每帧第一遍重置顶点游标和纹理绑定状态。 */
void djui_modern_font_atlas_frame_begin(void) {
    sVtxUsed = 0;
    sBoundTex = NULL;
#if defined(DJUI_MODERN_FLAG_COUNT)
    // 清除跨帧国旗配对状态。
    sFlagCurPtr = NULL;
    sFlagCurKind = FLAG_KIND_NONE;
    sFlagPrevEnd = NULL;
    sFlagPrevCp = 0;
    sFlagPrevWasHead = false;
#endif
}

/* 第二遍渲染只追加顶点，不清零顶点池。 */
void djui_modern_font_atlas_pass_begin(void) {
    // 有意为空
}

bool djui_modern_font_atlas_ready(void) { return sReady; }
const char* djui_modern_font_atlas_name(void) { return sFontName; }

void djui_modern_font_atlas_stats(u32* atlasPixels, u32* glyphCount, u32* uploads) {
    if (atlasPixels) { *atlasPixels = sGlyphBytes; }
    if (glyphCount)  { *glyphCount  = sGlyphRasterized; }
    if (uploads)     { *uploads     = sGlyphRasterized; }
}

// 字号 -> 档位

static f32 hint_normalize(f32 emPixels, f32 hudScaleX) {
    if (hudScaleX <= 0.0f) { return 1.0f; }
    return (emPixels > 0.0f) ? emPixels : 1.0f;
}

void djui_modern_font_atlas_push_scale_hint(f32 emPixels, f32 hudScaleX) {
    const f32 norm = hint_normalize(emPixels, hudScaleX);
    if (sScaleHintTop >= SCALE_HINT_STACK_MAX) {
        sScaleHintCurrentFont = emPixels;
        sScaleHintCurrentHud  = norm;
        return;
    }
    sScaleHintFont[sScaleHintTop] = emPixels;
    sScaleHintTop++;
    sScaleHintCurrentFont = emPixels;
    sScaleHintCurrentHud  = norm;
}

void djui_modern_font_atlas_pop_scale_hint(void) {
    if (sScaleHintTop > 0) { sScaleHintTop--; }
    if (sScaleHintTop > 0) {
        sScaleHintCurrentFont = sScaleHintFont[sScaleHintTop - 1];
    } else {
        sScaleHintCurrentFont = 0.0f;
        sScaleHintCurrentHud  = 1.0f;
    }
}

f32 djui_modern_font_atlas_get_scale_hint(void) {
    if (sScaleHintCurrentFont > 0.0f) { return sScaleHintCurrentHud; }
    return kFallbackEmPixels;
}

/* 字形光栅超采样系数。 */
#define kGlyphSupersample 1.25f

/* 将屏幕 em 像素映射到光栅档位。 */
static s32 bucket_index_for_em(f32 em) {
    const f32 want = em * kGlyphSupersample;
    for (s32 i = 0; i < BUCKET_COUNT; i++) {
        if ((f32)kRasterBuckets[i] >= want) { return i; }
    }
    return BUCKET_COUNT - 1;
}

// 宽度查询

/* 字偶间距：字符宽度包含与下一字符之间的调整量。 */

/* 可选字偶间距兜底表。只在字体缺少该对数据时使用。 */
static const struct { u32 a, b; s16 v; } kFallbackKern[] = {
    { 'T','o', -85 }, { 'T','a', -85 }, { 'T','e', -85 }, { 'T','u', -75 },
    { 'T','r', -55 }, { 'T','y', -85 }, { 'T','w', -65 }, { 'T','c', -65 },
    { 'T','s', -65 }, { 'T','A', -75 }, { 'T','.', -85 }, { 'T',',', -85 },
    { 'A','T', -75 }, { 'A','V', -85 }, { 'A','W', -75 }, { 'A','Y', -85 },
    { 'A','v', -55 }, { 'A','w', -45 }, { 'A','y', -55 },
    { 'V','A', -75 }, { 'V','a', -55 }, { 'V','e', -55 }, { 'V','o', -55 },
    { 'V','.', -75 }, { 'V',',', -75 },
    { 'W','A', -55 }, { 'W','a', -45 }, { 'W','.', -55 }, { 'W',',', -55 },
    { 'Y','A', -85 }, { 'Y','a', -65 }, { 'Y','e', -65 }, { 'Y','o', -75 },
    { 'Y','.', -85 }, { 'Y',',', -85 },
    { 'L','T', -95 }, { 'L','V', -75 }, { 'L','W', -65 }, { 'L','Y', -95 },
    { 'P','A', -65 }, { 'P','.', -65 }, { 'P',',', -65 },
    { 'F','A', -55 }, { 'F','.', -55 }, { 'F',',', -55 },
    { 'r','.', -35 }, { 'r',',', -35 },
    { 'y','.', -35 }, { 'y',',', -35 },
    { 'v','.', -35 }, { 'v',',', -35 },
};

/* 检测西文等宽字体；等宽字体不使用兜底字偶间距。 */
static bool sLatinMonospace = false;

static void detect_latin_monospace(void) {
    static const char kProbe[] = "iIlmMW1A.w";
    f32 mn = 1e9f, mx = -1e9f;
    s32 n = 0;
    if (sFaceCount == 0) { sLatinMonospace = false; return; }
    sCur = &sFaces[0];
    for (const char* p = kProbe; *p != '\0'; p++) {
        const s32 gi = stbtt_FindGlyphIndex(&sFont, (s32)*p);
        if (gi == 0) { continue; }
        s32 adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&sFont, gi, &adv, &lsb);
        if (adv <= 0) { continue; }
        const f32 em = (f32)adv / sUnitsPerEm;
        if (em < mn) { mn = em; }
        if (em > mx) { mx = em; }
        n++;
    }
    const f32 spread = (n >= 6) ? (mx - mn) : 1.0f;
    sLatinMonospace = (n >= 6) && (spread <= 0.02f);
    LOG_INFO("djui_modern: latin metrics %s (spread=%.4fem)%s",
             sLatinMonospace ? "uniform" : "proportional", spread,
             sLatinMonospace ? " -> fallback kerning disabled" : "");
}

static s16 fallback_kern_units(u32 cp1, u32 cp2) {
    const s32 n = (s32)(sizeof(kFallbackKern) / sizeof(kFallbackKern[0]));
    for (s32 i = 0; i < n; i++) {
        if (kFallbackKern[i].a == cp1 && kFallbackKern[i].b == cp2) { return kFallbackKern[i].v; }
    }
    return 0;
}

// 成对字偶间距（em 相对量，通常为负）。字体无 kern/GPOS 数据时走兜底表。
static f32 glyph_kern_em(u32 prev, u32 cur) {
    if (prev == 0 || cur == 0) { return 0.0f; }
    // 字偶间距只在同一份字体内部生效。
    const s32 fi = font_face_for(prev);
    if (fi < 0 || font_face_for(cur) != fi) { return 0.0f; }
    sCur = &sFaces[fi];

    const s32 g1 = stbtt_FindGlyphIndex(&sFont, (s32)prev);
    const s32 g2 = stbtt_FindGlyphIndex(&sFont, (s32)cur);
    if (g1 == 0 || g2 == 0) { return 0.0f; }
    const s32 k = stbtt_GetGlyphKernAdvance(&sFont, g1, g2);
    if (k != 0) { return (f32)k / sUnitsPerEm; }

    // 只在字体没有 GPOS/kern 数据时使用 ASCII 兜底表。
    const bool fontHasKern = (sCur->info.gpos != 0) || (sCur->info.kern != 0);
    if (!fontHasKern && gDjuiModernConfig.fontFallbackKerning && !sLatinMonospace && prev < 128 && cur < 128) {
        const s16 fk = fallback_kern_units(prev, cur);
        if (fk != 0) { return (f32)fk / 1000.0f; }
    }
    return 0.0f;
}

f32 djui_modern_font_atlas_char_width(const char* c, f32 fontScale) {
    if (!sReady || c == NULL || *c == '\0') { return 0.5f; }
    (void)fontScale;

    s32 bytes = 1;
    const u32 rawCp = djui_modern_font_utf8_decode(c, &bytes);
    const u32 cp = codepoint_remap(rawCp);

    // 空格使用字体原生 advance。
    if (cp == ' ') {
        const s32 sci = charset_lookup(cp);
        sCur = &sFaces[(sCharsetFace[sci] < sFaceCount) ? sCharsetFace[sci] : 0];
        const s32 gs = stbtt_FindGlyphIndex(&sFont, ' ');
        s32 sAdv = 0, sLsb = 0;
        if (gs != 0) { stbtt_GetGlyphHMetrics(&sFont, gs, &sAdv, &sLsb); }
        return (sAdv > 0) ? ((f32)sAdv / sUnitsPerEm) : 0.25f;
    }

    // Emoji 固定为 1em 宽。
    if (djui_modern_emoji_available(c)) { return 1.0f; }

    // 国旗头部占 1em，尾部占 0。
    if (flag_consumes(c)) { return (flag_tile_at(c) >= 0) ? 1.0f : 0.0f; }

    // 使用该字形所属字体。
    const s32 ci = charset_lookup(cp);
    sCur = &sFaces[(sCharsetFace[ci] < sFaceCount) ? sCharsetFace[ci] : 0];

    const s32 gi = stbtt_FindGlyphIndex(&sFont, (s32)cp);
    f32 adv;
    if (cp == 0x2605 && gi == 0) {
        // 程序化五角星固定为 1em 宽。
        adv = 1.0f;
    } else if (gi == 0) {
        // 缺字时使用回退字形宽度。
        const s32 q = stbtt_FindGlyphIndex(&sFont, '?');
        s32 advance = 0, lsb = 0;
        if (q != 0) { stbtt_GetGlyphHMetrics(&sFont, q, &advance, &lsb); }
        adv = (advance > 0) ? ((f32)advance / sUnitsPerEm)
                            : (codepoint_is_cjk(cp) ? 1.0f : 0.5f);
    } else {
        s32 advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&sFont, gi, &advance, &lsb);
        adv = (advance > 0) ? ((f32)advance / sUnitsPerEm)
                            : (codepoint_is_cjk(cp) ? 1.0f : 0.5f);
    }

    // 字偶调整量计入当前字符宽度。
    u32 nextCp = 0;
    const char* nc = c + bytes;
    if (*nc != '\0' && *nc != '\n' && *nc != '\r' && *nc != '\t') {
        s32 nb = 1;
        nextCp = codepoint_remap(djui_modern_font_utf8_decode(nc, &nb));
    }
    adv += glyph_kern_em(cp, nextCp);
    if (adv < 0.05f) { adv = 0.05f; }

    return adv;
}

// 绘制

static Vtx* vtx_alloc(void) {
    if (sVtxPool == NULL) {
        sVtxPool = malloc(sizeof(Vtx) * GLYPH_VERTS * GLYPH_POOL_BLOCKS);
        if (sVtxPool == NULL) { return NULL; }
        memset(sVtxPool, 0, sizeof(Vtx) * GLYPH_VERTS * GLYPH_POOL_BLOCKS);
    }
    if (sVtxUsed + GLYPH_VERTS > GLYPH_VERTS * GLYPH_POOL_BLOCKS) {
        // 异常路径下强制复用顶点池。
        static bool sWarned = false;
        if (!sWarned) {
            sWarned = true;
            LOG_ERROR("djui_modern: glyph vertex pool full (%d blocks) - resetting",
                      GLYPH_POOL_BLOCKS);
        }
        sVtxUsed = 0;
    }
    Vtx* v = &sVtxPool[sVtxUsed];
    sVtxUsed += GLYPH_VERTS;
    return v;
}

/* 字形使用 DJUI 的 2D 三角形路径。 */
static const Gfx sGlyphTemplate[] = {
    gsSPVertexNonGlobal(NULL, GLYPH_VERTS, 0),
    // 依次提交顶点、纹理裁剪和三角形。
    gsSPExecuteDjui(G_TEXCLIP_DJUI),
    gsSP2TrianglesDjui(0, 1, 3, 0, 0, 3, 2, 0),
};

/* 绑定字形纹理；override 使用边长 log2，纹理块使用纹素数。 */
static void bind_glyph_texture(const GlyphSlot* g) {
    gDPLoadTextureBlockWithoutTexture(gDisplayListHead++, NULL, G_IM_FMT_RGBA, G_IM_SIZ_16b,
        g->tw, g->th, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    gDPSetTextureOverrideDjui(gDisplayListHead++, g->tex,
        g->twLog2, g->thLog2, G_IM_FMT_RGBA, G_IM_SIZ_32b);
    *(gDisplayListHead++) = (Gfx) gsSPExecuteDjui(G_TEXOVERRIDE_DJUI);
    sBoundTex = g->tex;
}

/* Emoji 使用离线生成的 RGBA 图集。 */

#if defined(DJUI_MODERN_EMOJI_COUNT)
/* 将内嵌 Emoji 图集解码为 RGBA32；失败时交回原版 Emoji。 */
static void emoji_atlas_prepare(void) {
#if DJUI_MODERN_EMOJI_ATLAS_PNG
    s32 w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load_from_memory(sDjuiModernEmojiAtlas,
                                        (int)DJUI_MODERN_EMOJI_ATLAS_SIZE,
                                        &w, &h, &ch, 4);
    if (px == NULL) {
        LOG_ERROR("djui_modern: emoji atlas PNG decode failed (%d bytes); "
                  "falling back to vanilla emoji", (int)DJUI_MODERN_EMOJI_ATLAS_SIZE);
        return;
    }
    if (w != DJUI_MODERN_EMOJI_DIM_X || h != DJUI_MODERN_EMOJI_DIM_Y) {
        LOG_ERROR("djui_modern: emoji atlas size mismatch (%dx%d, expected %dx%d)",
                  w, h, DJUI_MODERN_EMOJI_DIM_X, DJUI_MODERN_EMOJI_DIM_Y);
        stbi_image_free(px);
        return;
    }
    sEmojiAtlasOwned = px;
    sEmojiAtlasRGBA  = px;
    LOG_INFO("djui_modern: emoji atlas ready (%dx%d, %d tiles)",
             w, h, DJUI_MODERN_EMOJI_COUNT);
#else
    // 直接使用内嵌 RGBA32 数据。
    sEmojiAtlasRGBA = sDjuiModernEmojiAtlas;
#endif
}
#endif

#if defined(DJUI_MODERN_FLAG_COUNT)
/* 解码国旗图集；失败时交回普通字形路径。 */
static void flag_atlas_prepare(void) {
#if DJUI_MODERN_FLAG_ATLAS_PNG
    s32 w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load_from_memory(sDjuiModernFlagAtlas,
                                        (int)DJUI_MODERN_FLAG_ATLAS_SIZE,
                                        &w, &h, &ch, 4);
    if (px == NULL) {
        LOG_ERROR("djui_modern: flag atlas PNG decode failed (%d bytes); "
                  "flags disabled", (int)DJUI_MODERN_FLAG_ATLAS_SIZE);
        return;
    }
    // 布局必须与 Emoji 图集一致。
    if (w != DJUI_MODERN_EMOJI_DIM_X || h != DJUI_MODERN_EMOJI_DIM_Y) {
        LOG_ERROR("djui_modern: flag atlas size mismatch (%dx%d, expected %dx%d)",
                  w, h, DJUI_MODERN_EMOJI_DIM_X, DJUI_MODERN_EMOJI_DIM_Y);
        stbi_image_free(px);
        return;
    }
    sFlagAtlasOwned = px;
    sFlagAtlasRGBA  = px;
    LOG_INFO("djui_modern: flag atlas ready (%dx%d, %d flags)",
             w, h, DJUI_MODERN_FLAG_COUNT);
#else
    sFlagAtlasRGBA = sDjuiModernFlagAtlas;
#endif
}
#endif

/* 码点 -> tile 下标（`sDjuiModernEmojiCodepoints[]` 升序，二分查找）；没有则 -1。 */
static s32 emoji_tile_for(u32 cp) {
#if defined(DJUI_MODERN_EMOJI_COUNT)
    s32 lo = 0, hi = DJUI_MODERN_EMOJI_COUNT - 1;
    while (lo <= hi) {
        const s32 mid = (lo + hi) >> 1;
        const u32 v = sDjuiModernEmojiCodepoints[mid];
        if (v == cp) { return mid; }
        if (v < cp) { lo = mid + 1; } else { hi = mid - 1; }
    }
#else
    (void)cp;
#endif
    return -1;
}

/*
 * 国旗由两个 Regional Indicator 组成。
 * 头部绘制整面旗，尾部不绘制；同一字符的重复查询使用缓存。
 */
#define RI_FIRST 0x1F1E6u
#define RI_LAST  0x1F1FFu

#if defined(DJUI_MODERN_FLAG_COUNT)
static bool is_regional_indicator(u32 cp) { return cp >= RI_FIRST && cp <= RI_LAST; }

/* 查询 RI 对对应的国旗 tile 下标。 */
static s32 flag_tile_for(u32 ri1, u32 ri2) {
    if (DJUI_MODERN_FLAG_COUNT == 0) { return -1; }
    if (!is_regional_indicator(ri1) || !is_regional_indicator(ri2)) { return -1; }
    const u16 key = (u16)((ri1 - RI_FIRST) * 26u + (ri2 - RI_FIRST));
    s32 lo = 0, hi = DJUI_MODERN_FLAG_COUNT - 1;
    while (lo <= hi) {
        const s32 mid = (lo + hi) >> 1;
        const u16 v = sDjuiModernFlagKeys[mid];
        if (v == key) { return mid; }
        if (v < key) { lo = mid + 1; } else { hi = mid - 1; }
    }
    return -1;
}

/* 这一对 RI 是否真有对应国旗。 */
static bool flag_pair_valid(u32 ri1, u32 ri2) { return flag_tile_for(ri1, ri2) >= 0; }

/* 求值：本字符在国旗序列里扮演什么角色（NONE / HEAD / TAIL）。幂等。 */
static s32 flag_kind_at(const char* c) {
    if (c == sFlagCurPtr) { return sFlagCurKind; }

    s32 b = 1;
    const u32 cp = djui_modern_font_utf8_decode(c, &b);

    // 仅处理连续字符。
    const bool adjacent = (sFlagPrevEnd != NULL) && (c == sFlagPrevEnd);

    s32 kind = FLAG_KIND_NONE;
    if (is_regional_indicator(cp)) {
        if (adjacent && sFlagPrevWasHead && flag_pair_valid(sFlagPrevCp, cp)) {
            kind = FLAG_KIND_TAIL;
        } else {
            const char* n = c + b;
            s32 nb = 1;
            if (*n != '\0' && flag_pair_valid(cp, djui_modern_font_utf8_decode(n, &nb))) {
                kind = FLAG_KIND_HEAD;
            }
            // 孤立 RI 不绘制。
        }
    }

    sFlagPrevEnd = c + b;
    sFlagPrevCp = cp;
    sFlagPrevWasHead = (kind == FLAG_KIND_HEAD);
    sFlagCurPtr = c;
    sFlagCurKind = kind;
    return kind;
}
#endif

/* 本字符是不是"国旗对的头"。 */
static bool flag_is_head(const char* c) {
#if defined(DJUI_MODERN_FLAG_COUNT)
    if (DJUI_MODERN_FLAG_COUNT == 0 || !gDjuiModernConfig.emojiModern) { return false; }
    if (sFlagAtlasRGBA == NULL) { return false; }
    return flag_kind_at(c) == FLAG_KIND_HEAD;
#else
    (void)c;
    return false;
#endif
}

/* 本字符是否由国旗逻辑接管（头或尾都要"吃掉"，不再走字形路径）。 */
static bool flag_consumes(const char* c) {
#if defined(DJUI_MODERN_FLAG_COUNT)
    if (DJUI_MODERN_FLAG_COUNT == 0 || !gDjuiModernConfig.emojiModern) { return false; }
    if (sFlagAtlasRGBA == NULL) { return false; }
    return flag_kind_at(c) != FLAG_KIND_NONE;
#else
    (void)c;
    return false;
#endif
}

/* 当前字符若是"国旗对的头"，返回 tile 下标；否则 -1。 */
static s32 flag_tile_at(const char* c) {
#if defined(DJUI_MODERN_FLAG_COUNT)
    if (!flag_is_head(c)) { return -1; }
    s32 b1 = 1;
    const u32 c1 = djui_modern_font_utf8_decode(c, &b1);
    const char* n = c + b1;
    if (*n == '\0') { return -1; }
    s32 b2 = 1;
    return flag_tile_for(c1, djui_modern_font_utf8_decode(n, &b2));
#else
    (void)c;
    return -1;
#endif
}

/* 这个字符是否由现代 emoji 图集负责绘制（否则交回原版）。 */
bool djui_modern_emoji_available(const char* c) {
#if defined(DJUI_MODERN_EMOJI_COUNT)
    if (!gDjuiModernConfig.emojiModern || c == NULL || *c == '\0') { return false; }
    if (sEmojiAtlasRGBA == NULL) { return false; }
    s32 bytes = 1;
    const u32 cp = djui_modern_font_utf8_decode(c, &bytes);
    return emoji_tile_for(cp) >= 0;
#else
    (void)c;
    return false;
#endif
}

/* 放行本模块可渲染的 Emoji 与国旗字符。 */
bool djui_modern_unicode_accepts(const char* text) {
#if defined(DJUI_MODERN_EMOJI_COUNT)
    if (text == NULL || *text == '\0') { return false; }
    if (!gDjuiModernConfig.emojiModern) { return false; }
    s32 bytes = 1;
    const u32 cp = djui_modern_font_utf8_decode(text, &bytes);
    if (cp == 0) { return false; }
    if (cp >= RI_FIRST && cp <= RI_LAST) { return true; }   // 国旗（Regional Indicator）
    return djui_modern_emoji_available(text);               // 现代 emoji 图集
#else
    (void)text;
    return false;
#endif
}

#if defined(DJUI_MODERN_EMOJI_COUNT)
/* 绘制图集中的一个 tile。 */
static void emoji_render_tile(const u8* atlas, s32 idx) {
    if (atlas == NULL) { return; }

    // 使用与字形相同的 1em 对象空间。
    const f32 x0 = 0.0f, x1 = 1.0f;
    const f32 y0 = sBaselineY + 0.80f;
    const f32 y1 = y0 - 1.0f;

    // 计算 tile 的 UV 范围。
    const s32 tx = idx % DJUI_MODERN_EMOJI_GRID_X;
    const s32 ty = idx / DJUI_MODERN_EMOJI_GRID_X;
    const u16 u0 = (u16)((u32)(tx * DJUI_MODERN_EMOJI_TILE) << 5);
    const u16 v0 = (u16)((u32)(ty * DJUI_MODERN_EMOJI_TILE) << 5);
    const u16 u1 = (u16)((u32)((tx + 1) * DJUI_MODERN_EMOJI_TILE) << 5);
    const u16 v1 = (u16)((u32)((ty + 1) * DJUI_MODERN_EMOJI_TILE) << 5);

    Vtx* vtx = vtx_alloc();
    if (vtx == NULL) { return; }

    for (s32 i = 0; i < GLYPH_VERTS; i++) {
        memset(&vtx[i], 0, sizeof(Vtx));
        vtx[i].v.cn[0] = 255;
        vtx[i].v.cn[1] = 255;
        vtx[i].v.cn[2] = 255;
        vtx[i].v.cn[3] = 255;
    }
    vtx[0].v.ob[0] = x0; vtx[0].v.ob[1] = y0; vtx[0].v.tc[0] = u0; vtx[0].v.tc[1] = v0;
    vtx[1].v.ob[0] = x1; vtx[1].v.ob[1] = y0; vtx[1].v.tc[0] = u1; vtx[1].v.tc[1] = v0;
    vtx[2].v.ob[0] = x0; vtx[2].v.ob[1] = y1; vtx[2].v.tc[0] = u0; vtx[2].v.tc[1] = v1;
    vtx[3].v.ob[0] = x1; vtx[3].v.ob[1] = y1; vtx[3].v.tc[0] = u1; vtx[3].v.tc[1] = v1;

    // 图集内容不可变；地址相同时跳过重复绑定。
    if (sBoundTex != atlas) {
        gDPLoadTextureBlockWithoutTexture(gDisplayListHead++, NULL, G_IM_FMT_RGBA, G_IM_SIZ_16b,
            DJUI_MODERN_EMOJI_DIM_X, DJUI_MODERN_EMOJI_DIM_Y, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
        gDPSetTextureOverrideDjui(gDisplayListHead++, (const void*)atlas,
            DJUI_MODERN_EMOJI_LOG2_X, DJUI_MODERN_EMOJI_LOG2_Y, G_IM_FMT_RGBA, G_IM_SIZ_32b);
        *(gDisplayListHead++) = (Gfx) gsSPExecuteDjui(G_TEXOVERRIDE_DJUI);
        sBoundTex = atlas;
    }

    // Emoji 四边形占满 1em 字格。
    glyph_fix_pending_clip(0.0f, DJUI_MODERN_FONT_CELL_W, y0, y0 - y1);

    Gfx* dl = gDisplayListHead;
    memcpy(dl, sGlyphTemplate, sizeof(sGlyphTemplate));
    dl[0].words.w1 = (uintptr_t)VIRTUAL_TO_PHYSICAL(vtx);
    gDisplayListHead += sizeof(sGlyphTemplate) / sizeof(Gfx);
    sGlyphDlMark = gDisplayListHead;
}
#endif

void djui_modern_font_atlas_render_begin(void) {
    if (!sReady) { return; }

    gSPClearGeometryMode(gDisplayListHead++, G_LIGHTING | G_CULL_BOTH);
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);

    // 文字颜色由 djui_text 设置的 ENV 提供。
    gDPSetCombineMode(gDisplayListHead++, G_CC_FADEA, G_CC_FADEA);
    gDPSetRenderMode(gDisplayListHead++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetTextureFilter(gDisplayListHead++, gDjuiModernConfig.fontBilinear ? G_TF_BILERP : G_TF_POINT);
    gDPSetTexturePersp(gDisplayListHead++, G_TP_NONE);
    gSPTexture(gDisplayListHead++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);

    // 每段文字都可能使用不同纹理，因此重置绑定状态。
    sBoundTex = NULL;

    // 纹理裁剪换算起点。
    sGlyphDlMark = gDisplayListHead;
}

void djui_modern_font_atlas_render_end(void) {
    if (!sReady) { return; }
    gSPTexture(gDisplayListHead++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);
    gDPSetTexturePersp(gDisplayListHead++, G_TP_PERSP);
    gSPSetGeometryMode(gDisplayListHead++, G_LIGHTING | G_CULL_BACK);
    sBoundTex = NULL;
}

void djui_modern_font_atlas_render_char(const char* c) {
    if (!sReady || sSlotGrid == NULL || c == NULL || *c == '\0') { return; }
    if (*c == ' ' || *c == '\n' || *c == '\t') { return; }

#if defined(DJUI_MODERN_EMOJI_COUNT)
    // 现代 Emoji。
    {
        s32 eb = 1;
        const s32 et = emoji_tile_for(djui_modern_font_utf8_decode(c, &eb));
        if (et >= 0 && gDjuiModernConfig.emojiModern) {
            emoji_render_tile(sEmojiAtlasRGBA, et);
            return;
        }
    }

    // 国旗。
    if (flag_consumes(c)) {
        const s32 ft = flag_tile_at(c);
        if (ft >= 0) {
            emoji_render_tile(sFlagAtlasRGBA, ft);
        } else {
            // 尾部或孤立 RI 不绘制，但需要消费裁剪状态。
            glyph_consume_pending_clip();
        }
        return;
    }
#endif

    s32 bytes = 1;
    // 与 char_width 使用相同的码点重定向。
    const u32 cp = codepoint_remap(djui_modern_font_utf8_decode(c, &bytes));
    if (cp == 0) { return; }

    // 字符集下标。
    const s32 ci = charset_lookup(cp);
    sCur = &sFaces[(sCharsetFace[ci] < sFaceCount) ? sCharsetFace[ci] : 0];

    // 光栅档位。
    f32 em = djui_modern_font_atlas_get_scale_hint() * gDjuiModernConfig.textScale;
    if (em < 1.0f) { em = 1.0f; }
    if (em > 4096.0f) { em = 4096.0f; }
    const s32 bi = bucket_index_for_em(em);
    const s32 bucket = kRasterBuckets[bi];

    // 获取或生成字形槽位。
    GlyphSlot* g = slot_at(ci, bi);
    if (g == NULL) {
        glyph_consume_pending_clip();
        return;
    }
    if (g->state == 0) {
        slot_rasterize(g, sCharset[ci], bucket, (s32)sCharsetFace[ci]);
    }
    if (g->state != 1 || g->bw <= 0 || g->bh <= 0 || g->tex == NULL) {
        // 无字形时不绘制，并消费裁剪状态。
        glyph_consume_pending_clip();
        return;
    }

    // 将位图像素换算为 em 相对几何。
    const f32 invRaster = 1.0f / ((g->rasterEm > 0.001f) ? g->rasterEm : (f32)bucket);

    const f32 x0 = (f32)g->xoff * invRaster;
    const f32 x1 = (f32)(g->xoff + g->bw) * invRaster;
    const f32 y0 = sBaselineY + (-(f32)g->yoff) * invRaster;
    const f32 y1 = sBaselineY - ((f32)(g->yoff + g->bh)) * invRaster;

    // 字形位图位于自纹理左上角。
    const u16 u0 = 0;
    const u16 u1 = (u16)((u32)g->bw << 5);
    const u16 v0 = 0;
    const u16 v1 = (u16)((u32)g->bh << 5);

    Vtx* vtx = vtx_alloc();
    if (vtx == NULL) { return; }

    for (s32 i = 0; i < 4; i++) {
        memset(&vtx[i], 0, sizeof(Vtx));
        vtx[i].v.cn[0] = 255;
        vtx[i].v.cn[1] = 255;
        vtx[i].v.cn[2] = 255;
        vtx[i].v.cn[3] = 255;
    }
    vtx[0].v.ob[0] = x0; vtx[0].v.ob[1] = y0; vtx[0].v.tc[0] = u0; vtx[0].v.tc[1] = v0;
    vtx[1].v.ob[0] = x1; vtx[1].v.ob[1] = y0; vtx[1].v.tc[0] = u1; vtx[1].v.tc[1] = v0;
    vtx[2].v.ob[0] = x0; vtx[2].v.ob[1] = y1; vtx[2].v.tc[0] = u0; vtx[2].v.tc[1] = v1;
    vtx[3].v.ob[0] = x1; vtx[3].v.ob[1] = y1; vtx[3].v.tc[0] = u1; vtx[3].v.tc[1] = v1;

    // 连续相同纹理跳过重复绑定。
    if (sBoundTex != g->tex) {
        bind_glyph_texture(g);
    }

    // 将待处理裁剪换算到墨迹盒坐标。
    glyph_fix_pending_clip(x0, x1 - x0, y0, y0 - y1);

    Gfx* dl = gDisplayListHead;
    memcpy(dl, sGlyphTemplate, sizeof(sGlyphTemplate));
    dl[0].words.w1 = (uintptr_t)VIRTUAL_TO_PHYSICAL(vtx);
    gDisplayListHead += sizeof(sGlyphTemplate) / sizeof(Gfx);
    sGlyphDlMark = gDisplayListHead;
}
