using ComUI.Core;
using Xunit;

namespace ComUI.Core.Tests;

public class HostConfigTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "comui-tests-" + Guid.NewGuid().ToString("N")[..8]);

    private void WriteConfig(string json) => File.WriteAllText(Path.Combine(_dir, "host.json"), json);

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, true);
    }

    [Fact]
    public void MissingFile_ReturnsDefaults()
    {
        var cfg = HostConfig.Load(_dir);
        Assert.Equal("comdll", cfg.PluginDirectory);
        Assert.True(cfg.AutoOpenPlugins);
        Assert.Empty(cfg.RequiredPlugins);
    }

    [Fact]
    public void JsoncWithComments_Parses()
    {
        Directory.CreateDirectory(_dir);
        WriteConfig("""
            {
              // 注释：插件目录
              "pluginDirectory": "mydll",
              "autoOpenPlugins": false,
              "requiredPlugins": [ "ui.nodegraph", "ui.extra" ]
            }
            """);
        var cfg = HostConfig.Load(_dir);
        Assert.Equal("mydll", cfg.PluginDirectory);
        Assert.False(cfg.AutoOpenPlugins);
        Assert.Equal(2, cfg.RequiredPlugins.Count);
        Assert.Contains("ui.nodegraph", cfg.RequiredPlugins);
    }

    [Fact]
    public void BrokenJson_FallsBackToDefaults()
    {
        Directory.CreateDirectory(_dir);
        WriteConfig("{ not valid json !!!");
        string? err = null;
        var cfg = HostConfig.Load(_dir, e => err = e);
        Assert.Equal("comdll", cfg.PluginDirectory);
        Assert.NotNull(err);   // 错误有反馈，不静默
    }
}
