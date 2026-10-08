using System;
using System.Collections.Generic;
using System.Linq;

namespace ComUI.Core.Split;

/// <summary>拆分方位。</summary>
public enum SplitSide { Left, Right, Top, Bottom }

/// <summary>分屏树节点基类（Leaf=标签组，Container=横/纵分隔）。</summary>
public abstract class SplitNode
{
    public SplitContainer? Parent { get; internal set; }
}

/// <summary>标签组：一组面板 Id（顺序=标签顺序）+ 当前激活面板。</summary>
public sealed class SplitLeaf : SplitNode
{
    public List<string> PanelIds { get; } = new();
    public string? ActivePanelId { get; set; }
}

/// <summary>分隔容器：子节点 + 每格宽度比例（star 值，与 Children 一一对应）。</summary>
public sealed class SplitContainer : SplitNode
{
    /// <summary>true = 上下排列；false = 左右排列。</summary>
    public bool Vertical { get; set; }
    public List<SplitNode> Children { get; } = new();
    public List<double> Stars { get; } = new();
}

/// <summary>
/// 分屏树（持有根引用，结构变更自动更新根；纯模型无 UI 依赖，可单测）。
/// 不变式：Container 至少 2 个子节点（少于则退化）；空 Leaf 被回收（根 Leaf 除外）；
/// 同一面板同一时刻只在一个 Leaf（同面板两格显示默认禁止，AllowMultipleInstances 由 UI 层建第二实例规避）。
/// </summary>
public sealed class SplitLayout
{
    public SplitNode Root { get; private set; } = new SplitLeaf();

    /// <summary>在 leaf 处按方位拆分出一个新空 Leaf 并返回（面板由调用方移入）。</summary>
    public SplitLeaf Split(SplitLeaf leaf, SplitSide side)
    {
        var newLeaf = new SplitLeaf();
        var vertical = side is SplitSide.Top or SplitSide.Bottom;
        var before = side is SplitSide.Left or SplitSide.Top;

        if (leaf.Parent is SplitContainer parent && parent.Vertical == vertical)
        {
            var idx = parent.Children.IndexOf(leaf);
            parent.Children.Insert(before ? idx : idx + 1, newLeaf);
            parent.Stars.Insert(before ? idx : idx + 1, 1);
            newLeaf.Parent = parent;
        }
        else
        {
            var container = new SplitContainer { Vertical = vertical };
            ReplaceInParent(leaf, container);
            if (ReferenceEquals(Root, leaf)) Root = container;
            container.Children.Add(leaf);
            container.Stars.Add(1);
            leaf.Parent = container;
            container.Children.Add(newLeaf);
            container.Stars.Add(1);
            newLeaf.Parent = container;
            if (before)
            {
                container.Children.Reverse();
                container.Stars.Reverse();
            }
        }
        return newLeaf;
    }

    /// <summary>把面板从 from 组移动到 to 组（源组空则回收；toIndex=插入位下标，缺省追加到末尾）。</summary>
    public void MovePanel(SplitLeaf from, string panelId, SplitLeaf to, int? toIndex = null)
    {
        if (ReferenceEquals(from, to) || !from.PanelIds.Remove(panelId)) return;
        if (!to.PanelIds.Contains(panelId))
        {
            var idx = Math.Clamp(toIndex ?? to.PanelIds.Count, 0, to.PanelIds.Count);
            to.PanelIds.Insert(idx, panelId);
        }
        if (from.ActivePanelId == panelId) from.ActivePanelId = from.PanelIds.LastOrDefault();
        to.ActivePanelId = panelId;
        Recycle(from);
    }

    /// <summary>组内重排：把 panelId 移到 newIndex（下标按"先移出该面板"后的列表语义，与 UI 摘/插一致）。
    /// 位置没变或面板不在组里返回 false。</summary>
    public bool ReorderPanel(SplitLeaf leaf, string panelId, int newIndex)
    {
        var old = leaf.PanelIds.IndexOf(panelId);
        if (old < 0 || leaf.PanelIds.Count < 2) return false;
        var idx = Math.Clamp(newIndex, 0, leaf.PanelIds.Count - 1);   // 移出后最多 Count-1 项
        if (idx == old) return false;
        leaf.PanelIds.RemoveAt(old);
        leaf.PanelIds.Insert(idx, panelId);
        return true;
    }

    /// <summary>从 leaf 移除面板；空 Leaf 回收、单子 Container 退化（根 Leaf 除外）。</summary>
    public void RemovePanel(SplitLeaf leaf, string panelId)
    {
        if (!leaf.PanelIds.Remove(panelId)) return;
        if (leaf.ActivePanelId == panelId) leaf.ActivePanelId = leaf.PanelIds.LastOrDefault();
        Recycle(leaf);
    }

