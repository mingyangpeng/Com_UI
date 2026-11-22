using ComUI.Plugin.Cloud3D;
using ComUI.Sdk;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// 3D 工作台本地点云 IO 与基础处理（CloudPointIO）：PLY/XYZ 导入导出往返、
/// 随机下采样数量收敛、半径离群滤波剔除孤立点、多实体合并（世界坐标契约由调用方保证）。
/// </summary>
public class CloudPointIOTests
{
    private static CloudPayload MakeCloud(int n, bool colored = true)
    {
        var pts = new float[n * 3];
        var cols = colored ? new byte[n * 3] : null;
        for (int i = 0; i < n; i++)
        {
            pts[i * 3] = i * 0.1f;
            pts[i * 3 + 1] = i * 0.2f;
            pts[i * 3 + 2] = i * 0.3f;
            if (cols is not null)
            {
                cols[i * 3] = (byte)(i % 256);
                cols[i * 3 + 1] = (byte)((i * 7) % 256);
                cols[i * 3 + 2] = (byte)((i * 13) % 256);
            }
        }
        return new CloudPayload { Id = "t", Count = n, Points = pts, ColorsRgb = cols };
    }

    [Fact]
    public void Xyz_ExportImport_Roundtrip_WithAndWithoutColor()
    {
        var src = MakeCloud(1000);
        var path = Path.Combine(Path.GetTempPath(), "comui_io_" + Guid.NewGuid().ToString("N")[..8] + ".xyz");
        try
        {
            CloudPointIO.SaveTo(path, src);
            var back = CloudPointIO.LoadFrom(path);
            Assert.Equal(src.Count, back.Count);
            Assert.Equal(src.Points[0], back.Points[0]);
            Assert.Equal(src.Points[src.Count * 3 - 1], back.Points[back.Count * 3 - 1]);
            Assert.NotNull(back.ColorsRgb);
            Assert.Equal(src.ColorsRgb![0], back.ColorsRgb[0]);
        }
        finally { File.Delete(path); }

        // 无色版本
        var bare = MakeCloud(50, colored: false);
        var path2 = Path.Combine(Path.GetTempPath(), "comui_io_" + Guid.NewGuid().ToString("N")[..8] + ".xyz");
        try
        {
            CloudPointIO.SaveTo(path2, bare);
            var back2 = CloudPointIO.LoadFrom(path2);
            Assert.Equal(50, back2.Count);
            Assert.Null(back2.ColorsRgb);
        }
        finally { File.Delete(path2); }
    }

