#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "pc/djui/djui.h"
#include "pc/djui/djui_panel.h"
#include "pc/djui/djui_panel_menu.h"
#include "pc/djui/djui_paginated.h"
#include "pc/debuglog.h"
#include "djui_modern.h"
#include "djui_modern_config.h"
#include "djui_modern_lang.h"
#include "djui_modern_settings.h"

static struct DjuiModernConfig sDraft;
static unsigned int sSliderValues[5];
static unsigned int sPanelAlpha;
static unsigned int sElementAlpha;
static unsigned int sOverlayAlpha;
static unsigned int sFontCjkBoost;
static unsigned int sTextScale;
static unsigned int sEmojiChoice;
static char sFontPath[DJUI_MODERN_FONT_PATH_MAX];
static bool sFontSlots[DJUI_MODERN_FONT_SLOT_COUNT];
static char sStatusMessage[256];
static struct DjuiText* sStatusText = NULL;
static struct DjuiText* sDetailText = NULL;
static struct DjuiText* sDetailTitle = NULL;
static struct DjuiThreePanel* sDetailPanel = NULL;
static struct DjuiInputbox* sFontPathInput = NULL;
static struct DjuiBase* sRowBase[24];
static struct DjuiBase* sFontSlotBase[DJUI_MODERN_FONT_SLOT_COUNT];

enum SliderField {
    SLIDER_RADIUS_PANEL,
    SLIDER_RADIUS_ELEMENT,
    SLIDER_CORNER_ROUNDNESS,
    SLIDER_GRADIENT_TOP,
    SLIDER_GRADIENT_BOTTOM,
    SLIDER_COUNT,
};

#define SLIDER_HUNDREDTH 100.0f
#define SETTINGS_PANEL_WIDTH (410.0f + 32.0f)

static const char* const kFontSlotLabels[DJUI_MODERN_FONT_SLOT_COUNT] = {
    "Normal", "Title", "HUD", "Aliased", "CustomHUD", "RecolorHUD", "Special",
};

static const bool kFontSlotDefaults[DJUI_MODERN_FONT_SLOT_COUNT] = {
    true, false, false, true, false, false, false,
};

enum SettingKind {
    SETTING_BOOL,
    SETTING_FLOAT,
    SETTING_UINT,
    SETTING_EMOJI,
    SETTING_STRING,
    SETTING_SLOT,
};

struct SettingRow {
    const char* key;
    const char* labelKey;
    enum SettingKind kind;
    f32 min;
    f32 max;
    bool unitHundredth;
};

static const struct SettingRow kRows[] = {
    { "enabled", "ENABLED", SETTING_BOOL, 0, 0, 0 },
    { "radius_panel", "RADIUS_PANEL", SETTING_FLOAT, 0, 640, 0 },
    { "radius_element", "RADIUS_ELEMENT", SETTING_FLOAT, 0, 640, 0 },
    { "corner_roundness", "CORNER_ROUNDNESS", SETTING_FLOAT, 0, 1, 1 },
    { "panel_alpha", "PANEL_ALPHA", SETTING_UINT, 0, 255, 0 },
    { "element_alpha", "ELEMENT_ALPHA", SETTING_UINT, 0, 255, 0 },
    { "overlay_alpha", "OVERLAY_ALPHA", SETTING_UINT, 0, 255, 0 },
    { "gradients", "GRADIENTS", SETTING_BOOL, 0, 0, 0 },
    { "gradient_top_boost", "GRADIENT_TOP", SETTING_FLOAT, 0.5f, 2.0f, 1 },
    { "gradient_bottom_scale", "GRADIENT_BOTTOM", SETTING_FLOAT, 0.5f, 2.0f, 1 },
    { "show_borders", "SHOW_BORDERS", SETTING_BOOL, 0, 0, 0 },
    { "font_enabled", "FONT_ENABLED", SETTING_BOOL, 0, 0, 0 },
    { "font_path", "FONT_PATH", SETTING_STRING, 0, 0, 0 },
    { "font_cjk_raster_boost", "FONT_CJK_BOOST", SETTING_FLOAT, 0.5f, 2.0f, 1 },
    { "font_bilinear", "FONT_BILINEAR", SETTING_BOOL, 0, 0, 0 },
    { "font_native_metrics", "FONT_NATIVE_METRICS", SETTING_BOOL, 0, 0, 0 },
    { "font_fallback_kerning", "FONT_FALLBACK_KERNING", SETTING_BOOL, 0, 0, 0 },
    { "font_slots", "FONT_SLOTS", SETTING_SLOT, 0, 0, 0 },
    { "emoji", "EMOJI", SETTING_EMOJI, 0, 1, 0 },
    { "text_scale", "TEXT_SCALE", SETTING_FLOAT, 0.5f, 2.0f, 1 },
    { "font_player_list", "FONT_PLAYER_LIST", SETTING_BOOL, 0, 0, 0 },
    { "font_mod_description", "FONT_MOD_DESCRIPTION", SETTING_BOOL, 0, 0, 0 },
    { "show_stats", "SHOW_STATS", SETTING_BOOL, 0, 0, 0 },
};

