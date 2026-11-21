#!/usr/bin/env python3
"""把 TTF/OTF 字体转成可内嵌的 C 源文件与声明头文件。

用法:
    python src/pc/djui_modern/tools/make_font_c.py <字体文件> <输出头文件> [数组名] [宏前缀] [分片数]

例:
    python src/pc/djui_modern/tools/make_font_c.py my.ttf src/pc/djui_modern/djui_modern_font_cjk_data.h

生成的头文件只声明数组和长度，字节数据按分片数写入同目录的
djui_modern_font_<前缀>_data_N.c，由构建系统分别编译。

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
PART_MAX_BYTES = 1536 * 1024


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

    header = []
    header.append("// 自动生成，请勿手工编辑。")
    header.append("//")
    command = "python src/pc/djui_modern/tools/make_font_c.py %s %s %s %s" % (
        base, os.path.basename(dst), array_name, macro)
    header.append("// 生成命令: %s" % command)
    header.append("// 字体文件: %s (%d bytes)" % (base, len(data)))
    header.append("// SHA-256: %s" % digest)
    header.append("// 授权: SIL Open Font License 1.1，全文见同目录 font/OFL.txt")
    header.append("//")
    header.append("// 也可将字体放入游戏根目录或 djui_modern/，由运行期加载。")
    header.append("")
    header.append("#pragma once")
    header.append("")
    header.append("#define %s_PRESENT 1" % macro)
    header.append('#define %s_NAME "%s"' % (macro, base.replace('"', "")))
    header.append("")
    out_dir = os.path.dirname(dst)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    out_dir = os.path.dirname(dst) or "."
    stem = os.path.splitext(os.path.basename(dst))[0]
    parts = max(1, (len(data) + PART_MAX_BYTES - 1) // PART_MAX_BYTES)
    per_part = (len(data) + parts - 1) // parts
    part_names = []
    for index in range(parts):
        part = data[index * per_part:(index + 1) * per_part]
        part_name = "%s_part%d" % (array_name, index)
        part_names.append((part_name, len(part)))
        part_path = os.path.join(out_dir, "%s_%02d.c" % (stem, index))
        lines = [
            "// 自动生成，请勿手工编辑。",
            '#include "macros.h"',
            "",
            "ALIGNED16 const unsigned char %s[] = {" % part_name,
        ]
        for offset in range(0, len(part), PER_LINE):
            chunk = part[offset:offset + PER_LINE]
            lines.append("    " + ",".join("0x%02x" % b for b in chunk) + ",")
        lines.append("};")
        lines.append("")
        with open(part_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines))

    for part_name, part_size in part_names:
        header.append("extern const unsigned char %s[];" % part_name)
    header.append("")
    header.append("static const struct DjuiModernFontPart %s[] = {" % array_name)
    for part_name, part_size in part_names:
        header.append("    { %s, %du }," % (part_name, part_size))
    header.append("};")
    header.append("")
    header.append("#define %s %d" % (size_name, len(data)))
    header.append("")
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(header))

    print("wrote %s and %d data part(s)" % (dst, parts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
