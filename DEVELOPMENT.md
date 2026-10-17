# djui_modern — 现代化 DJUI 皮肤层

一个**自包含**的 DJUI 外观层：圆角 + 半透明 + 渐变 + 矢量字体（TTF/OTF）。

设计目标按优先级排序：

1. **上游可移植** —— 上游更新后，本目录可以直接照搬，只需要重新确认 `djui_rect_render`  
   的序言能被解码（失败会自动退回原版，不会崩）。
2. **mod 兼容** —— 不改任何 DJUI 的布局/交互逻辑，所有 `djui_*` lua 接口行为完全不变。
3. **低开销** —— 渲染一个圆角矩形与渲染一个方角矩形同数量级；字形图集在内容不变的帧  
   零纹理上传。

---

## 0. 安装（推荐：跑脚本）

本模块可以作为一个 **git 子模块**挂在 `src/pc/djui_modern`，由随附脚本自动完成
"挂子模块 + 应用下面表里列出的全部上游改动"：

```bash
# 桌面（Windows / macOS / Linux）
sh src/pc/djui_modern/install.sh                 # 在当前源码树根目录执行
sh src/pc/djui_modern/install.sh --root <源码树>  # 或指定目录

# Android 分支
sh src/pc/djui_modern/install-android.sh
```

常用开关：

| 开关              | 作用                                            |
| --------------- | --------------------------------------------- |
| `--root <目录>`   | 指定 sm64coopdx 源码树根目录（默认：当前目录，或脚本所在位置往上找）      |
| `--url <仓库地址>`  | 挂子模块时用的仓库地址（默认自动探测本模块自己的 `origin`）             |
| `--no-submodule` | 不碰 git，只应用补丁（模块已就位时用）                          |
| `--revert`      | 反向应用补丁，把原版文件恢复成未安装状态（**模块目录本身不删**）             |
| `--dry-run`     | 只检查能否应用，不写任何文件                                |

脚本的行为特点：

- **幂等** —— 已经打过的补丁会跳过，重复执行不会重复插入（判据是
  `patches/markers.txt` 里的"已应用标记"，比"反向能否应用"可靠）。
- **原子** —— 应用前先 `--check` / `--dry-run`，失败就一个字节都不改；
  也不会留下 `.rej` / `.orig`。
- **两个分支各一套补丁** —— Android 分支的 `Makefile` / `djui.c` 不是同一个上游
  修订，所以额外生成 `*.android.patch` 变体。**绝不靠 `patch --fuzz` 硬套**：
  实测高 fuzz 会把 `djui_modern_post_render();` 插到 `djui_render()` 的右花括号
  **之后**（文件作用域）却仍然报告成功 —— 静默产出错误代码。
- 补丁只覆盖**下面表里列出的文件**（`tools/make_patches.py` 里的 `TARGET_FILES`
  与这张表一一对应）⇒ README 没写的改动不会混进上游。

> 手工合也可以：下表每行都给了精确的位置与行数。脚本只是把这件事自动化。

---

## 1. 上游移植指引（更新后怎么合）

### 需要改动的原文件：10 个文件（**全部是 `.c/.cpp`，一个头文件都不动**），共 +237 / −22 行

| 文件                                                       | 改动                                                            | 行数  | 必要性                 |
| -------------------------------------------------------- | ------------------------------------------------------------- | --- | ------------------- |
| `Makefile`                                               | `SRC_DIRS += ... src/pc/djui_modern ...`                      | 1   | 必须                  |
| `src/pc/djui/djui.c` `djui_init()` **开头**（第一个 `djui_text_create()` 之前） | `djui_modern_init();`（**位置很关键，见下**）                            | 1   | 必须                  |
| `src/pc/djui/djui.c` `djui_render()` 开头                  | `djui_modern_pre_render();`                                   | 1   | 必须                  |
| `src/pc/djui/djui.c` `djui_render()` 末尾                  | `djui_modern_post_render();`                                  | 1   | 必须（诊断，可关）           |
| `src/pc/djui/djui.c` `djui_shutdown()`                   | `djui_modern_shutdown();`                                     | 1   | 可选（释放内存）            |
| `src/pc/djui/djui_rect.c` `djui_rect_render()` 入口        | `if (djui_modern_rect_override(base)) return true;` + 弱兜底定义   | 1+8 | 必须（见下）              |
| `src/pc/djui/djui_gfx.c` `djui_gfx_size_translate()` 末尾 / `djui_gfx_add_clipping_specific()` 开头 | 两处各 `+1` 行转发/上报 + 两处弱兜底定义 | 2+25 | 必须（字体 / 文字裁剪，见下） |
| `src/pc/djui/djui_hud_utils.c` `djui_hud_size_translate()` **N64 分支** | 同样上报 `djui_modern_note_font_size(*size);`（原型写在本文件里，**不加 weak**）—— 见"为什么 N64 屏幕空间也要上报字号" | 1+32 | 必须（HUD 文字档位，见下） |
| `src/pc/djui/djui_unicode.c` `djui_unicode_valid_char()` | `if (djui_modern_unicode_accepts(text)) return true;` + 弱兜底定义 | 3+8 | 必须（输入框 emoji/国旗，见下） |
| `src/pc/djui/djui_text.c` `djui_text_create()` 末尾          | `djui_modern_note_text_created(text);` + 弱兜底定义                 | 1+7 | 必须（文字节点字体策略，见下）     |
| `src/pc/djui/djui_panel_host_mods.c` mod 描述面板 | 描述面板指针改为非 static 的 `gDjuiHostModsDescriptionPanel`，**并把赋值提到创建描述文字之前**（`extern` 声明由模块自己给） | 3+7 | 必须（`font_mod_description`，见下） |
| `src/pc/djui/djui_panel_host.c` 密码标签                     | `djui_text_set_drop_shadow(...)`（**补原版漏设的投影**）                | 1   | 可选（原版 bug，见下）       |
| `src/pc/gfx/gfx_dxgi.cpp` `gfx_dxgi_on_text_input()`   | **支持 UTF-16 代理对**（原版把代理直接丢弃）                              | ~53 | 可选（原版 bug，见下） |

> 237 行里，真正的**钩子调用**只有十来处（各 1~3 行），其余是弱兜底定义、
> 原型与说明注释、两处原版 bug 修复。删掉任意一处"可选"改动都不影响模块其余部分。
>
> ⚠ **两个分支各一套补丁**：Android 分支的 `Makefile` 与 `djui.c` 与桌面版不同
> （`TARGET_ANDROID` 分支、`djui_render()` 的结尾结构都不一样），所以
> `patches/` 里对这两个文件额外有 `*.android.patch`。改上游时两份都要重新生成。


> **为什么玩家列表"时而矢量字体、时而原版字体"**：`djui_text_create()` 会把
> `gDjuiFonts[slot]` 的**当前指针**存进 `text->font`，而模块是在 `djui_modern_init()`
> 里替换 `gDjuiFonts[]` 的。原来 `djui_modern_init()` 排在 `djui_init()` 的**末尾**，
> 而 `djui_panel_playerlist_create(NULL)` 在它**前面** —— 面板的文字节点先创建、
> 拿到的是**原版字体指针**，之后替换 `gDjuiFonts[]` 也改不到它们；
> 只有**重开面板**时新建的节点才拿到矢量字体 ⇒ 时好时坏。
>
> **修法就是把 `djui_modern_init()` 挪到 `djui_init()` 开头**（第一个
> `djui_text_create()` 之前）—— 之后创建的所有文字节点自然都拿到模块字体，
> 不需要任何"事后修正"，也不占每帧开销。
>
> 在这之上，`djui_text_create()` 末尾会上报一次 `djui_modern_note_text_created()`，
> 让模块按配置把**三个特定面板**固定成矢量字体或原版字体：
> · `font_player_list = false` ⇒ 玩家列表用原版点阵字体；
> · `font_mod_description = false` ⇒ mod 列表 / mod 描述面板用原版点阵字体。
> 默认两项都是 `true` ⇒ 走"目标 = 模块字体"这条最短路，**不做任何面板查找**。
> 面板识别靠 `gDjuiPlayerList` / `gDjuiModList` / `gDjuiHostModsDescriptionPanel`
> 三个全局指针沿 parent 链比对（**不是**控件类型枚举 ⇒ 上游加新控件不会出错）。

> **为什么 emoji"能粘贴、打不出来"**：Windows 从 Vista 起用 **UTF-16 代理对**
> 发送非 BMP 字符，而**所有 emoji 都在 U+1F000 以上**（含国旗的
> Regional Indicator `U+1F1E6..U+1F1FF`）⇒ 打字 / 输入法联想 / `win + .`
> 送来的必然是"先高位代理、再低位代理"两条 `WM_CHAR`。
> 原版 `gfx_dxgi_on_text_input()` 里那句
> `if (... && !IS_HIGH_SURROGATE(c) && !IS_LOW_SURROGATE(c))` 把两个代理
> **都直接丢弃** ⇒ **所有 emoji 都打不出来，但中文（BMP）正常**。
> 粘贴能进是因为走剪贴板、不经过这条路径。
> 这里把代理对拼回一个码点再按 4 字节 UTF-8 送出。



> **输入框为什么输入不了 emoji / 国旗**：原版 `djui_unicode_valid_char()` 只认  
> 点阵字形表 `sCharMap`（由 `sSmCodeGlyphs` CJK + `sSmCodeGlyphs_EMOJI`  
> **原版那 52 个 emoji** 构成）。于是：  
> · 原版 52 个 emoji → 合法（所以"复制粘贴能进去"）；  
> · 本模块新收的 127 个 emoji → 不在表里 → 被丢掉；  
> · 国旗（Regional Indicator `U+1F1E6..U+1F1FF`）→ 完全不在表里 → 被丢掉。  
> `win + .` 里绝大多数是现代 emoji，所以表现为"打字也不行、旗帜完全进不去"。  
> 这里加一个**弱兜底钩子**（与 `djui_rect.c` / `djui_gfx.c` 同一手法），  
> 模块侧把"自己能渲染的字符"放行（现代 emoji 图集 + RI 区间）。  
> 模块缺席时弱兜底返回 false，行为与改动前**逐字节一致**。

> **唯一一处"非钩子"改动**：`djui_panel_host.c` 里"密码"标签漏了  
> `djui_text_set_drop_shadow()`，而同面板的 `PORT` / `存档槽位` 两个标签都设了 ——  
> 原版自身的不一致（在亮色 / 文件选择主题下特别显眼：密码二字比旁边亮一档）。  
> 补 1 行与同面板保持一致。**不需要的话直接删掉这一行即可**，模块其余部分不受影响。

头文件侧**一处都不动**（早期版本曾在 `djui_rect.h` / `djui_gfx.h` / `djui_text.h` /
`djui_panel_host_mods.h` 里加普通声明，现已全部撤回）—— 钩子原型改写在**调用它的那个 `.c` 文件里**：

| 钩子                              | 原型写在哪                                        |
| ------------------------------- | -------------------------------------------- |
| `djui_modern_rect_override`     | `djui_rect.c`（弱兜底定义就在它上面，无需额外原型）                |
| `djui_modern_note_font_size`    | `djui_gfx.c`（弱兜底）+ `djui_hud_utils.c`（调用点，显式原型） |
| `djui_modern_note_text_base`    | `djui_gfx.c`（弱兜底）                             |
| `djui_modern_note_text_created` | `djui_text.c`（弱兜底）                            |
| `djui_modern_unicode_accepts`   | `djui_unicode.c`（弱兜底）                         |
| `gDjuiHostModsDescriptionPanel` | 上游只把 `static` 去掉；`extern` 声明由**模块自己**给          |

> **为什么必须写在 `.c` 里而不是头文件里**：`djui_rect.h` / `djui_gfx.h` / `djui_text.h`
> 都被 `djui.h` 包含，而 `djui.h` 是全院最共享的头。往里加一行声明，**每次打补丁**
> 都会让所有 include 它的翻译单元判成过期 —— 实测 `make -n` 需要重编 226 个目标，
> 其中 **220 个的唯一原因就是 `djui_text.h` 的时间戳变了**。改写进 `.c` 之后，
> 打补丁的重编范围回到"这十来个文件"。

