using System.Collections.Generic;
using ComUI.Core;
using ComUI.Sdk;
using Xunit;

namespace ComUI.Core.Tests;

public class DataBusTests
{
    private readonly DataBus _bus = new();
    private readonly List<string> _disposedNames = new();

    [Fact]
    public void Publish_DeliversToSubscriber()
    {
        string? got = null;
        _bus.Subscribe<string>("t/a", v => got = v);
        _bus.Publish("t/a", "hello");
        Assert.Equal("hello", got);
    }

    [Fact]
    public void LateSubscribe_ReceivesLatestFrame()
    {
        _bus.Publish("t/b", "frame1");
        string? got = null;
        _bus.Subscribe<string>("t/b", v => got = v);
        Assert.Equal("frame1", got);   // 晚订阅立即拿到当前帧
    }

    [Fact]
    public void LatestFrame_ReplacedByNewer()
    {
        _bus.Publish("t/c", "old");
        _bus.Publish("t/c", "new");
        string? got = null;
        _bus.Subscribe<string>("t/c", v => got = v);
        Assert.Equal("new", got);
    }

    [Fact]
    public void Unsubscribe_StopsDelivery()
    {
        int count = 0;
        var sub = _bus.Subscribe<string>("t/d", _ => count++);
        _bus.Publish("t/d", "x");
        sub.Dispose();
        _bus.Publish("t/d", "y");
        Assert.Equal(1, count);
    }

    [Fact]
    public void Topics_AreIsolated()
    {
        string? a = null, b = null;
        _bus.Subscribe<string>("t/a", v => a = v);
        _bus.Subscribe<string>("t/b", v => b = v);
        _bus.Publish("t/a", "only-a");
        Assert.Equal("only-a", a);
        Assert.Null(b);
    }

    [Fact]
    public void TopicNames_CaseInsensitive()
    {
        string? got = null;
        _bus.Subscribe<string>("T/E", v => got = v);
        _bus.Publish("t/e", "case");
        Assert.Equal("case", got);
    }

    [Fact]
    public void ReplaceFrame_ReleasesDisposableOldFrame()
    {
        _disposedNames.Clear();
        var a = new DisposableFrame("A", _disposedNames);
        var b = new DisposableFrame("B", _disposedNames);
        _bus.Subscribe<DisposableFrame>("t/f", _ => { });
        _bus.Publish("t/f", a);
        _bus.Publish("t/f", b);   // 替换 → 旧帧 A 被总线释放
        Assert.Contains("A", _disposedNames);
        Assert.DoesNotContain("B", _disposedNames);   // 新帧（最新帧）不释放
        Assert.Same(b, _bus.TryGetLatest<DisposableFrame>("t/f", out var v) ? v : null);
    }

    private sealed class DisposableFrame(string name, List<string> log) : IDisposable
    {
        public string Name => name;
        public void Dispose() => log.Add(name);
    }
}
