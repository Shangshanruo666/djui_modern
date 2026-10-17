#!/usr/bin/env python3
"""把中日韩字体子集化到 UI 使用的字符范围。

用法:
    python src/pc/djui_modern/tools/make_subset_font.py <源字体> <输出字体> [--han LEVEL]

用途
----
子集化可减小内嵌头文件和编译体积。DJUI 使用的字符范围包括：
  · ASCII 可见区
  · 中文标点
  · 常用汉字（默认取 GB2312 的 6763 字）

--han LEVEL
-----------
    gb2312   GB2312 全部汉字，6763 字（默认，推荐）
    common   常用 3500 字（GB2312 一级字库），体积更小
    none     只要 ASCII + 标点

子集化只影响内嵌默认字体。游戏目录或 djui_modern/ 中的运行期字体不受限制。
"""

import os
import sys

# 中文标点与常用符号（GB2312 之外还要补的）
EXTRA_CHARS = (
    # 必须与 src/pc/djui/cn_glyphs.h 里的字符完全对齐，
    # 再补上该文件没有、但中文排版常见的标点。
    "　、。，．！？；：（）〔〕【】〖〗《》〈〉"
    "「」『』"                      # 直角引号（cn_glyphs.h 未收录，常见需求）
    "‘’“”…—～·"
    "！＂＃＄％＆＇（）＊＋，－．／"
    "０１２３４５６７８９：；＜＝＞？"
    "＠ＡＢＣＤＥＦＧＨＩＪＫＬＭＮＯＰＱＲＳＴＵＶＷＸＹＺ"
    "［＼］＾＿｀ａｂｃｄｅｆｇｈｉｊｋｌｍｎｏｐｑｒｓｔｕｖｗｘｙｚ"
    "€£¥©®°±×÷§¶µ¢"
    # Coopdx 用 0x7F 表示"星星"（见 src/pc/djui/djui_text.c:235），运行期会
    # 重定向到 Unicode 规范五角星 U+2605。这里把它放进子集，让内嵌字体
    # 自带一个真正的 ★ 字形（模块在字体缺字形时会退化为程序化生成）。
    "★☆"
)


def gb2312_hanzi(level):
    """返回 GB2312 编码里定义的汉字集合。"""
    out = set()
    if level == "none":
        return out

    # GB2312 分 94 个区。一级字库(常用)在 16-55 区，二级字库在 56-87 区。
    hi_start, hi_end = (0xB0, 0xF8)          # 16 区 ~ 87 区
    if level == "common":
        hi_end = 0xD8                        # 只到 55 区（一级字库）

    for hi in range(hi_start, hi_end):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            out.add(ch)
    return out


def build_text(level):
    chars = set()

    # ASCII 可见区
    for cp in range(0x20, 0x7F):
        chars.add(chr(cp))

    # 中文标点与符号
    for ch in EXTRA_CHARS:
        chars.add(ch)

    # 汉字
    chars |= gb2312_hanzi(level)

    return "".join(sorted(chars))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    level = "gb2312"
    if "--han" in sys.argv:
        i = sys.argv.index("--han")
        if i + 1 < len(sys.argv):
            level = sys.argv[i + 1]
    if level not in ("gb2312", "common", "none"):
        print("error: --han must be one of gb2312/common/none", file=sys.stderr)
        return 1

    if len(args) < 2:
        print(__doc__)
        return 1

    src, dst = args[0], args[1]
    if not os.path.isfile(src):
        print("error: %s not found" % src, file=sys.stderr)
        return 1

    from fontTools import subset
    from fontTools.ttLib import TTFont

    text = build_text(level)
    print("source : %s (%d bytes)" % (src, os.path.getsize(src)))
    print("charset: %d chars (han level=%s)" % (len(text), level))

    font = TTFont(src)

    options = subset.Options()
    # 去掉用不到的表，压缩体积。保留 glyf/CFF、cmap、hmtx、head、hhea、maxp、name、OS/2、post。
    options.drop_tables += ["DSIG", "FFTM", "LTSH", "VDMX", "hdmx", "PCLT"]
    options.name_IDs = ["*"]
    options.name_legacy = True
    options.notdef_outline = True
    options.recommended_glyphs = True
    options.layout_features = ["kern", "liga", "vert", "vrt2"]
    options.notdef_glyph = True

    before = len(font.getGlyphOrder())

    subsetter = subset.Subsetter(options=options)
    subsetter.populate(text=text)
    subsetter.subset(font)

    after = len(font.getGlyphOrder())

    out_dir = os.path.dirname(dst)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    font.save(dst)

    print("glyphs : %d -> %d" % (before, after))
    print("output : %s (%d bytes, %.1f%% of source)"
          % (dst, os.path.getsize(dst),
             100.0 * os.path.getsize(dst) / os.path.getsize(src)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