    [Fact]
    public void Ply_ExportImport_BinaryNotRequiredAsciiRoundtrip()
    {
        var src = MakeCloud(500);
        var path = Path.Combine(Path.GetTempPath(), "comui_io_" + Guid.NewGuid().ToString("N")[..8] + ".ply");
        try
        {
            CloudPointIO.SaveTo(path, src);   // 导出为 ascii PLY（含 RGB）
            var back = CloudPointIO.LoadFrom(path);
            Assert.Equal(500, back.Count);
            Assert.NotNull(back.ColorsRgb);
            for (int i = 0; i < 10; i++)
            {
                Assert.Equal(src.Points[i], back.Points[i]);
                Assert.Equal(src.ColorsRgb![i], back.ColorsRgb[i]);
            }
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void RandomSample_TargetCountAndContract()
    {
        var src = MakeCloud(10000);
        var sampled = CloudPointIO.RandomSample(src, 0.1);
        Assert.True(sampled.Count >= 900 && sampled.Count <= 1001, $"10% 采样应 ≈1000，实际 {sampled.Count}");
        Assert.Equal(sampled.Points.Length, sampled.Count * 3);
        // 抽出的点必须是原点云的点（值域抽查）
        Assert.InRange(sampled.Points[0], src.Points.Min(), src.Points.Max());
    }

    [Fact]
    public void StatisticOutlierRemove_RemovesIsolatedPoints()
    {
        // 125 个密集格点 + 1 个孤立点（远离 10mm）→ 半径 1mm 最少 5 邻居应只剔除孤立点
        var pts = new float[126 * 3];
        int i = 0;
        for (int z = 0; z < 5; z++)
            for (int y = 0; y < 5; y++)
                for (int x = 0; x < 5; x++)
                {
                    pts[i * 3] = x * 0.1f; pts[i * 3 + 1] = y * 0.1f; pts[i * 3 + 2] = z * 0.1f;
                    i++;
                }
        pts[125 * 3] = 50f; pts[125 * 3 + 1] = 50f; pts[125 * 3 + 2] = 50f;
        var src = new CloudPayload { Count = 126, Points = pts };

        var (kept, removed) = CloudPointIO.StatisticOutlierRemove(src, 1.0, 5);
        Assert.Equal(1, removed);
        Assert.Equal(125, kept.Count);
    }

    [Fact]
    public void RansacPlaneSplit_ExtractsDominantPlane()
    {
        // 200 点 z=0 平面（带 0.01 抖动）+ 30 点远处噪声团 → 分割应把平面整片提出
        var pts = new float[230 * 3];
        int i = 0;
        var rng = new Random(7);
        for (int k = 0; k < 200; k++)
        {
            pts[k * 3] = (float)(rng.NextDouble() * 10);
            pts[k * 3 + 1] = (float)(rng.NextDouble() * 10);
            pts[k * 3 + 2] = (float)(rng.NextDouble() * 0.02);
            i++;
        }
        for (int k = 0; k < 30; k++)
        {
            pts[(200 + k) * 3] = 20f + (float)rng.NextDouble();
            pts[(200 + k) * 3 + 1] = 20f + (float)rng.NextDouble();
            pts[(200 + k) * 3 + 2] = 8f + (float)rng.NextDouble();
        }
        var src = new CloudPayload { Count = 230, Points = pts };

        var (plane, rest, inliers) = CloudPointIO.RansacPlaneSplit(src, 0.05, 300);
        Assert.Equal(200, inliers);            // 平面整片命中（阈值 0.05 覆盖抖动）
        Assert.Equal(200, plane.Count);
        Assert.Equal(30, rest.Count);
        Assert.All(plane.Points, v => Assert.True(float.IsFinite(v)));
    }

    [Fact]
    public void RansacPlaneSplit_TooFewPoints_Throws()
    {
        var src = new CloudPayload { Count = 2, Points = new float[6] };
        Assert.Throws<Exception>(() => CloudPointIO.RansacPlaneSplit(src, 0.1, 100));
    }

    [Fact]
    public void Perf_MillionPoints_RansacAndSor_Bounded()
    {
        // 性能回归锁：100 万点 RANSAC(200 迭代)/SOR 半径滤波均应在秒级完成（当前实现 ~1-2s/项；
        // 若实现退化（如 O(n²)）本测试必炸）。机器差异大 → 断言给足余量（10s）。
        const int n = 1_000_000;
        var pts = new float[n * 3];
        var rng = new Random(42);
        for (int i = 0; i < n; i++)
        {
            pts[i * 3] = (float)rng.NextDouble() * 100;
            pts[i * 3 + 1] = (float)rng.NextDouble() * 100;
            pts[i * 3 + 2] = (float)(rng.NextDouble() * 0.02);   // 薄片云（z 抖动 0.02 < 阈值 0.05）
        }
        var payload = new CloudPayload { Count = n, Points = pts };

        var sw = System.Diagnostics.Stopwatch.StartNew();
        var (plane, rest, inliers) = CloudPointIO.RansacPlaneSplit(payload, 0.05, 200);
        sw.Stop();
        Assert.True(inliers > n / 2, "薄片云大部分点应为平面内点");
        Assert.True(sw.ElapsedMilliseconds < 10_000, $"RANSAC 100 万点×200 迭代应 <10s，实际 {sw.ElapsedMilliseconds}ms");

        sw.Restart();
        var (kept, removed) = CloudPointIO.StatisticOutlierRemove(payload, 2.0, 5);
        sw.Stop();
        Assert.True(sw.ElapsedMilliseconds < 10_000, $"SOR 100 万点应 <10s，实际 {sw.ElapsedMilliseconds}ms");
        Assert.True(kept.Count > 0);
    }

    [Fact]
    public void LoadPly_BinaryLittleEndian_ExactDataOffset()
    {
        // 手写 binary_little_endian PLY（x,y,z float + rgb uchar）：header 后紧跟数据。
        // 回归锁：曾用 StreamReader 读 header——内部缓冲越过 end_header 使 fs.Position 越位 → 数据错位。
        var src = MakeCloud(100);
        var path = Path.Combine(Path.GetTempPath(), "comui_plyb_" + Guid.NewGuid().ToString("N")[..8] + ".ply");
        try
        {
            using (var fs = File.Create(path))
            using (var w = new BinaryWriter(fs))
            {
                foreach (var l in new[]
                {
                    "ply",
                    "format binary_little_endian 1.0",
                    "comment generated by ComUI test",
                    "element vertex 100",
                    "property float x",
                    "property float y",
                    "property float z",
                    "property uchar red",
                    "property uchar green",
                    "property uchar blue",
                    "end_header",
                }) { foreach (var ch in System.Text.Encoding.ASCII.GetBytes(l)) w.Write(ch); w.Write((byte)'\n'); }

                for (int i = 0; i < 100; i++)
                {
                    w.Write(src.Points[i * 3]);
                    w.Write(src.Points[i * 3 + 1]);
                    w.Write(src.Points[i * 3 + 2]);
                    w.Write(src.ColorsRgb![i * 3]);
                    w.Write(src.ColorsRgb[i * 3 + 1]);
                    w.Write(src.ColorsRgb[i * 3 + 2]);
                }
            }

            var back = CloudPointIO.LoadFrom(path);
            Assert.Equal(100, back.Count);
            for (int i = 0; i < 100; i++)
            {
                Assert.Equal(src.Points[i * 3], back.Points[i * 3]);
                Assert.Equal(src.Points[i * 3 + 2], back.Points[i * 3 + 2]);
                Assert.Equal(src.ColorsRgb![i * 3], back.ColorsRgb![i * 3]);
            }
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void RansacPlaneSplit_TwoRounds_ExtractBothPlanes()
    {
        // 两平行平面（z=0 与 z=5 各 100 点）→ 轮 1 提走 z=0，轮 2 在残差上提走 z=5
        var pts = new float[200 * 3];
        int i = 0;
        var rng = new Random(11);
        for (int k = 0; k < 100; k++)
        {
            pts[k * 3] = (float)rng.NextDouble() * 10;
            pts[k * 3 + 1] = (float)rng.NextDouble() * 10;
            pts[k * 3 + 2] = (float)(rng.NextDouble() * 0.02);
            i++;
        }
        for (int k = 100; k < 200; k++)
        {
            pts[k * 3] = (float)rng.NextDouble() * 10;
            pts[k * 3 + 1] = (float)rng.NextDouble() * 10;
            pts[k * 3 + 2] = 5f + (float)(rng.NextDouble() * 0.02);
            i++;
        }
        var src = new CloudPayload { Count = 200, Points = pts };

        // 轮 1
        var (p1, rest, il1) = CloudPointIO.RansacPlaneSplit(src, 0.05, 300);
        Assert.Equal(100, p1.Count);
        // 轮 2（残差再分割）
        var (p2, rest2, il2) = CloudPointIO.RansacPlaneSplit(rest, 0.05, 300);
        Assert.Equal(100, p2.Count);
        Assert.Equal(0, rest2.Count);
        // 两轮平面 z 均值可区分
        double zMean(CloudPayload c)
        {
            double s = 0;
            for (int k = 0; k < c.Count; k++) s += c.Points[k * 3 + 2];
            return s / c.Count;
        }
        Assert.True(Math.Abs(zMean(p1) - zMean(p2)) > 4.0, $"两平面 z 均值应可区分：{zMean(p1)} vs {zMean(p2)}");
    }

    [Fact]
    public void VoxelDownsample_DeterministicAndBounded()
    {
        // 确定性：同输入同输出；kept ≤ count；voxel≤0 报错
        var src = MakeCloud(1000);
        var (a, ca) = CloudPointIO.VoxelDownsample(src, 0.5);
        var (b, cb) = CloudPointIO.VoxelDownsample(src, 0.5);
        Assert.Equal(a.Count, b.Count);
        Assert.Equal(a.Points, b.Points);   // 确定性：字节级一致
        Assert.True(a.Count <= 1000);
        Assert.Equal(a.Count, ca);
        Assert.NotNull(a.ColorsRgb);        // 源有色 → 输出有色
        Assert.Equal(a.Count * 3, a.ColorsRgb!.Length);
        Assert.Throws<Exception>(() => CloudPointIO.VoxelDownsample(src, 0));

        // 体素足够大 → 全部点并入同格 → 1 点
        var (one, _) = CloudPointIO.VoxelDownsample(src, 1000);
        Assert.Equal(1, one.Count);
    }

    [Fact]
    public void Merge_ConcatsPointsAndDropsColorsWhenAnyMissing()
    {
        var a = MakeCloud(100);
        var b = MakeCloud(50, colored: false);
        var merged = CloudPointIO.Merge(new[] { a, b });
        Assert.Equal(150, merged.Count);
        Assert.Null(merged.ColorsRgb);          // 任一片无色 → 整体无色
        Assert.Equal(a.Points[0], merged.Points[0]);
        Assert.Equal(b.Points[0], merged.Points[100 * 3]);   // 第二片接续偏移

        var c = MakeCloud(30);
        var merged2 = CloudPointIO.Merge(new[] { a, c });
        Assert.Equal(130, merged2.Count);
        Assert.NotNull(merged2.ColorsRgb);
    }
}
