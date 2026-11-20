using Avalonia.Input;

namespace ComUI.Plugin.Cloud3D;

/// <summary>3D 工作台动作（快捷键分发目标）。</summary>
public enum CloudAction
{
    None,
    // 模式开关（互斥）
    MeasureToggle, PolyToggle, RegistToggle, LockPitchToggle,
    // 模式内（Enter/Back/Esc，模式开时优先于全局）
    ModeConfirm, ModeBack, ModeCancel,
    // 选取
    ExtractInside, ExtractOutside, BoxCrop, SelectAll, SelectInvert, SelectClear,
    // 导航/视角
    FocusEntity, ResetView, Preset1, Preset2, Preset3, Preset4, Preset5, Preset6, Preset7,
    ProjToggle, NudgeLeft, NudgeRight, NudgeUp, NudgeDown, PanLeft, PanRight, PanUp, PanDown,
    // 工具
    RandomSample, OutlierFilter, PlaneSplit, Merge, Transform, Normals, HeightMap,
    // 文件/算子/量测
    Import, Export, OperatorMenu, CopyMeasurements,
    PointSizeUp, PointSizeDown,
    Undo, Redo,
    Help,
}

/// <summary>
/// 快捷键解析（纯函数，可测试）：键位 → 动作。优先级链（高→低）：
/// 模式内 Enter/Back/Esc（任一模式开时优先于全局）→ 全局映射。
/// TextBox 聚焦由调用方守卫（不进入本函数）。
/// </summary>
public static class CloudHotkeys
{
    /// <summary>键位表（用于帮助浮层渲染）：动作 → 显示文本。</summary>
    public static readonly (CloudAction Action, string Keys, string Desc)[] Bindings =
    {
        (CloudAction.MeasureToggle, "M", "距离量测模式开关（连续拾取=折线累段）"),
        (CloudAction.PolyToggle, "P", "多边形选取模式开关（左键加点/右键闭合）"),
        (CloudAction.RegistToggle, "R", "点对配准模式开关（两云交替拾取对应点）"),
        (CloudAction.LockPitchToggle, "L", "锁定竖直轴旋转开关（俯视巡检）"),
        (CloudAction.ModeConfirm, "Enter", "模式内：结束当前折线 / 应用点对配准 / 闭合多边形"),
        (CloudAction.ModeBack, "Backspace", "模式内：撤销上一段 / 上一对"),
        (CloudAction.ModeCancel, "Esc", "取消挂起点/退出模式；无模式时清除选区与拾取"),
        (CloudAction.ExtractInside, "E", "提取框内（按当前选区生成新实体）"),
        (CloudAction.ExtractOutside, "X", "剔除框内（保留框外为新实体）"),
        (CloudAction.BoxCrop, "B", "盒裁剪（轴对齐包围盒）"),
        (CloudAction.SelectAll, "Ctrl+A", "全选当前实体点"),
        (CloudAction.SelectInvert, "Ctrl+I", "反选当前实体点"),
        (CloudAction.SelectClear, "Ctrl+D", "清空选区"),
        (CloudAction.FocusEntity, "F", "聚焦当前实体（保持朝向）"),
        (CloudAction.ResetView, "Shift+F", "重置全景视角"),
        (CloudAction.Preset1, "1", "视角预设：等轴"),
        (CloudAction.Preset2, "2", "视角预设：前"),
        (CloudAction.Preset3, "3", "视角预设：后"),
        (CloudAction.Preset4, "4", "视角预设：左"),
        (CloudAction.Preset5, "5", "视角预设：右"),
        (CloudAction.Preset6, "6", "视角预设：上"),
        (CloudAction.Preset7, "7", "视角预设：下"),
        (CloudAction.ProjToggle, "O", "透视 / 正交 投影切换"),
        (CloudAction.NudgeLeft, "←", "旋转步进（左）"),
        (CloudAction.NudgeRight, "→", "旋转步进（右）"),
        (CloudAction.NudgeUp, "↑", "旋转步进（上）"),
        (CloudAction.NudgeDown, "↓", "旋转步进（下）"),
        (CloudAction.PanLeft, "Shift+←", "平移步进（左）"),
        (CloudAction.PanRight, "Shift+→", "平移步进（右）"),
        (CloudAction.PanUp, "Shift+↑", "平移步进（上）"),
        (CloudAction.PanDown, "Shift+↓", "平移步进（下）"),
        (CloudAction.PointSizeUp, "+", "点大小 +0.5"),
        (CloudAction.PointSizeDown, "-", "点大小 −0.5"),
        (CloudAction.RandomSample, "S", "随机下采样"),
        (CloudAction.OutlierFilter, "D", "离群点滤波"),
        (CloudAction.PlaneSplit, "G", "RANSAC 平面分割"),
        (CloudAction.Merge, "J", "合并实体"),
        (CloudAction.Transform, "T", "变换工具"),
        (CloudAction.Normals, "N", "法线估计 + 光照"),
        (CloudAction.HeightMap, "H", "高度图 → 2D 工作台"),
        (CloudAction.Import, "Ctrl+O", "导入点云"),
        (CloudAction.Export, "Ctrl+E", "导出点云"),
        (CloudAction.OperatorMenu, "Q", "算子工具菜单"),
        (CloudAction.CopyMeasurements, "Ctrl+Shift+C", "复制量测清单"),
        (CloudAction.Undo, "Ctrl+Z", "撤销（删除/变换/合并等）"),
        (CloudAction.Redo, "Ctrl+Y", "重做（撤销的反向）"),
        (CloudAction.Help, "?", "显示/关闭本快捷键帮助"),
    };

