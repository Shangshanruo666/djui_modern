<div align="center">

# djui_modern

**A modern DJUI appearance layer for sm64coopdx.**

English | [简体中文](./README.md)

<img alt="sm64coopdx" src="https://img.shields.io/badge/sm64coopdx-supported-1f6feb?style=flat-square" />
<img alt="DJUI skin" src="https://img.shields.io/badge/DJUI-skin-7c3aed?style=flat-square" />
<img alt="TTF and OTF" src="https://img.shields.io/badge/fonts-TTF%20%7C%20OTF-2ea043?style=flat-square" />
<img alt="Desktop" src="https://img.shields.io/badge/desktop-Windows%20%7C%20macOS%20%7C%20Linux-0078d4?style=flat-square" />
<img alt="Android" src="https://img.shields.io/badge/Android-supported-3ddc84?style=flat-square&logo=android&logoColor=white" />

</div>

## About

djui_modern is an appearance layer for sm64coopdx. It replaces DJUI's flat
rectangle fill with rounded, translucent surfaces and can render regular text
with TTF/OTF fonts, color Emoji, and flags.

The original DJUI continues to own layout, input, clipping, and the `djui_*`
Lua API. If the module is disabled or absent, the original renderer remains
active, so existing mods do not need a separate integration path.

## Features

- Consistent rounded surfaces for menus, buttons, inputs, sliders, chat,
  popups, and overlays.
- Active DJUI theme colors, including hover, pressed, disabled, and fade states.
- TTF/OTF support with embedded Latin, CJK, color Emoji, and flag assets.
- Unchanged DJUI layout, controls, and Lua behavior.
- One module implementation for desktop and Android, with platform-specific
  patches generated separately.
- Runtime appearance and font settings through `djui_modern.cfg`.

## Installation

The installer expects an sm64coopdx source tree containing:

- `Makefile`
- `src/pc/djui/`
- `src/pc/djui/djui.c`
- `src/pc/pc_main.c`

Run the installer from this repository and pass the game source tree:

```sh
# Desktop
sh install.sh --root /path/to/sm64coopdx

# Android
sh install.sh --android --root /path/to/sm64coopdx
```

The installer mounts the module at `src/pc/djui_modern` and applies the matching
upstream patches. To use a different repository:

```sh
sh install.sh --root /path/to/sm64coopdx --url <repository-url>
```

Options:

| Option | Purpose |
| --- | --- |
| `--android` | Use Android checks and patches |
| `--root <directory>` | Select the sm64coopdx source tree |
| `--url <repository>` | Select the repository used for the submodule |
| `--no-submodule` | Do not modify git; only check or apply patches |
| `--dry-run` | Check patch compatibility without writing files |
| `--revert` | Reverse the patches |
| `-h`, `--help` | Show installer help |

Build from the source tree root after installation:

```sh
make
```

## Configuration

On first launch, the module creates `djui_modern.cfg`:

- Desktop: beside the sm64coopdx executable.
- Android: in the game data directory beside `dynos/` and `mods/`.

Restart the game after changing the file. Missing options are appended on
module upgrades; existing values are not rewritten.

| Option | Default | Purpose |
| --- | --- | --- |
| `enabled` | `true` | Enable the module |
| `radius_panel` | `12` | Panel radius in 320x240 DJUI coordinates |
| `radius_element` | `8` | Regular control radius |
| `radius_pill` | `-1` | Pill radius; a negative value uses half the height |
| `corner_roundness` | `0.90` | Corner transition shape |
| `panel_alpha` | `244` | Panel opacity |
| `element_alpha` | `208` | Control opacity |
| `overlay_alpha` | `176` | Overlay opacity |
| `gradients` | `true` | Enable subtle gradients |
| `show_borders` | `true` | Draw rounded borders |
| `font_enabled` | `true` | Enable vector fonts |
| `font_path` | empty | Custom font file |
| `font_slots` | `0 3` | DJUI font slots to replace |
| `emoji` | `modern` | `modern` atlas or `vanilla` sprites |
| `text_scale` | `1.0` | Glyph raster scale; layout size is unchanged |

See [DEVELOPMENT.md](./DEVELOPMENT.md) for every option and the font-slot
semantics.

## Custom fonts

Set `font_path` to a `.ttf` or `.otf` file, or place a font in one of these
locations:

```text
<game directory>/djui_modern/*.ttf
<game directory>/djui_modern/*.otf
<game directory>/*.ttf
<game directory>/*.otf
```

External fonts are loaded first in the font chain. Embedded Latin and CJK fonts
remain available for missing glyphs. See [font/README.md](./font/README.md) for
font sources, subsets, and regeneration steps.

## Documentation

- [Development guide (Chinese)](./DEVELOPMENT.md): integration, patch
  generation, render constraints, and validation.
- [Architecture](./ARCHITECTURE.en.md): module boundaries, interfaces, and data
  flow.

## Contributing

- Do not change DJUI layout, input, clipping, or Lua behavior.
- Implement new behavior inside `src/pc/djui_modern` when possible.
- Change only registered upstream hooks and regenerate both patch variants.
- Validate desktop and Android source branches separately.

## License

Project code is licensed under the [MIT License](./LICENSE).

Bundled Roboto Condensed, Source Han Sans SC, Power Star, and Noto Emoji data
use the [SIL Open Font License 1.1](./font/OFL.txt).
