#include <math.h>
#include <string.h>
#include "pc/djui/djui.h"
#include "djui_modern.h"
#include "djui_modern_config.h"
#include "djui_modern_rrect.h"
#include "djui_modern_gbi.h"   // gsSP2TrianglesDjui（模块私有，不改上游 gbi_extension.h）
#include "pc/debuglog.h"
#include "engine/math_util.h"

/* 圆角矩形渲染原语。填充使用九宫格遮罩，边框使用三角化环带。 */

/* 遮罩分辨率。角部纹素数约为总边长的 28%。 */
#define RRECT_MASK_TEXELS   64
// 64 纹素对应 log2 6。
#define RRECT_MASK_LOG2     6
// 角区纹素数。
#define RRECT_CORNER_TEXELS 18
/* SDF 角弧半径与采样窗口对齐。 */
#define RRECT_CORNER_ARC    ((f32)RRECT_CORNER_TEXELS - 0.5f)
#define RRECT_UV_PER_TEXEL  32          // S10.5 定点（纹素 << 5），与遮罩大小无关
#define RRECT_VERTS         24          // 4 行 x 6 列

/* UV 断点使用纹素序号；gfx_pc 会在线性过滤时补齐半纹素。 */
#define RRECT_U_FIRST    0
#define RRECT_U_CORNER   (RRECT_CORNER_TEXELS * RRECT_UV_PER_TEXEL - RRECT_UV_PER_TEXEL / 2)  // 17.5 纹素
#define RRECT_U_FAR      ((RRECT_MASK_TEXELS - 1) * RRECT_UV_PER_TEXEL)                       // 63 纹素
#define RRECT_U_STRAIGHT (RRECT_U_FAR - RRECT_U_CORNER)                                       // 45.5 纹素

// 每角 12 段，共 49 个轮廓点。
#define RRECT_BAND_SEG_CORNER  12
#define RRECT_BAND_POINTS      (4 * RRECT_BAND_SEG_CORNER + 1)

// 每批最多 18 个点，接缝点由相邻批次共用。
#define RRECT_BAND_BATCH_PTS   18

/* 边框边缘羽化宽度，单位为屏幕像素。 */
#define RRECT_BAND_FEATHER     1.0f

// 半径上限，单位为 320x240 DJUI 坐标。
#define RRECT_MAX_RADIUS    24.0f

// 顶点池容量。每帧复用，不在绘制路径中分配。
#define RRECT_POOL_BLOCKS   2560

// 角部轮廓：羊角螺线 + 正圆弧 + 羊角螺线

/* 角部极径表：启动时积分羊角螺线与圆弧，并归一化到单位切距。 */
#define RRECT_RHO_SEGS  256
// 曲线积分步数。
#define RRECT_LUT_STEPS 4096
static f32 sCornerRho[RRECT_RHO_SEGS + 1];
static f32 sCornerMinRadius = 1.0f;

static void rrect_build_corner_lut(void) {
    f32 round = gDjuiModernConfig.cornerRoundness;
    if (round < 0.0f) { round = 0.0f; }
    if (round > 1.0f) { round = 1.0f; }
    const f32 alpha = (1.0f - round) * ((f32)M_PI * 0.25f);  // 每个过渡段转过的角
    const f32 L1    = 2.0f * alpha;                          // 羊角螺线段长（R=1）
    const f32 L2    = (f32)M_PI * 0.5f - 2.0f * alpha;       // 正圆弧段长
    const f32 Ltot  = 2.0f * L1 + L2;

    const f32 ds = Ltot / (f32)RRECT_LUT_STEPS;

    // 第一遍：积分出整条曲线。起点 (1,0)，起始切向竖直，总共转过 90 度。
    static f32 curveX[RRECT_LUT_STEPS + 1];
    static f32 curveY[RRECT_LUT_STEPS + 1];
    f32 x = 1.0f, y = 0.0f, phi = (f32)M_PI * 0.5f;
    curveX[0] = x;
    curveY[0] = y;
    for (s32 i = 0; i < RRECT_LUT_STEPS; i++) {
        const f32 s = ((f32)i + 0.5f) * ds;
        f32 k;
        if (s < L1) {
            k = (L1 > 1e-6f) ? (s / L1) : 1.0f;              // 曲率 0 -> 1
        } else if (s < L1 + L2) {
            k = 1.0f;                                        // 正圆弧
        } else {
            k = (L1 > 1e-6f) ? (1.0f - (s - L1 - L2) / L1) : 0.0f;   // 1 -> 0
        }
        if (k < 0.0f) { k = 0.0f; }
        if (k > 1.0f) { k = 1.0f; }

        // 中点法积分（曲率在段内线性，中点法精度足够）
        phi += k * ds * 0.5f;
        x   += cosf(phi) * ds;
        y   += sinf(phi) * ds;
        phi += k * ds * 0.5f;

        curveX[i + 1] = x;
        curveY[i + 1] = y;
    }

    // 两条切边的切点到角点的距离就是曲线终点的 y；把它归一成 1，
    // 于是"切线距离 = 角部的视觉尺寸 r"。
    f32 tanDist = curveY[RRECT_LUT_STEPS];
    if (tanDist < 1e-6f) { tanDist = 1.0f; }
    sCornerMinRadius = 1.0f / tanDist;

    // 第二遍：把曲线变换到规范形 p' = (p - (1, yEnd)) / tanDist + (1, 1)，
    // 再按极角均匀采样它的极径。曲线极角单调，用一个前向游标即可。
    s32 next = 0;
    for (s32 i = 0; i <= RRECT_RHO_SEGS; i++) {
        const f32 target = ((f32)i / (f32)RRECT_RHO_SEGS) * ((f32)M_PI * 0.5f);
        while (next < RRECT_LUT_STEPS) {
            const f32 nx = (curveX[next + 1] - 1.0f) / tanDist + 1.0f;
            const f32 ny = (curveY[next + 1] - tanDist) / tanDist + 1.0f;
            if (atan2f(ny, nx) >= target) { break; }
            next++;
        }
        const f32 px = (curveX[next] - 1.0f) / tanDist + 1.0f;
        const f32 py = (curveY[next] - tanDist) / tanDist + 1.0f;
        sCornerRho[i] = sqrtf(px * px + py * py);
    }
    sCornerRho[0] = 1.0f;                                     // theta = 0 -> 切点 (1,0)
    sCornerRho[RRECT_RHO_SEGS] = 1.0f;                        // theta = pi/2 -> 切点 (0,1)

    LOG_INFO("djui_modern: rrect corner = clothoid+arc+clothoid "
             "(roundness=%.2f, transition=%.1f deg, tangent=%.4f, mid-rho=%.4f, min-radius=%.4f)",
             round, alpha * 180.0f / (f32)M_PI, tanDist,
             sCornerRho[RRECT_RHO_SEGS / 2], sCornerMinRadius);
}

