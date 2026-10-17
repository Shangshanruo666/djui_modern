#!/bin/sh
#  djui_modern 安装脚本（桌面：Windows / macOS / Linux）
#
#  把一个干净的 sm64coopdx（及其分支）源码树改造成"已装好本模块"的状态：
#
#    1. 确认当前目录确实是 sm64coopdx 源码树（有 Makefile + src/pc/djui/…）
#    2. 把本模块挂成 git 子模块：<根目录>/src/pc/djui_modern
#    3. 应用 patches/*.patch：也就是 README 里"需要改动的原文件"表列出的
#       全部改动。README 没收录的改动不会进补丁（防止夹带）。
#
#  用法：
#      sh install.sh                 # 在当前目录安装（须是源码树根目录）
#      sh install.sh --root <目录>    # 指定源码树根目录
#      sh install.sh --url <仓库地址>  # 指定本模块的仓库地址（默认自动探测）
#      sh install.sh --no-submodule  # 不碰 git，只应用补丁（模块已就位时用）
#      sh install.sh --revert        # 反向应用补丁（撤销对原版文件的改动）
#      sh install.sh --dry-run       # 只检查能否应用，不写文件
#      sh install.sh <分支>          # 指定模块分支
#      curl -LSs <install.sh 链接> | bash -s <分支>
#
#  Android 分支请改用 install-android.sh（它多做的是 Android 专属检查，
#  并提示配置文件位置：Android 上 djui_modern.cfg 与 dynos/、mods/ 同级）。
#
#  依赖：`git` 或 `patch` 至少有一个；`git` 用于挂子模块（--no-submodule 可跳过）。

set -u

# ── 本模块的默认仓库地址（可用 --url 覆盖；在 git 克隆里会自动探测 origin）──
DEFAULT_URL="https://github.com/Shangshanruo666/djui_modern.git"

# ── 解析参数 ────────────────────────────────────────────────────────────────
ROOT=""
URL=""
MODULE_REF=""
USE_SUBMODULE=1
DRY_RUN=0
REVERT=0
TARGET="desktop"

while [ $# -gt 0 ]; do
    case "$1" in
        --root)         ROOT="$2"; shift 2 ;;
        --url)          URL="$2"; shift 2 ;;
        --no-submodule) USE_SUBMODULE=0; shift ;;
        --dry-run)      DRY_RUN=1; shift ;;
        --revert)       REVERT=1; shift ;;
        --target)       TARGET="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        -*)
            echo "未知参数: $1（用 --help 看用法）" >&2
            exit 2 ;;
        *)
            [ -z "$MODULE_REF" ] || { echo "未知参数: $1" >&2; exit 2; }
            MODULE_REF="$1"
            shift ;;
    esac
done

# ── 小工具 ──────────────────────────────────────────────────────────────────
say()  { printf '%s\n' "$*"; }
info() { printf '  %s\n' "$*"; }
ok()   { printf '  [OK]   %s\n' "$*"; }
warn() { printf '  [警告] %s\n' "$*"; }
err()  { printf '  [错误] %s\n' "$*" >&2; }
die()  { err "$*"; exit 1; }


# 脚本自身所在目录（本模块的根目录）
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODULE_SOURCE_DIR=""
if [ -f "$SCRIPT_DIR/install.sh" ] && [ -d "$SCRIPT_DIR/patches" ]; then
    MODULE_SOURCE_DIR="$SCRIPT_DIR"
fi

# ── 1. 定位源码树根目录 ─────────────────────────────────────────────────────
looks_like_sm64coopdx() {
    d="$1"
    [ -f "$d/Makefile" ] &&
    [ -d "$d/src/pc/djui" ] &&
    [ -f "$d/src/pc/pc_main.c" ] &&
    [ -f "$d/src/pc/djui/djui.c" ]
}

if [ -z "$ROOT" ]; then
    if looks_like_sm64coopdx "$PWD"; then
        ROOT="$PWD"
    elif looks_like_sm64coopdx "$SCRIPT_DIR/../../.."; then
        # 从"模块已挂在 src/pc/djui_modern"的位置运行
        ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
    fi
fi

say "== djui_modern 安装脚本 =="
say ""

[ -n "$ROOT" ] || die "当前目录不是 sm64coopdx 源码树。请 cd 到源码树根目录，或用 --root <目录> 指定。"
ROOT=$(CDPATH= cd -- "$ROOT" && pwd) || die "无法进入目录: $ROOT"

