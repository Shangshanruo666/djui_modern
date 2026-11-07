#!/usr/bin/env python3
"""将完整字体子集化到按 Unicode 区段定义的字符范围。

用法:
    python src/pc/djui_modern/tools/make_subset_font.py <源字体> <输出字体>
        [--han LEVEL] [--exclude-font PATH] [--codepoints PATH]

收录范围由下列区段给出，覆盖中文、日文与西文的界面文本、玩家命名和
mod 文本：

    U+0020-U+007E  ASCII 可见区
    U+0080-U+00FF  Latin-1 Supplement
    U+0100-U+017F  Latin Extended-A
    U+0180-U+024F  Latin Extended-B
    U+0400-U+04FF  Cyrillic
    U+0500-U+052F  Cyrillic Supplement
    U+2000-U+206F  General Punctuation
    U+3000-U+303F  CJK 标点及全角空格
    U+3040-U+30FF  平假名与片假名
    U+3400-U+4DBF  CJK 扩展 A
    U+4E00-U+9FFF  CJK 基本区
    U+FF01-U+FF5E  全角形式常用子集
    U+FF66-U+FF9D  半角片假名

思源黑体 / 思源宋体的 OTF 通常为 15~20MB。直接内嵌为 C 头文件会膨胀到
约 4 倍（60MB+），显著增大仓库体积和构建时间。子集化后通常只剩 1~3MB。

--han 保留用于缩小汉字范围，默认 none 表示只收录上表中的区段：
    none      按区段表收录（默认）
    cjk       U+3400-U+4DBF 与 U+4E00-U+9FFF 全部汉字
    gb2312    GB2312 全部汉字，6763 字
    common    常用 3500 字（GB2312 一级字库），体积更小

--exclude-font 用于排除另一字体已经提供的码点。生成中文字体子集时传入
RobotoCondensed-Subset.ttf，可避免重复内嵌 Roboto 已有的 ASCII 和标点。

--codepoints 指定码点列表文件，每行一个十进制或十六进制码点。
位图字体（sbix 表）没有可用的字符范围，需要用该选项显式给出收录范围。

注：子集化只影响内嵌默认字体。仍可在游戏根目录或 djui_modern/ 文件夹中
放置任意 .ttf/.otf 覆盖它。该字体在运行期完整加载，不受这里限制。
"""

import os
import sys


# 收录区段。顺序与文档一致，覆盖西文、标点、假名与汉字。
CHARSET_RANGES = (
    (0x0020, 0x007E),
    (0x0080, 0x00FF),
    (0x0100, 0x017F),
    (0x0180, 0x024F),
    (0x0400, 0x04FF),
    (0x0500, 0x052F),
    (0x2000, 0x206F),
    (0x3000, 0x303F),
    (0x3040, 0x30FF),
    (0x3400, 0x4DBF),
    (0x4E00, 0x9FFF),
    (0xFF01, 0xFF5E),
    (0xFF66, 0xFF9D),
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


def cjk_unified_hanzi():
    """返回 U+3400-U+4DBF 与 U+4E00-U+9FFF 的全部码点。"""
    return {chr(cp) for cp in range(0x3400, 0x4DC0)} | \
           {chr(cp) for cp in range(0x4E00, 0xA000)}


def build_text(level, excluded_codepoints=None):
    chars = set()
    for lo, hi in CHARSET_RANGES:
        chars |= {chr(cp) for cp in range(lo, hi + 1)}

    if level == "cjk":
        chars |= cjk_unified_hanzi()
    elif level in ("gb2312", "common"):
        chars |= gb2312_hanzi(level)

    if excluded_codepoints:
        chars = {ch for ch in chars if ord(ch) not in excluded_codepoints}

    return "".join(sorted(chars))


def parse_args(argv):
    src = None
    dst = None
    level = "cjk"
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
    if level not in ("cjk", "gb2312", "common", "none"):
        raise ValueError("--han must be one of cjk/gb2312/common/none")

    return src, dst, level, exclude_font, codepoints_file


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
        src, dst, level, exclude_font, codepoints_file = parse_args(sys.argv[1:])
    except ValueError as exc:
        print("error: %s" % exc, file=sys.stderr)
        print(__doc__)
        return 1

    if not os.path.isfile(src):
        print("error: %s not found" % src, file=sys.stderr)
        return 1
        if not os.path.isfile(path):
            print("error: %s not found" % path, file=sys.stderr)
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
            text = build_text(level, excluded_codepoints)
        except ValueError as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 1
        excluded_count = len(excluded_codepoints)
    print("source : %s (%d bytes)" % (src, os.path.getsize(src)))
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