/* 极角 theta（0 到 pi/2）方向的归一化半径 rho。 */
static inline f32 rrect_corner_rho(f32 theta) {
    const f32 halfPi = (f32)M_PI * 0.5f;
    if (theta <= 0.0f)   { return sCornerRho[0]; }
    if (theta >= halfPi) { return sCornerRho[RRECT_RHO_SEGS]; }
    const f32 t = theta * ((f32)RRECT_RHO_SEGS / halfPi);
    const s32 i = (s32)t;
    const f32 f = t - (f32)i;
    return sCornerRho[i] + (sCornerRho[i + 1] - sCornerRho[i]) * f;
}

/*
 * 有符号距离：点到"角半径 r 的圆角正方形"（中心在原点）的边界。
 * d <= 0 表示在内部。qx、qy 是点相对内框角的坐标（已取绝对值）。
 *
 * 角部分支用的是径向距离 |q| - rho(theta) * r，只有对正圆弧才是严格
 * 距离，羊角螺线段上是近似；但零等值面（边界）是精确的，遮罩只关心
 * 边界内外与超采样覆盖度，近似部分只影响一两个 texel 的柔和度。
 */
static inline f32 rrect_sdf(f32 px, f32 py, f32 inner, f32 r) {
    const f32 qx = fabsf(px) - inner;
    const f32 qy = fabsf(py) - inner;

    if (qx > 0.0f && qy > 0.0f) {
        return sqrtf(qx * qx + qy * qy) - rrect_corner_rho(atan2f(qy, qx)) * r;
    }

    const f32 ox = fmaxf(qx, 0.0f);
    const f32 oy = fmaxf(qy, 0.0f);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - r;
}

// 遮罩纹理

static u8* sMaskTexture = NULL;

void djui_modern_rrect_init(void) {
    if (sMaskTexture != NULL) { return; }

    // 角部轮廓的 rho 表（数值积分一次）
    rrect_build_corner_lut();

    sMaskTexture = malloc(RRECT_MASK_TEXELS * RRECT_MASK_TEXELS * 4);
    if (sMaskTexture == NULL) {
        LOG_ERROR("djui_modern: failed to allocate rounded-rect mask");
        free(sMaskTexture); sMaskTexture = NULL;
        return;
    }

    const f32 half  = (f32)RRECT_MASK_TEXELS * 0.5f;
    const f32 inner = half - (f32)RRECT_CORNER_TEXELS;

    // 4x4 超采样求覆盖度（消除阶梯，得到柔和的边缘）
    const s32 SS = 4;
    u32 insideCount = 0;
    for (s32 y = 0; y < RRECT_MASK_TEXELS; y++) {
        for (s32 x = 0; x < RRECT_MASK_TEXELS; x++) {
            s32 hits = 0;
            for (s32 sy = 0; sy < SS; sy++) {
                for (s32 sx = 0; sx < SS; sx++) {
                    const f32 px = (f32)x + ((f32)sx + 0.5f) / (f32)SS - half;
                    const f32 py = (f32)y + ((f32)sy + 0.5f) / (f32)SS - half;
                    if (rrect_sdf(px, py, inner, RRECT_CORNER_ARC) <= 0.0f) {
                        hits++;
                    }
                }
            }
            const u8 cov = (u8)((hits * 255 + (SS * SS) / 2) / (SS * SS));
            if (cov > 0) { insideCount++; }

            u8* px = &sMaskTexture[((size_t)y * RRECT_MASK_TEXELS + x) * 4];
            px[0] = 255;
            px[1] = 255;
            px[2] = 255;
            px[3] = cov;
        }
    }

    LOG_INFO("djui_modern: rrect mask %dx%d (corner=%dt arc=%.1ft, filled=%u)",
             RRECT_MASK_TEXELS, RRECT_MASK_TEXELS, RRECT_CORNER_TEXELS,
             RRECT_CORNER_ARC, insideCount);
    {
        // 采样几个关键点，验证形状对称且直边满覆盖
        const u32 mid    = ((u32)(RRECT_MASK_TEXELS / 2) * RRECT_MASK_TEXELS + RRECT_MASK_TEXELS / 2) * 4;
        const u32 tEdge  = (0 * RRECT_MASK_TEXELS + RRECT_MASK_TEXELS / 2) * 4;
        const u32 bEdge  = ((RRECT_MASK_TEXELS - 1) * RRECT_MASK_TEXELS + RRECT_MASK_TEXELS / 2) * 4;
        const u32 lEdge  = ((RRECT_MASK_TEXELS / 2) * RRECT_MASK_TEXELS + 0) * 4;
        const u32 rEdge  = ((RRECT_MASK_TEXELS / 2) * RRECT_MASK_TEXELS + RRECT_MASK_TEXELS - 1) * 4;
        const u32 cornerO = (1 * RRECT_MASK_TEXELS + 1) * 4;
        LOG_INFO("djui_modern: mask alpha center=%u T=%u B=%u L=%u R=%u cornerOut=%u",
                 sMaskTexture[mid + 3], sMaskTexture[tEdge + 3], sMaskTexture[bEdge + 3],
                 sMaskTexture[lEdge + 3], sMaskTexture[rEdge + 3], sMaskTexture[cornerO + 3]);
    }

    djui_modern_rrect_frame_begin();
}

