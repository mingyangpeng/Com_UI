using ComUI.Plugin.NodeGraph;

// 离线冒烟：走 NodeJsonRegistry 真实加载路径（扫 comdll/algo 注册 native 算子+source 元数据），
// 加载指定算法文件，调 CppExporter.Export 生成完整 C++ 工程。
// 用法：dotnet run -- <算法json路径> <导出目录>
var comdll = @"D:\pmy\Com_UI\comdll";
Console.WriteLine("comdll = " + comdll);
foreach (var def in NodeJsonRegistry.LoadFrom(comdll, m => Console.WriteLine(m)))
    NodeDefs.Register(def);

var jsonPath = args.Length > 0 ? args[0] : @"D:\pmy\Com_UI\config\algorithms\sick_e2e.pipeline.json";
var outDir = args.Length > 1 ? args[1] : @"D:\pmy\Com_UI\output\e2e_export";
var g = new GraphDocument();
g.LoadFromJson(File.ReadAllText(jsonPath));
Console.WriteLine($"nodes={g.Nodes.Count} conns={g.Connections.Count}");
var err = CppExporter.Export(g, outDir, System.IO.Path.GetFileNameWithoutExtension(jsonPath));
Console.WriteLine(err is null ? "EXPORT OK" : "EXPORT FAIL: " + err);
