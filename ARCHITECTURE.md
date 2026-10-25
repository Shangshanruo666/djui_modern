# djui_modern 架构

中文 | [English](./ARCHITECTURE.en.md)

## 范围

`djui_modern` 是 sm64coopdx 的 DJUI 渲染与字体适配模块。模块负责圆角矩形、
主题色应用、矢量字体和 Emoji 图集；DJUI 的布局、输入、裁剪、命中检测和
Lua 接口仍由原版代码负责。

所有新增实现都位于 `src/pc/djui_modern/`。对原版源码的修改限制在生命周期调用、
弱定义和已登记钩子的调用点。

## 模块划分

| 路径 | 职责 |
| --- | --- |
| `djui_modern.c` | 生命周期、主题遍历、矩形接管、面板字体策略 |
| `djui_modern_rrect.c` | 圆角轮廓、遮罩、顶点池和 display list |
| `djui_modern_font.c` | `DjuiFont` 适配器和字体槽安装 |
| `djui_modern_font_atlas.c` | TTF/OTF 光栅化、字形纹理、Emoji 和国旗 |
| `djui_modern_config.c` | 配置读取、默认文件生成和缺失项补全 |
| `djui_modern_gbi.h` | 模块私有的 display list 宏 |
| `*_data.h` | 生成的字体、Emoji 和国旗数据 |
| `font/` | 内嵌字体和授权信息 |
| `patches/` | 由工具生成的上游补丁 |
| `tools/` | 补丁和字形资源生成工具 |
| `install.sh` | 桌面与 Android 安装脚本 |

## 生命周期

1. `djui_init()` 在创建第一个文字节点之前调用 `djui_modern_init()`。
2. 初始化读取 `djui_modern.cfg`，准备圆角矩形后端，并替换配置指定的
   `gDjuiFonts` 槽位。
3. `djui_render()` 开始绘制时调用 `djui_modern_pre_render()`，重置帧状态、
   顶点池，并应用当前主题。
4. `djui_rect_render()` 调用 `djui_modern_rect_override()`。返回 true 表示模块
   已输出矩形；返回 false 时继续使用原版绘制。
5. 字体回调通过 `djui_modern_font.c` 进入字形后端。
6. `djui_render()` 结束绘制时调用 `djui_modern_post_render()`，执行可选的
   display list 诊断。
7. `djui_shutdown()` 调用 `djui_modern_shutdown()`，还原字体指针并释放资源。

## 上游接口

| 接口 | 调用方 | 实现 |
| --- | --- | --- |
| `djui_modern_init` | `djui_init` | `djui_modern.c` |
| `djui_modern_pre_render` | `djui_render` | `djui_modern.c` |
| `djui_modern_post_render` | `djui_render` | `djui_modern.c` |
| `djui_modern_shutdown` | `djui_shutdown` | `djui_modern.c` |
| `djui_modern_rect_override` | `djui_rect_render` | `djui_modern.c` |
| `djui_modern_note_font_size` | `djui_gfx_size_translate` | `djui_modern_font.c` |
| `djui_modern_note_text_base` | `djui_gfx_add_clipping_specific` | `djui_modern_font_atlas.c` |
| `djui_modern_note_text_created` | `djui_text_create` | `djui_modern.c` |
| `djui_modern_unicode_accepts` | `djui_unicode_valid_char` | `djui_modern_font_atlas.c` |

每个上游钩子只有一个实现。模块缺席时，由原版 `.c` 文件里的弱空定义保持
原有行为；头文件不增加钩子声明。

## 模块接口

| 接口组 | 主要函数 | 职责 |
| --- | --- | --- |
| 生命周期 | `djui_modern_init` / `pre_render` / `post_render` / `shutdown` | 模块状态和 DJUI 集成 |
| 字体适配 | `djui_modern_font_*` | 安装、查询和还原字体槽 |
| 字体后端 | `djui_modern_font_atlas_*` | 光栅化、测量、纹理和字形输出 |
| 圆角矩形 | `djui_modern_rrect_*` | 几何、遮罩、边框和 display list |
| 配置 | `djui_modern_config_load` | 读取配置并应用默认值 |

只有字体适配层可以修改 `gDjuiFonts`。主模块不直接访问字形纹理或字体表。

## 数据流

```text
DjuiBase
  -> djui_modern_rect_override
  -> 圆角矩形 display list

DjuiFont 回调
  -> djui_modern_font
  -> djui_modern_font_atlas
  -> UTF-8 码点
  -> 字体链与光栅档位
  -> 不可变字形纹理

djui_modern.cfg
  -> djui_modern_config
  -> 渲染、字体和圆角矩形模块
```

`pre_render` 从当前 `DjuiTheme` 读取基础颜色。矩形最终颜色和不透明度仍来自
`DjuiBase`，因此原版的淡出和交互状态继续生效。

## 上游补丁边界

当前补丁集修改以下原版文件：

| 文件 | 用途 |
| --- | --- |
| `Makefile` | 将 `src/pc/djui_modern` 加入 `SRC_DIRS` |
| `Android.mk` | 仅 Android：将 `src/pc/djui_modern` 加入 `SRC_DIRS` |
| `src/pc/djui/djui.c` | 生命周期调用 |
| `src/pc/djui/djui_rect.c` | 矩形接管钩子 |
| `src/pc/djui/djui_gfx.c` | 字号和文字裁剪钩子 |
| `src/pc/djui/djui_hud_utils.c` | N64 屏幕空间字号上报 |
| `src/pc/djui/djui_unicode.c` | Unicode 字符接受钩子 |
| `src/pc/djui/djui_text.c` | 文字节点创建钩子 |
| `src/pc/djui/djui_panel_host_mods.c` | mod 描述面板身份 |
| `src/pc/gfx/gfx_dxgi.cpp` | Windows UTF-16 代理对输入 |

桌面和 Android 只在原版文件内容不同时生成独立的 `.android.patch` 变体。
补丁由 `tools/make_patches.py` 生成，不建议手工修改。

Android 的 ndk-build 读取 `Android.mk` 的 `SRC_DIRS`，与根目录 `Makefile`
相互独立，两处必须同步修改。

## 维护约束

- 能放在模块内解决的行为，不增加新的上游修改。
- 一个上游函数只保留一个钩子，不为同一操作增加并行入口。
- 钩子原型写在调用它的 `.c` 文件中，避免影响整个 DJUI 头文件依赖图。
- `weak` 只用于原版文件中的空定义，不写在声明上。
- 运行时配置统一由 `djui_modern.cfg` 提供。
- 修改上游钩子后重新生成补丁，并分别验证桌面和 Android。