> **"弱符号"是关键手法**：只有**定义处**带 `__attribute__((weak))`（即  
> `djui_rect.c` / `djui_gfx.c` 里的空兜底）；模块侧的强定义在链接期覆盖它。  
> 因此原版文件**不需要 include 任何模块头文件、不需要条件编译** —— 把  
> `djui_modern/` 目录整个删掉，原版只需删掉那几处调用就能恢复原状。
>
> ⚠ **绝对的禁忌：不要在（任何）声明上加 `__attribute__((weak))`。**  
> GCC 的 weak 属性会**传染**：一旦声明带 weak，所有经 `djui.h` 包含它的  
> 翻译单元里、同名函数的定义都会变成 weak —— 包括 djui_modern 侧本该是  
> 强符号的定义。链接器面对两个 weak 符号时会**任选其一**（实测选中先出现的  
> 原版空实现），结果就是：接管**静默失效**、无任何编译/链接告警、  
> 界面完全没变化。本模块踩过这个坑（表现为"改了代码但界面毫无反应"），  
> 所以各处原型都是**不带 weak 的普通声明**，  
> weak 只写在原版 `.c` 的兜底定义上。
>
> weak 属性语法是 GCC/Clang 的 `__attribute__((weak))`；MSVC 下这段被  
> `#if defined(__GNUC__)` 括住，退化为普通的强定义（此时若同一符号在模块里  
> 也有定义会重复定义报错 —— MSVC 用户把模块侧的 `djui_modern_rect_override` /  
> `djui_modern_note_font_size` 删掉即可，原版行为完全不变）。

```diff
  void djui_render(void) {
      if (!sDjuiInited || gDjuiDisabled) { return; }
+     djui_modern_pre_render();
      sSavedDisplayListHead = gDisplayListHead;
```

```diff
      djui_lua_profiler_create();
+     djui_modern_init();
      sDjuiInited = true;
```

```diff
  bool djui_rect_render(struct DjuiBase* base) {
+     if (djui_modern_rect_override(base)) { return true; }
      struct DjuiBaseRect* clip = &base->clip;
```

```diff
  void djui_gfx_size_translate(f32* size) {
      ...
      *size = *size * ((f32)SCREEN_HEIGHT / (f32)windowHeight) * djui_gfx_get_scale();
+     djui_modern_note_font_size(*size);
  }
```

**除此之外没有任何其他改动。** 如果上游把 `djui_init()` / `djui_render()` 改名或挪走，  
把这几行跟着挪过去即可。

### 为什么只要 1 行就够了：`djui_rect_render` 是单点汇聚

原版**所有**矩形填充都汇聚到一个叶子函数 `djui_rect_render()`：

```
djui_rect_create() → base->render = &djui_rect_render
djui_base_render() → 遍历时现读 base->render 调用它
```

它被 **9 处直调**（`grep -n "djui_rect_render(" src/pc/djui/*.c`）：

| 控件                    | 直调点    |
| --------------------- | ------ |
| `djui_chat_message.c` | `:42`  |
| `djui_console.c`      | `:67`  |
| `djui_ctx_display.c`  | `:84`  |
| `djui_fps_display.c`  | `:22`  |
| `djui_inputbox.c`     | `:564` |
| `djui_lua_profiler.c` | `:133` |
| `djui_paginated.c`    | `:118` |
| `djui_popup.c`        | `:31`  |
| `djui_three_panel.c`  | `:158` |

再加控件自身经 `base->render` 的调用，**全部**经过这一个入口。  
所以在 `djui_rect_render()` 入口转一次，9 处**自动全部**走现代化实现 ——  
这就是"原版只需 1 行"的全部秘密。

**为什么不是在外部包 wrapper 换 `base->render` 指针？**  
旧做法是遍历控件树，把 `base->render` 从 `djui_rect_render` 换成模块的 wrapper，  
wrapper 里把 `base->color.a` 置 0 调一次原版（保留布局与淡出）、再补画圆角。  
这条路有两个**结构性**问题，实测都会暴露：

1. **漏掉直调点。** `djui_chat_message_render` 等 9 个函数内部是  
   **直接写 `djui_rect_render(base)`**，绕过 `base->render` 指针 —— 换指针栏不住。  
   旧解法是去这些 `.c` 里去掉 `static` 再 extern，把原版改动扩散到 8 个文件 26 行，  
   这正是 1.0 版的问题。
2. **颜色被原版覆盖 ⇒ 双重绘制 + 丢淡出。** wrapper 在**进入原版之前**把  
   `base->color.a` 置 0，但原版控件在**每帧自己重设 color**（聊天消息的生命周期淡出、  
   `djui_inputbox_update_style` 等）。结果是：wrapper 的清零被覆盖 → 原版照常画出方角，  
   你补画的圆角叠在上面 → **同位置方角 + 圆角同时出现**；且 wrapper 提前读到的 alpha  
   是上一帧的旧值，淡出逻辑丢失。

**入口转发的做法从根上避开了这两点**：我们是在**原版设好颜色之后**才被调用的，  
读到的 `base->color` / `base->gradient` 就是原版本帧真正要用的值。因此：

- 原版淡出（聊天消息随生命周期降低 alpha）**原样生效**；
- 不存在"原版方角 + 我们圆角"的双重绘制；
- 9 处直调**全都自动覆盖**，一个都不用改。

> 转发只是一次普通函数调用，与机器码无关，不改可执行内存、不依赖架构 ——  
> 平台无关的承诺依然成立。

未启用 `djui_modern`（或模块未参与编译、图集未就绪、控件尺寸非法）时，  
`djui_modern_rect_override()` 返回 `false`，原版实现照常执行，行为与改动前**完全一致**。  
返回 `true` 表示本次已由现代化实现绘制完毕，原版实现直接跳过。

### 为什么字体也要 1 行：`djui_gfx_size_translate` 是唯一的字号转换点

矢量字体需要知道"这次文字的字号是多少"来决定字形位图的**光栅化分辨率**  
（Lua 的 `djui_hud_print_text` 里 scaleX 可以是 8.0，屏幕上字高 256px；  
若位图按 32px 光栅化再放大 8 倍，边缘必然糊成一团）。

问题在于 `DjuiFont::render_begin/char/end` 这几个回调是**无参数的**，  
物理上拿不到字号。早期版本试图**沿 display list 往回扫 64 条**找"字号缩放矩阵"，  
但会误命中 `djui_text.c:255` 的**逐字符平移矩阵** —— 平移矩阵是对角阵  
（`m[0]=m[5]=1.0`、非对角元 0），与缩放矩阵**数值上无法区分**，所有判据都会放行它。  
于是字号被算成 `1.0 × 窗口系数`（1080p 下 ≈ 4.5）→ 钳到下限 → 字形按最小档光栅化  
→ **屏幕上一个字都看不见**。多行文本从第 2 行起必然触发，因为  
`djui_text_render_line()`（`djui_text.c:416`）**每行**都会调一次 `render_begin()`。  
这套方案在架构上必然失败。

正解是找一个**语义无歧义**的上报点：`djui_gfx_size_translate()` 是**全工程唯一的  
文字字号转换函数**，只有 3 个调用者，全部是文字：

| 调用者                    | 场景                                         |
| ---------------------- | ------------------------------------------ |
| `djui_text.c:487`      | DJUI 控件文字                                  |
| `djui_inputbox.c:581`  | 输入框文字                                      |
| `djui_hud_utils.c:124` | Lua HUD 文字（经 `djui_hud_size_translate` 转发） |

元素尺寸走 `djui_gfx_scale_translate()`、位置走 `djui_gfx_position_translate()`，  
都不会经过它 —— 所以它上报的值**只可能是文字字号**。在函数末尾加 1 行把结果  
告诉模块，字体后端就能算出屏幕 em 像素：

```
屏幕 em 像素 = 上报的字号 × (windowHeight / SCREEN_HEIGHT)
```

彻底删除 DL 回溯，问题从架构上消失。

### 为什么 N64 屏幕空间也要上报字号（mod 的 HUD 文字模糊）

上表第 3 行有个陷阱：`djui_hud_size_translate()` **只在 `RESOLUTION_DJUI` 时**
才转调 `djui_gfx_size_translate()`：

```c
if (sHudUtilsState.resolution == RESOLUTION_DJUI) { djui_gfx_size_translate(size); }
```

`RESOLUTION_N64` 时字号矩阵**不做任何换算**（矩阵字号就是 N64 屏幕单位的字号），  
这条分支以前既不换算、也**不上报** ⇒ 字体后端只能沿用「上一次 DJUI 文字留下的  
字号」或回退值 32。而 mod 的 HUD 文字几乎都用 `RESOLUTION_N64`：

| 典型 mod 文字                  | 真实屏幕 em                                    | 按 32 取档 |
| -------------------------- | ------------------------------------------ | ------- |
| `hide-and-seek.lua` 顶部横幅 | `32 × 0.5 × (900/240)` = **60px**（1600×900） | 40px 位图 |
| `day-night-cycle` 时钟       | 同上                                         | 同上      |

60px 的字用 40px 位图放大 1.5 倍 ⇒ **模糊 + 锯齿**，描边（mod 自己画的 4 份偏移拷贝）  
跟着一起变"像素风"。更迷惑的是：改「界面缩放」会**碰巧**让它变清楚 —— 因为那改的是  
上一次 DJUI 文字上报的字号，与本次 HUD 文字无关；而 HUD 文字本身的大小不变。

修法与 DJUI 分支完全对称：N64 分支也上报 `*size`：

```c
static void djui_hud_size_translate(f32* size) {
    if (sHudUtilsState.resolution == RESOLUTION_DJUI) {
        djui_gfx_size_translate(size);
    } else {
        djui_modern_note_font_size(*size);   // ← 本模块加的那 1 行
    }
}
```

`current_em_pixels()` 会乘回 `windowHeight / SCREEN_HEIGHT`，两个分支得到同一种  
"屏幕像素 em" 口径。**注意这条分支的原型同样不能加 weak**（理由见 `djui_hud_utils.c` 里的原型注释）。

### 字体后端：为什么是"一字形一纹理，且内容不可变"

拿到字号之后，剩下的问题是**字形位图放哪、怎么查、什么时候上传显存**。  
前三版都把全部字形塞进一张共享大图集，靠"内容变了就重新上传"来显示新字形 ——  
这条路**在 `gfx_pc` 的缓存模型下结构上不可能成立**：

```c
// gfx_pc.c:328  纹理缓存以**数据指针**为键
hash = (uintptr_t)orig_addr;
...
if (CMPADDR((*node)->texture_addr, orig_addr) && ...) { ... return true; }   // 命中

// gfx_pc.c:591  命中即直接复用旧 GL 纹理，**跳过上传**
if (gfx_texture_cache_lookup(...)) { return; }
```

而一个地址**只在第一次出现时被插入缓存链表头**，此后永远命中它。于是：

```
图集内容变了、指针没变  ⇒  命中旧条目  ⇒  永远显示第一次上传的那一版
```

真机表现正是用户看到的：

> **西文只能显示 `1` 和 `5`；中文只能显示"房间"等字。**

—— 只有**第一次上传那一刻**已经光栅化的那几个字形可见，之后光栅化的全部  
画不出来，而且**没有任何报错**。

> 曾经的"双副本地址轮换"也救不了：地址被复用时旧条目仍在链表头，同样命中；  
> 而且每换一次地址就要重传整张图集，显存里会永久留一份 1024² RGBA32（4MB）  
> 的废弃副本，换十次就是 40MB。

v4 的解法是让"内容不可变"成为**结构上的事实**：

| 做法                                | 说明                                  |
| --------------------------------- | ----------------------------------- |
| 每个 `(字符, 档位)` 独占一张 2 的幂 RGBA32 纹理 | 边长 = 位图尺寸的 `pot_ceil`（最小上界），浪费约 20% |
| **生成后字节永不再改**                     | 指针缓存天然正确：命中旧条目 = 拿到正确内容             |
| 缓存池整池回绕（4096 项）也无所谓               | 最坏只是重新上传一遍，内容依旧正确                   |
| 显存占用 = Σ 字形纹理面积                   | 一次性开销，**不随更新次数增长**                  |
| 连续重复字形跳过重绑                        | 常见字（"的"、"你"）连续出现时省掉纹理命令             |

因为不再有共享图集，下面这些整类复杂度**直接消失**：

