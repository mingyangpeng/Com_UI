using ComUI.Plugin.Cloud3D;
using ComUI.Sdk;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// LAS 1.2 导入导出（CloudLas）：往返精度（scale 0.001 ±0.0005）、负坐标（offset 机制）、
/// RGB 保真（<<8 写 / >>8 读）、魔数与 point format 校验拒绝。
/// </summary>
public class CloudLasTests
{
    private static CloudPayload MakeCloud(int n, bool colored = true, float x0 = 0f)
    {
        var pts = new float[n * 3];
        var cols = colored ? new byte[n * 3] : null;
        var rng = new Random(n);
        for (int i = 0; i < n; i++)
        {
            pts[i * 3] = x0 + (float)(rng.NextDouble() * 500) - 100;      // 含负区间（-100~400mm）
            pts[i * 3 + 1] = (float)(rng.NextDouble() * 300) - 150;
            pts[i * 3 + 2] = (float)(rng.NextDouble() * 50);
            if (cols is not null)
            {
                cols[i * 3] = (byte)(i % 256);
                cols[i * 3 + 1] = (byte)((i * 7) % 256);
                cols[i * 3 + 2] = (byte)((i * 13) % 256);
            }
        }
        return new CloudPayload { Id = "las", Count = n, Points = pts, ColorsRgb = cols };
    }

    [Fact]
    public void Las_SaveLoad_Roundtrip_WithNegativeCoordsAndColor()
    {
        var src = MakeCloud(500);
        var path = Path.Combine(Path.GetTempPath(), "comui_las_" + Guid.NewGuid().ToString("N")[..8] + ".las");
        try
        {
            CloudLas.Save(path, src);
            var back = CloudPointIO.LoadFrom(path);

            Assert.Equal(src.Count, back.Count);
            Assert.NotNull(back.ColorsRgb);
            // 精度断言：scale 0.001 → 还原误差 ≤ 0.0005mm
            for (int i = 0; i < src.Count; i++)
            {
                Assert.InRange(back.Points[i * 3], src.Points[i * 3] - 0.0006f, src.Points[i * 3] + 0.0006f);
                Assert.InRange(back.Points[i * 3 + 1], src.Points[i * 3 + 1] - 0.0006f, src.Points[i * 3 + 1] + 0.0006f);
                Assert.InRange(back.Points[i * 3 + 2], src.Points[i * 3 + 2] - 0.0006f, src.Points[i * 3 + 2] + 0.0006f);
            }
            // RGB 保真（<<8 写 / >>8 读 应无损）
            Assert.Equal(src.ColorsRgb![3], back.ColorsRgb![3]);
            Assert.Equal(src.ColorsRgb[src.Count * 3 - 1], back.ColorsRgb[src.Count * 3 - 1]);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Las_SaveLoad_NoColor_Format0()
    {
        var src = MakeCloud(30, colored: false);
        var path = Path.Combine(Path.GetTempPath(), "comui_las_" + Guid.NewGuid().ToString("N")[..8] + ".las");
        try
        {
            CloudLas.Save(path, src);
            // format 0 校验：直接读 header byte 84
            var header = new byte[227];
            using (var fs = File.OpenRead(path)) { fs.ReadExactly(header); }
            Assert.Equal(0, header[84]);
            Assert.Equal(20, BitConverter.ToUInt16(header, 85));   // record length

            var back = CloudPointIO.LoadFrom(path);
            Assert.Equal(30, back.Count);
            Assert.Null(back.ColorsRgb);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Las_Load_BadMagic_Throws()
    {
        var path = Path.Combine(Path.GetTempPath(), "comui_las_bad_" + Guid.NewGuid().ToString("N")[..8] + ".las");
        try
        {
            File.WriteAllBytes(path, new byte[400]);   // 全零：无 LASF 魔数
            var ex = Assert.Throws<Exception>(() => CloudLas.Load(path));
            Assert.Contains("LASF", ex.Message);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Las_Save_RangeOverflow_Throws()
    {
        // 超过 int32×scale 范围 → 明确报错（不静默截断）
        var pts = new float[] { 0f, 0f, 0f, 5_000_000f, 0f, 0f };   // 500 万 mm = 5km > 2147 万？不超——放大到 3000 万
        pts[3] = 30_000_000f;
        var src = new CloudPayload { Count = 2, Points = pts };
        var path = Path.Combine(Path.GetTempPath(), "comui_las_of_" + Guid.NewGuid().ToString("N")[..8] + ".las");
        try
        {
            var ex = Assert.Throws<Exception>(() => CloudLas.Save(path, src));
            Assert.Contains("int32", ex.Message);
        }
        finally { if (File.Exists(path)) File.Delete(path); }
    }
}
