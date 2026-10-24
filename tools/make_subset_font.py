#!/usr/bin/env python3
"""将完整中日韩字体子集化到 UI 实际使用的字符范围。

用法:
    python src/pc/djui_modern/tools/make_subset_font.py <源字体> <输出字体>
        [--han LEVEL] [--glyph-source PATH] [--exclude-font PATH]
        [--codepoints PATH]

思源黑体 / 思源宋体的 OTF 通常为 15~20MB。直接内嵌为 C 头文件会膨胀到
约 4 倍（60MB+），显著增大仓库体积和构建时间。

DJUI 实际使用的字符包括 ASCII 可见区、中文标点和常用汉字。默认取 GB2312
的 6763 字，覆盖日常中文文本的绝大多数内容。

子集化后通常只剩 1~3MB，内嵌代价完全可接受。

--han 取值:
    gb2312   GB2312 全部汉字，6763 字（默认，推荐）
    common   常用 3500 字（GB2312 一级字库），体积更小
    none     只要 ASCII + 标点

--exclude-font 用于排除另一字体已经提供的码点。生成中文字体子集时传入
RobotoCondensed-Subset.ttf，可避免重复内嵌 Roboto 已有的 ASCII 和标点。

--glyph-source 指定游戏字表文件。生成中文子集时传入
src/pc/djui/cn_glyphs.h，脚本会读取其中的全部字符。

--codepoints 指定码点列表文件，每行一个十进制或十六进制码点。
位图字体（sbix 表）没有可用的字符范围，需要用该选项显式给出收录范围。

注：子集化只影响内嵌默认字体。仍可在游戏根目录或 djui_modern/ 文件夹中
放置任意 .ttf/.otf 覆盖它。该字体在运行期完整加载，不受这里限制。
"""

import os
import re
import sys
import unicodedata


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


def gb2312_symbols():
    out = set()
    for hi in range(0xA1, 0xAA):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if unicodedata.category(ch)[0] in ("P", "S"):
                out.add(ch)
    return out


def load_game_chars(path):
    with open(path, encoding="utf-8") as f:
        source = f.read()

    chars = set()
    for match in re.finditer(r'\{ "([^"]+)"', source):
        chars.update(match.group(1))
    if not chars:
        raise ValueError("no glyph entries found in %s" % path)
    return chars


def build_text(level, glyph_source=None, excluded_codepoints=None):
    chars = set()

    for cp in range(0x20, 0x7F):
        chars.add(chr(cp))

    chars |= gb2312_symbols()

    if level != "none":
        if glyph_source is None:
            raise ValueError("--glyph-source is required for han level %s" % level)
        chars |= load_game_chars(glyph_source)
        chars |= gb2312_hanzi(level)

    if excluded_codepoints:
        chars = {ch for ch in chars if ord(ch) not in excluded_codepoints}

    return "".join(sorted(chars))


def parse_args(argv):
    src = None
    dst = None
    level = "gb2312"
    glyph_source = None
    exclude_font = None
    codepoints_file = None
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--han":
            if i + 1 >= len(argv):
                raise ValueError("--han requires a value")
            level = argv[i + 1]
            i += 2
        elif arg == "--exclude-font":
            if i + 1 >= len(argv):
                raise ValueError("--exclude-font requires a path")
            exclude_font = argv[i + 1]
            i += 2
        elif arg == "--glyph-source":
            if i + 1 >= len(argv):
                raise ValueError("--glyph-source requires a path")
            glyph_source = argv[i + 1]
            i += 2
        elif arg == "--codepoints":
            if i + 1 >= len(argv):
                raise ValueError("--codepoints requires a path")
            codepoints_file = argv[i + 1]
            i += 2
        elif arg.startswith("-"):
            raise ValueError("unknown option: %s" % arg)
        elif src is None:
            src = arg
            i += 1
        elif dst is None:
            dst = arg
            i += 1
        else:
            raise ValueError("unexpected argument: %s" % arg)

    if src is None or dst is None:
        raise ValueError("source and output font paths are required")
    if level not in ("gb2312", "common", "none"):
        raise ValueError("--han must be one of gb2312/common/none")

    return src, dst, level, glyph_source, exclude_font, codepoints_file


def load_codepoint_list(path):
    out = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            out.add(int(line, 0))
    if not out:
        raise ValueError("no codepoints found in %s" % path)
    return out


def load_codepoints(path):
    from fontTools.ttLib import TTFont

    font = TTFont(path, lazy=True)
    try:
        return set(font.getBestCmap().keys())
    finally:
        font.close()


def main():
    try:
        src, dst, level, glyph_source, exclude_font, codepoints_file = parse_args(sys.argv[1:])
    except ValueError as exc:
        print("error: %s" % exc, file=sys.stderr)
        print(__doc__)
        return 1

    if not os.path.isfile(src):
        print("error: %s not found" % src, file=sys.stderr)
        return 1
    if glyph_source is not None and not os.path.isfile(glyph_source):
        print("error: %s not found" % glyph_source, file=sys.stderr)
        return 1
    if exclude_font is not None and not os.path.isfile(exclude_font):
        print("error: %s not found" % exclude_font, file=sys.stderr)
        return 1
    if codepoints_file is not None and not os.path.isfile(codepoints_file):
        print("error: %s not found" % codepoints_file, file=sys.stderr)
        return 1

    from fontTools import subset
    from fontTools.ttLib import TTFont

    excluded_codepoints = load_codepoints(exclude_font) if exclude_font else set()
    if codepoints_file is not None:
        try:
            codepoints = load_codepoint_list(codepoints_file)
        except ValueError as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 1
        text = "".join(chr(cp) for cp in sorted(codepoints))
        excluded_count = len(excluded_codepoints)
    else:
        try:
            text = build_text(level, glyph_source, excluded_codepoints)
        except ValueError as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 1
        excluded_count = len(excluded_codepoints)
    print("source : %s (%d bytes)" % (src, os.path.getsize(src)))
    if glyph_source:
        print("glyphs : %s" % glyph_source)
    if exclude_font:
        print("exclude: %s (%d mapped codepoints)" % (exclude_font, len(excluded_codepoints)))
    if codepoints_file:
        print("codepoints: %d entries from %s"
              % (len(text), codepoints_file))
    else:
        print("charset: %d chars (han level=%s, excluded=%d)"
              % (len(text), level, excluded_count))

    font = TTFont(src)
    font.recalcTimestamp = False

    options = subset.Options()
    # 移除不参与运行期渲染的表。
    options.drop_tables += ["DSIG", "FFTM", "LTSH", "VDMX", "hdmx", "PCLT"]
    options.name_IDs = ["*"]
    options.name_legacy = True
    options.notdef_outline = True
    options.recommended_glyphs = True
    # 位图字体的连字（国旗）依赖 GSUB，保留全部布局特性。
    options.layout_features = ["*"]
    options.notdef_glyph = True
    options.recalc_timestamp = False

    before = len(font.getGlyphOrder())

    subsetter = subset.Subsetter(options=options)
    subsetter.populate(text=text)
    subsetter.subset(font)

    after = len(font.getGlyphOrder())

    out_dir = os.path.dirname(dst)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    font.save(dst)
    font.close()

    print("glyphs : %d -> %d" % (before, after))
    print("output : %s (%d bytes, %.1f%% of source)"
          % (dst, os.path.getsize(dst),
             100.0 * os.path.getsize(dst) / os.path.getsize(src)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
