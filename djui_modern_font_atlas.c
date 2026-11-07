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

// 码点哈希表的槽位数，同时是字形集数组的上限。
// 覆盖中文、日文与韩文编码标准后约 28500 条，装载率低于 50%。
#define CHARSET_MAX 65536

#define GLYPH_TEX_MIN 4
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
    u8   state;       // 0 未生成，1 已生成
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

/* 供 ImGui 构建字体图谱的中文字体数据；生命周期与字体链一致。 */
static const u8* sCjkFontData = NULL;
static u32 sCjkFontSize = 0;

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

static s32   sCharsetCount = 0;

typedef struct GlyphDense {
    GlyphSlot slots[BUCKET_COUNT];
} GlyphDense;

typedef struct GlyphEntry {
    u8 bi;
    GlyphSlot slot;
} GlyphEntry;

typedef struct GlyphSet {
    GlyphDense* dense;
    GlyphEntry* sparse;
    s32 count;
    s32 cap;
} GlyphSet;

static GlyphSet* sGlyphSets = NULL;     // [CHARSET_MAX]，按字符分配
static s32      sGlyphEntries = 0;
static s32      sGlyphDenses = 0;


/* 档位 0 是常用的正文尺寸；其余档位按需稀疏分配。 */
static GlyphSlot* slot_at(s32 ci, s32 bi) {
    if (sGlyphSets == NULL || ci < 0 || ci >= CHARSET_MAX || bi < 0 || bi >= BUCKET_COUNT) {
        return NULL;
    }
    GlyphSet* set = &sGlyphSets[ci];
    if (set->dense == NULL) {
        set->dense = (GlyphDense*)calloc(1, sizeof(GlyphDense));
        if (set->dense == NULL) { return NULL; }
        sGlyphDenses++;
    }
    if (bi == 0) {
        return &set->dense->slots[0];
    }
    for (s32 i = 0; i < set->count; i++) {
        if (set->sparse[i].bi == bi) {
            return &set->sparse[i].slot;
        }
    }
    if (set->count >= set->cap) {
        const s32 next = (set->cap == 0) ? 4 : set->cap * 2;
        GlyphEntry* grown = (GlyphEntry*)realloc(set->sparse, (size_t)next * sizeof(GlyphEntry));
        if (grown == NULL) { return NULL; }
        set->sparse = grown;
        set->cap = next;
    }
    GlyphEntry* entry = &set->sparse[set->count++];
    entry->bi = (u8)bi;
    memset(&entry->slot, 0, sizeof(entry->slot));
    sGlyphEntries++;
    return &entry->slot;
}

// 顶点池（每帧重置；djui_render 每帧两遍，见 frame_begin 说明）
#define GLYPH_VERTS 4
#define GLYPH_POOL_BLOCKS 16384
static Vtx*  sVtxPool = NULL;
static u32   sVtxUsed = 0;

// 当前已绑定的纹理（用于跳过重复绑定）
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
 * 码点索引使用开放寻址哈希表。码点 0 作为空槽标记，
 * U+0000 不参与字体渲染。
 */
static u32 sCharHash[CHARSET_MAX];
static u8  sCharsetFace[CHARSET_MAX];

static u32 charset_hash(u32 cp) {
    u32 h = cp * 2654435761u;
    return h ^ (h >> 16);
}

/* 返回码点的槽位；未收录时按字体链选取字体并插入。 */
static s32 charset_add(u32 cp) {
    if (cp == 0 || cp >= 0x110000u) { return -1; }
    u32 i = charset_hash(cp) & (CHARSET_MAX - 1);
    for (s32 n = 0; n < CHARSET_MAX; n++) {
        if (sCharHash[i] == cp) { return (s32)i; }
        if (sCharHash[i] == 0) {
            const s32 face = font_face_for(cp);
            if (face < 0) { return -1; }
            sCharHash[i] = cp;
            sCharsetFace[i] = (u8)face;
            sCharsetCount++;
            return (s32)i;
        }
        i = (i + 1) & (CHARSET_MAX - 1);
    }
    return -1;
}

