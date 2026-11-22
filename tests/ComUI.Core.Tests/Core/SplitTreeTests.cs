using ComUI.Core.Split;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>分屏树模型（阶段 1：拆分/移动/回收/退化）。</summary>
public class SplitTreeTests
{
    private static SplitLeaf AddPanel(SplitLayout layout, SplitLeaf leaf, string id)
    {
        leaf.PanelIds.Add(id);
        leaf.ActivePanelId = id;
        return leaf;
    }

    [Fact]
    public void SplitRoot_Right_ProducesContainerWithTwoLeaves()
    {
        var layout = new SplitLayout();
        var root = AddPanel(layout, (SplitLeaf)layout.Root, "a");
        var newLeaf = layout.Split(root, SplitSide.Right);

        // 根引用自动更新为容器；原面板留在第一格，新格为空
        Assert.IsType<SplitContainer>(layout.Root);
        Assert.Equal(new[] { "a" }, root.PanelIds);
        Assert.Empty(newLeaf.PanelIds);
        Assert.Same(root, ((SplitContainer)layout.Root).Children[0]);
        Assert.Same(newLeaf, ((SplitContainer)layout.Root).Children[1]);
    }

    [Fact]
    public void Split_Left_InsertsBefore_Right_InsertsAfter()
    {
        var layout = new SplitLayout();
        var root = AddPanel(layout, (SplitLeaf)layout.Root, "a");
        var right = layout.Split(root, SplitSide.Right);
        AddPanel(layout, right, "b");

        var container = Assert.IsType<SplitContainer>(right.Parent);
        Assert.False(container.Vertical);              // 左右拆 = 横排
        Assert.Same(root, container.Children[0]);
        Assert.Same(right, container.Children[1]);

        var top = layout.Split(root, SplitSide.Top);   // 异方向：包裹 root
        var wrapper = Assert.IsType<SplitContainer>(top.Parent);
        Assert.True(wrapper.Vertical);                 // 上下拆 = 纵排
        Assert.Same(top, wrapper.Children[0]);         // Top = 新叶在上
        Assert.Same(root, wrapper.Children[1]);
    }

    [Fact]
    public void SameDirectionSplit_InsertsSiblingWithoutWrapping()
    {
        var layout = new SplitLayout();
        var root = AddPanel(layout, (SplitLeaf)layout.Root, "a");
        var right = layout.Split(root, SplitSide.Right);
        var right2 = layout.Split(right, SplitSide.Right);   // 同方向 → 兄弟插入
        var container = Assert.IsType<SplitContainer>(right2.Parent);
        Assert.Equal(3, container.Children.Count);
        Assert.Same(right2, container.Children[2]);
    }

    [Fact]
    public void MovePanel_BetweenGroups_RecyclesEmptySource()
    {
        var layout = new SplitLayout();
        var root = AddPanel(layout, (SplitLeaf)layout.Root, "a");
        var other = AddPanel(layout, layout.Split(root, SplitSide.Right), "b");

        layout.MovePanel(root, "a", other);   // a 移入 other 组，root 组变空被回收

        Assert.Empty(root.PanelIds);
        Assert.Equal(new[] { "b", "a" }, other.PanelIds);
        // root 空叶回收后容器只剩单子 → 退化，other 成为根
        Assert.Null(other.Parent);
        Assert.Same(other, layout.Root);
    }

    [Fact]
    public void RemoveLastPanel_CollapsesTree()
    {
        var layout = new SplitLayout();
        var root = AddPanel(layout, (SplitLeaf)layout.Root, "a");
        var l2 = AddPanel(layout, layout.Split(root, SplitSide.Right), "b");
        var l3 = AddPanel(layout, layout.Split(l2, SplitSide.Bottom), "c");

        layout.RemovePanel(l3, "c");        // 空叶回收
        Assert.Empty(l3.PanelIds);
        layout.RemovePanel(l2, "b");        // 又空 → 容器单子退化 → 树回到单根
        Assert.Same(root, layout.Root);
        Assert.Equal(new[] { "a" }, root.PanelIds);
        Assert.Single(layout.Leaves());
    }

