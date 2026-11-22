using ComUI.Plugin.Cloud3D;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// Undo/Redo 栈管理（CloudUndo 纯函数层）：Push/PopUndo/PopRedo 指针语义、
/// 新操作丢弃 redo 分支、max 淘汰最旧、CanUndo/CanRedo 边界。
/// </summary>
public class CloudUndoTests
{
    private static CloudUndo.UndoEntry E(string desc) => new() { Description = desc };

    [Fact]
    public void PushPop_UndoRedo_PointerSemantics()
    {
        var u = new CloudUndo();
        u.Push(E("a")); u.Push(E("b")); u.Push(E("c"));
        Assert.Equal(3, u.Count);
        Assert.True(u.CanUndo); Assert.False(u.CanRedo);

        var undo1 = u.PopUndo();
        Assert.Equal("c", undo1!.Description);
        Assert.True(u.CanUndo); Assert.True(u.CanRedo);

        var undo2 = u.PopUndo();
        Assert.Equal("b", undo2!.Description);
        Assert.True(u.CanUndo); Assert.True(u.CanRedo);

        var redo1 = u.PopRedo();
        Assert.Equal("b", redo1!.Description);   // LIFO：redo 按撤销逆序
        Assert.Equal(2, u.Pos);
    }

    [Fact]
    public void Push_AfterUndo_DiscardsRedoBranch()
    {
        var u = new CloudUndo();
        u.Push(E("a")); u.Push(E("b"));
        u.PopUndo();                             // 指针在 1，redo=[b]
        u.Push(E("c"));                          // 新操作 → redo 分支丢弃
        Assert.Equal(2, u.Count);
        Assert.False(u.CanRedo);
        Assert.Equal("c", u.PopUndo()!.Description);
        Assert.Equal("a", u.PopUndo()!.Description);
        Assert.Null(u.PopUndo());                // 栈底
    }

    [Fact]
    public void Push_MaxEvictsOldest()
    {
        var u = new CloudUndo();
        for (int i = 0; i < 55; i++) u.Push(E("e" + i));
        Assert.Equal(50, u.Count);
        // 最旧被淘汰：undo 到底描述是 e5（前 5 个被淘汰）
        CloudUndo.UndoEntry? last = null;
        while (u.CanUndo) last = u.PopUndo();
        Assert.Equal("e5", last!.Description);
    }

    [Fact]
    public void Clear_Resets()
    {
        var u = new CloudUndo();
        u.Push(E("a")); u.Push(E("b"));
        u.PopUndo();
        u.Clear();
        Assert.False(u.CanUndo); Assert.False(u.CanRedo);
        Assert.Null(u.PopUndo());
    }
}
