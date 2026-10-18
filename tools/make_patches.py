#!/usr/bin/env python3
"""从"未修改的上游源码"与"已装好本模块的源码"生成上游改动补丁集。

本模块需要往 sm64coopdx 的若干**原版文件**里插几行钩子。这些改动以 `.patch` 形式
随模块发布，由 `install.sh` / `install-android.sh` 自动应用。

用法（在 sm64coopdx 源码树根目录下执行）:

    python src/pc/djui_modern/tools/make_patches.py \\
        --upstream "D:/path/to/sm64coopdx-pristine" \\
        --upstream-android "D:/path/to/sm64coopdx-android-pristine" \\
        --current  .

生成到 `src/pc/djui_modern/patches/`：
  · `<nn>-<文件>.patch`           —— 两个分支通用
  · `<nn>-<文件>.android.patch`   —— 仅当 Android 分支的那份原版文件与桌面版
                                      **内容不同**时才会额外生成
"""
import argparse
import os
import subprocess
import sys

TARGET_FILES = [
    "Makefile",
    "src/pc/djui/djui.c",
    "src/pc/djui/djui_rect.c",
    "src/pc/djui/djui_rect.h",
    "src/pc/djui/djui_gfx.c",
    "src/pc/djui/djui_gfx.h",
    "src/pc/djui/djui_unicode.c",
    "src/pc/djui/djui_text.c",
    "src/pc/djui/djui_text.h",
    "src/pc/djui/djui_panel_host.c",
    "src/pc/djui/djui_panel_host_mods.c",
    "src/pc/djui/djui_panel_host_mods.h",
    "src/pc/gfx/gfx_dxgi.cpp",
]

ANDROID_ONLY_TARGET_FILES = [
    "Android.mk",
]


def read_lf(path):
    with open(path, "rb") as f:
        return f.read().replace(b"\r\n", b"\n")


def make_patch(rel, old_bytes, new_bytes):
    """生成 a/<rel> -> b/<rel> 的标准 unified diff（可被 git apply -p1 / patch -p1 使用）。"""
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        fa = os.path.join(td, "old.tmp")
        fb = os.path.join(td, "new.tmp")
        with open(fa, "wb") as f:
            f.write(old_bytes)
        with open(fb, "wb") as f:
            f.write(new_bytes)
        r = subprocess.run(["git", "diff", "--no-index", "--no-color", fa, fb],
                           capture_output=True, text=True)
        raw = r.stdout.splitlines(keepends=True)

    out = []
    for line in raw:
        if line.startswith("diff --git "):
            out.append("diff --git a/%s b/%s\n" % (rel, rel))
        elif line.startswith("index "):
            continue
        elif line.startswith("--- "):
            out.append("--- a/%s\n" % rel)
        elif line.startswith("+++ "):
            out.append("+++ b/%s\n" % rel)
        else:
            out.append(line)
    return "".join(out)


def pick_marker(patch, old_text):
    """挑一条"只可能出现在打过补丁的文件里"的行，作为幂等判据。"""
    added = [l[1:].strip() for l in patch.splitlines()
             if l.startswith("+") and not l.startswith("+++")]
    for line in added:
        if len(line) >= 6 and line not in old_text:
            return line
    return max(added, key=len) if added else ""
    

def parse_hunks(patch_text):
    """把 unified diff 解析成 hunk 列表；每个 hunk 是 [(kind, text)]，kind ∈ ' +-'。"""
    hunks = []
    cur = None
    for raw in patch_text.split("\n"):
        if raw.startswith("@@"):
            if cur:
                hunks.append(cur)
            cur = []
            continue
        if cur is None:
            continue
        if raw.startswith("\\"):          # "\ No newline at end of file"
            continue
        if raw.startswith("+++") or raw.startswith("---"):
            continue
        if raw[:1] in (" ", "+", "-"):
            cur.append((raw[0], raw[1:]))
    if cur:
        hunks.append(cur)
    return hunks


def _find_seq(lines, seq, hint):
    if not seq:
        return -1
    n, m = len(lines), len(seq)
    cands = [i for i in range(0, n - m + 1) if lines[i:i + m] == seq]
    if not cands:
        return -1
    for c in cands:
        if c >= hint:
            return c
    return cands[0]


def _find_line(lines, text, hint):
    cands = [i for i, l in enumerate(lines) if l == text]
    if not cands:
        return -1
    for c in cands:
        if c >= hint:
            return c
    return cands[0]


