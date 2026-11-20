using ComUI.Plugin.Cloud3D;
using ComUI.Sdk;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// 点云栅格化（CloudRaster）：网格高度统计（max/mean/min + 空格掩膜）、jet 色带（与 GLSL 同参）、
/// 高度图 BGRA 输出（y 翻转对齐图像习惯、无点黑格）。
/// </summary>
public class CloudRasterTests
{
    /// <summary>3×3 网格 9 点：高度 = 10 + (行×10 + 列)（x=列 y=行 z=高度）。</summary>
    private static CloudPayload MakeGrid()
    {
        var pts = new float[9 * 3];
        int i = 0;
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 3; x++)
            {
                pts[i * 3] = x * 10f;
                pts[i * 3 + 1] = y * 10f;
                pts[i * 3 + 2] = 10f + y * 10f + x;
                i++;
            }
        return new CloudPayload { Id = "g", Count = 9, Points = pts };
    }

    [Fact]
    public void RasterizeHeight_MaxStat_AndMask()
    {
        var (heights, mask, w, h, minx, miny, spanx, spany, zmin, zmax) = CloudRaster.RasterizeHeight(MakeGrid(), 3, 3, "max");
        Assert.Equal(3, w);
        Assert.Equal(3, h);
        Assert.All(mask, m => Assert.True(m));   // 9 点 9 格全命中
        // 一点一格（3×3 点 ↔ 3×3 格）：heights[gy*3+cx] = z(x=cx, y=gy)
        Assert.Equal(10f, heights[0]);           // (x0,y0) → 10
        Assert.Equal(12f, heights[2]);           // (x20,y0) → 10+2
        Assert.Equal(32f, heights[2 * 3 + 2]);   // (x20,y20) → 10+20+2
        Assert.Equal(10f, zmin);
        Assert.Equal(32f, zmax);
    }

    [Fact]
    public void RasterizeHeight_MeanStat()
    {
        // 单格多点：4 点完全重叠（span→1e-9 保护 → 同格 cx=cy=0）→ mean
        var pts = new float[4 * 3]
        {
            0, 0, 10, 0, 0, 20, 0, 0, 30, 0, 0, 40,
        };
        var src = new CloudPayload { Count = 4, Points = pts };
        var (heights, mask, _, _, _, _, _, _, _, _) = CloudRaster.RasterizeHeight(src, 4, 4, "mean");
        int hit = -1;
        for (int i = 0; i < 16; i++)
            if (mask[i]) hit = i;
        Assert.True(hit >= 0);
        Assert.Equal(25f, heights[hit]);   // (10+20+30+40)/4
    }

    [Fact]
    public void Jet_MonotonicAndClamped()
    {
        var (r0, g0, b0) = CloudRaster.Jet(0f);
        Assert.Equal(0, r0); Assert.Equal(0, g0); Assert.Equal(255, b0);   // 起点=蓝
        var (r1, g1, b1) = CloudRaster.Jet(1f);
        Assert.Equal(255, r1); Assert.Equal(0, g1); Assert.Equal(0, b1);   // 终点=红
        var (rm, _, _) = CloudRaster.Jet(0.5f);
        Assert.True(rm < 40);   // 中点偏绿（红分量低）
        var (ro, _, _) = CloudRaster.Jet(5f);   // 越界钳制
        Assert.Equal(255, ro);
    }

    [Fact]
    public void ToImage_FlipAndBlackBackground()
    {
        // 2×2 栅格：仅底行（gy=0）有点 → 图像应画在最下行（y 翻转），顶行黑
        var heights = new float[] { 10, 10, 0, 0 };
        var mask = new[] { true, true, false, false };
        var img = CloudRaster.ToImage(heights, mask, 2, 2, 10, 10, "t");
        Assert.Equal(2, img.Width);
        Assert.Equal(2, img.Height);
        Assert.Equal(16, img.PixelsBgra.Length);
        // 图像第一行（顶部）= 栅格 gy=1 → 无点 → 黑
        Assert.Equal(0, img.PixelsBgra[2]);   // 顶行右上 B=0
        // 图像第二行（底部）= 栅格 gy=0 → 有点 → jet 非 0（至少 alpha/亮度非黑）
        int offBottom = (1 * 2 + 0) * 4;
        Assert.Equal(255, img.PixelsBgra[offBottom]);       // t=0 → jet 纯蓝（B=255）
        Assert.Equal(0, img.PixelsBgra[offBottom + 2]);     // R=0
    }
}