```
图集装箱（行游标 / 行高 / 容量 / 满图集判定） · GLYPH_PAD 隔离带
纹素坐标 <= 1023 的 S10.5 硬上限 · LRU 淘汰 · 行回收 · 内容版本号 · 双副本轮换
```

运行期状态只剩 **2 类**：字形是否已光栅化（`GlyphSlot.state`）、顶点游标（`sVtxUsed`）。

另外，字符集装不下全部 Unicode，所以 `charset_lookup()` 查不到时返回  
`FALLBACK_INDEX(0)` —— 槽位表第 0 行专门放 `'?'` 的位图。  
**没预登记的字显示为 `?`，但绝不会走进未定义状态。**

> **特例：Coopdx 的"星星"符号（0x7F）。**  
> 上游把 DEL(0x7F) 当星形用（`djui_text.c:235 // the star`、  
> `djui_panel_host.c:178` 的 `'~' + 1`）。矢量字体没有这个私有约定，若不处理  
> 就会显示 `?`。模块把 0x7F **重定向到 Unicode 规范五角星 ★（U+2605）**：  
> 字体自带 ★ 就用字体字形；字体没有（子集常见）则由 `star_build_bitmap()`  
> **程序化生成单色五角星**（标准五角星几何 + 4×4 超采样抗锯齿，由 ENV 着色，  
> **不是彩色 emoji**）。重定向在 `codepoint_remap()` 一处完成，测量与绘制同源。

这带出两条通用原则（对任何嵌入式/受限渲染后端都成立）：

> **1. 资源一旦要被下游按"指针/句柄"缓存，就让它的内容不可变。**  
> 想改内容就换一个新资源对象；旧对象任其被缓存也不出错。  
> 这比在缓存层加"失效通知"要可靠得多 —— 后者要求下游配合，而下游往往不受你控制。

> **2. 在"状态漏重置 → 静默全灭、无报错"的环境里，宁可多花一点带宽做无条件  
> 重做，也不要为了省那点开销引入需要跨帧维护的状态。**

### ⚠ 字形 display list 必须"应用并消费"纹理裁剪状态

模块的字形模板 `sGlyphTemplate` 是三条命令：

```c
gsSPVertexNonGlobal(NULL, GLYPH_VERTS, 0),
gsSPExecuteDjui(G_TEXCLIP_DJUI),          // ← 绝不能少
gsSP2TrianglesDjui(0, 1, 3, 0, 0, 3, 2, 0),
```

与 `djui_gfx.c:250-252` 的原版顺序一致。

**为什么少不得**：`djui_text` 画每个字形前会调 `djui_gfx_add_clipping_specific()`  
（`djui_text.c:251`），需要裁剪时它发出 `gDPSetTextureClippingDjui()`；而  
`gfx_pc` 侧那个命令**只是记下参数并置 `sDjuiClip = true`**（`gfx_pc.c:2278`），  
真正把裁剪应用到顶点、并清掉标志的是随后的 `G_TEXCLIP_DJUI` 执行  
（`gfx_pc.c:2175`，内部 `sDjuiClip = false`）。

少了这一条会有两个后果：

1. 模块自己的字形**永远不被裁剪**；
2. 更隐蔽的是那组**陈旧的裁剪参数 + 置位的标志会一直留着**，被  
   **下一个执行该命令的字形**吃掉 —— 而面板标题用的是**原版艺术字体**  
   （slot 1 未被替换），于是**标题的第一个字被上一次残留的裁剪切掉一截**，  
   后面的字却完全正常。残留值来自"最后一次被裁剪的文字"（例如滑条右侧  
   那个宽度随数值变化的数值文本）⇒ 表现为"**截断程度随滑条的值变化**"。

这类 bug 的特征是"**只有第一个字/第一个元素出错，其余正常**" ——  
凡是遇到这种症状，先怀疑"某个每元素都会消费的全局状态漏了一次消费"。

**补充两条（后续又踩到的）**：

1. **"不画任何几何"的分支也必须消费**。国旗的**尾巴**（第二个 RI）、孤立 RI、
   以及"无墨迹的字形"都不发射几何，但**绝不能直接 `return`** —— 否则裁剪
   状态残留、下一个字形中招（真机：**聊天里带国旗时，下一行的名字首字母
   被截掉一块**）。统一走 `glyph_consume_pending_clip()`。

2. **垂直裁剪：只有聊天框硬裁剪，其余一律放行**。
   清零是为了满足"文字可以超出控件"（控件比文字略矮时不要把西文下伸部 /
   汉字上下沿切掉）。**唯一例外是聊天框** —— 它靠裁剪把滚出可见区的行藏起来，
   放行就会把本该隐藏的内容整行画到框外。

   判据**不是**"被裁掉的比例"（那个阈值判据在真机上并不可靠），而是
   `djui_modern_note_text_base()`：上游在 `djui_gfx_add_clipping_specific()`
   里上报宿主控件，模块沿 `parent` 链比对 `gDjuiChatBox` 的成员（自身 /
   `chatContainer` / `chatFlow` / `chatInput`），命中 ⇒ `sTextClipHard = true`。
   聊天消息的 parent 链是 `chatText → chatMessage → chatFlow`，所以整条链都被覆盖。
   · **每字形遍历 parent 链即可，别做指针缓存** —— 控件销毁后地址可能被复用，
     缓存会留下过期的 `true`。链深 5~8 层，开销可忽略。
   · 保留垂直时**同样要换算坐标**（见下节）—— 不换算就会变成"每个字按自己
     墨迹盒的比例被裁"，裁剪边界高低不一。
   · ⚠ **底部还要再往下放宽 `sClipPadBottomEm`（≈0.094em）**，否则聊天框会把
     西文**下伸部**（g / y / p / q / j）的尾巴切掉一点（真机："聊天栏文字的截断
     正常工作，但不知道为什么 g 的下面也截断了一点"）。
     原因：基线放在 `-(1 - desc)` 处、而 `desc` 被钳到 ≤ 0.15em，字体真实下伸
     0.244em ⇒ 下伸部有 0.094em 落到**字格之外**；而聊天消息的文字节点高度
     **恰好 = 1em**，裁剪框正好压着字格下沿。
     ⚠ 不能靠调 `g2` —— `g2 = 0` 已经表示"裁到字格下沿"，再往下没有对应的编码；
     要直接在**算出来的裁剪坐标**上放宽（坐标单位就是 em）。
     滚出可见区的整行被裁的比例远大于这个余量（滚出 50% 仍裁掉 70%），照常隐藏。
   将来若发现别的滚动列表（控制台等）也要硬裁剪，往 `base_is_chat_box_part()`
   里加即可 —— **把名单写在一个函数里，别散落在各处**。

> 水平方向**永远保留且换算** —— 滑条那个"反色数值"效果靠它。

> **为什么需要上游钩子**：模块在渲染端，拿不到"这个字形属于哪个控件" ——
> 只有控件自己知道。所以挂在 `djui_gfx.c` 的 `djui_gfx_add_clipping_specific()`
> 开头（1 行调用 + 16 行弱兜底定义）：它是**所有**文字裁剪的统一入口
> （`djui_text_render_single_char()` 与 `djui_inputbox_render_char()` 都走它），
> 一次挂钩覆盖全部路径，也不怕上游将来新增文字渲染路径。
> 模块缺席时是空实现，行为与改动前**逐字节一致**。

### ⚠ 纹理裁剪比例是**相对"名义字格"**的，水平和垂直都必须换算到四边形坐标

`djui_gfx_add_clipping_specific()`（定义在 `djui_gfx.c`）按**字体名义字格**算比例：

```
dW = font->charWidth  * fontScale       // 本模块 charWidth = 1em（见下）
dH = font->charHeight * fontScale
dClipX1 = (clip.x - dX) / dW            // ← 相对**字格**的比例
```

而 `djui_gfx_dp_execute_clipping()`（`gfx_pc.c`）把这组比例**乘到当前
四边形的包围盒上**。原版点阵字体的四边形就是整个字格，所以对得上；
但本模块的字形四边形是**墨迹盒**（西文 ≈ 0.47em 宽 × 0.7em 高，
汉字 ≈ 0.9em 见方），字格是 1em ⇒ 比例被**放大**，裁剪提前。

**水平和垂直都要换算**，否则会出两个症状：

| 方向 | 不换算的症状 |
|---|---|
| 水平 | 滑条右侧"反色数值"的明暗交界比填充位置**偏左约半个数字**（"最右边的数字有一半是白的"） |
| 垂直 | 裁剪边界**高低不一** —— 每个字按自己墨迹盒的比例被裁，与字母本身的高低无关（"截断方式不像原版，每个字裁掉自己位图的部分比例"） |

离线复刻（把上游两段代码照抄成 Python）：

```
水平：填充切在数字一半      期望可见 50.0%   旧实现 28.5%   误差 −21.5%
      汉字（墨迹盒≈字格）    期望可见 50.0%   旧实现 50.0%   误差   0.0%
垂直：裁剪线在屏幕上的位置   旧实现 108.7 ~ 122.9px（跨度 14.2px，越矮的字偏得越多）
                            新实现 恒为 109.60px（跨度 0）
```

修法（`glyph_fix_pending_clip()`，`djui_modern_font_atlas.c`）—— 把比例换算到
四边形自己的坐标：

```c
// 水平：裁剪区在字格里的位置 = [f1*cw, cw - f2*cw]
q1  = (f1 * cw - inkX0) / inkW;
q2  = (inkX0 + inkW - (cw - f2 * cw)) / inkW;

// 垂直：字格坐标里 顶 = 0、底 = -cellH（向下为负）
//       裁剪区 = [-g1*cellH, (g2-1)*cellH]
ky1 = (inkY0 - (-g1 * cellH)) / inkH;
ky2 = ((g2 - 1.0f) * cellH - (inkY0 - inkH)) / inkH;
```

- **水平永远保留**：滑条那个反色效果靠它。
- **垂直默认清零**（允许文字上下溢出控件，用户要求）；**只有聊天框**保留，
  且**同样换算** ⇒ 裁剪边界落在屏幕上的同一位置，与原版一致。
- `cellH` = 当前字体的 `charHeight`（各槽位可能不同），由
  `djui_modern_font.c` 在每次 `render_begin` 时通过
  `djui_modern_font_atlas_set_cell_height()` 同步。
- 扫描范围由 `sGlyphDlMark` 限定在"本次字形之前新发出的那条命令"，
  绝不误改历史命令。

> `DjuiFont.charWidth` 是**字格宽**（本模块固定 1em，见
> `DJUI_MODERN_FONT_CELL_W`），不是平均字宽 —— 它只被 `djui_text.c` 与
> `djui_inputbox.c` 当作**裁剪框**用，改它要同步改换算用的那个常量。

### 上游变更时的风险

| 上游可能的改动                            | 影响                                 |
| ---------------------------------- | ---------------------------------- |
| 重写 `djui_rect_render()` 的函数体       | **无影响**（我们只比较函数地址、不解析它的字节）         |
| 改名/移走 `djui_rect_render`           | 编译报错，顺手把 `modern_adopt()` 里的比较改掉即可 |
| 新增会创建矩形控件的 UI 根（不挂在 `gDjuiRoot` 下） | 该树的矩形不会被现代化，但**仍然正常显示**（走原版渲染），不会崩 |
| 新增矩形控件类型（不走 `djui_rect_create()`）  | 同上，保持原版外观                          |

也就是说：**最坏情况是"某处没有现代化"，永远不是"崩溃"或"花屏"。**

---

## 2. 配置

配置文件位置（**各平台都放在用户最容易找到的地方**）：

| 平台                     | 位置                                          |
| ---------------------- | ------------------------------------------- |
| Windows / macOS / Linux | **游戏根目录**（与 coopdx 可执行文件同级）`djui_modern.cfg` |
| Android                | **与 `sm64config.txt` 同级**（游戏数据目录下的 `user/`） |

**文件不存在时会自动生成一份**，内容与下方各表一致、**带完整注释** —— 所以
删掉它不会丢任何说明，下次启动又会得到一份可读的默认配置。

**版本更新后，文件里缺失的配置项会自动补全**：加载完用户文件后，逐块扫描
内置的默认配置模板，凡是文件里没有的键，就把"这个块里的注释行 + 缺失的键行"
追加到文件末尾（并加一段说明头）。因此：

