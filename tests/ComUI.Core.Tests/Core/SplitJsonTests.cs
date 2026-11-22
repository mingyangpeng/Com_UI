using ComUI.Core.Split;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>分屏树会话序列化回环。</summary>
public class SplitJsonTests
{
    [Fact]
    public void ToJson_FromJson_Roundtrip()
    {
        var layout = new SplitLayout();
        var root = (SplitLeaf)layout.Root;
        root.PanelIds.Add("welcome");
        var l2 = layout.Split(root, SplitSide.Right);
        l2.PanelIds.Add("ui.nodegraph.main");
        l2.PanelIds.Add("doc:1");
        l2.ActivePanelId = "doc:1";

        var json = layout.ToJson();
        var layout2 = new SplitLayout();
        layout2.LoadFromJson(json);

        var leaves = layout2.Leaves().ToList();
        Assert.Equal(2, leaves.Count);
        Assert.Equal(new[] { "welcome" }, leaves[0].PanelIds);
        Assert.Equal(new[] { "ui.nodegraph.main", "doc:1" }, leaves[1].PanelIds);
        Assert.Equal("doc:1", leaves[1].ActivePanelId);
    }

    [Fact]
    public void LoadFromJson_ThrowsOnInvalid()
    {
        var layout = new SplitLayout();
        Assert.ThrowsAny<Exception>(() => layout.LoadFromJson("{ \"type\": \"container\", \"children\": [ { \"type\": \"leaf\", \"panels\": [] } ] }"));
    }

    [Fact]
    public void NodeDefs_ContainsDrawOperators()
    {
        Assert.True(ComUI.Plugin.NodeGraph.NodeDefs.Map.ContainsKey("draw.2d"), "draw.2d 未注册");
        Assert.True(ComUI.Plugin.NodeGraph.NodeDefs.Map.ContainsKey("draw.3d"), "draw.3d 未注册");
    }

    [Fact]
    public void LoadFromJson_DefaultsMissingStars()
    {
        var layout = new SplitLayout();
        layout.LoadFromJson("""
            { "type": "container", "vertical": false,
              "children": [ { "type": "leaf", "panels": ["a"] }, { "type": "leaf", "panels": ["b"], "active": "b" } ] }
            """);
        var leaves = layout.Leaves().ToList();
        Assert.Equal(2, leaves.Count);
        Assert.Equal("b", leaves[1].ActivePanelId);   // 每格激活面板恢复
    }
}
