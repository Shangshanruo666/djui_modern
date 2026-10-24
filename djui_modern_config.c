#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pc/djui/djui.h"
#include "djui_modern_config.h"
#include "pc/debuglog.h"
#include "pc/fs/fs.h"
#include "engine/math_util.h"

struct DjuiModernConfig gDjuiModernConfig;

static bool sConfigLoaded = false;

static void djui_modern_config_defaults(void) {
    gDjuiModernConfig.enabled      = true;

    // 半径单位：320x240 DJUI 坐标。
    gDjuiModernConfig.radiusPanel   = 12.0f;
    gDjuiModernConfig.radiusElement = 8.0f;
    gDjuiModernConfig.radiusPill    = -1.0f;  // < 0：使用高度的一半

    // 0：羊角螺线过渡；1：正圆弧。
    gDjuiModernConfig.cornerRoundness = 0.90f;

    gDjuiModernConfig.panelAlpha   = 244;
    gDjuiModernConfig.elementAlpha = 208;
    gDjuiModernConfig.overlayAlpha = 176;

    gDjuiModernConfig.gradients      = true;
    gDjuiModernConfig.gradientTopBoost = 1.18f;
    gDjuiModernConfig.gradientBotScale = 0.86f;

    gDjuiModernConfig.showBorders  = true;

    gDjuiModernConfig.fontEnabled  = true;
    gDjuiModernConfig.fontPath[0]  = '\0';

    // CJK 字形光栅尺寸倍率。
    gDjuiModernConfig.fontRasterBoostCJK = 1.0f;
    gDjuiModernConfig.fontBilinear = true;

    // true：使用字体原生字距；false：对齐原版点阵字体宽度。
    gDjuiModernConfig.fontNativeMetrics = true;

    // 字体缺少字偶间距数据时使用内置兜底。
    gDjuiModernConfig.fontFallbackKerning = true;

    // true：内嵌彩色 Emoji 图集；false：原版 Emoji。
    gDjuiModernConfig.emojiModern = true;

    // 字形光栅尺寸倍率；不改变排版尺寸。
    gDjuiModernConfig.textScale = 1.0f;

    // 指定面板是否使用矢量字体。
    gDjuiModernConfig.fontPlayerList     = false;
    gDjuiModernConfig.fontModDescription = false;

    // 默认替换槽 0（Normal）与槽 3（Aliased）。
    {
        static const u8 kDefaultFontSlots[] = { 0, 3 };
        memcpy(gDjuiModernConfig.fontSlots, kDefaultFontSlots, sizeof(kDefaultFontSlots));
        gDjuiModernConfig.fontSlotCount = (u8)(sizeof(kDefaultFontSlots) / sizeof(kDefaultFontSlots[0]));
    }
    gDjuiModernConfig.showStats = false;
}

static char* trim(char* s) {
    if (!s) { return s; }
    while (*s && isspace((unsigned char)*s)) { s++; }
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) { end--; }
    *end = '\0';
    return s;
}