// 顶点池

static Vtx* sVtxPool = NULL;
static u32  sVtxUsed = 0;

bool djui_modern_rrect_ready(void) {
    return sMaskTexture != NULL;
}

void djui_modern_rrect_frame_begin(void) {
    sVtxUsed = 0;
}

void djui_modern_rrect_shutdown(void) {
    if (sVtxPool != NULL)     { free(sVtxPool);     sVtxPool = NULL; }
    if (sMaskTexture != NULL) { free(sMaskTexture); sMaskTexture = NULL; }
    sVtxUsed = 0;
}

/* 从同一个顶点池分配 n 个连续顶点（九宫格用固定 24）。 */
static Vtx* vtx_alloc_count(s32 n) {
    if (sVtxPool == NULL) {
        sVtxPool = malloc(sizeof(Vtx) * RRECT_VERTS * RRECT_POOL_BLOCKS);
        if (sVtxPool == NULL) {
            LOG_ERROR("djui_modern: failed to allocate rrect vertex pool");
            return NULL;
        }
        memset(sVtxPool, 0, sizeof(Vtx) * RRECT_VERTS * RRECT_POOL_BLOCKS);
    }
    if (n <= 0 || sVtxUsed + (u32)n > RRECT_VERTS * RRECT_POOL_BLOCKS) { return NULL; }
    Vtx* v = &sVtxPool[sVtxUsed];
    sVtxUsed += (u32)n;
    return v;
}

static Vtx* vtx_alloc(void) { return vtx_alloc_count(RRECT_VERTS); }

// 静态 display list 模板

/*
 * 顶点索引（行优先，4 行 x 6 列 = 24 个顶点）：
 *   行 0 (y=0)     0..5
 *   行 1 (y=ry)    6..11
 *   行 2 (y=h-ry) 12..17
 *   行 3 (y=h)    18..23
 *
 * 9 个四边形（3 带 x 3 列）。带 b、列对 p 的四个顶点：
 *   A = b*6 + 2p,  B = A+1,  C = (b+1)*6 + 2p + 1,  D = (b+1)*6 + 2p
 *
 * 命令索引：
 *   0  gsDPPipeSync
 *   1  gsSPClearGeometryMode
 *   2  gsDPSetCycleType(1CYCLE)
 *   3  gsDPSetCombineMode(MODULATEIA)
 *   4  gsDPSetRenderMode
 *   5  gsDPSetTextureFilter
 *   6  gsDPSetTexturePersp(NONE)
 *   7  gsSPTexture(ON)
 *   8  gsSPExecuteDjui(OVERRIDE)   <- override 由调用方前置发出
 *   9  gsSPVertexNonGlobal         <- 运行期替换顶点指针
 *   10..18 九个 gsSP2Triangles
 *   19 gsSPTexture(OFF)
 *   20 gsDPSetCombineMode(SHADE)
 *   21 gsDPSetTexturePersp(PERSP)
 *   22 gsSPSetGeometryMode
 *   无 gsSPEndDisplayList：本模板是内联片段
 */
static const Gfx sRrectTemplate[] = {
    gsDPPipeSync(),
    gsSPClearGeometryMode(G_LIGHTING | G_CULL_BOTH),
    gsDPSetCycleType(G_CYC_1CYCLE),
    gsDPSetCombineMode(G_CC_MODULATEIA, G_CC_MODULATEIA),
    gsDPSetRenderMode(G_RM_XLU_SURF, G_RM_XLU_SURF2),
    gsDPSetTextureFilter(G_TF_BILERP),
    gsDPSetTexturePersp(G_TP_NONE),
    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON),

    gsSPExecuteDjui(G_TEXOVERRIDE_DJUI),

    gsSPVertexNonGlobal(NULL, RRECT_VERTS, 0),

    // 必须用 gsSP2TrianglesDjui（G_TRI2_EXT），不能用标准 gsSP2Triangles
    // （G_TRI2）：前者走 djui_gfx_sp_simple_tri1（直接送光栅器，适配
    // DJUI 的屏幕空间 2D 几何），后者走 gfx_sp_tri1（完整 3D 顶点管线，
    // 会重新做矩阵变换与投影）。两者索引编码一致，但顶点处理路径不同。
    // 详见 include/PR/gbi_extension.h 里 gsSP2TrianglesDjui 的说明。
    gsSP2TrianglesDjui(0, 1, 7, 0, 0, 7, 6, 0),
    gsSP2TrianglesDjui(2, 3, 9, 0, 2, 9, 8, 0),
    gsSP2TrianglesDjui(4, 5, 11, 0, 4, 11, 10, 0),
    gsSP2TrianglesDjui(6, 7, 13, 0, 6, 13, 12, 0),
    gsSP2TrianglesDjui(8, 9, 15, 0, 8, 15, 14, 0),
    gsSP2TrianglesDjui(10, 11, 17, 0, 10, 17, 16, 0),
    gsSP2TrianglesDjui(12, 13, 19, 0, 12, 19, 18, 0),
    gsSP2TrianglesDjui(14, 15, 21, 0, 14, 21, 20, 0),
    gsSP2TrianglesDjui(16, 17, 23, 0, 16, 23, 22, 0),

    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF),
    gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),
    gsDPSetTexturePersp(G_TP_PERSP),
    gsSPSetGeometryMode(G_LIGHTING | G_CULL_BACK),
};