- 用户的取值**一个都不会被动**（只追加、不重写、不排序）；
- 新增配置项自带完整注释，**不需要手工维护**；
- 键的先后顺序不影响解析（解析本来就与顺序无关）。

> ⚠ 桌面端实现上**不能用 `fs_get_write_path()`**：它返回的是 fs 的写入目录，而
> `fs_init()` 传的是 `sys_user_path()`（`%APPDATA%\sm64coopdx\`）——用它写会
> 写到 AppData 里，用户"启动后看不到配置文件"。桌面端用 `sys_exe_path_dir()`
> 自己拼绝对路径。
> Android 端**反过来**：那里没有"可执行文件所在目录"（程序在 APK 里），
> `sys_exe_path_dir()` 返回的是游戏数据目录、与 `sm64config.txt` 不同级，
> 所以改用 `fs_get_write_path()`（= `<游戏数据目录>/user`）。
> 两个分支都在 `djui_modern_config.c` 的 `config_path()` 里，用
> `#if defined(__ANDROID__)` 分开，**不需要额外补丁**。
>
> 同理，**游戏目录不在 fs 的搜索路径里**，所以读配置、
> 扫字体目录都必须走真实文件系统（`fopen` / `fs_sys_walk`），
> 不能用 `fs_open` / `fs_enumerate`。

全部是 `key=value` 纯文本，`#` 或 `;` 开头是注释。任何一项缺失都用默认值。
旧配置里已删除的键（`atlas_size` / `debug_*`）会被静默忽略，不影响启动。

### 形状

| 键                  | 默认     | 说明                                      |
| ------------------ | ------ | --------------------------------------- |
| `radius_panel`     | `12`   | 大面板圆角（看得见的圆角半径，320x240 虚拟坐标系；边框从这条轮廓向外长）  |
| `radius_element`   | `8`    | 普通控件圆角（同上，是内容一侧的可见半径）                   |
| `radius_pill`      | `-1`   | 胶囊元素圆角，`-1` = 高度的一半（按钮/滑条）              |
| `corner_roundness` | `0.90` | 圆角形状的"圆度" 0~1：1 = 正圆弧（最圆），0 = 全程羊角螺线（最饱满） |

半径上限被硬编码为 24（纵横两向相同，取短边一半是硬约束），  
想突破可以改 `djui_modern_rrect.c` 里的 `RRECT_MAX_RADIUS`。

`corner_roundness` 只改**形状**、不改半径（见"圆角轮廓"一节）：  
· `1.00` = 正圆弧（最圆，但直边→圆弧处曲率从 0 突变到 1/R）  
· `0.90` = **默认**，两端各 ≈4.5° 羊角螺线过渡（肉眼与正圆弧几乎一样，接缝更柔）  
· `0.75` = 过渡段各 11.25°，角部比正圆弧饱满 6.2%  
· `0.00` = 全程羊角螺线，最饱满（偏方，45° 方向比正圆弧鼓 11.7%）  

### 半透明

| 键               | 默认    | 说明                 |
| --------------- | ----- | ------------------ |
| `panel_alpha`   | `224` | 面板背景不透明度（0-255）    |
| `element_alpha` | `208` | 控件背景不透明度           |
| `overlay_alpha` | `176` | 覆盖层（聊天栏底、弹窗遮罩）不透明度 |

### 渐变

| 键                       | 默认     | 说明           |
| ----------------------- | ------ | ------------ |
| `gradients`             | `true` | 是否给面板做轻微上下渐变 |
| `gradient_top_boost`    | `1.18` | 顶部相对底色的亮度倍数  |
| `gradient_bottom_scale` | `0.86` | 底部相对底色的亮度倍数  |

渐变是在**矩形自身颜色**基础上做上亮下暗（不是换色），且是**免费的**：
上排/下排顶点使用不同顶点色即可，不增加任何绘制命令。不需要就设 `false`。

> 本模块**没有**"大矩形专属"的透明/渐变逻辑（曾试过按面积自动降透明度，
> 已按用户要求删除）。透明度只由上面三个值决定。

### 边框

| 键              | 默认     | 说明             |
| -------------- | ------ | -------------- |
| `show_borders` | `true` | 是否显示边框。`false` = 完全不画描边（更简洁的视觉效果）；`true` = 画**圆角**描边。<br>边框统一由本模块圆角绘制（原版方角边框已被收编阻止），所以打开时也不会出现"外方内圆"。<br>旧键 `keep_borders` 已废弃（原语义"保留原版方角边框"已无意义），若仍写了会按"显示边框"解释。 |

### 字体

| 键                       | 默认      | 说明                                    |
| ----------------------- | ------- | ------------------------------------- |
| `font_enabled`          | `true`  | 是否用 TTF/OTF 替换点阵字体                    |
| `font_path`             | 空       | 显式字体文件路径；留空则自动查找                      |
| `font_cjk_raster_boost` | `1.0`   | CJK 字形光栅尺寸倍率（**默认关闭**，见下方说明）         |
| `text_scale`            | `1.0`   | **全部字符**的字形光栅尺寸倍率（同上一项的全局版，**不改变排版尺寸**） |
| `font_bilinear`         | `true`  | 字形是否双线性过滤                             |
| `font_native_metrics`   | `true`  | 西文是否直接用字体原生字距（关掉会明显偏窄，见下）        |
| `font_fallback_kerning` | `true`  | 字体缺该对字偶间距时，用内置"常见西文对"兜底（见下）      |
| `font_slots`            | `0 3`   | 用 TTF 替换的字体槽（见下）                     |
| `font_player_list`      | `true`  | **玩家列表（PLAYERS 面板）**是否用矢量字体（见下）        |
| `font_mod_description`  | `true`  | **mod 列表 / mod 描述面板**是否用矢量字体（见下）       |
| `emoji`                 | `modern` | Emoji 版本：`modern` = 内嵌彩色图集；`vanilla` = 原版 32×32 单色贴图 |
| `show_stats`            | `false` | 打印字体统计                                |

> **关于 `font_player_list` / `font_mod_description`：**
>
> 这两个面板的文字节点原本是在模块初始化**之前**创建的（`djui_init()` 里
> `djui_panel_playerlist_create()` 早于 `djui_modern_init()`），拿到的是
> **原版字体指针** ⇒ 真机表现"**时而矢量字体、时而原版字体**"（重开面板时新建的
> 节点又拿到模块字体）。根因与修法见上方"为什么玩家列表时而矢量字体"一节 ——
> **把 `djui_modern_init()` 挪到 `djui_init()` 开头**即可根治。
>
> 这两项在此基础上决定那两个面板最终用哪一种：
>
> · `true`（默认）= 矢量字体；`false` = 原版点阵字体。
> · 生效时机：`djui_text_create()` 末尾上报一次（**每次创建一次，不是每帧**），
>   由模块按上面两个开关决定是否把 `text->font` 指回原版字体。
>   默认两项都是 `true` ⇒ 直接走"目标 = 模块字体"的最短路，**不做面板查找**。
> · 面板识别靠 `gDjuiPlayerList` / `gDjuiModList` / `gDjuiHostModsDescriptionPanel`
>   三个全局指针，**沿 parent 链比对**（不是控件类型枚举 ⇒ 上游加新控件不会出错）。
> · 其它字体槽（标题艺术字 / HUD / 自定义 HUD）**一律不碰**。

> **关于 `font_slots`：只替换"普通文字"两个槽。**
>
> | 槽 | 含义 | 默认 |
> |---|---|---|
> | 0 | 正文 / 聊天 / 按钮 / DJUI 文本 | **替换** |
> | 1 | FONT_MENU（DJUI 标题字体） | 原版 |
> | 2 | HUD（N64 经典 HUD 字形） | 原版 |
> | 3 | ALIASED（DJUI 锐利字体） | **替换** |
> | 4 | custom HUD | 原版 |
> | 5 | recolor HUD | 原版 |
> | 6 | special | 原版 |
>
> 1/2/4/5/6 都是原版自带的**变种艺术字 / HUD 字形**，Lua mod 常拿它们写标题；
> 换成矢量字体会丢掉原有艺术效果，因此**一律保持原版**。想改就往列表里加数字。
>
> ⚠ **崩溃屏始终使用原版字体**（不受 `font_slots` 影响）。
> `crash_handler.c` 渲染崩溃屏时**硬编码每字符 0.4em 的等宽步进**
> （`f32 charWidth = 0.4f;` + `create_dl_translation_matrix(..., 0.4f, 0, 0)`），
> 那个 0.4em 正好是**原版点阵字体**的字符宽度。换成比例字体会让字形落在
> 等宽格子里、宽窄不一（真机："崩溃屏是按等宽排的，但字体不是等宽的，
> 有的宽有的窄"）。
> 崩溃屏用的就是 `gDjuiFonts[0]`（正文槽），**没法靠"少替换某个槽"回避**。
> 实现：模块的四个字体回调都检查原版的 `gDjuiDisabled`
> （崩溃时由 `crash_handler.c` 置 true，且**只在崩溃时**置位），
> 一旦为真就整体交回原版字体 ⇒ 崩溃屏恢复等宽点阵，正常游戏完全不受影响。
>
> ⚠⚠ **槽位数组必须按"原版槽号"索引，不能用"已安装个数"。**
> `install_slot()` 写入 `sSlots[slot]`，而槽回调是按槽号生成的
> （`DECLARE_SLOT(N)` → `sActive = &sSlots[N]`），两边下标必须一致。
> 曾误用紧凑计数 `sSlotCount` 当索引 ⇒ 默认 `font_slots = 0 3` 时 slot 3 被写进
> `sSlots[1]`，而 `slot_begin_3` 仍指向**从未初始化**的 `sSlots[3]`：
> · `self == NULL` ⇒ 任何 `sActive->self->...` 读 **NULL + 偏移** ⇒
>   **启动即崩溃**（`DjuiFont.charHeight` 恰好在偏移 4，崩溃屏显示
>   "tried to read at address 0x4"）；
> · `legacyScale == 0` / `latinCalibration == 0` ⇒ 宽度标定失效 ⇒
>   真机表现"**字符宽度全一样、字形大小不一**"。
> 两个症状一个根因。凡是"按槽号生成的回调 + 运行时填充的表"，索引口径必须统一。

> **关于 `font_cjk_raster_boost`：默认 1.0（关闭），建议保持。**  
> 分档机制里已内置 `kGlyphSupersample = 1.25` 的恒定超采样（bitmap 严格大于
> 屏幕 em 再双线性降采样 = 标准抗锯齿），清晰度已由档位选择保证。本项是
> **在此基础上再乘一次**，清晰度收益很小，
> 而字形纹理面积按**平方**增长（1.15 时约 +32% 内存，1.75 时约 +191%）。
> 真觉得小字号汉字糊，优先调 `text_scale`，或只微调到 1.1~1.2。

> **关于 `font_native_metrics`：默认 `true`，建议保持。**  
> `true` = 西文**直接用字体自身的 advance + 字偶间距**，换任何字体都自动得到
> 该字体设计的正确字距。  
> `false` = 退回旧行为：把西文平均宽度对齐到**原版点阵字体**（其 ASCII 平均
> 仅 0.364em，是给 8×16 点阵量身定做的窄字距）。即便只补一半差距（×0.839），
> 思源黑体的 0.5373em 也会被压到 0.4506em —— 实测就是用户反馈的
> "**英文连成一片**"。除非你要刻意模仿原版窄字距，否则不要关。

> **关于 `font_fallback_kerning`：默认 `true`。**  
> **很多字体完全没有西文字偶间距数据** —— 实测霞鹜文楷（LXGW WenKai）的 GPOS
> 里只有 `chws` / `halt` / `vchw` / `vert` / `vhal` 这些 CJK 特性，**没有 `kern`**，
> 两个 PairPos 子表的覆盖率只有 2 / 34 / 28 个字形且 `T` 不在其中。此时
> `stbtt_GetGlyphKernAdvance` 恒返回 0，表现为"**To 的 o 不往 T 下面靠**、
> 整串字松散"——这是字体缺数据，不是 bug。  
> 打开本项后，会在**字体自身没有该对数据时**用内置的常见西文对兜底表
> （约 50 对，全部小幅负值），因此**绝不会覆盖字体自己的度量**。
> 不喜欢就设 `false`。

