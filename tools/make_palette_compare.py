#!/usr/bin/env python3
"""Generate the djui_modern palette before/after comparison image."""

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


THEMES = [
    ("Light", "浅色", [
        ("正文", "text", (11, 11, 11), (18, 18, 21)),
        ("禁用文字", "disabledText", (60, 60, 60), (68, 68, 71)),
        ("控件底色", "rect", (222, 222, 222), (236, 238, 242)),
        ("悬停底色", "hoveredRect", (229, 241, 251), (242, 246, 251)),
        ("按下底色", "cursorDownRect", (204, 228, 247), (221, 232, 244)),
        ("边框", "border", (75, 75, 75), (82, 82, 85)),
        ("悬停边框", "hoveredBorder", (0, 120, 215), (42, 124, 194)),
        ("按下边框", "cursorDownBorder", (0, 84, 153), (34, 92, 151)),
        ("面板", "panel", (0, 0, 0), (20, 24, 31)),
        ("面板边框", "panelBorder", (0, 0, 0), (16, 18, 23)),
    ]),
    ("Dark", "深色", [
        ("正文", "text", (220, 220, 220), (226, 226, 230)),
        ("禁用文字", "disabledText", (140, 140, 140), (148, 150, 155)),
        ("控件底色", "rect", (22, 22, 22), (46, 50, 58)),
        ("悬停底色", "hoveredRect", (80, 80, 80), (82, 92, 108)),
        ("按下底色", "cursorDownRect", (100, 100, 100), (108, 120, 137)),
        ("边框", "border", (75, 75, 75), (82, 84, 89)),
        ("悬停边框", "hoveredBorder", (0, 120, 215), (48, 128, 199)),
        ("按下边框", "cursorDownBorder", (0, 84, 153), (36, 94, 151)),
        ("面板", "panel", (0, 0, 0), (20, 24, 31)),
        ("面板边框", "panelBorder", (0, 0, 0), (17, 20, 26)),
    ]),
    ("File Select", "文件选择", [
        ("正文", "text", (11, 11, 11), (18, 20, 18)),
        ("禁用文字", "disabledText", (60, 60, 60), (68, 70, 68)),
        ("控件底色", "rect", (200, 215, 197), (226, 234, 222)),
        ("悬停底色", "hoveredRect", (229, 241, 251), (240, 246, 250)),
        ("按下底色", "cursorDownRect", (204, 228, 247), (218, 233, 244)),
        ("边框", "border", (74, 79, 74), (80, 85, 80)),
        ("悬停边框", "hoveredBorder", (0, 120, 215), (44, 126, 195)),
        ("按下边框", "cursorDownBorder", (0, 84, 153), (35, 96, 153)),
        ("面板", "panel", (208, 165, 32), (222, 183, 65)),
        ("面板边框", "panelBorder", (182, 135, 8), (188, 151, 56)),
    ]),
    ("Mario", "马里奥", [
        ("正文", "text", (11, 11, 11), (20, 18, 16)),
        ("禁用文字", "disabledText", (60, 60, 60), (70, 66, 62)),
        ("控件底色", "rect", (255, 227, 0), (246, 219, 79)),
        ("悬停底色", "hoveredRect", (229, 241, 251), (241, 247, 251)),
        ("按下底色", "cursorDownRect", (204, 228, 247), (220, 233, 245)),
        ("边框", "border", (196, 165, 0), (200, 169, 12)),
        ("悬停边框", "hoveredBorder", (0, 120, 215), (45, 127, 196)),
        ("按下边框", "cursorDownBorder", (0, 84, 153), (36, 97, 154)),
        ("面板", "panel", (76, 116, 201), (91, 126, 196)),
        ("面板边框", "panelBorder", (255, 82, 82), (229, 111, 111)),
    ]),
    ("Odyssey", "奥德赛", [
        ("正文", "text", (11, 11, 11), (18, 17, 20)),
        ("禁用文字", "disabledText", (60, 60, 60), (68, 66, 70)),
        ("控件底色", "rect", (255, 255, 255), (247, 247, 249)),
        ("悬停底色", "hoveredRect", (229, 241, 251), (241, 246, 251)),
        ("按下底色", "cursorDownRect", (204, 228, 247), (220, 233, 245)),
        ("边框", "border", (200, 200, 200), (203, 203, 206)),
        ("悬停边框", "hoveredBorder", (0, 120, 215), (45, 126, 196)),
        ("按下边框", "cursorDownBorder", (0, 84, 153), (35, 97, 155)),
        ("面板", "panel", (247, 0, 37), (226, 55, 78)),
        ("面板边框", "panelBorder", (255, 255, 255), (246, 235, 238)),
    ]),
]

