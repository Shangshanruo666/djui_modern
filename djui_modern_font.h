#pragma once
#include "pc/djui/djui.h"

// 字体槽适配层；字形光栅化和纹理由 djui_modern_font_atlas.c 提供。
void djui_modern_font_install(void);
void djui_modern_font_shutdown(void);
bool djui_modern_font_ready(void);
const char* djui_modern_font_name(void);

// 查询字体槽替换前后的对象；未替换时返回 NULL。
const struct DjuiFont* djui_modern_font_replaced(s32 slot);
const struct DjuiFont* djui_modern_font_legacy(s32 slot);

// 统计参数均可为 NULL。
void djui_modern_font_get_stats(u32* atlasPixels, u32* glyphCount, u32* uploads);
void djui_modern_font_frame_begin(void);
void djui_modern_font_pass_begin(void);
