using System.IO;
using System.Text.Json;
using ComUI.Sdk;
using ComUI.Sdk.Native;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 3D 工作台「算子工具」层：扫描 alg 算子库目录（comdll/algo/AlgProject）里 Point3DAlg 的
/// native node.json，生成工具清单（名称/说明/参数声明），经 Sdk NativeAlgoInvoker 直接调用——
/// 工作台工具与流水线算子同源（同一份 node.json + 同一份 DLL），零重复维护。
/// 支持过滤：恰好 1 个点云输入、无图像输入（多输入对比/配准类在流水线里编排，不在工具栏）。
/// </summary>
public sealed class CloudToolOps : IDisposable
{
    /// <summary>工具参数声明（与 node.json params 同构）。</summary>
    public sealed class ToolParam
    {
        public required string Name { get; init; }
        public string Label { get; init; } = "";
        public string Type { get; init; } = "number";
        public string? Default { get; init; }
        public double? Min { get; init; }
        public double? Max { get; init; }
        public string? Description { get; init; }
    }

    public sealed class ToolOp
    {
        public required string Id { get; init; }
        public required string Name { get; init; }
        public string? Description { get; init; }
        public required string DllPath { get; init; }
        public required string Entry { get; init; }
        public ToolParam[] Params { get; init; } = Array.Empty<ToolParam>();
        /// <summary>点云输入端口数（1=单实体工具；2=对比/配准类，对话框选基准/被测两个实体）。</summary>
        public int CloudInputs { get; init; } = 1;
    }

    private readonly Dictionary<string, NativeAlgoInvoker> _invokers = new();   // entry → invoker（同 DLL 复用）

    /// <summary>扫描算子库目录（*.node.json，runtime=native 且恰好 1 点云输入 0 图像输入）。</summary>
    public static List<ToolOp> LoadFromDir(string dir, Action<string>? log = null)
    {
        var list = new List<ToolOp>();
        if (!Directory.Exists(dir)) return list;
        foreach (var f in Directory.EnumerateFiles(dir, "*.node.json", SearchOption.TopDirectoryOnly))
        {
            try
            {
                var json = JsonDocument.Parse(File.ReadAllText(f), new JsonDocumentOptions
                {
                    CommentHandling = JsonCommentHandling.Skip,
                    AllowTrailingCommas = true,
                }).RootElement;

                var node = json.GetProperty("node");
                if (!string.Equals(json.GetProperty("runtime").GetString(), "native", StringComparison.OrdinalIgnoreCase))
                    continue;
                string dll = json.GetProperty("assembly").GetString() ?? "";
                if (!dll.Contains("Point3DAlg", StringComparison.OrdinalIgnoreCase)) continue;   // 3D 工作台只挂 3D 算子
                string entry = json.GetProperty("method").GetString() ?? "";

                int cloudIn = 0, imgIn = 0;
                foreach (var port in json.GetProperty("inputs").EnumerateArray())
                {
                    var t = port.GetProperty("type").GetString() ?? "";
                    if (t == "cloud") cloudIn++;
                    else if (t == "image") imgIn++;
                }
                if (cloudIn is not (1 or 2) || imgIn != 0) continue;   // 单/双点云输入；0 输入源算子与多输入走其它路径

                var ps = new List<ToolParam>();
                if (json.TryGetProperty("params", out var jps))
                    foreach (var jp in jps.EnumerateArray())
                    {
                        ps.Add(new ToolParam
                        {
                            Name = jp.GetProperty("name").GetString() ?? "",
                            Label = jp.TryGetProperty("label", out var l) ? l.GetString() ?? "" : "",
                            Type = jp.TryGetProperty("type", out var t2) ? t2.GetString() ?? "number" : "number",
                            Default = jp.TryGetProperty("default", out var dv) ? JsonText(dv) : null,
                            Min = jp.TryGetProperty("min", out var mn) && mn.ValueKind == JsonValueKind.Number ? mn.GetDouble() : null,
                            Max = jp.TryGetProperty("max", out var mx) && mx.ValueKind == JsonValueKind.Number ? mx.GetDouble() : null,
                            Description = jp.TryGetProperty("description", out var de) ? de.GetString() : null,
                        });
                    }

                list.Add(new ToolOp
                {
                    Id = node.GetProperty("id").GetString() ?? Path.GetFileNameWithoutExtension(f),
                    Name = node.GetProperty("name").GetString() ?? Path.GetFileNameWithoutExtension(f),
                    Description = node.TryGetProperty("description", out var dsc) ? dsc.GetString() : null,
                    DllPath = Path.Combine(Path.GetDirectoryName(f)!, dll),
                    Entry = entry,
                    Params = ps.ToArray(),
                    CloudInputs = cloudIn,
                });
            }
            catch (Exception ex)
            {
                log?.Invoke($"工具算子解析失败 {Path.GetFileName(f)}: {ex.Message}");
            }
        }
        return list.OrderBy(t => t.Name, StringComparer.OrdinalIgnoreCase).ToList();
    }

    private static string JsonText(JsonElement el) => el.ValueKind switch
    {
        JsonValueKind.Number => el.GetRawText(),
        JsonValueKind.True => "true",
        JsonValueKind.False => "false",
        _ => el.ToString() ?? "",
    };

    /// <summary>单输入便捷重载（内部转多输入通用路径）。</summary>
    public (CloudPayload[] Clouds, string? Message) Invoke(ToolOp op, CloudPayload input, IReadOnlyList<object?> paramValues)
        => Invoke(op, new[] { input }, paramValues);

