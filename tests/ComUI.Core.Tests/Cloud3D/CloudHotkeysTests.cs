using Avalonia.Input;
using ComUI.Plugin.Cloud3D;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// 快捷键解析（CloudHotkeys.Resolve）：模式内语义优先于全局、组合键/单键映射、
/// 无修饰约束、键位表无重复键位（同一 (键,修饰) 组合不得映射两个动作）。
/// </summary>
public class CloudHotkeysTests
{
    private const KeyModifiers None = KeyModifiers.None;
    private const KeyModifiers Ctrl = KeyModifiers.Control;
    private const KeyModifiers Shift = KeyModifiers.Shift;

    [Fact]
    public void Resolve_ModeConfirmBeatsGlobal_WhenAnyModeOn()
    {
        // Enter 在模式开时 = ModeConfirm（抢占全局 Enter 语义）
        Assert.Equal(CloudAction.ModeConfirm, CloudHotkeys.Resolve(Key.Enter, None, measureMode: true, polyMode: false, registMode: false));
        Assert.Equal(CloudAction.ModeConfirm, CloudHotkeys.Resolve(Key.Enter, None, measureMode: false, polyMode: true, registMode: false));
        Assert.Equal(CloudAction.ModeConfirm, CloudHotkeys.Resolve(Key.Enter, None, measureMode: false, polyMode: false, registMode: true));
        // 无模式时 Enter = None
        Assert.Equal(CloudAction.None, CloudHotkeys.Resolve(Key.Enter, None, false, false, false));
        // Back/Esc 同理
        Assert.Equal(CloudAction.ModeBack, CloudHotkeys.Resolve(Key.Back, None, true, false, false));
        Assert.Equal(CloudAction.ModeCancel, CloudHotkeys.Resolve(Key.Escape, None, false, false, true));
    }

    [Fact]
    public void Resolve_ModeToggles()
    {
        Assert.Equal(CloudAction.MeasureToggle, CloudHotkeys.Resolve(Key.M, None, false, false, false));
        Assert.Equal(CloudAction.PolyToggle, CloudHotkeys.Resolve(Key.P, None, false, false, false));
        Assert.Equal(CloudAction.RegistToggle, CloudHotkeys.Resolve(Key.R, None, false, false, false));
        Assert.Equal(CloudAction.LockPitchToggle, CloudHotkeys.Resolve(Key.L, None, false, false, false));
    }

    [Fact]
    public void Resolve_SelectionAndNav()
    {
        Assert.Equal(CloudAction.SelectAll, CloudHotkeys.Resolve(Key.A, Ctrl, false, false, false));
        Assert.Equal(CloudAction.SelectInvert, CloudHotkeys.Resolve(Key.I, Ctrl, false, false, false));
        Assert.Equal(CloudAction.SelectClear, CloudHotkeys.Resolve(Key.D, Ctrl, false, false, false));
        Assert.Equal(CloudAction.FocusEntity, CloudHotkeys.Resolve(Key.F, None, false, false, false));
        Assert.Equal(CloudAction.ResetView, CloudHotkeys.Resolve(Key.F, Shift, false, false, false));
        for (int i = 0; i < 7; i++)
        {
            var key = (Key)((int)Key.D1 + i);
            Assert.Equal(CloudAction.Preset1 + i, CloudHotkeys.Resolve(key, None, false, false, false));
        }
        Assert.Equal(CloudAction.ProjToggle, CloudHotkeys.Resolve(Key.O, None, false, false, false));
    }

    [Fact]
    public void Resolve_NudgeAndPan_Directional()
    {
        Assert.Equal(CloudAction.NudgeLeft, CloudHotkeys.Resolve(Key.Left, None, false, false, false));
        Assert.Equal(CloudAction.NudgeRight, CloudHotkeys.Resolve(Key.Right, None, false, false, false));
        Assert.Equal(CloudAction.NudgeUp, CloudHotkeys.Resolve(Key.Up, None, false, false, false));
        Assert.Equal(CloudAction.NudgeDown, CloudHotkeys.Resolve(Key.Down, None, false, false, false));
        Assert.Equal(CloudAction.PanLeft, CloudHotkeys.Resolve(Key.Left, Shift, false, false, false));
        Assert.Equal(CloudAction.PanUp, CloudHotkeys.Resolve(Key.Up, Shift, false, false, false));
    }

