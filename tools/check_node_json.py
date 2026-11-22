# -*- coding: utf-8 -*-
"""算子 node.json 参数说明守卫（PM 裁定方案 (a)，2026-11-22）。

背景：DemoStitching/demo.node.json 的 gamma 参数说明被外部 alg 会话同步
覆盖过 4 次。本脚本只报警不回写（避免与外部会话互相覆盖打架）。

规则：
  R1 通用：每个 params[] 条目 description 非空
  R2 通用：每个算子 node.description 非空
  R3 哨兵：algo.gamma 的 description 必须含「提亮」（钉住被冲掉的具体字段）
  R4 一致性：src/Plugins/DemoAlgo.Core/demo.node.json 与
             comdll/algo/DemoStitching/demo.node.json 逐字节一致

范围：comdll/algo/**/*.node.json（运行时权威，NodeJsonRegistry 按
AllDirectories 读）+ src/Plugins/**/demo.node.json。

接入：日常回归在 dotnet test 后追加本脚本；alg 会话触碰 node.json 后必跑。
退出码：0 通过；1 失败（逐条打印 文件/算子/参数/现状值）。

JSONC 容忍：与 C# 端 JsonCommentHandling.Skip 对齐，先剥 // 与 /* */ 再解析。
"""
import io
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# 哨兵：id → (必含子串, 说明)
SENTINELS = {
    "algo.gamma": ("提亮", "gamma 说明被 alg 同步冲掉过 4 次的哨兵字段"),
}


def strip_comments(s: str) -> str:
    """剥 JSONC 注释（状态机：字符串内不剥，转义不误判）。"""
    out = []
    i, n = 0, len(s)
    in_str = False
    while i < n:
        c = s[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(s[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
        elif c == "/" and i + 1 < n and s[i + 1] == "/":
            while i < n and s[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and s[i + 1] == "*":
            i += 2
            while i + 1 < n and not (s[i] == "*" and s[i + 1] == "/"):
                i += 1
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def iter_files():
    # bin/obj 是构建产物（拷贝滞后会假警），守卫只看源与部署件
    def wanted(p: Path) -> bool:
        return not any(seg in {"bin", "obj"} for seg in p.parts)

    algo = ROOT / "comdll" / "algo"
    if algo.is_dir():
        yield from (p for p in sorted(algo.rglob("*.node.json")) if wanted(p))
    yield from (p for p in sorted(ROOT.glob("src/Plugins/**/demo.node.json")) if wanted(p))


def main() -> int:
    problems = []          # (文件, 算子, 参数, 问题, 现状值)
    parsed = []            # (文件, node dict)
    parse_fail = []        # (文件, 原因)

    for f in iter_files():
        rel = f.relative_to(ROOT).as_posix()
        try:
            raw = io.open(f, encoding="utf-8-sig").read()
            d = json.loads(strip_comments(raw))
        except Exception as ex:  # 解析失败本身就是问题
            parse_fail.append((rel, str(ex)))
            continue
        parsed.append((rel, d))

        node = d.get("node") if isinstance(d, dict) else None
        if not isinstance(node, dict):
            problems.append((rel, "-", "-", "缺 node 对象", "-"))
            continue

        nid = node.get("id") or "-"

        # R2 节点说明
        desc = node.get("description")
        if desc is None or not str(desc).strip():
            problems.append((rel, nid, "-", "节点说明为空", repr(desc)))

        # R1 参数说明（params 可能在 node 内或顶层）
        params = node.get("params")
        if params is None and isinstance(d.get("params"), list):
            params = d.get("params")
        if isinstance(params, list):
            for prm in params:
                if not isinstance(prm, dict):
                    continue
                pname = prm.get("name") or "-"
                pdesc = prm.get("description")
                if pdesc is None or not str(pdesc).strip():
                    problems.append((rel, nid, pname, "参数说明为空", repr(pdesc)))

        # R3 哨兵（params 位置与 R1 同口径：node 内优先、顶层回退）
        if nid in SENTINELS:
            want, why = SENTINELS[nid]
            got = node.get("params")
            if got is None and isinstance(d.get("params"), list):
                got = d.get("params")
            gdesc = ""
            if isinstance(got, list):
                for prm in got:
                    if isinstance(prm, dict) and prm.get("name") == "gamma":
                        gdesc = str(prm.get("description") or "")
            if want not in gdesc:
                problems.append((rel, nid, "gamma", f"哨兵缺失[{why}]，需含「{want}」", repr(gdesc)))

    # R4 两份 demo.node.json 字节级一致
    demo_a = ROOT / "src/Plugins/DemoAlgo.Core/demo.node.json"
    demo_b = ROOT / "comdll/algo/DemoStitching/demo.node.json"
    if demo_a.exists() and demo_b.exists():
        ba = demo_a.read_bytes()
        bb = demo_b.read_bytes()
        if ba != bb:
            problems.append(
                (f"{demo_a.relative_to(ROOT).as_posix()} vs {demo_b.relative_to(ROOT).as_posix()}",
                 "-", "-", "两份 demo.node.json 不一致（R4）",
                 f"{len(ba)}B vs {len(bb)}B"))
    else:
        missing = [p for p in (demo_a, demo_b) if not p.exists()]
        for p in missing:
            problems.append((p.as_posix(), "-", "-", "demo.node.json 缺失（R4）", "-"))

    # 汇总输出
    print(f"check_node_json: 扫描 {len(parsed) + len(parse_fail)} 个文件"
          f"（解析失败 {len(parse_fail)}），问题 {len(problems)} 条")
    for rel, why in parse_fail:
        print(f"  [解析失败] {rel}: {why}")
    for rel, nid, pname, why, val in problems:
        print(f"  [FAIL] {rel} | {nid} | {pname} | {why} | {val}")
    if problems or parse_fail:
        print("FAIL: 存在缺失说明或一致性破坏——按上面清单逐条补回后重跑")
        return 1
    print("PASS: 全部算子节点/参数说明完整，demo 双份一致，哨兵在位")
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    sys.exit(main())
