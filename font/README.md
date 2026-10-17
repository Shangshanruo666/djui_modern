# 字体来源与授权

## 当前内嵌字体：**字体链**（西文 + 中文）+ 现代 Emoji 图集

| 用途 | 文件 | 上游 | 授权 |
|---|---|---|---|
| 西文 | `RobotoCondensed-Subset.ttf`（16 KB / 123 字形） | Roboto Condensed | SIL OFL 1.1 |
| 中文 | `SourceHanSansSC-Subset.otf`（1.80 MB / 7028 字形） | Source Han Sans SC | SIL OFL 1.1 |
| Emoji | `djui_modern_emoji_data.h`（512×512 PNG 图集，179 格） | Noto Emoji | SIL OFL 1.1 |
| 国旗 | `djui_modern_flag_data.h`（512×512 PNG 图集，251 格） | Noto Emoji | SIL OFL 1.1 |

### 重新生成

```bash
# 1) 子集化
python src/pc/djui_modern/tools/make_subset_font.py build/RobotoCondensed-Regular.ttf \
        src/pc/djui_modern/font/RobotoCondensed-Subset.ttf --han none
python src/pc/djui_modern/tools/make_subset_font.py build/SourceHanSansSC-Regular.otf \
        src/pc/djui_modern/font/SourceHanSansSC-Subset.otf --han gb2312

# 2) 转成 C 头文件
python src/pc/djui_modern/tools/make_font_c.py src/pc/djui_modern/font/RobotoCondensed-Subset.ttf \
        src/pc/djui_modern/djui_modern_font_latin_data.h sDjuiModernLatinFont DJUI_MODERN_LATIN_FONT
python src/pc/djui_modern/tools/make_font_c.py src/pc/djui_modern/font/SourceHanSansSC-Subset.otf \
        src/pc/djui_modern/djui_modern_font_cjk_data.h sDjuiModernCjkFont DJUI_MODERN_CJK_FONT

# 3) Emoji + 国旗图集（一次生成两张；--size 默认 32，务必别超过 32）
#    --format 默认 png（推荐）；--format raw 出裸 RGBA32（头文件会大 4 倍）
#    国旗头文件名由 emoji 那个派生（emoji -> flag），见输出
python src/pc/djui_modern/tools/make_emoji_atlas.py build/Noto-3D-128.ttf \
        src/pc/djui_modern/djui_modern_emoji_data.h \
        --flags build/Noto-3D-128-flagsonly.ttf
```

> ⚠ `--han` **必须放在最后**（否则它的值会被当成位置参数）。

生成后建议跑一遍探针确认汉字能真正光栅化：

```bash
gcc -Iinclude -o /tmp/fontprobe src/pc/djui_modern/tools/font_probe.c -lm
/tmp/fontprobe src/pc/djui_modern/font/SourceHanSansSC-Subset.otf
```

## 授权要点（OFL 1.1 摘要）

- 允许：使用、研究、修改、再分发（含商业用途）。
- 要求：衍生字体**不得使用保留字体名**（"Source Han Sans" / "Noto" / "Roboto" 等）；
  分发时须附带本授权全文。
- 本模块只是把字体**作为数据内嵌**，未修改字体本身、未改字体名，
  授权文件随模块一起分发，符合 OFL 要求。
