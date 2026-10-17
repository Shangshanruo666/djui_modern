#pragma once
#include "pc/djui/djui.h"

/*
 * 矢量字体后端：按需光栅化字形，每个字形使用一张不可变纹理。
 * 字体优先级：font_path、游戏目录字体、内嵌字体。
 */

// 初始化。幂等。
bool djui_modern_font_atlas_init(void);

// 释放字体数据与全部字形纹理。
void djui_modern_font_atlas_shutdown(void);

// 每帧开始（第一遍渲染）调用：重置每帧顶点游标。
void djui_modern_font_atlas_frame_begin(void);

/* 第二遍渲染开始时调用，不清零顶点池。 */
void djui_modern_font_atlas_pass_begin(void);

// 字体是否可用。
bool djui_modern_font_atlas_ready(void);

// 字体文件名（用于日志）。
const char* djui_modern_font_atlas_name(void);

// 统计：atlasPixels=字形纹理总字节，glyphCount=已生成纹理数，uploads=纹理上传数。
void djui_modern_font_atlas_stats(u32* atlasPixels, u32* glyphCount, u32* uploads);

/* 返回字符宽度，单位为 em。 */
f32 djui_modern_font_atlas_char_width(const char* c, f32 fontScale);

/* 判断码点是否属于 CJK 或全角标点。 */
bool codepoint_is_cjk(u32 cp);

/* 一段文字的渲染开始与结束。 */
void djui_modern_font_atlas_render_begin(void);
void djui_modern_font_atlas_render_end(void);

// 渲染一个字符。
void djui_modern_font_atlas_render_char(const char* c);

// 字号提示栈，单位为屏幕像素。
void djui_modern_font_atlas_push_scale_hint(f32 emPixels, f32 valid);
void djui_modern_font_atlas_pop_scale_hint(void);
f32  djui_modern_font_atlas_get_scale_hint(void);

// 取回 UTF-8 字符的 codepoint（供上层做 CJK 判定等），bytes 返回字节数。
u32 djui_modern_font_utf8_decode(const char* s, s32* bytes);

/* 判断字符是否由现代 Emoji 图集渲染。 */
bool djui_modern_emoji_available(const char* c);

/* 名义字格宽；裁剪换算依赖该值。 */
#define DJUI_MODERN_FONT_CELL_W 1.0f

/* 放行本模块可渲染的输入字符。 */
bool djui_modern_unicode_accepts(const char* text);

/* 上报当前文字所属控件。 */
void djui_modern_note_text_base(struct DjuiBase* base);

/* 同步名义字格高。 */
void djui_modern_font_atlas_set_cell_height(f32 h);