static bool parse_bool(const char* v, bool fallback) {
    if (!v) { return fallback; }
    if (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on")) { return true; }
    if (!strcasecmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off")) { return false; }
    return fallback;
}

/* 解析空格或逗号分隔的字体槽位列表，范围为 0-7。 */
static u8 parse_slot_list(const char* value, u8* out, u8 maxCount) {
    u8 count = 0;
    const char* p = value;
    while (*p != '\0' && count < maxCount) {
        while (*p != '\0' && (*p == ' ' || *p == ',' || *p == '\t')) { p++; }
        if (*p < '0' || *p > '9') { break; }
        int slot = 0;
        while (*p >= '0' && *p <= '9') { slot = slot * 10 + (*p - '0'); p++; }
        if (slot >= 0 && slot < 8) { out[count++] = (u8)slot; }
    }
    return count;
}

static void apply_kv(const char* key, const char* value) {
    if (!strcasecmp(key, "enabled")) {
        gDjuiModernConfig.enabled = parse_bool(value, gDjuiModernConfig.enabled);
    } else if (!strcasecmp(key, "radius_panel")) {
        gDjuiModernConfig.radiusPanel = (f32)atof(value);
    } else if (!strcasecmp(key, "radius_element")) {
        gDjuiModernConfig.radiusElement = (f32)atof(value);
    } else if (!strcasecmp(key, "radius_pill")) {
        gDjuiModernConfig.radiusPill = (f32)atof(value);
    } else if (!strcasecmp(key, "corner_roundness")) {
        gDjuiModernConfig.cornerRoundness = (f32)atof(value);
        if (gDjuiModernConfig.cornerRoundness < 0.0f) { gDjuiModernConfig.cornerRoundness = 0.0f; }
        if (gDjuiModernConfig.cornerRoundness > 1.0f) { gDjuiModernConfig.cornerRoundness = 1.0f; }
    } else if (!strcasecmp(key, "panel_alpha")) {
        gDjuiModernConfig.panelAlpha = (u8)clamp((s32)atoi(value), 0, 255);
    } else if (!strcasecmp(key, "element_alpha")) {
        gDjuiModernConfig.elementAlpha = (u8)clamp((s32)atoi(value), 0, 255);
    } else if (!strcasecmp(key, "overlay_alpha")) {
        gDjuiModernConfig.overlayAlpha = (u8)clamp((s32)atoi(value), 0, 255);
    } else if (!strcasecmp(key, "gradients")) {
        gDjuiModernConfig.gradients = parse_bool(value, gDjuiModernConfig.gradients);
    } else if (!strcasecmp(key, "gradient_top_boost")) {
        gDjuiModernConfig.gradientTopBoost = (f32)atof(value);
    } else if (!strcasecmp(key, "gradient_bottom_scale")) {
        gDjuiModernConfig.gradientBotScale = (f32)atof(value);
    } else if (!strcasecmp(key, "show_borders")) {
        gDjuiModernConfig.showBorders = parse_bool(value, gDjuiModernConfig.showBorders);
    } else if (!strcasecmp(key, "font_enabled")) {
        gDjuiModernConfig.fontEnabled = parse_bool(value, gDjuiModernConfig.fontEnabled);
    } else if (!strcasecmp(key, "font_path")) {
        snprintf(gDjuiModernConfig.fontPath, sizeof(gDjuiModernConfig.fontPath), "%s", value);
    } else if (!strcasecmp(key, "font_cjk_raster_boost")) {
        gDjuiModernConfig.fontRasterBoostCJK = (f32)atof(value);
    } else if (!strcasecmp(key, "font_bilinear")) {
        gDjuiModernConfig.fontBilinear = parse_bool(value, gDjuiModernConfig.fontBilinear);
    } else if (!strcasecmp(key, "font_native_metrics")) {
        gDjuiModernConfig.fontNativeMetrics = parse_bool(value, gDjuiModernConfig.fontNativeMetrics);
    } else if (!strcasecmp(key, "emoji")) {
        // emoji = modern | vanilla（也接受 true/false 写法）
        if (!strcasecmp(value, "modern") || !strcasecmp(value, "new") ||
            !strcasecmp(value, "1") || !strcasecmp(value, "true")) {
            gDjuiModernConfig.emojiModern = true;
        } else if (!strcasecmp(value, "vanilla") || !strcasecmp(value, "original") ||
                   !strcasecmp(value, "0") || !strcasecmp(value, "false")) {
            gDjuiModernConfig.emojiModern = false;
        }
    } else if (!strcasecmp(key, "font_fallback_kerning")) {
        gDjuiModernConfig.fontFallbackKerning = parse_bool(value, gDjuiModernConfig.fontFallbackKerning);
    } else if (!strcasecmp(key, "font_slots")) {
        const u8 n = parse_slot_list(value, gDjuiModernConfig.fontSlots, 8);
        if (n > 0) { gDjuiModernConfig.fontSlotCount = n; }
    } else if (!strcasecmp(key, "text_scale")) {
        gDjuiModernConfig.textScale = (f32)atof(value);
        if (gDjuiModernConfig.textScale < 0.5f) { gDjuiModernConfig.textScale = 0.5f; }
        if (gDjuiModernConfig.textScale > 2.0f) { gDjuiModernConfig.textScale = 2.0f; }
    } else if (!strcasecmp(key, "font_player_list")) {
        gDjuiModernConfig.fontPlayerList = parse_bool(value, gDjuiModernConfig.fontPlayerList);
    } else if (!strcasecmp(key, "font_mod_description")) {
        gDjuiModernConfig.fontModDescription = parse_bool(value, gDjuiModernConfig.fontModDescription);
    } else if (!strcasecmp(key, "show_stats")) {
        gDjuiModernConfig.showStats = parse_bool(value, gDjuiModernConfig.showStats);
    }
}

/* 配置文件路径、默认生成与缺失项补全。 */

#define DJUI_MODERN_CFG_FILE "djui_modern.cfg"

/* 桌面：游戏根目录；Android：fs 写入目录。两者均使用真实文件路径访问。 */
static const char* config_path(void) {
    static char buf[SYS_MAX_PATH];
    static bool sTried = false;
    if (!sTried) {
        sTried = true;
        buf[0] = '\0';
        char raw[SYS_MAX_PATH];
        raw[0] = '\0';

#if defined(__ANDROID__)
        // fs_get_write_path("") 返回带尾部分隔符的目录。
        const char* dir = fs_get_write_path("");
        if (dir != NULL && dir[0] != '\0') {
            snprintf(raw, sizeof(raw), "%s%s", dir, DJUI_MODERN_CFG_FILE);
        }
#else
        const char* dir = sys_exe_path_dir();
        if (dir != NULL && dir[0] != '\0') {
            snprintf(raw, sizeof(raw), "%s/%s", dir, DJUI_MODERN_CFG_FILE);
        }
#endif

        if (raw[0] != '\0') {
            fs_convert_path(buf, sizeof(buf), raw);
        }
    }
    return (buf[0] != '\0') ? buf : NULL;
}

static const char kDefaultCfgText[] =
"# djui_modern：现代化 DJUI 皮肤层配置\n"
"#\n"
"# 位置：Windows / macOS / Linux = 游戏根目录（与 coopdx 可执行文件同级）；\n"
"#       Android = 与 sm64config.txt 同级（游戏数据目录下的 user/ 里）。\n"
"# 删掉本文件，下次启动会自动重新生成一份带完整注释的默认配置。\n"
"# 版本更新后本文件会自动补上新增的配置项（连同说明注释），不必手动维护。\n"
"#\n"
"# 全部是 key = value 纯文本；'#' 或 ';' 开头是注释。改完需要重启游戏。\n"
"\n"
"# ---------------- 总开关 ----------------\n"
"enabled = true\n"
"\n"
"# ---------------- 形状 ----------------\n"
"# 圆角半径，单位是 DJUI 的 320x240 虚拟坐标系像素。\n"
"radius_panel   = 12     # 大面板\n"
"radius_element = 8      # 普通控件\n"
"radius_pill    = -1     # 胶囊元素（按钮/滑条）；-1 表示\"高度的一半\"\n"
"\n"
"# 圆角形状，范围 0.0 ~ 1.0。\n"
"#   1.0 = 正圆弧\n"
"#   0.0 = 羊角螺线过渡\n"
"#   0.90 = 默认值\n"
"corner_roundness = 0.90\n"
"\n"
"# ---------------- 半透明 ----------------\n"
"panel_alpha   = 244     # 面板背景不透明度 0-255\n"
"element_alpha = 208     # 控件背景不透明度\n"
"overlay_alpha = 176     # 覆盖层（聊天栏底、弹窗遮罩）不透明度\n"
"\n"
"# ---------------- 渐变 ----------------\n"
"# 在当前颜色的基础上做上亮下暗的渐变。\n"
"gradients             = true\n"
"gradient_top_boost    = 1.18   # 顶部亮度倍数\n"
"gradient_bottom_scale = 0.86   # 底部亮度倍数\n"
"\n"
"# ---------------- 边框 ----------------\n"
"# true = 显示圆角边框；false = 不显示边框。\n"
"show_borders = true\n"
"\n"
"# ---------------- 字体 ----------------\n"
"font_enabled = true\n"
"\n"
"# 字体文件路径。留空 = 按下列目录顺序自动查找任意文件名的 .ttf / .otf：\n"
"#   djui_modern/*.ttf, djui_modern/*.otf     （游戏根目录下的 djui_modern 文件夹）\n"
"#   *.ttf, *.otf                             （游戏根目录）\n"
"# 有多个字体时按文件名升序选择；找不到时使用内嵌字体。\n"
"font_path =\n"
"\n"
"# CJK 字形光栅尺寸倍率。提高该值会增加清晰度和内存占用。\n"
"font_cjk_raster_boost = 1.0\n"
"\n"
"font_bilinear = true          # 字形纹理的双线性过滤\n"
"\n"
"# true = 使用字体原生字距；false = 对齐原版点阵字体宽度。\n"
"font_native_metrics = true\n"
"\n"
"# 字体缺少字偶间距数据时，使用内置西文兜底表。\n"
"font_fallback_kerning = true\n"
"\n"
"# 用 TTF 替换哪些字体槽：\n"
"#   0=Normal（DJUI 常规字体） 1=Title（DJUI 标题字体） 2=HUD 3=Aliased（DJUI 锐利字体）\n"
"#   4=CustomHUD 5=RecolorHUD 6=Special\n"
"#\n"
"# 默认替换槽 0（Normal）与槽 3（Aliased）。\n"
"font_slots = 0 3\n"
"\n"
"# Emoji 样式：\n"
"#   modern  = 内嵌彩色 Emoji 与国旗图集\n"
"#   vanilla = 原版 Emoji 贴图\n"
"emoji = modern\n"
"\n"
"# 全局字形光栅尺寸倍率 0.5 ~ 2.0，只影响清晰度和内存，不改变排版尺寸。\n"
"text_scale = 1.0\n"
"\n"
"# 玩家列表（PLAYERS 面板）是否用矢量字体。\n"
"#   true  = 矢量字体（默认）\n"
"#   false = 原版点阵字体\n"
"font_player_list = false\n"
"\n"
"# mod 列表 / mod 描述面板是否用矢量字体。语义同上。\n"
"font_mod_description = false\n"
"\n"
"# ---------------- 调试 ----------------\n"
"# 输出加载信息与每帧渲染统计。排查问题时启用。\n"
"show_stats = false\n";

/* 文件不存在时写一份带注释的默认配置（与 sm64config.txt 的行为一致）。 */
static void config_write_default(void) {
    const char* path = config_path();
    if (path == NULL) {
        LOG_ERROR("djui_modern: cannot resolve game directory for djui_modern.cfg");
        return;
    }
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        LOG_ERROR("djui_modern: could not write default config to %s", path);
        return;
    }
    fputs(kDefaultCfgText, f);
    fclose(f);
    LOG_INFO("djui_modern: wrote default config to %s", path);
}

// 自动补全缺失的配置项。只追加内容，不修改已有取值。

/* 模板行区间。 */
struct CfgLine { const char* start; const char* end; };

/* 取下一行 [*pp, ...)，返回 false 表示已到末尾。 */
static bool cfg_next_line(const char** pp, const char** pstart, const char** pend) {
    const char* p = *pp;
    if (*p == '\0') { return false; }
    const char* nl = strchr(p, '\n');
    const char* end = (nl != NULL) ? nl : (p + strlen(p));
    *pstart = p;
    *pend = end;
    *pp = (nl != NULL) ? (nl + 1) : end;
    return true;
}

/* 判断空白行。 */
static bool cfg_line_is_blank(const char* s, const char* e) {
    for (const char* q = s; q < e; q++) {
        if (!isspace((unsigned char)*q)) { return false; }
    }
    return true;
}

/* 提取 key = value 中的键名。 */
static size_t cfg_line_key(const char* s, const char* e, const char** keyOut) {
    const char* cut = e;
    for (const char* q = s; q < e; q++) {
        if (*q == '#' || *q == ';') { cut = q; break; }
    }
    const char* eq = NULL;
    for (const char* q = s; q < cut; q++) {
        if (*q == '=') { eq = q; break; }
    }
    if (eq == NULL) { return 0; }
    const char* k0 = s;
    while (k0 < eq && isspace((unsigned char)*k0)) { k0++; }
    const char* k1 = eq;
    while (k1 > k0 && isspace((unsigned char)k1[-1])) { k1--; }
    if (k1 == k0) { return 0; }
    *keyOut = k0;
    return (size_t)(k1 - k0);
}

/* 检查键名是否已出现。 */
static bool cfg_seen_has(const char* seen, const char* key, size_t keyLen) {
    for (const char* p = seen; *p != '\0'; ) {
        const char* nl = strchr(p, '\n');
        const char* end = (nl != NULL) ? nl : (p + strlen(p));
        if ((size_t)(end - p) == keyLen && strncasecmp(p, key, keyLen) == 0) { return true; }
        if (nl == NULL) { break; }
        p = nl + 1;
    }
    return false;
}

/* 检查文件是否以换行结尾。 */
static bool cfg_file_ends_with_newline(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) { return true; }
    bool ok = true;
    if (fseek(f, 0, SEEK_END) != 0) { ok = true; }
    else {
        const long sz = ftell(f);
        if (sz <= 0) { ok = true; }
        else if (fseek(f, sz - 1, SEEK_SET) != 0) { ok = true; }
        else { ok = (fgetc(f) == '\n'); }
    }
    fclose(f);
    return ok;
}

