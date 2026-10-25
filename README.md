<div align="center">

# djui_modern

**适用于 sm64coopdx 的现代化 DJUI 外观模块。**

简体中文 | [English](./README.en.md)

<img alt="sm64coopdx" src="https://img.shields.io/badge/sm64coopdx-supported-1f6feb?style=flat-square" />
<img alt="DJUI skin" src="https://img.shields.io/badge/DJUI-skin-7c3aed?style=flat-square" />
<img alt="TTF and OTF" src="https://img.shields.io/badge/fonts-TTF%20%7C%20OTF-2ea043?style=flat-square" />
<img alt="Desktop" src="https://img.shields.io/badge/desktop-Windows%20%7C%20macOS%20%7C%20Linux-0078d4?style=flat-square" />
<img alt="Android" src="https://img.shields.io/badge/Android-supported-3ddc84?style=flat-square&logo=android&logoColor=white" />

</div>

## 关于

djui_modern 是面向 sm64coopdx 的外观层。它将 DJUI 的平面矩形填充替换为圆角、
半透明表面，并可为普通文字提供 TTF/OTF 字体、彩色 Emoji 和国旗支持。

布局、输入、裁剪和 `djui_*` Lua 接口继续由原版 DJUI 负责。模块缺席或关闭时，
原版渲染器会接管，因此现有 Mod 无需专门适配。

## 功能

- 菜单、按钮、输入框、滑条、聊天栏、弹窗和浮层使用统一的圆角外观。
- 沿用当前 DJUI 主题，以及控件自身的悬停、按下、禁用和淡出颜色。
- 支持 TTF/OTF 字体、内嵌中文与西文字体、彩色 Emoji 和国旗。
- 保留 DJUI 布局、操作方式和 Lua 接口行为。
- 桌面与 Android 共用同一套模块代码和补丁生成流程。
- 通过首次启动生成的 `djui_modern.cfg` 调整外观和字体。

## 安装

安装脚本要求目标是 sm64coopdx 源码树，并至少包含以下内容：

- `Makefile`
- `src/pc/djui/`
- `src/pc/djui/djui.c`
- `src/pc/pc_main.c`

在模块仓库中执行：

```sh
# 桌面
sh install.sh --root /path/to/sm64coopdx

# Android
sh install.sh --android --root /path/to/sm64coopdx
```

安装器会把模块挂载到 `src/pc/djui_modern`，并应用对应的原版补丁。需要指定模块
仓库地址时：

```sh
sh install.sh --root /path/to/sm64coopdx --url <仓库地址>
```

也可以直接把脚本管道给 bash，位置参数为模块分支：

```sh
# 桌面
curl -LSs https://raw.githubusercontent.com/Shangshanruo666/djui_modern/main/install.sh \
  | bash -s main --root /path/to/sm64coopdx

# Android
curl -LSs https://raw.githubusercontent.com/Shangshanruo666/djui_modern/main/install.sh \
  | bash -s main --android --root /path/to/sm64coopdx
```

常用参数：

| 参数 | 作用 |
| --- | --- |
| `--android` | 使用 Android 检查和补丁 |
| `--root <目录>` | 指定 sm64coopdx 源码树 |
| `--url <地址>` | 指定挂载子模块时使用的仓库地址 |
| `--no-submodule` | 不操作 git，只检查或应用补丁 |
| `--dry-run` | 只检查补丁兼容性，不写入文件 |
| `--revert` | 反向应用补丁 |
| `-h`, `--help` | 显示安装器帮助 |

位置参数为挂载子模块时使用的分支，省略时使用仓库默认分支。

安装完成后，在源码树根目录执行：

```sh
make
```

## 配置

首次启动时，模块会生成 `djui_modern.cfg`：

- 桌面端：与 `sm64coopdx` 可执行文件同级。
- Android：与 `dynos/`、`mods/` 同级的游戏数据目录。

修改配置后需要重启游戏。启动时文件会与模板比对：缺失的配置项补回默认值，
模板之外的配置项移除，注释恢复为模板内容，已有取值保持不变。

| 配置项 | 默认值 | 作用 |
| --- | --- | --- |
| `enabled` | `true` | 是否启用模块 |
| `radius_panel` | `12` | 大面板圆角，使用 320x240 DJUI 坐标 |
| `radius_element` | `8` | 普通控件圆角 |
| `radius_pill` | `-1` | 胶囊圆角；负值表示高度的一半 |
| `corner_roundness` | `0.90` | 圆角过渡形状 |
| `panel_alpha` | `244` | 面板不透明度 |
| `element_alpha` | `208` | 控件不透明度 |
| `overlay_alpha` | `176` | 浮层不透明度 |
| `gradients` | `true` | 是否启用轻微渐变 |
| `show_borders` | `true` | 是否绘制圆角边框 |
| `font_enabled` | `true` | 是否启用矢量字体 |
| `font_path` | 空 | 自定义字体文件路径 |
| `font_slots` | `0 3` | 替换的 DJUI 字体槽 |
| `emoji` | `modern` | `modern` 使用模块图集，`vanilla` 使用原版贴图 |
| `text_scale` | `1.0` | 字形光栅倍率，不改变排版尺寸 |

完整配置项、字体槽含义和内部行为见 [DEVELOPMENT.md](./DEVELOPMENT.md)。

## 自定义字体

可以通过 `font_path` 指定 `.ttf` 或 `.otf` 文件，也可以把字体放入以下位置：

```text
<游戏目录>/djui_modern/*.ttf
<游戏目录>/djui_modern/*.otf
<游戏目录>/*.ttf
<游戏目录>/*.otf
```

外部字体作为字体链首项加载；缺少字形时，内嵌西文、中文和星形字体继续提供回退字形。
字体来源、子集范围和重新生成方法见 [font/README.md](./font/README.md)。

## 文档

- [开发与维护](./DEVELOPMENT.md)：集成方式、补丁生成、渲染约束和验证流程。
- [架构与接口](./ARCHITECTURE.md)：模块边界、接口清单和数据流。

## 参与贡献

修改模块时请保持以下边界：

- 不改变 DJUI 的布局、输入、裁剪或 Lua 行为。
- 优先在 `src/pc/djui_modern` 内实现功能。
- 只修改已登记的上游钩子，并同步重新生成桌面和 Android 补丁。
- 修改钩子后分别验证两个平台分支。

## 许可

模块代码使用 [MIT License](./LICENSE)。

内嵌的 Roboto Condensed、Source Han Sans SC、Power Star 和 Noto Emoji 数据使用
[SIL Open Font License 1.1](./font/OFL.txt)。