    /// <summary>找面板所在的 Leaf；找不到返回 null。</summary>
    public SplitLeaf? FindLeaf(string panelId) =>
        Leaves().FirstOrDefault(l => l.PanelIds.Contains(panelId));

    /// <summary>遍历全部 Leaf（深度优先，左→右 / 上→下）。</summary>
    public IEnumerable<SplitLeaf> Leaves() => Leaves(Root);

    private static IEnumerable<SplitLeaf> Leaves(SplitNode node)
    {
        switch (node)
        {
            case SplitLeaf leaf:
                yield return leaf;
                break;
            case SplitContainer c:
                foreach (var child in c.Children)
                    foreach (var l in Leaves(child))
                        yield return l;
                break;
        }
    }

    /// <summary>回收空 Leaf：父容器删除该子（比例并入相邻兄弟）；父容器只剩单子时用该子替换之；
    /// 根容器退化时根引用更新；根 Leaf（无父）不回收（允许为空）。</summary>
    private void Recycle(SplitLeaf leaf)
    {
        if (leaf.PanelIds.Count > 0 || leaf.Parent is null) return;
        var parent = leaf.Parent;
        var idx = parent.Children.IndexOf(leaf);
        parent.Children.RemoveAt(idx);
        var star = parent.Stars[idx];
        parent.Stars.RemoveAt(idx);
        if (parent.Stars.Count > 0)
        {
            var mergeIdx = Math.Min(idx, parent.Stars.Count - 1);
            parent.Stars[mergeIdx] += star;
        }
        leaf.Parent = null;

        if (parent.Children.Count == 1)
        {
            var only = parent.Children[0];
            ReplaceInParent(parent, only);
            only.Parent = parent.Parent;
            if (ReferenceEquals(Root, parent)) Root = only;
            parent.Children.Clear();
            parent.Stars.Clear();
        }
    }

    private static void ReplaceInParent(SplitNode old, SplitNode new_)
    {
        if (old.Parent is SplitContainer p)
        {
            var idx = p.Children.IndexOf(old);
            p.Children[idx] = new_;
            new_.Parent = p;   // 必须回填：漏了会让后续回收链静默失效（单测抓出）
        }
    }

    // ==================== 会话持久化（面板 Id 原样存取；doc:N → 恢复键的翻译由 UI 层做） ====================

    /// <summary>序列化为 JSON（含比例与每格激活面板）。</summary>
    public string ToJson()
    {
        var root = Write(Root);
        return System.Text.Json.JsonSerializer.Serialize(root, SplitJsonOpts);
    }

    /// <summary>从 JSON 恢复整棵树（根引用重置；结构非法时抛异常，调用方决定回退）。</summary>
    public void LoadFromJson(string json)
    {
        var root = System.Text.Json.JsonSerializer.Deserialize<SplitJson>(json, SplitJsonOpts)
                   ?? throw new Exception("分屏数据为空");
        Root = Read(root, null);
    }

    private static readonly System.Text.Json.JsonSerializerOptions SplitJsonOpts = new()
    {
        PropertyNameCaseInsensitive = true,
        WriteIndented = true,
    };

    private static SplitJson Write(SplitNode node) => node switch
    {
        SplitLeaf l => new SplitJson { Type = "leaf", Panels = l.PanelIds.ToList(), Active = l.ActivePanelId },
        SplitContainer c => new SplitJson
        {
            Type = "container",
            Vertical = c.Vertical,
            Stars = c.Stars.ToList(),
            Children = c.Children.Select(Write).ToList(),
        },
        _ => throw new Exception("未知分屏节点类型"),
    };

    private static SplitNode Read(SplitJson json, SplitContainer? parent)
    {
        if (json.Type == "leaf")
        {
            var leaf = new SplitLeaf { ActivePanelId = json.Active };
            leaf.Parent = parent;
            foreach (var id in json.Panels) leaf.PanelIds.Add(id);
            return leaf;
        }
        var container = new SplitContainer { Vertical = json.Vertical, Parent = parent };
        foreach (var childJson in json.Children)
        {
            var child = Read(childJson, container);
            container.Children.Add(child);
            container.Stars.Add(1);   // 比例缺省均分；真实比例在 Stars 字段
        }
        if (json.Stars is { Count: > 0 } && json.Stars.Count == container.Children.Count)
            for (int i = 0; i < json.Stars.Count; i++) container.Stars[i] = json.Stars[i];
        if (container.Children.Count < 2) throw new Exception("分屏容器子节点不足 2 个");
        return container;
    }

    private sealed class SplitJson
    {
        public string Type { get; set; } = "";
        public bool Vertical { get; set; }
        public List<double>? Stars { get; set; }
        public List<SplitJson>? Children { get; set; }
        public List<string>? Panels { get; set; }
        public string? Active { get; set; }
    }
}