/*
 * 启动时只收录 ASCII，其余码点在首次渲染时由 charset_lookup 插入。
 * 收录范围由子集化脚本的区段表决定。
 */
static void charset_build(void) {
    memset(sCharHash, 0, sizeof(sCharHash));
    sCharsetCount = 0;

    // ASCII 可见区最高频，先收
    for (u32 cp = 0x20; cp <= 0x7F; cp++) { charset_add(cp); }

    LOG_INFO("djui_modern: charset built (%d entries, capped at %d)",
             sCharsetCount, CHARSET_MAX);
}

/* 返回码点在哈希表中的槽位下标；未收录码点先尝试插入。 */
static s32 charset_lookup(u32 cp) {
    return charset_add(cp);
}

///////////////////////////////////////////////////////////////////////////////
// 字形纹理（每字形一张，不可变）
///////////////////////////////////////////////////////////////////////////////

/*
 * 把 stb 输出的 8 位覆盖度位图落成一张独立的 POT IA8 纹理。
 * 高 4 位强度恒为满值，低 4 位保存覆盖度，字形颜色由 G_CC_FADEA 的 ENV 决定。
 */
static bool glyph_make_texture(GlyphSlot* g, const u8* bmp, s32 bw, s32 bh) {
    const s32 tw = pot_ceil(bw);
    const s32 th = pot_ceil(bh);
    if (tw < bw || th < bh || tw > GLYPH_TEX_MAX || th > GLYPH_TEX_MAX) {
        return false;
    }

    const u32 bytes = (u32)tw * (u32)th;
    if (sGlyphBytes + bytes > GLYPH_BYTES_LIMIT) {
        if (!sBudgetWarned) {
            sBudgetWarned = true;
            LOG_ERROR("djui_modern: glyph texture budget exhausted (%u MB), further glyphs dropped",
                      GLYPH_BYTES_LIMIT / (1024u * 1024u));
        }
        return false;
    }

    u8* tex = (u8*)calloc(bytes, 1);
    if (tex == NULL) { return false; }
    for (s32 y = 0; y < bh; y++) {
        const u8* src = bmp + (size_t)y * bw;
        u8* dst = tex + (size_t)y * tw;
        for (s32 x = 0; x < bw; x++) {
            dst[x] = (u8)(0xF0u | (src[x] >> 4));
        }
    }

    g->tex = tex;
    g->tw = tw;
    g->th = th;
    g->twLog2 = log2_of(tw);
    g->thLog2 = log2_of(th);
    g->bw = bw;
    g->bh = bh;
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

static u8* font_parts_join(const struct DjuiModernFontPart* parts, s32 count, u32* sizeOut) {
    u32 total = 0;
    for (s32 i = 0; i < count; i++) { total += parts[i].size; }
    u8* data = (u8*)malloc(total);
    if (data == NULL) { return NULL; }
    u32 offset = 0;
    for (s32 i = 0; i < count; i++) {
        memcpy(data + offset, parts[i].data, parts[i].size);
        offset += parts[i].size;
    }
    *sizeOut = total;
    return data;
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
 * 字体查找使用真实文件系统接口。fs_init 只把 AppData 用户目录挂入
 * fs_enumerate 的搜索路径，游戏目录不在其中。
 *
 * 依次查找游戏根目录下的 djui_modern/ 与根目录本身，取文件名字典序
 * 最小的 .ttf/.otf，使结果不依赖目录枚举顺序。
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
 * 字体链按注册顺序确定优先级，font_face_for 返回第一个包含码点的字体：
 *   1) 配置项 font_path
 *   2) 游戏根目录中的外部字体
 *   3) 内嵌西文、中日韩与星星字体
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
            // 游戏根目录下的 djui_modern 目录优先
            if (snprintf(dir, sizeof(dir), "%s/djui_modern", exeDir) > 0 && font_try_dir(dir)) {
                haveExternal = true;
            } else if (font_try_dir(exeDir)) {
                haveExternal = true;
            }
        }
    }
    if (haveExternal) {
        LOG_INFO("djui_modern: external font registered first; embedded chain appended as fallback");
    }

    // 内嵌字体追加在外部字体之后，用于补充外部字体缺少的码点。