    /// <summary>解析动作。模式参数=当前三模式开关状态；TextBox 聚焦由调用方先行守卫。</summary>
    public static CloudAction Resolve(Key key, KeyModifiers mods, bool measureMode, bool polyMode, bool registMode)
    {
        bool anyMode = measureMode || polyMode || registMode;

        // 1) Esc：任何状态都返回 ModeCancel（模式开=退模式；无模式=清选区与拾取，由调用方分支处理）
        if (key == Key.Escape) return CloudAction.ModeCancel;

        // 2) 模式内语义优先（Enter/Back 在模式开时抢占全局）
        if (anyMode)
        {
            if (key == Key.Enter) return CloudAction.ModeConfirm;
            if (key == Key.Back) return CloudAction.ModeBack;
        }

        // 2) 组合键
        bool ctrl = mods.HasFlag(KeyModifiers.Control);
        bool shift = mods.HasFlag(KeyModifiers.Shift);
        if (ctrl && !shift)
        {
            switch (key)
            {
                case Key.A: return CloudAction.SelectAll;
                case Key.I: return CloudAction.SelectInvert;
                case Key.D: return CloudAction.SelectClear;
                case Key.O: return CloudAction.Import;
                case Key.E: return CloudAction.Export;
                case Key.Z: return CloudAction.Undo;
                case Key.Y: return CloudAction.Redo;
            }
        }
        if (ctrl && shift)
        {
            if (key == Key.C) return CloudAction.CopyMeasurements;
        }
        if (shift && !ctrl)
        {
            switch (key)
            {
                case Key.F: return CloudAction.ResetView;
                case Key.Left: return CloudAction.PanLeft;
                case Key.Right: return CloudAction.PanRight;
                case Key.Up: return CloudAction.PanUp;
                case Key.Down: return CloudAction.PanDown;
                case Key.OemQuestion: return CloudAction.Help;
                case Key.OemPlus: return CloudAction.PointSizeUp;   // US 布局 "+"=Shift+OemPlus
            }
        }

        // 3) 单键（无修饰）
        if (!ctrl && !shift && !mods.HasFlag(KeyModifiers.Alt))
        {
            switch (key)
            {
                case Key.M: return CloudAction.MeasureToggle;
                case Key.P: return CloudAction.PolyToggle;
                case Key.R: return CloudAction.RegistToggle;
                case Key.L: return CloudAction.LockPitchToggle;
                case Key.E: return CloudAction.ExtractInside;
                case Key.X: return CloudAction.ExtractOutside;
                case Key.B: return CloudAction.BoxCrop;
                case Key.F: return CloudAction.FocusEntity;
                case Key.O: return CloudAction.ProjToggle;
                case Key.S: return CloudAction.RandomSample;
                case Key.D: return CloudAction.OutlierFilter;
                case Key.G: return CloudAction.PlaneSplit;
                case Key.J: return CloudAction.Merge;
                case Key.T: return CloudAction.Transform;
                case Key.N: return CloudAction.Normals;
                case Key.H: return CloudAction.HeightMap;
                case Key.Q: return CloudAction.OperatorMenu;
                case Key.D1: return CloudAction.Preset1;
                case Key.D2: return CloudAction.Preset2;
                case Key.D3: return CloudAction.Preset3;
                case Key.D4: return CloudAction.Preset4;
                case Key.D5: return CloudAction.Preset5;
                case Key.D6: return CloudAction.Preset6;
                case Key.D7: return CloudAction.Preset7;
                case Key.Left: return CloudAction.NudgeLeft;
                case Key.Right: return CloudAction.NudgeRight;
                case Key.Up: return CloudAction.NudgeUp;
                case Key.Down: return CloudAction.NudgeDown;
                case Key.OemPlus: case Key.Add: return CloudAction.PointSizeUp;   // US 布局 "+"=Shift+OemPlus；"="无 shift 同加语义
                case Key.OemMinus: case Key.Subtract: return CloudAction.PointSizeDown;
                case Key.OemQuestion: return CloudAction.Help;   // 部分键盘 / 无 Shift
            }
        }
        return CloudAction.None;
    }
}