def _apply_hunk(lines, hunk, hint):
    """把一个 hunk 落到 lines 上。

    hunk 里可能有**多段**新增（git 会把靠得近的改动合并进同一个 hunk），
    所以不能简单地按"一段新增"处理 —— 直接构造：
        search  = hunk 里所有非 '+' 行（原样）
        replace = hunk 里所有非 '-' 行（打补丁后应有的样子）
    """
    search = [t for k, t in hunk if k != "+"]
    replace = [t for k, t in hunk if k != "-"]
    is_pure_insert = all(k != "-" for k, _ in hunk)

    # 1) 整段精确匹配（最可靠）
    pos = _find_seq(lines, search, hint)
    if pos >= 0:
        lines[pos:pos + len(search)] = replace
        return pos + len(replace), True

    # 2) 纯插入 → 锚点定位（用最近的非空前置上下文行）
    #    放在"丢弃尾部上下文"之前：锚点法更精确，不会误命中别处的 `}`。
    if is_pure_insert:
        pre = []
        for k, t in hunk:
            if k == " ":
                pre.append(t)
            else:
                break
        adds = [t for k, t in hunk if k == "+"]
        for line in reversed(pre):
            if not line.strip():
                continue
            p = _find_line(lines, line, hint)
            if p >= 0:
                ins = p + 1
                lines[ins:ins] = adds
                return ins + len(adds), True
        post = [t for k, t in hunk if k == " "][len(pre):]
        for line in post:
            if not line.strip():
                continue
            p = _find_line(lines, line, hint)
            if p >= 0:
                lines[p:p] = adds
                return p + len(adds), True

    # 3) 尾部上下文与目标分支不同 → 逐步丢掉尾部上下文行再试
    tail = 0
    for k, _ in reversed(hunk):
        if k == " ":
            tail += 1
        else:
            break
    for drop in range(1, min(tail, 4) + 1):
        s2, r2 = search[:-drop], replace[:-drop]
        pos = _find_seq(lines, s2, hint)
        if pos >= 0:
            lines[pos:pos + len(s2)] = r2
            return pos + len(r2), True

    return hint, False


