using ComUI.Plugin.Cloud3D;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// 量测 CSV 生成（CloudMeasurements.ToCsv）：表头/逐段行/total 行/InvariantCulture 小数点/空输入。
/// </summary>
public class CloudMeasurementsTests
{
    [Fact]
    public void ToCsv_EmptyInput_HeaderOnly()
    {
        var csv = CloudMeasurements.ToCsv(Array.Empty<double[]>());
        Assert.StartsWith("polyline,seq,total_length", csv);
    }

    [Fact]
    public void ToCsv_SingleSegment_RoundtripFields()
    {
        // 单段折线（两点）：x1..z2 精确、total=seg（3-4-5 三角形斜边）
        var line = new double[] { 1.0, 2.0, 3.0, 4.0, 6.0, 3.0 };
        var csv = CloudMeasurements.ToCsv(new[] { line });
        var rows = csv.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        Assert.Equal(3, rows.Length);   // 表头 + 逐段行 + total 行（数据行=纯数字，无 polyline 前缀）
        Assert.StartsWith("1,1,5.0000", rows[1]);
        Assert.Contains("1.0000,2.0000,3.0000,4.0000,6.0000,3.0000", rows[1]);
        Assert.StartsWith("1,total", rows[2]);
        Assert.Contains("5.0000", rows[2]);
    }

    [Fact]
    public void ToCsv_MultiSegment_TotalAccumulates()
    {
        // 三点折线：段1=5（3-4-5），段2=5 → 总长 10
        var line = new double[] { 0, 0, 0, 3, 4, 0, 6, 8, 0 };
        var csv = CloudMeasurements.ToCsv(new[] { line });
        Assert.Contains(",10.0000", csv);   // total 行
        Assert.Contains(",5.0000", csv);    // 逐段行
    }

    [Fact]
    public void ToCsv_InvariantCulture_DecimalPoint()
    {
        // InvariantCulture：小数点恒为 '.'（当前文化为逗号小数时也不变）
        var line = new double[] { 0.5, 0.5, 0, 1.5, 0.5, 0 };
        var csv = CloudMeasurements.ToCsv(new[] { line });
        var dataRow = csv.Split('\n')[1];
        Assert.True(dataRow.Contains("0.5000") || dataRow.Contains("1.5000"), "小数点应为 '.'");
    }
}

/// <summary>
/// 拾取历史 CSV（HistoryToCsv）：表头 seq,x,y,z + InvariantCulture 逐行 + 空历史仅表头。
/// </summary>
public class CloudHistoryToCsvTests
{
    [Fact]
    public void HistoryToCsv_HeaderAndInvariantRows()
    {
        var h = new List<(double, double, double)> { (1.5, -2.25, 300.0), (4.0, 5.0, 6.0) };
        var csv = CloudMeasurements.HistoryToCsv(h);
        var rows = csv.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        Assert.StartsWith("seq,x,y,z", rows[0]);
        Assert.Equal(3, rows.Length);   // 表头 + 2 行
        Assert.StartsWith("1,1.5000,-2.2500,300.0000", rows[1]);
        Assert.StartsWith("2,4.0000,5.0000,6.0000", rows[2]);
    }

    [Fact]
    public void HistoryToCsv_Empty_HeaderOnly()
    {
        var csv = CloudMeasurements.HistoryToCsv(Array.Empty<(double, double, double)>());
        Assert.StartsWith("seq,x,y,z", csv);
        Assert.Equal(1, csv.Split('\n', StringSplitOptions.RemoveEmptyEntries).Length);   // 仅表头
    }
}

/// <summary>
/// 拾取历史截断（TrimHistory）：FIFO 截断、连续重复折叠（先去重后截断语义）。
/// </summary>
public class CloudTrimHistoryTests
{
    [Fact]
    public void TrimHistory_UnderLimit_Passthrough()
    {
        var h = new List<(double, double, double)> { (1, 1, 1), (2, 2, 2) };
        var r = CloudMeasurements.TrimHistory(h, 50);
        Assert.Equal(2, r.Length);
    }

    [Fact]
    public void TrimHistory_OverLimit_KeepsMostRecent()
    {
        var h = new List<(double, double, double)>();
        for (int i = 0; i < 60; i++) h.Add((i, i, i));
        var r = CloudMeasurements.TrimHistory(h, 50);
        Assert.Equal(50, r.Length);
        Assert.Equal((59, 59, 59), r[^1]);   // 保留最近
    }

    [Fact]
    public void TrimHistory_CollapsesConsecutiveDuplicates_BeforeTruncation()
    {
        // 前 45 条连续重复点（折叠后 1 条）+ 尾部 10 个不同点 → 折叠后 11 条 ≤ 50 → 全保留
        var h = new List<(double, double, double)>();
        for (int i = 0; i < 45; i++) h.Add((1, 1, 1));   // 连续重复
        for (int i = 0; i < 10; i++) h.Add((100 + i, 0, 0));
        var r = CloudMeasurements.TrimHistory(h, 50);
        Assert.Equal(11, r.Length);   // 1（折叠）+ 10
        Assert.Equal((109, 0, 0), r[^1]);
    }

    [Fact]
    public void TrimHistory_DuplicateCollapse_WinsOverLimit()
    {
        // 60 条连续重复（超限 50）→ 折叠后 1 条，不应被截断为 50 条重复
        var h = new List<(double, double, double)>();
        for (int i = 0; i < 60; i++) h.Add((5, 5, 5));
        var r = CloudMeasurements.TrimHistory(h, 50);
        Assert.Single(r);
    }
}
