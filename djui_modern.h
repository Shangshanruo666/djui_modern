#pragma once

/* djui_modern 公共接口。 */

#include "pc/djui/djui.h"

/* 初始化。在 djui_init() 开头调用一次，需晚于 gfx 初始化。 */
void djui_modern_init(void);

/* 每帧渲染前重置顶点池并应用主题。 */
void djui_modern_pre_render(void);

/* 每帧渲染后的诊断钩子。 */
void djui_modern_post_render(void);

/* 释放字体与图集资源。 */
void djui_modern_shutdown(void);
