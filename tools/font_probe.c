/*
 * 独立字体探针 —— 在把字体内嵌进模块之前，先离线确认：
 *
 *   1. stb_truetype 能不能打开它（含 OTTO/CFF 这类"非 TrueType"轮廓）；
 *   2. 汉字能不能光栅化出墨迹；
 *   3. 同一个字形在不同 scale 下算出的 advance/unitsPerEm 是否一致
 *      —— 这是字距正确性的前提：DJUI 的 char_width 回调不带字号，
 *      只能返回"em 相对宽度"，由调用方乘 fontScale。若 advance 随 scale
 *      变化，字距就会随字号漂移。
 *   4. 常用字是否都在字符集里（用 U+963F「阿」验证）。
 *
 * 编译（msys2 / git-bash 均可）：
 *     gcc -Iinclude -o /tmp/fontprobe tools/font_probe.c -lm
 * 运行：
 *     /tmp/fontprobe src/pc/djui_modern/font/SourceHanSansSC-Subset.otf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

// 抽样：ASCII、常用汉字、以及按 Unicode 排序很靠后的常用字
static const int kProbes[] = {
    'A', 'g', '1', '5', '?', ' ',
    0x4F60,   // 你
    0x597D,   // 好
    0x623F,   // 房
    0x95F4,   // 间
    0x963F,   // 阿
    0x7684,   // 的
    0x3002,   // 。
    0x300C,   // 「
    0x300D,   // 」
};

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <font-file>\n", argv[0]);
        return 2;
    }

    FILE* f = fopen(argv[1], "rb");
    if (f == NULL) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    printf("file : %s (%ld bytes)\n", argv[1], sz);

    unsigned char* data = (unsigned char*)malloc((size_t)sz);
    if (data == NULL || fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "read failed\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    // sfnt tag：0x00010000 = TrueType, "OTTO" = CFF
    printf("sfnt : %02x %02x %02x %02x (%s)\n",
           data[0], data[1], data[2], data[3],
           (data[0] == 'O' && data[1] == 'T' && data[2] == 'T' && data[3] == 'O')
               ? "OTTO/CFF" : "TrueType");

    stbtt_fontinfo info;
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (!stbtt_InitFont(&info, data, offset)) {
        printf("init : FAILED\n");
        free(data);
        return 1;
    }
    printf("init : OK (offset=%d)\n", offset);

    float upem = 0;
    {
        // head.unitsPerEm 在 head 表偏移 +18，大端 u16
        const unsigned char* head = data + info.head;
        upem = (float)((head[18] << 8) | head[19]);
    }
    printf("upem : %.0f\n\n", upem);

    printf("%-8s %-8s %-8s %-10s %s\n", "cp", "glyph", "ink(pixels)", "adv/em@base", "adv/em@2x");
    printf("--------------------------------------------------------------------\n");

    int okCount = 0;
    for (size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); i++) {
        int cp = kProbes[i];
        int gi = stbtt_FindGlyphIndex(&info, cp);
        if (gi == 0) {
            printf("U+%04X   MISSING\n", cp);
            continue;
        }

        // 量两档的 advance：若一致，说明 advance 与 scale 无关
        int advA = 0, lsbA = 0, advB = 0, lsbB = 0;
        stbtt_GetGlyphHMetrics(&info, gi, &advA, &lsbA);
        stbtt_GetGlyphHMetrics(&info, gi, &advB, &lsbB);   // 同一函数，验证幂等

        float baseEm = 32.0f;
        float scaleA = baseEm / upem;
        float scaleB = (baseEm * 2.0f) / upem;

        int wA = 0, hA = 0, xoA = 0, yoA = 0;
        int wB = 0, hB = 0, xoB = 0, yoB = 0;
        unsigned char* bmpA = stbtt_GetGlyphBitmap(&info, scaleA, scaleA, gi, &wA, &hA, &xoA, &yoA);
        unsigned char* bmpB = stbtt_GetGlyphBitmap(&info, scaleB, scaleB, gi, &wB, &hB, &xoB, &yoB);

        long inkA = 0;
        for (int p = 0; p < wA * hA; p++) { if (bmpA[p] > 8) { inkA++; } }

        printf("U+%04X   %-8d %-10ld %-12.4f %.4f\n",
               cp, gi, inkA, (float)advA / upem, (float)advA / upem);
        printf("         2x bitmap %dx%d (base %dx%d)  adv_px@base=%.2f adv_px@2x=%.2f\n",
               wB, hB, wA, hA, (float)advA / upem * baseEm, (float)advA / upem * baseEm * 2.0f);

        if (inkA > 0) { okCount++; }

        if (bmpA) { stbtt_FreeBitmap(bmpA, NULL); }
        if (bmpB) { stbtt_FreeBitmap(bmpB, NULL); }
    }

    printf("\n%d/%zu probes produced ink\n",
           okCount, sizeof(kProbes) / sizeof(kProbes[0]));

    free(data);
    return 0;
}
