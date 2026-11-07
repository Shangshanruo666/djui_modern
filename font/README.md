# 字体资源与生成说明

本目录保存 djui_modern 的内嵌字体文件和授权信息。运行期字体数据位于
`djui_modern_font_latin_data.h`、`djui_modern_font_cjk_data.h`、
`djui_modern_font_star_data.h`、`djui_modern_emoji_data.h` 和
`djui_modern_flag_data.h`。

## 内嵌资源

| 用途 | 文件 | 内容 | 授权 |
| --- | --- | --- | --- |
| 西文与通用标点 | `RobotoCondensed-Subset.ttf` | 121 KB，1317 个字形，922 个映射码点 | SIL OFL 1.1 |
| 中文字形补充 | `SourceHanSansSC-Subset.otf` | 13 MB，46926 个字形，27987 个映射码点 | SIL OFL 1.1 |
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

收录范围由 Unicode 区段表给出，两个内嵌字体的并集覆盖下列区段：

| 区段 | 码点范围 |
| --- | --- |
| ASCII | U+0020-U+007E |
| Latin-1 Supplement | U+0080-U+00FF |
| Latin Extended-A | U+0100-U+017F |
| Latin Extended-B | U+0180-U+024F |
| Cyrillic | U+0400-U+04FF |
| Cyrillic Supplement | U+0500-U+052F |
| General Punctuation | U+2000-U+206F |
| CJK 标点及全角空格 | U+3000-U+303F |
| 平假名与片假名 | U+3040-U+30FF |
| CJK 扩展 A | U+3400-U+4DBF |
| CJK 基本区 | U+4E00-U+9FFF |
| 全角形式常用子集 | U+FF01-U+FF5E |
| 半角片假名 | U+FF66-U+FF9D |

区段表覆盖中文与日文文本、玩家命名和 mod 文本用到的字符，繁简汉字同表。
Emoji 由独立彩色图集渲染，不在矢量字体的收录范围内。

Roboto Condensed 子集收录 922 个映射码点。Source Han Sans SC 子集排除
Roboto 已提供的码点后收录其余字符，共 27987 个映射码点。
两份内嵌字体的 cmap 没有交集。

字体数据按 1.5 MB 分片生成，每片是独立的 C 源文件。声明头只保存数组
描述，构建时各分片可并行编译。

区段表之外的字形使用回退字符。需要 CJK 扩展 B 及以后的生僻字时，
可通过 `font_path` 指定完整字体，或将字体放入上述外部字体目录。

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

中文子集按同一区段表生成，再通过 `--exclude-font` 排除 Roboto 已提供的字符：

```sh
python src/pc/djui_modern/tools/make_subset_font.py \
  build/SourceHanSansSC-Regular.otf \
  src/pc/djui_modern/font/SourceHanSansSC-Subset.otf \
  --han none \
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

确认两份内嵌字体的并集覆盖全部收录区段：

```sh
python -c "
from fontTools.ttLib import TTFont
a = TTFont('src/pc/djui_modern/font/RobotoCondensed-Subset.ttf').getBestCmap()
b = TTFont('src/pc/djui_modern/font/SourceHanSansSC-Subset.otf').getBestCmap()
ranges = [(0x20,0x7E),(0x80,0xFF),(0x100,0x17F),(0x180,0x24F),
          (0x400,0x4FF),(0x500,0x52F),(0x2000,0x206F),(0x3000,0x303F),
          (0x3040,0x30FF),(0x3400,0x4DBF),(0x4E00,0x9FFF),
          (0xFF01,0xFF5E),(0xFF66,0xFF9D)]
missing = [c for lo,hi in ranges for c in range(lo,hi+1) if c not in a and c not in b]
print(len(missing))"
```

输出为 Unicode 未分配位（控制符、孤立代理对、方向控制符）的数量，其余码位
都应有字形。

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
