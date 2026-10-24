#!/usr/bin/env python3
"""列出 emoji 图集收录范围内的码点，供位图字体子集化使用。

用法:
    python src/pc/djui_modern/tools/make_emoji_codepoints.py <输出码点文件>

收录范围与 make_emoji_atlas.py 一致：原版 emoji_glyphs.h 的全部 emoji，
加上小黄脸、骷髅、鬼、恶魔、小丑、大脑、骨牙耳鼻脚和手势系列。
只有带 sbix 位图的码点会被写入输出文件。
"""

import os
import re
import sys

from fontTools.ttLib import TTFont

EMOJI_HEADER = "src/pc/djui/emoji_glyphs.h"

EXTRA_RANGES = [
    (0x1F600, 0x1F64F),
    (0x1F910, 0x1F92F),
    (0x1F970, 0x1F97F),
    (0x1F9D0, 0x1F9D0),
    (0x1F479, 0x1F480),
    (0x1F47B, 0x1F47B),
    (0x1F47D, 0x1F47F),
    (0x1F921, 0x1F921),
    (0x1F9E0, 0x1F9E0),
    (0x1F9B4, 0x1F9B9),
    (0x1F440, 0x1F450),
    (0x1F590, 0x1F596),
    (0x1F918, 0x1F91F),
    (0x1F932, 0x1F933),
]

RI_FIRST = 0x1F1E6
RI_LAST = 0x1F1FF


def parse_original_codepoints(path):
    with open(path, encoding="utf-8") as f:
        source = f.read()
    return set(ord(m.group(1)[0]) for m in re.finditer(r'\{ "([^"]+)"', source))


def bitmap_codepoints(font_path, wanted):
    font = TTFont(font_path, lazy=True)
    try:
        if "sbix" not in font:
            raise ValueError("font has no sbix table: %s" % font_path)
        strike = font["sbix"].strikes[max(font["sbix"].strikes.keys())]
        cmap = font.getBestCmap()
        out = set()
        for cp in wanted:
            glyph = cmap.get(cp)
            if glyph is None or glyph not in strike.glyphs:
                continue
            if strike.glyphs[glyph].imageData is None:
                continue
            out.add(cp)
        return out
    finally:
        font.close()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    emoji_font = sys.argv[1] if len(sys.argv) > 1 else ""
    flag_font = sys.argv[2] if len(sys.argv) > 2 else ""
    out_path = sys.argv[3] if len(sys.argv) > 3 else ""
    if not (emoji_font and flag_font and out_path):
        print(__doc__)
        return 1

    wanted = parse_original_codepoints(EMOJI_HEADER)
    for lo, hi in EXTRA_RANGES:
        wanted.update(range(lo, hi + 1))

    emoji = bitmap_codepoints(emoji_font, wanted)
    flags = bitmap_codepoints(flag_font, range(RI_FIRST, RI_LAST + 1))

    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# emoji 与国旗图集收录的码点，供 make_subset_font.py 使用。\n")
        f.write("# emoji=%d flags=%d\n" % (len(emoji), len(flags)))
        for cp in sorted(emoji | flags):
            f.write("0x%04X\n" % cp)

    print("emoji : %d codepoints" % len(emoji))
    print("flags : %d codepoints" % len(flags))
    print("output: %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
