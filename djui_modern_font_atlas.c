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
#include <stb/stb_truetype.h>

// stb_image 的实现由 gfx_pc.c 提供。
#include <stb/stb_image.h>

// 内嵌字体、Emoji 和国旗数据均由 tools/ 下的脚本生成。
#if defined(__has_include)
#  if __has_include("djui_modern_font_latin_data.h")
#    include "djui_modern_font_latin_data.h"
#  endif
#  if __has_include("djui_modern_font_cjk_data.h")
#    include "djui_modern_font_cjk_data.h"
#  endif
#  if __has_include("djui_modern_font_star_data.h")
#    include "djui_modern_font_star_data.h"
#  endif
#  if __has_include("djui_modern_emoji_data.h")
#    include "djui_modern_emoji_data.h"
#  endif
#  if __has_include("djui_modern_flag_data.h")
#    include "djui_modern_flag_data.h"
#  endif
#endif

// UV 使用 S10.5 定点坐标，图集边长不能超过 1023。
#if defined(DJUI_MODERN_EMOJI_COUNT)
typedef char djui_modern_emoji_atlas_dim_ok[
    (DJUI_MODERN_EMOJI_DIM_X <= 1023 && DJUI_MODERN_EMOJI_DIM_Y <= 1023) ? 1 : -1];
#endif

/*
 * 每个字符和字号档位使用一张独立的二次幂 RGBA 纹理。
 * 纹理生成后不再修改，因此可安全依赖 gfx_pc 的指针缓存。
 */

///////////////////////////////////////////////////////////////////////////////
// 常量
///////////////////////////////////////////////////////////////////////////////

// 位图档位覆盖普通界面到高分辨率 HUD 大字。
static const s32 kRasterBuckets[] = { 12, 16, 20, 26, 32, 40, 52, 64, 88, 128, 192, 320, 512 };
#define BUCKET_COUNT ((s32)(sizeof(kRasterBuckets) / sizeof(kRasterBuckets[0])))

// 字符集只记录码点，位图按需生成。
#define CHARSET_MAX 8192

#define FALLBACK_INDEX 0

#define GLYPH_TEX_MIN 4
// 512 配合 S10.5 UV 编码仍留有一倍余量。
#define GLYPH_TEX_MAX 512

#define GLYPH_BYTES_LIMIT (128u * 1024u * 1024u)

///////////////////////////////////////////////////////////////////////////////
// 字形槽位
///////////////////////////////////////////////////////////////////////////////

typedef struct GlyphSlot {
    u8*  tex;         // 生成后不可修改
    s32  tw, th;
    s32  twLog2, thLog2;
    s32  bw, bh;
    s32  xoff, yoff;
    f32  advanceEm;
    f32  rasterEm;
    u8   state;       // 0 未生成，1 已生成，2 无法生成
} GlyphSlot;

///////////////////////////////////////////////////////////////////////////////
// 状态（运行期全部可变量，就这些）
///////////////////////////////////////////////////////////////////////////////

// 字体链按注册顺序选择第一个包含目标码点的字体。
#define FONT_MAX 4

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

/*
 * 当前正在使用的字体。
 *
 * stbtt_* 调用点通过下面的宏间接访问当前字体，
 * 每个用到字形的函数在入口处把 sCur 指向该字形所属的字体。
 */
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

// 每个字符的档位行按首次使用分配。
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

// 顶点池（每帧重置；djui_render 每帧两遍，见 frame_begin 说明）
#define GLYPH_VERTS 4
#define GLYPH_POOL_BLOCKS 16384
static Vtx*  sVtxPool = NULL;
static u32   sVtxUsed = 0;

// 当前已绑定的字形纹理（用于跳过重复绑定）
static const u8* sBoundTex = NULL;

// 上游按名义字格计算裁剪比例，绘制前需要换算到字形墨迹盒。
static const Gfx* sGlyphDlMark = NULL;   // 本次字形之前"已消费"的 DL 位置

// 聊天框保留垂直裁剪，其余控件允许文字上下溢出。
static bool sTextClipHard = false;

// 当前 DjuiFont.charHeight。
static f32 sCellHeight = 1.0f;

// 聊天框硬裁剪时为字形下伸部预留的 em 余量。
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

static void glyph_fix_pending_clip(f32 inkX0, f32 inkW, f32 inkY0, f32 inkH) {
    if (sGlyphDlMark == NULL) { return; }
    for (const Gfx* p = sGlyphDlMark; p < gDisplayListHead; p++) {
        if ((u32)(p->words.w0 >> 24) != (u32)G_TEXCLIP_DJUI) { continue; }

        const f32 f1 = (f32)((p->words.w0 >> 16) & 0xFFu) / 255.0f;   // 从左边裁掉
        const f32 f2 = (f32)((p->words.w1 >> 16) & 0xFFu) / 255.0f;   // 从右边裁掉
        const f32 g1 = (f32)((p->words.w0 >>  8) & 0xFFu) / 255.0f;   // 从上边裁掉
        const f32 g2 = (f32)((p->words.w1 >>  8) & 0xFFu) / 255.0f;   // 从下边裁掉
        const f32 cw = DJUI_MODERN_FONT_CELL_W;

        // 水平：裁剪区在字格里的位置 = [f1*cw, cw - f2*cw]
        f32 q1 = 0.0f, q2 = 0.0f;
        if (inkW > 0.001f) {
            q1 = (f1 * cw - inkX0) / inkW;
            q2 = (inkX0 + inkW - (cw - f2 * cw)) / inkW;
        }

        // 垂直：只有聊天框保留
        f32 ky1 = 0.0f, ky2 = 0.0f;
        if (sTextClipHard && inkH > 0.001f) {
            const f32 clipTop = -g1 * sCellHeight;
            const f32 clipBot = (g2 - 1.0f) * sCellHeight - sClipPadBottomEm;
            ky1 = (inkY0 - clipTop) / inkH;
            ky2 = (clipBot - (inkY0 - inkH)) / inkH;
        }

        q1 = clip01(q1); q2 = clip01(q2);
        ky1 = clip01(ky1); ky2 = clip01(ky2);

        // 原位改写本次字形产生的命令，扫描范围由 sGlyphDlMark 限定。
        Gfx* w = (Gfx*)p;
        w->words.w0 = _SHIFTL((u32)G_TEXCLIP_DJUI, 24, 8)
                    | _SHIFTL((u32)(q1  * 255.0f + 0.5f), 16, 8)
                    | _SHIFTL((u32)(ky1 * 255.0f + 0.5f),  8, 8);
        w->words.w1 = _SHIFTL((u32)(q2  * 255.0f + 0.5f), 16, 8)
                    | _SHIFTL((u32)(ky2 * 255.0f + 0.5f),  8, 8);
    }
}