/* 将缺失键及其注释追加到配置文件。 */
static s32 config_append_missing(const char* path, const char* seenKeys) {
    if (path == NULL || seenKeys == NULL) { return 0; }

    enum { kMaxLines = 512 };
    static struct CfgLine lines[kMaxLines];
    s32 lineCount = 0;
    for (const char* p = kDefaultCfgText; *p != '\0' && lineCount < kMaxLines; ) {
        const char* ls; const char* le;
        if (!cfg_next_line(&p, &ls, &le)) { break; }
        lines[lineCount].start = ls;
        lines[lineCount].end   = le;
        lineCount++;
    }

    s32 missingKeys = 0;
    s32 i = 0;
    while (i < lineCount) {
        if (cfg_line_is_blank(lines[i].start, lines[i].end)) { i++; continue; }

        const s32 blkBegin = i;
        s32 blkEnd = i;
        i++;
        while (i < lineCount && !cfg_line_is_blank(lines[i].start, lines[i].end)) {
            blkEnd = i;
            i++;
        }

        for (s32 k = blkBegin; k <= blkEnd; k++) {
            const char* key;
            const size_t kl = cfg_line_key(lines[k].start, lines[k].end, &key);
            if (kl == 0) { continue; }
            if (!cfg_seen_has(seenKeys, key, kl)) { missingKeys++; }
        }
    }
    if (missingKeys == 0) { return 0; }

    FILE* f = fopen(path, "a");
    if (f == NULL) {
        LOG_ERROR("djui_modern: could not append missing options to %s", path);
        return 0;
    }
    if (!cfg_file_ends_with_newline(path)) { fputc('\n', f); }
    fputs("\n"
          "# ---------------- 自动补全的配置项 ----------------\n"
          "# 以下是本文件里缺失、已由程序按当前版本补全的项（含说明注释）。\n"
          "# 键的先后顺序不影响解析；想归类可以自行剪切到上面的对应小节。\n", f);

    s32 addedKeys = 0;
    i = 0;
    while (i < lineCount) {
        if (cfg_line_is_blank(lines[i].start, lines[i].end)) { i++; continue; }
        const s32 blkBegin = i;
        s32 blkEnd = i;
        i++;
        while (i < lineCount && !cfg_line_is_blank(lines[i].start, lines[i].end)) {
            blkEnd = i;
            i++;
        }

        s32 missingCount = 0;
        for (s32 k = blkBegin; k <= blkEnd; k++) {
            const char* key;
            const size_t kl = cfg_line_key(lines[k].start, lines[k].end, &key);
            if (kl > 0 && !cfg_seen_has(seenKeys, key, kl)) { missingCount++; }
        }
        if (missingCount == 0) { continue; }

        for (s32 k = blkBegin; k <= blkEnd; k++) {
            const char* key;
            if (cfg_line_key(lines[k].start, lines[k].end, &key) != 0) { continue; }
            fwrite(lines[k].start, 1, (size_t)(lines[k].end - lines[k].start), f);
            fputc('\n', f);
        }
        for (s32 k = blkBegin; k <= blkEnd; k++) {
            const char* key;
            const size_t kl = cfg_line_key(lines[k].start, lines[k].end, &key);
            if (kl == 0 || cfg_seen_has(seenKeys, key, kl)) { continue; }
            fwrite(lines[k].start, 1, (size_t)(lines[k].end - lines[k].start), f);
            fputc('\n', f);
            addedKeys++;
        }
    }

    fclose(f);
    LOG_INFO("djui_modern: config auto-completed (%d new option(s)) -> %s", addedKeys, path);
    return addedKeys;
}