#if defined(DJUI_MODERN_LATIN_FONT_PRESENT)
    if (sFaceCount < FONT_MAX) {
        u32 size = 0;
        u8* data = font_parts_join(sDjuiModernLatinFont,
            (s32)(sizeof(sDjuiModernLatinFont) / sizeof(sDjuiModernLatinFont[0])), &size);
        if (data != NULL && font_face_setup(&sFaces[sFaceCount], data, size,
                                            true, DJUI_MODERN_LATIN_FONT_NAME)) {
            sFaceCount++;
        }
    }
#endif
#if defined(DJUI_MODERN_CJK_FONT_PRESENT)
    if (sFaceCount < FONT_MAX) {
        u32 size = 0;
        u8* data = font_parts_join(sDjuiModernCjkFont,
            (s32)(sizeof(sDjuiModernCjkFont) / sizeof(sDjuiModernCjkFont[0])), &size);
        if (data != NULL && font_face_setup(&sFaces[sFaceCount], data, size,
                                            true, DJUI_MODERN_CJK_FONT_NAME)) {
            sFaceCount++;
            sCjkFontData = data;
            sCjkFontSize = size;
        }
    }
#endif
#if defined(DJUI_MODERN_STAR_FONT_PRESENT)
    if (sFaceCount < FONT_MAX) {
        u32 size = 0;
        u8* data = font_parts_join(sDjuiModernStarFont,
            (s32)(sizeof(sDjuiModernStarFont) / sizeof(sDjuiModernStarFont[0])), &size);
        if (data != NULL && font_face_setup(&sFaces[sFaceCount], data, size,
                                            true, DJUI_MODERN_STAR_FONT_NAME)) {
            sFaceCount++;
        }
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
     * 基线与行高取字体链首的字体系数，每个字形按自身字体的 upem
     * 换算尺寸，混排时共享同一条基线。
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
     * 下伸部超出一行高度的部分需要计入聊天框硬裁剪，否则 g、y、p、q、j
     * 的下伸笔画会被截断。
     */
    {
        const f32 descUsed = 1.0f + sBaselineY;   // 基线下方的可用空间（= 钳位后的 desc）
        const f32 descNeed = -sDescentEm;         // 字体真实下伸
        sClipPadBottomEm = (descNeed > descUsed) ? (descNeed - descUsed) : 0.0f;
    }

    charset_build();

    sGlyphSets = (GlyphSet*)calloc((size_t)CHARSET_MAX, sizeof(GlyphSet));
    if (sGlyphSets == NULL) {
        LOG_ERROR("djui_modern: glyph set alloc failed (%d slots)", CHARSET_MAX);
        font_free_all();
        return false;
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
    LOG_INFO("djui_modern: font slots = %d chars, bucket 0 dense + other buckets sparse",
             sCharsetCount);
    LOG_INFO("djui_modern: heap budget: glyph set table %.1f MB, vtx pool %.0f KB",
             (double)CHARSET_MAX * sizeof(GlyphSet) / (1024.0 * 1024.0),
             (double)(sizeof(Vtx) * GLYPH_VERTS * GLYPH_POOL_BLOCKS) / 1024.0);

    /*
     * 字符覆盖抽检。
     *
     * 用于诊断指定字符是否已收录：
     *   (a) 字符集没收录（i=-1）  → 字符集构建的问题
     *   (b) 收录了但渲染链路没画出来 → 纹理上传/绑定的问题
     * 抽检包含 U+963F「阿」，用于确认字符集不是只收录某个区间的前 N 个码点；
     * 和 U+300C「」，CJK 标点区段的首字符附近。
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
    if (sGlyphSets != NULL) {
        LOG_INFO("djui_modern: glyph storage %d dense chars, %d sparse buckets",
                 sGlyphDenses, sGlyphEntries);
        for (s32 ci = 0; ci < CHARSET_MAX; ci++) {
            GlyphSet* set = &sGlyphSets[ci];
            if (set->dense != NULL) {
                free(set->dense->slots[0].tex);
            }
            for (s32 bi = 0; bi < set->count; bi++) {
                free(set->sparse[bi].slot.tex);
            }
            free(set->dense);
            free(set->sparse);
        }
        free(sGlyphSets);
    }
    sGlyphBytes = 0;
    sGlyphSets = NULL;
    sGlyphEntries = 0;
    sGlyphDenses = 0;
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
 * 半帧重渲的第二遍不清零顶点池。
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

const unsigned char* djui_modern_font_atlas_cjk_data(unsigned int* size) {
    if (size != NULL) { *size = sCjkFontSize; }
    return sCjkFontData;
}

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
 *   而档位表包含 16，此时比值为 1.00，无法降采样。
 *
 * 位图严格大于屏幕 em 时，双线性缩小采样可提供盒式滤波效果，
 * 使笔画边缘获得平滑的灰度过渡。
 *
 * 取值：1.25 即可让每个屏幕像素至少覆盖一个完整纹素周期；
 *   再往上画质收益迅速衰减（人眼几乎看不出 1.25 与 1.6 的差别），
 *   而纹理面积按平方增长（1.25² 约增加 56%，1.6² 增加 156%）。
 *   1.25 与档位表本身较密的间距（12→16→20→26）配合，绝大多数 em
 *   会落到"下一个档"，ratio 落在 [1.0, 1.7] 区间，兼顾清晰与内存。
 */
#define kGlyphSupersample 1.25f

/* 小字号汉字需要更多纹理像素，避免密集笔画在降采样后粘连。 */
#define kSmallCjkMinRasterEm 40.0f

/*
 * 屏幕 em 像素 -> 档位下标。
 *
 * 取第一个不小于 em × kGlyphSupersample 的档位。因为 kGlyphSupersample > 1，
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
    sCur = &sFaces[0];   // 等宽检测只使用字体链首
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
    // 同一字符重复时仍查询字偶间距，字体可能为这类字符对提供数据。
    // 跨字体的字偶间距没有共同度量体系，直接返回零。
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
     * 空格使用字体的真实 advance，与其他字符保持同一套度量。
     * 字体没有空格字形时才退回 0.25em 的经验值。
     */
    if (cp == ' ') {
        const s32 sci = charset_lookup(cp);
        const s32 face = (sci >= 0) ? sCharsetFace[sci] : 0;
        sCur = &sFaces[(face < sFaceCount) ? face : 0];
        const s32 gs = stbtt_FindGlyphIndex(&sFont, ' ');
        s32 sAdv = 0, sLsb = 0;
        if (gs != 0) { stbtt_GetGlyphHMetrics(&sFont, gs, &sAdv, &sLsb); }
        return (sAdv > 0) ? ((f32)sAdv / sUnitsPerEm) : 0.25f;
    }

    // Emoji 固定 1em 宽（与原版 32x32 emoji 贴图的推进量一致）
    if (djui_modern_emoji_available(c)) { return 1.0f; }

    // 国旗：头 1em、尾 0（见 flag_tile_at 的说明）
    if (flag_consumes(c)) { return (flag_tile_at(c) >= 0) ? 1.0f : 0.0f; }

    // 度量必须来自"这个字所属的那份字体"；未收录时使用回退字符
    s32 ci = charset_lookup(cp);
    if (ci < 0) { ci = charset_lookup('?'); }
    if (ci < 0) { return 0.5f; }
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
     * 字偶间距计入当前字符的推进量，决定下一个字符的位置。
     * 前瞻只读取 c 与 c+bytes，测量与渲染使用同一结果。
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
 * gDPSetTextureOverrideDjui 的宽高参数是边长的 log2，gfx_pc.c 会左移
 * 还原为纹素数，因此纹理边长必须是 2 的幂。gDPLoadTextureBlockWithoutTexture
 * 的宽高是纹素数，两者必须表示同一张纹理。
 */
static void bind_glyph_texture(const GlyphSlot* g) {
    gDPLoadTextureBlockWithoutTexture(gDisplayListHead++, NULL, G_IM_FMT_IA, G_IM_SIZ_8b,
        g->tw, g->th, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    gDPSetTextureOverrideDjui(gDisplayListHead++, g->tex,
        g->twLog2, g->thLog2, G_IM_FMT_IA, G_IM_SIZ_8b);
    *(gDisplayListHead++) = (Gfx) gsSPExecuteDjui(G_TEXOVERRIDE_DJUI);
    sBoundTex = g->tex;
}

///////////////////////////////////////////////////////////////////////////////
// Emoji（离线烘焙的 RGBA 图集）
///////////////////////////////////////////////////////////////////////////////
/*
 * 原版 Emoji 字体使用 sbix 位图而不是轮廓，stb_truetype 不支持 sbix、
 * CBDT 和 COLR，因此彩色 Emoji 由 tools/make_emoji_atlas.py 离线生成图集。
 *
 * tile 下标等于码点在升序表中的位置，查找使用二分搜索。Emoji 与国旗是
 * 两张独立图集，染色的 RGBA 数据不能改为单通道纹理。
 */

#if defined(DJUI_MODERN_EMOJI_COUNT)
/*
 * 内嵌图集以 PNG 保存，启动时解码为 RGBA32。解码失败时图集指针保持空，
 * 渲染回退到原版 Emoji。
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
 * 返回国旗图集内的 tile 下标，未找到时返回 -1。
 * 下标从零开始，不叠加 Emoji 图集的条目数。
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

/* 返回该字符是否由现代 Emoji 图集绘制。 */
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
 * 绘制一个 tile。atlas 是 Emoji 或国旗的 RGBA32 缓冲，idx 是该图集内的
 * tile 下标。两张图布局一致，几何与 UV 共用，只有数据指针不同。
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

    // 图集内容不可变，按运行时缓冲地址跳过重复绑定。
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

    // 绑定时机在单个字形内，开新一批文字时必须清除上次的绑定缓存。
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
    if (!sReady || sGlyphSets == NULL || c == NULL || *c == '\0') { return; }
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

    // 1) 字符集下标（查不到时使用回退字符）
    s32 ci = charset_lookup(cp);
    if (ci < 0) { ci = charset_lookup('?'); }
    if (ci < 0) { glyph_consume_pending_clip(); return; }
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

    // 3) 槽位（已光栅化的直接使用；其他槽位按需分配）
    GlyphSlot* g = slot_at(ci, bi);
    if (g == NULL) {
        glyph_consume_pending_clip();
        return;
    }
    if (g->state == 0) {
        slot_rasterize(g, sCharHash[ci], bucket, (s32)sCharsetFace[ci]);
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

    if (sBoundTex != g->tex) {
        bind_glyph_texture(g);
    }

    /*
     * 把待处理的纹理裁剪换算到当前字形四边形的坐标。水平按墨迹盒换算，
     * 垂直只对聊天框保留裁剪。
     */
    glyph_fix_pending_clip(x0, x1 - x0, y0, y0 - y1);

    Gfx* dl = gDisplayListHead;
    memcpy(dl, sGlyphTemplate, sizeof(sGlyphTemplate));
    dl[0].words.w1 = (uintptr_t)VIRTUAL_TO_PHYSICAL(vtx);
    gDisplayListHead += sizeof(sGlyphTemplate) / sizeof(Gfx);
    sGlyphDlMark = gDisplayListHead;
}
