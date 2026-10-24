#!/usr/bin/env python3
"""把 TTF/OTF 字体转成可内嵌的 C 头文件。

用法:
    python src/pc/djui_modern/tools/make_font_c.py <字体文件> <输出头文件> [数组名] [宏前缀]

例:
    python src/pc/djui_modern/tools/make_font_c.py my.ttf src/pc/djui_modern/djui_modern_font_cjk_data.h

生成的头文件由 src/pc/djui_modern/djui_modern_font_atlas.c 直接包含，
使模块默认提供矢量字体，无需额外放置字体文件。

内嵌字体会按字节数增加头文件体积，约增至字体文件的 5 倍；可执行文件只增加
接近字体文件大小的数据。完整中日韩字体通常为 15~20MB，应先子集化再内嵌。

生成中文子集时应排除 Roboto 已包含的码点：

    python src/pc/djui_modern/tools/make_subset_font.py SourceHan.otf \
        SourceHan-Subset.otf --han gb2312 \
        --glyph-source src/pc/djui/cn_glyphs.h \
        --exclude-font src/pc/djui_modern/font/RobotoCondensed-Subset.ttf

内嵌前可运行 tools/font_probe.c，确认汉字能够正常光栅化。
"""

import hashlib
import os
import sys

PER_LINE = 20


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    src = sys.argv[1]
    dst = sys.argv[2]
    array_name = sys.argv[3] if len(sys.argv) > 3 else "sDjuiModernEmbeddedFont"
    # 宏前缀（第 4 个参数）：内嵌多份字体时用它区分，避免宏重定义。
    macro = sys.argv[4] if len(sys.argv) > 4 else "DJUI_MODERN_EMBEDDED_FONT"
    size_name = array_name + "Size"

    with open(src, "rb") as f:
        data = f.read()

    if len(data) < 12:
        print("error: %s is too small to be a font" % src, file=sys.stderr)
        return 1

    tag = data[:4]
    if tag not in (b"\x00\x01\x00\x00", b"OTTO", b"true", b"ttcf"):
        print("warning: %s does not look like TTF/OTF (tag=%r)" % (src, tag), file=sys.stderr)

    digest = hashlib.sha256(data).hexdigest()
    base = os.path.basename(src)

    lines = []
    lines.append("// 自动生成，请勿手工编辑。")
    lines.append("//")
    command = "python src/pc/djui_modern/tools/make_font_c.py %s %s %s %s" % (
        base, os.path.basename(dst), array_name, macro)
    lines.append("// 生成命令: %s" % command)
    lines.append("// 字体文件: %s (%d bytes)" % (base, len(data)))
    lines.append("// SHA-256: %s" % digest)
    lines.append("// 授权: SIL Open Font License 1.1，全文见同目录 font/OFL.txt")
    lines.append("//")
    lines.append("// 也可将字体放入游戏根目录或 djui_modern/，由运行期加载。")
    lines.append("")
    lines.append("#pragma once")
    lines.append('#include "macros.h"')
    lines.append("")
    lines.append("#define %s_PRESENT 1" % macro)
    lines.append('#define %s_NAME "%s"' % (macro, base.replace('"', "")))
    lines.append("")
    lines.append("ALIGNED16 static const unsigned char %s[] = {" % array_name)

    for i in range(0, len(data), PER_LINE):
        chunk = data[i:i + PER_LINE]
        lines.append("    " + ",".join("0x%02x" % b for b in chunk) + ",")

    lines.append("};")
    lines.append("")
    lines.append("static const unsigned int %s = %d;" % (size_name, len(data)))
    lines.append("")

    out_dir = os.path.dirname(dst)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print("wrote %s (%d bytes font -> %d bytes header)" % (dst, len(data), os.path.getsize(dst)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