#define RRECT_VTX_CMD_INDEX 9
// 模板里"顶点命令之后"的部分（gsSPTexture(OFF) 起），共 4 条，到数组
// 末尾（整个模板 23 条命令）。头 = 0..9（10 条）。
#define RRECT_TEMPLATE_TAIL_INDEX 19
#define RRECT_TEMPLATE_HEAD_CMDS  10
#define RRECT_TEMPLATE_TAIL_CMDS  4
#define RRECT_TEMPLATE_CMDS       (RRECT_TEMPLATE_TAIL_INDEX + RRECT_TEMPLATE_TAIL_CMDS)

// 编译期自检：索引常量必须和模板数组的真实长度一致。数错了会在运行期
// 越界读模板，把垃圾当成 GBI 命令塞进 display list，整帧管道状态全乱，
// 而不是"少画一条"那种局部错误。
_Static_assert(sizeof(sRrectTemplate) / sizeof(Gfx) == RRECT_TEMPLATE_CMDS,
               "sRrectTemplate command count mismatch");

/*
 * 遮罩绑定：tile 元数据 -> override -> execute。
 *
 * gDPSetTextureOverrideDjui 的 (w,h) 参数是边长的 log2，不是纹素数：
 * gfx_pc 内部按 1<<v 还原尺寸，且字段只有 8 位。64 纹素对应 6。
 * 传错会按错误尺寸建 GL 纹理，采样只取到 1 个像素。
 *
 * 后面那对 (texels, texels) 是 gDPLoadTextureBlock 风格的纹素数，只用来
 * 算 lrs/lrt（UV 归一化基准），与上面的 log2 是两套参数。
 */
