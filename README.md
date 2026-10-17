<div align="center">
  # djui_modern

  **适用于 sm64coopdx 的自包含现代化 DJUI 皮肤层。**

  [English](./README.en.md) | 简体中文

  <br />

  <img alt="sm64coopdx" src="https://img.shields.io/badge/sm64coopdx-supported-1f6feb?style=flat-square" />
  <img alt="DJUI skin" src="https://img.shields.io/badge/DJUI-skin-7c3aed?style=flat-square" />
  <img alt="TTF and OTF" src="https://img.shields.io/badge/fonts-TTF%20%7C%20OTF-2ea043?style=flat-square" />
  <img alt="Desktop" src="https://img.shields.io/badge/desktop-Windows%20%7C%20macOS%20%7C%20Linux-0078d4?style=flat-square" />
  <img alt="Android" src="https://img.shields.io/badge/Android-supported-3ddc84?style=flat-square&logo=android&logoColor=white" />
</div>

## 关于

djui_modern 是一个面向 sm64coopdx 的开源外观层。它把 DJUI 的平面矩形填充替换为圆角、半透明界面，同时保留原版的布局、输入、裁剪逻辑与 `djui_*` Lua 接口。

项目优先考虑上游可移植性和 Mod 兼容性。模块只在 `djui_rect_render()` 这一处接管共享矩形绘制原语；字体后端则按需把普通文字换成高分辨率 TTF/OTF，并提供内嵌现代 Emoji 图集。模块缺席或关闭时，原版渲染器会继续接管。

## 功能

- 让菜单、按钮、输入框、滑条、聊天栏、弹窗和 HUD 表面统一使用现代圆角外观。
- 跟随当前 DJUI 主题，并保留悬停、按下、禁用和覆盖层的原版配色。
- 支持 TTF/OTF 矢量字体、内置中文字体和 Emoji，也支持更换自定义字体。
- 保留原版 DJUI 布局、操作方式和 Mod 兼容性。
- 为桌面与 Android 源码树提供幂等、带标记校验的安装脚本。
- 通过首次启动自动生成的 `djui_modern.cfg` 调整外观与字体。

## 安装

安装脚本要求目标为包含 `Makefile`、`src/pc/djui/` 和 `src/pc/pc_main.c` 的 sm64coopdx 源码树。

```bash
# 桌面：Windows、macOS 或 Linux
sh src/pc/djui_modern/install.sh

# Android
sh src/pc/djui_modern/install-android.sh
```

脚本会把本目录挂载为 `src/pc/djui_modern`，应用已登记的原版改动，并跳过已经安装的补丁。常用参数：

| 参数 | 作用 |
| --- | --- |
| `--root <目录>` | 指定 sm64coopdx 源码树 |
| `--url <仓库地址>` | 指定挂载子模块时使用的仓库 |
| `--no-submodule` | 模块已经就位时，只应用补丁 |
| `--dry-run` | 只检查补丁兼容性，不写入文件 |
| `--revert` | 反向应用补丁，同时保留模块目录 |

安装完成后，在源码树根目录执行 `make`。

## 配置

首次启动时，djui_modern 会在桌面端可执行文件旁、或 Android 游戏数据目录中生成 `djui_modern.cfg`。修改后需重启游戏；模块更新时，缺失的配置项会自动补全。

### 自定义字体

把 `.ttf` 或 `.otf` 文件放进 coopdx 的数据目录（即 djui_modern.cfg 所在目录），或用 `font_path` 指向具体文件。外部字体在运行时加载，并作为字体链的第一项；字体缺字时会自动使用默认字体。

内嵌字体与 Emoji 图集采用 SIL Open Font License 1.1。来源与再生成方式见[字体说明](./font/README.md)，完整条款见[授权文本](./font/OFL.txt)。

## 技术手册

开发时 AI 写的中文手册在 [DEVELOPMENT.md](./DEVELOPMENT.md)，其中包含上游移植表、精确补丁位置、圆角几何、纹理策略、字体流水线、验证记录与已知限制。（AI 写的东西非常美味，确保你能看下去）

## 参与贡献

修改前，请保持以下边界：

- 不改变 DJUI 的布局、输入、裁剪与 Lua 行为。
- 只修改模块本身以及文档登记过的上游补丁面。
- 钩子发生变化时，同时验证桌面与 Android 两个源码分支。
- 始终保留原版渲染器作为自动回退路径。

## 开源协议

本项目代码基于 [MIT License](./LICENSE) 开源。

内嵌的 Roboto Condensed、Source Han Sans SC 与 Noto Emoji 数据采用 [SIL Open Font License 1.1](./font/OFL.txt)。
