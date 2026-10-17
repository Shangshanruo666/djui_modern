#!/bin/sh
#  djui_modern 安装脚本（Android 分支）
#
#  与 install.sh 做的是同一件事（挂子模块 + 应用 README 里列出的全部上游改动），
#  配置文件的落点也一样：两端都放在游戏根目录：
#
#      桌面（Windows/macOS/Linux）: 与 coopdx 可执行文件同级
#      Android                    : 与 dynos/、mods/ 同级（游戏数据目录那一层）
#
#  两端共用同一条路径（`sys_exe_path_dir()`），所以本脚本不需要额外的补丁：
#  它就是 install.sh 加一个 `--target android` 开关，另外多做几项 Android 检查。
#
#  用法：
#      sh install-android.sh                 # 在当前目录安装
#      sh install-android.sh --root <目录>
#      sh install-android.sh --url <仓库地址>
#      sh install-android.sh --no-submodule
#      sh install-android.sh --revert
#      sh install-android.sh --dry-run
#
#  Android 上配置文件的实际位置（首次启动自动生成）：
#      <游戏数据目录>/djui_modern.cfg          （与 dynos/、mods/ 同级）
#    未授予存储权限时：/storage/emulated/0/Android/data/<包名>/files/
#    已授予存储权限时：/storage/emulated/0/<包名>/
#    其中 <包名> 见 src/pc/platform.h 的 ANDROID_APPNAME。

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ ! -f "$SCRIPT_DIR/install.sh" ]; then
    echo "  [错误] 找不到 install.sh（应与本脚本在同一目录）" >&2
    exit 1
fi

# ── Android 专属检查：确认这确实是 Android 分支 ────────────────────────────
ROOT_HINT=""
prev=""
for a in "$@"; do
    [ "$prev" = "--root" ] && ROOT_HINT="$a"
    prev="$a"
done
[ -n "$ROOT_HINT" ] || ROOT_HINT="$PWD"

if [ -f "$ROOT_HINT/Android.mk" ]; then
    echo "  [OK]   确认存在 Android.mk"
else
    echo "  [警告] $ROOT_HINT 下没有 Android.mk：这可能不是 Android 分支。"
    echo "         如果你其实在装桌面版，请改用 install.sh。"
    echo "         仍然继续（本脚本与 install.sh 的差别只有配置文件落点）。"
fi

if [ -f "$ROOT_HINT/src/pc/platform.h" ]; then
    APPNAME=$(sed -n 's/^#define[[:space:]]\+ANDROID_APPNAME[[:space:]]\+"\(.*\)".*/\1/p' \
              "$ROOT_HINT/src/pc/platform.h" | head -1)
    if [ -n "$APPNAME" ]; then
        echo "  应用包名（ANDROID_APPNAME）= $APPNAME"
        echo "  配置文件将生成在（与 dynos/、mods/ 同级，不是 user/ 里）："
        echo "    /storage/emulated/0/Android/data/$APPNAME/files/djui_modern.cfg"
        echo "    /storage/emulated/0/$APPNAME/djui_modern.cfg   （已授予存储权限时）"
    fi
fi

echo ""

# ── 转交给 install.sh ──────────────────────────────────────────────────────
exec sh "$SCRIPT_DIR/install.sh" --target android "$@"
