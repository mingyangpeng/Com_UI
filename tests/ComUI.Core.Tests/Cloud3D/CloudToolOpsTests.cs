using ComUI.Plugin.Cloud3D;
using ComUI.Sdk;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// 3D 工作台算子工具层（CloudToolOps）：node.json 工具清单装载（过滤规则/参数声明/说明）+
/// 真实原生往返（voxel_downsample 进程内调用部署 DLL——合成点云 → 体素下采样 → 点数收敛）。
/// </summary>
public class CloudToolOpsTests
{
    private static string AlgDir()
    {
        // tests/ComUI.Core.Tests/bin/Debug/net8.0 → 仓库根 → comdll/algo/AlgProject
        var root = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", ".."));
        return Path.Combine(root, "comdll", "algo", "AlgProject");
    }

    [Fact]
    public void LoadFromDir_ReturnsPoint3dOps_WithParamsAndDescriptions()
    {
        using var ops = new CloudToolOps();
        var list = CloudToolOps.LoadFromDir(AlgDir(), _ => { });

        Assert.True(list.Count >= 8, $"Point3D 工具算子应 >= 8（单输入 6 + 双输入 2），实际 {list.Count}");
        // 过滤规则：全部是 Point3DAlg 且 1~2 个点云输入（0 输入源算子/图像输入不入列）
        Assert.All(list, t => Assert.Contains("Point3DAlg", Path.GetFileName(t.DllPath)));
        Assert.All(list, t => Assert.False(string.IsNullOrWhiteSpace(t.Entry)));
        // 双实体算子在列（对比/配准走基准/被测选择）
        var diff = list.FirstOrDefault(t => t.Id == "alg.point3d.cloud_diff");
        Assert.NotNull(diff);
        Assert.Equal(2, diff!.CloudInputs);
        var regist = list.FirstOrDefault(t => t.Id == "alg.point3d.manual_regist");
        Assert.NotNull(regist);
        Assert.Equal(2, regist!.CloudInputs);
        Assert.DoesNotContain(list, t => t.Id == "alg.point3d.read_sick");      // 源算子（无点云输入）→ 数据源切换已覆盖

        // 体素下采样在列且参数声明完整（含说明——属性说明契约）
        var voxel = list.FirstOrDefault(t => t.Id == "alg.point3d.voxel_downsample");
        Assert.NotNull(voxel);
        var leaf = Assert.Single(voxel!.Params);
        Assert.Equal("leaf_size", leaf.Name);
        Assert.False(string.IsNullOrWhiteSpace(leaf.Description), "leaf_size 缺参数说明");

        // ROI/剖面类算子的参数说明（预填交互依赖 name 约定）
        var plane = list.FirstOrDefault(t => t.Id == "alg.point3d.region_plane");
        Assert.NotNull(plane);
        Assert.Equal(4, plane!.Params.Length);
        Assert.All(plane.Params, p => Assert.False(string.IsNullOrWhiteSpace(p.Description)));
    }

    [Fact]
    public void Invoke_VoxelDownsample_NativeRoundTrip_ReducesPoints()
    {
        using var ops = new CloudToolOps();
        var list = CloudToolOps.LoadFromDir(AlgDir(), _ => { });
        var voxel = list.FirstOrDefault(t => t.Id == "alg.point3d.voxel_downsample")
                    ?? throw new Exception("算子库缺 voxel_downsample（先部署 alg Release）");

        // 合成 30000 点网格（0.1mm 间距、10×10×3 体）——leaf=1mm 应收敛到 ~1000 格
        const int n = 30000;
        var pts = new float[n * 3];
        int i = 0;
        for (int z = 0; z < 10; z++)
            for (int y = 0; y < 30; y++)
                for (int x = 0; x < 100; x++)
                {
                    pts[i * 3] = x * 0.1f;
                    pts[i * 3 + 1] = y * 0.1f;
                    pts[i * 3 + 2] = z * 0.1f;
                    i++;
                }
        var payload = new CloudPayload { Id = "t", Count = n, Points = pts, ColorsRgb = null };

        var (clouds, _) = ops.Invoke(voxel, payload, new object?[] { 1.0 });

        var outCloud = Assert.Single(clouds);
        Assert.True(outCloud.Count > 0 && outCloud.Count < n, $"体素后点数应在 (0, {n})，实际 {outCloud.Count}");
        Assert.True(outCloud.Points.Length >= outCloud.Count * 3, "输出 SoA 长度契约");
    }

    [Fact]
    public void Invoke_MissingDll_ThrowsWithHint()
    {
        using var ops = new CloudToolOps();
        var op = new CloudToolOps.ToolOp
        {
            Id = "t", Name = "t", DllPath = Path.Combine(Path.GetTempPath(), "no_such_3d_dll.dll"), Entry = "x",
        };
        var payload = new CloudPayload { Count = 1, Points = new float[3] };
        var ex = Assert.Throws<Exception>(() => ops.Invoke(op, payload, Array.Empty<object?>()));
        Assert.Contains("算子库缺失", ex.Message);
    }

