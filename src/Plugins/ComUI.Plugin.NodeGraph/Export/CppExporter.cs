using System.IO;
using System.Text;

namespace ComUI.Plugin.NodeGraph;

/// <summary>
/// 流水线 → C++ 工程一键导出：把当前图导出为**完整可编译**的 C++ 控制台工程
/// （main.cpp + 算子真实源码副本 + comui_native_abi.h + CMakeLists.txt）。
///
/// 生成规则（与宿主运行时语义逐条对齐）：
/// - 节点按拓扑序展开为顺序 C++ 代码；变量名 = 节点安全名（中文/非法字符转 _）。
/// - **native 节点**（runtime=native 且 node.json 声明了 source.files）：
///   ① 把算子源码逐文件拷进导出工程（保持相对路径）；
///   ② main.cpp 直接 extern 声明其入口并调用——**编译进可执行文件，不依赖 DLL**；
///   ③ 生成最小导出宏头（POINT3DALG_EXPORT 等按目标名推导，编译时定义 <Target>_EXPORTS）。
/// - **无源码的 native 节点 / 内置托管节点**：生成 TODO 桩（签名与数据流对齐，注释说明宿主侧行为）。
/// - 数据契约与总线一致：图像 = 紧排 BGRA（CV_8UC4 模式）、点云 = SoA（xyz float×3 + rgb u8×3）。
/// - 环路在导出前检测，直接报错不出代码。
/// </summary>
public static class CppExporter
{
    // ==================== 对外入口 ====================

    /// <summary>导出整张图。返回错误消息（null = 成功）。</summary>
    public static string? Export(GraphDocument graph, string outputDir, string programName)
    {
        var order = TopoOrder(graph);
        if (order is null) return "图中存在环路，无法导出";

        var usedNames = new HashSet<string>();
        var nodes = order.Select(n => (Node: n, Var: SafeName(n, usedNames))).ToList();
        var srcMap = BuildSourceMap(graph, nodes);

        Directory.CreateDirectory(outputDir);

        // 1) 拷贝 native 算子源码（去重：同文件多算子共享）+ 收集 include 目录
        //    同时推导：模块清单（src/<Module>/ 路径 → <MODULE>_BUILD_DLL 宏）与 OpenCV 依赖
        var copiedFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var includeDirs = new List<string>();
        var modules = new HashSet<string>();          // src/<Module>/ → IMAGE2DALG_BUILD_DLL 等
        var needsOpenCv = false;                      // 拷贝的源码引用 opencv2 → CMake 生成 find_package
        var needsGeos = false;                        // 拷贝的源码引用 geos → CMake 链接 geos_c
        var sourceNodes = new Dictionary<string, NodeJsonRegistry.NativeSourceMeta>();   // 节点Id → 源码元数据
        var sourceBaseDirs = new Dictionary<string, string>();   // 节点Id → node.json 所在目录（源码相对路径基准）
        foreach (var (node, _) in nodes)
        {
            var meta = NodeJsonRegistry.TryGetNativeSource(node.DefId);
            if (meta is null) continue;
            sourceNodes[node.Id] = meta;
            sourceBaseDirs[node.Id] = NodeJsonRegistry.GetJsonDir(node.DefId);
            foreach (var inc in meta.Includes) if (!includeDirs.Contains(inc)) includeDirs.Add(inc);
        }
        var copyErrors = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var pendingFiles = new Queue<(string JsonDir, string RelPath)>();
        foreach (var (nodeId, meta) in sourceNodes)
        {
            foreach (var file in meta.Sources)
            {
                // 源码相对路径基准 = node.json 所在目录（comdll/algo/<算子>/）——与 assembly 字段同基准，
                // 第三方交付物把源码随包放在算子目录内即可被找到
                var srcPath = Path.GetFullPath(Path.Combine(sourceBaseDirs[nodeId], file.Replace('/', Path.DirectorySeparatorChar)));
                if (!File.Exists(srcPath)) { copyErrors.Add(srcPath); continue; }
                var destRel = SanitizeRelPath(file);
                if (copiedFiles.Contains(destRel)) continue;
                if (!CopyOne(outputDir, sourceBaseDirs[nodeId], destRel, srcPath, copiedFiles, modules, ref needsOpenCv, ref needsGeos)) continue;
                pendingFiles.Enqueue((sourceBaseDirs[nodeId], destRel));
            }
        }

        // 1.5) 引用闭包：扫描已拷源码的 #include "xxx"，在算子源码树里解析缺失头并自动补拷，
        //      循环到收敛——交付清单（source.files）漏头不再导致导出工程编译失败
        while (pendingFiles.Count > 0)
        {
            var (jsonDir, rel) = pendingFiles.Dequeue();
            var text = File.ReadAllText(Path.Combine(outputDir, rel));
            foreach (var inc in ParseQuotedIncludes(text))
            {
                if (copiedFiles.Any(c => c.EndsWith(inc.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase))) continue;
                // 解析顺序：含方文件目录 → json 目录 → json/src → include 目录映射（json 目录/该目录）
                string? hit = null;
                var bases = new[] { Path.GetDirectoryName(Path.Combine(jsonDir, rel))!, jsonDir, Path.Combine(jsonDir, "src") }
                            .Concat(includeDirs.Select(d => Path.Combine(jsonDir, d)));
                foreach (var b in bases)
                {
                    var cand = Path.GetFullPath(Path.Combine(b, inc.Replace('/', Path.DirectorySeparatorChar)));
                    if (File.Exists(cand) && cand.StartsWith(jsonDir, StringComparison.OrdinalIgnoreCase)) { hit = cand; break; }
                }
                if (hit is null) continue;   // 系统头/第三方角括号包含或不在源码树里——导出工程按需处理
                var incRel = SanitizeRelPath(Path.GetRelativePath(jsonDir, hit));
                if (copiedFiles.Contains(incRel)) continue;
                if (!CopyOne(outputDir, jsonDir, incRel, hit, copiedFiles, modules, ref needsOpenCv, ref needsGeos)) continue;
                pendingFiles.Enqueue((jsonDir, incRel));
            }
        }

        // 2) 运行时 DLL（第三方依赖：opencv/pcl/geos/OpenNI2 等）——拷进 runtime/，
        //    CMake POST_BUILD 复制到 exe 目录（加载器在 exe 同目录找到，导出工程开箱即跑）
        var runtimeDlls = new List<string>();
        foreach (var dir in sourceBaseDirs.Values.Distinct())
        {
            foreach (var dll in Directory.EnumerateFiles(dir, "*.dll"))
                if (!runtimeDlls.Any(d => d.EndsWith(Path.GetFileName(dll), StringComparison.OrdinalIgnoreCase)))
                    runtimeDlls.Add(dll);
        }
        if (runtimeDlls.Count > 0)
        {
            Directory.CreateDirectory(Path.Combine(outputDir, "runtime"));
            foreach (var dll in runtimeDlls)
                File.Copy(dll, Path.Combine(outputDir, "runtime", Path.GetFileName(dll)), true);
        }

        // 3) 生成 main.cpp + ABI 头 + CMakeLists
        // C++ 源文件带 UTF-8 BOM：MSVC 对无 BOM 的 UTF-8 按 ANSI/GBK 误读，中文注释的字节
        // 会把 #endif 等预处理行"吃掉"（实测报"意外的 #endif"）——BOM 是 MSVC 识别 UTF-8 的唯一开关
        var bomUtf8 = new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: true);
        File.WriteAllText(Path.Combine(outputDir, "comui_native_abi.h"), AbiHeader(), bomUtf8);
        File.WriteAllText(Path.Combine(outputDir, "main.cpp"), MainCpp(graph, nodes, srcMap, sourceNodes), bomUtf8);
        File.WriteAllText(Path.Combine(outputDir, "CMakeLists.txt"), CMake(programName, copiedFiles, includeDirs, modules, needsOpenCv, needsGeos, runtimeDlls.Count > 0), bomUtf8);