UI_COLORS = [
    ("覆盖层", "overlay", (8, 10, 14), (17, 21, 28)),
    ("模块面板", "panel", (20, 23, 30), (30, 35, 44)),
    ("面板阴影", "panelShadow", (0, 0, 0), (8, 11, 17)),
    ("详情标题", "detailTitle", (255, 255, 255), (252, 252, 253)),
    ("详情正文", "detailText", (222, 222, 222), (222, 223, 227)),
    ("错误提示", "statusError", (255, 100, 100), (236, 116, 125)),
    ("普通提示", "statusNotice", (255, 220, 100), (238, 202, 116)),
    ("文字阴影", "textShadow", (64, 64, 64), (64, 64, 64)),
]

FONT_PATH = Path(r"C:\Windows\Fonts\msyh.ttc")
FONT_BOLD_PATH = Path(r"C:\Windows\Fonts\msyhbd.ttc")
ROOT = Path(__file__).resolve().parents[4]
OUTPUT = ROOT / "djui_modern_palette_compare.png"


def font(size, bold=False):
    path = FONT_BOLD_PATH if bold and FONT_BOLD_PATH.exists() else FONT_PATH
    return ImageFont.truetype(str(path), size)


def hex_color(color):
    return "#{:02X}{:02X}{:02X}".format(*color)


def luminance(color):
    channels = []
    for value in color:
        value /= 255.0
        channels.append(value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4)
    return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2]


