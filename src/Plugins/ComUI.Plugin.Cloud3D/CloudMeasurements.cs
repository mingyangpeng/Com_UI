using System.Globalization;
using System.Text;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 量测/拾取的持久化纯函数（可测试）：量测清单 CSV 生成、拾取历史截断。
/// </summary>
public static class CloudMeasurements
{
    /// <summary>单条折线的世界坐标序列（每点 3 项 x,y,z）。</summary>
    public static string ToCsv(double[][] polylines)
    {
        var sb = new StringBuilder();
        sb.AppendLine("polyline,seq,total_length,x1,y1,z1,x2,y2,z2,segment_length");
        for (int pi = 0; pi < polylines.Length; pi++)
        {
            var pl = polylines[pi];
            if (pl.Length < 6) continue;   // 少于 2 点（3 项/点）不构成折线
            double total = 0;
            for (int i = 0; i + 5 < pl.Length; i += 3)
            {
                double dx = pl[i + 3] - pl[i], dy = pl[i + 4] - pl[i + 1], dz = pl[i + 5] - pl[i + 2];
                double seg = Math.Sqrt(dx * dx + dy * dy + dz * dz);
                total += seg;
                sb.AppendLine(FormattableString.Invariant(
                    $"{pi + 1},{i / 3 + 1},{total:F4},{pl[i]:F4},{pl[i + 1]:F4},{pl[i + 2]:F4},{pl[i + 3]:F4},{pl[i + 4]:F4},{pl[i + 5]:F4},{seg:F4}"));
            }
            sb.AppendLine(FormattableString.Invariant(
                $"{pi + 1},total,,,,,,,,,{total:F4}"));
        }
        return sb.ToString();
    }

    /// <summary>拾取历史 CSV（header seq,x,y,z；InvariantCulture 小数点）。</summary>
    public static string HistoryToCsv(IReadOnlyList<(double X, double Y, double Z)> history)
    {
        var sb = new StringBuilder();
        sb.AppendLine("seq,x,y,z");
        for (int i = 0; i < history.Count; i++)
            sb.AppendLine(FormattableString.Invariant(
                $"{i + 1},{history[i].X:F4},{history[i].Y:F4},{history[i].Z:F4}"));
        return sb.ToString();
    }

    /// <summary>拾取历史截断（保留最近 max 条 FIFO；去重连续重复点）。纯函数。</summary>
    public static (double X, double Y, double Z)[] TrimHistory(
        IReadOnlyList<(double X, double Y, double Z)> history, int max)
    {
        // 先折叠连续重复 → 再 FIFO 截断（语义：重复拾取不占历史额度）
        var collapsed = new List<(double, double, double)>(history.Count);
        foreach (var p in history)
        {
            if (collapsed.Count > 0 &&
                Math.Abs(collapsed[^1].Item1 - p.X) < 1e-9 &&
                Math.Abs(collapsed[^1].Item2 - p.Y) < 1e-9 &&
                Math.Abs(collapsed[^1].Item3 - p.Z) < 1e-9) continue;
            collapsed.Add(p);
        }
        if (collapsed.Count <= max) return collapsed.ToArray();
        return collapsed.Skip(collapsed.Count - max).ToArray();
    }
}
