# djui_modern Architecture

[简体中文](./ARCHITECTURE.md) | English

## Scope

`djui_modern` is a DJUI renderer and font adapter for sm64coopdx. The module
owns rounded rectangles, theme application, vector fonts, and Emoji atlases.
The original DJUI continues to own layout, input, clipping, hit testing, and
the Lua API.

All new implementation code lives under `src/pc/djui_modern/`. Upstream changes
are limited to lifecycle calls, weak definitions, registered hook call sites,
and `DjuiBase.type` control-category metadata.

## Module layout

| Path | Responsibility |
| --- | --- |
| `djui_modern.c` | Lifecycle, theme pass, rectangle takeover, panel font policy |
| `djui_modern_rrect.c` | Rounded outlines, mask, vertex pool, display lists |
| `djui_modern_font.c` | `DjuiFont` adapters and font-slot installation |
| `djui_modern_font_atlas.c` | TTF/OTF rasterization, glyph textures, Emoji, flags |
| `djui_modern_config.c` | Configuration loading, settings-panel I/O, and default-file maintenance |
| `djui_modern_settings.c` | Settings panel built from native DJUI controls |
| `djui_modern_lang.c` | Module language-table lookup |
| `lang/` | Interface text and option descriptions for each language |
| `djui_modern_gbi.h` | Private display-list macros |
| `*_data.h` | Generated font, Emoji, and flag data |
| `font/` | Embedded fonts and license information |
| `patches/` | Generated upstream patches |
| `tools/` | Patch and glyph-asset generation utilities |
| `install.sh` | Desktop and Android installer |

## Lifecycle

1. `djui_init()` calls `djui_modern_init()` before the first text node is
   created.
2. Initialization loads `djui_modern.cfg`, prepares the rounded-rectangle
   backend, and replaces configured `gDjuiFonts` slots.
3. `djui_render()` calls `djui_modern_pre_render()` to reset frame state and
   vertex pools, then apply the active theme.
4. `djui_rect_render()` calls `djui_modern_rect_override()`. A true result
   means the module emitted the rectangle; false continues with the original
   renderer.
5. Font callbacks enter the glyph backend through `djui_modern_font.c`.
6. `djui_render()` calls `djui_modern_post_render()` after drawing for optional
   display-list diagnostics.
7. `djui_shutdown()` calls `djui_modern_shutdown()` to restore font pointers
   and release resources.

## Control categories

Upstream constructors write `DjuiBase.type` when they create composite controls.
Buttons, checkboxes, sliders, input boxes, selection boxes, progress bars, and
panels have separate categories. Surfaces, tracks, and fills inside composite
controls also carry their own categories. The module selects radius, border, and
fill behavior from this field without inferring a control type from dimensions,
colors, or child-node structure.

| Category group | Examples | Module behavior |
| --- | --- | --- |
| Panels | `PANEL` | Panel radius and shadow |
| Interactive controls | `BUTTON_SURFACE`, `INPUTBOX`, `SELECTIONBOX_BOX` | Control radius and theme color |
| Sliders | `SLIDER_FILL` | The containing track draws the inset fill and border once |
| Checkboxes | `CHECKBOX_FRAME`, `CHECKBOX_VALUE` | Original renderer |
| Generic rectangles | `RECT`, `GENERIC` | Regular control rendering |

## Upstream interfaces

| Interface | Consumer | Implementation |
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

Each upstream hook has one implementation. In builds without the module, empty
weak definitions in the patched upstream `.c` files preserve original behavior.
No hook declarations are added to upstream headers.

## Module interfaces

| Group | Primary functions | Responsibility |
| --- | --- | --- |
| Lifecycle | `djui_modern_init` / `pre_render` / `post_render` / `shutdown` | Module state and DJUI integration |
| Font adapter | `djui_modern_font_*` | Install, query, and restore font slots |
| Font backend | `djui_modern_font_atlas_*` | Rasterization, metrics, textures, glyph output |
| Rounded rectangles | `djui_modern_rrect_*` | Geometry, mask, borders, display lists |
| Configuration | `djui_modern_config_load` / `djui_modern_config_read` / `djui_modern_config_save` | Startup load, panel read, and save |
| Settings panel | `djui_modern_settings_create` | Create controls and write the configuration file |
| Localization | `djui_modern_lang_get` | Look up module text in the active game language |

Only the font adapter may mutate `gDjuiFonts`. The main module does not access
glyph textures or font tables directly.

## Data flow

```text
DjuiBase
  -> djui_modern_rect_override
  -> rounded-rectangle display list

DjuiFont callback
  -> djui_modern_font
  -> djui_modern_font_atlas
  -> UTF-8 codepoint
  -> font chain and raster bucket
  -> immutable glyph texture

djui_modern.cfg
  -> djui_modern_config
  -> renderer, font, and rectangle modules

Options menu
  -> djui_modern_settings
  -> read djui_modern.cfg and the language table
  -> request a restart after saving
```

`pre_render` reads base colors from the active `DjuiTheme`. Rectangle color and
opacity still come from each `DjuiBase`, so original fade and interaction
states remain authoritative.

## Upstream patch boundary

The current patch set modifies these upstream files:

| File | Purpose |
| --- | --- |
| `Makefile` | Add `src/pc/djui_modern` to `SRC_DIRS` |
| `src/pc/djui/djui.c` | Lifecycle calls |
| `src/pc/djui/djui_rect.c` | Rectangle override hook and rectangle category |
| `src/pc/djui/djui_gfx.c` | Font-size and text-clipping hooks |
| `src/pc/djui/djui_hud_utils.c` | N64-space font-size reporting |
| `src/pc/djui/djui_unicode.c` | Unicode acceptance hook |
| `src/pc/djui/djui_text.c` | Text-node creation hook |
| `src/pc/djui/djui_base.h` | `DjuiBaseType` enum and `DjuiBase.type` field |
| `src/pc/djui/djui_base.c` | Default category for new nodes |
| `src/pc/djui/djui_button.c` and related constructors | Composite and child-rectangle categories |
| `src/pc/djui/djui_panel_menu_options.c` | Settings-panel entry |
| `src/pc/gfx/gfx_dxgi.cpp` | Windows UTF-16 surrogate-pair input |
| `src/pc/gfx/gfx_pc.c` | Large RGBA32 texture upload paths |

Desktop and Android get separate `.android.patch` variants only when their
upstream files differ. Patches are generated by `tools/make_patches.py` and
should not be edited by hand.

## Maintenance constraints

- Keep behavior inside the module unless an upstream change is unavoidable.
- Keep one hook per upstream function; do not add parallel adapters for the
  same operation.
- Declare hooks in the calling `.c` file to avoid invalidating the full DJUI
  include graph.
- Apply `weak` only to empty definitions in upstream files, never declarations.
- Every composite control constructor must write `DjuiBase.type`; the module
  must not infer categories from geometry.
- Expose runtime settings through `djui_modern.cfg`.
- Regenerate patches after changing an upstream hook and validate desktop and
  Android separately.
