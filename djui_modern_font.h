#pragma once
#include "pc/djui/djui.h"

/* djui_modern 矢量字体接口。 */

// 替换配置中的字体槽。幂等。
void djui_modern_font_install(void);

// 还原原版字体并释放资源。幂等。
void djui_modern_font_shutdown(void);

// 字体后端是否可用。
bool djui_modern_font_ready(void);

// 当前字体名；未就绪时返回 "none"。
const char* djui_modern_font_name(void);

// 查询替换后的字体对象。
const struct DjuiFont* djui_modern_font_replaced(s32 slot);

// 查询槽位原有的字体对象。
const struct DjuiFont* djui_modern_font_legacy(s32 slot);

// 获取字形纹理字节数、字形数与上传数。参数可传 NULL。
void djui_modern_font_get_stats(u32* atlasPixels, u32* glyphCount, u32* uploads);

// 每帧第一遍渲染开始时重置顶点游标。
void djui_modern_font_frame_begin(void);

// 每帧第二遍渲染开始时调用，不清零顶点池。
void djui_modern_font_pass_begin(void);

/* 上报当前文字字号。 */
