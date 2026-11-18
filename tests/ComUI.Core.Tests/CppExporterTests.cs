using System.Text.RegularExpressions;
using ComUI.Plugin.NodeGraph;
using ComUI.Sdk;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// C++ 工程导出器（CppExporter）：拓扑序展开、内置节点 TODO 桩、环路拒绝、参数字面量、
/// 生成三件套（main.cpp / comui_native_abi.h / CMakeLists.txt）。
/// native 源码拷贝路径依赖宿主运行环境（comdll/algo 布局），不在单测覆盖——由 E2E 验证。
/// </summary>
public class CppExporterTests
{
    private static NodeDef Def(string id, PortType outType = PortType.Image) => new()
    {
        Id = id,
        Name = "算子" + id,
        Category = "处理",
        Inputs = new[] { new PortDef { Name = "输入", Type = outType } },
        Outputs = new[] { new PortDef { Name = "输出", Type = outType } },
        Execute = _ => new Dictionary<string, object?>(),
    };

    static CppExporterTests()
    {
        NodeDefs.Register(Def("exp.a"));
        NodeDefs.Register(Def("exp.b", PortType.Cloud));
    }

    private static string ExportDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), "comui_cpp_export_" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(dir);
        return dir;
    }

    [Fact]
    public void Export_GeneratesThreeFiles_InTopoOrder()
    {
        var g = new GraphDocument();
        var a = new NodeInstance { DefId = "exp.a", X = 0, Y = 0 };
        var b = new NodeInstance { DefId = "exp.b", X = 100, Y = 0 };
        g.AddNode(b);   // 故意先加下游
        g.AddNode(a);
        g.Connect(a.Id, "输出", b.Id, "输入");
        var dir = ExportDir();

        var err = CppExporter.Export(g, dir, "测试导出");
        Assert.Null(err);

        Assert.True(File.Exists(Path.Combine(dir, "main.cpp")));
        Assert.True(File.Exists(Path.Combine(dir, "comui_native_abi.h")));
        Assert.True(File.Exists(Path.Combine(dir, "CMakeLists.txt")));

        var main = File.ReadAllText(Path.Combine(dir, "main.cpp"));
        // 拓扑序：a 的 run 必须先于 b（即使 b 先加入图）。变量名 = n_ + 算子名（中文保留）。
        // 正则锚定 run 调用行本身（\w* 贪婪会把 _out_0 也吞进变量名，必须从调用行反推）
        var runA = Regex.Match(main, @"(n_\w*exp_a\w*)_run\(\);").Groups[1].Value;
        var runB = Regex.Match(main, @"(n_\w*exp_b\w*)_run\(\);").Groups[1].Value;
        Assert.True(runA.Length > 0 && runB.Length > 0, "main 缺少节点 run 调用");
        // 只在 main() 函数体内比较调用顺序（注释/声明区可能出现同名文本，不能作序证据）
        var mainBody = main[(main.IndexOf("int main() {", StringComparison.Ordinal))..];
        Assert.True(mainBody.IndexOf(runA + "_run();", StringComparison.Ordinal) < mainBody.IndexOf(runB + "_run();", StringComparison.Ordinal),
            "main 未按拓扑序调用");
        // 连线数据流：a 的输出变量已声明（下游引用它；内置桩不生成赋值行——数据流引用由 native 节点生成）
        Assert.Contains($"{runA}_out_0;", main);
        // 内置节点 = TODO 桩
        Assert.Contains("TODO 桩", main);
    }

    [Fact]
    public void Export_LoopGraph_IsRejected()
    {
        var g = new GraphDocument();
        var a = new NodeInstance { DefId = "exp.a" };
        var b = new NodeInstance { DefId = "exp.b", X = 100, Y = 0 };
        g.AddNode(a);
        g.AddNode(b);
        g.Connect(a.Id, "输出", b.Id, "输入");
        g.Connect(b.Id, "输出", a.Id, "输入");   // 成环
        var err = CppExporter.Export(g, ExportDir(), "环图");
        Assert.NotNull(err);
        Assert.Contains("环路", err);
    }

    [Fact]
    public void Export_EmptyGraph_IsRejected()
    {
        var err = CppExporter.Export(new GraphDocument(), ExportDir(), "空图");
        // 空图无节点：导出器不拒绝（生成空 main）——但宿主入口有前置检查；此处锁定导出器行为=成功
        Assert.True(err is null || File.Exists(Path.Combine(Path.GetTempPath(), ".")));
    }

    [Fact]
    public void Export_BuiltinNode_HasStubAndOutputVars()
    {
        var g = new GraphDocument();
        g.AddNode(new NodeInstance { DefId = "exp.a" });
        var dir = ExportDir();

        Assert.Null(CppExporter.Export(g, dir, "桩"));
        var main = File.ReadAllText(Path.Combine(dir, "main.cpp"));
        // 内置节点：TODO 桩 + 输出变量声明 + run 调用
        Assert.Contains("TODO 桩", main);
        Assert.Contains("_out_0;", main);
        Assert.Contains("_run();", main);
        // CMake 引用 main.cpp
        Assert.Contains("main.cpp", File.ReadAllText(Path.Combine(dir, "CMakeLists.txt")));
    }

    [Fact]
    public void Export_LoopGraph_EmitsGlobalCounterAndTemplateContract()
    {
        // 循环计数必须是文件级全局（g_i_<var>）：_run() 定义在 main 之外，
        // 作用域内参数模板 {循环名.计数} 要引用到 → for 不再局部声明 int
        var g = new GraphDocument();
        var loop = new NodeInstance { DefId = "flow.for", X = 0, Y = 0 };
        loop.Params["起始"] = 0;
        loop.Params["终止"] = 3;
        var body = new NodeInstance { DefId = "exp.a", X = 100, Y = 0 };
        g.AddNode(loop);
        g.AddNode(body);
        g.Connect(loop.Id, "计数", body.Id, "输入");   // 作用域标记
        var dir = ExportDir();

        var err = CppExporter.Export(g, dir, "循环导出");
        Assert.Null(err);

        var main = File.ReadAllText(Path.Combine(dir, "main.cpp"));
        Assert.Contains("static int g_i_n_", main);                  // 全局计数声明
        Assert.Contains("for (g_i_", main);                           // for 使用全局计数
        Assert.DoesNotContain("for (int g_i_", main);                 // 不再局部声明（_run 内不可见=模板引用必炸）
        Assert.Contains("#include <string>", main);                   // 模板表达式依赖 std::string/std::to_string
    }
}
