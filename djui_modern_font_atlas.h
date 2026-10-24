#pragma once
#include "pc/djui/djui.h"

/*
 * 矢量字形后端。
 * 字形按字符与屏幕字号分档光栅化，每个槽位对应一张不可变纹理。
 * 字体查找顺序：font_path、djui_modern/、游戏根目录、内嵌字体。
 */
bool djui_modern_font_atlas_init(void);
void djui_modern_font_atlas_shutdown(void);
void djui_modern_font_atlas_frame_begin(void);
// 第二遍渲染保留第一遍的顶点池。
void djui_modern_font_atlas_pass_begin(void);
bool djui_modern_font_atlas_ready(void);
const char* djui_modern_font_atlas_name(void);
void djui_modern_font_atlas_stats(u32* atlasPixels, u32* glyphCount, u32* uploads);

// 返回 em 相对宽度；字偶间距通过前瞻下一个字符计算。
f32 djui_modern_font_atlas_char_width(const char* c, f32 fontScale);
bool codepoint_is_cjk(u32 cp);

// 一行文字开始和结束时切换 RDP 状态。
void djui_modern_font_atlas_render_begin(void);
void djui_modern_font_atlas_render_end(void);
void djui_modern_font_atlas_render_char(const char* c);

// emPixels 使用屏幕物理像素；push 与 pop 必须配对。
void djui_modern_font_atlas_push_scale_hint(f32 emPixels, f32 valid);
void djui_modern_font_atlas_pop_scale_hint(void);
f32  djui_modern_font_atlas_get_scale_hint(void);

u32 djui_modern_font_utf8_decode(const char* s, s32* bytes);

// 彩色 Emoji 使用离线 RGBA 图集；返回 false 时交给原版渲染。
bool djui_modern_emoji_available(const char* c);

// DjuiFont.charWidth 必须使用同一个字格宽度。
#define DJUI_MODERN_FONT_CELL_W 1.0f

// 声明处不能带 weak，否则会把模块内的强定义降级。
bool djui_modern_unicode_accepts(const char* text);

// 聊天框保留垂直裁剪，其余控件允许文字上下溢出。
void djui_modern_note_text_base(struct DjuiBase* base);

void djui_modern_font_atlas_set_cell_height(f32 h);