say "目标源码树: $ROOT"
if ! looks_like_sm64coopdx "$ROOT"; then
    die "该目录不像 sm64coopdx 源码树（缺 Makefile / src/pc/djui/ / src/pc/pc_main.c）。"
fi
ok "确认是 sm64coopdx（及其分支）源码树"

IS_ANDROID_TREE=0
[ -f "$ROOT/Android.mk" ] && IS_ANDROID_TREE=1

# 分支类型判定（决定用哪一套补丁：桌面 / Android）
#   · --target android 明确指定；或
#   · 同时存在 Android.mk 且 Makefile 里有 TARGET_ANDROID
IS_ANDROID=0
if [ "$TARGET" = "android" ]; then
    IS_ANDROID=1
elif [ "$IS_ANDROID_TREE" = "1" ] && grep -q "TARGET_ANDROID" "$ROOT/Makefile" 2>/dev/null; then
    IS_ANDROID=1
fi
if [ "$IS_ANDROID" = "1" ]; then
    say "分支类型: Android（使用 *.android.patch 变体）"
else
    say "分支类型: 桌面（Windows / macOS / Linux）"
    if [ "$IS_ANDROID_TREE" = "1" ]; then
        warn "这看起来是 Android 分支（存在 Android.mk）。"
        warn "Android 的补丁变体不同，建议改用 install-android.sh。"
    fi
fi

MODULE_DIR="$ROOT/src/pc/djui_modern"
PATCH_DIR=""

# git / patch 的可用性（早于子模块步骤就要知道）
# 用 `git --version` 真跑一次：PATH 里可能存在"存在但不可用"的 git。
HAVE_GIT=0;  git --version >/dev/null 2>&1 && HAVE_GIT=1
HAVE_PATCH=0; patch --version >/dev/null 2>&1 && HAVE_PATCH=1

say ""
say "-- 步骤 1/3: 挂载模块 --"

# ── 2. 子模块 ───────────────────────────────────────────────────────────────
if [ "$USE_SUBMODULE" = "0" ]; then
    info "--no-submodule：跳过子模块步骤"
    [ -d "$MODULE_DIR" ] || die "模块目录不存在: ${MODULE_DIR}（请先手动放置模块，或去掉 --no-submodule）"
elif [ -d "$MODULE_DIR" ]; then
    info "模块目录已存在，跳过子模块挂载: src/pc/djui_modern"
else
    if [ -z "$URL" ] && [ -n "$MODULE_SOURCE_DIR" ]; then
        # 优先用本模块自己克隆里的 origin
        if [ "$HAVE_GIT" = "1" ] && git -C "$MODULE_SOURCE_DIR" rev-parse --git-dir >/dev/null 2>&1; then
            URL=$(git -C "$MODULE_SOURCE_DIR" remote get-url origin 2>/dev/null || true)
        fi
    fi
    [ -n "$URL" ] || URL="$DEFAULT_URL"

    if [ "$HAVE_GIT" = "0" ]; then
        die "需要 git 才能挂子模块（或改用 --no-submodule 并手动放置模块）。"
    fi
    if ! git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
        warn "$ROOT 不是 git 仓库，无法挂子模块。"
        warn "请手动把模块放到 src/pc/djui_modern，然后加 --no-submodule 重跑。"
        exit 1
    fi
    if [ -n "$MODULE_REF" ]; then
        info "git submodule add -b $MODULE_REF $URL src/pc/djui_modern"
    else
        info "git submodule add $URL src/pc/djui_modern"
    fi
    if [ "$DRY_RUN" = "1" ]; then
        info "（--dry-run：不实际执行）"
    else
        if [ -n "$MODULE_REF" ]; then
            ( cd "$ROOT" && git submodule add -b "$MODULE_REF" "$URL" src/pc/djui_modern ) ||
                die "子模块挂载失败（分支或仓库地址是否正确？）"
        else
            ( cd "$ROOT" && git submodule add "$URL" src/pc/djui_modern ) ||
                die "子模块挂载失败（仓库地址是否正确？可用 --url 指定）"
        fi
        ok "子模块已挂到 src/pc/djui_modern"
    fi
fi

say ""
say "-- 步骤 2/3: 应用原版文件补丁 --"

SCRIPT_DIR="$MODULE_DIR"
PATCH_DIR="$MODULE_DIR/patches"
[ -d "$PATCH_DIR" ] || die "找不到补丁目录: $PATCH_DIR"
[ -d "$MODULE_DIR" ] || die "模块目录不存在: $MODULE_DIR"