> **字符集：以"游戏要用的字"为准，而不是"字体里所有的字"。**  
> 构建顺序是 ASCII → 上游 `cn_glyphs.h` 的 **GB2312 全集** → ★ → 再尽力补齐
> 字体里其余的字。字符集因此被钉在 **~6900**，与字体多大无关。  
> ⚠ 早期实现直接扫 `0x4E00–0x9FFF`：内嵌子集字体（6986 字）装得下，但换成
> **完整 CJK 字体**（霞鹜文楷有 20992 个 CJK 字形）就会撞上 `CHARSET_MAX` 而在
> 码点 ≈0x6xxx 处截断 —— "间(U+95F4)/选(U+9009)/项(U+9879)"这类字全变成 `?`，
> 而"创/建/房/加/入"（码点更低）却正常。用权威字表优先即可根治。

> **已删除的旧键**：`atlas_size`（v4 起字形不再共享图集，早已失效）与
> `debug_plain` / `debug_paint` / `debug_force_env` / `debug_dump_atlas`
> （开发期诊断开关）。旧配置文件里若还留着，会被静默忽略。

> **注意**：字形是否真正走双线性过滤，最终由游戏全局设置  
> `configFiltering`（Options → Display → Texture Filtering）决定。  
> 关掉全局过滤时字形会走最近邻，字会显得硬一些，但界面仍然正常。

---

## 3. 换字体

### 优先级

```
1. 配置里的 font_path
2. 自动探测下列目录（**先找到先用**，文件名随便叫，只要是 .ttf / .otf）：
     <游戏根目录>/djui_modern/*.ttf, *.otf   ← 推荐：建个文件夹丢进去即可
     <游戏根目录>/*.ttf, *.otf               ← 直接放游戏根目录
   同一目录里有多个字体时，按**文件名升序**取第一个（结果确定，不会每次换）。
3. 内嵌**字体链**（默认，始终作为**回退**追加）：
     西文 Roboto Condensed 子集（ASCII + 标点，16KB）
     中文 Source Han Sans SC 子集（GB2312 6763 字，1.8MB）
   —— 按码点依次查，**取第一份含该字形的**。两者都是 SIL OFL 1.1。
```

> **外部字体是"链首"而不是"独占"**（2026-10-10 修正）。
> 外部字体注册在链的最前面，内嵌链**照常追加在后面**。所以：
> · 丢一个**纯西文等宽字体**（例如 Maple Mono，**不含汉字**）进去 ⇒
>   西文用它、**汉字自动落回内嵌思源黑体**；
> · 丢一个自带汉字的字体（霞鹜文楷）⇒ 中英文都走它，整体接管。
> 早期版本是"外部字体加载成功就不加载内嵌链"，代价是缺字直接画成 `?`。

**中文开箱即用**：内嵌字体已经包含 GB2312 全部汉字，不需要你另外下载或放置
任何字体文件。Emoji 也自带（内嵌彩色图集：**179 个 emoji + 250 余面国旗**，
`emoji = modern`）。

### 换成别的字体（可选）

在**游戏根目录**建一个 `djui_modern` 文件夹，把 TTF/OTF 丢进去即可
（文件名随意，例如 `LXGWWenKai-Medium.ttf`）；也可以直接放游戏根目录。
不用改代码也不用重新编译。外部字体是**运行期加载完整字体**，不受内嵌子集的范围限制。

### 重新生成内嵌字体

内嵌的是**字体链**（西文 + 中文）加一张 **Emoji 图集**，都由 `src/pc/djui_modern/tools/` 里的脚本
生成（可复现）：

```bash
# 1) 子集化：西文只要 ASCII+标点（16KB）；中文要 GB2312（1.8MB）
python src/pc/djui_modern/tools/make_subset_font.py build/RobotoCondensed-Regular.ttf \
        src/pc/djui_modern/font/RobotoCondensed-Subset.ttf --han none
python src/pc/djui_modern/tools/make_subset_font.py build/SourceHanSansSC-Regular.otf \
        src/pc/djui_modern/font/SourceHanSansSC-Subset.otf --han gb2312

# 2) 转成 C 头文件（第 4 个参数是宏前缀，多份字体必须不同）
python src/pc/djui_modern/tools/make_font_c.py src/pc/djui_modern/font/RobotoCondensed-Subset.ttf \
        src/pc/djui_modern/djui_modern_font_latin_data.h sDjuiModernLatinFont DJUI_MODERN_LATIN_FONT
python src/pc/djui_modern/tools/make_font_c.py src/pc/djui_modern/font/SourceHanSansSC-Subset.otf \
        src/pc/djui_modern/djui_modern_font_cjk_data.h sDjuiModernCjkFont DJUI_MODERN_CJK_FONT

# 3) Emoji + 国旗图集（一次生成两张；--size 默认 32，绝不要调大）
#    --flags 另给一份**带国旗**的字体（Noto-3D-128 本体没有国旗连字）
#    国旗头文件由 emoji 那个派生（emoji -> flag），见脚本输出
python src/pc/djui_modern/tools/make_emoji_atlas.py build/Noto-3D-128.ttf \
        src/pc/djui_modern/djui_modern_emoji_data.h \
        --flags build/Noto-3D-128-flagsonly.ttf
```

**为什么要子集化**：完整 CJK 字体 15~20MB，直接内嵌会让头文件膨胀到 60MB+
（C 数组约 4 倍），仓库和编译速度都受不了。子集化后合计约 1.8MB。

`--han` 可选 `gb2312`（6763 字，默认）/ `common`（3500 常用字，更小）/ `none`。
**必须放在最后**（否则它的值会被当成位置参数）。

> **Emoji 为什么要烘焙**：原版 emoji 字体（Noto-3D-128）只有 `sbix` 彩色位图、
> **没有轮廓**（`glyf` 仅 103KB / `sbix` 147MB），而 stb_truetype 完全不支持
> sbix/CBDT/COLR。所以只能把要用的 emoji 离线取出来拼成图集
> （**179 个**：原版 52 个 + 小黄脸 80 + 补充表情 49 + 骷髅/鬼/恶魔等 20 + 手势 30）。
> 配置项 `emoji = modern | vanilla` 可切回原版贴图。详见 `font/README.md`。
>
> **国旗怎么来的**：Unicode 国旗不是单个字形，而是"两个 Regional Indicator 字母"
> 的序列（U+1F1E6..U+1F1FF，如 U+1F1E8 U+1F1F3 = 中国），字体里靠 `ccmp` 下的
> **GSUB 连字**合成。Noto-3D-128 本体没有这些连字，所以 `--flags` 另收一份带国旗的
> 字体（`Noto-3D-128-flagsonly.ttf`），从它的**连字表**直接读出
> "哪两个字母 = 哪面旗"——**不需要字形名**，子集化把名字改成 `glyphNNNNN` 也无所谓。
> C 侧做前瞻合成：当前是 RI 且下一个也是 RI 且组合在表里 ⇒ 头画整面旗（1em）、
> 尾宽度 0；孤立 RI 不画。共 259 条连字 → **去重 251 面旗**（个别非 ISO 属地代码
> 与主代码共用同一面旗，按最小键保留一份）。
>
> **为什么 emoji 与国旗是**两张**独立图集**：N64 的 `Vtx.tc` 是 **signed 16-bit 的
> S10.5 定点**（纹素<<5），最大可寻址 1023 texel，POT 边长最大 512；单张 512×512
> （每格 32）最多 256 格，而 emoji(179) + 国旗(251) = 430 格装不下。强行用
> 1024 或 2048 宽的图集，右半部分 tile 的 UV 会溢出成负、被 CLAMP 到 0，渲染
> 错位（真机表现"一个表情变成两个、宽度各占一半"）。拆成两张 512×512 后
> 两张都远在安全范围内（max UV=16384 << 32767）。
>
> **图集为什么存 PNG**：裸 RGBA32 在 C 头里是 5 字符/字节，体积与编译时间都
> 扛不住。PNG 无损压到原始的 1/4 左右，两张图合计 4MB，编译 11 秒；
> 代价是启动时用 stb_image（`gfx_pc.c` 已编译了实现）解一次。
> 解码失败会自动把 emoji / 国旗整体交回原版，不会崩。

换字体前建议先跑探针确认能正常光栅化（尤其是汉字）：

```bash
gcc -Iinclude -o /tmp/fontprobe src/pc/djui_modern/tools/font_probe.c -lm
/tmp/fontprobe 你的字体.otf      # 看 result 行是否 0 failed
```

想完全不内嵌：删掉那几个 `*_data.h` 即可（latin / cjk / emoji / flag）。  
`djui_modern_font_atlas.c` 用 `__has_include` 逐个检测，缺失就跳过，  
此时没放外部字体的话会自动退回原点阵字体。

---

## 4. 调色

> **铁律：颜色一律以原版主题为准。** 模块只在原版色基础上做轻微现代化调整
> （冷色偏移 + 半透明 + 渐变），**绝不自己发明一套配色**。

原版主题的全部颜色都在 `gDjuiThemes[configDjuiTheme]`（`src/pc/djui/djui_theme.c`）：

| 主题字段 | 用在 |
|---|---|
| `interactables.defaultRectColor` | 按钮 / 输入框 / 滑条的常态底色 |
| `interactables.hoveredRectColor` / `cursorDownRectColor` | 悬停 / 按下 |
| `interactables.{default,hovered,cursorDown}BorderColor` | 对应状态的描边（原版绘制，模块不动） |
| `interactables.textColor` | 控件文字色 |
| `threePanels.rectColor` | 三段面板 / 弹窗底 |


亮/暗两个主题的差别全在这里：亮色主题 `defaultRectColor = {222,222,222}`，
暗色是 `{22,22,22}`；马里奥主题是 `{255,227,0}`，奥德赛是白、文件选择是黄绿。

### 主题是怎么套上去的

`djui_modern_theme_pass()` 每帧遍历一次控件树，**只写 `base->color`**：

- 有 `interactable`（自己或父节点）→ **可交互元素**：底色**实时取自当前主题**
  （常态 / 悬停 / 按下 / 禁用分别对应 `defaultRectColor` / `hoveredRectColor` /
  `cursorDownRectColor` / `defaultRectColor×0.6`）
- `color.a == 0` → 不动（原版要它不可见）
- 近黑（r,g,b < 40）且不透明 → **面板底** ← `threePanels.rectColor`
- 近黑且半透明 → **覆盖层**（聊天栏底、弹窗遮罩）
- 其余中性灰 → **浅色面**（滑条 / 进度条的填充 `{220,220,220}`）→
  **保留原版色相**，只做半透明化
- 有彩色 → 保留色相，只做半透明化
- **自绘底板容器**（三段面板 / 弹窗：`render` 不是 `djui_rect_render`，内部再调一次
  `djui_rect_render` 画自己那层底）→ **单独一个方案：只把 alpha 按 `panel_alpha` 等比缩放，
  RGB 与 gradient 一个都不碰**（见下一条坑）

判断依据刻意只用"原版自己设置的颜色"和"是否可交互"，**不依赖任何控件类型枚举**，
因此上游新增控件也不会出错 —— 最坏情况只是少一层主题色，仍然会画成圆角矩形。

### 几个曾经踩过的坑（勿回退）

0. **自绘底板容器绝不能用"元素"那套判据**。三段面板的父节点是 `gDjuiRoot`，而它
   **带 `interactable`** —— 于是"父节点有 interactable ⇒ 按元素上色"这条会把面板
   判成按钮，把面板涂成主题的**按钮底色**（马里奥主题下是亮黄 255,227,0）。
   真机表现：PLAYERS 面板整块变黄。容器现在走 `SCHEME_PANEL_ALPHA`（只改不透明度），
   判据是 `base_is_fill_container()`（非矩形渲染器 + 无 interactable + 有子节点 + alpha>0，
   **不依赖 comp**，隐藏时结论也一致）。

1. **绝不要把"中性灰"一律当成深色段落底**。原版的浅色面（滑条 / 进度条填充
   `{220,220,220}`、复选框中心块）都是中性灰，它们配的文字色是**深色**
   （`djui_slider.c:18` 的 `{18,18,18}`）。把填充染深 ⇒ 对比度崩掉、
   原版为它配的深色文字也失去意义。现在这一支**保留原色**。

