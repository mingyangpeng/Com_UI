#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sync_alg.py —— 从 alg submodule 同步源码到运行时算子目录。

背景：
  comdll/algo/alg           = git submodule（alg 仓库，提供源码，稀疏检出 src/ 与 integration/comui/）
  comdll/algo/AlgProject    = 运行时目录（node.json + DLL + 源码快照），宿主从这里读 DLL 和 node.json

职责：
  1. 把 submodule 的 src/** 与 integration/comui/{*.cpp,comui_native_abi.h} 同步到 AlgProject
     —— 只同步源码，不同步 submodule 里的 node.json、CMakeLists、thrid 等；
     —— DLL 由本地编译产生，本脚本不碰。
  2. 同步后调用 gen_node_json.py 重新生成 node.json：
     backfill（从现有 node.json 反向注入标注）→ gen（从标注生成 node.json）→ check（一致性校验）。
     若 alg 源码新增了导出函数而 node.json 没有对应声明，check 会报未匹配，提示人工补声明。

用法：
  python tools/sync_alg.py init                   # 首次初始化 submodule（浅克隆 + 稀疏检出，避免下载 2 万文件）
  python tools/sync_alg.py status                 # 列出会变化的文件，不落盘
  python tools/sync_alg.py sync                   # 同上（默认 dry-run）
  python tools/sync_alg.py sync --write           # 真正同步源码 + 重建 node.json + 校验
  python tools/sync_alg.py sync --write --skip-json   # 只同步源码，不重建 node.json

注意：
  - init 封装了「先 no-checkout 注册 → 设稀疏检出 → 再 checkout」的顺序；
    直接 git submodule update --init 会 checkout 全部文件（含 thrid/ 2 万个），慢但也能用。
  - git submodule 的 filter/shallow 配置写在 .gitmodules（会提交），
    稀疏检出写在 submodule 的 .git/info/sparse-checkout（本地配置，故用 init 封装）。
  - 若本机需要代理，init 前先设置 git 全局代理；或 init 后手动给 submodule 配 http.proxy。"""
import argparse
import os
import re
import shutil
import subprocess
import sys

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUBMODULE = os.path.join(PROJECT_ROOT, "comdll", "algo", "alg")
ALG_DIR = os.path.join(PROJECT_ROOT, "comdll", "algo", "AlgProject")
SRC_REL = "src"
CPP_DIR_REL = os.path.join("integration", "comui")
CPP_NAMES = ["Image2DAlg_comui.cpp", "Point3DAlg_comui.cpp", "BusinessAlg_comui.cpp",
             "comui_native_abi.h"]
GEN_SCRIPT = os.path.join(PROJECT_ROOT, "tools", "gen_node_json.py")

# 不同步这些（submodule 有但运行时目录不需要 / 由本地维护）
SKIP_SRC_NAMES = {"LastTest.log"}


def sha(path):
    import hashlib
    h = hashlib.md5()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def walk_files(root):
    out = []
    if not os.path.isdir(root):
        return out
    for dp, _, fns in os.walk(root):
        for fn in fns:
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, root).replace("\\", "/")
            out.append((rel, p))
    return out


def collect_changes():
    """返回 (待同步列表, 警告列表)。每项 (rel, src_path, dst_path, action, reason)。"""
    changes, warns = [], []

    # --- src/** ---
    src_root = os.path.join(SUBMODULE, SRC_REL)
    dst_root = os.path.join(ALG_DIR, SRC_REL)
    if not os.path.isdir(src_root):
        warns.append(f"submodule 未初始化或缺少 {SRC_REL}/：{src_root}"
                     "（先执行 git submodule update --init comdll/algo/alg）")
    else:
        src_files = {r: p for r, p in walk_files(src_root)
                     if os.path.basename(r) not in SKIP_SRC_NAMES}
        dst_files = {r: p for r, p in walk_files(dst_root)}
        for rel in sorted(src_files):
            sp = src_files[rel]
            dp = os.path.join(dst_root, rel.replace("/", os.sep))
            if rel not in dst_files:
                changes.append((f"{SRC_REL}/{rel}", sp, dp, "新增", "submodule 有，本地无"))
            elif sha(sp) != sha(dst_files[rel]):
                changes.append((f"{SRC_REL}/{rel}", sp, dp, "覆盖", "内容不同"))

    # --- integration/comui/{*.cpp, comui_native_abi.h} ---
    # 本地 cpp 带 @comui-node 标注，submodule 的干净；比较时先剥掉标注块再比。
    ann_re = re.compile(r"// @comui-node-begin\r?\n.*?// @comui-node-end\r?\n", re.S)
    s_cpp = os.path.join(SUBMODULE, CPP_DIR_REL)
    d_cpp = os.path.join(ALG_DIR, CPP_DIR_REL)
    for name in CPP_NAMES:
        sp, dp = os.path.join(s_cpp, name), os.path.join(d_cpp, name)
        if not os.path.isfile(sp):
            warns.append(f"submodule 缺少 {CPP_DIR_REL}/{name}")
            continue
        sb = open(sp, "rb").read()
        if os.path.isfile(dp):
            db = open(dp, "rb").read()
            dtxt = db.decode("utf-8-sig", errors="replace")
            d_clean = ann_re.sub("", dtxt).replace("\r\n", "\n").strip()
            s_clean = sb.decode("utf-8-sig", errors="replace").replace("\r\n", "\n").strip()
            same = d_clean == s_clean
        else:
            same = False
        if not same:
            note = "内容不同（本地标注会在同步后由 backfill 恢复）" if os.path.isfile(dp) else "本地无"
            changes.append((f"{CPP_DIR_REL}/{name}", sp, dp,
                            "覆盖" if os.path.isfile(dp) else "新增", note))
    return changes, warns


def run_gen(subcmd, extra=()):
    cmd = [sys.executable, GEN_SCRIPT, subcmd, *extra]
    print("  $", " ".join(cmd))
    r = subprocess.run(cmd, cwd=PROJECT_ROOT)
    return r.returncode


def do_sync(changes, write, skip_json):
    for rel, sp, dp, action, reason in changes:
        print(f"  [{action}] {rel}  ({reason})")
        if write:
            os.makedirs(os.path.dirname(dp), exist_ok=True)
            shutil.copyfile(sp, dp)
    if not write:
        print("\n（dry-run：未落盘。加 --write 执行同步）")
        return 0
    if not changes:
        print("\n源码已一致，无需同步。")
    else:
        print(f"\n已同步 {len(changes)} 个文件。")

    if skip_json:
        print("--skip-json：跳过 node.json 重建。")
        return 0

    print("\n=== 重建 node.json ===")
    print("[1/3] backfill：从现有 node.json 恢复 cpp 标注")
    rc1 = run_gen("backfill", ("--write",))
    print("\n[2/3] gen：从标注生成 node.json")
    rc2 = run_gen("gen", ("--write",))
    print("\n[3/3] check：三方一致性校验")
    rc3 = run_gen("check")
    if rc1 or rc2 or rc3:
        print("\n⚠ 重建过程有报错，请按上面输出处理（新增算子需人工在 cpp 写标注后重跑 gen）。")
        return 1
    print("\n✓ 同步 + 重建完成，校验通过。")
    return 0


def do_init():
    """初始化 submodule：no-checkout 注册 → 稀疏检出 → checkout，避免下载 thrid/ 2 万文件。"""
    cmds = [
        ["git", "submodule", "init", "comdll/algo/alg"],
        ["git", "-C", SUBMODULE, "config", "remote.origin.promisor", "true"],
        ["git", "-C", SUBMODULE, "config", "remote.origin.partialclonefilter", "blob:none"],
        ["git", "-C", SUBMODULE, "sparse-checkout", "init", "--cone"],
        ["git", "-C", SUBMODULE, "sparse-checkout", "set", "src", "integration/comui"],
        ["git", "-C", SUBMODULE, "checkout", "main"],
    ]
    for cmd in cmds:
        print("$", " ".join(cmd))
        r = subprocess.run(cmd, cwd=PROJECT_ROOT)
        if r.returncode:
            print("✗ 命令失败，init 中止。若 submodule 目录已存在但内容不对，"
                  "可先 git submodule deinit -f comdll/algo/alg 后重试。")
            return r.returncode
    print("\n✓ submodule 初始化完成（仅 src/ 与 integration/comui/）。")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("init", help="首次初始化 submodule（浅克隆 + 稀疏检出）")
    sub.add_parser("status", help="列出会变化的文件（只读）")
    ps = sub.add_parser("sync", help="从 submodule 同步源码并重建 node.json")
    ps.add_argument("--write", action="store_true", help="落盘（默认 dry-run）")
    ps.add_argument("--skip-json", action="store_true", help="只同步源码，不重建 node.json")
    args = ap.parse_args()

    if args.cmd == "init":
        return do_init()

    if not os.path.exists(os.path.join(SUBMODULE, ".git")):
        print(f"✗ submodule 未初始化：{SUBMODULE}")
        print("  先执行：python tools/sync_alg.py init")
        return 2

    print(f"submodule HEAD: ", end="")
    r = subprocess.run(["git", "-C", SUBMODULE, "log", "-1", "--format=%h %ad %s",
                        "--date=short"], capture_output=True, text=True)
    print(r.stdout.strip() or "(未知)")

    changes, warns = collect_changes()
    for w in warns:
        print("[WARN]", w)
    print(f"\n待同步 {len(changes)} 个文件：")
    if args.cmd == "status":
        for rel, _, _, action, reason in changes:
            print(f"  [{action}] {rel}  ({reason})")
        if not changes:
            print("  （无，已一致）")
        return 0
    return do_sync(changes, args.write, args.skip_json)


if __name__ == "__main__":
    sys.exit(main())