if [ "$HAVE_GIT" = "0" ] && [ "$HAVE_PATCH" = "0" ]; then
    die "系统里既没有可用的 git 也没有 patch，无法应用补丁。"
fi

# 应用/回退一个补丁；返回 0=已改动 1=失败
#
# 分两级尝试，先严后宽：
#   一级：`git apply`（精确，干净，不留副产物）；
#   二级：`patch`，先 --fuzz=0，失败再放到 --fuzz=2。
# fuzz 只放到 2：再大就会把钩子插到函数的右花括号之后、还报告成功。
# 无论走哪一级，都先 --dry-run 再真正写盘，且 -r /dev/null
# --no-backup-if-mismatch，不在源码树里留 .rej / .orig。
#
# 兼容性由"分级 + markers.txt 事后校验"共同保证：
# 上游上下文轻微漂移时 fuzz=2 能接住；真的插错位置时，
# 主循环会用标记复核并回滚该补丁，不会留下半坏的源码树。
apply_one() {
    p="$1"; rev="$2"

    if [ "$HAVE_GIT" = "1" ]; then
        if [ "$rev" = "0" ]; then
            if git -C "$ROOT" apply -p1 --check "$p" >/dev/null 2>&1; then
                [ "$DRY_RUN" = "1" ] && return 0
                git -C "$ROOT" apply -p1 "$p" >/dev/null 2>&1 && return 0
            fi
        else
            if git -C "$ROOT" apply -p1 -R --check "$p" >/dev/null 2>&1; then
                [ "$DRY_RUN" = "1" ] && return 0
                git -C "$ROOT" apply -p1 -R "$p" >/dev/null 2>&1 && return 0
            fi
        fi
    fi

    if [ "$HAVE_PATCH" = "1" ]; then
        for fz in 0 2; do
            if [ "$rev" = "0" ]; then
                patch -p1 -d "$ROOT" --dry-run --silent --fuzz=$fz -f -r /dev/null -i "$p" </dev/null >/dev/null 2>&1 || continue
                [ "$DRY_RUN" = "1" ] && return 0
                patch -p1 -d "$ROOT" --silent --fuzz=$fz -f -r /dev/null --no-backup-if-mismatch -i "$p" </dev/null >/dev/null 2>&1 && return 0
            else
                patch -p1 -d "$ROOT" --dry-run --silent --fuzz=$fz -f -R -r /dev/null -i "$p" </dev/null >/dev/null 2>&1 || continue
                [ "$DRY_RUN" = "1" ] && return 0
                patch -p1 -d "$ROOT" --silent --fuzz=$fz -f -R -r /dev/null --no-backup-if-mismatch -i "$p" </dev/null >/dev/null 2>&1 && return 0
            fi
        done
    fi
    return 1
}

# ── "这个补丁是否已经应用过"的确定性判据 ──────────────────────────────────
# patches/markers.txt 由 src/pc/djui_modern/tools/make_patches.py 生成：
# 每个补丁对应一条"只可能出现在打过补丁的文件里"的标记行，外加它的适用平台。
# 比"反向能否应用"可靠得多：`patch` 在已打过的文件上偶尔仍能匹配成功，
# 会造成重复应用。
MARKERS="$PATCH_DIR/markers.txt"
marker_field() {   # $1=补丁名  $2=字段号（2=目标文件 3=标记 4=平台）
    [ -f "$MARKERS" ] || return 0
    awk -F'|' -v n="$1" -v f="$2" '$1 == n { print $f; exit }' "$MARKERS"
}

CHANGED=0; SKIPPED=0; FAILED=0; FAILED_LIST=""