    private static double[] Id() => new double[] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    private static double Wx(double[] m, double x, double y, double z) => x * m[0] + y * m[1] + z * m[2] + m[3];
    private static double Wy(double[] m, double x, double y, double z) => x * m[4] + y * m[5] + z * m[6] + m[7];
    private static double Wz(double[] m, double x, double y, double z) => x * m[8] + y * m[9] + z * m[10] + m[11];

    [Fact]
    public void ComposeTransform_TranslateRotateMirror_Math()
    {
        // 单位阵 + 平移 (1,2,3)：PickAt 契约 wx = x·xf[0]+y·xf[1]+z·xf[2]+xf[3]
        var t = CloudToolOps.ComposeTransform(Id(), 1, 2, 3, 0, 0, 0, 1, 1, 1, false, false, false);
        Assert.Equal(1 + 1, Wx(t, 1, 0, 0));   // 点 x=1 + 平移 1
        Assert.Equal(0 + 2, Wy(t, 1, 0, 0));   // 点 y=0 + 平移 2
        Assert.Equal(0 + 3, Wz(t, 1, 0, 0));   // 点 z=0 + 平移 3

        // 绕 Z 旋转 90°：点 (1,0,0) → (0,1,0)
        var r = CloudToolOps.ComposeTransform(Id(), 0, 0, 0, 0, 0, 90, 1, 1, 1, false, false, false);
        Assert.Equal(0, Wx(r, 1, 0, 0), 5);
        Assert.Equal(1, Wy(r, 1, 0, 0), 5);

        // 镜像 X：点 (2,3,4) → (-2,3,4)
        var mx = CloudToolOps.ComposeTransform(Id(), 0, 0, 0, 0, 0, 0, 1, 1, 1, true, false, false);
        Assert.Equal(-2, Wx(mx, 2, 3, 4), 5);
        Assert.Equal(3, Wy(mx, 2, 3, 4), 5);

        // 统一缩放 2 + 平移叠加在既有变换上（先旧后新）：旧=平移(1,0,0)，新=缩放2+平移(0,0,5) → 点 (1,0,0) → (2,0,0) → (4,0,5)
        var cur = CloudToolOps.ComposeTransform(Id(), 1, 0, 0, 0, 0, 0, 1, 1, 1, false, false, false);
        var both = CloudToolOps.ComposeTransform(cur, 0, 0, 5, 0, 0, 0, 2, 2, 2, false, false, false);
        Assert.Equal(4, Wx(both, 1, 0, 0), 5);
        Assert.Equal(5, Wz(both, 1, 0, 0), 5);
    }

    [Fact]
    public void ComposeTransform_AxisScaleAndRotX()
    {
        // 指定轴缩放：sx=2, sy=3 → 点 (1,1,1) → (2,3,1)
        var s = CloudToolOps.ComposeTransform(Id(), 0, 0, 0, 0, 0, 0, 2, 3, 1, false, false, false);
        Assert.Equal(2, Wx(s, 1, 1, 1), 5);
        Assert.Equal(3, Wy(s, 1, 1, 1), 5);
        Assert.Equal(1, Wz(s, 1, 1, 1), 5);

        // 绕 X 旋转 90°：点 (0,1,0) → (0,0,1)
        var rx = CloudToolOps.ComposeTransform(Id(), 0, 0, 0, 90, 0, 0, 1, 1, 1, false, false, false);
        Assert.Equal(0, Wx(rx, 0, 1, 0), 5);
        Assert.Equal(0, Wy(rx, 0, 1, 0), 5);
        Assert.Equal(1, Wz(rx, 0, 1, 0), 5);

        // reset_first：忽略既有平移 → 点回到原始坐标再叠加新平移
        var cur = CloudToolOps.ComposeTransform(Id(), 100, 0, 0, 0, 0, 0, 1, 1, 1, false, false, false);
        var reset = CloudToolOps.ComposeTransform(cur, 1, 0, 0, 0, 0, 0, 1, 1, 1, false, false, false, reset: true);
        Assert.Equal(2, Wx(reset, 1, 0, 0), 5);   // 若未 reset 则为 102
    }

    [Fact]
    public void PointInPolygon_RayCast_TriangleMembership()
    {
        // 射线法：三角形 (0,0)(4,0)(2,3)——内点/外点/顶点上方延伸外点
        var tri = new List<(double X, double Y)> { (0, 0), (4, 0), (2, 3) };
        Assert.True(CloudToolOps.PointInPolygon(tri, 2, 1));     // 内
        Assert.True(CloudToolOps.PointInPolygon(tri, 2, 0.1));   // 近底边内
        Assert.False(CloudToolOps.PointInPolygon(tri, 3.9, 2.9)); // 右上外
        Assert.False(CloudToolOps.PointInPolygon(tri, -1, 1));   // 左外
        Assert.False(CloudToolOps.PointInPolygon(tri, 5, 5));    // 远外
        Assert.False(CloudToolOps.PointInPolygon(tri, 2, 3.1));  // 顶点略上方外
        // 顶点不足 3 个恒 false
        Assert.False(CloudToolOps.PointInPolygon(new List<(double, double)> { (0, 0), (1, 1) }, 0.5, 0.5));
    }

