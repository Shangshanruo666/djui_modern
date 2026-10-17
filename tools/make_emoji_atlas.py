#!/usr/bin/env python3
"""把彩色 Emoji 字体烘焙成 RGBA 图集和码点查找表。

用法:
    python src/pc/djui_modern/tools/make_emoji_atlas.py <emoji字体> <输出头文件> [--size 64]

字符范围
--------
A. 汉化版 `src/pc/djui/emoji_glyphs.h` 里的全部 emoji
B. 小黄脸系列：Emoticons(U+1F600-1F64F)、补充表情(U+1F910-1F92F、
   U+1F970-1F97F)、单片眼镜(U+1F9D0)
C. 骷髅 / 鬼 / 外星人 / 恶魔 / 小丑 / 大脑 / 骨牙耳鼻脚（U+1F479-1F480、
   U+1F47B、U+1F47D-1F47F、U+1F921、U+1F9E0、U+1F9B4-1F9B9）
D. 手势系列：U+1F440-1F450、U+1F590-1F596、U+1F918-1F91F、U+1F932-1F933
E. 国旗（可选，用 `--flags <国旗字体>` 另给一份）

图集尺寸限制
------------
`Vtx.tc` 使用 signed 16-bit S10.5 定点，图集边长不能超过 1023 texel。
Emoji 与国旗分别生成一张 512x512 图集：
  · emoji 图集 → `djui_modern_emoji_data.h`（含 DIM/LOG2/GRID/TILE 宏）
  · 国旗图集 → `djui_modern_flag_data.h`（布局与前者完全一致，共用同一套宏）

图集布局
--------
    grid_x x grid_y 格（两边都是 2 的幂，尽量接近方形），每格 size x size
    · emoji：tile 下标 = 该码点在 `sDjuiModernEmojiCodepoints[]`（升序）里的下标
    · 国旗：tile 下标 = 该 RI 对在 `sDjuiModernFlagKeys[]`（升序）里的下标
      键 = (ri1-0x1F1E6)*26 + (ri2-0x1F1E6)。两张图**各自独立编号、都从 0 开始**。
    C 侧二分查找即可，不需要额外的哈希表。

存储格式
--------
    `--format png`（默认）：把整张图集编码成 PNG，再以**十六进制字节数组**内嵌。
    PNG 为无损压缩，运行时使用 stb_image 解码一次。
    `--format raw`：直接内嵌 RGBA32 字节（兼容旧行为，体积大得多）。
"""

import io
import os
import re
import sys

from fontTools.ttLib import TTFont
from PIL import Image

# 原版权威 emoji 表（全部收录，保证既有文本不变）
EMOJI_HEADER = "src/pc/djui/emoji_glyphs.h"

# 额外收录的区段（不含肤色变种）
EXTRA_RANGES = [
    (0x1F600, 0x1F64F),   # Emoticons：小黄脸主体
    (0x1F910, 0x1F92F),   # Supplemental：补充表情
    (0x1F970, 0x1F97F),   # Supplemental：补充表情 2
    (0x1F9D0, 0x1F9D0),   # 单片眼镜
    (0x1F479, 0x1F480),   # 食人魔 / 哥布林 / 骷髅
    (0x1F47B, 0x1F47B),   # 鬼
    (0x1F47D, 0x1F47F),   # 外星人 / 外星怪物 / 恶魔
    (0x1F921, 0x1F921),   # 小丑
    (0x1F9E0, 0x1F9E0),   # 大脑
    (0x1F9B4, 0x1F9B9),   # 骨 / 牙 / 耳 / 鼻 / 脚
    (0x1F440, 0x1F450),   # 眼睛 / 各种手
    (0x1F590, 0x1F596),   # 手势补充
    (0x1F918, 0x1F91F),   # 手势补充 2
    (0x1F932, 0x1F933),   # 手势补充 3
]


def parse_original_codepoints(path: str) -> list:
    src = open(path, encoding="utf-8").read()
    return [ord(m.group(1)[0]) for m in re.finditer(r'\{ "([^"]+)"', src)]


RI_FIRST, RI_LAST = 0x1F1E6, 0x1F1FF   # Regional Indicator A..Z