2. **绝不要把按钮底色写死成模块自己的深色**。亮色主题的按钮底色 `{222,222,222}`
   同样是中性灰，写死会让"亮色主题永远不亮"。现在这一支**实时读主题**，
   切主题立刻跟着变（原版切主题会 `djui_panel_shutdown()` 重建面板，
   控件对象是新的，会重新登记）。

3. **alpha 必须按原版的 alpha 等比缩放，绝不能无条件写 `elementAlpha`**。
   原版给不同控件的 alpha 含义不同：

   | 控件 | 原版 alpha | 含义 |
   |---|---|---|
   | 按钮 / 输入框 | 255 | 实心底 → 缩放到 `elementAlpha`（现代半透明） |
   | 滑条 / 进度条的**轨道** | 0（常态）/ 32（悬停·按下） | **淡淡的叠加高光**，不是实心底 |

   无条件写 208 会把轨道变成近乎不透明的白/灰实心板：亮色与奥德赛主题下
   与它上面"浅底深字"的文字撞成一片白（用户："辨识度成了灾难"）；
   又因为 `djui_interactable_on_hover()` **每帧**都会跑一遍
   `update_style`（`djui_interactable.c:499`），原版每帧把 alpha 设回 0/32，
   模块每帧写回 208 ⇒ **指针停在滑条左侧时有背景/无背景剧烈闪烁**
   （离线复刻：写入序列 `[208,208,208,0,208,0,…]`，12 次可见性翻转；
   改成等比缩放后是 `[26,26,26,0,0,0,…]`，只有 1 次正常过渡）。

4. **"全透明填充"的矩形仍可能要画描边**。滑条 / 进度条的轨道在原版里就是
   `{0,0,0,0}` + 2px 描边，而描边已被 `frame_border_takeover` 收编
   （`borderWidth` 清零、原版不再画）。`djui_modern_rect_override()` 里那个
   `if (!hasFill) return true;` 必须改成"**先画收编来的圆角描边再返回**"，
   否则轨道连轮廓一起消失（用户："滑条不点按背景不见了，点一下才恢复"）。

5. **`draw_framed` 不能是"整块边框色 + 内缩内圈填充"**（上游
   `djui_base_render_border` 的画法）。那个画法只在填充**不透明**时才成立：
   填充是半透明时内圈盖不住底下那层边框色，边框色会从填充下面透上来一起
   混合。面板正是半透明的（填充 alpha 由 `panel_alpha` 缩放，边框 alpha 是
   原版值），于是：
   - 马里奥主题 蓝底 `(76,116,201)` + 红框 `(255,82,82)` → `(107,95,151)` 紫；
   - 奥德赛主题 红底 `(247,0,37)` + 白框 `(255,255,255)` → 苍白粉。
   正确画法是**先铺填充色、再沿轮廓画一圈环带**（`rrect_emit_band`）：
   外轮廓取解析角曲线（与填充遮罩同一条 `rho` 表，严格贴合矩形），内轮廓是
   外轮廓沿法向内缩边框宽度，连成闭合三角带 —— 法向厚度恒等于边框宽度，
   直边与圆角粗细一致。环带中间是空的，边框色只落在四周一圈；边框画在
   填充**之上**，半透明边框与底下填充混合，这正是原版半透明边框的观感。
   半径口径见下面第 6 条。也不要用"整块环带遮罩 + 九宫格铺"，正方形遮罩的
   角区铺到屏幕后弧线处会偏厚，圆角看着比直边粗。

   环带还要**两侧各加一圈 1px 的 alpha 羽化**（`RRECT_BAND_FEATHER`）：填充的
   边缘来自遮罩双线性（自带抗锯齿），而环带是三角形拼的、没有抗锯齿，两者的
   圆角放在一起就是"填充平滑、边框有阶梯"（用户实测报过："矩形的圆角非常平滑，
   而边框的圆角有锯齿感和像素感"）。羽化把实心部分两端各让出 0.5px，外面再铺
   1px 的渐变带（靠轮廓满不透明、外侧透明），50% 点仍落在原轮廓上，所以边框
   的视觉宽度与位置都不变。羽化的透明端允许探出 comp 半个像素（抗锯齿本来就要
   跨边界），但实心部分仍收拢在 comp ∩ clip 内。

6. **配置里的半径是"看得见的圆角半径"，边框向外长。**  
   环带外轮廓半径取 `半径 + borderWidth`、与矩形两条边相切，填充按边框
   内沿（内缩 `borderWidth`、半径仍是配置值）画。反过来把外轮廓放在半径
   上（边框向内长，等同 CSS 的 `border-radius` 语义）时，可见圆角只剩
   `半径 − 边框宽度`：`radius 12` 配边框 8 的面板只剩 4 的圆角，看上去
   几乎和直边一样；而且内轮廓在直边附近贴住矩形边、到 45° 才弯，圆角处
   会显得比直边窄。截图实测（1080p·x1.5）：改前可见圆角只有约 6px、
   边框内沿在圆角处塌成直角；改后可见圆角 = 18px、环带法向厚度恒为 12px。
   填充的内缩量要比 `borderWidth` 少约半个纹素（`0.5 * (r+bw) / 17.5`），
   否则填充边缘的抗锯齿过渡与环带内沿之间会留一条发丝缝。

### "现代化"体现在哪

`modern_tint()`：暗面抬一点亮度 + 偏冷（蓝多一点），亮面只做极轻微冷偏移。
**不改变明暗关系**，所以原版为各控件配好的文字色始终成立、对比度不被破坏。
近黑面板（亮/暗主题都是纯黑）换成模块的深蓝灰 `{20,23,30}` 保留层次感；
主题自带的**彩色面板**（马里奥蓝、奥德赛红、文件选择黄）一律跟随主题。

如果上游改了默认配色，只有 `color_is_theme_element()` 的匹配容差
（`COLOR_MATCH_TOLERANCE`）需要留意；方案本身不再依赖任何硬编码色值。

---

## 5. 性能特征

| 项目     | 开销                                                                                                      |
| ------ | ------------------------------------------------------------------------------------------------------- |
| 圆角矩形   | 24 顶点 / 9 四边形 / 18 三角形，1 次 `G_VTX_EXT` + 9 次 `G_TRI2_EXT`。与方角矩形（1 次 `G_VTX_EXT` + 1 次 `G_TRI2_EXT`）同数量级 |
| 带边框者   | 再多一圈三角化环带：98 顶点（49 外 + 49 内），按 64 顶点上限分 3 批提交，约 49 次 `G_TRI2_EXT`。只在 `show_borders` 打开且有边框时发生 |
| 圆角遮罩纹理 | 64x64 RGBA32（16 KB），生成一次，上传一次（地址恒定 → 命中 gfx_pc 的地址缓存）                                              |
| 顶点内存   | 编译期静态模板 + 每帧复用的固定顶点池（1536 块 × 24 顶点 = 36864 顶点，约 576 KB），**零 malloc**                                          |
| 字形光栅化  | 只在字形**首次出现**时做一次；按"光栅像素高度"分档（12/16/20/26/32/40/52/64/88/128）                                            |
| 字形绘制   | 每字形 1 次 `G_VTX_EXT`(4) + 1 次 `G_TRI2_EXT`                                                               |
| 纹理绑定   | 每个字形绑自己那张纹理（8+2 条 DL 指令）。同一纹理**连续**出现时跳过重绑，所以常见字重复连排几乎零额外开销                              |
| 每帧主题遍历 | 递归遍历控件树，每控件约 5 次写入。500 个控件 ≈ 2500 次写入/帧                                                                 |
| 额外内存   | Σ 字形纹理（每字形一张 2 的幂 RGBA32，边长 ≤ 256；常规会话约 10MB 量级）；槽位表 ≈ 7000×10×56B = 3.9MB；顶点池 36KB + 9KB              |


### 纹理预算与"用满了怎么办"

- 每个 `(字符, 档位)` 光栅化后生成**自己的一张** 2 的幂 RGBA32 纹理（边长 ≤ 256）。
  生成后字节**永不再改**，所以 `gfx_pc` 的指针缓存天然正确（详见 §1 的说明）。
- 总预算上限 `GLYPH_BYTES_LIMIT = 128MB`（`.c` 里的常量）。超过后不再生成新字形，
  打一次 ERROR，该字形本帧不绘制。**已生成的字形不受影响** —— 失败面收敛为
  "本帧少画这一个字"，绝不影响其它字。
- 实测容量参考：ASCII 全 10 档 ≈ 95 × 10 张（最大 128² 档约 64KB，多数在 16²~40²）  
  ≈ 1MB 量级；常用汉字数百字 × 1~2 档 ≈ 数 MB。**常规会话远不会触顶。**
- 想更省内存：把 `font_cjk_raster_boost` 保持在 `1.0`（默认），它按**平方**影响面积。

---

## 6. 文件结构

```
src/pc/djui_modern/
├── djui_modern.h             公共接口（init / pre_render / shutdown）
├── djui_modern.c             主模块：控件树遍历、主题色、圆角矩形转发入口、圆角渲染
├── djui_modern_config.h/.c   配置读取（缺失时自动生成；版本更新时自动补全缺失项）
├── djui_modern_rrect.h/.c    圆角矩形原语（遮罩生成 + 九宫格 + 顶点池）
├── djui_modern_gbi.h         与 N64 无关的 GBI 辅助宏
├── djui_modern_font.h/.c     把 TTF 接进 gDjuiFonts[]（度量标定：西文只补一半、CJK 不标定）
├── djui_modern_font_atlas.h/.c  字形光栅化 + 每字形一张不可变纹理（按需光栅、字偶间距）
├── djui_modern_font_latin_data.h  内嵌**西文**字体（Roboto Condensed 子集，16KB）
├── djui_modern_font_cjk_data.h    内嵌**中文**字体（Source Han Sans SC 子集，1.8MB）
├── djui_modern_emoji_data.h       内嵌**Emoji** 图集（512×512 PNG，179 emoji）
├── djui_modern_flag_data.h        内嵌**国旗**图集（512×512 PNG，251 国旗；与 emoji 图集共用布局）
└── font/
    ├── RobotoCondensed-Subset.ttf   西文子集（ASCII + 标点）
    ├── SourceHanSansSC-Subset.otf   中文子集（GB2312）
    ├── Roboto-Variable.ttf   早期 Latin 字体，现在仅作备选保留
    ├── OFL.txt               SIL Open Font License 1.1 全文
    ├── README.md             字体来源与授权说明
    └── METADATA.pb           上游元数据
├── tools/                    离线工具（构建时不需要）
│   ├── make_font_c.py        字体 → C 头文件
│   ├── make_subset_font.py   完整 CJK 字体 → 子集化（16.5MB → 1.8MB）
│   ├── make_emoji_atlas.py   emoji / 国旗 sbix 位图 → PNG 图集 → C 头文件
│   ├── make_patches.py       生成 patches/（从"未修改源码"与"已安装源码"求差）
│   └── font_probe.c          独立验证：字体能否被 stb 打开、汉字能否光栅化
├── patches/                  上游改动补丁（由 tools/make_patches.py 生成）
│   ├── NN-<文件>.patch       桌面 / 通用
│   ├── NN-<文件>.android.patch  Android 专用变体（仅 Makefile 与 djui.c 需要）
│   └── markers.txt           "已应用"判据 + 适用平台（安装脚本据此保证幂等）
├── install.sh                把本模块装进一份 sm64coopdx 源码树（桌面平台）
└── install-android.sh        同上，Android 分支（补丁变体与配置文件位置不同）
```

---

## 7. 验证方式

本模块现在**不含任何机器码改动**，所以没有需要单独单元测试的"打补丁"逻辑。  
验证方式是：

1. 用与构建完全一致的 include/flags 逐文件编译成 `.o`（`-Wall -Wextra` 零告警）；
2. 游戏内实跑观察外观 + 控制台日志。

**踩过的坑（供以后排查参考）**

- `Vtx_t.ob[]` 是 **`float`**（`include/PR/gbi.h`），不是 N64 的 `short`。  
  顶点坐标必须直接写浮点值；写成 `(s16)` 会把 0.1 这类 em 系数截断成 0，  
  导致四边形退化成零面积 —— 表现为**文字完全不显示 / 按钮不显示**。  
  （曾两处各犯一次：字形渲染、圆角矩形网格。）
