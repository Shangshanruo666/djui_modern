#pragma once
#include "pc/djui/djui.h"

/* 圆角矩形渲染接口。 */

// 生成遮罩纹理（幂等）。
void djui_modern_rrect_init(void);

// 遮罩与顶点池是否就绪。
bool djui_modern_rrect_ready(void);

// 释放顶点池（退出时调用）。
void djui_modern_rrect_shutdown(void);

// 每帧开始时调用一次，重置顶点池。
void djui_modern_rrect_frame_begin(void);

// 圆角半径策略，单位与 DJUI 一致。
enum DjuiModernRadius {
    DJUI_MODERN_RADIUS_PANEL,    // 大面板
    DJUI_MODERN_RADIUS_ELEMENT,  // 普通控件
    DJUI_MODERN_RADIUS_PILL,     // 胶囊（按钮/滑条/输入框）
    DJUI_MODERN_RADIUS_NONE,     // 退化为方角
};

/* 绘制圆角矩形。clip 限制绘制范围。 */
void djui_modern_rrect_draw(struct DjuiBaseRect* clip,
                            struct DjuiBaseRect* comp,
                            enum DjuiModernRadius radiusKind,
                            f32 explicitRadius,
                            struct DjuiColor topColor,
                            struct DjuiColor bottomColor,
                            u8 alphaScale);

/* 返回包含边框的外轮廓半径。 */
f32 djui_modern_rrect_outer_radius(struct DjuiBaseRect* comp,
                                   enum DjuiModernRadius radiusKind,
                                   f32 explicitRadius,
                                   f32 borderWidth);

/* 绘制填充与圆角边框。borderWidth 按短边一半封顶。 */
void djui_modern_rrect_draw_framed(struct DjuiBaseRect* clip,
                                   struct DjuiBaseRect* comp,
                                   enum DjuiModernRadius radiusKind,
                                   f32 explicitRadius,
                                   f32 borderWidth,
                                   struct DjuiColor borderColor,
                                   struct DjuiColor topColor,
                                   struct DjuiColor bottomColor,
                                   u8 alphaScale);