# ⚠ 图集边长的**硬上限**：N64 `Vtx.tc` 是 signed 16-bit 的 S10.5 定点
# （纹素 << 5），可表达的最大正值是 32767 ⇒ 边长 ≤ 32767/32 = 1023 texel。
# 边长上限为 1023 texel。
MAX_DIM = 1023


def _glyph_to_codepoint(font):
    """字形名 -> 码点。用它把 GSUB 连字里的字形名还原成码点，
    这样**完全不依赖字形名是否可读**（子集化常把名字改成 glyphNNNNN）。"""
    rev = {}
    for cp, gn in font.getBestCmap().items():
        rev.setdefault(gn, cp)
    return rev


def collect_flags(font, strike):
    """从 GSUB 连字表取出国旗：返回 [( (ri1, ri2), glyph ), ...]。"""
    if "GSUB" not in font:
        return None
    gsub = font["GSUB"].table
    if gsub.LookupList is None:
        return None

    rev = _glyph_to_codepoint(font)

    def ri(cp):
        return cp is not None and RI_FIRST <= cp <= RI_LAST

    pairs = []
    for lk in gsub.LookupList.Lookup:
        for st in lk.SubTable:
            ext = getattr(st, "ExtSubTable", None)
            if ext is not None:
                st = ext
            ligs = getattr(st, "ligatures", None)
            if not ligs:
                continue
            for first, lst in ligs.items():
                cp1 = rev.get(first)
                if not ri(cp1):
                    continue
                for lg in lst:
                    comp = lg.Component
                    if len(comp) != 1:
                        continue
                    cp2 = rev.get(comp[0])
                    if not ri(cp2):
                        continue
                    g = lg.LigGlyph
                    d = strike.glyphs.get(g)
                    if d is not None and d.imageData is not None:
                        pairs.append(((cp1, cp2), g))
    return pairs or None


def dedupe_flags(pairs):
    """去重：同一面旗被多个 RI 对命中时只保留**最小键**的那一对（ISO 顺序）。

    Returns [(pair, glyph), ...] sorted by key.
"""
    from collections import defaultdict
    g2pairs = defaultdict(list)
    for p, g in pairs:
        g2pairs[g].append(p)
    kept = []
    for g, ps in g2pairs.items():
        ps.sort(key=lambda p: (p[0] - RI_FIRST) * 26 + (p[1] - RI_FIRST))
        kept.append((ps[0], g))
    kept.sort(key=lambda x: (x[0][0] - RI_FIRST) * 26 + (x[0][1] - RI_FIRST))
    return kept