- **遮罩角半径 `RRECT_CORNER_TEXELS` 必须远小于遮罩边长的一半。**  
  曾设成 32（= 64/2），圆弧在中线相切，遮罩本身变成一个**圆形** ——  
  所有矩形画成椭圆色块、上边缘 alpha≈0。正确比例 ≈ 28%（当前 18 / 64）。  

- **遮罩分辨率只影响"锯齿感"，不影响形状。**  
  角部 18 texel = 这条 90° 弧的 18 个采样点，放大后能看到 texel 台阶；
  想更细腻就把 `RRECT_MASK_TEXELS` / `RRECT_CORNER_TEXELS` 同时放大（保持 ~28%）。  
  UV 断点全部由这两个常量推导（`RRECT_U_FIRST` / `RRECT_U_CORNER` /
  `RRECT_U_STRAIGHT` / `RRECT_U_FAR`），**不要再写死 2048/2047** ——
  改尺寸时会静默错位。注意 `RRECT_MASK_TEXELS` 放大到 128 时
  `gDPSetTextureOverrideDjui` 的 log2 参数要同步从 6 改成 7。  
  已用离线仿真（Python 复刻算法 + ASCII 渲染）验证过"角半径过大会变圆"：
  ```
  ......########......     <- 旧 (corner=32): 整个是圆
  .####..      ..#####.
  ##              ####
  ```

- **九宫格 UV 断点必须是常量**（列 `{0,512,512,1536,1536,2048}`），  
  不能按 `1024*(r/w)` 动态缩放：半径小时角区只采样到遮罩左上角的  
  alpha=0 区域，按钮会整只消失。动态的只有顶点的**位置**断点（`r/w`）。
- **九宫格必须是 4 行**（`0, ry, h-ry, h`）：3 行缺 `h-ry` 行时底部两角  
  几何错误，而且中间带缺少填充格。
- 圆角矩形/字形模板必须显式 `G_CYC_1CYCLE`。若继承了 2 周期，  
  `G_CC_MODULATEIA` 会被算两次（`TEXEL0×SHADE×SHADE`），颜色变成 `C²/255` 而偏暗。
- 字体渲染**绝不能**在 `render_begin`/`render_end` 里设置 `Prim/EnvColor`：  
  文字颜色由 `djui_text` 通过 `ENV` 传入（`G_CC_FADEA` = `TEXEL0 × ENVIRONMENT`），  
  覆盖它会让所有文字变成白色（或全丢失）。
- **主题遍历只允许碰"自己画矩形填充"的节点** —— 两类：
  `base->render == djui_rect_render` 的矩形节点（走常规配色方案），以及**自绘底板容器**
  （`djui_three_panel` / `djui_popup`：render 是各自的 `xxx_render`，内部再调
  `djui_rect_render(base)`；判据 `base_is_fill_container()`，走 `SCHEME_PANEL_ALPHA`）。
  漏掉后者时**主面板底色永远走原版值** ⇒ cfg 的 `panel_alpha` 怎么看都没效果。
  ⚠ 容器**不能**套用"元素"判据：它的父节点 `gDjuiRoot` 带 interactable，会被判成按钮、
  涂成按钮底色（马里奥主题下整块变黄）。  
  文字节点的 `color` 是**文字颜色**、图片节点的 `color` 是染色层，  
  无差别"主题化"会把 `{220,220,220}` 的浅色文字改写成深色 → 按钮文字全黑消失。
- ⛔⛔ **圆角轮廓 = 羊角螺线 + 正圆弧 + 羊角螺线；`rho(θ)` 表必须从积分出的
  曲线采样，不能自己编一条解析式。**  
  这是 2026-10-10 花了几轮才收敛的**几何问题**，务必看懂。

  **曲率 k(s) 沿弧长的形状**（一个角共转 90°）：  
  ```
       k
   1/R |         ,-----------.
       |       ,'             `.
       |     ,'                 `.
     0 |___,'                     `.___
       +---+-----------------------+---+--> s
          L1          L2           L1
  ```
  · 段 1/3（羊角螺线）：k 从 0 **线性长到** 1/R、再线性回到 0，各转过 α  
  · 段 2（正圆弧）：k 恒为 1/R，转过 π/2 − 2α —— **这一段就是标准圆角，最"圆"**  
  过渡段长度由配置 `corner_roundness`（0~1）映射：`α = (1 − roundness) · π/4`，
  **默认 0.90**（过渡段各 ≈4.5°，中段 81° 正圆弧）。

  **为什么是这个**（几次真机教训都在这一个角上）：  

  | 方案 | 端点曲率 | rho(45°) | 真机表现 |
  |---|---|---|---|
  | 极坐标解析式 `1+amp·sin²` | 0.24（≠0） | 1.095 | "上边缘突然开始斜切、接不上" |
  | 超椭圆 N=4 | 0（连续） | **1.189** | **"不够圆"** |
  | **本方案（0.90）** | 0（连续） | **1.029** | 圆，且接缝无突变 |

  · **自己编的极坐标解析式不行**：`P(θ) = r(θ)(cosθ, sinθ)` 的切向量是  
    `P' = r'·(cosθ, sinθ) + r·(−sinθ, cosθ)`；在 θ=0 处 `P' = (r', r)`，
    要切于**水平**的上直边需要 `r' → ∞`（不可能）⇒ 曲率从 0 跳到 0.24
    ⇒ 看起来"突然开始斜切"。**`rho' = 0` 只保证"径向不变化"，
    与"和直边相切"毫无关系**。  
  · **超椭圆虽然端点曲率连续**，但中段**比圆弧更方**（45° 处 1.189R）⇒ 不够圆。  
  · **本方案**：中段是**正圆弧**（45° 处 = R）⇒ 圆；两端曲率从 0 连续长起来
    ⇒ 与直边光滑相接、无折角、无曲率突变。这就是"羊角螺线圆角"的标准构造
    （Apple 的连续圆角同理）。

  `rho` 表由 `rrect_build_corner_lut()` 在**启动时数值积分**生成（羊角螺线没有
  初等闭式，要用 Fresnel 积分），按 θ ∈ [0, π/2] 均匀采样 256 点。
  采样前要先把积分曲线**缩放平移成规范形**（角点在 (1,1)、切点在 (1,0)/(0,1)）：
  角点不在极坐标原点上，直接对积分曲线取极径会让 `rho(θ)` 不关于 45° 对称，
  四个角各歪一个方向。规范化后 `rho(θ) = rho(π/2 − θ)` 严格成立、端点精确为 1。
  形状与半径**解耦**（表里存的是"归一化半径"）⇒ 同一张遮罩服务任意半径。  
  ⚠ 角部分支用的是"径向距离" `|q| − rho(θ)·r`，只有对正圆弧是严格距离；
  羊角螺线段上是近似 —— 但**零等值面（边界）是精确的**，只影响一两个 texel
  的抗锯齿柔和度，肉眼看不出。  

- ⛔ **九宫格的 UV 断点按"纹素序号"写，别自己加半纹素，也别让窗口越过纹素中心。**  
  顶点 UV 到采样位置的换算由 gfx_pc 定：`gfx_sp_tri1` 拼 vbo 时对开了线性
  过滤的纹理**额外补了半个纹素**（`u += 0.5f`），于是

  ```
  采样位置(纹素) = tc/32 + 0.5      ⇒  texel i 的中心恰好对应 tc = 32 * i
  ```

  字体图集与 emoji 图集就是直接写 `texel << 5`，同一个口径。自己再加 0.5 纹素
  （曾经写成 `RRECT_U_FIRST = 16`）等于把整块填充推出去半个纹素：角弧的切点
  落到矩形边外 `0.0286r` 处、填充角半径看起来大 2.86%，而且肩部被窗口切平。
  正确的断点是 **`0 / 17.5 / 45.5 / 63`（纹素）**，采样窗口外沿正好落在纹素
  0 与纹素 63 的中心上。
  离线自检（`rrect_g2_refactor.py` 第 6 节）把这三条一致性钉死：
  窗口外沿的 `|px|` 必须等于遮罩零等值面的 `inner + ARC`（= 31.5）、直边区
  的纹素数必须等于 `MASK − 2 * CORNER`（= 28）、角区的纹素数必须等于 `ARC`。

- ⛔ **遮罩 SDF 的角弧半径取 `RRECT_CORNER_TEXELS − 0.5`，不是 `RRECT_CORNER_TEXELS`。**  
  角弧半径取 18 时切点落在纹素边界 0 上：屏幕上的填充角弧被放大
  `18/17.5 ≈ 2.86%`，而且过渡肩部被采样窗口切平（肩部露出一小段"突然开始
  斜切"的折角）。取 17.5 后切点恰好落在采样窗口外沿（纹素 0 的中心）上，
  屏幕上的填充角弧**就是** `rho(θ)·r`、切点恰在矩形边上；边框环带的外轮廓
  因此可以直接取同一条曲线，不需要任何折算系数。
  离线量化（`rrect_g2_refactor.py` 第 2 节）：改前填充边缘比解析曲线鼓
  +0.27~+0.86px（随 r 线性放大），改后偏差 ≤0.18px（纯采样噪声）。
  副作用一条：直边的外沿此刻正好压在最外一圈纹素中心上（覆盖率 50%），
  所以**填充的直边也变成约一个纹素宽的抗锯齿过渡**（以前是硬边）。带边框时
  这一段整个落在环带底下，看不到；无边框（`show_borders = false`）时表现为
  边缘更柔和，位置仍然精确在矩形边界上。

- **边框环带内轮廓要沿曲线法向内缩，不是沿"极点方向"。**  
  极点（角弧圆心）方向与曲线法向在羊角螺线段上差 7°~11°（roundness 0.9~0），
  沿极点方向内缩会让圆角处的法向厚度窄 0.85%~2%。法向由相邻轮廓点的中差
  切向旋转 90° 得到，方向用"指向矩形外"校正（与矩形中心做点积定符号）。
  内轮廓必须从**收拢进裁剪区之后**的外点算起：父级裁掉一条边时外点会被切
  到裁剪区边界上，内点若从未收拢的外点算起，被裁那几条边的厚度就偏，
  四个角却是满的 —— 就是"边框粗细不一"。离线复刻（φ 扫 0°→90°）厚度恒为
  `bw`。

- **`rrect_emit_band` 的顶点数上限**：整个环带 49 个点（内外各 49，共 98 个
  顶点），而 `src/pc/gfx/gfx.h` 的 `MAX_VERTICES` 是 64，一次
  `gsSPVertexNonGlobal` 最多带 64 个顶点 ⇒ 必须分批（18+18+16 个点/批，
  接缝点两批共用），不是"一次画完"。`RRECT_BAND_POINTS = 4 * SEG + 1`
  （首角保留起点、其余角跳过与上角重合的起点），不是 `4 * SEG`；算错会越界
  写顶点池。

- **`GLYPH_TEX_MAX` 只能到 512**：`Vtx.tc` 是 S10.5 定点（纹素<<5），  
  1024<<5=32768 溢出为负 ⇒ 纹理/UV 全乱。512<<5=16384 安全。  
- **`pot_ceil` 钳位后必须重查 `tw<bw`**：`pot_ceil` 上界被钳在 `GLYPH_TEX_MAX`，  
  若字形位图本身大于 512，返回的 `tw` 可能**小于** `bw`，按 `bw` 逐行拷贝会**越界写**。  
  判据须为 `tw < bw || th < bh || tw > MAX || th > MAX`。  
- **字距太窄的确切根因 = 把为点阵字体设计的窄字距套到现代字体上**：  
  `widthCalibration = legacyAvg/modernAvg ≈ 0.677`（ASCII 样本），
  CJK（advance 恒 1.0em）被压窄 32%（"汉字挤成一堆"）；即便只补一半
  （`latinCalibration ≈ 0.839`）西文仍偏窄（"英文连成一片"）。  
  **修法：默认 `font_native_metrics = true`，西文/CJK 全用字体原生 advance。**  
- **定位"幽灵矩形"**：`djui_rect_render` 是全工程唯一填充点，任何残留填充必属某个  
  `DjuiBase`。开 `show_stats=true` 后 `djui_modern_rect_override` 会对"面积较大"的  
  填充打印节点地址 + 颜色 + 尺寸 + 整条祖先链（含 `chatContainer` / `console` /  
  `root` 等已知身份），一眼定位归属。
