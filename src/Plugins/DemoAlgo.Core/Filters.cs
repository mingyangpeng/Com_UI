using ComUI.Sdk;

namespace DemoAlgo.Core;

/// <summary>
/// 可注册为节点图算子的图像处理方法示例。
/// 与 demo.node.json 配套：JSON 声明端口/参数，方法按参数名绑定。
/// </summary>
public static class Filters
{
    /// <summary>伽马校正：输出 = 255 * (v/255)^(1/gamma)。</summary>
    public static ImagePayload Gamma(ImagePayload input, double gamma, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        var lut = new byte[256];
        for (int i = 0; i < 256; i++)
            lut[i] = (byte)Math.Clamp(255 * Math.Pow(i / 255.0, 1.0 / Math.Max(0.05, gamma)), 0, 255);

        var px = new byte[input.PixelsBgra.Length];
        for (int i = 0; i < px.Length; i += 4)
        {
            px[i] = lut[input.PixelsBgra[i]];
            px[i + 1] = lut[input.PixelsBgra[i + 1]];
            px[i + 2] = lut[input.PixelsBgra[i + 2]];
            px[i + 3] = 255;
        }
        return new ImagePayload
        {
            Width = input.Width,
            Height = input.Height,
            PixelsBgra = px,
            Source = $"伽马 {gamma:F2}",
        };
    }
}
