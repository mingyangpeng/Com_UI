#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 C++ 导出函数上方的 @comui-node 标注生成 *.node.json（单一事实源 = 标注）。

设计
----
标注写在 EXPORT 函数正上方，形如：

    // @comui-node-begin
    // {
    //   "id": "alg.image2d.blur",
    //   "name": "高斯模糊",
    //   "group": "2D 图像", "category": "处理", "description": "...",
    //   "inputs": [{"name":"输入","type":"image"}],
    //   "params": [{"name":"ksize","label":"核大小","type":"int","default":3,"min":1,"max":31}],
    //   "outputs": [{"name":"输出","type":"image"}],
    //   "source": {"files":[...], "includes":[...]},
    //   "file": "image2d_blur.node.json"
    // }
    // @comui-node-end
    IMAGE2DALG_EXPORT int Image2DAlg_ComuiBlur(const AlgoCInput* in, AlgoCOutput* out) {

脚本自动推导的字段（不要在标注里重复写）：
    runtime  = "native"
    assembly = <cpp 文件名去 _comui.cpp> + ".dll"    （Image2DAlg_comui.cpp -> Image2DAlg.dll）
    method   = 紧随标注之后的导出函数名
    file     = 可选；缺省由 id 推导（alg.xxx.yyy -> xxx_yyy.node.json）

子命令
------
    backfill  从现有 *.node.json 反向注入标注到 cpp（迁移用；已有标注默认跳过）
    gen       从 cpp 标注生成 node.json（默认 dry-run；--write 才落盘）
    check     三方一致性校验：标注 / node.json / 函数体读参；可选 --params 做参数绑定宽松检查

常用：
    python tools/gen_node_json.py check
    python tools/gen_node_json.py gen --write
    python tools/gen_node_json.py backfill --write        # 仅首次迁移
"""

import argparse
import glob
import json
import os
import re
import sys
from collections import Counter

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALG_DIR = os.path.join(PROJECT_ROOT, "comdll", "algo", "AlgProject")
CPP_DIR = os.path.join(ALG_DIR, "integration", "comui")
CPP_FILES = ["Image2DAlg_comui.cpp", "Point3DAlg_comui.cpp", "BusinessAlg_comui.cpp"]

BEGIN = "// @comui-node-begin"
END = "// @comui-node-end"
IGNORE_FUNCS = {"algo_free"}

TYPE2KIND = {
    "number": 0, "double": 0, "float": 0,
    "int": 1, "int32": 1,
    "text": 2, "string": 2, "path": 2, "enum": 2,
    "bool": 3, "boolean": 3,
}


# ----------------------------- 基础工具 -----------------------------

def strip_jsonc(text):
    """剥离 // 行注释与 /* */ 块注释（保留字符串内的 //），用于解析 JSONC 标注。"""
    out = []
    i = 0
    n = len(text)
    in_str = False
    esc = False
    while i < n:
        ch = text[i]
        if in_str:
            out.append(ch)
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
            out.append(ch)
            i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(ch)
        i += 1
    s = "".join(out)
    s = re.sub(r",\s*([}\]])", r"\1", s)   # 尾逗号
    return s


def read_text(path):
    """读文件，保留 CRLF 与 BOM 信息。"""
    raw = open(path, "rb").read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    crlf = raw.count(b"\r\n")
    text = raw.decode("utf-8-sig")
    return text, bom, crlf


def write_text(path, text, bom, crlf):
    """按原 BOM/换行风格写回。"""
    if crlf:
        text = text.replace("\r\n", "\n").replace("\n", "\r\n")
    data = text.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    open(path, "wb").write(data)


def json_dumps(obj):
    """固定字段顺序 + 2 空格缩进（已验证与现有 node.json 字节一致）。"""
    return json.dumps(obj, ensure_ascii=False, indent=2)


def default_file(node_id):
    s = node_id[4:] if node_id.startswith("alg.") else node_id
    return s.replace(".", "_") + ".node.json"


def assembly_of(cpp_name):
    base = cpp_name[:-len("_comui.cpp")] if cpp_name.endswith("_comui.cpp") else os.path.splitext(cpp_name)[0]
    return base + ".dll"


def iter_exports(text):
    """返回所有 (method, start_index, line_no)。兼容 extern \"C\" 前缀与全限定导出宏。"""
    pat = re.compile(
        r'(?:extern\s+"C"\s+)?\b[A-Z][A-Z0-9_]*_EXPORT\s+int\s+([A-Za-z_]\w*)\s*\(\s*'
        r'const\s+AlgoCInput\s*\*\s*\w+\s*,\s*AlgoCOutput\s*\*\s*\w+\s*\)'
    )
    for m in pat.finditer(text):
        yield m.group(1), m.start(), text.count("\n", 0, m.start()) + 1


# ----------------------------- 标注解析 -----------------------------

def parse_annotations(text, cpp_name):
    """
    返回 (annotations, exported)：
      annotations: method -> {"meta": {...}, "line": int, "file": str}
      exported:    method -> line_no
    """
    exported = {m: ln for m, _, ln in iter_exports(text) if m not in IGNORE_FUNCS}
    exports_pos = [(m, p) for m, p, _ in iter_exports(text)]

    annotations = {}
    pos = 0
    while True:
        b = text.find(BEGIN, pos)
        if b < 0:
            break
        e = text.find(END, b + len(BEGIN))
        if e < 0:
            break
        block = text[b + len(BEGIN):e]
        # 去掉每行行首的 "//"
        lines = []
        for ln in block.splitlines():
            s = ln.strip()
            if s.startswith("//"):
                s = s[2:]
                if s.startswith(" "):
                    s = s[1:]
            lines.append(s)
        raw_json = "\n".join(lines)
        try:
            meta = json.loads(strip_jsonc(raw_json))
        except Exception as ex:
            print(f"[ERROR] {cpp_name} 标注 JSON 解析失败 (line {text.count(chr(10),0,b)+1}): {ex}", file=sys.stderr)
            pos = e + len(END)
            continue

        # 绑定到标注之后最近的导出函数（距离上限 40 行）
        tail = text[e + len(END):]
        method = None
        for m, p in exports_pos:
            if p > e:
                gap_lines = text.count("\n", e, p)
                if gap_lines <= 40:
                    method = m
                break
        if method is None:
            print(f"[ERROR] {cpp_name} 标注未绑定到导出函数 (line {text.count(chr(10),0,b)+1})", file=sys.stderr)
            pos = e + len(END)
            continue

        node_id = (meta.get("node") or {}).get("id", "")
        fname = meta.get("file") or default_file(node_id)
        annotations[method] = {
            "meta": meta,
            "line": text.count("\n", 0, b) + 1,
            "file": fname,
            "cpp": cpp_name,
        }
        pos = e + len(END)
    return annotations, exported


def build_descriptor(anno, method):
    """按固定字段顺序组装与现有 node.json 同构的对象。"""
    meta = anno["meta"]
    return {
        "node": {
            "id": meta.get("node", {}).get("id", ""),
            "name": meta.get("node", {}).get("name", ""),
            "group": meta.get("node", {}).get("group", "通用"),
            "category": meta.get("node", {}).get("category", "处理"),
            "description": meta.get("node", {}).get("description", ""),
        },
        "runtime": "native",
        "assembly": assembly_of(anno["cpp"]),
        "method": method,
        "inputs": meta.get("inputs", []),
        "params": meta.get("params", []),
        "outputs": meta.get("outputs", []),
        "source": meta.get("source", {"files": [], "includes": []}),
    }


def load_all_annotations():
    """扫描 3 个 cpp，返回 method -> anno，以及每个 cpp 的原始文本信息。"""
    all_anno = {}
    files = {}
    for cpp in CPP_FILES:
        path = os.path.join(CPP_DIR, cpp)
        text, bom, crlf = read_text(path)
        annos, exported = parse_annotations(text, cpp)
        all_anno.update(annos)
        files[cpp] = {"text": text, "bom": bom, "crlf": crlf, "annos": annos, "exported": exported}
    return all_anno, files


def load_disk_descriptors():
    out = {}
    for f in glob.glob(os.path.join(ALG_DIR, "*.node.json")):
        j = json.loads(open(f, "rb").read().decode("utf-8-sig"))
        out[j.get("method", "")] = {"file": os.path.basename(f), "json": j}
    return out


# ----------------------------- 子命令 -----------------------------

def cmd_gen(args):
    annos, _ = load_all_annotations()
    if not annos:
        print("[ERROR] 未发现任何 @comui-node 标注；请先运行 backfill 迁移。")
        return 2
    changed, same, missing = 0, 0, []
    for method, anno in sorted(annos.items()):
        target = os.path.join(ALG_DIR, anno["file"])
        new_text = json_dumps(build_descriptor(anno, method)) + "\n"
        if os.path.exists(target):
            old = open(target, "rb").read().decode("utf-8-sig").replace("\r\n", "\n")
            if old == new_text or old == new_text.rstrip("\n"):
                same += 1
                continue
            changed += 1
            if args.verbose:
                print(f"[DIFF] {anno['file']}  ({anno['cpp']}:{anno['line']})")
        else:
            changed += 1
            print(f"[NEW ] {anno['file']}  <- {method}")
        if args.write:
            open(target, "wb").write(new_text.encode("utf-8"))
    print(f"生成: 一致 {same}, 变更/新增 {changed}, 标注总数 {len(annos)}"
          + ("  (已写入)" if args.write else "  (dry-run，加 --write 落盘)"))
    return 0


def cmd_check(args):
    annos, files = load_all_annotations()
    disk = load_disk_descriptors()
    errors = []
    warns = []

    # 1) 导出函数必须都有标注
    for cpp, info in files.items():
        for method in info["exported"]:
            if method not in annos:
                errors.append(f"{cpp}: 导出函数 {method} 无 @comui-node 标注")

    # 2) 标注 ↔ 磁盘 node.json 一致性
    for method, anno in sorted(annos.items()):
        target = os.path.join(ALG_DIR, anno["file"])
        if not os.path.exists(target):
            errors.append(f"{anno['file']}: 标注存在但 node.json 缺失 ({method})")
            continue
        j = json.loads(open(target, "rb").read().decode("utf-8-sig"))
        expect = build_descriptor(anno, method)
        if j != expect:
            for k in expect:
                if j.get(k) != expect[k]:
                    errors.append(f"{anno['file']}: 字段 {k} 与标注不一致 (method={method})")
        if disk.get(method, {}).get("file") != anno["file"]:
            errors.append(f"{anno['file']}: method={method} 与磁盘索引冲突")

    # 3) 磁盘 node.json 必须都有标注
    for method, d in disk.items():
        if method not in annos:
            warns.append(f"{d['file']}: node.json 存在但 cpp 无标注 (method={method})")

    # 4) source.files / includes 引用存在性
    for method, anno in sorted(annos.items()):
        src = anno["meta"].get("source") or {}
        for rel in (src.get("files") or []):
            if not os.path.exists(os.path.join(ALG_DIR, rel.replace("/", os.sep))):
                errors.append(f"{anno['file']}: source.files 引用不存在: {rel}")
        for rel in (src.get("includes") or []):
            if not os.path.exists(os.path.join(ALG_DIR, rel.replace("/", os.sep))):
                errors.append(f"{anno['file']}: source.includes 引用不存在: {rel}")

    # 5) 参数绑定宽松检查（可选）
    if args.params:
        for cpp, info in files.items():
            for method, anno in annos.items():
                if anno["cpp"] != cpp:
                    continue
                need = analyze_read_params(info["text"], method)
                provided = params_kinds(anno["meta"].get("params", []))
                if need and need != provided:
                    warns.append(f"{anno['file']}: 参数 kind 数量可疑 C++读={need} vs JSON声明={provided}"
                                 f"（内联循环/委托读参可能误报，请人工确认）")

    for e in errors:
        print("[ERROR]", e)
    for w in warns:
        print("[WARN ]", w)
    print(f"check: 标注 {len(annos)}, 磁盘 node.json {len(disk)}, 错误 {len(errors)}, 警告 {len(warns)}")
    return 1 if errors else 0


def params_kinds(params):
    c = Counter()
    for p in params:
        k = TYPE2KIND.get(str(p.get("type", "")).strip().lower(), 2)
        c[k] += 1
    return dict(c)


def analyze_read_params(text, method):
    """尽力分析函数体读参 kind 计数。解析失败返回 None（不告警）。"""
    m = re.search(
        r'(?:extern\s+"C"\s+)?\b[A-Z][A-Z0-9_]*_EXPORT\s+int\s+' + re.escape(method) + r'\s*\([^)]*\)\s*\{',
        text,
    )
    if not m:
        return None
    start = m.end() - 1
    depth = 0
    end = None
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end is None:
        return None
    body = text[start:end]
    need = Counter()
    # FindParam(in, kind, occ)
    occ = {}
    for mm in re.finditer(r'FindParam\s*\(\s*\w+\s*,\s*(\d+)\s*,\s*(\d+)\s*\)', body):
        k, o = int(mm.group(1)), int(mm.group(2))
        occ[(k, o)] = True
    for (k, o) in occ:
        need[k] = max(need.get(k, 0), o + 1)
    # GetDoubleParam / GetIntParam / GetTextParam / GetBoolParam
    for pat, k in [(r'\bGetDoubleParam\s*\(', 0), (r'\bGetIntParam\s*\(', 1),
                   (r'\bGetTextParam\s*\(', 2), (r'\bGetBoolParam\s*\(', 3)]:
        # 排除 static 定义行本身
        for mm in re.finditer(pat, body):
            line_start = body.rfind("\n", 0, mm.start()) + 1
            line = body[line_start:mm.start()]
            if "static" not in line:
                need[k] = max(need.get(k, 0), 1)
    # 内联 seen==N 循环
    for km in re.finditer(r'\bkind\s*==\s*(\d+)', body):
        k = int(km.group(1))
        win = body[km.start():km.start() + 600]
        idxs = [int(x) for x in re.findall(r'(?:seen|index|idx)\s*==\s*(\d+)', win)]
        if idxs:
            need[k] = max(need.get(k, 0), max(idxs) + 1)
        elif re.search(r'params\[\w+\]\.(?:d|i|s)', win):
            need[k] = max(need.get(k, 0), 1)
    # 循环填充数组：v[i] = FindParam(in, k, i) 已由上面 occ 覆盖
    return dict(need) if need else None


def cmd_backfill(args):
    """从现有 node.json 反向注入标注到 cpp。幂等：已有标注默认跳过。"""
    disk = load_disk_descriptors()
    # method -> cpp 归属（按 method 前缀）
    prefix_cpp = {
        "Image2DAlg": "Image2DAlg_comui.cpp",
        "Point3DAlg": "Point3DAlg_comui.cpp",
        "BusinessAlg": "BusinessAlg_comui.cpp",
    }
    injected, skipped, nomatch = 0, 0, []
    by_cpp = {cpp: [] for cpp in CPP_FILES}
    for method, d in sorted(disk.items()):
        j = d["json"]
        prefix = method.split("_Comui")[0]
        cpp = prefix_cpp.get(prefix)
        if not cpp:
            nomatch.append(f"{d['file']}: method 前缀 {prefix} 无对应 cpp")
            continue
        # 标注 meta = 去掉脚本推导字段后的子集
        meta = {
            "node": j.get("node", {}),
            "inputs": j.get("inputs", []),
            "params": j.get("params", []),
            "outputs": j.get("outputs", []),
            "source": j.get("source", {}),
        }
        if d["file"] != default_file(j.get("node", {}).get("id", "")):
            meta["file"] = d["file"]
        by_cpp[cpp].append((method, meta, d["file"]))

    for cpp in CPP_FILES:
        path = os.path.join(CPP_DIR, cpp)
        text, bom, crlf = read_text(path)
        existing = {m for m, _ in parse_annotations(text, cpp)[0].items()}
        # 按 method 在文本中从后往前插入，避免行号漂移
        items = sorted(by_cpp[cpp], key=lambda t: text.find(t[0]), reverse=True)
        for method, meta, fname in items:
            if method in existing:
                skipped += 1
                continue
            m = re.search(
                r'(?:extern\s+"C"\s+)?\b[A-Z][A-Z0-9_]*_EXPORT\s+int\s+' + re.escape(method) + r'\s*\(',
                text,
            )
            if not m:
                nomatch.append(f"{fname}: cpp 中找不到函数 {method}")
                continue
            # 找到该行行首插入
            ls = text.rfind("\n", 0, m.start()) + 1
            block = render_annotation(meta)
            text = text[:ls] + block + text[ls:]
            injected += 1
        if args.write:
            write_text(path, text, bom, crlf)

    for x in nomatch:
        print("[WARN ]", x)
    print(f"backfill: 注入 {injected}, 跳过(已有标注) {skipped}, 未匹配 {len(nomatch)}"
          + ("  (已写入)" if args.write else "  (dry-run，加 --write 落盘)"))
    return 0


def render_annotation(meta):
    """把 meta 渲染成紧凑但可读的 C++ 标注块（每行一个字段，数组元素各一行）。"""
    lines = [BEGIN]
    node = meta.get("node", {})
    lines.append("// {")
    # node 一行
    node_str = json.dumps(node, ensure_ascii=False, separators=(",", ":"))
    lines.append(f'//   "node": {node_str},')
    for key in ("inputs", "params", "outputs"):
        arr = meta.get(key, [])
        if not arr:
            lines.append(f'//   "{key}": [],')
            continue
        lines.append(f'//   "{key}": [')
        for i, item in enumerate(arr):
            s = json.dumps(item, ensure_ascii=False, separators=(",", ":"))
            comma = "," if i < len(arr) - 1 else ""
            lines.append(f"//     {s}{comma}")
        lines.append("//   ],")
    src = meta.get("source", {})
    if "file" in meta:
        lines.append(f'//   "file": {json.dumps(meta["file"], ensure_ascii=False)},')
    files = json.dumps(src.get("files", []), ensure_ascii=False, separators=(",", ":"))
    incs = json.dumps(src.get("includes", []), ensure_ascii=False, separators=(",", ":"))
    lines.append(f'//   "source": {{"files": {files}, "includes": {incs}}}')
    lines.append("// }")
    lines.append(END)
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_gen = sub.add_parser("gen", help="从 cpp 标注生成 node.json")
    p_gen.add_argument("--write", action="store_true", help="落盘（默认 dry-run）")
    p_gen.add_argument("-v", "--verbose", action="store_true")
    p_gen.set_defaults(func=cmd_gen)

    p_check = sub.add_parser("check", help="三方一致性校验")
    p_check.add_argument("--params", action="store_true", help="附带参数绑定宽松检查")
    p_check.set_defaults(func=cmd_check)

    p_back = sub.add_parser("backfill", help="从现有 node.json 反向注入标注（首次迁移）")
    p_back.add_argument("--write", action="store_true")
    p_back.set_defaults(func=cmd_backfill)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
