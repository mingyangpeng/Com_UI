using Avalonia;
using ComUI.Core;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Host.Views;

/// <summary>欢迎页（固定文档）。</summary>
public sealed class WelcomeView : UserControl
{
    public WelcomeView()
    {
        var root = new StackPanel
        {
            MaxWidth = 860,
            Margin = new Thickness(48, 40),
            HorizontalAlignment = HorizontalAlignment.Center,
        };

        root.Children.Add(new TextBlock { Text = "ComUI 显示平台", FontSize = 28, FontWeight = FontWeight.Bold });
        root.Children.Add(new TextBlock
        {
            Text = "插件化显示框架（Avalonia 跨平台） · 宿主 v0.3.0 · UI / 算法双目录插件",
            FontSize = 13,
            Foreground = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D)),
            Margin = new Thickness(0, 8, 0, 30),
        });

        root.Children.Add(Card("① 插件目录",
            "comdll/common —— 共享契约（算法 IO 接口 DLL），宿主最先加载；\n" +
            "comdll/ui/<插件>/ —— UI 插件（实现 IPlugin，可注册多个面板）；\n" +
            "comdll/algo/<插件>/ —— 算法插件（算法 DLL + *.algo.json，或手写 IAlgoPlugin）。\n" +
            "安装 = 拷文件夹，按 F5（或工具栏「重新加载插件」）生效。"));
        root.Children.Add(Card("② 面板与标签页",
            "左侧「面板」侧边栏单击即打开；中间标签页支持多面板并存，可关闭；\n" +
            "图像 + 点云可并排对比。"));
        root.Children.Add(Card("③ 算法接入",
            "算法开发人员只需提供「算法 DLL + 一份 *.algo.json」：声明入口方法、参数与输出主题，" +
            "宿主自动生成参数对话框与执行入口，运行结果通过数据总线发布，面板自动显示。"));
        root.Children.Add(Card("④ 数据与联动",
            "算法 → IBus.Publish(topic, 数据) → 显示面板 Subscribe 显示；\n" +
            "面板间联动（点云选点 ↔ 图像高亮、图像框 ROI）通过约定主题 sel/point3d、sel/roi2d 广播。"));

        Content = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Content = root };
    }

    private static Control Card(string title, string body)
    {
        var stack = new StackPanel();
        stack.Children.Add(new TextBlock { Text = title, FontWeight = FontWeight.SemiBold, FontSize = 14 });
        stack.Children.Add(new TextBlock
        {
            Text = body,
            TextWrapping = TextWrapping.Wrap,
            Foreground = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D)),
            Margin = new Thickness(0, 6, 0, 0),
            FontSize = 12.5,
        });

        return new Border
        {
            Background = new SolidColorBrush(Color.FromRgb(0x26, 0x26, 0x29)),
            CornerRadius = new CornerRadius(6),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x31, 0x31, 0x35)),
            BorderThickness = new Thickness(1),
            Padding = new Thickness(18, 14),
            Margin = new Thickness(0, 0, 0, 12),
            Child = stack,
        };
    }
}
