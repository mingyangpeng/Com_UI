using ComUI.Plugin.Cloud3D;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// 视图设置持久化（CloudViewSettings）：JSON 往返一致、坏 JSON 回默认、字段钳制、lo>hi 互换、缺文件回默认。
/// </summary>
public class CloudViewSettingsTests
{
    private static string Tmp(string tag) => Path.Combine(Path.GetTempPath(), "comui_vs_" + tag + "_" + Guid.NewGuid().ToString("N")[..8] + ".json");

    [Fact]
    public void SaveLoad_Roundtrip_PreservesAll()
    {
        var path = Tmp("rt");
        try
        {
            var s = new CloudViewSettings
            {
                Stride = 4, FarClip = 123.5, ClipAxis = 2,
                ClipLo = 1.0, ClipHi = 5.0, ColorMin = -1.5, ColorMax = 9.5,
                Projection = 1, LockPitch = true,
            };
            CloudViewSettings.Save(path, s);
            var back = CloudViewSettings.Load(path);

            Assert.Equal(4, back.Stride);
            Assert.Equal(123.5, back.FarClip);
            Assert.Equal(2, back.ClipAxis);
            Assert.Equal(1.0, back.ClipLo);
            Assert.Equal(5.0, back.ClipHi);
            Assert.Equal(-1.5, back.ColorMin);
            Assert.Equal(9.5, back.ColorMax);
            Assert.Equal(1, back.Projection);
            Assert.True(back.LockPitch);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Load_BadJson_ReturnsDefaults()
    {
        var path = Tmp("bad");
        try
        {
            File.WriteAllText(path, "{ this is not json !!!");
            var s = CloudViewSettings.Load(path);
            Assert.Equal(1, s.Stride);       // 默认值
            Assert.Equal(0, s.Projection);
            Assert.Null(s.ClipLo);
            Assert.False(s.LockPitch);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Load_MissingFile_ReturnsDefaults()
    {
        var s = CloudViewSettings.Load(Path.Combine(Path.GetTempPath(), "comui_vs_missing_" + Guid.NewGuid().ToString("N")[..8] + ".json"));
        Assert.Equal(1, s.Stride);
        Assert.Null(s.ClipLo);
    }

    [Fact]
    public void Load_ClampsStrideAndAxis()
    {
        var path = Tmp("clamp");
        try
        {
            File.WriteAllText(path, """{"Stride": 99999, "ClipAxis": 9}""");
            var s = CloudViewSettings.Load(path);
            Assert.Equal(10000, s.Stride);
            Assert.Equal(2, s.ClipAxis);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Load_LoGreaterThanHi_Swapped()
    {
        var path = Tmp("swap");
        try
        {
            File.WriteAllText(path, """{"ClipLo": 5.0, "ClipHi": 1.0}""");
            var s = CloudViewSettings.Load(path);
            Assert.True(s.ClipLo <= s.ClipHi, "lo>hi 应在加载时互换");
        }
        finally { File.Delete(path); }
    }
}