// 无几何字符也必须消费上游的待处理裁剪状态。
static void glyph_consume_pending_clip(void) {
    glyph_fix_pending_clip(0.0f, DJUI_MODERN_FONT_CELL_W, 0.0f, sCellHeight);
    *(gDisplayListHead++) = (Gfx) gsSPExecuteDjui(G_TEXCLIP_DJUI);
    sGlyphDlMark = gDisplayListHead;
}

// Emoji 图集在初始化时解码并持有 RGBA32 缓冲。
#if defined(DJUI_MODERN_EMOJI_COUNT)
static const u8* sEmojiAtlasRGBA = NULL;
static u8* sEmojiAtlasOwned = NULL;
#endif

// 国旗使用与 Emoji 相同布局的独立图集。
#if defined(DJUI_MODERN_FLAG_COUNT)
static const u8* sFlagAtlasRGBA = NULL;
static u8* sFlagAtlasOwned = NULL;
#endif

static s32  flag_tile_at(const char* c);
static bool flag_consumes(const char* c);
#if defined(DJUI_MODERN_EMOJI_COUNT)
static void emoji_atlas_prepare(void);
#endif
#if defined(DJUI_MODERN_FLAG_COUNT)
static void flag_atlas_prepare(void);
#endif

#define FLAG_KIND_NONE 0
#define FLAG_KIND_HEAD 1
#define FLAG_KIND_TAIL 2

#if defined(DJUI_MODERN_FLAG_COUNT)
static const char* sFlagCurPtr  = NULL;   // 上一次求值的字符指针（幂等缓存键）
static s32         sFlagCurKind = FLAG_KIND_NONE;
    static const char* sFlagPrevEnd = NULL;
    static u32         sFlagPrevCp  = 0;
static bool        sFlagPrevWasHead = false;
#endif

// 统计
static u32   sGlyphRasterized = 0;
static u32   sGlyphBytes = 0;
static bool  sBudgetWarned = false;

///////////////////////////////////////////////////////////////////////////////
// 渲染缩放提示栈
//
// 栈里存"本次文本在屏幕上的 em 像素高度"（物理像素），由
// djui_modern_font.c 从 djui_gfx_size_translate() 上报的字号换算得到。
///////////////////////////////////////////////////////////////////////////////

#define SCALE_HINT_STACK_MAX 32
static f32 sScaleHintFont[SCALE_HINT_STACK_MAX];
static s32 sScaleHintTop = 0;
static f32 sScaleHintCurrentFont = 0.0f;   // 0 = 尚未设置
static f32 sScaleHintCurrentHud  = 1.0f;

#define kFallbackEmPixels 32.0f

///////////////////////////////////////////////////////////////////////////////
// 工具
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// 字符集构建
///////////////////////////////////////////////////////////////////////////////

/*
 * 已收录码点的去重位图（0x110000 位 = 136KB BSS）。
 * 重复码点会破坏 charset_lookup 的二分查找。
 */
static u8 sCharsetSeen[0x110000 / 8];

/* 与 sCharset 一一对应：每个码点由哪一份字体提供，决定度量与光栅使用哪份。 */
static u8 sCharsetFace[CHARSET_MAX];

/*
 * 收录一个码点。face < 0 时使用字体链中第一个包含该码点的字体。
 * 已收录或成功新增时返回 true；没有字体包含该码点时返回 false。
 */
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

/*
 * 以只读方式引用上游 GB2312 字表，并放入布局兼容的本地数组。
 * 不直接引用 sSmCodeGlyphs_CN[]，因为其完整类型仅在上游 .c 文件中可见。
 */
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

/*
 * 字符集按下述顺序构建：
 *   1) ASCII；
 *   2) cn_glyphs.h 中的 GB2312 字符；
 *   3) 字体 cmap 中的其他字符。
 * 游戏字符优先占位，可避免完整 CJK 字体在 CHARSET_MAX 处截断常用文本。
 */