def build_atlas(count, size, gx, gy, bitmap_at):
    """建一张 POT RGBA 图集；`bitmap_at(i)` 返回第 i 格的 PNG 字节。"""
    dim_x, dim_y = size * gx, size * gy
    atlas = Image.new("RGBA", (dim_x, dim_y), (0, 0, 0, 0))
    for i in range(count):
        data = bitmap_at(i)
        im = Image.open(io.BytesIO(data)).convert("RGBA")
        if im.size != (size, size):
            im = im.resize((size, size), Image.LANCZOS)
        atlas.paste(im, ((i % gx) * size, (i // gx) * size))
    return atlas, dim_x, dim_y


def encode_blob(pil_img, fmt):
    if fmt == "png":
        buf = io.BytesIO()
        pil_img.save(buf, format="PNG", optimize=True)
        return buf.getvalue(), "PNG", 1
    return pil_img.tobytes(), "RGBA32", 0


def _dump_byte_array(blob, per_line=20):
    """把字节流格式化成 '0xXX,0xXX,...' 列表（按 20 个/行）。"""
    out = []
    for i in range(0, len(blob), per_line):
        out.append("    " + "".join("0x%02x," % b for b in blob[i:i + per_line]) + "\n")
    return "".join(out)


def safe_grid(count, size):
    cap = MAX_DIM // size
    if cap < 1:
        return None
    gx = 1
    while gx < cap and gx * gx < count:
        gx <<= 1
    gy = gx
    while gy > 1 and gx * (gy >> 1) >= count:
        gy >>= 1
    return gx, gy


def write_emoji_header(path, cps, blob, blob_kind, atlas_png, size, gx, gy,
                       dim_x, dim_y, src_font, src_ppem, cmd):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        w = f.write
        w("// ============================================================================\n")
        w("// 自动生成，请勿手动编辑。\n")
        w("//\n")
        w(f"//   生成命令: {cmd}\n")
        w(f"//   来源字体: {os.path.basename(src_font)} (ppem={src_ppem})\n")
        w("//   授权    : SIL Open Font License 1.1（Noto Emoji）\n")
        w("//\n")
        w(f"// {gx}x{gy} 格，每格 {size}x{size}，整图 {dim_x}x{dim_y} RGBA32。\n")
        w(f"//   边长 {max(dim_x, dim_y)} ≤ {MAX_DIM}（S10.5 定点上限）。\n")
        w(f"// 图集数据以 {blob_kind} 内嵌（{len(blob)} 字节），"
          f"展开后 {dim_x * dim_y * 4} 字节。\n")
        w("// tile 下标 = 该码点在 sDjuiModernEmojiCodepoints[]（升序）里的下标。\n")
        w(f"// 共 {len(cps)} 个 emoji。\n")
        w("//\n")
        w("// 收录范围：汉化版 emoji_glyphs.h 全部 + 小黄脸/骷髅/鬼/恶魔等 + 手势系列。\n")
        w("// ============================================================================\n\n")
        w("#pragma once\n\n")
        w(f"#define DJUI_MODERN_EMOJI_COUNT {len(cps)}\n")
        w(f"#define DJUI_MODERN_EMOJI_GRID_X {gx}\n")
        w(f"#define DJUI_MODERN_EMOJI_GRID_Y {gy}\n")
        w(f"#define DJUI_MODERN_EMOJI_TILE  {size}\n")
        w(f"#define DJUI_MODERN_EMOJI_DIM_X {dim_x}\n")
        w(f"#define DJUI_MODERN_EMOJI_DIM_Y {dim_y}\n")
        w(f"#define DJUI_MODERN_EMOJI_LOG2_X {dim_x.bit_length() - 1}\n")
        w(f"#define DJUI_MODERN_EMOJI_LOG2_Y {dim_y.bit_length() - 1}\n")
        w(f"/* 图集内嵌格式：1 = PNG（需 stb_image 解一次），0 = 裸 RGBA32。 */\n")
        w(f"#define DJUI_MODERN_EMOJI_ATLAS_PNG {atlas_png}\n")
        w(f"/* PNG 字节数（--format raw 时为 RGBA32 字节数）。 */\n")
        w(f"#define DJUI_MODERN_EMOJI_ATLAS_SIZE {len(blob)}\n\n")
        w("/* 升序排列的码点表；下标即 tile 下标，供二分查找。 */\n")
        w("static const unsigned int sDjuiModernEmojiCodepoints[] = {\n")
        for i in range(0, len(cps), 12):
            chunk = cps[i:i + 12]
            w("    " + "".join("0x%04x," % c for c in chunk) + "\n")
        w("};\n\n")
        w(f"/* 图集：{blob_kind}，{len(blob)} 字节。 */\n")
        w("ALIGNED16 static const unsigned char sDjuiModernEmojiAtlas[] = {\n")
        w(_dump_byte_array(blob))
        w("};\n")


def write_flag_header(path, flags, blob, blob_kind, atlas_png, size, gx, gy,
                      dim_x, dim_y, src_font, src_ppem, cmd):
    """`flags` = 去重后的 [( (ri1,ri2), glyph ), ...]，已按 key 升序。
    tile 下标 = 在该数组里的下标（**国旗图集独立编号，从 0 开始**）。"""
    keys = [(p[0] - RI_FIRST) * 26 + (p[1] - RI_FIRST) for p, _ in flags]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        w = f.write
        w("// ============================================================================\n")
        w("// 自动生成，请勿手动编辑。\n")
        w("//\n")
        w(f"//   生成命令: {cmd}\n")
        w(f"//   来源字体: {os.path.basename(src_font)} (ppem={src_ppem})\n")
        w("//   授权    : SIL Open Font License 1.1（Noto Emoji）\n")
        w("//\n")
        w(f"// {gx}x{gy} 格，每格 {size}x{size}，整图 {dim_x}x{dim_y} RGBA32。\n")
        w(f"//   边长 {max(dim_x, dim_y)} ≤ {MAX_DIM}（S10.5 定点上限，见 tools 内注释）。\n")
        w(f"// 图集数据以 {blob_kind} 内嵌（{len(blob)} 字节），"
          f"展开后 {dim_x * dim_y * 4} 字节。\n")
        w(f"// 共 {len(flags)} 面国旗（已按 glyph 去重，丢掉非主流属地别名）。\n")
        w("//\n")
        w("// 图集尺寸/网格与 emoji 图集**共用同一套宏**（两张图布局一致），\n")
        w("// 所以这里不再重复定义 DIM / LOG2 / GRID / TILE。\n")
        w("// ============================================================================\n\n")
        w("#pragma once\n\n")
        w(f"#define DJUI_MODERN_FLAG_COUNT {len(flags)}\n")
        w(f"/* 图集内嵌格式：1 = PNG（需 stb_image 解一次），0 = 裸 RGBA32。 */\n")
        w(f"#define DJUI_MODERN_FLAG_ATLAS_PNG {atlas_png}\n")
        w(f"/* PNG 字节数（--format raw 时为 RGBA32 字节数）。 */\n")
        w(f"#define DJUI_MODERN_FLAG_ATLAS_SIZE {len(blob)}\n\n")
        w("/*\n")
        w(" * 国旗键表（升序）：key = (ri1-0x1F1E6)*26 + (ri2-0x1F1E6)，\n")
        w(" * 下标 j 就是**国旗图集**里的 tile 下标（独立编号，从 0 开始）。\n")
        w(" */\n")
        w("static const unsigned short sDjuiModernFlagKeys[] = {\n")
        for i in range(0, len(keys), 12):
            chunk = keys[i:i + 12]
            w("    " + "".join("0x%04x," % k for k in chunk) + "\n")
        w("};\n\n")
        w(f"/* 国旗图集：{blob_kind}，{len(blob)} 字节。 */\n")
        w("ALIGNED16 static const unsigned char sDjuiModernFlagAtlas[] = {\n")
        w(_dump_byte_array(blob))
        w("};\n")


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        print(__doc__)
        return 1

    font_path, out_path = args[0], args[1]
    size = 32
    if "--size" in sys.argv:
        size = int(sys.argv[sys.argv.index("--size") + 1])

    flag_font_path = None
    if "--flags" in sys.argv:
        flag_font_path = sys.argv[sys.argv.index("--flags") + 1]

    fmt = "png"
    if "--format" in sys.argv:
        fmt = sys.argv[sys.argv.index("--format") + 1].lower()
        if fmt not in ("png", "raw"):
            print(f"error: --format 只支持 png / raw（收到 {fmt}）", file=sys.stderr)
            return 1

    font = TTFont(font_path)
    if "sbix" not in font:
        print("error: font has no sbix table (stb_truetype cannot read color glyphs "
              "from any other table)", file=sys.stderr)
        return 1

    ppem = max(font["sbix"].strikes.keys())
    strike = font["sbix"].strikes[ppem]
    cmap = font.getBestCmap()

    def has_bitmap(cp: int) -> bool:
        g = cmap.get(cp)
        return g is not None and g in strike.glyphs and strike.glyphs[g].imageData is not None

    wanted = set(parse_original_codepoints(EMOJI_HEADER))
    for lo, hi in EXTRA_RANGES:
        wanted.update(range(lo, hi + 1))

    cps = sorted(cp for cp in wanted if has_bitmap(cp))
    if not cps:
        print("error: no emoji bitmap found", file=sys.stderr)
        return 1

    # ── 国旗（可选，来自第二份字体）──
    flags = []
    fstrike = None
    fppem = 0
    n_lig = 0
    if flag_font_path:
        ff = TTFont(flag_font_path)
        if "sbix" not in ff:
            print("error: flag font has no sbix table", file=sys.stderr)
            return 1
        fppem = max(ff["sbix"].strikes.keys())
        fstrike = ff["sbix"].strikes[fppem]
        got = collect_flags(ff, fstrike)
        if got is None:
            print("error: 国旗字体里没有任何 Regional Indicator 连字 —— 它不是一份国旗字体，"
                  "或子集化时把 GSUB 表删掉了。", file=sys.stderr)
            print("       请改用 pyftsubset --layout-features='*' 重新子集化，"
                  "或直接提供原始字体。", file=sys.stderr)
            return 1
        n_lig = len(got)
        flags = dedupe_flags(got)
        print(f"flags : {n_lig} 条连字 / {len(flags)} 面国旗（去重后），来自 "
              f"{os.path.basename(flag_font_path)} (ppem={fppem})")

    # ── 两张独立图集（各自都要满足 S10.5 边长上限）──
    grid_e = safe_grid(len(cps), size)
    if grid_e is None or grid_e[0] * grid_e[1] < len(cps):
        print(f"error: {len(cps)} 个 emoji 装不进边长 ≤{MAX_DIM} 的图集"
              f"（--size 太大，请调小）", file=sys.stderr)
        return 1
    gx_e, gy_e = grid_e
    dim_x, dim_y = size * gx_e, size * gy_e

    e_atlas, dim_x, dim_y = build_atlas(
        len(cps), size, gx_e, gy_e, lambda i: strike.glyphs[cmap[cps[i]]].imageData)
    e_blob, e_kind, e_png = encode_blob(e_atlas, fmt)

    f_blob, f_kind, f_png = b"", "PNG", 1
    gx_f = gy_f = 0
    if flags:
        grid_f = safe_grid(len(flags), size)
        if grid_f is None or grid_f[0] * grid_f[1] < len(flags):
            print(f"error: {len(flags)} 面国旗装不进边长 ≤{MAX_DIM} 的图集"
                  f"（--size 太大，请调小）", file=sys.stderr)
            return 1
        gx_f, gy_f = grid_f
        f_atlas, fdx, fdy = build_atlas(
            len(flags), size, gx_f, gy_f, lambda i: fstrike.glyphs[flags[i][1]].imageData)
        if (fdx, fdy) != (dim_x, dim_y):
            print(f"error: 两张图集尺寸不一致 ({dim_x}x{dim_y} vs {fdx}x{fdy})；"
                  f"C 侧共用了同一套 DIM 宏", file=sys.stderr)
            return 1
        f_blob, f_kind, f_png = encode_blob(f_atlas, fmt)

    cmd = (f"python src/pc/djui_modern/tools/make_emoji_atlas.py {os.path.basename(font_path)} "
           f"{os.path.basename(out_path)} --size {size}")
    if flag_font_path:
        cmd += f" --flags {os.path.basename(flag_font_path)}"
    if fmt != "png":
        cmd += f" --format {fmt}"

    write_emoji_header(out_path, cps, e_blob, e_kind, e_png, size, gx_e, gy_e,
                       dim_x, dim_y, font_path, ppem, cmd)

    flag_out = None
    if flags:
        # 国旗图集：把输出路径里的 emoji 换成 flag
        flag_out = out_path.replace("emoji", "flag")
        if flag_out == out_path:
            flag_out = out_path.rsplit(".", 1)[0] + "_flag." + out_path.rsplit(".", 1)[1]
        write_flag_header(flag_out, flags, f_blob, f_kind, f_png, size, gx_f, gy_f,
                          dim_x, dim_y, flag_font_path, fppem, cmd)

    print(f"emoji : {len(cps)} tiles  grid {gx_e}x{gy_e}  {dim_x}x{dim_y}  {e_kind}={len(e_blob)}B")
    if flags:
        print(f"flags : {len(flags)} tiles  grid {gx_f}x{gy_f}  {dim_x}x{dim_y}  {f_kind}={len(f_blob)}B")
    print(f"edge  : {max(dim_x, dim_y)} ≤ {MAX_DIM} (S10.5 UV 上限)  OK")
    print(f"output: {out_path} ({os.path.getsize(out_path)} bytes)")
    if flag_out:
        print(f"output: {flag_out} ({os.path.getsize(flag_out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
