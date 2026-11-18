using System;
using Avalonia.Media;
using ComUI.Sdk;

namespace ComUI.Plugin.NodeGraph;

/// <summary>算子在库中的视觉元数据：小图标 + 强调色 + 二级子模块名。</summary>
public sealed record OpVisual(string Icon, Color Accent, string Sub);

/// <summary>
/// 算子图标 / 二级子模块推导：按算子 Id 的语义关键词匹配（有序规则，首个命中生效），
/// 未命中回退到 Category 配色。规则与算子注册顺序无关，新增算子自动归类。
/// 图标用 emoji（与宿主活动栏 🧰 同风格），在圆角色块里居中显示。
/// </summary>
public static class OperatorGlyphs
{
    // macOS 系统色（柔和、深色模式适配）
    private static readonly Color Blue = C(0x0A84FF);
    private static readonly Color Teal = C(0x40C8E0);
    private static readonly Color Green = C(0x30D158);
    private static readonly Color Orange = C(0xFF9F0A);
    private static readonly Color Purple = C(0xBF5AF2);
    private static readonly Color Pink = C(0xFF375F);
    private static readonly Color Yellow = C(0xFFD60A);
    private static readonly Color Indigo = C(0x5E5CE6);
    private static readonly Color Gray = C(0x8E8E93);

    //              Id 关键词           图标     色      二级子模块
    private static readonly (string Match, string Icon, Color Accent, string Sub)[] Rules =
    {
        // —— 流程控制 ——
        ("flow.for",              "🔁", Orange, "循环"),
        ("flow.while",            "🔁", Orange, "循环"),
        ("flow.switch",           "🔀", Purple, "分支"),

        // —— IO / 数据源 / 发布 / 预览 ——
        ("source.image",          "🖼️", Blue,   "数据源"),
        ("source.cloud",          "☁️", Teal,   "数据源"),
        ("source.cube",           "🧊", Teal,   "数据源"),
        ("source.heightmap",      "⛰️", Green,  "数据源"),
        ("read_sick",             "📡", Teal,   "点云读取"),
        ("image2d.read",          "📂", Blue,   "图像读取"),
        ("pub.remove",            "➖", Orange, "发布输出"),
        ("pub.",                  "📤", Orange, "发布输出"),
        ("draw.",                 "👁️", Indigo, "结果预览"),

        // —— 内置 img.* 算子（旧版漏配，曾全部回退 ⚙️；须在通用规则前） ——
        ("img.gray",              "⚫", Gray,   "色彩调整"),
        ("img.invert",            "🌗", Purple, "色彩调整"),
        ("img.bright",            "☀️", Yellow, "色彩调整"),
        ("img.blur",              "🌫️", Blue,   "滤波增强"),

        // —— 2D：匹配对位（掩膜规则须在 match_template 前） ——
        ("match_template_masked", "🎭", Pink,   "匹配对位"),
        ("match_template",        "🎯", Pink,   "匹配对位"),
        ("find_matches",          "🎯", Pink,   "匹配对位"),

        // —— 2D：几何变换 ——
        ("image2d.crop",          "✂️", Teal,   "几何变换"),
        ("image2d.resize",        "⤢",  Teal,   "几何变换"),
        ("image2d.rotate",        "🔄", Teal,   "几何变换"),

        // —— 2D：滤波与边缘 ——
        ("image2d.blur",          "🌫️", Blue,   "滤波增强"),
        ("image2d.sobel",         "〰️", Purple, "边缘梯度"),
        ("image2d.canny",         "✏️", Purple, "边缘梯度"),
        ("image2d.morphology",    "🧩", Indigo, "形态学"),

        // —— 2D：色彩与阈值 ——
        ("image2d.to_gray",       "⚫", Gray,   "色彩调整"),
        ("image2d.binarize",      "🎚️", Green,  "阈值分割"),
        ("image2d.threshold",     "🎚️", Green,  "阈值分割"),
        ("image2d.gamma",         "☀️", Yellow, "色彩调整"),
        ("image2d.enhance",       "✨", Yellow, "色彩调整"),
        ("image2d.absdiff",       "🆚", Orange, "差分对比"),
        ("image2d.hsv_threshold", "🎚️", Green,  "阈值分割"),
        ("image2d.threshold",     "🎚️", Green,  "阈值分割"),

        // —— 2D：特征测量 ——
        ("connected_components",  "🔗", Teal,   "连通域"),
        ("analyze_contours",      "◯",  Yellow, "轮廓分析"),
        ("fit_circles",           "⭕", Pink,   "几何拟合"),
        ("fit_ellipses",          "⬭",  Pink,   "几何拟合"),
        ("hough_circles",         "⭕", Orange, "霍夫检测"),
        ("hough_lines",           "📏", Orange, "霍夫检测"),
        ("region_stats",          "📊", Blue,   "区域统计"),

        // —— 3D 点云 ——
        ("point3d.voxel",         "🧊", Teal,   "点云处理"),
        ("circle_fit",            "⭕", Pink,   "几何量测"),
        ("dome_fit",              "⛰️", Pink,   "几何量测"),
        ("point3d.profile",       "📈", Green,  "几何量测"),
        ("region_plane",          "📐", Teal,   "几何量测"),
        ("bump_report",           "📋", Orange, "几何量测"),
        ("cloud_diff",            "🆚", Orange, "点云比对"),
        ("manual_regist",         "🎯", Purple, "点云比对"),
        ("bump_ng",               "⚠️", Green,  "业务判定"),
        ("region.ratio",          "📐", Green,  "业务判定"),

        // —— 子流程 ——
        ("sub.",                  "📦", Indigo, "子流程"),
    };

    /// <summary>按算子 Id 推导视觉元数据；未命中按 Category 回退。</summary>
    public static OpVisual For(NodeDef def)
    {
        var id = def.Id.ToLowerInvariant();
        foreach (var r in Rules)
            if (id.Contains(r.Match, StringComparison.OrdinalIgnoreCase))
                return new OpVisual(r.Icon, r.Accent, r.Sub);

        return def.Category switch
        {
            "源" => new OpVisual("🎲", Blue, "数据源"),
            "输出" => new OpVisual("📤", Orange, "输出"),
            _ => new OpVisual("⚙️", Gray, "处理"),
        };
    }

    /// <summary>一级分组头的小图标与强调色。</summary>
    public static (string Icon, Color Accent) ForGroup(string group) => group switch
    {
        "IO" => ("📥", Blue),
        "2D 图像" => ("🖼️", Blue),
        "3D 点云" => ("🧊", Teal),
        "输出预览" => ("👁️", Indigo),
        "联动演示" => ("🔗", Green),
        "流程控制" => ("🔀", Orange),
        "业务算子" => ("💼", Pink),
        "子流程" => ("📦", Indigo),
        "算法" => ("📁", Purple),
        _ => ("•", Gray),
    };

    private static Color C(uint rgb) => Color.FromRgb((byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);
}