static void rrect_bind_texture(const u8* tex, s32 texels, s32 log2size) {
    gDPLoadTextureBlockWithoutTexture(gDisplayListHead++, NULL, G_IM_FMT_RGBA, G_IM_SIZ_16b,
        texels, texels, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    gDPSetTextureOverrideDjui(gDisplayListHead++, (void*)tex,
        log2size, log2size, G_IM_FMT_RGBA, G_IM_SIZ_32b);
    *(gDisplayListHead++) = (Gfx) gsSPExecuteDjui(G_TEXOVERRIDE_DJUI);
}

/* 整块遮罩（填充用）。 */
static void rrect_bind_mask(void) {
    rrect_bind_texture(sMaskTexture, RRECT_MASK_TEXELS, RRECT_MASK_LOG2);
}

/* 把静态模板拷进 display list，并把第 9 条命令（gsSPVertexNonGlobal）的指针换成 vtx。 */
static void rrect_submit(Vtx* vtx, const Gfx* tmpl, s32 ncmds) {
    Gfx* dl = gDisplayListHead;
    memcpy(dl, tmpl, ncmds * sizeof(Gfx));
    dl[RRECT_VTX_CMD_INDEX].words.w1 = (uintptr_t)VIRTUAL_TO_PHYSICAL(vtx);
    gDisplayListHead += ncmds;
}

// 半径策略

static void rrect_radius_for(enum DjuiModernRadius kind, f32 explicitRadius,
                             f32 w, f32 h, f32* outR, f32* outRy) {
    f32 r;
    switch (kind) {
        case DJUI_MODERN_RADIUS_PANEL:   r = gDjuiModernConfig.radiusPanel;   break;
        case DJUI_MODERN_RADIUS_ELEMENT: r = gDjuiModernConfig.radiusElement; break;
        case DJUI_MODERN_RADIUS_PILL:    r = gDjuiModernConfig.radiusPill;    break;
        case DJUI_MODERN_RADIUS_NONE:
        default:                         r = 0.0f;                            break;
    }

    if (explicitRadius >= 0.0f) {
        r = explicitRadius;
    } else if (kind == DJUI_MODERN_RADIUS_PILL && gDjuiModernConfig.radiusPill < 0.0f) {
        r = h * 0.5f;
    }

    // 半径不得超过短边一半（否则两端圆弧相切，矩形退化成胶囊）
    r = fminf(r, fminf(w, h) * 0.5f);
    r = fminf(r, RRECT_MAX_RADIUS);
    r = fmaxf(r, 0.0f);

    // 纵横两向用同一个半径，配置里调大时圆角才能严格跟随配置值
    f32 ry = r;
    ry = fminf(ry, h * 0.5f);
    ry = fmaxf(ry, 0.0f);

    *outR  = r;
    *outRy = ry;
}

// 绘制

static inline struct DjuiColor lerp_color(struct DjuiColor a, struct DjuiColor b, f32 t) {
    struct DjuiColor c;
    c.r = (u8)((f32)a.r + ((f32)b.r - (f32)a.r) * t);
    c.g = (u8)((f32)a.g + ((f32)b.g - (f32)a.g) * t);
    c.b = (u8)((f32)a.b + ((f32)b.b - (f32)a.b) * t);
    c.a = (u8)((f32)a.a + ((f32)b.a - (f32)a.a) * t);
    return c;
}

/*
 * 把一条坐标轴上的断点收拢到 [lo, hi] 内。
 *
 * 填充的圆角几何按整体尺寸生成，只在画的时候按 clip 截一刀。断点被推
 * 到边界上即可，UV 也跟着钳到对应边界值，落在 clip 外的部分本来就不
 * 显示，钳过去不会产生可见误差。
 *
 * 这里不做 UV 内插：九宫格里角区（r 像素比 C 纹素）与直边区（w-2r
 * 像素比 M-2C 纹素）的映射比例天然不同，内插出来是错的。
 */
static void rrect_clamp_axis(f32* vals, u16* uvs, s32 n, f32 lo, f32 hi) {
    for (s32 i = 0; i < n; i++) {
        if (vals[i] < lo) { vals[i] = lo; if (uvs[i] < uvs[0]) { uvs[i] = uvs[0]; } }
        if (vals[i] > hi) { vals[i] = hi; if (uvs[i] > uvs[n - 1]) { uvs[i] = uvs[n - 1]; } }
    }
    // 断点必须单调不减，否则四边形会自交
    for (s32 i = 1; i < n; i++) {
        if (vals[i] < vals[i - 1]) { vals[i] = vals[i - 1]; uvs[i] = uvs[i - 1]; }
    }
}

/*
 * 画一个圆角矩形（可选渐变），只保留落在 clip 内的部分。
 *
 * 几何与圆角半径都按"comp 内缩 inset 之后"的尺寸生成，再把断点收拢进
 * clip。inset = 0 就是整块矩形；带边框时传边框宽度，让填充的角正好落在
 * 环带内沿上。传 explicitRadius = 0 时就是纯直角矩形，边框退化路径的
 * 四条直边就是这么画的。
 */
static void rrect_emit(struct DjuiBaseRect* clip, struct DjuiBaseRect* comp,
                       enum DjuiModernRadius radiusKind, f32 explicitRadius,
                       struct DjuiColor topColor, struct DjuiColor bottomColor,
                       u8 alphaScale, f32 inset) {
    if (sMaskTexture == NULL) { return; }

    struct DjuiBaseRect rc = *comp;
    if (inset > 0.0f) {
        rc.x += inset;
        rc.y += inset;
        rc.width  -= inset * 2.0f;
        rc.height -= inset * 2.0f;
        if (rc.width <= 0.0f || rc.height <= 0.0f) { return; }
    }

    const f32 x1 = fmaxf(rc.x, clip->x);
    const f32 y1 = fmaxf(rc.y, clip->y);
    const f32 x2 = fminf(rc.x + rc.width,  clip->x + clip->width);
    const f32 y2 = fminf(rc.y + rc.height, clip->y + clip->height);
    if (x2 <= x1 || y2 <= y1) { return; }

    topColor.a    = (u8)(((s32)topColor.a    * alphaScale) / 255);
    bottomColor.a = (u8)(((s32)bottomColor.a * alphaScale) / 255);

    // 半径按整体尺寸判定（不是裁剪后的尺寸）
    f32 r = 0.0f, ry = 0.0f;
    rrect_radius_for(radiusKind, explicitRadius, rc.width, rc.height, &r, &ry);

    // 断点按整体几何生成（屏幕坐标）
    f32 xs[6] = { rc.x,
                  rc.x + r,           rc.x + r,
                  rc.x + rc.width - r, rc.x + rc.width - r,
                  rc.x + rc.width };
    f32 ys[4] = { rc.y,
                  rc.y + ry,
                  rc.y + rc.height - ry,
                  rc.y + rc.height };

    // UV 断点是常量（见常量定义：外沿是纹素中心，角区 17.5 个纹素，直边区 28 个）
    u16 us[6] = { RRECT_U_FIRST, RRECT_U_CORNER, RRECT_U_CORNER, RRECT_U_STRAIGHT, RRECT_U_STRAIGHT, RRECT_U_FAR };
    u16 vs[4] = { RRECT_U_FIRST, RRECT_U_CORNER, RRECT_U_STRAIGHT, RRECT_U_FAR };

    // 收拢到 clip
    rrect_clamp_axis(xs, us, 6, x1, x2);
    rrect_clamp_axis(ys, vs, 4, y1, y2);

    // 收拢后可能整块都被压到边界，此时跳过
    if (xs[5] <= xs[0] || ys[3] <= ys[0]) { return; }

    Vtx* vtx = vtx_alloc();
    if (vtx == NULL) { return; }

    // 矩阵与原版矩形渲染一致：先平移到左上角，再按尺寸缩放。
    const f32 w = x2 - x1;
    const f32 h = y2 - y1;

    // 渐变方向：从底（row3）到顶（row0）
    const f32 topY = ys[0], botY = ys[3];
    const f32 span = (botY > topY) ? (botY - topY) : 1.0f;

    for (s32 row = 0; row < 4; row++) {
        // t = 距屏幕底部的比例：底行 0 -> bottomColor，顶行 1 -> topColor
        const f32 t = (botY - ys[row]) / span;
        const struct DjuiColor col = (t <= 0.0f) ? bottomColor
                                  : (t >= 1.0f) ? topColor
                                  : lerp_color(bottomColor, topColor, t);
        for (s32 c = 0; c < 6; c++) {
            Vtx* v = &vtx[row * 6 + c];
            memset(v, 0, sizeof(Vtx));
            v->v.ob[0] = (xs[c] - x1) / w;
            v->v.ob[1] = (y1 - ys[row]) / h;
            v->v.tc[0] = us[c];
            v->v.tc[1] = vs[row];
            v->v.cn[0] = col.r;
            v->v.cn[1] = col.g;
            v->v.cn[2] = col.b;
            v->v.cn[3] = col.a;
        }
    }

    f32 tx = x1, ty = y1;
    djui_gfx_position_translate(&tx, &ty);
    create_dl_translation_matrix(DJUI_MTX_PUSH, tx, ty, 0);

    f32 tw = w, th = h;
    djui_gfx_scale_translate(&tw, &th);
    create_dl_scale_matrix(DJUI_MTX_NOPUSH, tw, th, 1.0f);

    rrect_bind_mask();
    rrect_submit(vtx, sRrectTemplate, RRECT_TEMPLATE_CMDS);

    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);
}

