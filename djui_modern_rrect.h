#pragma once
#include "pc/djui/djui.h"

/*
 * 圆角矩形渲染原语（羊角螺线圆角）。
 *
 * 实现要点
 * --------
 * - 圆角轮廓是"羊角螺线 + 正圆弧 + 羊角螺线"：曲率沿弧长从 0 线性
 *   长到 1/R、中段保持正圆弧、再线性回到 0，与直边相切处曲率连续
 *   （G2）。正圆角矩形在切点的曲率突变（0 跳到 1/R）在这里不存在。
 * - 轮廓由数值积分生成归一化极径表，填充（遮罩 SDF）与边框（屏幕
 *   三角化）取同一条曲线，几何天然对齐。
 * - 几何是常量顶点网格 + 静态 display list；遮罩纹理地址恒定，全程
 *   只上传一次；顶点来自预分配对象池（每帧复用，零 malloc）。
 * - 遮罩按"基准半径 + 抗锯齿覆盖度"预计算，直边区域通过 UV 拉伸
 *   复用，因此任意尺寸与圆角半径共用同一张纹理。
 */

// 生成遮罩纹理（幂等）。
void djui_modern_rrect_init(void);

// 遮罩与顶点池是否就绪。未就绪时调用方应回退到原版实现。
bool djui_modern_rrect_ready(void);

// 释放顶点池（退出时调用）。
void djui_modern_rrect_shutdown(void);

// 每帧开始时调用一次，重置顶点池。
void djui_modern_rrect_frame_begin(void);

// 圆角半径策略。半径单位与 DJUI 一致（320x240 虚拟坐标系）。
enum DjuiModernRadius {
    DJUI_MODERN_RADIUS_PANEL,    // 大面板
    DJUI_MODERN_RADIUS_ELEMENT,  // 普通控件
    DJUI_MODERN_RADIUS_PILL,     // 胶囊（按钮/滑条/输入框）
    DJUI_MODERN_RADIUS_NONE,     // 退化为方角
};

/*
 * 画一个圆角矩形（含可选渐变）。调用前调用方需已就位自身位置矩阵；
 * 本函数会自己 push/pop 矩阵。
 * clip 用于把绘制限制在父级裁剪区内。
 */
void djui_modern_rrect_draw(struct DjuiBaseRect* clip,
                            struct DjuiBaseRect* comp,
                            enum DjuiModernRadius radiusKind,
                            f32 explicitRadius,
                            struct DjuiColor topColor,
                            struct DjuiColor bottomColor,
                            u8 alphaScale);

/*
 * 该矩形在给定半径策略与边框宽度下的**外轮廓半径**（可见半径 + 边框宽度，
 * 按短边一半封顶）。投影之类的"贴在控件外侧"的图形要用它，否则角部会比
 * 控件轮廓更尖锐，并超出圆角外侧。
 */
f32 djui_modern_rrect_outer_radius(struct DjuiBaseRect* comp,
                                   enum DjuiModernRadius radiusKind,
                                   f32 explicitRadius,
                                   f32 borderWidth);

/*
 * 边框 + 填充的圆角矩形。
 *
 * 画法：先按边框内沿绘制圆角填充，再沿轮廓绘制边框环带。
 * 环带内部不填充，边框颜色只出现在外轮廓周围。
 *
 * 环带使用弧线三角化：外轮廓取解析角曲线，内轮廓沿法向按边框宽度内缩，
 * 两条折线组成闭合三角带，
 * 法向厚度恒等于边框宽度，直边与圆角粗细一致。
 *
 * radiusKind / explicitRadius 表示填充侧的可见圆角半径。边框从该轮廓向外
 * 扩展，因此外轮廓半径为半径加 borderWidth。若边框向内扩展，可见圆角会
 * 缩小为半径减 borderWidth。
 *
 * 不使用“整块边框色加内缩填充”的顺序。面板填充可能由 panel_alpha
 * 调整透明度，内圈无法完全覆盖底层边框色，会导致边框颜色参与混色。
 *
 * 不采用整块环带遮罩加九宫格绘制，因为弧线处的法向厚度会略大于直边。
 *
 * borderWidth 按矩形短边的一半封顶（同上游 djui_base_render_border）。
 */
void djui_modern_rrect_draw_framed(struct DjuiBaseRect* clip,
                                   struct DjuiBaseRect* comp,
                                   enum DjuiModernRadius radiusKind,
                                   f32 explicitRadius,
                                   f32 borderWidth,
                                   struct DjuiColor borderColor,
                                   struct DjuiColor topColor,
                                   struct DjuiColor bottomColor,
                                   u8 alphaScale);