static void charset_build(void) {
    memset(sCharsetSeen, 0, sizeof(sCharsetSeen));
    for (s32 i = 0; i < 128; i++) { sAsciiIndex[i] = -1; }

    // 下标 0 留给回退字形；实际码点从 1 开始放。
    sCharsetCount = 1;
    sCharset[0] = '?';
    sCharsetFace[0] = (u8)((font_face_for('?') >= 0) ? font_face_for('?') : 0);

    // 1) ASCII 可见区（最高频，先收）
    charset_scan_range(0x20, 0x7F);

    // 2) 游戏字符必须在扫描字体 cmap 之前加入
    charset_add_needed_cn();

    // 3) 其余区段：尽量补齐字体包含的字符
    charset_scan_range(0x4E00, 0x9FFF);
    charset_scan_range(0xA0, 0x4DFF);
    charset_scan_range(0xA000, 0xFFFD);

    // charset_lookup 的二分查找依赖升序排列
    if (sCharsetCount > 2) {
        qsort(&sCharset[1], (size_t)(sCharsetCount - 1), sizeof(u32), charset_cmp);
    }

    // 重建 ASCII 快速索引（下标在排序后变了）
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

///////////////////////////////////////////////////////////////////////////////
// 字形纹理（每字形一张，不可变）
///////////////////////////////////////////////////////////////////////////////

/*
 * 把 stb 输出的 8 位覆盖度位图落成一张独立的 POT RGBA32 纹理。
 *
 * RGB 恒为 255（白），颜色完全由 RDP 的 ENV 决定（G_CC_FADEA 的 TEXEL0×ENV）。
 * 位图左上角对齐在纹理 (0,0)，因此 UV 直接就是 (0,0)-(bw,bh) 纹素。
 */
static bool glyph_make_texture(GlyphSlot* g, const u8* bmp, s32 bw, s32 bh) {
    const s32 tw = pot_ceil(bw);
    const s32 th = pot_ceil(bh);
    /*
     * pot_ceil 的上界被钳在 GLYPH_TEX_MAX，字形位图超出该上限时
     * 返回值会小于位图尺寸，因此按位图尺寸逐行拷贝前必须确认纹理能够容纳。
     */
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

    // 纹理其余部分必须是透明（alpha=0），否则双线性会采到未初始化字节
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

/*
 * 确保某个槽位已光栅化。state 为 0 时才做事。
 *
 * 纹理字节在生成后不再修改，指针缓存因此始终有效。
 */
static void slot_rasterize(GlyphSlot* g, u32 cp, s32 bucket, s32 face) {
    if (g->state != 0) { return; }

    // 这个字形由哪份字体提供，就整条链路都用它（度量 / 光栅 / 基线）
    sCur = &sFaces[(face >= 0 && face < sFaceCount) ? face : 0];

    const s32 gi = stbtt_FindGlyphIndex(&sFont, (s32)cp);
    if (gi == 0) {
        g->state = 1;      // 记为"已处理"，bw/bh=0 -> 无墨迹
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
        g->state = 2;      // 生成失败：绘制时退化为不可见
        return;
    }

    g->state = 1;
    sGlyphRasterized++;
    stbtt_FreeBitmap(bmp, NULL);
}

///////////////////////////////////////////////////////////////////////////////
// 字体数据加载
///////////////////////////////////////////////////////////////////////////////

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
 * 字体查找：**游戏根目录**（可执行文件所在目录）。
 *
 * ⚠⚠ 必须走**真实文件系统**（`fs_sys_walk`），不能用 `fs_enumerate`：
 *   `fs_init()` 只把**用户目录**（`sys_user_path()` =
 *   `%APPDATA%\sm64coopdx\`）挂进了搜索路径，**游戏目录不在其中** ——
 *   `fs_enumerate("")` 列出来的是 AppData 里的文件，永远看不到游戏目录。
 *
 * 顺序（先找到先用）：
 *   <游戏根目录>/djui_modern/   ← 推荐目录，文件名可为任意有效 .ttf/.otf
 *   <游戏根目录>/               ← 根目录下任意 .ttf / .otf
 * 同一目录里若有多个字体，按**文件名升序**取第一个 —— 结果确定，
 * 不会因为文件系统的枚举顺序不同而每次换字体。
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
 * 装配字体链。
 *
 * 优先级（**注册顺序即优先级**：`font_face_for()` 取第一个含该码点的字体）：
 *   1) cfg 的 font_path
 *   2) 游戏根目录里的外部字体
 *        <游戏根目录>/djui_modern/  →  <游戏根目录>/
 *   3) 内嵌字体链 —— 西文(Roboto Condensed) → 中文(Source Han Sans SC)
 *                → 星星(Power Star，仅提供 U+007F)
 *
 * 外部字体位于链首，内嵌字体追加在后作为缺字回退。
 * 仅包含西文的外部字体由内嵌中文字体补充汉字；包含汉字的字体会优先命中自身字形。
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
            // 1) 游戏根目录下的 djui_modern 文件夹（推荐）
            if (snprintf(dir, sizeof(dir), "%s/djui_modern", exeDir) > 0 && font_try_dir(dir)) {
                haveExternal = true;
            // 2) 游戏根目录本身
            } else if (font_try_dir(exeDir)) {
                haveExternal = true;
            }
        }
    }
    if (haveExternal) {
        LOG_INFO("djui_modern: external font registered first; embedded chain appended as fallback");
    }

    // 3) 内嵌字体链：顺序即优先级（西文、中文、星星）。
    //    外部字体存在时也照常追加 —— 它只负责"外部字体没覆盖到的码点"。
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
#if defined(DJUI_MODERN_STAR_FONT_PRESENT)
    if (sFaceCount < FONT_MAX &&
        font_face_setup(&sFaces[sFaceCount], (u8*)sDjuiModernStarFont,
                        sDjuiModernStarFontSize, false, DJUI_MODERN_STAR_FONT_NAME)) {
        sFaceCount++;
    }
#endif
}

///////////////////////////////////////////////////////////////////////////////
// 公共 API
///////////////////////////////////////////////////////////////////////////////

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

    /*
     * 基线与行高以**链首**（西文）为准；每个字形各自按**自己那份字体**的
     * upem 换算（见 slot_rasterize / char_width 里的 sCur），基线共享 ——
     * 这正是"多字体混排"该有的行为：不同字体坐在同一条基线上。
     */
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

    /*
     * 下伸部**超出字格**的部分 = 字体真实下伸 - 基线下方的可用空间。
     * 聊天框的硬裁剪要把下边界放宽这么多，否则 g / y / p / q / j 的尾巴会被切掉
     * （聊天消息的文字节点高度恰好 = 1em）。详见 sClipPadBottomEm 的说明。
     */
    {
        const f32 descUsed = 1.0f + sBaselineY;   // 基线下方的可用空间（= 钳位后的 desc）
        const f32 descNeed = -sDescentEm;         // 字体真实下伸
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

    // 把回退槽（下标 0）指向 '?' 的位图：改 sCharset[0] 为 '?' 的 codepoint，
    // 这样 slot_rasterize 会光栅化 '?'，之后所有"查不到的字"都画成 '?'。
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
    // Emoji 图集（PNG 解一次；失败则 emoji 整体降级回原版，不影响字体链）
    emoji_atlas_prepare();
#endif
#if defined(DJUI_MODERN_FLAG_COUNT)
    // 国旗图集（另一张，独立解码）
    flag_atlas_prepare();
#endif

    // 组装字体链名字（日志用；只取文件名，避免长路径把缓冲撑满）
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
    /*
     * 内存预算（便于诊断"堆不够用"这类问题）：
     *   · 槽位表是**按行惰性分配**的 —— 只有真正显示过的字符才占内存，
     *     未用到的行只占一个指针。这里报的是"全部用满时的上限"，不是实占。
     *   · 顶点池是固定分配，每帧重置。
     * 实际用掉的行数在 shutdown 时打印。
     */
    LOG_INFO("djui_modern: heap budget: slot grid lazy %d x %d x %u B (<= %.1f MB if all used), "
             "vtx pool %.0f KB",
             sSlotRows, BUCKET_COUNT, (unsigned)sizeof(GlyphSlot),
             (double)sSlotRows * BUCKET_COUNT * sizeof(GlyphSlot) / (1024.0 * 1024.0),
             (double)(sizeof(Vtx) * GLYPH_VERTS * GLYPH_POOL_BLOCKS) / 1024.0);

    /*
     * 字符覆盖抽检。
     *
     * 用于诊断指定字符是否已收录：
     *   (a) 字符集没收录（i=-1）  → 字符集构建的问题
     *   (b) 收录了但渲染链路没画出来 → 纹理上传/绑定的问题
     * 抽检包含 U+963F「阿」，用于确认字符集不是只收录某个区间的前 N 个码点；
     * 和 U+300C「」（cn_glyphs.h 没有、本模块额外补的直角引号）。
     */
    {
        static const u32 kProbe[] = {
            '1', '5', 'A', 'g', '?',
            0x4F60, 0x597D, 0x623F, 0x95F4, 0x963F, 0x7684, 0x300C,
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

    return true;
}

void djui_modern_font_atlas_shutdown(void) {
    // 释放全部字形纹理（每个字形独立 malloc，必须逐个释放；行本身也按需分配）
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
    // Emoji 图集缓冲（PNG 解出来的那份才需要释放）
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

/* 每帧第一遍：重置顶点游标与"已绑定纹理"记录。字形纹理常驻，无需每帧维护。 */
void djui_modern_font_atlas_frame_begin(void) {
    sVtxUsed = 0;
    sBoundTex = NULL;
#if defined(DJUI_MODERN_FLAG_COUNT)
    /*
     * 国旗配对状态也在这里清一次：它记录的是"上一个字符的指针"，
     * 跨帧留着可能指向已失效的字符串缓冲 —— 虽然还有"指针必须紧邻"这道
     * 保险，但每帧清一次能从根上杜绝陈旧状态。
     */
    sFlagCurPtr = NULL;
    sFlagCurKind = FLAG_KIND_NONE;
    sFlagPrevEnd = NULL;
    sFlagPrevCp = 0;
    sFlagPrevWasHead = false;
#endif
}

/*
 * 半帧重渲的第二遍：**不清零**顶点池。
 *
 * djui_render() 在插值重渲时会执行两遍。Lua HUD 文字的指令块由 memcpy
 * 恢复，第二遍不会重新执行字体钩子；此时清零顶点池会覆写其几何数据。
 * 因此第二遍只追加顶点，不重置顶点池。
 */
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

///////////////////////////////////////////////////////////////////////////////
// 字号 -> 档位
///////////////////////////////////////////////////////////////////////////////

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

/*
 * ── 字形光栅的超采样系数 ──
 *
 * 屏幕 em 到字形档位使用恒定超采样，再通过双线性缩小采样。
 * 超采样是保证小字号清晰度的必要条件：
 *
 * 位图与屏幕像素为 1:1 时，字形顶点位于分数像素坐标，采样点与纹素边界重合。
 * 轻微的亚像素偏移会在点采样和双线性采样之间切换，导致边缘同时出现锯齿和模糊。
 *
 *   Hide and Seek 顶部文字使用 scale=0.5，
 *   屏幕 em = defaultFontScale(32) × 0.5 × djuiScale = 16px，
 *   而档位表里**正好有一档 16** -> ratio 恒为 1.00 -> 最坏情形。
 *
 * 位图严格大于屏幕 em 时，双线性缩小采样可提供盒式滤波效果，
 * 使笔画边缘获得平滑的灰度过渡。
 *
 * 取值：1.25 即可让每个屏幕像素至少覆盖一个完整纹素周期；
 *   再往上画质收益迅速衰减（人眼几乎看不出 1.25 与 1.6 的差别），
 *   而纹理面积按**平方**增长（1.25² ≈ +56%，1.6² = +156%）。
 *   1.25 与档位表本身较密的间距（12→16→20→26）配合，绝大多数 em
 *   会落到"下一个档"，ratio 落在 [1.0, 1.7] 区间，兼顾清晰与内存。
 */
#define kGlyphSupersample 1.25f

/* 小字号汉字需要更多纹理像素，避免密集笔画在降采样后粘连。 */
#define kSmallCjkMinRasterEm 48.0f

/*
 * 屏幕 em 像素 -> 档位下标。
 *
 * 取**第一个 >= em × kGlyphSupersample 的档**。因为 kGlyphSupersample > 1，
 * 结果必然严格大于 em（除非 em 已超过最高档 512，此时钳到最高档），
 * 从而保证"位图分辨率 > 屏幕分辨率"，恒有超采样可供降采样。
 *
 * 档位必须取第一个不小于 em × kGlyphSupersample 的值。
 * 若直接取第一个不小于 em 的档位，em 与档位相等时比值为 1，无法进行超采样。
 */
static s32 bucket_index_for_em(f32 em) {
    const f32 want = em * kGlyphSupersample;
    for (s32 i = 0; i < BUCKET_COUNT; i++) {
        if ((f32)kRasterBuckets[i] >= want) { return i; }
    }
    return BUCKET_COUNT - 1;
}

///////////////////////////////////////////////////////////////////////////////
// 宽度查询
///////////////////////////////////////////////////////////////////////////////

/*
 * DJUI 逐字符调用无状态的 char_width(c)，返回值决定下一个字符的位置。
 * 因此这里前瞻下一个字符，将 kern(c, next) 计入当前字符的宽度。
 *
 * stbtt_GetGlyphKernAdvance 会优先读取 GPOS Pair Adjustment，
 * 缺少 GPOS 时再读取 legacy kern 表。
 */

/*
 * 部分字体不包含西文字偶数据。该表提供常见西文对的保守间距，
 * 仅在字体没有对应字偶数据时启用，不覆盖字体自身的度量。
 */
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

/*
 * 等宽拉丁字体禁用回退字偶间距，避免破坏字符网格。
 * 通过抽样 ASCII 字形的 advance 极差判断是否为等宽字体。
 */
static bool sLatinMonospace = false;

static void detect_latin_monospace(void) {
    static const char kProbe[] = "iIlmMW1A.w";
    f32 mn = 1e9f, mx = -1e9f;
    s32 n = 0;
    if (sFaceCount == 0) { sLatinMonospace = false; return; }
    sCur = &sFaces[0];   // 只看**西文那份**（链首）
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
    /*
     * 同字重复**不再直接跳过**：字体给 ll / oo 这类对配字偶的情况是存在的，
     * 交给字体自己回答更可靠（CJK 字体没有字偶数据，查出来自然是 0）。
     */

    /*
     * 字偶间距只在**同一份字体内部**才有意义：跨字体（例如西文 T 紧接中文"中"）
     * 没有共同的度量体系，硬套只会出错。
     */
    const s32 fi = font_face_for(prev);
    if (fi < 0 || font_face_for(cur) != fi) { return 0.0f; }
    sCur = &sFaces[fi];

    const s32 g1 = stbtt_FindGlyphIndex(&sFont, (s32)prev);
    const s32 g2 = stbtt_FindGlyphIndex(&sFont, (s32)cur);
    if (g1 == 0 || g2 == 0) { return 0.0f; }
    const s32 k = stbtt_GetGlyphKernAdvance(&sFont, g1, g2);
    if (k != 0) { return (f32)k / sUnitsPerEm; }

    /*
     * 回退表仅在字体既无 GPOS 也无 legacy kern 数据时启用。
     * 字体提供字偶数据时，缺失的字符对保持字体原始间距。
     * 回退表只覆盖 ASCII 字符对，可通过配置关闭。
     */
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
    const u32 cp = djui_modern_font_utf8_decode(c, &bytes);

    /*
     * 空格：取**字体的真实 advance**。
     *
     * 这里以前写死 0.26em。Roboto Condensed 的 space 是 470/2048 = 0.2295em，
     * 也就是说空格比字体设计宽了 13%。别的字符全都走字体原生度量，只有空格
     * 偏宽，词间距与字母间距不成比例 —— 整行读起来就是"字距忽松忽紧"。
     * 只有字体根本没有空格字形时，才退回 0.25em 的经验值。
     */
    if (cp == ' ') {
        const s32 sci = charset_lookup(cp);
        sCur = &sFaces[(sCharsetFace[sci] < sFaceCount) ? sCharsetFace[sci] : 0];
        const s32 gs = stbtt_FindGlyphIndex(&sFont, ' ');
        s32 sAdv = 0, sLsb = 0;
        if (gs != 0) { stbtt_GetGlyphHMetrics(&sFont, gs, &sAdv, &sLsb); }
        return (sAdv > 0) ? ((f32)sAdv / sUnitsPerEm) : 0.25f;
    }

    // Emoji 固定 1em 宽（与原版 32x32 emoji 贴图的推进量一致）
    if (djui_modern_emoji_available(c)) { return 1.0f; }

    // 国旗：头 1em、尾 0（见 flag_tile_at 的说明）
    if (flag_consumes(c)) { return (flag_tile_at(c) >= 0) ? 1.0f : 0.0f; }

    // 度量必须来自"这个字所属的那份字体"
    const s32 ci = charset_lookup(cp);
    sCur = &sFaces[(sCharsetFace[ci] < sFaceCount) ? sCharsetFace[ci] : 0];

    const s32 gi = stbtt_FindGlyphIndex(&sFont, (s32)cp);
    f32 adv;
    if (gi == 0) {
        // 字体没有这个字形：回退为 '?' 的宽度，避免与绘制不一致
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

    /*
     * 字偶间距：加在**当前字符**的推进量上（= 与"下一个字符"之间的收紧量）。
     * char_width(c) 的返回值决定"下一个字符画在哪"，所以 kern(cur, next)
     * 必须算在 cur 上 —— 见上方"字偶间距"一节的说明。
     * 前瞻是纯函数（只看 c 与 c+bytes），测量与渲染结果必然一致。
     */
    u32 nextCp = 0;
    const char* nc = c + bytes;
    if (*nc != '\0' && *nc != '\n' && *nc != '\r' && *nc != '\t') {
        s32 nb = 1;
        nextCp = djui_modern_font_utf8_decode(nc, &nb);
    }
    adv += glyph_kern_em(cp, nextCp);
    if (adv < 0.05f) { adv = 0.05f; }   // 防止字偶间距把宽度压成 0/负

    return adv;
}

///////////////////////////////////////////////////////////////////////////////
// 绘制
///////////////////////////////////////////////////////////////////////////////

static Vtx* vtx_alloc(void) {
    if (sVtxPool == NULL) {
        sVtxPool = malloc(sizeof(Vtx) * GLYPH_VERTS * GLYPH_POOL_BLOCKS);
        if (sVtxPool == NULL) { return NULL; }
        memset(sVtxPool, 0, sizeof(Vtx) * GLYPH_VERTS * GLYPH_POOL_BLOCKS);
    }
    if (sVtxUsed + GLYPH_VERTS > GLYPH_VERTS * GLYPH_POOL_BLOCKS) {
        /*
         * 顶点池满表示一帧内已经输出 4096 个字形。正常渲染流程会在
         * frame_begin 重置顶点池；崩溃界面停止正常帧循环时不会触发重置，
         * 因此这里重置游标，使错误信息能够完整绘制。
         */
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

/*
 * 每个字形 = 1 条 G_VTX_EXT + 1 条 G_TRI2_EXT。
 * 必须用 gsSP2TrianglesDjui（G_TRI2_EXT=0x12），不能用标准 gsSP2Triangles
 * （G_TRI2=0x06，会走 3D 顶点管线，把屏幕空间顶点再变换一遍）。
 * 模板中不得包含 gsSPEndDisplayList，否则顶层循环会提前返回。
 */
static const Gfx sGlyphTemplate[] = {
    gsSPVertexNonGlobal(NULL, GLYPH_VERTS, 0),
    /*
     * 保持与原版字体渲染相同的顺序：顶点、执行裁剪、三角形。
     * djui_gfx_add_clipping_specific() 只记录待处理裁剪状态，
     * 必须由 G_TEXCLIP_DJUI 执行并清除，否则参数会残留到后续字形。
     */
    gsSPExecuteDjui(G_TEXCLIP_DJUI),
    gsSP2TrianglesDjui(0, 1, 3, 0, 0, 3, 2, 0),
};

/*
 * 绑定一张字形纹理。
 *
 * ⚠ gDPSetTextureOverrideDjui 的 (w,h) 参数是**边长的 log2**，不是纹素数！
 *   证据：gfx_pc.c:2465 的处理是
 *       djui_gfx_dp_set_override(..., 1 << C0(16, 8), 1 << C0(8, 8), ...)
 *   而且 gSetOverrideDjui 里 w/h 各只占 8 位字段（_SHIFTL(w,16,8)）。
 *   所以纹理边长必须是 2 的幂 —— 这正是 pot_ceil() 的用途。
 *
 *   注意 gDPLoadTextureBlockWithoutTexture 里的 (tw, th) 是**纹素数**，
 *   只用来算 tile 的 lrs/lrt（决定 UV 归一化基准 tex_width/tex_height
 *   = (lrs-uls+4)/4），与上面的 log2 是两套参数。二者必须一致，否则 UV 会
 *   整体缩放错位。（对照 djui_gfx.c:118 的 djui_gfx_power_of_two(w)。）
 */
static void bind_glyph_texture(const GlyphSlot* g) {
    gDPLoadTextureBlockWithoutTexture(gDisplayListHead++, NULL, G_IM_FMT_RGBA, G_IM_SIZ_16b,
        g->tw, g->th, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    gDPSetTextureOverrideDjui(gDisplayListHead++, g->tex,
        g->twLog2, g->thLog2, G_IM_FMT_RGBA, G_IM_SIZ_32b);
    *(gDisplayListHead++) = (Gfx) gsSPExecuteDjui(G_TEXOVERRIDE_DJUI);
    sBoundTex = g->tex;
}

///////////////////////////////////////////////////////////////////////////////
// Emoji（离线烘焙的 RGBA 图集）
///////////////////////////////////////////////////////////////////////////////
/*
 * 为什么 Emoji 不走字体链：原版 emoji 字体（Noto-3D-128.ttf）**只有位图没有
 * 轮廓**（`glyf` 仅 103KB，`sbix` 147MB），而 stb_truetype **完全不支持**
 * sbix / CBDT / COLR。所以现代 emoji 只能离线烘焙成图集内嵌
 * （见 tools/make_emoji_atlas.py）。
 *
 * 图集布局与内嵌格式全部由生成头文件里的宏描述：
 *   DJUI_MODERN_EMOJI_COUNT   emoji 个数（tile 0 .. COUNT-1，**emoji 图集**）
 *   DJUI_MODERN_FLAG_COUNT    国旗条数（tile 0 .. FLAG-1，**国旗图集**，独立编号）
 *   DJUI_MODERN_EMOJI_GRID_X/Y 每行/每列多少格（两张图共用）
 *   DJUI_MODERN_EMOJI_TILE    每格边长（像素，两张图共用）
 *   DJUI_MODERN_EMOJI_DIM_X/Y 整图边长（像素，均为 2 的幂，两张图共用）
 *   DJUI_MODERN_EMOJI_ATLAS_PNG / DJUI_MODERN_FLAG_ATLAS_PNG
 *                             1 = PNG 内嵌，0 = 裸 RGBA32
 *
 * tile 下标就是码点 / RI 对在各自升序表里的位置，所以 C 侧不需要额外的哈希
 * 表，二分查找即可。两张图的唯一区别是**数据指针**，渲染时换掉绑定即可。
 */

#if defined(DJUI_MODERN_EMOJI_COUNT)
/*
 * 把内嵌图集准备好（PNG 解成 RGBA32）。
 *
 * 为什么要存 PNG 而不是裸 RGBA32：
 *   裸 RGBA32 在 C 头文件里是 5 字符/字节，体积与编译时间都扛不住
 *   （512×512 是 1MB 原始 → 5MB 文本；裸存的 8MB 图集 → 40MB 级）。
 *   PNG 无损压到原始的 1/4 左右，代价只是启动时解一次（几十毫秒）。
 *
 * 失败时**不致命**：sEmojiAtlasRGBA 保持 NULL ⇒ djui_modern_emoji_available
 * 返回 false ⇒ 全部 emoji 自动交回原版绘制。
 */
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
    // 裸 RGBA32：内嵌数组本身就是最终数据，直接指过去
    sEmojiAtlasRGBA = sDjuiModernEmojiAtlas;
#endif
}
#endif

#if defined(DJUI_MODERN_FLAG_COUNT)
/* 国旗图集：另一张图，同样处理；失败则国旗整体不接管（退回普通字形路径）。 */
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
    // 布局必须与 emoji 图集完全一致（C 侧共用同一套 DIM / GRID 宏）
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
 * 国旗由两个相邻的 Regional Indicator 组成。DJUI 逐字符调用字体回调，
 * 因此第一个字符负责绘制整面旗，第二个字符宽度为零且不输出几何。
 * 仅相邻字符可以配对，避免连续国旗或国旗后的普通字符产生错误组合。
 */
#define RI_FIRST 0x1F1E6u
#define RI_LAST  0x1F1FFu

#if defined(DJUI_MODERN_FLAG_COUNT)
static bool is_regional_indicator(u32 cp) { return cp >= RI_FIRST && cp <= RI_LAST; }

/*
 * RI 对 -> **国旗图集**里的 tile 下标（`sDjuiModernFlagKeys[]` 升序，二分查找）；
 * 没有则 -1。
 *
 * ⚠ 下标是**国旗图集内部**的编号（0 .. FLAG_COUNT-1），不再叠加
 *   DJUI_MODERN_EMOJI_COUNT —— 国旗是独立的一张图集。
 */
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
    if (c == sFlagCurPtr) { return sFlagCurKind; }   // 同一字符重复询问 ⇒ 复用

    s32 b = 1;
    const u32 cp = djui_modern_font_utf8_decode(c, &b);

    // 只有"紧邻上一个字符"才算同一趟遍历里的连续字符
    const bool adjacent = (sFlagPrevEnd != NULL) && (c == sFlagPrevEnd);

    s32 kind = FLAG_KIND_NONE;
    if (is_regional_indicator(cp)) {
        if (adjacent && sFlagPrevWasHead && flag_pair_valid(sFlagPrevCp, cp)) {
            kind = FLAG_KIND_TAIL;                       // 尾：由前一个头字符消费
        } else {
            const char* n = c + b;
            s32 nb = 1;
            if (*n != '\0' && flag_pair_valid(cp, djui_modern_font_utf8_decode(n, &nb))) {
                kind = FLAG_KIND_HEAD;                   // 头：画整面旗
            }
            // 否则是孤立 RI：宽 0、不画
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
    if (sFlagAtlasRGBA == NULL) { return false; }    // 国旗图集没准备好 ⇒ 不接管
    return flag_kind_at(c) == FLAG_KIND_HEAD;
#else
    (void)c;
    return false;
#endif
}

/* 本字符是否由国旗逻辑消费（头或尾都不再进入普通字形路径）。 */
static bool flag_consumes(const char* c) {
#if defined(DJUI_MODERN_FLAG_COUNT)
    if (DJUI_MODERN_FLAG_COUNT == 0 || !gDjuiModernConfig.emojiModern) { return false; }
    if (sFlagAtlasRGBA == NULL) { return false; }    // 国旗图集没准备好 ⇒ 不接管
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

/* 这个字符是否由**现代 emoji 图集**负责绘制（否则交回原版）。 */
bool djui_modern_emoji_available(const char* c) {
#if defined(DJUI_MODERN_EMOJI_COUNT)
    if (!gDjuiModernConfig.emojiModern || c == NULL || *c == '\0') { return false; }
    if (sEmojiAtlasRGBA == NULL) { return false; }   // 图集解码失败 ⇒ 整体降级
    s32 bytes = 1;
    const u32 cp = djui_modern_font_utf8_decode(c, &bytes);
    return emoji_tile_for(cp) >= 0;
#else
    (void)c;
    return false;
#endif
}

/*
 * djui_unicode_valid_char() 的接管钩子（原版侧是弱空兜底，见 djui_unicode.c）。
 *
 * 原版只接受 sCharMap 中的字形，该表不包含模块新增的 Emoji 和国旗字符。
 * 此处放行模块能够渲染的字符，判定与渲染端保持一致：
 *   · 国旗：两个 Regional Indicator 都通过校验后才接受；
 *   · Emoji：使用 djui_modern_emoji_available() 判断。
 * 图集未就绪或配置为原版 Emoji 时返回 false，保持原版行为。
 */
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
/*
 * 画一个 tile。`atlas` 是**运行时 RGBA32 缓冲**（emoji 图集或国旗图集之一），
 * `idx` 是**该图集内部**的 tile 下标。两张图布局一致，所以几何与 UV 完全共用，
 * 只有绑定的数据指针不同。
 */
static void emoji_render_tile(const u8* atlas, s32 idx) {
    if (atlas == NULL) { return; }

    /*
     * 几何与字形同一套对象空间约定（1 unit = 1em）：
     *   宽 1em；顶在基线上方 0.80em，底在下方 0.20em。
     */
    const f32 x0 = 0.0f, x1 = 1.0f;
    const f32 y0 = sBaselineY + 0.80f;   // 顶（对象空间里更负 = 更高）
    const f32 y1 = y0 - 1.0f;            // 底

    // 图集是 grid_x x grid_y 的非方形排布（尽量接近方形，避免大片空白格）
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

    /*
     * 纹理绑定：整张图集只上传一次（地址恒定、内容不可变，与字形纹理同一套前提）。
     * 键是**运行时缓冲的地址**（PNG 解出来的那份），不是内嵌数组。
     */
    if (sBoundTex != atlas) {
        gDPLoadTextureBlockWithoutTexture(gDisplayListHead++, NULL, G_IM_FMT_RGBA, G_IM_SIZ_16b,
            DJUI_MODERN_EMOJI_DIM_X, DJUI_MODERN_EMOJI_DIM_Y, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
        gDPSetTextureOverrideDjui(gDisplayListHead++, (const void*)atlas,
            DJUI_MODERN_EMOJI_LOG2_X, DJUI_MODERN_EMOJI_LOG2_Y, G_IM_FMT_RGBA, G_IM_SIZ_32b);
        *(gDisplayListHead++) = (Gfx) gsSPExecuteDjui(G_TEXOVERRIDE_DJUI);
        sBoundTex = atlas;
    }

    // emoji / 国旗的四边形是 1em 见方的方块（水平正好占满字格，垂直位置由基线决定）
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

    /*
     * G_CC_FADEA：RGB = TEXEL0.rgb(白) * ENV.rgb（文字颜色）；Alpha = 覆盖度 * ENV.a。
     * ENV 由 djui_text 设置（含 \#rrggbb 转义）。此处不得修改 Prim/Env。
     */
    gDPSetCombineMode(gDisplayListHead++, G_CC_FADEA, G_CC_FADEA);
    gDPSetRenderMode(gDisplayListHead++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetTextureFilter(gDisplayListHead++, gDjuiModernConfig.fontBilinear ? G_TF_BILERP : G_TF_POINT);
    gDPSetTexturePersp(gDisplayListHead++, G_TP_NONE);
    gSPTexture(gDisplayListHead++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);

    /*
     * 这里**不再绑定图集**：绑定已下沉到每个字形（见 bind_glyph_texture）。
     *
     * 上一个 render_begin 与本 begin 之间可能画过圆角矩形等使用别的纹理的
     * 几何，所以"当前已绑定的字形纹理"必须失效，否则会拿上一次的纹理画字。
     */
    sBoundTex = NULL;

    // 纹理裁剪换算的扫描起点（见 glyph_fix_pending_clip）
    sGlyphDlMark = gDisplayListHead;

    /*
     * 字偶间距使用前瞻式计算，不依赖跨调用状态，因此此处无需重置。
     */
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
    // Emoji：由现代图集负责（不可用时上层已经交回原版，不会走到这里）
    {
        s32 eb = 1;
        const s32 et = emoji_tile_for(djui_modern_font_utf8_decode(c, &eb));
        if (et >= 0 && gDjuiModernConfig.emojiModern) {
            emoji_render_tile(sEmojiAtlasRGBA, et);
            return;
        }
    }

    // 国旗：头画整面旗，尾巴与孤立 RI 不画
    if (flag_consumes(c)) {
        const s32 ft = flag_tile_at(c);
        if (ft >= 0) {
            emoji_render_tile(sFlagAtlasRGBA, ft);   // 国旗在自己的那张图集里
        } else {
            // 尾字符和孤立 RI 不输出几何，但仍需消费待处理裁剪状态。
            glyph_consume_pending_clip();
        }
        return;
    }
#endif

    s32 bytes = 1;
    const u32 cp = djui_modern_font_utf8_decode(c, &bytes);
    if (cp == 0) { return; }

    // 1) 字符集下标（查不到时使用回退槽显示 '?'）
    const s32 ci = charset_lookup(cp);
    sCur = &sFaces[(sCharsetFace[ci] < sFaceCount) ? sCharsetFace[ci] : 0];

    // 2) 档位
    f32 em = djui_modern_font_atlas_get_scale_hint() * gDjuiModernConfig.textScale;
    if (em < 1.0f) { em = 1.0f; }
    if (em > 4096.0f) { em = 4096.0f; }
    if (codepoint_is_cjk(cp) && em < kSmallCjkMinRasterEm) {
        em = kSmallCjkMinRasterEm;
    }
    const s32 bi = bucket_index_for_em(em);
    const s32 bucket = kRasterBuckets[bi];

    // 3) 槽位（已光栅化的直接使用；其他槽位按需光栅化）
    //    该字符首次被用到时才会为它分配一整行（见 slot_at）
    GlyphSlot* g = slot_at(ci, bi);
    if (g == NULL) {
        glyph_consume_pending_clip();
        return;
    }
    if (g->state == 0) {
        slot_rasterize(g, sCharset[ci], bucket, (s32)sCharsetFace[ci]);
    }
    if (g->state != 1 || g->bw <= 0 || g->bh <= 0 || g->tex == NULL) {
        // 无墨迹 / 生成失败：本帧不画，但同样要消费掉待处理裁剪（见上面的说明）
        glyph_consume_pending_clip();
        return;
    }

    /*
     * 4) 几何。
     *
     * 位图尺寸是光栅像素，光栅 em 基准 = rasterEm（bucket × cjkBoost）。
     * 除以它即得 em 相对值，于是无论是否 boost，屏幕上的字号都一致。
     */
    const f32 invRaster = 1.0f / ((g->rasterEm > 0.001f) ? g->rasterEm : (f32)bucket);

    const f32 x0 = (f32)g->xoff * invRaster;
    const f32 x1 = (f32)(g->xoff + g->bw) * invRaster;
    const f32 y0 = sBaselineY + (-(f32)g->yoff) * invRaster;
    const f32 y1 = sBaselineY - ((f32)(g->yoff + g->bh)) * invRaster;

    /*
     * UV：顶点 tc 是 S10.5 定点（纹素 << 5）。
     * 字形位图对齐在自己的小纹理左上角，所以 UV 就是 (0,0)-(bw,bh) 纹素，
     * 最大 256<<5 = 8192，离 short 上限 32767 很远，不存在溢出。
     */
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

    // 5) 纹理绑定（连续相同字形跳过；不同字形必须重绑，见文件头说明）
    if (sBoundTex != g->tex) {
        bind_glyph_texture(g);
    }

    /*
     * 6) 把 djui_text 设下的待处理纹理裁剪换算到**本字形四边形**的坐标
     *    （四边形 = 墨迹盒 [x0, x1]，字格宽 = DJUI_MODERN_FONT_CELL_W）。
     *    水平精确、垂直清零 —— 见 glyph_fix_pending_clip 的说明。
     */
    glyph_fix_pending_clip(x0, x1 - x0, y0, y0 - y1);

    Gfx* dl = gDisplayListHead;
    memcpy(dl, sGlyphTemplate, sizeof(sGlyphTemplate));
    dl[0].words.w1 = (uintptr_t)VIRTUAL_TO_PHYSICAL(vtx);
    gDisplayListHead += sizeof(sGlyphTemplate) / sizeof(Gfx);
    sGlyphDlMark = gDisplayListHead;
}
