#pragma once

#include "pc/djui/djui.h"

// 在 DJUI 首次创建文字节点前调用。
void djui_modern_init(void);

// 在 DJUI 每帧绘制前调用。
void djui_modern_pre_render(void);

// 在 DJUI 每帧绘制后调用。
void djui_modern_post_render(void);

// 在 DJUI 关闭时调用。
void djui_modern_shutdown(void);
