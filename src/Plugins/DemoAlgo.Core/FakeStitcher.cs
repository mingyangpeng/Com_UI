using System.Diagnostics;
using ComUI.Sdk;

namespace DemoAlgo.Core;

/// <summary>拼接结果（JSON outputs 按属性名发布）。</summary>
public sealed class StitchResult
{
    public ImagePayload Image { get; init; } = new();
    public CloudPayload Cloud { get; init; } = new();
}

/// <summary>
/// 模拟拼接算法 —— 代替真实算法的占位实现：
/// 阶段式进度 + 可取消，产出一幅测试图卡和一个环面点云。
/// 真实接入时，把 Stitch 换成调用你们算法 DLL 的入口即可，JSON 描述不变。
/// </summary>
public static class FakeStitcher
{
    public static StitchResult Stitch(
        int points,
        double voxel,
        string mode,
        IProgress<string>? progress,
        CancellationToken ct)
    {
        var sw = Stopwatch.StartNew();
        void Report(int pct, string msg) => progress?.Report($"[{pct,3}%] {msg}");

        Report(0, $"开始拼接（模式 {mode}，体素 {voxel:F2}mm，目标 {points} 点）…");

        // 阶段 1：模拟读取与粗配准
        for (int step = 1; step <= 4; step++)
        {
            ct.ThrowIfCancellationRequested();
            Thread.Sleep(350);
            Report(step * 15, $"阶段 {step}/6：粗配准迭代 {step}…");
        }

        ct.ThrowIfCancellationRequested();
        Thread.Sleep(300);
        Report(70, "生成拼接结果…");

        var image = MakeTestImage(800, 600);
        var cloud = MakeTorusCloud(points);

        ct.ThrowIfCancellationRequested();
        Report(95, "写入结果…");
        Thread.Sleep(200);

        Report(100, $"完成，耗时 {sw.ElapsedMilliseconds}ms");
        return new StitchResult
        {
            Image = new ImagePayload { Width = image.Width, Height = image.Height, PixelsBgra = image.Pixels, Source = "演示拼接结果" },
            Cloud = new CloudPayload { Points = cloud.Points, ColorsRgb = cloud.Colors, Count = cloud.Count, Source = "演示拼接点云" },
        };
    }

    // ==================== 测试图卡 ====================

    private static (int Width, int Height, byte[] Pixels) MakeTestImage(int w, int h)
    {
        var px = new byte[w * h * 4];
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                int i = (y * w + x) * 4;
                // 渐变底
                byte r = (byte)(x * 255 / w);
                byte g = (byte)(y * 255 / h);
                byte b = (byte)(120);

                // 同心圆环
                double dx = x - w / 2.0, dy = y - h / 2.0;
                double d = Math.Sqrt(dx * dx + dy * dy);
                if ((int)(d / 28) % 2 == 0)
                {
                    r = (byte)Math.Min(255, r + 70);
                    g = (byte)Math.Min(255, g + 30);
                }

                // 十字线
                if (Math.Abs(dx) < 1.5 || Math.Abs(dy) < 1.5)
                {
                    r = 255; g = 255; b = 60;
                }

                px[i] = b; px[i + 1] = g; px[i + 2] = r; px[i + 3] = 255;
            }
        }
        return (w, h, px);
    }

    // ==================== 环面点云（高度伪彩数据源） ====================

    private static (float[] Points, byte[] Colors, int Count) MakeTorusCloud(int count)
    {
        const double R = 2.4, r = 1.0;
        var pts = new float[count * 3];
        var colors = new byte[count * 3];
        var rnd = new Random(42);

        double zMin = -r, zMax = r;
        for (int i = 0; i < count; i++)
        {
            double u = rnd.NextDouble() * Math.PI * 2;
            double v = rnd.NextDouble() * Math.PI * 2;
            double x = (R + r * Math.Cos(v)) * Math.Cos(u) + (rnd.NextDouble() - 0.5) * 0.02;
            double y = r * Math.Sin(v) + (rnd.NextDouble() - 0.5) * 0.02;
            double z = (R + r * Math.Cos(v)) * Math.Sin(u) + (rnd.NextDouble() - 0.5) * 0.02;

            pts[i * 3] = (float)x;
            pts[i * 3 + 1] = (float)y;      // Y 为"高度"轴
            pts[i * 3 + 2] = (float)z;

            // 按高度上色（蓝→青→绿→黄→红），让查看器"库颜色"模式有内容
            double t = (y - zMin) / (zMax - zMin);
            var (cr, cg, cb) = HeightColor(t);
            colors[i * 3] = cr;
            colors[i * 3 + 1] = cg;
            colors[i * 3 + 2] = cb;
        }
        return (pts, colors, count);
    }

    private static (byte, byte, byte) HeightColor(double t)
    {
        t = Math.Clamp(t, 0, 1) * 4;
        return t switch
        {
            < 1 => Lerp((48, 60, 200), (0, 176, 240), t),
            < 2 => Lerp((0, 176, 240), (48, 208, 128), t - 1),
            < 3 => Lerp((48, 208, 128), (240, 208, 64), t - 2),
            _ => Lerp((240, 208, 64), (240, 64, 48), t - 3),
        };

        static (byte, byte, byte) Lerp((byte, byte, byte) a, (byte, byte, byte) b, double k)
        {
            k = Math.Clamp(k, 0, 1);
            return (
                (byte)(a.Item1 + (b.Item1 - a.Item1) * k),
                (byte)(a.Item2 + (b.Item2 - a.Item2) * k),
                (byte)(a.Item3 + (b.Item3 - a.Item3) * k));
        }
    }
}
