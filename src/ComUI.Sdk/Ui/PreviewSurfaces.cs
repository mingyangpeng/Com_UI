using System;
using Avalonia.Controls;

namespace ComUI.Sdk.Ui;

/// <summary>
/// 3D 点云预览控件（draw.3d 卡片内嵌用）：显示点云 + 基础拖拽旋转，
/// 数据经 SetCloud 推入（由 3D 插件提供实现并注册到 <see cref="PreviewSurfaces"/>）。
/// </summary>
public interface ICloudPreview : IDisposable
{
    Control Control { get; }

    /// <summary>推入/更新点云（同 Id 更新语义由实现方保证）。</summary>
    void SetCloud(CloudPayload payload);
}

/// <summary>跨插件预览控件注册表：3D 插件 Initialize 时注册工厂，draw 卡片按需创建。</summary>
public static class PreviewSurfaces
{
    /// <summary>创建 3D 点云预览控件（传入初始载荷）。</summary>
    public static Func<CloudPayload, ICloudPreview>? CloudPreview { get; set; }
}