/* 计算有效边框宽度，避免环带自交。 */
static f32 rrect_band_width(struct DjuiBaseRect* comp, f32 ro, f32 ry, f32 bw) {
    bw = fminf(bw, fminf(comp->width, comp->height) * 0.5f);
    if (bw <= 0.001f) { return 0.0f; }
    const f32 rox = fminf(ro + bw, comp->width * 0.5f);
    const f32 roy = fminf(ry + bw, comp->height * 0.5f);
    bw = fminf(bw, sCornerMinRadius * fminf(rox, roy));
    return (bw > 0.001f) ? bw : 0.0f;
}

void djui_modern_rrect_draw(struct DjuiBaseRect* clip,
                            struct DjuiBaseRect* comp,
                            enum DjuiModernRadius radiusKind,
                            f32 explicitRadius,
                            struct DjuiColor topColor,
                            struct DjuiColor bottomColor,
                            u8 alphaScale) {
    rrect_emit(clip, comp, radiusKind, explicitRadius, topColor, bottomColor, alphaScale, 0.0f);
}

/* 返回包含边框的外轮廓半径。 */
f32 djui_modern_rrect_outer_radius(struct DjuiBaseRect* comp,
                                   enum DjuiModernRadius radiusKind,
                                   f32 explicitRadius, f32 borderWidth) {
    f32 ro = 0.0f, ry = 0.0f;
    rrect_radius_for(radiusKind, explicitRadius, comp->width, comp->height, &ro, &ry);
    const f32 bw = rrect_band_width(comp, ro, ry, borderWidth);
    const f32 cap = fminf(comp->width, comp->height) * 0.5f;
    return fminf(ro + bw, cap);
}

/* 提交一条可带透明度渐变的三角带。 */
static void rrect_band_strip(f32 x1, f32 y1, f32 w, f32 h,
                             const f32* ax, const f32* ay, u8 alphaA,
                             const f32* bx, const f32* by, u8 alphaB,
                             struct DjuiColor color) {
    s32 quad = 0;   // 已画到第几个四边形
    while (quad < RRECT_BAND_POINTS) {
        s32 npts = RRECT_BAND_BATCH_PTS;
        if (npts > RRECT_BAND_POINTS - quad + 1) {
            npts = RRECT_BAND_POINTS - quad + 1;   // 末批：补上回卷的起点
        }

        Vtx* vtx = vtx_alloc_count(2 * npts);
        if (vtx == NULL) { return; }
        for (s32 i = 0; i < npts; i++) {
            const s32 idx = (quad + i) % RRECT_BAND_POINTS;   // 末批回卷到起点闭合
            Vtx* va = &vtx[i];
            Vtx* vb = &vtx[npts + i];
            memset(va, 0, sizeof(Vtx));
            memset(vb, 0, sizeof(Vtx));
            va->v.ob[0] = (ax[idx] - x1) / w;
            va->v.ob[1] = (y1 - ay[idx]) / h;
            vb->v.ob[0] = (bx[idx] - x1) / w;
            vb->v.ob[1] = (y1 - by[idx]) / h;
            va->v.cn[0] = color.r; va->v.cn[1] = color.g;
            va->v.cn[2] = color.b; va->v.cn[3] = alphaA;
            vb->v.cn[0] = color.r; vb->v.cn[1] = color.g;
            vb->v.cn[2] = color.b; vb->v.cn[3] = alphaB;
        }

        // 顶点处理前关闭光照与背面剔除，绘制后恢复。
        Gfx* dl = gDisplayListHead;
        *dl++ = (Gfx) gsSPClearGeometryMode(G_LIGHTING | G_CULL_BOTH);
        *dl++ = (Gfx) gsDPSetCycleType(G_CYC_1CYCLE);
        *dl++ = (Gfx) gsDPSetRenderMode(G_RM_XLU_SURF, G_RM_XLU_SURF2);
        *dl++ = (Gfx) gsSPVertexNonGlobal(VIRTUAL_TO_PHYSICAL(vtx), 2 * npts, 0);
        *dl++ = (Gfx) gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
        *dl++ = (Gfx) gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE);
        for (s32 i = 0; i + 1 < npts; i++) {
            const s32 ai = i,          aj = i + 1;
            const s32 bi = npts + i,   bj = npts + i + 1;
            *dl++ = (Gfx) gsSP2TrianglesDjui(ai, aj, bj, 0, ai, bj, bi, 0);
        }
        *dl++ = (Gfx) gsSPSetGeometryMode(G_LIGHTING | G_CULL_BACK);
        gDisplayListHead = dl;

        quad += npts - 1;
    }
}

/*
 * 边框环带：外轮廓与填充共用角部曲线，内轮廓沿法向内缩 bw。
 * 外轮廓先收拢到裁剪区，再计算内轮廓。
 */
