using System.Globalization;
using OpenTK.Mathematics;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 实体统计纯函数（可测试）：属性面板「实体统计」分段的数据源。
/// 数值计算（包围盒尺寸/对角线/体积/密度/选中率）与格式化拆层——数值层返回基本类型便于单测，
/// 格式化层（BuildLines）产出 label/value 行集合供 Cloud3DView 渲染。
/// 输入取自 CloudData（Count/Loaded/SelCount/BoundsMin/BoundsMax），调用方负责事件驱动刷新（禁每帧重算）。
/// </summary>
public static class CloudStats
{
    /// <summary>包围盒三轴尺寸（max-min 逐分量；坏数据（min&gt;max）钳到非负，防负尺寸毒化体积/对角线）。</summary>
    public static Vector3 BoundsSize(Vector3 boundsMin, Vector3 boundsMax)
        => Vector3.ComponentMax(boundsMax - boundsMin, Vector3.Zero);

    /// <summary>包围盒对角线长度（三维欧氏，double 精度避免大坐标 float 溢出）。退化盒（体积 0）仍返回真实对角线。</summary>
    public static double DiagonalLength(Vector3 boundsMin, Vector3 boundsMax)
    {
        var s = BoundsSize(boundsMin, boundsMax);
        return Math.Sqrt((double)s.X * s.X + (double)s.Y * s.Y + (double)s.Z * s.Z);
    }

    /// <summary>包围盒体积（任一轴尺寸 ≤ 0 → 0：扁平/退化盒不产生假密度）。</summary>
    public static double Volume(Vector3 boundsMin, Vector3 boundsMax)
    {
        var s = BoundsSize(boundsMin, boundsMax);
        return (double)s.X * s.Y * s.Z;
    }

    /// <summary>密度 = loaded / 包围盒体积（点/unit³）；体积 ≤ 0 或点数 0（loaded ≤ 0）→ null（界面显示「—」）。</summary>
    public static double? Density(int loaded, Vector3 boundsMin, Vector3 boundsMax)
    {
        var vol = Volume(boundsMin, boundsMax);
        if (vol <= 0 || loaded <= 0) return null;
        return loaded / vol;
    }

    /// <summary>选中率（%）= selCount / count × 100（相对实体总点数）；count ≤ 0 → 0（防空实体除零）。</summary>
    public static double SelectionRatePercent(int selCount, int count)
        => count <= 0 ? 0 : 100.0 * selCount / count;

    /// <summary>「实体统计」行集合（InvariantCulture 格式化）：点数 / 已加载 / 选中率 / 包围盒 XYZ / 对角线 / 密度。</summary>
    public static IReadOnlyList<(string Label, string Value)> BuildLines(
        int count, int loaded, int selCount, Vector3 boundsMin, Vector3 boundsMax)
    {
        var s = BoundsSize(boundsMin, boundsMax);
        var dens = Density(loaded, boundsMin, boundsMax);
        return new (string Label, string Value)[]
        {
            ("点数", count.ToString("N0", CultureInfo.InvariantCulture)),
            ("已加载", loaded.ToString("N0", CultureInfo.InvariantCulture)),
            ("选中率", $"{SelectionRatePercent(selCount, count):F1}%（{selCount.ToString("N0", CultureInfo.InvariantCulture)} 点）"),
            ("包围盒", $"{s.X:F2} × {s.Y:F2} × {s.Z:F2}"),
            ("对角线", $"{DiagonalLength(boundsMin, boundsMax):F3}"),
            ("密度", dens is { } d ? $"{d.ToString("N1", CultureInfo.InvariantCulture)} 点/unit³" : "—"),
        };
    }
}
