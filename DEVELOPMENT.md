# djui_modern 开发与维护

本文记录模块的集成方式、上游补丁边界、渲染约束和验证流程。面向发布用户的
安装说明与配置概览见 [README.md](./README.md)，模块边界和接口清单见
[ARCHITECTURE.md](./ARCHITECTURE.md)。

## 设计原则

1. 保持 DJUI 的布局、输入、裁剪、命中检测和 Lua 行为不变。
2. 能在模块内完成的功能，不增加原版源码修改。
3. 对上游的修改只保留必要的生命周期调用、弱定义和钩子调用点。
4. 桌面与 Android 共用实现，只对确有差异的原版文件生成平台变体补丁。

## 源码树与安装

安装脚本要求目标目录是 sm64coopdx 源码树，并包含：

- `Makefile`
- `src/pc/djui/`
- `src/pc/djui/djui.c`
- `src/pc/pc_main.c`

模块在目标源码树中的固定位置是：

```text
src/pc/djui_modern/
```

默认安装流程会把模块作为 git 子模块挂载到该位置，然后应用补丁：

```sh
sh install.sh --root /path/to/sm64coopdx
sh install.sh --android --root /path/to/sm64coopdx
```

参数 `--android` 会启用 Android 分支检查、配置文件路径提示和 Android 补丁变体。
不传该参数时安装桌面版本。

以下情况使用 `--no-submodule`：

- 模块目录已经存在。
- 目标源码树不是 git 仓库。
- 安装流程不希望修改 `.gitmodules` 或索引。

```sh
cd /path/to/sm64coopdx
sh src/pc/djui_modern/install.sh --no-submodule
```

补丁安装是幂等的。脚本先检查补丁能否应用，再写入文件；已应用项由
`patches/markers.txt` 识别。

## 上游补丁边界

当前修改 9 个原版文件。桌面和 Android 的 `Makefile`、`djui.c` 内容不同，
因此各自生成一份变体补丁。

| 原版文件 | 修改 |
| --- | --- |
| `Makefile` | 将 `src/pc/djui_modern` 加入 `SRC_DIRS` |
| `src/pc/djui/djui.c` | 初始化和每帧生命周期调用 |
| `src/pc/djui/djui_rect.c` | 在 `djui_rect_render()` 入口转发矩形绘制 |
| `src/pc/djui/djui_gfx.c` | 上报文字字号和文字所属控件 |
| `src/pc/djui/djui_hud_utils.c` | 上报 N64 屏幕空间中的 HUD 字号 |
| `src/pc/djui/djui_unicode.c` | 放行模块能够渲染的 Emoji 和国旗 |
| `src/pc/djui/djui_text.c` | 上报新建文字节点的字体策略 |
| `src/pc/djui/djui_panel_host_mods.c` | 暴露 mod 描述面板指针 |
| `src/pc/gfx/gfx_dxgi.cpp` | 合并 Windows UTF-16 代理对并转换 UTF-8 |

所有钩子声明都写在调用它的 `.c` 文件中，不修改上游头文件。这样可以避免
`djui.h` 的依赖树在应用补丁后大范围重编。

### 弱符号

模块缺席时，原版文件仍需要正常编译和链接，因此各钩子在原版 `.c` 文件中提供
空定义：

```c
#if defined(__GNUC__)
__attribute__((weak))
#endif
bool djui_modern_rect_override(struct DjuiBase* base) {
    (void)base;
    return false;
}
```

`weak` 只写在空定义上。不要把 `weak` 加到声明上，否则模块内的强定义也可能被
降级，链接后可能静默选中空实现。

### 初始化顺序

`djui_modern_init()` 必须在 `djui_init()` 创建第一个文字节点之前调用。
`djui_text_create()` 会保存当前 `gDjuiFonts[slot]` 指针；如果模块稍后才替换
字体槽，已创建节点仍会持有原版字体指针。

## 模块结构