def contrast(a, b):
    la, lb = sorted((luminance(a), luminance(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


def text_color(background):
    return (24, 28, 34) if luminance(background) > 0.43 else (245, 247, 250)


def rounded_rect(draw, box, radius, fill, outline=None, width=1):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def draw_panel_preview(draw, x, y, colors, label, theme_index):
    panel = colors[8][3]
    panel_border = colors[9][3]
    text = colors[0][3]
    rect = colors[2][3]
    border = colors[5][3]
    background = (17, 21, 28) if theme_index in (0, 2, 3, 4) else (20, 24, 31)

    rounded_rect(draw, (x, y, x + 280, y + 155), 18, background)
    rounded_rect(draw, (x + 18, y + 18, x + 262, y + 137), 14, panel, panel_border, 2)
    draw.text((x + 36, y + 32), label, fill=text, font=font(21, True))
    for row in range(2):
        top = y + 72 + row * 32
        rounded_rect(draw, (x + 36, top, x + 244, top + 24), 8, rect, border, 2)
        fill_width = 140 if row == 0 else 205
        rounded_rect(draw, (x + 38, top + 2, x + 38 + fill_width, top + 22), 6,
                     colors[3][3] if row == 0 else colors[4][3])
        draw.text((x + 46, top + 4), "MODERN UI", fill=text_color(rect), font=font(12, True))


def draw_swatch(draw, x, y, old, new):
    old_box = (x, y, x + 98, y + 34)
    new_box = (x + 104, y, x + 202, y + 34)
    rounded_rect(draw, old_box, 8, old, (70, 76, 88), 1)
    rounded_rect(draw, new_box, 8, new, (70, 76, 88), 1)
    draw.text((x + 6, y + 8), hex_color(old), fill=text_color(old), font=font(12, True))
    draw.text((x + 110, y + 8), hex_color(new), fill=text_color(new), font=font(12, True))


def control_state_color(color, scale):
    return tuple(round(channel * scale) for channel in color)


def draw_back_button_comparison(draw, x, y):
    palettes = [
        ("Light", (222, 222, 222), (236, 238, 242)),
        ("Dark", (22, 22, 22), (46, 50, 58)),
        ("File Select", (200, 215, 197), (226, 234, 222)),
        ("Mario", (255, 227, 0), (246, 219, 79)),
        ("Odyssey", (255, 255, 255), (247, 247, 249)),
    ]

    draw.text((x, y), "返回按钮底色：原版倍率  vs  调整后倍率", fill=(226, 232, 241), font=font(19, True))
    y += 32
    draw.text((x + 176, y), "原版常态 / 禁用", fill=(120, 132, 150), font=font(13, True))
    draw.text((x + 424, y), "调整后常态 / 禁用", fill=(120, 132, 150), font=font(13, True))
    y += 26

    for name, original, modern in palettes:
        is_dark = name == "Dark"
        old_normal = control_state_color(original, 0.6)
        old_disabled = control_state_color(original, 0.3)
        new_normal = control_state_color(modern, 0.6 if is_dark else 0.84)
        new_disabled = control_state_color(modern, 0.3 if is_dark else 0.55)

        draw.text((x, y + 7), name, fill=(194, 203, 216), font=font(14))
        draw_swatch(draw, x + 176, y, old_normal, new_normal)
        draw_swatch(draw, x + 424, y, old_disabled, new_disabled)
        y += 42

    return y


def main():
    width = 1580
    row_height = 52
    header_height = 150
    theme_header = 66
    ui_header = 72
    back_button_height = 300
    footer = 58
    height = header_height + len(THEMES) * (theme_header + row_height * 10 + 18) + ui_header + row_height * len(UI_COLORS) + back_button_height + footer
    image = Image.new("RGB", (width, height), (12, 15, 20))
    draw = ImageDraw.Draw(image)

    draw.text((54, 32), "djui_modern 调色板现代化对比", fill=(241, 245, 250), font=font(34, True))
    draw.text((56, 82), "保留原有色相 · 降低极端明度与高饱和 · 统一控件层级", fill=(156, 167, 183), font=font(17))
    draw.line((54, 120, 1526, 120), fill=(47, 54, 66), width=1)

    left_x = 54
    label_x = 56
    old_x = 356
    new_x = 562
    draw.text((old_x + 26, 128), "调整前", fill=(139, 150, 166), font=font(15, True))
    draw.text((new_x + 26, 128), "调整后", fill=(139, 150, 166), font=font(15, True))

    y = header_height
    for theme_index, (name, cn_name, rows) in enumerate(THEMES):
        draw.text((54, y + 2), f"{name}  /  {cn_name}", fill=(226, 232, 241), font=font(22, True))
        draw.text((300, y + 8), "原版主题色", fill=(120, 132, 150), font=font(14))
        draw.line((54, y + 42, 1526, y + 42), fill=(38, 44, 55), width=1)
        y += theme_header

        for row_index, (label, field, old, new) in enumerate(rows):
            if row_index % 2 == 0:
                draw.rounded_rectangle((42, y - 5, 1538, y + 43), 8, fill=(18, 22, 29))
            draw.text((label_x, y + 8), label, fill=(194, 203, 216), font=font(15))
            draw.text((label_x + 112, y + 10), field, fill=(104, 116, 133), font=font(12))
            draw_swatch(draw, old_x, y + 5, old, new)
            ratio = contrast(old, new)
            ratio_color = (136, 207, 163) if ratio < 2.0 else (236, 189, 121)
            draw.text((1350, y + 10), f"变化 {ratio:.2f}x", fill=ratio_color, font=font(13))
            y += row_height

        preview_y = y - row_height * 10 + 8
        draw_panel_preview(draw, 1198, preview_y, rows, name, theme_index)
        y += 18

    draw.line((54, y, 1526, y), fill=(47, 54, 66), width=1)
    y += 18
    draw.text((54, y), "模块固定 UI 色", fill=(226, 232, 241), font=font(22, True))
    draw.text((300, y + 8), "覆盖层、设置详情与状态提示", fill=(120, 132, 150), font=font(14))
    y += ui_header

    for row_index, (label, field, old, new) in enumerate(UI_COLORS):
        if row_index % 2 == 0:
            draw.rounded_rectangle((42, y - 5, 1538, y + 43), 8, fill=(18, 22, 29))
        draw.text((label_x, y + 8), label, fill=(194, 203, 216), font=font(15))
        draw.text((label_x + 112, y + 10), field, fill=(104, 116, 133), font=font(12))
        draw_swatch(draw, old_x, y + 5, old, new)
        y += row_height

    draw.line((54, y, 1526, y), fill=(47, 54, 66), width=1)
    y += 18
    draw_back_button_comparison(draw, 54, y)

    draw.text((54, height - 40), "色块用于比较 RGB；透明度由主题与配置单独控制。", fill=(112, 124, 141), font=font(13))
    image.save(OUTPUT, "PNG")
    print(OUTPUT)


if __name__ == "__main__":
    main()