    [Fact]
    public void FindLeaf_FindsByPanelId()
    {
        var layout = new SplitLayout();
        var root = AddPanel(layout, (SplitLeaf)layout.Root, "a");
        var l2 = AddPanel(layout, layout.Split(root, SplitSide.Right), "b");
        Assert.Same(root, layout.FindLeaf("a"));
        Assert.Same(l2, layout.FindLeaf("b"));
        Assert.Null(layout.FindLeaf("ghost"));
    }

    [Fact]
    public void SamePanel_LivesInExactlyOneGroup()
    {
        var layout = new SplitLayout();
        var root = AddPanel(layout, (SplitLeaf)layout.Root, "a");
        var l2 = AddPanel(layout, layout.Split(root, SplitSide.Right), "b");

        layout.MovePanel(l2, "b", l2);   // 移到同组 = 无操作
        Assert.Equal(new[] { "b" }, l2.PanelIds);

        // 跨组移动会把面板从源组摘除（同一实例不会同时出现在两格）
        layout.MovePanel(root, "a", l2);
        Assert.DoesNotContain("a", root.PanelIds);
        Assert.Contains("a", l2.PanelIds);
    }

    // ==================== 组内重排 / 按位移动（标签左右拖拽排序的模型层） ====================

    [Fact]
    public void ReorderPanel_MovesWithinLeaf()
    {
        var layout = new SplitLayout();
        var leaf = AddPanel(layout, (SplitLeaf)layout.Root, "a");
        AddPanel(layout, leaf, "b");
        AddPanel(layout, leaf, "c");

        Assert.True(layout.ReorderPanel(leaf, "a", 2));   // a → 末尾（下标按移出后语义）
        Assert.Equal(new[] { "b", "c", "a" }, leaf.PanelIds);

        Assert.True(layout.ReorderPanel(leaf, "a", 0));   // 再回到开头
        Assert.Equal(new[] { "a", "b", "c" }, leaf.PanelIds);

        Assert.True(layout.ReorderPanel(leaf, "c", 1));   // 中间插入
        Assert.Equal(new[] { "a", "c", "b" }, leaf.PanelIds);
        Assert.Equal("c", leaf.ActivePanelId);            // 重排不动激活面板
    }

    [Fact]
    public void ReorderPanel_NoOpOrMissing_ReturnsFalse()
    {
        var layout = new SplitLayout();
        var leaf = AddPanel(layout, (SplitLeaf)layout.Root, "a");
        AddPanel(layout, leaf, "b");
        AddPanel(layout, leaf, "c");

        Assert.False(layout.ReorderPanel(leaf, "a", 0));      // 原位 = 无变化
        Assert.False(layout.ReorderPanel(leaf, "ghost", 0));  // 不在组里
        Assert.Equal(new[] { "a", "b", "c" }, leaf.PanelIds);

        Assert.True(layout.ReorderPanel(leaf, "a", 99));      // 越界钳制到末尾
        Assert.Equal(new[] { "b", "c", "a" }, leaf.PanelIds);
    }

    [Fact]
    public void MovePanel_WithIndex_InsertsAtPosition()
    {
        var layout = new SplitLayout();
        var root = AddPanel(layout, (SplitLeaf)layout.Root, "a");
        AddPanel(layout, root, "b");
        AddPanel(layout, root, "c");
        var target = AddPanel(layout, layout.Split(root, SplitSide.Right), "x");

        layout.MovePanel(root, "c", target, 0);   // 跨组按位：下标 0 = 目标组最前
        Assert.Equal(new[] { "c", "x" }, target.PanelIds);
        Assert.Equal(new[] { "a", "b" }, root.PanelIds);
        Assert.Equal("c", target.ActivePanelId);

        layout.MovePanel(target, "c", root, 1);   // 回移插中间
        Assert.Equal(new[] { "a", "c", "b" }, root.PanelIds);
    }

    [Fact]
    public void MovePanel_WithoutIndex_AppendsLikeBefore()
    {
        var layout = new SplitLayout();
        var root = AddPanel(layout, (SplitLeaf)layout.Root, "a");
        var target = AddPanel(layout, layout.Split(root, SplitSide.Right), "x");
        AddPanel(layout, target, "y");

        layout.MovePanel(root, "a", target);      // 缺省 = 追加末尾（旧行为不变）
        Assert.Equal(new[] { "x", "y", "a" }, target.PanelIds);
    }
}
