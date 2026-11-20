using OpenTK.Mathematics;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 3D 工作台 Undo/Redo 纯函数层（可测试）：实体状态快照 + 栈管理（单栈+指针）。
/// 拦截点（View 层）把操作前后快照压栈；Apply 侧（View）负责按快照恢复/删除实体。
/// 快照为浅拷贝（Pts/Col/Normals 引用共享——载荷约定不可变），Xform 单独克隆。
/// </summary>
public sealed class CloudUndo
{
    /// <summary>实体全量状态快照（浅拷贝：点/色/法线数组引用共享）。</summary>
    public sealed class EntSnapshot
    {
        public required string Id;
        public required float[] Pts;
        public byte[]? Col;
        public float[]? Normals;
        public required double[] Xform;
        public required Vector3 BoundsMin;
        public required Vector3 BoundsMax;
        public int Count;
        public int Loaded;
        public int SelCount;
        public byte[]? Sel;
        public int ColorMode;
        public float PointSize;
        public Vector3 SingleColor;
        public bool Locked;
        public string? Source;
    }

    /// <summary>单条 undo 记录：undo 应用 UndoSnapshots（含待删实体清单），redo 应用 RedoSnapshots。</summary>
    public sealed class UndoEntry
    {
        public required string Description;
        public List<EntSnapshot> UndoSnapshots = new();   // undo 时恢复这些实体（存在→覆盖，不存在→重建）
        public List<string> UndoDeletes = new();          // undo 时删除这些实体（新建类操作的反向）
        public List<EntSnapshot> RedoSnapshots = new();   // redo 时恢复
        public List<string> RedoDeletes = new();          // redo 时删除
    }

    private readonly List<UndoEntry> _stack = new();
    private int _pos;   // 指向下一个可 undo 的记录（_pos==Count 时只能 redo）

    public int Count => _stack.Count;
    public int Pos => _pos;
    public bool CanUndo => _pos > 0;
    public bool CanRedo => _pos < _stack.Count;

    /// <summary>压栈（丢弃 redo 分支——新操作后旧 redo 不可达）。超过 max 时淘汰最旧。</summary>
    public void Push(UndoEntry entry, int max = 50)
    {
        if (_pos < _stack.Count) _stack.RemoveRange(_pos, _stack.Count - _pos);
        _stack.Add(entry);
        if (_stack.Count > max) { _stack.RemoveAt(0); _pos = Math.Max(0, _pos - 1); }
        _pos = _stack.Count;
    }

    /// <summary>撤销：返回要应用的 entry（调用方执行后调 CommitApplied 或 Advance）。</summary>
    public UndoEntry? PopUndo()
    {
        if (_pos <= 0) return null;
        return _stack[--_pos];
    }

    /// <summary>重做：返回下一个 entry（无则 null）。</summary>
    public UndoEntry? PopRedo()
    {
        if (_pos >= _stack.Count) return null;
        return _stack[_pos++];
    }

    public void Clear()
    {
        _stack.Clear();
        _pos = 0;
    }
}

/// <summary>实体状态快照/恢复纯函数（CloudData 由 View 层映射，此处操作纯数据结构）。</summary>
public static class CloudUndoOps
{
    /// <summary>undo 应用侧：快照存在→覆盖恢复；不存在→按删除处理（返回待删 id）。</summary>
    public static (List<string> Restore, List<string> Delete) SplitApply(
        IReadOnlyList<(string Id, bool Present)> states)
    {
        var restore = new List<string>();
        var delete = new List<string>();
        foreach (var (id, present) in states)
            (present ? restore : delete).Add(id);
        return (restore, delete);
    }
}