```text
src/pc/djui_modern/
├── djui_modern.c                 生命周期、主题、矩形接管
├── djui_modern_rrect.c           圆角轮廓、遮罩、边框、顶点池
├── djui_modern_font.c            DjuiFont 适配器和槽位安装
├── djui_modern_font_atlas.c      TTF/OTF 光栅化和字形纹理
├── djui_modern_config.c          配置读取和默认文件维护
├── djui_modern_gbi.h             模块私有 display list 宏
├── *_data.h                      生成的字体、Emoji 和国旗数据
├── font/                         内嵌字体和授权信息
├── patches/                      生成的上游补丁
├── tools/                        补丁与字体资源生成工具
└── install.sh                    桌面与 Android 安装脚本
```

## 渲染流程

### 矩形接管

DJUI 的矩形填充集中在 `djui_rect_render()`。模块在函数入口调用：

```c
if (djui_modern_rect_override(base)) {
    return true;
}
```

返回 true 时，模块已经输出矩形；返回 false 时，原版实现继续执行。

接管点位于原版设置颜色之后，因此模块读取到的是当前帧实际的颜色、透明度、
渐变和交互状态。原版淡出效果不需要在模块中重新模拟。

### 主题处理

`djui_modern_pre_render()` 遍历当前 DJUI 控件树，将控件颜色映射为以下方案：

- 可交互元素。
- 半透明覆盖层。
- 面板背景。
- 次级面板或段落背景。
- 带自定义色相的填充。
- 自绘底板容器。

映射结果记录在 `sThemedMap` 中。后续帧只更新悬停、按下、禁用和透明度状态，
不会基于模块已修改过的颜色重复推导，避免颜色逐帧衰减。

### 圆角几何

圆角轮廓采用“羊角螺线 + 正圆弧 + 羊角螺线”：

- `corner_roundness = 1` 时退化为正圆弧。
- `corner_roundness = 0` 时全程使用羊角螺线。
- 默认值 `0.90` 保留圆的中段，并在与直边相接处平滑过渡曲率。

可见形状由 64x64 RGBA 遮罩承载，颜色由顶点色提供。主填充使用 4x6 顶点网格，
一次填充输出 24 个顶点和 9 个四边形。

圆角半径最终受下列条件限制：

- 配置半径。
- 元素短边的一半。
- `RRECT_MAX_RADIUS`，当前为 24 个 DJUI 坐标单位。

圆角遮罩纹理地址固定，运行期只上传一次。顶点来自预分配对象池，每帧重置，
正常绘制路径不在帧内分配内存。

### 边框

原版方角边框由模块接管，避免同一控件同时出现方角外框和圆角填充。

圆角边框由轮廓环带生成：

- 实心环带负责主要边框颜色。
- 内外羽化环带减小弧线边缘的锯齿。
- 边框沿轮廓向外生长，配置半径代表可见圆角半径。

关闭 `show_borders` 时，模块不绘制边框。

## 字体流程

### 字体槽

模块替换 `gDjuiFonts` 中由 `font_slots` 指定的槽位。

| 槽位 | 名称 | 说明 |
| --- | --- | --- |
| 0 | Normal | DJUI 常规字体 |
| 1 | Title | DJUI 标题字体 |
| 2 | HUD | HUD 字体 |
| 3 | Aliased | DJUI 锐利字体 |
| 4 | CustomHUD | 自定义 HUD 字体 |
| 5 | RecolorHUD | 重着色 HUD 字体 |
| 6 | Special | 特殊字体 |

默认替换槽 0 和槽 3。

其他槽位保持原版艺术字或 HUD 字形。崩溃界面使用原版字体的布局假设，因此
模块在 `gDjuiDisabled` 状态下交回原版字体回调和度量。

### 字号来源

`DjuiFont` 的绘制回调不直接接收字号。模块通过原版字号转换函数上报当前值：

- `djui_gfx_size_translate()` 覆盖 DJUI 控件和输入框文字。
- `djui_hud_utils.c` 的 N64 分支覆盖 HUD 文字。

上报名称为矩阵字号。字体后端根据窗口高度换算为屏幕 em 像素，再选择光栅档位。

### 字体链

字体按以下顺序加载：

1. `font_path` 指定的外部字体。
2. 游戏目录下 `djui_modern/` 中的 `.ttf` 或 `.otf`。
3. 游戏目录中的 `.ttf` 或 `.otf`。
4. 内嵌 Roboto Condensed、Source Han Sans SC 和 Power Star。