static void rrect_emit_band(struct DjuiBaseRect* clip, struct DjuiBaseRect* comp,
                            f32 ro, f32 ry, f32 bw, struct DjuiColor color) {
    if (ro <= 0.001f || ry <= 0.001f) { return; }
    if (bw <= 0.001f || color.a == 0) { return; }

    // 外轮廓半径包含边框宽度。
    const f32 rox = fminf(ro + bw, comp->width * 0.5f);
    const f32 roy = fminf(ry + bw, comp->height * 0.5f);

    // 使用与填充相同的裁剪矩形。
    const f32 x1 = fmaxf(comp->x, clip->x);
    const f32 y1 = fmaxf(comp->y, clip->y);
    const f32 x2 = fminf(comp->x + comp->width,  clip->x + clip->width);
    const f32 y2 = fminf(comp->y + comp->height, clip->y + clip->height);
    if (x2 <= x1 || y2 <= y1) { return; }
    const f32 w = x2 - x1;
    const f32 h = y2 - y1;

    // 每个角的圆心位于整体矩形的内侧。
    const f32 cx[4] = { comp->x + rox, comp->x + comp->width - rox,
                        comp->x + comp->width - rox, comp->x + rox };
    const f32 cy[4] = { comp->y + roy, comp->y + roy,
                        comp->y + comp->height - roy, comp->y + comp->height - roy };
    static const f32 a0[4] = { 180.0f, 270.0f, 0.0f, 90.0f };
    const f32 rad = (f32)(M_PI / 180.0);

    /*
     * 第一遍：按屏幕坐标算出整圈解析轮廓（未收拢），并存下每个点的
     * 外法向。法向由相邻折线段的中差切向旋转 90 度得到，方向用"指向
     * 矩形外"校正；沿法向内缩才是真正的等宽偏移，沿极点方向内缩在
     * 羊角螺线段会窄千分之几到百分之二。
     */
    f32 bx[RRECT_BAND_POINTS], by[RRECT_BAND_POINTS];
    f32 nx[RRECT_BAND_POINTS], ny[RRECT_BAND_POINTS];
    s32 pi = 0;
    for (s32 corner = 0; corner < 4 && pi < RRECT_BAND_POINTS; corner++) {
        const s32 kStart = (corner == 0) ? 0 : 1;   // 其余角的 k=0 与上角末点重合
        for (s32 k = kStart; k <= RRECT_BAND_SEG_CORNER && pi < RRECT_BAND_POINTS; k++) {
            const f32 t = (f32)k / (f32)RRECT_BAND_SEG_CORNER;
            const f32 phi = 90.0f * t;              // 该角内的极角 0 -> 90
            const f32 ang = (a0[corner] + phi) * rad;
            const f32 rho = rrect_corner_rho(phi * rad);
            bx[pi] = cx[corner] + rho * rox * cosf(ang);
            by[pi] = cy[corner] + rho * roy * sinf(ang);
            pi++;
        }
    }
    if (pi != RRECT_BAND_POINTS) { return; }   // 点数不符（不该发生），宁可不画

    const f32 ccx = (x1 + x2) * 0.5f;
    const f32 ccy = (y1 + y2) * 0.5f;
    for (s32 i = 0; i < RRECT_BAND_POINTS; i++) {
        const s32 prev = (i + RRECT_BAND_POINTS - 1) % RRECT_BAND_POINTS;
        const s32 next = (i + 1) % RRECT_BAND_POINTS;
        f32 tx = bx[next] - bx[prev];
        f32 ty = by[next] - by[prev];
        f32 len = sqrtf(tx * tx + ty * ty);
        if (len < 1e-4f) {
            // 退化的兜底：退回"从矩形中心指向外"的方向
            tx = bx[i] - ccx;
            ty = by[i] - ccy;
            len = sqrtf(tx * tx + ty * ty);
            if (len < 1e-4f) { len = 1.0f; }
        }
        nx[i] = ty / len;
        ny[i] = -tx / len;
        if (nx[i] * (bx[i] - ccx) + ny[i] * (by[i] - ccy) < 0.0f) {
            nx[i] = -nx[i];
            ny[i] = -ny[i];
        }
    }

    // 生成实心环带和两侧羽化边。
    const f32 hf = RRECT_BAND_FEATHER * 0.5f;
    // 羽化区域限制在 clip 内。
    const f32 kx1 = clip->x, ky1 = clip->y;
    const f32 kx2 = clip->x + clip->width, ky2 = clip->y + clip->height;
    f32 ox[RRECT_BAND_POINTS], oy[RRECT_BAND_POINTS];   // 外轮廓
    f32 ix[RRECT_BAND_POINTS], iy[RRECT_BAND_POINTS];   // 内轮廓
    f32 cox[RRECT_BAND_POINTS], coy[RRECT_BAND_POINTS]; // 实心外沿
    f32 cix[RRECT_BAND_POINTS], ciy[RRECT_BAND_POINTS]; // 实心内沿
    f32 fox[RRECT_BAND_POINTS], foy[RRECT_BAND_POINTS]; // 外羽化外沿（透明端）
    f32 gix[RRECT_BAND_POINTS], giy[RRECT_BAND_POINTS]; // 内羽化内沿（透明端）
    for (s32 i = 0; i < RRECT_BAND_POINTS; i++) {
        const f32 pox = fminf(fmaxf(bx[i], x1), x2);
        const f32 poy = fminf(fmaxf(by[i], y1), y2);
        const f32 pix = fminf(fmaxf(pox - bw * nx[i], x1), x2);
        const f32 piy = fminf(fmaxf(poy - bw * ny[i], y1), y2);

        ox[i] = pox; oy[i] = poy;
        ix[i] = pix; iy[i] = piy;

        cox[i] = fminf(fmaxf(pox - hf * nx[i], x1), x2);
        coy[i] = fminf(fmaxf(poy - hf * ny[i], y1), y2);
        cix[i] = fminf(fmaxf(pix + hf * nx[i], x1), x2);
        ciy[i] = fminf(fmaxf(piy + hf * ny[i], y1), y2);
        fox[i] = fminf(fmaxf(pox + hf * nx[i], kx1), kx2);
        foy[i] = fminf(fmaxf(poy + hf * ny[i], ky1), ky2);
        gix[i] = fminf(fmaxf(pix - hf * nx[i], x1), x2);
        giy[i] = fminf(fmaxf(piy - hf * ny[i], y1), y2);
    }

    f32 tx = x1, ty = y1;
    djui_gfx_position_translate(&tx, &ty);
    create_dl_translation_matrix(DJUI_MTX_PUSH, tx, ty, 0);

    f32 tw = w, th = h;
    djui_gfx_scale_translate(&tw, &th);
    create_dl_scale_matrix(DJUI_MTX_NOPUSH, tw, th, 1.0f);

    const u8 ca = color.a;
    if (bw > RRECT_BAND_FEATHER * 1.5f) {
        // 窄边框直接使用硬边环带。
        rrect_band_strip(x1, y1, w, h, fox, foy, 0,  cox, coy, ca, color);   // 外羽化
        rrect_band_strip(x1, y1, w, h, cix, ciy, ca, gix, giy, 0,  color);   // 内羽化
        rrect_band_strip(x1, y1, w, h, cox, coy, ca, cix, ciy, ca, color);   // 实心
    } else {
        rrect_band_strip(x1, y1, w, h, ox, oy, ca, ix, iy, ca, color);
    }

    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);
}