        if (copyErrors.Count > 0)
            return "以下源码文件未找到（node.json source.files 路径错误）：" + string.Join("、", copyErrors);
        return null;
    }

    /// <summary>拷一个源文件到导出工程并登记模块/OpenCV/GEOS 推导。返回 false = 跳过（独立 main 的工具 exe）。</summary>
    private static bool CopyOne(string outputDir, string jsonDir, string destRel, string srcPath,
        HashSet<string> copiedFiles, HashSet<string> modules, ref bool needsOpenCv, ref bool needsGeos)
    {
        var bytes = File.ReadAllBytes(srcPath);
        // 独立工具 exe：自带 int main( 与导出工程的 main 冲突（LNK2005）——跳过不拷。
        // 按行判定：行内 main 之前有 // 或 /* 视为注释（nlohmann/json.hpp 的示例注释里就有
        // "//   int main("——整文件级查找会误杀整个头）。这类文件只服务于算法作者的手工调试，
        // 宿主/导出链路不引用其符号。
        if (HasStandaloneMain(bytes)) return false;
        Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(outputDir, destRel))!);
        File.WriteAllBytes(Path.Combine(outputDir, destRel), bytes);
        copiedFiles.Add(destRel);
        // 模块推导：src/<Module>/<file> 三段才算（src 根下的散文件如 AlgExport.h 不属于任何模块）
        var segs = destRel.Replace('\\', '/').Split('/');
        if (segs.Length >= 3 && segs[0] == "src")
            modules.Add(segs[1].ToUpperInvariant());
        // OpenCV/GEOS 依赖探测：源码内容引用 → CMake 需 find_package/link（内容按字节找，ASCII 串与编码无关）
        if (IndexOfAscii(bytes, "#include <opencv2/") >= 0) needsOpenCv = true;
        if (IndexOfAscii(bytes, "geos::") >= 0 || IndexOfAscii(bytes, "geos_c") >= 0) needsGeos = true;
        return true;
    }

    /// <summary>解析源码里的 #include "xxx"（引号包含——系统/第三方角括号包含不在闭包范围）。</summary>
    private static List<string> ParseQuotedIncludes(string text)
    {
        var result = new List<string>();
        var idx = 0;
        while ((idx = text.IndexOf("#include \"", StringComparison.Ordinal)) >= 0)
        {
            var start = idx + "#include \"".Length;
            var end = text.IndexOf('"', start);
            if (end < 0) break;
            result.Add(text[start..end]);
            text = text[(end + 1)..];
        }
        return result;
    }

    /// <summary>按行判定是否含独立 main（行内 main 之前有 // 或 /* = 注释示例，不算）。</summary>
    private static bool HasStandaloneMain(byte[] data)
    {
        var start = 0;
        for (int i = 0; i <= data.Length; i++)
        {
            if (i == data.Length || data[i] == (byte)'\n')
            {
                var line = System.Text.Encoding.ASCII.GetString(data, start, i - start);
                var m = line.IndexOf("int main(", StringComparison.Ordinal);
                if (m >= 0)
                {
                    var prefix = line[..m];
                    if (!prefix.Contains("//") && !prefix.Contains("/*")) return true;
                }
                start = i + 1;
            }
        }
        return false;
    }

    /// <summary>在原始字节里找 ASCII 子串（编码无关——UTF-8/GBK 下 ASCII 段字节相同）。</summary>
    private static int IndexOfAscii(byte[] data, string ascii)
    {
        var pat = System.Text.Encoding.ASCII.GetBytes(ascii);
        for (int i = 0; i + pat.Length <= data.Length; i++)
        {
            int j = 0;
            while (j < pat.Length && data[i + j] == pat[j]) j++;
            if (j == pat.Length) return i;
        }
        return -1;
    }

    /// <summary>相对路径净化：折叠 ".."/"." 段，防止源文件越界写到导出工程外；空结果回退占位名。</summary>
    private static string SanitizeRelPath(string rel)
    {
        var parts = rel.Replace('\\', '/').Split('/', StringSplitOptions.RemoveEmptyEntries);
        var kept = new List<string>();
        foreach (var p in parts)
        {
            if (p == "..") { if (kept.Count > 0) kept.RemoveAt(kept.Count - 1); continue; }
            if (p == ".") continue;
            kept.Add(p);
        }
        if (kept.Count == 0) return Path.Combine("src", "unknown.cpp");
        return Path.Combine(kept.ToArray());
    }

    /// <summary>(节点Id, 输入端口名) → (源变量名, 源 NodeDef, 源输出端口名)。</summary>
    private static Dictionary<(string Node, string Port), (string Var, NodeDef Def, string OutPort)> BuildSourceMap(
        GraphDocument graph, List<(NodeInstance Node, string Var)> nodes)
    {
        var map = new Dictionary<(string, string), (string, NodeDef, string)>();
        var nameOf = nodes.ToDictionary(kv => kv.Node.Id, kv => kv.Var);
        foreach (var c in graph.Connections)
        {
            var from = graph.FindNode(c.FromNode);
            var to = graph.FindNode(c.ToNode);
            if (from is null || to is null) continue;
            if (!nameOf.TryGetValue(from.Id, out var v)) continue;
            if (NodeDefs.Map[from.DefId].Id is "flow.for" or "flow.while") continue;   // 控制边不传数据
            map[(to.Id, c.ToPort)] = (v, NodeDefs.Map[from.DefId], c.FromPort);
        }
        return map;
    }

    // ==================== 代码生成 ====================

    private static string MainCpp(GraphDocument graph, List<(NodeInstance Node, string Var)> nodes,
        Dictionary<(string Node, string Port), (string Var, NodeDef Def, string OutPort)> srcMap,
        Dictionary<string, NodeJsonRegistry.NativeSourceMeta> sourceNodes)
    {
        var sb = new StringBuilder();
        sb.AppendLine("// 由 ComUI「算法流水线」一键导出 —— 完整可编译的 C++ 控制台工程。");
        sb.AppendLine("// 算子源码已随工程拷贝并直接编译进可执行文件（不依赖宿主与原 DLL）。");
        sb.AppendLine("// 数据契约：图像 = 紧排 BGRA（宽×高×4 字节）；点云 = SoA（xyz float[3n] + rgb u8[3n]）。");
        sb.AppendLine("// 无源码的节点 = TODO 桩（内置测试源/发布等宿主特有语义），按注释自行实现。");
        sb.AppendLine("#include \"comui_native_abi.h\"");
        sb.AppendLine();
        sb.AppendLine("#include <cstdio>");
        sb.AppendLine("#include <cstring>");
        sb.AppendLine("#include <string>");
        sb.AppendLine();

        // ---- 有源码的 native 入口：extern "C" 直接声明（编译进可执行文件） ----
        foreach (var (nodeId, meta) in sourceNodes)
        {
            var node = nodes.FirstOrDefault(kv => kv.Node.Id == nodeId);
            if (node.Node is null) continue;
            var def = NodeDefs.Map[node.Node.DefId];
            sb.AppendLine($"// {def.Name} <{def.Id}> —— 源码已拷入本工程（{string.Join(", ", meta.Sources.Select(Path.GetFileName))}）");
            sb.AppendLine($"extern \"C\" int {meta.Entry}(const AlgoCInput* in, AlgoCOutput* out);");
            sb.AppendLine("extern \"C\" void algo_free(void* p);");
            sb.AppendLine();
        }

        // ---- 循环映射（先于逐节点发射：EmitSourceCall 的模板替换要引用计数变量名） ----
        var flowVars = nodes.Where(kv => kv.Node.DefId is "flow.for" or "flow.while")
                            .ToDictionary(kv => kv.Var, kv => kv.Node);
        var closureOf = new Dictionary<string, HashSet<string>>();   // flow 节点 → 下游闭包节点 Id 集
        foreach (var kv in nodes.Where(kv => flowVars.ContainsKey(kv.Var)))
        {
        var closure = new HashSet<string> { kv.Node.Id };
        var q = new Queue<NodeInstance>();
        q.Enqueue(kv.Node);
        while (q.Count > 0)
        {
            var cur = q.Dequeue();
            foreach (var c in graph.Connections.Where(c => c.FromNode == cur.Id))
            {
                var nxt = graph.Nodes.FirstOrDefault(n2 => n2.Id == c.ToNode);
                if (nxt is null || !closure.Add(nxt.Id)) continue;
                q.Enqueue(nxt);
            }
        }
        closure.Remove(kv.Node.Id);   // 自身不进闭包（由 for 块自身承载）
        closureOf[kv.Node.Id] = closure;
        }

        // 循环计数全局变量映射（模板 {循环名.计数} → std::to_string(g_i_<var>)）：
        // loopIvByLabel = 显示名（Title 优先）→ 计数变量；defaultIv = 图中只有一个循环时裸 {计数} 的目标
        var ivOfFlowNode = flowVars.ToDictionary(kv => kv.Value.Id, kv => "g_i_" + kv.Key);
        // 同名循环取注册序最后一个（与宿主 ApplyTemplates "同名后者覆盖" 对齐）
        var loopIvByLabel = flowVars.Values
            .GroupBy(n => n.Title ?? NodeDefs.Map[n.DefId].Name)
            .ToDictionary(g => g.Key, g => ivOfFlowNode[g.Last().Id]);
        var defaultIv = flowVars.Count == 1 ? ivOfFlowNode[flowVars.Values.First().Id] : null;

        // ---- 每节点：有源码 = 直调；无源码/内置 = TODO 桩；flow 控制节点 = 循环块（不生成函数） ----
        foreach (var (node, var) in nodes)
        {
            var def = NodeDefs.Map[node.DefId];
            if (node.DefId is "flow.for" or "flow.while")
            {
                sb.AppendLine($"// flow 控制节点 {def.Name} —— main 中生成真实 C++ 循环（作用域=下游闭包）");
                // 计数为文件级全局：_run() 定义在 main 之外，作用域内参数模板 {循环名.计数} 才能引用到
                sb.AppendLine($"static int g_i_{var} = 0;");
                continue;
            }
            sb.AppendLine($"// ==================== {def.Name}（{node.Id[..6]}）====================");
            if (sourceNodes.TryGetValue(node.Id, out var meta))
            {
                // 模板守卫（与宿主对齐）：只有每轮迭代都执行的节点（在任一循环闭包内）才解析 {..计数}——
                // 作用域外节点宿主侧 counters=null 保留原文，导出端同样回退字面量
                bool perIteration = flowVars.Any(kv => closureOf[kv.Value.Id].Contains(node.Id));
                EmitSourceCall(sb, node, def, var, meta, srcMap, perIteration, loopIvByLabel, defaultIv);
            }
            else
                EmitBuiltinStub(sb, node, def, var);
            sb.AppendLine();
        }

        // ---- main：按拓扑序展开；flow.for/while 节点生成真实 C++ 循环（作用域=下游闭包，嵌套自动处理） ----
        var emittedCalls = new HashSet<string>();
        void EmitCalls(StringBuilder sb, List<(NodeInstance Node, string Var)> seq)
        {
            foreach (var (node, var) in seq)
            {
                if (!emittedCalls.Add(var)) continue;
                if (flowVars.TryGetValue(var, out var loopNode))
                {
                    int start = loopNode.Params.TryGetValue("起始", out var sv) && sv is int s1 ? s1 : 0;
                    int end = loopNode.Params.TryGetValue("终止", out var ev) && ev is int e1 ? e1 : 10;
                    int step = loopNode.Params.TryGetValue("步长", out var tv) && tv is int t1 ? t1 : 1;
                    if (step == 0) step = 1;
                    var cmp = step > 0 ? "<" : ">";
                    var iv = "g_i_" + var;
                    var closureIds = closureOf[node.Id];
                    sb.AppendLine($"    // flow 循环：作用域 {closureIds.Count} 个节点，计数变量 {iv}");
                    sb.AppendLine($"    for ({iv} = {start}; {iv} {cmp} {end}; {iv} += {step}) {{");
                    EmitCalls(sb, nodes.Where(p2 => closureIds.Contains(p2.Node.Id)).ToList());
                    sb.AppendLine("    }");
                }
                else
                {
                    sb.AppendLine($"    {var}_run();");
                }
            }
        }

        sb.AppendLine("int main() {");
        EmitCalls(sb, nodes);
        foreach (var (node, var) in nodes)
        {
            var def = NodeDefs.Map[node.DefId];
            if (node.DefId is "flow.for" or "flow.while")
                continue;   // flow 控制节点无数据输出（计数在循环日志可见），不参与汇总
            for (int i = 0; i < def.Outputs.Length; i++)
            {
                var unit = def.Outputs[i].Type == PortType.Image ? "px" : "点";
                var cnt = def.Outputs[i].Type == PortType.Image ? $"{var}_out_{i}.width * {var}_out_{i}.height" : $"{var}_out_{i}.count";
                // 算子名/端口名嵌进 printf 格式串：% 必须转 %%（Escape 不管 %——名字含 % 会变成非法格式符）
                var label = Escape(def.Name).Replace("%", "%%");
                var port = Escape(def.Outputs[i].Name).Replace("%", "%%");
                sb.AppendLine($"    printf(\"{label}[{port}]: %d {unit}\\n\", (int)({cnt}));");
            }
        }
        sb.AppendLine("    return 0;");
        sb.AppendLine("}");
        return sb.ToString();
    }

    /// <summary>有源码的 native 节点：组装 AlgoCInput → 直接调用入口（编译期链接，无 DLL）。
    /// 循环模板参数：字符串参数含 {计数}/{循环名.计数} → 生成 const std::string 局部变量 + std::to_string 表达式。</summary>
    private static void EmitSourceCall(StringBuilder sb, NodeInstance node, NodeDef def, string var,
        NodeJsonRegistry.NativeSourceMeta meta,
        Dictionary<(string Node, string Port), (string Var, NodeDef Def, string OutPort)> srcMap,
        bool perIteration,
        IReadOnlyDictionary<string, string> loopIvByLabel,
        string? defaultIv)
    {
        // 输出变量（文件级 static，供下游连线引用）
        for (int i = 0; i < def.Outputs.Length; i++)
            sb.AppendLine($"static AlgoC{TypeOf(def.Outputs[i].Type)} {var}_out_{i};  // 输出「{def.Outputs[i].Name}」");
        sb.AppendLine();

        sb.AppendLine($"static void {var}_run() {{");
        sb.AppendLine($"    // 算子：{def.Name} <{def.Id}>（源码 {Path.GetFileName(meta.Sources.FirstOrDefault() ?? "")}!{meta.Entry}）");

        // 参数（按 JSON 声明顺序；值取节点当前参数）
        var paramNames = def.Params.Select(p => p.Name).ToList();
        sb.AppendLine("    // 参数（当前流水线里调好的值）");
        for (int i = 0; i < paramNames.Count; i++)
        {
            var (kind, d, iv, s) = ParamLiteral(node, def.Params[i]);
            if (perIteration && kind == "2" && TemplateToCpp(node.Params.TryGetValue(paramNames[i], out var rv) ? rv?.ToString() ?? "" : def.Params[i].Default?.ToString() ?? "",
                                             loopIvByLabel, defaultIv) is { } expr)
            {
                // 循环模板：每次迭代重算——std::string 局部变量存活到函数尾，.c_str() 安全
                sb.AppendLine($"    const std::string p_{i}_s = {expr};");
                sb.AppendLine($"    AlgoCParam p_{i} = {{{kind}, {d}, {iv}, p_{i}_s.c_str()}};  // {paramNames[i]}（循环模板）");
            }
            else
                sb.AppendLine($"    AlgoCParam p_{i} = {{{kind}, {d}, {iv}, {s}}};  // {paramNames[i]}");
        }
        if (paramNames.Count == 0) sb.AppendLine("    // 本算子无参数");

        // 输入（按端口连线取上游输出）
        sb.AppendLine();
        sb.AppendLine("    // 输入（来自上游连线）");
        for (int i = 0; i < def.Inputs.Length; i++)
        {
            if (!srcMap.TryGetValue((node.Id, def.Inputs[i].Name), out var src))
            {
                sb.AppendLine($"    // 输入「{def.Inputs[i].Name}」未连线 —— 宿主运行时该节点会被跳过；离线版按需补");
                sb.AppendLine($"    AlgoC{TypeOf(def.Inputs[i].Type)} in_{i} = {{}};");
            }
            else
            {
                var sIdx = Math.Max(0, Array.FindIndex(src.Def.Outputs, o => o.Name == src.OutPort));
                sb.AppendLine($"    AlgoC{TypeOf(def.Inputs[i].Type)} in_{i} = {src.Var}_out_{sIdx};  // ← {src.Var}「{src.OutPort}」");
            }
        }
        if (def.Inputs.Length == 0) sb.AppendLine("    // 本算子无输入端口");

        // 组装 AlgoCInput
        sb.AppendLine();
        sb.AppendLine("    AlgoCParam params_arr[] = {");
        for (int i = 0; i < paramNames.Count; i++) sb.AppendLine($"        p_{i},");
        if (paramNames.Count == 0) sb.AppendLine("        {}");
        sb.AppendLine("    };");
        sb.AppendLine("    AlgoCImage images_arr[] = {");
        for (int i = 0; i < def.Inputs.Length; i++)
        {
            var f = def.Inputs[i].Type == PortType.Image ? $"in_{i}.width, in_{i}.height, in_{i}.bgra" : "0, 0, nullptr";
            sb.AppendLine($"        {{{f}}},");
        }
        if (def.Inputs.Length == 0) sb.AppendLine("        {}");
        sb.AppendLine("    };");
        sb.AppendLine("    AlgoCCloud clouds_arr[] = {");
        for (int i = 0; i < def.Inputs.Length; i++)
        {
            var f = def.Inputs[i].Type == PortType.Cloud ? $"in_{i}.count, in_{i}.xyz, in_{i}.rgb" : "0, nullptr, nullptr";
            sb.AppendLine($"        {{{f}}},");
        }
        if (def.Inputs.Length == 0) sb.AppendLine("        {}");
        sb.AppendLine("    };");
        sb.AppendLine($"    AlgoCInput cin = {{ {paramNames.Count}, params_arr, {def.Inputs.Count(i => i.Type == PortType.Image)}, images_arr, {def.Inputs.Count(i => i.Type == PortType.Cloud)}, clouds_arr }};");

        // 调用 + 输出提取
        sb.AppendLine();
        sb.AppendLine("    AlgoCOutput cout_ = {};");
        sb.AppendLine($"    if ({meta.Entry}(&cin, &cout_) != 0) {{");
        sb.AppendLine("        printf(\"算子失败: %s\\n\", cout_.error ? cout_.error : \"未知错误\");");
        sb.AppendLine("        return;");
        sb.AppendLine("    }");
        for (int i = 0; i < def.Outputs.Length; i++)
        {
            var outType = def.Outputs[i].Type;
            var arr = outType == PortType.Image ? "images" : "clouds";
            var fields = outType == PortType.Image ? "cout_.images[i].width, cout_.images[i].height, cout_.images[i].bgra"
                                                    : "cout_.clouds[i].count, cout_.clouds[i].xyz, cout_.clouds[i].rgb";
            var fieldRefs = fields.Replace("[i]", $"[{i}]");
            var countField = outType == PortType.Image ? "imageCount" : "cloudCount";
            sb.AppendLine($"    if (cout_.{countField} > {i}) {var}_out_{i} = {{ {fieldRefs} }};  // 输出「{def.Outputs[i].Name}」");
        }
        sb.AppendLine("    // 释放约定：输出缓冲由 algo_free 释放；导出版浅拷贝进 out 变量传给下游，程序退出交还操作系统");
        sb.AppendLine("    //       （离线版需要精细释放时，在下游消费完后对 cout_ 内部缓冲调 algo_free）。");
        sb.AppendLine("}");
    }

    /// <summary>内置/托管节点：TODO 桩（签名与数据流对齐）。</summary>
    private static void EmitBuiltinStub(StringBuilder sb, NodeInstance node, NodeDef def, string var)
    {
        for (int i = 0; i < def.Outputs.Length; i++)
            sb.AppendLine($"static AlgoC{TypeOf(def.Outputs[i].Type)} {var}_out_{i};  // 输出「{def.Outputs[i].Name}」");
        sb.AppendLine();
        sb.AppendLine($"// TODO 桩：宿主内置算子「{def.Name}」 <{def.Id}>（{def.Category}）——离线环境自行实现。");
        sb.AppendLine($"// 宿主行为：{(def.Category switch { "源" => "生成数据（测试图案/点云）供下游", "输出" => "把上游数据发布到总线主题（离线版通常改为写文件）", _ => "处理上游数据并输出" })}。");
        sb.AppendLine($"static void {var}_run() {{");
        foreach (var p in def.Params)
        {
            var (kind, d, iv, s) = ParamLiteral(node, p);
            var val = s != "nullptr" ? s : d != "0" ? d : iv;
            sb.AppendLine($"    (void)0;  // 参数 {p.Name} = {val}（{p.Label}）");
        }
        sb.AppendLine("    // TODO: 填充输出变量");
        sb.AppendLine("}");
    }

    // ==================== 辅助 ====================

    private static string TypeOf(PortType t) => t == PortType.Cloud ? "Cloud" : "Image";

    private static (string Kind, string D, string I, string S) ParamLiteral(NodeInstance node, ComUI.Sdk.AlgoParam p)
    {
        var v = node.Params.TryGetValue(p.Name, out var raw) ? raw : p.Default;
        return p.Type switch
        {
            ComUI.Sdk.AlgoParamType.Number => ("0", FormattableString.Invariant($"{Convert.ToDouble(v)}"), "0", "nullptr"),
            ComUI.Sdk.AlgoParamType.Int => ("1", "0", Convert.ToInt32(v).ToString(), "nullptr"),
            ComUI.Sdk.AlgoParamType.Boolean => ("3", "0", Convert.ToBoolean(v) ? "1" : "0", "nullptr"),
            _ => ("2", "0", "0", $"\"{Escape(v?.ToString() ?? "")}\""),
        };
    }

    /// <summary>参数模板正则：{...} 占位（不跨嵌套）——与 GraphExecutor 同语义。</summary>
    private static readonly System.Text.RegularExpressions.Regex CppTplRegex =
        new(@"\{([^{}]+)\}", System.Text.RegularExpressions.RegexOptions.Compiled);

    /// <summary>字符串参数的循环模板 → C++ 拼接表达式："case_{计数}" → "case_" + std::to_string(g_i_v)。
    /// 无占位符 / 有占位符但解析不了（多循环裸 {计数}、循环名未命中）→ 返回 null（调用方回退字面量，与宿主行为一致）。</summary>
    private static string? TemplateToCpp(string raw, IReadOnlyDictionary<string, string> loopIvByLabel, string? defaultIv)
    {
        if (!raw.Contains('{')) return null;
        var sb = new StringBuilder();
        int pos = 0;
        foreach (System.Text.RegularExpressions.Match m in CppTplRegex.Matches(raw))
        {
            var expr = m.Groups[1].Value.Trim();
            string? iv;
            if (expr == "计数") iv = defaultIv;
            else if (expr.EndsWith(".计数")) loopIvByLabel.TryGetValue(expr[..^3], out iv);
            else iv = null;
            if (iv is null) return null;   // 有解析不了的占位符 → 整体回退字面量（不生成半吊子表达式）
            if (m.Index > pos) AppendLit(sb, raw[pos..m.Index]);
            if (sb.Length > 0) sb.Append(" + ");
            sb.Append("std::to_string(").Append(iv).Append(')');
            pos = m.Index + m.Length;
        }
        if (pos == 0) return null;   // 没有可解析占位符
        if (pos < raw.Length) { if (sb.Length > 0) sb.Append(" + "); AppendLit(sb, raw[pos..]); }
        return sb.Length == 0 ? "\"\"" : sb.ToString();
    }

    private static void AppendLit(StringBuilder sb, string s) => sb.Append('"').Append(Escape(s)).Append('"');

    private static string Escape(string s) => s.Replace("\\", "\\\\").Replace("\"", "\\\"");

    private static string SafeName(NodeInstance node, HashSet<string> used)
    {
        var def = NodeDefs.Map[node.DefId];
        var baseName = new string(def.Name.Select(ch => char.IsLetterOrDigit(ch) ? ch : '_').ToArray());
        var name = $"n_{baseName}";
        int i = 1;
        while (!used.Add(name)) name = $"n_{baseName}_{i++}";
        return name;
    }

    private static List<NodeInstance>? TopoOrder(GraphDocument graph)
    {
        var byId = graph.Nodes.ToDictionary(n => n.Id);
        var inDegree = graph.Nodes.ToDictionary(n => n.Id, _ => 0);
        var adjacency = graph.Nodes.ToDictionary(n => n.Id, _ => new List<string>());
        foreach (var c in graph.Connections)
        {
            if (!byId.ContainsKey(c.FromNode) || !byId.ContainsKey(c.ToNode)) continue;
            inDegree[c.ToNode]++;
            adjacency[c.FromNode].Add(c.ToNode);
        }
        var queue = new Queue<string>(inDegree.Where(kv => kv.Value == 0).Select(kv => kv.Key));
        var order = new List<NodeInstance>();
        while (queue.Count > 0)
        {
            var id = queue.Dequeue();
            order.Add(byId[id]);
            foreach (var next in adjacency[id])
                if (--inDegree[next] == 0) queue.Enqueue(next);
        }
        return order.Count == graph.Nodes.Count ? order : null;
    }

    private static string CMake(string programName, HashSet<string> copiedFiles, List<string> includeDirs,
        HashSet<string> modules, bool needsOpenCv, bool needsGeos, bool hasRuntimeDlls)
    {
        var safe = new string(programName.Select(ch => char.IsLetterOrDigit(ch) ? ch : '_').ToArray());
        var sb = new StringBuilder();
        sb.AppendLine("# 由 ComUI 一键导出 —— CMake 构建（g++/MSVC 均可）");
        sb.AppendLine("cmake_minimum_required(VERSION 3.16)");
        sb.AppendLine($"project({safe} LANGUAGES CXX)");
        sb.AppendLine("set(CMAKE_CXX_STANDARD 17)");
        sb.AppendLine();
        sb.AppendLine("# 算子源码（从算子库拷贝的真实实现，与 main.cpp 一起编译）");
        sb.AppendLine("set(OPERATOR_SOURCES");
        foreach (var f in copiedFiles)
            sb.AppendLine($"    \"{f.Replace('\\', '/')}\"");
        sb.AppendLine(")");
        sb.AppendLine();
        sb.AppendLine("add_executable(" + safe);
        sb.AppendLine("    main.cpp");
        sb.AppendLine("    ${OPERATOR_SOURCES}");
        sb.AppendLine(")");
        // 模块导出宏（AlgExport.h 约定）：源码直接编译进 exe 时必须定义 <MODULE>_BUILD_DLL，
        // 否则宏展开成 dllimport → 函数定义报 C2491。模块清单从源码路径 src/<Module>/ 推导。
        if (modules.Count > 0)
        {
            sb.AppendLine($"target_compile_definitions({safe} PRIVATE");
            foreach (var m in modules)
                sb.AppendLine($"    {m}_BUILD_DLL");
            sb.AppendLine(")");
        }
        // include 目录：相对路径挂导出工程根（${CMAKE_SOURCE_DIR}）；绝对路径（第三方头，如
        // PCL/Eigen/Boost）原样输出——绝对路径硬拼前缀会变成非法路径
        foreach (var inc in includeDirs)
        {
            var incPath = inc.Replace('\\', '/');
            if (Path.IsPathRooted(inc)) sb.AppendLine($"target_include_directories({safe} PRIVATE \"{incPath}\")");
            else sb.AppendLine($"target_include_directories({safe} PRIVATE \"${{CMAKE_SOURCE_DIR}}/{incPath}\")");
        }
        // OpenCV：算子源码引用 opencv2 时由 CMake 查找（路径不对时 -DOpenCV_DIR=<OpenCV 构建目录>）
        if (needsOpenCv)
        {
            sb.AppendLine();
            sb.AppendLine("# 算子源码引用了 OpenCV——本机路径不对时加 -DOpenCV_DIR=<OpenCV 配置目录>");
            sb.AppendLine("find_package(OpenCV REQUIRED)");
            sb.AppendLine($"target_include_directories({safe} PRIVATE ${{OpenCV_INCLUDE_DIRS}})");
            sb.AppendLine($"target_link_libraries({safe} PRIVATE ${{OpenCV_LIBS}})");
        }
        // PCL：pcl 头的 #pragma 自动链接 boost/pcl 库——lib 目录从 json includes 里的 pcl-1.9 路径推导
        // （…/PCL-1.9.1/include/pcl-1.9 → 根 …/PCL-1.9.1 → lib 与 3rdParty/Boost/lib）
        var pclInc = includeDirs.FirstOrDefault(d => d.Replace('\\', '/').Contains("PCL-1.9.1/include/pcl-1.9"));
        if (pclInc is not null)
        {
            var pclRoot = Path.GetFullPath(Path.Combine(pclInc, "..", ".."));
            sb.AppendLine();
            sb.AppendLine("# PCL 头的 #pragma 自动链接 boost/pcl 库——lib 目录从 PCL 根推导");
            sb.AppendLine($"target_link_directories({safe} PRIVATE");
            sb.AppendLine($"    \"{(pclRoot + "/lib").Replace('\\', '/')}\"");
            sb.AppendLine($"    \"{(pclRoot + "/3rdParty/Boost/lib").Replace('\\', '/')}\"");
            // FLANN lib 目录（json includes 里的 FLANN/include 同级）
            var flannInc = includeDirs.FirstOrDefault(d => d.Replace('\\', '/').EndsWith("3rdParty/FLANN/include"));
            if (flannInc is not null)
                sb.AppendLine($"    \"{(Path.GetFullPath(Path.Combine(flannInc, "..", "lib"))).Replace('\\', '/')}\"");
            sb.AppendLine(")");
            // PCL 算法库：kitchen-sink 适配层引用全模块（registration/features/octree/io/…）——GLOB 全链接
            sb.AppendLine($"file(GLOB PCL_LIBS \"{(pclRoot + "/lib/pcl_*_release.lib").Replace('\\', '/')}\")");
            sb.AppendLine($"target_link_libraries({safe} PRIVATE ${{PCL_LIBS}} flann.lib)");
        }
        // GEOS：shapely 源码引用——lib 目录从 json includes 里的 geos include 路径推导
        if (needsGeos)
        {
            var geosInc = includeDirs.FirstOrDefault(d => d.Replace('\\', '/').EndsWith("geos-3.15.0/include"));
            if (geosInc is not null)
            {
                var geosLib = Path.GetFullPath(Path.Combine(geosInc, "..", "lib")).Replace('\\', '/');
                sb.AppendLine();
                sb.AppendLine("# GEOS：shapely 源码引用——lib 目录从 include 路径推导");
                sb.AppendLine($"target_link_directories({safe} PRIVATE \"{geosLib}\")");
                sb.AppendLine($"target_link_libraries({safe} PRIVATE geos_c.lib)");
            }
        }
        // 运行时依赖 DLL（opencv/pcl/geos/OpenNI2 等）构建后自动复制到 exe 目录——开箱即跑
        if (hasRuntimeDlls)
        {
            sb.AppendLine();
            sb.AppendLine("# 运行时依赖 DLL：构建后复制到 exe 目录");
            sb.AppendLine($"add_custom_command(TARGET {safe} POST_BUILD");
            sb.AppendLine("    COMMAND ${CMAKE_COMMAND} -E copy_directory");
            sb.AppendLine("    \"${CMAKE_SOURCE_DIR}/runtime\"");
            sb.AppendLine("    $<TARGET_FILE_DIR:" + safe + ">");
            sb.AppendLine(")");
        }
        return sb.ToString();
    }

    private static string AbiHeader() => """
// ComUI 原生算子统一 C ABI（v1）——与宿主 docs/THIRD_PARTY_OPS.md §5.5 一致（导出副本）。
#ifndef COMUI_NATIVE_ABI_H
#define COMUI_NATIVE_ABI_H
#include <stdint.h>

typedef struct AlgoCParam {
    int32_t kind;          // 0=double 1=int32 2=text(UTF-8) 3=bool(i!=0)
    double  d;
    int32_t i;
    const char* s;
} AlgoCParam;

typedef struct AlgoCImage { int32_t width, height; const uint8_t* bgra; } AlgoCImage;
typedef struct AlgoCCloud { int32_t count; const float* xyz; const uint8_t* rgb; } AlgoCCloud;

typedef struct AlgoCInput {
    int32_t paramCount;  const AlgoCParam* params;
    int32_t imageCount;  const AlgoCImage* images;
    int32_t cloudCount;  const AlgoCCloud* clouds;
} AlgoCInput;

typedef struct AlgoCImageOut { int32_t width, height; uint8_t* bgra; } AlgoCImageOut;
typedef struct AlgoCCloudOut { int32_t count; float* xyz; uint8_t* rgb; } AlgoCCloudOut;

typedef struct AlgoCOutput {
    int32_t imageCount; AlgoCImageOut* images;
    int32_t cloudCount; AlgoCCloudOut* clouds;
    const char* error;
} AlgoCOutput;

#endif // COMUI_NATIVE_ABI_H
""";
}