    /// <summary>在实体点云上执行工具算子：返回输出点云与消息（message = DLL error 通道，
    /// 按 manual_regist 先例成功时也承载统计文本——平面/剖面/圆量测的结果都走这里）。</summary>
    public (CloudPayload[] Clouds, string? Message) Invoke(ToolOp op, IReadOnlyList<CloudPayload> inputs, IReadOnlyList<object?> paramValues)
    {
        if (!File.Exists(op.DllPath))
            throw new Exception($"算子库缺失 {Path.GetFileName(op.DllPath)}（先在 alg 项目构建部署）");
        if (!_invokers.TryGetValue(op.Entry, out var invoker))
        {
            invoker = new NativeAlgoInvoker(op.DllPath, op.Entry);
            _invokers[op.Entry] = invoker;
        }
        var (imgs, clouds) = invoker.Run(paramValues.ToList(), new List<ImagePayload>(), inputs.ToList(), out var message);
        return (clouds.ToArray(), string.IsNullOrWhiteSpace(message) ? null : message);
    }

    // ==================== 4×4 行主序矩阵组装（与 CloudData.Xform 同契约：e[i*4+j]=M[i][j]，末列平移） ====================

    /// <summary>4×4 行主序矩阵乘 C = A×B。</summary>
    public static double[] MatMul(double[] a, double[] b)
    {
        var c = new double[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                double s = 0;
                for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
                c[i * 4 + j] = s;
            }
        return c;
    }

    public static double[] Identity() => new double[] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    public static double[] Translation(double tx, double ty, double tz) => new double[]
    {
        1, 0, 0, tx,
        0, 1, 0, ty,
        0, 0, 1, tz,
        0, 0, 0, 1,
    };

    public static double[] Scale(double sx, double sy, double sz) => new double[]
    {
        sx, 0, 0, 0,
        0, sy, 0, 0,
        0, 0, sz, 0,
        0, 0, 0, 1,
    };

    public static double[] RotX(double deg)
    {
        double r = deg * Math.PI / 180.0, c = Math.Cos(r), s = Math.Sin(r);
        return new double[] { 1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0, 0, 0, 0, 1 };
    }

    public static double[] RotY(double deg)
    {
        double r = deg * Math.PI / 180.0, c = Math.Cos(r), s = Math.Sin(r);
        return new double[] { c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1 };
    }

    public static double[] RotZ(double deg)
    {
        double r = deg * Math.PI / 180.0, c = Math.Cos(r), s = Math.Sin(r);
        return new double[] { c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    }

    /// <summary>组合实体变换（行主序）：在现有变换上叠加 新变换 = T·Rz·Ry·Rx·S（镜像并入缩放符号）。
    /// 指定轴缩放/三轴欧拉旋转全覆盖。reset=true 时忽略 current（从单位阵起算）。纯函数可测试。</summary>
    public static double[] ComposeTransform(double[] current,
        double tx, double ty, double tz,
        double rotXDeg, double rotYDeg, double rotZDeg,
        double scaleX, double scaleY, double scaleZ,
        bool mirrorX, bool mirrorY, bool mirrorZ, bool reset = false)
    {
        var m = Identity();
        m = MatMul(m, Translation(tx, ty, tz));
        m = MatMul(m, RotZ(rotZDeg));
        m = MatMul(m, RotY(rotYDeg));
        m = MatMul(m, RotX(rotXDeg));
        m = MatMul(m, Scale((mirrorX ? -1 : 1) * scaleX, (mirrorY ? -1 : 1) * scaleY, (mirrorZ ? -1 : 1) * scaleZ));
        return MatMul(m, reset ? Identity() : current);
    }

    public static bool PointInPolygon(IReadOnlyList<(double X, double Y)> poly, double x, double y)
    {
        if (poly.Count < 3) return false;
        bool inside = false;
        for (int i = 0, j = poly.Count - 1; i < poly.Count; j = i++)
        {
            var (xi, yi) = poly[i];
            var (xj, yj) = poly[j];
            if ((yi > y) != (yj > y) &&
                x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi)
                inside = !inside;
        }
        return inside;
    }

    /// <summary>盒裁剪索引选择（实体局部坐标，纯函数可测试）：盒内判定要求三分量有限（NaN 永不进盒内）。
    /// keepInside=true 返回盒内点索引，false 返回盒外（含 NaN）点索引——剔除场景 NaN 随盒外走，由 SetCloud 的
    /// 无效点防护兜底。stride 语义与框选一致（调用方控制遍历步长）。</summary>
    public static List<int> SelectBoxInterior(float[] pts, int count,
        double minx, double miny, double minz, double maxx, double maxy, double maxz,
        bool keepInside, int stride = 1)
    {
        var result = new List<int>();
        for (int i = 0; i < count; i += stride)
        {
            float x = pts[i * 3], y = pts[i * 3 + 1], z = pts[i * 3 + 2];
            bool inside = float.IsFinite(x) && float.IsFinite(y) && float.IsFinite(z) &&
                          x >= minx && x <= maxx && y >= miny && y <= maxy && z >= minz && z <= maxz;
            if (inside == keepInside) result.Add(i);
        }
        return result;
    }

    public void Dispose()
    {
        foreach (var kv in _invokers) kv.Value.Dispose();
        _invokers.Clear();
    }
}