void djui_modern_config_load(void) {
    if (sConfigLoaded) { return; }
    sConfigLoaded = true;
    djui_modern_config_defaults();

    const char* path = config_path();
    FILE* f = (path != NULL) ? fopen(path, "r") : NULL;

    if (f == NULL) {
        // 首次运行：生成默认配置。
        config_write_default();
        return;
    }

    // 已出现的键名，每行一个。
    char seen[2048];
    size_t seenLen = 0;
    seen[0] = '\0';

    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        // 去掉注释
        char* hash = strchr(line, '#');
        if (hash) { *hash = '\0'; }
        char* semi = strchr(line, ';');
        if (semi) { *semi = '\0'; }

        char* eq = strchr(line, '=');
        if (eq == NULL) { continue; }
        *eq = '\0';

        char* key = trim(line);
        char* value = trim(eq + 1);
        if (*key == '\0') { continue; }
        apply_kv(key, value);

        const size_t klen = strlen(key);
        if (klen > 0 && klen < 64 && seenLen + klen + 1 < sizeof(seen)) {
            memcpy(seen + seenLen, key, klen);
            seenLen += klen;
            seen[seenLen++] = '\n';
            seen[seenLen] = '\0';
        }
    }

    fclose(f);
    LOG_INFO("djui_modern: loaded config from %s", path);

    // 将缺失项追加到配置末尾。
    config_append_missing(path, seen);
}
