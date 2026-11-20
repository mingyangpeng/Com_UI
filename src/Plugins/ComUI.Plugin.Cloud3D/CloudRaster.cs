using ComUI.Sdk;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 点云栅格化（纯函数，可测试）：把点云按 XY 网格聚合高度统计（max/mean/min），
/// 映射 jet 色带生成 BGRA 图像（ImagePayload）→ 总线 image/stitched → 2D 工作台联动显示。
/// 对应 CloudCompare 的 Rasterize（真值面/顶面）。
/// </summary>
public static class CloudRaster
{
    /// <summary>高度栅格：网格尺寸 gridW×gridH，单元统计 z（max=顶面/mean=均值/min=谷底）。空格 mask=false。</summary>
    public static (float[] Heights, bool[] Mask, int W, int H, double MinX, double MinY, double SpanX, double SpanY, double ZMin, double ZMax)
        RasterizeHeight(CloudPayload src, int gridW, int gridH, string stat = "max")
    {
        if (gridW < 2 || gridH < 2) throw new Exception("栅格分辨率至少 2×2");
        if (src.Count == 0) throw new Exception("点云为空");

        double minx = double.MaxValue, miny = double.MaxValue, maxx = double.MinValue, maxy = double.MinValue;
        double zmin = double.MaxValue, zmax = double.MinValue;
        for (int i = 0; i < src.Count; i++)
        {
            double x = src.Points[i * 3], y = src.Points[i * 3 + 1];
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
            if (src.Points[i * 3 + 2] < zmin) zmin = src.Points[i * 3 + 2];
            if (src.Points[i * 3 + 2] > zmax) zmax = src.Points[i * 3 + 2];
        }
        double spanx = Math.Max(maxx - minx, 1e-9), spany = Math.Max(maxy - miny, 1e-9);

        var heights = new float[gridW * gridH];
        var mask = new bool[gridW * gridH];
        var sums = new double[gridW * gridH];
        var counts = new int[gridW * gridH];
        var maxs = new float[gridW * gridH];
        var mins = new float[gridW * gridH];
        Array.Fill(maxs, float.MinValue);
        Array.Fill(mins, float.MaxValue);

        for (int i = 0; i < src.Count; i++)
        {
            double x = src.Points[i * 3], y = src.Points[i * 3 + 1], z = src.Points[i * 3 + 2];
            int cx = Math.Clamp((int)((x - minx) / spanx * gridW), 0, gridW - 1);
            int cy = Math.Clamp((int)((y - miny) / spany * gridH), 0, gridH - 1);
            int idx = cy * gridW + cx;
            counts[idx]++;
            sums[idx] += z;
            if (z > maxs[idx]) maxs[idx] = (float)z;
            if (z < mins[idx]) mins[idx] = (float)z;
        }

        for (int idx = 0; idx < gridW * gridH; idx++)
        {
            if (counts[idx] == 0) continue;
            mask[idx] = true;
            heights[idx] = stat switch
            {
                "mean" => (float)(sums[idx] / counts[idx]),
                "min" => mins[idx],
                _ => maxs[idx],
            };
        }
        return (heights, mask, gridW, gridH, minx, miny, spanx, spany, zmin, zmax);
    }

    /// <summary>jet 色带（与 GlCloudRenderer 的 GLSL jet 完全同参：蓝→青→绿→黄→红）。t 钳制 [0,1]。</summary>
    public static (byte R, byte G, byte B) Jet(float t)
    {
        t = Math.Clamp(t, 0f, 1f);
        double r, g, b;
        if (t < 0.25) { r = 0; g = Lerp(0, 1, t / 0.25); b = 1; }
        else if (t < 0.5) { r = 0; g = Lerp(1, 0.8, (t - 0.25) / 0.25); b = Lerp(1, 0, (t - 0.25) / 0.25); }
        else if (t < 0.75) { r = Lerp(0, 1, (t - 0.5) / 0.25); g = Lerp(0.8, 1, (t - 0.5) / 0.25); b = 0; }
        else { r = 1; g = Lerp(1, 0, (t - 0.75) / 0.25); b = 0; }
        return ((byte)Math.Round(r * 255), (byte)Math.Round(g * 255), (byte)Math.Round(b * 255));

        static double Lerp(double a, double b2, double t) => a + (b2 - a) * t;
    }

    /// <summary>高度栅格 → BGRA 图像（jet 着色、无点=黑；图像 y 向下、点云 y 向上 → 行翻转对齐显示习惯）。</summary>
    public static ImagePayload ToImage(float[] heights, bool[] mask, int w, int h, double zmin, double zmax, string id)
    {
        var px = new byte[w * h * 4];
        double span = Math.Max(zmax - zmin, 1e-9);
        for (int gy = 0; gy < h; gy++)
        {
            int row = h - 1 - gy;   // 图像 y 向下：栅格第 gy 行画到倒数第 gy 行（上=高处）
            for (int gx = 0; gx < w; gx++)
            {
                int idx = gy * w + gx;
                int off = (row * w + gx) * 4;
                if (!mask[idx]) { px[off] = 0; px[off + 1] = 0; px[off + 2] = 0; px[off + 3] = 255; continue; }
                float t = (heights[idx] - (float)zmin) / (float)span;
                var (r, g, b2) = Jet(t);
                px[off] = b2; px[off + 1] = g; px[off + 2] = r; px[off + 3] = 255;
            }
        }
        return new ImagePayload { Id = id, Width = w, Height = h, PixelsBgra = px, Source = "3D 高度图" };
    }
}
