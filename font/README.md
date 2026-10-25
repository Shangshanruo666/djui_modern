# 字体资源与生成说明

本目录保存 djui_modern 的内嵌字体文件和授权信息。运行期字体数据位于
`djui_modern_font_latin_data.h`、`djui_modern_font_cjk_data.h`、
`djui_modern_font_star_data.h`、`djui_modern_emoji_data.h` 和
`djui_modern_flag_data.h`。

## 内嵌资源

| 用途 | 文件 | 内容 | 授权 |
| --- | --- | --- | --- |
| 西文与通用标点 | `RobotoCondensed-Subset.ttf` | 55 KB，580 个字形，310 个映射码点 | SIL OFL 1.1 |
| 中文字形补充 | `SourceHanSansSC-Subset.otf` | 3.2 MB，13198 个字形，6992 个映射码点 | SIL OFL 1.1 |
| 星星 | `Power Star.otf` | 2 KB，2 个字形，1 个映射码位（U+007F） | SIL OFL 1.1 |
| Emoji 源字体 | `Noto-3D-128-Subset.ttf` | 14 MB，461 个字形，位图数据 | SIL OFL 1.1 |
| 国旗源字体 | `Noto-3D-128-flagsonly-Subset.ttf` | 6.9 MB，279 个字形，位图数据 | SIL OFL 1.1 |
| 彩色 Emoji | `djui_modern_emoji_data.h` | 512×512 PNG 图集，179 个条目 | SIL OFL 1.1 |
| 国旗 | `djui_modern_flag_data.h` | 512×512 PNG 图集，251 个条目 | SIL OFL 1.1 |

Noto 字体是图集生成的源文件，不参与运行期加载。

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

## 字符范围

子集范围以原版字表为基准，两个内嵌字体的并集覆盖字表全部字符：

- `src/pc/djui/djui_unicode.c` 的 `sSmCodeGlyphs` 主表，154 个西文字符，含
  拉丁扩展、西里尔字母和组合符号。
- `src/pc/djui/cn_glyphs.h` 的 6791 个汉字和中文标点。
- `src/pc/djui/emoji_glyphs.h` 的 52 个 Emoji，由独立彩色图集渲染。
- GB2312 编码区的标点和符号，以及 GB2312 全部 6763 个汉字。
- 补充字符：GB2312 不收录的全角字母数字、表意空格、直角引号、中文排版
  标点和常用货币与数学符号。中文排版、mod 文本和玩家命名会用到这些字符。
- ASCII 可见区 0x20 至 0x7E。

Roboto Condensed 子集收录西文、ASCII 和补充字符中的货币与数学符号，共 310 个
映射码点。Source Han Sans SC 子集排除 Roboto 已提供的 310 个码点后收录其余
字符，共 6992 个映射码点。
两份内嵌字体的 cmap 没有交集。

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
  --han none \
  --glyph-source src/pc/djui/djui_unicode.c
```

### Source Han Sans SC

中文子集通过 `--exclude-font` 排除 Roboto 已提供的字符。两个字表都传入，
保证汉字、中文标点和西文重音字符都不遗漏：

```sh
python src/pc/djui_modern/tools/make_subset_font.py \
  build/SourceHanSansSC-Regular.otf \
  src/pc/djui_modern/font/SourceHanSansSC-Subset.otf \
  --han gb2312 \
  --glyph-source src/pc/djui/djui_unicode.c \
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
  src/pc/djui_modern/font/Noto-3D-128-Subset.ttf \
  src/pc/djui_modern/djui_modern_emoji_data.h \
  --size 32 \
  --flags src/pc/djui_modern/font/Noto-3D-128-flagsonly-Subset.ttf
```

格尺寸为 32，PNG 为默认输出格式。

位图字体没有可用的字符范围，子集化范围由码点清单给出。清单已随仓库提供：

```sh
python src/pc/djui_modern/tools/make_subset_font.py \
  src/pc/djui_modern/font/Noto-3D-128-Subset.ttf \
  --codepoints src/pc/djui_modern/font/emoji-codepoints.txt

python src/pc/djui_modern/tools/make_subset_font.py \
  src/pc/djui_modern/font/Noto-3D-128-flagsonly-Subset.ttf \
  --codepoints src/pc/djui_modern/font/emoji-codepoints.txt
```

国旗字体的连字依赖 GSUB，子集化时保留全部布局特性，因此字形数量不变，
体积只减少未使用的 strike 数据。

`make_emoji_codepoints.py` 用于从完整字体重新生成码点清单，需要完整字体文件。

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
