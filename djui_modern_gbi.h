#pragma once

/* djui_modern 私有 GBI 宏。 */

#include <PR/gbi.h>
#include <PR/gbi_extension.h>

/* gSP2TrianglesDjui 的静态初始化器；使用 DJUI 的 2D 三角形路径。 */
#ifndef gsSP2TrianglesDjui
#define gsSP2TrianglesDjui(v00, v01, v02, flag0, v10, v11, v12, flag1)	\
{{									\
    (_SHIFTL(G_TRI2_EXT, 24, 8) |					\
     __gsSP1Triangle_w1f(v00, v01, v02, flag0)),			\
    __gsSP1Triangle_w1f(v10, v11, v12, flag1)				\
}}
#endif