    [Fact]
    public void Resolve_ToolsAndFiles()
    {
        Assert.Equal(CloudAction.ExtractInside, CloudHotkeys.Resolve(Key.E, None, false, false, false));
        Assert.Equal(CloudAction.ExtractOutside, CloudHotkeys.Resolve(Key.X, None, false, false, false));
        Assert.Equal(CloudAction.BoxCrop, CloudHotkeys.Resolve(Key.B, None, false, false, false));
        Assert.Equal(CloudAction.RandomSample, CloudHotkeys.Resolve(Key.S, None, false, false, false));
        Assert.Equal(CloudAction.OutlierFilter, CloudHotkeys.Resolve(Key.D, None, false, false, false));
        Assert.Equal(CloudAction.PlaneSplit, CloudHotkeys.Resolve(Key.G, None, false, false, false));
        Assert.Equal(CloudAction.Merge, CloudHotkeys.Resolve(Key.J, None, false, false, false));
        Assert.Equal(CloudAction.Transform, CloudHotkeys.Resolve(Key.T, None, false, false, false));
        Assert.Equal(CloudAction.Normals, CloudHotkeys.Resolve(Key.N, None, false, false, false));
        Assert.Equal(CloudAction.HeightMap, CloudHotkeys.Resolve(Key.H, None, false, false, false));
        Assert.Equal(CloudAction.Import, CloudHotkeys.Resolve(Key.O, Ctrl, false, false, false));
        Assert.Equal(CloudAction.Export, CloudHotkeys.Resolve(Key.E, Ctrl, false, false, false));
        Assert.Equal(CloudAction.OperatorMenu, CloudHotkeys.Resolve(Key.Q, None, false, false, false));
        Assert.Equal(CloudAction.CopyMeasurements, CloudHotkeys.Resolve(Key.C, Ctrl | Shift, false, false, false));
        Assert.Equal(CloudAction.PointSizeUp, CloudHotkeys.Resolve(Key.OemPlus, None, false, false, false));
        Assert.Equal(CloudAction.PointSizeDown, CloudHotkeys.Resolve(Key.OemMinus, None, false, false, false));
        Assert.Equal(CloudAction.PointSizeUp, CloudHotkeys.Resolve(Key.Add, None, false, false, false));
        Assert.Equal(CloudAction.Help, CloudHotkeys.Resolve(Key.OemQuestion, Shift, false, false, false));
    }

    [Fact]
    public void Resolve_Escape_AlwaysModeCancel()
    {
        // Esc 全态语义：模式开=退模式；无模式=清选区（由调用方分支处理）——统一返回 ModeCancel
        Assert.Equal(CloudAction.ModeCancel, CloudHotkeys.Resolve(Key.Escape, None, true, false, false));
        Assert.Equal(CloudAction.ModeCancel, CloudHotkeys.Resolve(Key.Escape, None, false, true, false));
        Assert.Equal(CloudAction.ModeCancel, CloudHotkeys.Resolve(Key.Escape, None, false, false, false));
    }

    [Fact]
    public void Bindings_NoDuplicateKeyBinding()
    {
        // 同一 (Keys 文本) 不得出现两次（帮助浮层不产生歧义）
        var seen = new System.Collections.Generic.HashSet<string>();
        foreach (var (_, keys, _) in CloudHotkeys.Bindings)
            Assert.True(seen.Add(keys), $"键位重复: {keys}");
    }

    [Fact]
    public void Resolve_DigitsDoNotMap_WithCtrl()
    {
        // Ctrl+数字 不映射预设（留给系统/用户扩展）
        Assert.Equal(CloudAction.None, CloudHotkeys.Resolve(Key.D1, Ctrl, false, false, false));
    }
}
