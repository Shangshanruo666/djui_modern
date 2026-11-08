#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc/configfile.h"
#include "djui_modern_lang.h"
#include "djui_modern_lang_data.h"

#define DJUI_MODERN_LANG_FALLBACK "English"

static const struct DjuiModernLangTable* language_table(const char* name) {
    for (u32 i = 0; i < sizeof(sDjuiModernLangTables) / sizeof(sDjuiModernLangTables[0]); i++) {
        if (!strcmp(sDjuiModernLangTables[i].name, name)) {
            return &sDjuiModernLangTables[i];
        }
    }
    return NULL;
}

static const char* table_get(const struct DjuiModernLangTable* table, const char* key) {
    if (table == NULL) { return NULL; }
    for (u32 i = 0; i < table->count; i++) {
        if (!strcmp(table->entries[i].key, key)) {
            return table->entries[i].value;
        }
    }
    return NULL;
}

char* djui_modern_lang_get(const char* key) {
    const struct DjuiModernLangTable* selected = language_table(configLanguage);
    const struct DjuiModernLangTable* fallback = language_table(DJUI_MODERN_LANG_FALLBACK);
    const char* value = table_get(selected, key);
    if (value == NULL) { value = table_get(fallback, key); }
    if (value == NULL) { value = key; }
    return strdup(value);
}
