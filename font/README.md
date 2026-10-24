# 字体资源与生成说明

本目录保存 djui_modern 的内嵌字体文件和授权信息。运行期字体数据位于
`djui_modern_font_latin_data.h`、`djui_modern_font_cjk_data.h`、
`djui_modern_font_star_data.h`、`djui_modern_emoji_data.h` 和
`djui_modern_flag_data.h`。

## 内嵌资源

| 用途 | 文件 | 内容 | 授权 |
| --- | --- | --- | --- |
| 西文与通用标点 | `RobotoCondensed-Subset.ttf` | 17 KB，138 个字形，129 个映射码点 | SIL OFL 1.1 |
| 中文字形补充 | `SourceHanSansSC-Subset.otf` | 1.78 MB，6964 个字形，6930 个映射码点 | SIL OFL 1.1 |
| 星星 | `Power Star.otf` | 2 KB，2 个字形，1 个映射码位（U+007F） | SIL OFL 1.1 |
| 彩色 Emoji | `djui_modern_emoji_data.h` | 512×512 PNG 图集，179 个条目 | SIL OFL 1.1 |
| 国旗 | `djui_modern_flag_data.h` | 512×512 PNG 图集，251 个条目 | SIL OFL 1.1 |

## 字体链

字体按注册顺序选择第一个包含目标码点的字体：

1. `font_path` 指定的外部字体。
2. 游戏目录中 `djui_modern/` 目录下的 `.ttf` 和 `.otf` 文件。
3. 游戏目录下的 `.ttf` 和 `.otf` 文件。
4. 内嵌 Roboto Condensed。
5. 内嵌 Source Han Sans SC。
6. 内嵌 Power Star。

Roboto Condensed 负责西文和通用标点。Source Han Sans SC 只保留 Roboto
子集不包含的字符，因此两份内嵌字体的 cmap 没有重叠。Power Star 只包含
U+007F，是该码位的唯一来源。

外部字体位于字体链首位，内嵌字体继续提供缺字回退。包含完整汉字的外部字体
会优先命中自身字形，不需要修改内嵌字体。

## 中文字符范围

Source Han Sans SC 子集从以下字符集合生成：

- `src/pc/djui/cn_glyphs.h` 中的全部游戏字符。
- GB2312 编码区中的标点和符号。
- GB2312 全部汉字，6763 个字符。

生成器直接解析该字表。生成时排除 Roboto Condensed 子集的全部 129 个映射码点，
最终中文子集包含 6930 个映射码点和 6964 个字形。

超出该范围的字符使用回退字形。需要生僻字时，可通过 `font_path` 指定完整字体，
或将字体放入上述外部字体目录。

## Emoji 与国旗

彩色 Emoji 和国旗使用离线生成的独立图集。Noto Emoji 的彩色字形以位图形式
存储在 `sbix` 表中，stb_truetype 无法直接光栅化这些字形，因此生成阶段将它们
提取为 RGBA 数据。

Emoji 图集包含 179 个条目。国旗图集包含 251 个条目，由两个 Regional
Indicator 字符组成一个键：

```text
key = (ri1 - 0x1F1E6) * 26 + (ri2 - 0x1F1E6)
```

两张图集使用相同的格布局，但编号相互独立。运行期分别通过升序键表进行二分查找。

Vtx.tc 使用 S10.5 定点坐标，单张图集的边长上限为 512。Emoji 与国旗分开存储，
以满足该限制。

## 重新生成字体

### Roboto Condensed

```sh
python src/pc/djui_modern/tools/make_subset_font.py \
  build/RobotoCondensed-Regular.ttf \
  src/pc/djui_modern/font/RobotoCondensed-Subset.ttf \
  --han none
```

### Source Han Sans SC

中文子集通过 `--exclude-font` 排除 Roboto 已提供的字符：

```sh
python src/pc/djui_modern/tools/make_subset_font.py \
  build/SourceHanSansSC-Regular.otf \
  src/pc/djui_modern/font/SourceHanSansSC-Subset.otf \
  --han gb2312 \
  --glyph-source src/pc/djui/cn_glyphs.h \
  --exclude-font src/pc/djui_modern/font/RobotoCondensed-Subset.ttf
```

生成器会读取 Roboto 的 cmap，并从中文候选字符中移除相同的码点。

### Power Star

Power Star 只包含 U+007F 码位，为 CoopDX 的星星字符提供字形。

```sh
python src/pc/djui_modern/tools/make_font_c.py \
  src/pc/djui_modern/font/Power Star.otf \
  src/pc/djui_modern/djui_modern_font_star_data.h \
  sDjuiModernStarFont DJUI_MODERN_STAR_FONT
```

### C 头文件

```sh
python src/pc/djui_modern/tools/make_font_c.py \
  src/pc/djui_modern/font/RobotoCondensed-Subset.ttf \
  src/pc/djui_modern/djui_modern_font_latin_data.h \
  sDjuiModernLatinFont DJUI_MODERN_LATIN_FONT

python src/pc/djui_modern/tools/make_font_c.py \
  src/pc/djui_modern/font/SourceHanSansSC-Subset.otf \
  src/pc/djui_modern/djui_modern_font_cjk_data.h \
  sDjuiModernCjkFont DJUI_MODERN_CJK_FONT
```

生成器使用 UTF-8 输出头文件，并写入字体文件大小和 SHA-256 摘要。

### Emoji 与国旗

```sh
python src/pc/djui_modern/tools/make_emoji_atlas.py \
  build/Noto-3D-128.ttf \
  src/pc/djui_modern/djui_modern_emoji_data.h \
  --flags build/Noto-3D-128-flagsonly.ttf
```

默认格尺寸为 32，PNG 为默认输出格式。

## 验证

确认两份内嵌字体的 cmap 没有交集：

```sh
python -c "from fontTools.ttLib import TTFont; \
a=TTFont('src/pc/djui_modern/font/RobotoCondensed-Subset.ttf'); \
b=TTFont('src/pc/djui_modern/font/SourceHanSansSC-Subset.otf'); \
print(len(set(a.getBestCmap()) & set(b.getBestCmap())))"
```

预期输出为 `0`。

使用字体探针检查汉字能否被 stb_truetype 正常打开和光栅化：

```sh
gcc -Iinclude -o /tmp/fontprobe \
  src/pc/djui_modern/tools/font_probe.c -lm
/tmp/fontprobe src/pc/djui_modern/font/SourceHanSansSC-Subset.otf
```

中文子集不包含 Roboto 已提供的 ASCII 字符，因此探针输出中
U+0041、U+0067、U+0031、U+0035、U+003F 和 U+0020 显示为 MISSING。
汉字与中文标点应正常输出尺寸和墨迹数据。

## 授权

内嵌字体和 Emoji 数据使用 SIL Open Font License 1.1。完整条款见
[OFL.txt](./OFL.txt)。

分发衍生字体时不得使用原字体的保留名称。本模块以数据文件形式内嵌原字体，
未修改字体名称，并随项目提供授权文本。