Power Star 只包含 U+007F 码位，为 CoopDX 的星星字符提供字形。

每个码点使用字体链中第一个包含该字形的字体。外部字体不会屏蔽内嵌字体的缺字
回退。

### 字形纹理

每个 `(字符, 光栅档位)` 使用一张独立的二次幂 RGBA32 纹理。纹理生成后不再修改，
因此 `gfx_pc` 以纹理数据地址为键的缓存可以安全复用。

主要限制：

- 单张字形纹理边长上限为 512。
- `Vtx.tc` 使用 S10.5 定点坐标，UV 不能超过 32767。
- 全部字形纹理预算上限为 128 MiB。
- 连续使用同一纹理时跳过重复绑定。

测量与绘制使用同一档位计算。`char_width()` 是无状态接口，并通过前瞻下一个
字符计算字偶间距。

### 文字裁剪

DJUI 按名义字格计算纹理裁剪比例：

```text
字格 = DjuiFont.charWidth × DjuiFont.charHeight
```

模块的字形四边形表示墨迹区域，与名义字格尺寸不同。`glyph_fix_pending_clip()`
将水平裁剪比例换算到墨迹四边形坐标，使裁剪边界落在与原版相同的位置。

垂直裁剪默认允许文字上下溢出控件。聊天框依赖裁剪隐藏滚出可见区的行，因此
仍保留垂直硬裁剪。

### Emoji 和国旗

彩色 Emoji 和国旗使用离线生成的 RGBA 图集。Emoji 与国旗分成两张 512x512 图集，
分别通过升序码点表查找。

国旗由两个 Regional Indicator 组成。DJUI 逐字符调用字体回调，因此模块在
`char_width()` 和 `render_char()` 中跟踪配对：

- 每面国旗的第一个字符负责宽度和绘制。
- 第二个字符宽度为零且不输出几何。
- 下一个字符继续沿用上一字符的结束指针，避免跨字符串误配对。

Windows 的非 BMP 输入经 `gfx_dxgi_on_text_input()` 合并 UTF-16 代理对，再按
UTF-8 发送给 DJUI。`djui_unicode_accepts()` 放行模块图集支持的 Emoji 和国旗。

## 配置

配置文件名为 `djui_modern.cfg`。

- 桌面端位于可执行文件同级。
- Android 端位于游戏数据目录，与 `dynos/`、`mods/` 同级。

文件不存在时生成完整默认配置。文件已存在时只追加缺失键，不重写现有值。

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `enabled` | `true` | 模块总开关 |
| `radius_panel` | `12` | 大面板圆角 |
| `radius_element` | `8` | 普通控件圆角 |
| `radius_pill` | `-1` | 胶囊圆角，负值表示高度的一半 |
| `corner_roundness` | `0.90` | 圆角过渡形状，范围 0 到 1 |
| `panel_alpha` | `244` | 面板不透明度 |
| `element_alpha` | `208` | 控件不透明度 |
| `overlay_alpha` | `176` | 覆层不透明度 |
| `gradients` | `true` | 是否使用轻微渐变 |
| `gradient_top_boost` | `1.18` | 顶部亮度倍率 |
| `gradient_bottom_scale` | `0.86` | 底部亮度倍率 |
| `show_borders` | `true` | 是否绘制圆角边框 |
| `font_enabled` | `true` | 是否启用矢量字体 |
| `font_path` | 空 | 外部字体路径 |
| `font_cjk_raster_boost` | `1.0` | CJK 光栅尺寸倍率 |
| `font_bilinear` | `true` | 字形是否使用双线性过滤 |
| `font_native_metrics` | `true` | 使用字体原生西文宽度 |
| `font_fallback_kerning` | `true` | 字体无字偶数据时使用内置表 |
| `font_slots` | `0 3` | 替换的字体槽 |
| `emoji` | `modern` | `modern` 或 `vanilla` |
| `text_scale` | `1.0` | 全局字形光栅倍率，范围 0.5 到 2.0 |
| `font_player_list` | `false` | 玩家列表是否使用矢量字体 |
| `font_mod_description` | `false` | mod 列表和描述是否使用矢量字体 |
| `show_stats` | `false` | 输出矩形、字体和 display list 诊断 |