static const size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

static void lang_copy(char* out, size_t capacity, const char* key) {
    char* value = djui_modern_lang_get(key);
    if (value != NULL) {
        snprintf(out, capacity, "%s", value);
        free(value);
    } else {
        snprintf(out, capacity, "%s", key);
    }
}

static void lang_default_copy(char* out, size_t capacity, const char* key) {
    char langKey[96];
    snprintf(langKey, sizeof(langKey), "%s_DEFAULT", key);
    lang_copy(out, capacity, langKey);
}

static void lang_description_copy(char* out, size_t capacity, const char* key) {
    char langKey[96];
    snprintf(langKey, sizeof(langKey), "%s_DESC", key);
    lang_copy(out, capacity, langKey);
}

static void setting_detail_show(const struct SettingRow* row);

static void setting_detail_hover(struct DjuiBase* base) {
    if (base == NULL) { return; }
    const size_t index = (size_t)base->tag;
    if (index < kRowCount) { setting_detail_show(&kRows[index]); }
}

static void setting_detail_hide(UNUSED struct DjuiBase* base) {
    if (sDetailTitle != NULL) { djui_text_set_text(sDetailTitle, ""); }
    if (sDetailText != NULL) {
        djui_text_set_text(sDetailText, "");
    }
}

static void setting_detail_show(const struct SettingRow* row) {
    if (sDetailText == NULL) { return; }

    static char text[1024];
    char description[768];
    char defaultValue[128];
    char unitValue[128];
    lang_description_copy(description, sizeof(description), row->labelKey);
    lang_default_copy(defaultValue, sizeof(defaultValue), row->labelKey);
    djui_text_set_text(sDetailTitle, row->key);
    if (row->unitHundredth) {
        lang_copy(unitValue, sizeof(unitValue), "UNIT_HUNDREDTH");
        snprintf(text, sizeof(text), "%s\n\n%s\n%s", description, defaultValue, unitValue);
    } else {
        snprintf(text, sizeof(text), "%s\n\n%s", description, defaultValue);
    }
    djui_text_set_text(sDetailText, text);
}

static void settings_panel_destroy(UNUSED struct DjuiBase* base) {
    if (sDetailPanel != NULL) {
        djui_base_destroy(&sDetailPanel->base);
        sDetailPanel = NULL;
    }
    sDetailText = NULL;
    sDetailTitle = NULL;
    sStatusText = NULL;
    sFontPathInput = NULL;
    memset(sRowBase, 0, sizeof(sRowBase));
    memset(sFontSlotBase, 0, sizeof(sFontSlotBase));
}