- **透明容器不能画阴影**：全屏透明布局矩形若照常加投影，会在屏幕中央  
  留下一块巨大的半透明色块。渲染函数必须对 `color.a == 0` 直接早退。
- 纹理绑定的正确性：本模块的做法是**每个字形绑自己那张不可变纹理**（见下）。  
  全程不需要改动 `gfx_pc.c` 一个字符。
- **weak 属性会传染 —— 声明上绝不能加。** 若把 `__attribute__((weak))` 写在头文件  
  的声明上，所有经 `djui.h` 包含它的 TU 里同名定义都会变 weak，包括 djui_modern  
  侧本该强符号的定义；链接器在两个 weak 中任选（实测选中原版空实现）→ 接管  
  **静默失效**：没有任何编译/链接告警，界面就是完全没变化。用 `nm <obj>` 确认端到端  
  行为：原版 `.o` 里应为小写 `w`，模块 `.o` 里应为大写 `T`。若两边都是 `w`，  
  说明 weak 污染了，检查声明处有没有误加属性。

### 纹理绑定策略（v4：一字形一纹理，内容不可变）

```
字形光栅化 → 该字形独占的一张 POT RGBA32 纹理 g->tex（生成后字节永不改）
                │
                └─ render_char() 时（若与上次绑定的不是同一张）：
                       gDPLoadTextureBlockWithoutTexture(..., g->tw, g->th)
                       gDPSetTextureOverrideDjui(g->tex, twLog2, thLog2, ...)
                       gsSPExecuteDjui(G_TEXOVERRIDE_DJUI)
```

- 绑定命令只有 8+2 条 DL 指令；同一纹理**连续**出现时直接跳过（`sBoundTex` 比较），  
  所以"的的的"这类连排常见字几乎零额外开销。
- `render_begin` / `render_end` 都**不清空** `sBoundTex` 之外的状态；每次
  `render_begin` 会把它重置为 `NULL`，因为上一段文本与本段之间可能画过使用
  其它纹理的几何（圆角矩形遮罩等）。
- **为什么不能退回"共享图集 + 内容变了重传"**：`gfx_pc` 的纹理缓存以数据**指针**  
  为键，且一个地址只在第一次出现时插入链表头、此后永远命中、命中即跳过上传。  
  所以共享图集在内容更新后**永远显示第一次上传的那一版** —— 真机表现就是  
  "只有最开始那几个字形能看见"。让内容不可变是从结构上根治，见 §1 的完整推导。
- `gDPSetTextureOverrideDjui` 的 `(w, h)` 是**边长的 log2**（`gfx_pc` 内部 `1<<v`），  
  所以字形纹理边长必须取 2 的幂（`pot_ceil`）；而 `gDPLoadTextureBlockWithoutTexture`  
  里的 `(tw, th)` 是**纹素数**，两者必须一致，否则 UV 归一化基准错位。

> 之所以不做"给 `gfx_pc` 加 invalidated 字段"：那要求改 `gfx_pc.c`，而它的纹理池是  
> **环形复用**的 —— 手动解链会在池满覆写时留下悬垂 `next` 指针。  
> "内容不可变 + 逐字形绑定"完全靠原版既有行为工作，是**不动后端**的前提下最稳的解。

---

## 8. 已知限制 / 后续可做

- **平台无关**：不依赖任何机器码 hook，x86 / x86_64 / ARM64 均可。
- **加载画面（loading screen）不受影响**：它在 `djui_init()` 之前就建好了控件，  
  那时本模块还没启用。这是刻意的（更安全），但也意味着 loading 画面仍是原版外观。
- **不挂在 `gDjuiRoot` / `gDjuiConsole` 下的矩形控件不会被现代化**  
  （例如上游将来新增的独立 UI 根）。它们**仍然正常显示**，只是保持原版方角外观。  
  要覆盖新根：把它的指针或 extern 加到 `djui_modern_theme_pass()` 里。
- **复选框保持原版样式（模块完全不接管）**：  
  识别到复选框的外框 / 中心块后，`djui_modern_rect_override` 直接返回 `false`
  把绘制交回原版，同时 `apply_theme` 不改它的任何颜色、
  `modern_theme_and_adopt` **不做边框收编**（收编会清零 `borderWidth`，
  原版那条方角描边就没了）。所以复选框是"原版方框 + 原版灰/蓝配色"。  
  · 识别只靠**几何 + 结构**：自身无 interactable、父有 interactable、
    小正方形 ≤48、**有且仅有 1 个非交互子节点**。不 include `djui_checkbox.h`。  
  · ⚠ **不能读中心块的 `comp`** —— 它未选中时 `visible == false`，而 `comp`
    只在 `djui_base_render` 里算，那里对不可见节点**直接早退**，于是它的 `comp`
    恒为 0（真机表现："不点时颜色不对、点一下才对"）。  
  · ⚠ **不能用 `borderWidth`** 做判据 —— 边框收编会清零它。  
  · ⚠ 首帧 `comp` 还是 0（几何判据认不出来）：中心块会被误判成"中性灰段落底"
    染成深色（真机："刚点开界面里面是暗的，鼠标扫过才恢复"）。
    修法是**自愈** —— 识别到复选框时，若此前被主题化过，用登记时记下的原版
    颜色与 **`gradient`** 还原并把方案清空；同时 `base_wants_rounded_border`
    在 `comp` 无效时**不做决定**，避免首帧收编清零 `borderWidth`（方框描边会闪一帧）。  
    ⚠ 还原 `gradient` 时**必须用登记时记下的原版值**，不能写死 `false`：
    原版 `djui_base_init` 会把它设为 `configDjuiThemeGradients`（默认 true），
    `djui_rect_render` 据此选 `dl_djui_menu_rect`（带渐变）还是 `dl_djui_simple_rect`。
    写死 `false` 会把复选框中心块的**原版渐变**弄没。
- **"幽灵圆角矩形"的根治（两条一起才够）**：  
  原版 `djui_rect_render` 只要 `base->color.a > 0` 就会画（alpha=0 时画了也看不见），
  所以"我们画出来、原版却不画"的矩形，必然是**我们写入了非零 alpha**的节点。  
  1. **登记前先判 `color.a == 0`** —— 否则原版明明全透明的矩形（例如聊天栏那类
     "用 alpha 当显隐开关"的节点）会被登记成主题方案，此后每帧都被画出来。  
  2. **`apply_scheme` 的 alpha 变化检测必须覆盖 `SCHEME_ELEM`**（早期把它排除了，
     理由是"元素颜色每帧现算"）—— 那留下唯一的口子：ELEM 分支会**无条件**写入
     `elementAlpha`。这条检测同时也是识别"**base 地址被复用**"的唯一手段：
     面板 / 聊天栏销毁后重建时，新节点极可能拿到刚释放的地址并命中旧条目，
     而它的 alpha 是原版刚设的值（通常 0）≠ 我们上次写入的值。  
  3. 另加一道指纹：主题表条目同时记录 `base->parent`，`themed_index_of` 要求它
     匹配 —— 跨父节点的地址复用会被识别成新节点并重新分类。  
  真机表现：一开 `show_stats` 反而复现不出来（时序一变就错过那一帧）。
- **艺术字 / HUD 字形槽（1/2/4/5/6）一律保持原版**：  
  这些是原版自带的变种艺术字（"SUPER MARIO 64" 描边字、N64 HUD 字形等），  
  Lua mod 常拿它们写标题。换成矢量字体会丢掉原有艺术效果，故**不替换**。  
  想替换就往 `font_slots` 里加槽号。
- **度量标定默认不生效（`font_native_metrics = true`）**：西文**直接用字体原生  
  advance + 字偶间距**，换任何字体都自动得到该字体设计的字距。  
  背景：原版点阵字体 ASCII 平均仅 0.364em、现代 UI 字体约 0.53~0.54em。  
  早期把西文对齐到原版（`latinCalibration = 1-(1-calib)*0.5 ≈ 0.839`），  
  但仍比原生窄 ~16%（实测 0.5373 → 0.4506em），用户判为"英文连成一片"。  
  故默认改为完全用原生度量；把 `font_native_metrics = false` 可切回旧行为。  
  **CJK 任何模式下都不做标定**——现代字体 CJK advance 恒 1.0em，本就正确；  
  曾把 0.677 的西文标定无差别乘到 CJK 上，导致汉字被压窄 32%（"挤成一堆"）。  
  见 `djui_modern_font.c` 的 `slot_char_width` 与 `kLatinCalibRelax`。
- **字距 / 字偶间距（kerning）**：`char_width(c)` 返回"画完 c 之后光标推进多少"，
  所以它必须算出 `kern(c, **下一个**字符)` 并加进返回值 —— 见
  `djui_modern_font_atlas.c` 的"字偶间距"一节。  
  ⚠ 早期实现是"记住上一个字符、把 `kern(prev,cur)` 加到 `cur` 上"，**差了一位**：  
  kern 被算到下一对之间，字形位置整体错开。真机表现就是用户报的  
  "**Tofu 的 o 不在 T 下面、整串字松散**"。现在改为**前瞻一个字符**（`c + 字节数`），  
  纯函数、无状态，测量与渲染天然一致，也不再需要 `sKernActive` 那套开关。  
  kern 数据由 `stbtt_GetGlyphKernAdvance` 提供：字体有 GPOS 时走 GPOS 的  
  Pair Adjustment（LookupType 2，format 1/2），否则退回 legacy `kern` 表。  
  思源黑体子集只有 GPOS，该路径已实测可用（`kern('To') = -74/1000 em`）。  
  字体**没有**该对数据时，再由 `font_fallback_kerning` 的兜底表补上（见上方 cfg 说明）。
- **字形纹理内存**：Σ 每个 `(字符, 档位)` 一张 POT RGBA32。  
  **单字形纹理边长上限 = 512**（`GLYPH_TEX_MAX`；`Vtx.tc` 是 S10.5 定点，  
  512<<5=16384 安全；曾误取 1024，1024<<5=32768 溢出为负 ⇒ 纹理/UV 全乱）。  
  光栅档位表 `kRasterBuckets` 扩到 13 档（12…512），支持 mod 的大号 HUD 文字  
  （1440p/4K 下 em 可达 300+）落到高档位，避免"字号越大越糊且带锯齿"。
  常规会话（ASCII 数十张 + 汉字数百张）在 **10MB 量级**；上限由
  `GLYPH_BYTES_LIMIT = 128MB` 兜底，超出后仅丢弃新字形。  
  加上槽位表约 3.9MB（7000 字 × 13 档 × 56B），字形后端总内存约 15MB 量级。
- **⚠ 字形光栅必须恒定超采样（`kGlyphSupersample = 1.25`），不能 1:1**：  
  档位选择是"第一个 ≥ `em × 1.25` 的档"，**绝不能**退回"第一个 ≥ em"。
  因为字形 quad 的顶点落在**分数像素**坐标上（位置是浮点、还可能有亚像素
  抖动）。1:1（位图边长 == 屏幕 em）时采样点恰好压在纹素边界上，
  相位一偏就在"点采样留硬边 → 锯齿"与"双线性半纹素混叠 → 发糊"之间摆动，
  用户看到的就是"**模糊且有锯齿感**"。
  `Hide and Seek` 顶部文字是最典型的受害者：它 `scale=0.5`，
  屏幕 em = `defaultFontScale(32) × 0.5 × djuiScale` = **16px**，
  而档位表里**恰好有一档 16** ⇒ ratio 恒为 **1.00** ⇒ 最坏情形。
  超采样后 16 → 20 档（ratio 1.25），双线性降采样即成为标准抗锯齿。
  几何不受影响：`render_char` 用 `invRaster = 1/rasterEm` 做 em 相对换算，
  **任何档位**在屏幕上的字号都相同，档位只决定位图分辨率。
- **被隐藏的控件不占边框收编槽位**：`host_border_takeover` 会跳过
  "自身或祖先不可见"的基座（聊天栏 / 输入框这类控件把 `visible` 设在子节点上，
  父链矩形每帧仍会被遍历到，若不跳过会慢慢占满 64 个槽位，
  导致真正需要收编的按钮边框被丢弃、原版直角边框条重新出现）。
