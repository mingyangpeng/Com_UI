using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using ComUI.Sdk;
using ComUI.Sdk.Ui;
using OpenTK.Mathematics;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 3D 点云预览控件（draw.3d 卡片内嵌用）：轻量包装 Cloud3DView（embed 模式——
/// 不订阅总线，数据由上游经 SetCloud 直喂；隐藏信息栏），保留轨道相机拖拽旋转。
/// </summary>
internal sealed class MiniCloudPreview : ICloudPreview
{
    private readonly Cloud3DView _view;
    private int _count;

    public MiniCloudPreview(CloudPayload initial)
    {
        _view = new Cloud3DView(new BusNull(), embed: true);
        _view.Width = double.NaN;
        _view.Height = double.NaN;
        SetCloud(initial);
    }

    public Control Control => _view;

    public void SetCloud(CloudPayload payload)
    {
        _count = payload.Count;
        var min = new Vector3(
            payload.Points.Length > 0 ? payload.Points[0] : 0,
            payload.Points.Length > 2 ? payload.Points[1] : 0,
            payload.Points.Length > 5 ? payload.Points[2] : 0);
        var max = min;
        // 采样估界（预览卡不需要精确值——精确边界见工作台）
        var step = Math.Max(1, payload.Count / 4096);
        for (int i = 0; i < payload.Count; i += step)
        {
            float x = payload.Points[i * 3], y = payload.Points[i * 3 + 1], z = payload.Points[i * 3 + 2];
            if (x < min.X) min.X = x; if (y < min.Y) min.Y = y; if (z < min.Z) min.Z = z;
            if (x > max.X) max.X = x; if (y > max.Y) max.Y = y; if (z > max.Z) max.Z = z;
        }
        _view.SetCloud(payload.Count, payload.Points, payload.ColorsRgb,
            payload.Source is null ? "预览" : payload.Source + "（预览）", payload.Id ?? "预览");
    }

    public void Dispose() => _view.Dispose();

    /// <summary>空总线（内嵌模式数据直喂，不订总线）。</summary>
    private sealed class BusNull : IBus
    {
        public void Publish<T>(string topic, T payload) { }
        public IDisposable Subscribe<T>(string topic, Action<T> handler, bool uiThread = false) => new NullSub();
        public bool TryGetLatest<T>(string topic, out T value) { value = default!; return false; }
        public System.Collections.Generic.IReadOnlyList<BusTopicInfo> GetTopics() => Array.Empty<BusTopicInfo>();
        public event Action<string, object?>? FramePublished { add { } remove { } }
        public bool TryGetLatestRaw(string topic, out object? payload) { payload = null; return false; }
        private sealed class NullSub : IDisposable { public void Dispose() { } }
    }
}