static void settings_detail_panel_create(void) {
    struct DjuiThreePanel* panel = djui_three_panel_create(&gDjuiRoot->base, 64, 800, 0);
    sDetailPanel = panel;

    const struct DjuiModernPalette* palette = djui_modern_palette();
    const struct DjuiModernUiColors* colors = djui_modern_ui_colors();
    const struct DjuiColor panelColor = palette->panel;
    const struct DjuiColor panelBorder = palette->panelBorder;
    djui_base_set_alignment(&panel->base, DJUI_HALIGN_RIGHT, DJUI_VALIGN_CENTER);
    djui_base_set_size_type(&panel->base, DJUI_SVT_ABSOLUTE, DJUI_SVT_RELATIVE);
    djui_base_set_size(&panel->base, SETTINGS_PANEL_WIDTH, 1.0f);
    djui_base_set_color(&panel->base, panelColor.r, panelColor.g,
                        panelColor.b, panelColor.a);
    djui_base_set_border_color(&panel->base, panelBorder.r, panelBorder.g,
                               panelBorder.b, panelBorder.a);
    djui_base_set_border_width(&panel->base, 8);
    djui_base_set_padding(&panel->base, 16, 16, 16, 16);

    sDetailTitle = djui_text_create(&panel->base, "");
    djui_base_set_size_type(&sDetailTitle->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
    djui_base_set_size(&sDetailTitle->base, 1.0f, 40);
    const struct DjuiColor titleColor = colors->detailTitle;
    djui_base_set_color(&sDetailTitle->base, titleColor.r, titleColor.g,
                        titleColor.b, titleColor.a);
    djui_text_set_alignment(sDetailTitle, DJUI_HALIGN_LEFT, DJUI_VALIGN_CENTER);

    sDetailText = djui_text_create(&panel->base, "");
    djui_base_set_location_type(&sDetailText->base, DJUI_SVT_ABSOLUTE, DJUI_SVT_ABSOLUTE);
    djui_base_set_location(&sDetailText->base, 0, 48);
    djui_base_set_size_type(&sDetailText->base, DJUI_SVT_RELATIVE, DJUI_SVT_RELATIVE);
    djui_base_set_size(&sDetailText->base, 1.0f, 0.86f);
    const struct DjuiColor textColor = colors->detailText;
    djui_base_set_color(&sDetailText->base, textColor.r, textColor.g,
                        textColor.b, textColor.a);
    djui_text_set_alignment(sDetailText, DJUI_HALIGN_LEFT, DJUI_VALIGN_TOP);
    const struct DjuiColor shadow = colors->textShadow;
    djui_text_set_drop_shadow(sDetailText, shadow.r, shadow.g, shadow.b, shadow.a);
}

static void settings_checkbox_value_changed(struct DjuiBase* caller) {
    caller->interactable->update_style(caller);
}

static void settings_slider_foreground_pre(struct DjuiBase* base, bool* skipRender) {
    if (skipRender != NULL) { *skipRender = false; }
    if (base == NULL || base->parent == NULL) { return; }

    // 宽度按填充比例的倒数展开，使文字基准覆盖整条轨道；填充父节点负责裁剪。
    const f32 fillFraction = base->parent->width.value;
    const f32 widthScale = (fillFraction > 0.001f) ? (1.0f / fillFraction) : 1.0f;
    djui_base_set_location_type(base, DJUI_SVT_ABSOLUTE, DJUI_SVT_ABSOLUTE);
    djui_base_set_location(base, 0, 0);
    djui_base_set_size_type(base, DJUI_SVT_RELATIVE, DJUI_SVT_RELATIVE);
    djui_base_set_size(base, widthScale, 1.0f);
    djui_text_set_alignment((struct DjuiText*)base, DJUI_HALIGN_CENTER, DJUI_VALIGN_CENTER);
}

static void settings_row_create(struct DjuiBase* list, size_t index) {
    const struct SettingRow* row = &kRows[index];
    struct DjuiBase* base = NULL;
    char label[128];
    lang_copy(label, sizeof(label), row->labelKey);

    switch (index) {
        case 0:
            base = &djui_checkbox_create(list, label, &sDraft.enabled,
                                         settings_checkbox_value_changed)->base;
            break;
        case 1:
            base = &djui_slider_create(list, label, &sSliderValues[SLIDER_RADIUS_PANEL],
                                       0, 640, NULL)->base;
            break;
        case 2:
            base = &djui_slider_create(list, label, &sSliderValues[SLIDER_RADIUS_ELEMENT],
                                       0, 640, NULL)->base;
            break;
        case 3:
            base = &djui_slider_create(list, label, &sSliderValues[SLIDER_CORNER_ROUNDNESS],
                                       0, 100, NULL)->base;
            break;
        case 4:
            base = &djui_slider_create(list, label, &sPanelAlpha, 0, 255,
                                       NULL)->base;
            break;
        case 5:
            base = &djui_slider_create(list, label, &sElementAlpha, 0, 255,
                                       NULL)->base;
            break;
        case 6:
            base = &djui_slider_create(list, label, &sOverlayAlpha, 0, 255,
                                       NULL)->base;
            break;
        case 7:
            base = &djui_checkbox_create(list, label, &sDraft.gradients,
                                         settings_checkbox_value_changed)->base;
            break;
        case 8:
            base = &djui_slider_create(list, label, &sSliderValues[SLIDER_GRADIENT_TOP],
                                       50, 200, NULL)->base;
            break;
        case 9:
            base = &djui_slider_create(list, label, &sSliderValues[SLIDER_GRADIENT_BOTTOM],
                                       50, 200, NULL)->base;
            break;
        case 10:
            base = &djui_checkbox_create(list, label, &sDraft.showBorders,
                                         settings_checkbox_value_changed)->base;
            break;
        case 11:
            base = &djui_checkbox_create(list, label, &sDraft.fontEnabled,
                                         settings_checkbox_value_changed)->base;
            break;
        case 12: {
            struct DjuiText* text = djui_text_create(list, label);
            djui_base_set_size_type(&text->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
            djui_base_set_size(&text->base, 1.0f, 32);
            const struct DjuiColor textColor = djui_modern_ui_colors()->detailText;
            djui_base_set_color(&text->base, textColor.r, textColor.g,
                                textColor.b, textColor.a);

            struct DjuiInputbox* input = djui_inputbox_create(list, DJUI_MODERN_FONT_PATH_MAX);
            djui_inputbox_set_text(input, sFontPath);
            sFontPathInput = input;
            base = &input->base;
            break;
        }
        case 13:
            base = &djui_slider_create(list, label, &sFontCjkBoost, 50, 200,
                                       NULL)->base;
            break;
        case 14:
            base = &djui_checkbox_create(list, label, &sDraft.fontBilinear,
                                         settings_checkbox_value_changed)->base;
            break;
        case 15:
            base = &djui_checkbox_create(list, label, &sDraft.fontNativeMetrics,
                                         settings_checkbox_value_changed)->base;
            break;
        case 16:
            base = &djui_checkbox_create(list, label, &sDraft.fontFallbackKerning,
                                         settings_checkbox_value_changed)->base;
            break;
        case 17:
            for (u8 slot = 0; slot < DJUI_MODERN_FONT_SLOT_COUNT; slot++) {
                struct DjuiCheckbox* checkbox = djui_checkbox_create(
                    list, kFontSlotLabels[slot], &sFontSlots[slot],
                    settings_checkbox_value_changed);
                checkbox->base.tag = (s64)index;
                sFontSlotBase[slot] = &checkbox->base;
                djui_interactable_hook_hover(&checkbox->base, setting_detail_hover,
                                             setting_detail_hide);
            }
            return;
        case 18: {
            char* choices[2];
            choices[0] = djui_modern_lang_get("EMOJI_MODERN");
            choices[1] = djui_modern_lang_get("EMOJI_VANILLA");
            base = &djui_selectionbox_create(list, label, choices, 2,
                                             &sEmojiChoice, NULL)->base;
            free(choices[0]);
            free(choices[1]);
            break;
        }
        case 19:
            base = &djui_slider_create(list, label, &sTextScale, 50, 200,
                                       NULL)->base;
            break;
        case 20:
            base = &djui_checkbox_create(list, label, &sDraft.fontPlayerList,
                                         settings_checkbox_value_changed)->base;
            break;
        case 21:
            base = &djui_checkbox_create(list, label, &sDraft.fontModDescription,
                                         settings_checkbox_value_changed)->base;
            break;
        case 22:
            base = &djui_checkbox_create(list, label, &sDraft.showStats,
                                         settings_checkbox_value_changed)->base;
            break;
    }

    if (base != NULL) {
        base->tag = (s64)index;
        djui_interactable_hook_hover(base, setting_detail_hover, setting_detail_hide);
        if ((row->kind == SETTING_FLOAT || row->kind == SETTING_UINT)
            && gDjuiModernConfig.fontEnabled) {
            struct DjuiSlider* slider = (struct DjuiSlider*)base;
            if (slider->textValueForeground != NULL) {
                slider->textValueForeground->base.on_render_pre = settings_slider_foreground_pre;
            }
        }
        sRowBase[index] = base;
    }
}

static void settings_refresh_controls(void) {
    for (size_t i = 0; i < kRowCount; i++) {
        struct DjuiBase* base = sRowBase[i];
        if (base == NULL) { continue; }
        if (kRows[i].kind == SETTING_FLOAT || kRows[i].kind == SETTING_UINT) {
            djui_slider_update_value(base);
        } else if (kRows[i].kind == SETTING_BOOL) {
            base->interactable->update_style(base);
        } else if (kRows[i].kind == SETTING_EMOJI) {
            djui_selectionbox_update_value(base);
        }
    }
    for (u8 slot = 0; slot < DJUI_MODERN_FONT_SLOT_COUNT; slot++) {
        struct DjuiBase* base = sFontSlotBase[slot];
        if (base != NULL) { base->interactable->update_style(base); }
    }
}

static void draft_to_controls(void) {
    sSliderValues[SLIDER_RADIUS_PANEL] = (unsigned int)(sDraft.radiusPanel + 0.5f);
    sSliderValues[SLIDER_RADIUS_ELEMENT] = (unsigned int)(sDraft.radiusElement + 0.5f);
    sSliderValues[SLIDER_CORNER_ROUNDNESS] = (unsigned int)(sDraft.cornerRoundness * SLIDER_HUNDREDTH);
    sSliderValues[SLIDER_GRADIENT_TOP] = (unsigned int)(sDraft.gradientTopBoost * SLIDER_HUNDREDTH);
    sSliderValues[SLIDER_GRADIENT_BOTTOM] = (unsigned int)(sDraft.gradientBotScale * SLIDER_HUNDREDTH);
    sFontCjkBoost = (unsigned int)(sDraft.fontRasterBoostCJK * SLIDER_HUNDREDTH);
    sTextScale = (unsigned int)(sDraft.textScale * SLIDER_HUNDREDTH);
    sPanelAlpha = sDraft.panelAlpha;
    sElementAlpha = sDraft.elementAlpha;
    sOverlayAlpha = sDraft.overlayAlpha;
    sEmojiChoice = sDraft.emojiModern ? 0 : 1;
    snprintf(sFontPath, sizeof(sFontPath), "%s", sDraft.fontPath);
    memset(sFontSlots, 0, sizeof(sFontSlots));
    for (u8 i = 0; i < sDraft.fontSlotCount; i++) {
        if (sDraft.fontSlots[i] < DJUI_MODERN_FONT_SLOT_COUNT) {
            sFontSlots[sDraft.fontSlots[i]] = true;
        }
    }
}

static void draft_from_controls(void) {
    sDraft.radiusPanel = (f32)sSliderValues[SLIDER_RADIUS_PANEL];
    sDraft.radiusElement = (f32)sSliderValues[SLIDER_RADIUS_ELEMENT];
    sDraft.cornerRoundness = (f32)sSliderValues[SLIDER_CORNER_ROUNDNESS] / SLIDER_HUNDREDTH;
    sDraft.gradientTopBoost = (f32)sSliderValues[SLIDER_GRADIENT_TOP] / SLIDER_HUNDREDTH;
    sDraft.gradientBotScale = (f32)sSliderValues[SLIDER_GRADIENT_BOTTOM] / SLIDER_HUNDREDTH;
    sDraft.fontRasterBoostCJK = (f32)sFontCjkBoost / SLIDER_HUNDREDTH;
    sDraft.textScale = (f32)sTextScale / SLIDER_HUNDREDTH;
    sDraft.panelAlpha = (u8)sPanelAlpha;
    sDraft.elementAlpha = (u8)sElementAlpha;
    sDraft.overlayAlpha = (u8)sOverlayAlpha;
    sDraft.emojiModern = (sEmojiChoice == 0);
    snprintf(sDraft.fontPath, sizeof(sDraft.fontPath), "%s", sFontPath);
    sDraft.fontSlotCount = 0;
    for (u8 i = 0; i < DJUI_MODERN_FONT_SLOT_COUNT; i++) {
        if (sFontSlots[i]) { sDraft.fontSlots[sDraft.fontSlotCount++] = i; }
    }
}

static void draft_reset_slots(void) {
    memcpy(sFontSlots, kFontSlotDefaults, sizeof(sFontSlots));
}

static void draft_load(void) {
    const char* error = NULL;
    if (!djui_modern_config_read(&sDraft, &error)) {
        memcpy(&sDraft, &gDjuiModernConfig, sizeof(sDraft));
        lang_copy(sStatusMessage, sizeof(sStatusMessage),
                  "STATUS_READ_FAILED");
    } else {
        sStatusMessage[0] = '\0';
    }
    draft_to_controls();
}

static void settings_set_status(const char* message, bool isError) {
    if (sStatusText == NULL) { return; }
    djui_text_set_text(sStatusText, message);
    const struct DjuiModernUiColors* colors = djui_modern_ui_colors();
    const struct DjuiColor color = isError ? colors->statusError : colors->statusNotice;
    djui_base_set_color(&sStatusText->base, color.r, color.g, color.b, color.a);
}

static bool draft_differs_from_runtime(void) {
    return memcmp(&sDraft, &gDjuiModernConfig, sizeof(sDraft)) != 0;
}

static void settings_show_restart_notice(void) {
    char message[256];
    lang_copy(message, sizeof(message), "STATUS_RESTART");
    settings_set_status(message, false);
}

static void settings_save(struct DjuiBase* caller) {
    (void)caller;
    if (sFontPathInput != NULL) {
        snprintf(sFontPath, sizeof(sFontPath), "%s", sFontPathInput->buffer);
    }
    draft_from_controls();
    const char* error = NULL;
    if (!djui_modern_config_save(&sDraft, &error)) {
        lang_copy(sStatusMessage, sizeof(sStatusMessage), "STATUS_SAVE_FAILED");
        LOG_ERROR("djui_modern: settings save failed: %s", error != NULL ? error : "unknown");
        settings_set_status(sStatusMessage, true);
        return;
    }
    settings_show_restart_notice();
}

static void settings_restore_defaults(struct DjuiBase* caller) {
    (void)caller;
    djui_modern_config_set_defaults(&sDraft);
    draft_reset_slots();

    const char* error = NULL;
    if (!djui_modern_config_save(&sDraft, &error)) {
        lang_copy(sStatusMessage, sizeof(sStatusMessage), "STATUS_RESET_FAILED");
        settings_set_status(sStatusMessage, true);
        return;
    }
    draft_to_controls();
    djui_inputbox_set_text(sFontPathInput, sFontPath);
    settings_refresh_controls();
    settings_show_restart_notice();
}

void djui_modern_settings_button_create(struct DjuiBase* parent) {
    char label[64];
    lang_copy(label, sizeof(label), "MENU_ENTRY");
    djui_button_create(parent, label, DJUI_BUTTON_STYLE_NORMAL, djui_modern_settings_create);
}

void djui_modern_settings_create(struct DjuiBase* caller) {
    settings_detail_panel_create();

    char title[64];
    lang_copy(title, sizeof(title), "TITLE");
    struct DjuiThreePanel* panel = djui_panel_menu_create(title, true);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
    draft_load();

    struct DjuiPaginated* paginated = djui_paginated_create(body, 6);
    paginated->showMaxCount = true;
    struct DjuiBase* list = &paginated->layout->base;

    for (size_t i = 0; i < kRowCount; i++) {
        settings_row_create(list, i);
    }

    djui_paginated_calculate_height(paginated);

    char restoreLabel[128];
    char saveLabel[64];
    char backLabel[64];
    lang_copy(restoreLabel, sizeof(restoreLabel), "RESTORE_DEFAULTS");
    lang_copy(saveLabel, sizeof(saveLabel), "SAVE");
    lang_copy(backLabel, sizeof(backLabel), "BACK");
    sStatusText = djui_text_create(body, "");
    djui_text_set_alignment(sStatusText, DJUI_HALIGN_CENTER, DJUI_VALIGN_TOP);
    djui_base_set_size_type(&sStatusText->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
    djui_base_set_size(&sStatusText->base, 1.0f, 32);
    djui_button_create(body, restoreLabel, DJUI_BUTTON_STYLE_NORMAL, settings_restore_defaults);
    djui_button_create(body, saveLabel, DJUI_BUTTON_STYLE_NORMAL, settings_save);
    djui_button_create(body, backLabel, DJUI_BUTTON_STYLE_BACK, djui_panel_menu_back);

    if (sStatusMessage[0] != '\0') {
        settings_set_status(sStatusMessage, true);
    } else if (draft_differs_from_runtime()) { settings_show_restart_notice(); }

    struct DjuiPanel* active = djui_panel_add(caller, panel, NULL);
    if (active != NULL) {
        active->on_panel_destroy = settings_panel_destroy;
    }
}