## 内嵌资源

字体子集和 Emoji 图集由 `tools/` 中的脚本生成：

```sh
# 子集化
python src/pc/djui_modern/tools/make_subset_font.py \
  build/RobotoCondensed-Regular.ttf \
  src/pc/djui_modern/font/RobotoCondensed-Subset.ttf --han none

python src/pc/djui_modern/tools/make_subset_font.py \
  build/SourceHanSansSC-Regular.otf \
  src/pc/djui_modern/font/SourceHanSansSC-Subset.otf \
  --han gb2312 \
  --glyph-source src/pc/djui/cn_glyphs.h \
  --exclude-font src/pc/djui_modern/font/RobotoCondensed-Subset.ttf

# 转换为 C 头文件
python src/pc/djui_modern/tools/make_font_c.py \
  src/pc/djui_modern/font/RobotoCondensed-Subset.ttf \
  src/pc/djui_modern/djui_modern_font_latin_data.h \
  sDjuiModernLatinFont DJUI_MODERN_LATIN_FONT

python src/pc/djui_modern/tools/make_font_c.py \
  src/pc/djui_modern/font/SourceHanSansSC-Subset.otf \
  src/pc/djui_modern/djui_modern_font_cjk_data.h \
  sDjuiModernCjkFont DJUI_MODERN_CJK_FONT

# Emoji 和国旗图集
python src/pc/djui_modern/tools/make_emoji_atlas.py \
  src/pc/djui_modern/font/Noto-3D-128-Subset.ttf \
  src/pc/djui_modern/djui_modern_emoji_data.h \
  --size 32 \
  --flags src/pc/djui_modern/font/Noto-3D-128-flagsonly-Subset.ttf
```

来源、授权和字表范围见 [font/README.md](./font/README.md)。

## 补丁生成

需要两份未修改的上游源码树作为基准：

- 桌面分支。
- Android 分支。

在已安装模块的源码树根目录执行：

```sh
python src/pc/djui_modern/tools/make_patches.py \
  --upstream /path/to/upstream-desktop \
  --upstream-android /path/to/upstream-android \
  --current .
```

脚本只处理 `TARGET_FILES` 中的文件，并重新生成：

- `patches/*.patch`
- 平台变体 `*.android.patch`
- `patches/markers.txt`

不要在补丁目录中手工维护额外修改。

## 构建与验证

### 桌面增量构建

MSYS2 环境需要先选择工具链目录：

```sh
export PATH="/ucrt64/bin:/mingw64/bin:$PATH"
make -j2
```

至少确认：

- `src/pc/djui_modern` 下的 `.c` 文件完成编译。
- 可执行文件完成链接。
- 启动日志出现 `djui_modern` 初始化信息。
- 主菜单、聊天栏、输入框、滑条和弹窗显示正常。

### 补丁检查

桌面：

```sh
sh install.sh --root /path/to/desktop-tree --no-submodule --dry-run
```

Android：

```sh
sh install.sh --android --root /path/to/android-tree --no-submodule --dry-run
```

在临时克隆或可恢复的源码树中测试。测试结束后恢复原版仓库，避免测试补丁进入
上游对照目录。

### 重点回归项

- 首次创建文字节点时已经安装字体槽。
- 点击、悬停、禁用和淡出颜色正常。
- 聊天消息滚出可见区后被正确裁剪。
- 普通控件的下伸字母和汉字不会被垂直裁掉。
- 输入框可以输入中文、Emoji 和国旗。
- 崩溃界面仍使用原版等宽字体。
- 桌面和 Android 补丁均无失败项。

## 已知限制

- 圆角半径受 `RRECT_MAX_RADIUS` 和控件短边限制。
- 字形纹理最长边为 512，单字形不能继续提高分辨率。
- 字形纹理总预算为 128 MiB，超出后不再光栅化新字形。
- 内嵌中文字体是 GB2312 子集，生僻字需要外部完整字体。
- Emoji 和国旗来自固定离线图集，新增字符需要重新生成数据头文件。
- Android 构建需要完整的 Android 分支和对应工具链。
- `show_stats` 会输出较多日志，只用于诊断。
