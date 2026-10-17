<div align="center">
  # djui_modern

  **A self-contained modern DJUI skin layer for sm64coopdx.**

  English | [简体中文](./README.md)

  <br />

  <img alt="sm64coopdx" src="https://img.shields.io/badge/sm64coopdx-supported-1f6feb?style=flat-square" />
  <img alt="DJUI skin" src="https://img.shields.io/badge/DJUI-skin-7c3aed?style=flat-square" />
  <img alt="TTF and OTF" src="https://img.shields.io/badge/fonts-TTF%20%7C%20OTF-2ea043?style=flat-square" />
  <img alt="Desktop" src="https://img.shields.io/badge/desktop-Windows%20%7C%20macOS%20%7C%20Linux-0078d4?style=flat-square" />
  <img alt="Android" src="https://img.shields.io/badge/Android-supported-3ddc84?style=flat-square&logo=android&logoColor=white" />
</div>

## About

djui_modern is an open-source appearance layer for sm64coopdx. It replaces DJUI's flat rectangular fill with rounded, translucent surfaces while preserving the original layout, input, clipping, and `djui_*` Lua API.

The project prioritizes upstream portability and mod compatibility. The module takes over the shared rectangle primitive at `djui_rect_render()` and optionally replaces regular text with high-resolution TTF/OTF rendering and a built-in modern emoji atlas. When the module is unavailable or disabled, the original renderer remains active.

## Features

- Give menus, buttons, inputs, sliders, chat, popups, and HUD surfaces a consistent modern rounded appearance.
- Follow the active DJUI theme while preserving its hover, pressed, disabled, and overlay colors.
- Support TTF/OTF fonts, bundled Chinese and emoji glyphs, and custom fonts.
- Keep the original DJUI layout, controls, and mod compatibility.
- Provide idempotent, marker-checked installation scripts for desktop and Android source trees.
- Configure appearance and fonts through `djui_modern.cfg`, generated automatically on first launch.

## Installation

The installer expects an sm64coopdx source tree containing `Makefile`, `src/pc/djui/`, and `src/pc/pc_main.c`.

```bash
# Desktop: Windows, macOS, or Linux
sh src/pc/djui_modern/install.sh

# Android
sh src/pc/djui_modern/install-android.sh
```

The scripts mount this directory at `src/pc/djui_modern`, apply the registered upstream changes, and skip patches that are already installed. Common options:

| Option | Purpose |
| --- | --- |
| `--root <directory>` | Select the sm64coopdx source tree |
| `--url <repository>` | Select the repository used for the submodule |
| `--no-submodule` | Apply patches only when the module is already in place |
| `--dry-run` | Check patch compatibility without writing files |
| `--revert` | Reverse the patches while keeping the module directory |

After installation, run `make` from the source tree root.

## Configuration

On first launch, djui_modern creates `djui_modern.cfg` beside the executable on desktop or in the game data directory on Android. Restart the game after changing it. Missing options are added automatically when the module is updated.

### Custom Fonts

Place a `.ttf` or `.otf` file in the coopdx data directory, where `djui_modern.cfg` is located, or point `font_path` to a specific file. External fonts are loaded at runtime and placed first in the font chain; missing glyphs automatically use the default font.

Bundled fonts and emoji data use the SIL Open Font License 1.1. See the [font guide](./font/README.md) and [license text](./font/OFL.txt).

## Technical Guide

The Chinese development manual is available in [DEVELOPMENT.md](./DEVELOPMENT.md). It covers the upstream integration table, exact patch locations, rounded-corner geometry, texture strategy, font pipeline, validation notes, and known limitations.

## Contributing

Keep the following boundaries when making changes:

- Do not change DJUI layout, input, clipping, or Lua behavior.
- Limit changes to the module and its documented upstream patch surface.
- When hooks change, verify both desktop and Android source branches.
- Always retain the original renderer as an automatic fallback.

## License

Project code is licensed under the [MIT License](./LICENSE).

Bundled Roboto Condensed, Source Han Sans SC, and Noto Emoji data use the [SIL Open Font License 1.1](./font/OFL.txt).