/* 绘制填充与圆角边框。 */
void djui_modern_rrect_draw_framed(struct DjuiBaseRect* clip,
                                   struct DjuiBaseRect* comp,
                                   enum DjuiModernRadius radiusKind,
                                   f32 explicitRadius,
                                   f32 borderWidth,
                                   struct DjuiColor borderColor,
                                   struct DjuiColor topColor,
                                   struct DjuiColor bottomColor,
                                   u8 alphaScale) {
    f32 ro = 0.0f, ry = 0.0f;
    rrect_radius_for(radiusKind, explicitRadius, comp->width, comp->height, &ro, &ry);

    // 计算有效边框宽度。
    f32 bw = (borderColor.a == 0) ? 0.0f : rrect_band_width(comp, ro, ry, borderWidth);

    // 填充按边框内沿内缩，并保留半个纹素的重叠。
    f32 fillInset = 0.0f;
    if (bw > 0.001f) {
        f32 overlap = 0.5f * (ro + bw) / RRECT_CORNER_ARC;   // 约半个纹素
        if (overlap > bw * 0.5f) { overlap = bw * 0.5f; }
        fillInset = bw - overlap;
    }
    rrect_emit(clip, comp, radiusKind, explicitRadius, topColor, bottomColor, alphaScale, fillInset);

    if (bw <= 0.001f) { return; }

    struct DjuiColor bc = borderColor;
    bc.a = (u8)(((s32)bc.a * alphaScale) / 255);

    // 直角边框使用四条矩形绘制。
    if (ro <= 0.001f || ry <= 0.001f) {
        struct DjuiBaseRect band;
        band.x = comp->x; band.y = comp->y; band.width = comp->width; band.height = bw;
        rrect_emit(clip, &band, DJUI_MODERN_RADIUS_NONE, 0.0f, bc, bc, 255, 0.0f);
        band.y = comp->y + comp->height - bw;
        rrect_emit(clip, &band, DJUI_MODERN_RADIUS_NONE, 0.0f, bc, bc, 255, 0.0f);
        band.x = comp->x; band.y = comp->y + bw;
        band.width = bw; band.height = comp->height - bw * 2.0f;
        rrect_emit(clip, &band, DJUI_MODERN_RADIUS_NONE, 0.0f, bc, bc, 255, 0.0f);
        band.x = comp->x + comp->width - bw;
        rrect_emit(clip, &band, DJUI_MODERN_RADIUS_NONE, 0.0f, bc, bc, 255, 0.0f);
        return;
    }

    if (gDjuiModernConfig.showStats) {
        static u32 sDiagBand;
        if (sDiagBand < 12) {
            const f32 bx1 = fmaxf(comp->x, clip->x);
            const f32 by1 = fmaxf(comp->y, clip->y);
            const f32 bx2 = fminf(comp->x + comp->width,  clip->x + clip->width);
            const f32 by2 = fminf(comp->y + comp->height, clip->y + clip->height);
            LOG_INFO("djui_modern[band#%u]: comp(%.1f,%.1f %.1fx%.1f) clip(%.1f,%.1f %.1fx%.1f) rect(%.1f,%.1f %.1fx%.1f) r=%.1f/%.1f bw=%.1f kind=%d",
                     sDiagBand, comp->x, comp->y, comp->width, comp->height,
                     clip->x, clip->y, clip->width, clip->height,
                     bx1, by1, bx2 - bx1, by2 - by1,
                     ro, ry, bw, (int)radiusKind);
            sDiagBand++;
        }
    }

    // 绘制圆角边框环带。
    rrect_emit_band(clip, comp, ro, ry, bw, bc);
}
