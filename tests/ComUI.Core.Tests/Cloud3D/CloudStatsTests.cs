using ComUI.Plugin.Cloud3D;
using OpenTK.Mathematics;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// 实体统计纯函数（CloudStats）：对角线/体积/密度/零体积边界/选中率 + BuildLines 行集合。
/// 语义约定：退化盒（任一轴尺寸 ≤ 0）体积=0、密度=null；选中率相对实体总点数 count。
/// </summary>
public class CloudStatsTests
{
    private static readonly Vector3 Origin = new(0f, 0f, 0f);

    [Fact]
    public void DiagonalLength_Box345_Is5()
    {
        // 平面盒 3×4×0：体积为 0 但对角线仍是真实 5（对角线不依赖体积）
        var d = CloudStats.DiagonalLength(Origin, new Vector3(3f, 4f, 0f));
        Assert.Equal(5.0, d, 10);
    }

    [Fact]
    public void DiagonalLength_ThreeDimensions_IsHypotenuse()
    {
        // 2×3×6 长方体：sqrt(4+9+36)=7
        var d = CloudStats.DiagonalLength(new Vector3(-1f, -2f, 1f), new Vector3(1f, 1f, 7f));
        Assert.Equal(7.0, d, 10);
    }

    [Fact]
    public void Volume_Box_IsProductOfSizes()
    {
        // 2×3×4 = 24（非原点包围盒：尺寸取 max-min 而非绝对坐标）
        Assert.Equal(24.0, CloudStats.Volume(Origin, new Vector3(2f, 3f, 4f)), 10);
        Assert.Equal(30.0, CloudStats.Volume(new Vector3(-1f, -1f, 1f), new Vector3(1f, 2f, 6f)), 10);   // 2×3×5
    }

    [Fact]
    public void Volume_DegenerateBoxes_AreZero()
    {
        // 任一轴退化（=0 或反向）→ 体积 0（防假密度）
        Assert.Equal(0.0, CloudStats.Volume(Origin, new Vector3(2f, 3f, 0f)), 10);
        Assert.Equal(0.0, CloudStats.Volume(new Vector3(5f, 0f, 0f), new Vector3(3f, 2f, 3f)), 10);
    }

    [Fact]
    public void Density_Normal_IsLoadedPerUnitVolume()
    {
        // 2400 点 / 24 unit³ = 100 点/unit³
        var d = CloudStats.Density(2400, Origin, new Vector3(2f, 3f, 4f));
        Assert.NotNull(d);
        Assert.Equal(100.0, d!.Value, 10);
    }

    [Fact]
    public void Density_ZeroVolumeOrZeroPoints_ReturnsNull()
    {
        // 体积 0 → null（界面显示"—"）；点数 0 → null
        Assert.Null(CloudStats.Density(2400, Origin, new Vector3(2f, 3f, 0f)));
        Assert.Null(CloudStats.Density(0, Origin, new Vector3(2f, 3f, 4f)));
    }

    [Fact]
    public void SelectionRatePercent_RelativeToCount()
    {
        // 50/200 = 25%；负/空实体 count ≤ 0 → 0（防除零 NaN）
        Assert.Equal(25.0, CloudStats.SelectionRatePercent(50, 200), 10);
        Assert.Equal(100.0, CloudStats.SelectionRatePercent(7, 7), 10);
        Assert.Equal(0.0, CloudStats.SelectionRatePercent(5, 0), 10);
    }

    [Fact]
    public void BuildLines_SixRows_WithLabelsAndDensityPlaceholder()
    {
        // 零体积实体：密度行显示占位"—"，其余行数值就位
        var lines = CloudStats.BuildLines(1200, 800, 100, Origin, new Vector3(2f, 3f, 0f));
        Assert.Equal(6, lines.Count);
        Assert.Equal(("点数", "1,200"), lines[0]);
        Assert.Equal(("已加载", "800"), lines[1]);
        Assert.True(lines[2].Label == "选中率" && lines[2].Value.StartsWith("8.3%"));
        Assert.True(lines[3].Label == "包围盒" && lines[3].Value.Contains("2.00") && lines[3].Value.Contains("3.00"));
        Assert.Equal(("对角线", "3.606"), (lines[4].Label, lines[4].Value));   // sqrt(4+9)=3.6055→F3
        Assert.Equal(("密度", "—"), (lines[5].Label, lines[5].Value));
    }

    [Fact]
    public void BuildLines_NormalEntity_DensityHasValue()
    {
        // 2400 点 2×3×4 盒：密度 100.0 点/unit³；InvariantCulture 小数点
        var lines = CloudStats.BuildLines(2400, 2400, 0, Origin, new Vector3(2f, 3f, 4f));
        Assert.Contains("点/unit³", lines[5].Value);
        Assert.StartsWith("100.0", lines[5].Value);
    }
}