for p in "$PATCH_DIR"/*.patch; do
    [ -f "$p" ] || continue
    name=$(basename "$p")
    tgt=$(marker_field "$name" 2)
    mk=$(marker_field "$name" 3)
    plat=$(marker_field "$name" 4)

    # 平台不匹配的变体直接跳过（不计入统计）
    if [ "$plat" = "desktop" ] && [ "$IS_ANDROID" = "1" ]; then
        info "${name}：桌面专用，跳过"; continue
    fi
    if [ "$plat" = "android" ] && [ "$IS_ANDROID" = "0" ]; then
        info "${name}：Android 专用，跳过"; continue
    fi

    state="unknown"
    if [ -n "$tgt" ] && [ -n "$mk" ] && [ -f "$ROOT/$tgt" ]; then
        if grep -F -q -- "$mk" "$ROOT/$tgt" 2>/dev/null; then state="applied"; else state="clean"; fi
    fi

    if [ "$REVERT" = "0" ] && [ "$state" = "applied" ]; then
        SKIPPED=$((SKIPPED + 1)); info "${name}：已应用，跳过"; continue
    fi
    if [ "$REVERT" = "1" ] && [ "$state" = "clean" ]; then
        SKIPPED=$((SKIPPED + 1)); info "${name}：未应用，无需撤销"; continue
    fi

    if apply_one "$p" "$REVERT"; then
        # 事后用标记复核：补丁报告成功、标记却没出现，说明上下文匹配到了错误位置
        # （fuzz 放宽时可能发生）。此时立刻反向应用回滚，不留半坏的源码树。
        if [ "$DRY_RUN" = "0" ] && [ -n "$mk" ] && [ -n "$tgt" ] && [ -f "$ROOT/$tgt" ]; then
            if [ "$REVERT" = "0" ]; then
                if ! grep -F -q -- "$mk" "$ROOT/$tgt" 2>/dev/null; then
                    apply_one "$p" 1 >/dev/null 2>&1 || true
                    FAILED=$((FAILED + 1)); FAILED_LIST="$FAILED_LIST $name"
                    err "${name}：应用后校验失败（标记未出现），已回滚"
                    continue
                fi
            else
                if grep -F -q -- "$mk" "$ROOT/$tgt" 2>/dev/null; then
                    apply_one "$p" 0 >/dev/null 2>&1 || true
                    FAILED=$((FAILED + 1)); FAILED_LIST="$FAILED_LIST $name"
                    err "${name}：撤销后标记仍在，已重新应用"
                    continue
                fi
            fi
        fi
        CHANGED=$((CHANGED + 1))
        if [ "$REVERT" = "1" ]; then ok "$name （已撤销）"; else ok "$name （已校验）"; fi
    else
        FAILED=$((FAILED + 1)); FAILED_LIST="$FAILED_LIST $name"; err "${name}：应用失败"
    fi
done

say ""
say "-- 步骤 3/3: 结果 --"
info "已改动: $CHANGED   跳过: $SKIPPED   失败: $FAILED"

if [ "$FAILED" != "0" ]; then
    say ""
    err "以下补丁没能应用：$FAILED_LIST"
    say ""
    say "常见原因：你的源码树与本模块发布时的上游版本差异较大。"
    say "处理办法（任选）："
    say "  · 用 --revert 撤销已应用的部分，然后手动按 README 的"
    say "    \"需要改动的原文件\"表逐条修改（表里有精确的行数与说明）；"
    say "  · 或把冲突文件备份后，用  patch -p1 --merge 逐个手工合并。"
    exit 1
fi

if [ "$DRY_RUN" = "1" ]; then
    say ""
    ok "检查完毕（--dry-run，未写入任何文件）"
    exit 0
fi

# ── 配置文件位置提示 ────────────────────────────────────────────────────────
say ""
say "-- 配置文件（首次启动时自动生成，带完整注释）--"
if [ "$TARGET" = "android" ]; then
    say "  Android: <游戏数据目录>/djui_modern.cfg"
    say "           （与 dynos/、mods/ 同级；具体路径见下）"
else
    case "$(uname -s 2>/dev/null)" in
        Darwin) say "  macOS  : 与 sm64coopdx 可执行文件同级（.app 的话在 Contents/MacOS/ 旁）" ;;
        Linux)  say "  Linux  : 与 sm64coopdx 可执行文件同级" ;;
        *)      say "  Windows: 与 sm64coopdx.exe 同级（游戏根目录）" ;;
    esac
fi
say "  版本更新后，文件里缺失的配置项会自动补全（连同说明注释），无需手工维护。"

say ""
ok "安装完成。下一步："
if [ "$TARGET" = "android" ]; then
    say "     cd \"$ROOT\" && make            # 默认目标会产出 build/us_pc/sm64coopdx.apk"
    say "   （Android 上通常用 Termux 构建；构建产物是 APK，安装后即可运行）"
else
    say "     cd \"$ROOT\" && make            # Windows / macOS / Linux 的常规构建"
fi
say "  日志里看到 djui_modern 的几行 [INFO] 就说明模块已生效。"

if [ "$REVERT" = "1" ]; then
    say ""
    say "（本次是 --revert：已把原版文件恢复为未安装状态；模块目录本身未删除。）"
fi
