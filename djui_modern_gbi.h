#pragma once

#include <PR/gbi.h>
#include <PR/gbi_extension.h>

// 静态模板使用的 DJUI 2D 三角形命令。
#ifndef gsSP2TrianglesDjui
#define gsSP2TrianglesDjui(v00, v01, v02, flag0, v10, v11, v12, flag1)	\
{{									\
    (_SHIFTL(G_TRI2_EXT, 24, 8) |					\
     __gsSP1Triangle_w1f(v00, v01, v02, flag0)),			\
    __gsSP1Triangle_w1f(v10, v11, v12, flag1)				\
}}
#endif