def reapply_patch_to(src_text, patch_text):
    """把 patch_text 的改动重新落到 src_text 上。返回 (新文本, 错误信息)。"""
    lines = src_text.split("\n")
    hint = 0
    for idx, hunk in enumerate(parse_hunks(patch_text)):
        hint, ok = _apply_hunk(lines, hunk, hint)
        if not ok:
            return None, "第 %d 个 hunk 无法定位" % (idx + 1)
    return "\n".join(lines), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--upstream", required=True, help="未修改的上游源码目录（桌面分支）")
    ap.add_argument("--upstream-android", default=None,
                    help="未修改的 Android 分支源码目录（可选；给了就会为差异文件额外生成 .android.patch）")
    ap.add_argument("--current", default=".", help="已装好模块的源码目录（默认当前目录）")
    ap.add_argument("--out", default=None, help="补丁输出目录")
    args = ap.parse_args()

    upstream = os.path.abspath(args.upstream)
    android = os.path.abspath(args.upstream_android) if args.upstream_android else None
    current = os.path.abspath(args.current)
    outdir = args.out or os.path.join(current, "src", "pc", "djui_modern", "patches")
    os.makedirs(outdir, exist_ok=True)

    # 清掉旧补丁，避免删掉的改动留下残骸
    for name in sorted(os.listdir(outdir)):
        if name.endswith(".patch") or name == "markers.txt":
            os.remove(os.path.join(outdir, name))

    entries = []      # (patch 文件名, 目标文件, 标记, 平台)
    made = 0
    skipped = []

    for i, rel in enumerate(TARGET_FILES, start=1):
        up_path = os.path.join(upstream, rel)
        cur_path = os.path.join(current, rel)
        if not os.path.isfile(up_path) or not os.path.isfile(cur_path):
            skipped.append(rel + "（文件缺失）")
            continue

        up = read_lf(up_path)
        cur = read_lf(cur_path)
        if up == cur:
            skipped.append(rel + "（无改动）")
            continue

        base = os.path.basename(rel)

        # Android 分支那份原版文件是否与桌面版不同？
        android_differs = False
        if android is not None:
            and_path = os.path.join(android, rel)
            if os.path.isfile(and_path):
                android_differs = read_lf(and_path) != up

        # ── 桌面版补丁 ──
        p = make_patch(rel, up, cur)

        # ── 自检：把补丁重新落到"桌面原版"上，必须逐字节等于当前工作树 ──
        #    （保证 hunk 定位算法本身没跑偏；偏了就说明补丁或算法有问题）
        recheck, recheck_err = reapply_patch_to(up.decode("utf-8", "replace"), p)
        if recheck_err is not None or recheck != cur.decode("utf-8", "replace"):
            print("  !! %s 自检失败（%s），已中止" % (rel, recheck_err or "结果不一致"))
            return 2

        name = "%02d-%s.patch" % (i, base)
        with open(os.path.join(outdir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(p)
        added = sum(1 for l in p.splitlines() if l.startswith("+") and not l.startswith("+++"))
        removed = sum(1 for l in p.splitlines() if l.startswith("-") and not l.startswith("---"))
        plat = "desktop" if android_differs else "any"
        entries.append((name, rel, pick_marker(p, up.decode("utf-8", "replace")), plat))
        print("  %-40s +%-4d -%-4d  -> %-34s [%s]" % (rel, added, removed, name, plat))
        made += 1

        # ── Android 变体（仅当两分支的原版文件不同）──
        if android_differs:
            and_text = read_lf(os.path.join(android, rel)).decode("utf-8", "replace")
            merged_text, err = reapply_patch_to(and_text, p)
            if err is not None:
                skipped.append("%s 的 Android 变体：%s" % (rel, err))
                print("  !! %s 的 Android 变体失败：%s" % (rel, err))
                continue
            merged = merged_text.encode("utf-8")
            p2 = make_patch(rel, read_lf(os.path.join(android, rel)), merged)
            name2 = "%02d-%s.android.patch" % (i, base)
            with open(os.path.join(outdir, name2), "w", encoding="utf-8", newline="\n") as f:
                f.write(p2)
            entries.append((name2, rel,
                            pick_marker(p2, and_text),
                            "android"))
            added2 = sum(1 for l in p2.splitlines() if l.startswith("+") and not l.startswith("+++"))
            removed2 = sum(1 for l in p2.splitlines() if l.startswith("-") and not l.startswith("---"))
            print("  %-40s +%-4d -%-4d  -> %-34s [android]" % ("  ↳ Android 变体", added2, removed2, name2))
            made += 1

    for j, rel in enumerate(ANDROID_ONLY_TARGET_FILES, start=len(TARGET_FILES) + 1):
        if android is None:
            skipped.append(rel + "（未提供 Android 上游）")
            continue

        and_path = os.path.join(android, rel)
        cur_path = os.path.join(current, rel)
        if not os.path.isfile(and_path) or not os.path.isfile(cur_path):
            skipped.append(rel + "（文件缺失）")
            continue

        up = read_lf(and_path)
        cur = read_lf(cur_path)
        if up == cur:
            skipped.append(rel + "（无改动）")
            continue

        p = make_patch(rel, up, cur)
        recheck, recheck_err = reapply_patch_to(up.decode("utf-8", "replace"), p)
        if recheck_err is not None or recheck != cur.decode("utf-8", "replace"):
            print("  !! %s 自检失败（%s），已中止" % (rel, recheck_err or "结果不一致"))
            return 2

        base = os.path.basename(rel)
        name = "%02d-%s.android.patch" % (j, base)
        with open(os.path.join(outdir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(p)
        entries.append((name, rel, "src/pc/djui_modern", "android"))
        added = sum(1 for l in p.splitlines() if l.startswith("+") and not l.startswith("+++"))
        removed = sum(1 for l in p.splitlines() if l.startswith("-") and not l.startswith("---"))
        print("  %-40s +%-4d -%-4d  -> %-34s [android]" % (rel, added, removed, name))
        made += 1

    with open(os.path.join(outdir, "markers.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# 自动生成，请勿手工编辑（由 src/pc/djui_modern/tools/make_patches.py 产出）。\n")
        f.write("# 格式: <补丁文件>|<目标文件>|<已应用标记>|<适用平台 desktop|android|any>\n")
        f.write("# 「标记」出现于目标文件 ⇒ 该补丁已经应用过；安装脚本据此保证幂等。\n")
        for name, rel, marker, plat in entries:
            f.write("%s|%s|%s|%s\n" % (name, rel, marker, plat))

    print("\n生成 %d 个补丁 + markers.txt -> %s" % (made, outdir))
    for s in skipped:
        print("  跳过: %s" % s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