    [Fact]
    public void SelectBoxInterior_InsideOutsideAndNanGuards()
    {
        // 2×2×2 体：原点/盒外点/NaN 点——盒内只收有限且落在范围内的点；盒外收其余（含 NaN）
        var pts = new float[3 * 3];
        pts[0] = 0.5f; pts[1] = 0.5f; pts[2] = 0.5f;                 // 盒内
        pts[3] = 5.0f; pts[4] = 5.0f; pts[5] = 5.0f;                 // 盒外
        pts[6] = float.NaN; pts[7] = 0.5f; pts[8] = 0.5f;            // NaN（永不进盒内）

        var inside = CloudToolOps.SelectBoxInterior(pts, 3, 0, 0, 0, 1, 1, 1, keepInside: true);
        Assert.Equal(new[] { 0 }, inside);

        var outside = CloudToolOps.SelectBoxInterior(pts, 3, 0, 0, 0, 1, 1, 1, keepInside: false);
        Assert.Equal(new[] { 1, 2 }, outside);
    }

    [Fact]
    public async Task Invoke_RegionPlane_MessageChannelSurface()
    {
        // 成功消息通道（Sdk Run out message）：region_plane 的量测结果走 error 字段随行——合成平面点云验证
        using var ops = new CloudToolOps();
        var list = CloudToolOps.LoadFromDir(AlgDir(), _ => { });
        var plane = list.FirstOrDefault(t => t.Id == "alg.point3d.region_plane")
                    ?? throw new Exception("算子库缺 region_plane");

        // z = 1.23 的平面（加微小噪声避免退化）11×11 网格
        const int n = 121;
        var pts = new float[n * 3];
        int i = 0;
        for (int y = 0; y < 11; y++)
            for (int x = 0; x < 11; x++)
            {
                pts[i * 3] = x * 0.5f;
                pts[i * 3 + 1] = y * 0.5f;
                pts[i * 3 + 2] = 1.23f + (i % 3) * 1e-4f;
                i++;
            }
        var payload = new CloudPayload { Id = "plane", Count = n, Points = pts };

        var (clouds, message) = await Task.Run(() =>
            ops.Invoke(plane, payload, new object?[] { 0.0, 0.0, 5.0, 5.0 }));

        Assert.Null(clouds.FirstOrDefault());    // region_plane 无点云输出端口
        Assert.False(string.IsNullOrWhiteSpace(message), "region_plane 成功消息应经 message 通道返回");
        Assert.Contains("平面度", message);
    }

    [Fact]
    public async Task Invoke_CloudDiff_DualInputRoundTrip()
    {
        // 双实体算子（基准/被测）：被测 = 基准 + 0.05 台阶 → 输出差值点云（z=diff）
        using var ops = new CloudToolOps();
        var list = CloudToolOps.LoadFromDir(AlgDir(), _ => { });
        var diff = list.FirstOrDefault(t => t.Id == "alg.point3d.cloud_diff")
                   ?? throw new Exception("算子库缺 cloud_diff");
        Assert.Equal(2, diff.CloudInputs);

        const int n = 100;
        var basePts = new float[n * 3];
        var testPts = new float[n * 3];
        int i = 0;
        for (int y = 0; y < 10; y++)
            for (int x = 0; x < 10; x++)
            {
                basePts[i * 3] = x * 0.2f; basePts[i * 3 + 1] = y * 0.2f; basePts[i * 3 + 2] = 0f;
                testPts[i * 3] = x * 0.2f; testPts[i * 3 + 1] = y * 0.2f; testPts[i * 3 + 2] = 0.05f;
                i++;
            }
        var a = new CloudPayload { Id = "base", Count = n, Points = basePts };
        var b = new CloudPayload { Id = "test", Count = n, Points = testPts };

        var (clouds, message) = await Task.Run(() =>
            ops.Invoke(diff, new[] { a, b }, new object?[] { 0.2, 3, 0.01, 0.0 }));

        var outCloud = Assert.Single(clouds);
        Assert.True(outCloud.Count > 0);
        // 差值 z ≈ +0.05（被测高）：抽第一格验证
        Assert.True(Math.Abs(outCloud.Points[2] - 0.05f) < 0.01f, $"首格差值应 ≈0.05，实际 {outCloud.Points[2]}");
        Assert.False(string.IsNullOrWhiteSpace(message));
    }
}
