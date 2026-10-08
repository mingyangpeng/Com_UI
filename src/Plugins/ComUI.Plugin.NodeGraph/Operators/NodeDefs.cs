using ComUI.Sdk;

namespace ComUI.Plugin.NodeGraph;

/// <summary>内置节点类型：源（生成/取数）→ 处理（图像运算）→ 输出（发布到总线）。</summary>
public static class NodeDefs
{
    private static readonly List<NodeDef> _all = new()
    {
        // ==================== 源 ====================
        new()
        {
            Id = "source.image", Name = "测试图像源", Category = "源", Group = "IO",
            Outputs = new[] { new PortDef { Name = "图像", Type = PortType.Image } },
            Params = new[]
            {
                new AlgoParam { Name = "pattern", Label = "图案", Type = AlgoParamType.Enum,
                    Default = "圆环", Options = new[] { "圆环", "渐变", "棋盘" } },
            },
            Execute = ctx =>
            {
                var pattern = ctx.Param("pattern", "圆环");
                ctx.Log($"生成测试图像（{pattern}）");
                return new Dictionary<string, object?> { ["图像"] = MakeTestImage(pattern) };
            },
        },
        new()
        {
            Id = "source.cloud", Name = "测试点云源", Category = "源", Group = "3D 点云",
            Outputs = new[] { new PortDef { Name = "点云", Type = PortType.Cloud } },
            Params = new[]
            {
                new AlgoParam { Name = "points", Label = "点数", Type = AlgoParamType.Int, Default = 100000, Min = 10000, Max = 500000 },
            },
            Execute = ctx =>
            {
                var n = ctx.Param("points", 100000);
                ctx.Log($"生成环面点云（{n} 点）");
                return new Dictionary<string, object?> { ["点云"] = MakeTorusCloud(n) };
            },
        },

        new()
        {
            Id = "source.cube", Name = "立方体点云源", Category = "源", Group = "3D 点云",
            Outputs = new[] { new PortDef { Name = "点云", Type = PortType.Cloud } },
            Params = new[]
            {
                new AlgoParam { Name = "size", Label = "边长", Type = AlgoParamType.Number, Default = 2.0, Min = 0.5, Max = 10 },
                new AlgoParam { Name = "step", Label = "点间距", Type = AlgoParamType.Number, Default = 0.02, Min = 0.005, Max = 0.2 },
            },
            Execute = ctx =>
            {
                double size = ctx.Param("size", 2.0);
                double step = ctx.Param("step", 0.02);
                ctx.Log($"生成立方体表面点云（边长 {size}，间距 {step}）");
                return new Dictionary<string, object?> { ["点云"] = MakeCubeCloud(size, step) };
            },
        },

        new()
        {
            Id = "source.heightmap", Name = "关联高度图源", Category = "源", Group = "联动演示",
            Outputs = new[] { new PortDef { Name = "点云", Type = PortType.Cloud }, new PortDef { Name = "图像", Type = PortType.Image } },
            Params = new[]
            {
                new AlgoParam { Name = "n", Label = "分辨率", Type = AlgoParamType.Int, Default = 256, Min = 64, Max = 512 },
                new AlgoParam { Name = "amp", Label = "起伏幅度", Type = AlgoParamType.Number, Default = 0.4, Min = 0.1, Max = 1.0 },
            },
            Execute = ctx =>
            {
                int n = ctx.Param("n", 256);
                double amp = ctx.Param("amp", 0.4);
                ctx.Log($"生成关联高度图（{n}×{n}，点云+图像同源）");
                var (cloud, image) = MakeHeightMap(n, amp);
                return new Dictionary<string, object?> { ["点云"] = cloud, ["图像"] = image };
            },
        },

        new()
        {
            Id = "pub.remove", Name = "移除点云", Category = "输出", Group = "联动演示",
            Params = new[]
            {
                new AlgoParam { Name = "id", Label = "实体Id", Type = AlgoParamType.Text, Default = "立方体" },
            },
            Execute = ctx =>
            {
                var id = ctx.Param("id", "立方体");
                ctx.Publish(CloudPayload.TopicRemoved, new CloudPayload { Id = id });
                ctx.Log($"发布点云移除 → {id}");
                return new Dictionary<string, object?>();
            },
        },

        // ==================== 处理 ====================
        new()
        {
            Id = "img.gray", Name = "灰度化", Category = "处理", Group = "2D 图像",
            Inputs = new[] { new PortDef { Name = "输入", Type = PortType.Image } },
            Outputs = new[] { new PortDef { Name = "输出", Type = PortType.Image } },
            Execute = ctx => Img(ctx, px =>
            {
                for (int i = 0; i < px.Length; i += 4)
                {
                    byte y = (byte)((px[i] * 299 + px[i + 1] * 587 + px[i + 2] * 114) / 1000);
                    px[i] = px[i + 1] = px[i + 2] = y;
                }
            }),
        },
        new()
        {
            Id = "img.invert", Name = "反相", Category = "处理", Group = "2D 图像",
            Inputs = new[] { new PortDef { Name = "输入", Type = PortType.Image } },
            Outputs = new[] { new PortDef { Name = "输出", Type = PortType.Image } },
            Execute = ctx => Img(ctx, px =>
            {
                for (int i = 0; i < px.Length; i += 4)
                {
                    px[i] = (byte)(255 - px[i]);
                    px[i + 1] = (byte)(255 - px[i + 1]);
                    px[i + 2] = (byte)(255 - px[i + 2]);
                }
            }),
        },
        new()
        {
            Id = "img.bright", Name = "亮度/对比度", Category = "处理", Group = "2D 图像",
            Inputs = new[] { new PortDef { Name = "输入", Type = PortType.Image } },
            Outputs = new[] { new PortDef { Name = "输出", Type = PortType.Image } },
            Params = new[]
            {
                new AlgoParam { Name = "bright", Label = "亮度", Type = AlgoParamType.Number, Default = 20.0, Min = -100, Max = 100 },
                new AlgoParam { Name = "contrast", Label = "对比度", Type = AlgoParamType.Number, Default = 1.0, Min = 0.2, Max = 3 },
            },
            Execute = ctx =>
            {
                double bright = ctx.Param("bright", 20.0);
                double contrast = ctx.Param("contrast", 1.0);
                ctx.Log($"亮度 {bright:F0} / 对比度 {contrast:F2}");
                return Img(ctx, px =>
                {
                    for (int i = 0; i < px.Length; i += 4)
                    {
                        px[i] = Clamp((px[i] - 128) * contrast + 128 + bright);
                        px[i + 1] = Clamp((px[i + 1] - 128) * contrast + 128 + bright);
                        px[i + 2] = Clamp((px[i + 2] - 128) * contrast + 128 + bright);
                    }
                });
            },
        },
        new()
        {
            Id = "img.blur", Name = "模糊(3×3)", Category = "处理", Group = "2D 图像",
            Inputs = new[] { new PortDef { Name = "输入", Type = PortType.Image } },
            Outputs = new[] { new PortDef { Name = "输出", Type = PortType.Image } },
            Execute = ctx => BoxBlur(ctx),
        },

        // ==================== 输出 ====================
        new()
        {
            Id = "pub.image", Name = "发布图像", Category = "输出", Group = "IO",
            Inputs = new[] { new PortDef { Name = "输入", Type = PortType.Image } },
            Params = new[]
            {
                new AlgoParam { Name = "topic", Label = "主题", Type = AlgoParamType.Text, Default = BusTopics.ImageStitched },
            },
            Execute = ctx =>
            {
                var img = ctx.Input<ImagePayload>("输入");
                var topic = ctx.Param("topic", BusTopics.ImageStitched);
                ctx.Log($"发布图像 {img.Width}×{img.Height} → {topic}");
                ctx.Publish(topic, img);
                return new Dictionary<string, object?>();
            },
        },
        // ==================== 输出预览（draw 栏目） ====================
        new()
        {
            Id = "draw.2d", Name = "2D 预览", Category = "输出", Group = "输出预览",
            HasPreview = true,
            Inputs = new[] { new PortDef { Name = "输入", Type = PortType.Image } },
            Execute = ctx =>
            {
                var img = ctx.Input<ImagePayload>("输入");
                ctx.Preview(img);   // 卡片内嵌缩略图
                ctx.Log($"预览图像 {img.Width}×{img.Height}（点「→ 2D 工作台」发送）");
                return new Dictionary<string, object?>();
            },
        },
        new()
        {
            Id = "draw.3d", Name = "3D 预览", Category = "输出", Group = "输出预览",
            HasPreview = true,
            Inputs = new[] { new PortDef { Name = "输入", Type = PortType.Cloud } },
            Execute = ctx =>
            {
                var cloud = ctx.Input<CloudPayload>("输入");
                ctx.Preview(cloud);   // 卡片内嵌 3D 视图（拖拽旋转）
                ctx.Log($"预览点云 {cloud.Count} 点（点「→ 3D 工作台」导入）");
                return new Dictionary<string, object?>();
            },
        },
        new()
        {
            Id = "pub.cloud", Name = "发布点云", Category = "输出", Group = "IO",
            Inputs = new[] { new PortDef { Name = "输入", Type = PortType.Cloud } },
            Params = new[]
            {
                new AlgoParam { Name = "topic", Label = "主题", Type = AlgoParamType.Text, Default = BusTopics.CloudMerged },
            },
            Execute = ctx =>
            {
                var cloud = ctx.Input<CloudPayload>("输入");
                var topic = ctx.Param("topic", BusTopics.CloudMerged);
                ctx.Log($"发布点云 {cloud.Count} 点 → {topic}");
                ctx.Publish(topic, cloud);
                return new Dictionary<string, object?>();
            },
        },
    };

    public static IReadOnlyList<NodeDef> All => _all;

    public static Dictionary<string, NodeDef> Map { get; } =
        _all.ToDictionary(d => d.Id, StringComparer.OrdinalIgnoreCase);

    /// <summary>注册一个算子（内置 + JSON 声明 + 手写），Id 重复时忽略并返回 false。</summary>
    public static bool Register(NodeDef def)
    {
        if (Map.ContainsKey(def.Id)) return false;
        _all.Add(def);
        Map[def.Id] = def;
        return true;
    }

    // ==================== 执行辅助 ====================

    /// <summary>图像处理统一入口：拷贝输入缓冲（不污染上游）→ 原地变换 → 新 ImagePayload。</summary>
    private static Dictionary<string, object?> Img(NodeRunContext ctx, Action<byte[]> transform)
    {
        var img = ctx.Input<ImagePayload>("输入");
        var px = new byte[img.PixelsBgra.Length];
        Array.Copy(img.PixelsBgra, px, px.Length);
        transform(px);
        return new Dictionary<string, object?>
        {
            ["输出"] = new ImagePayload { Width = img.Width, Height = img.Height, PixelsBgra = px, Source = img.Source },
        };
    }

    private static byte Clamp(double v) => (byte)Math.Clamp(v, 0, 255);

    private static Dictionary<string, object?> BoxBlur(NodeRunContext ctx)
    {
        var img = ctx.Input<ImagePayload>("输入");
        int w = img.Width, h = img.Height;
        var src = img.PixelsBgra;
        var dst = new byte[src.Length];
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                int di = (y * w + x) * 4;
                for (int c = 0; c < 3; c++)
                {
                    int sum = 0, count = 0;
                    for (int dy = -1; dy <= 1; dy++)
                    {
                        int yy = y + dy;
                        if (yy < 0 || yy >= h) continue;
                        for (int dx = -1; dx <= 1; dx++)
                        {
                            int xx = x + dx;
                            if (xx < 0 || xx >= w) continue;
                            sum += src[(yy * w + xx) * 4 + c];
                            count++;
                        }
                    }
                    dst[di + c] = (byte)(sum / count);
                }
                dst[di + 3] = 255;
            }
        }
        return new Dictionary<string, object?>
        {
            ["输出"] = new ImagePayload { Width = w, Height = h, PixelsBgra = dst, Source = img.Source },
        };
    }

    // ==================== 测试数据 ====================

    private static ImagePayload MakeTestImage(string pattern)
    {
        const int w = 512, h = 384;
        var px = new byte[w * h * 4];
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                int i = (y * w + x) * 4;
                byte r, g, b;
                switch (pattern)
                {
                    case "渐变":
                        r = (byte)(x * 255 / w); g = (byte)(y * 255 / h); b = 110;
                        break;
                    case "棋盘":
                    {
                        bool c = (x / 32 + y / 32) % 2 == 0;
                        r = g = b = c ? (byte)210 : (byte)60;
                        if (x / 8 % 2 == 0) b = 180;
                        break;
                    }
                    default: // 圆环
                    {
                        double dx = x - w / 2.0, dy = y - h / 2.0;
                        double d = Math.Sqrt(dx * dx + dy * dy);
                        bool ring = (int)(d / 24) % 2 == 0;
                        r = ring ? (byte)240 : (byte)40;
                        g = (byte)(y * 200 / h + 40);
                        b = ring ? (byte)90 : (byte)200;
                        if (Math.Abs(dx) < 2 || Math.Abs(dy) < 2) { r = 255; g = 220; b = 40; }
                        break;
                    }
                }
                px[i] = b; px[i + 1] = g; px[i + 2] = r; px[i + 3] = 255;
            }
        }
        return new ImagePayload { Width = w, Height = h, PixelsBgra = px, Source = $"节点图·{pattern}" };
    }

    private static (float[] Points, byte[] Colors, int Count) _cloudCache;

    private static CloudPayload MakeTorusCloud(int count)
    {
        // 参数变化时重新生成；同一参数直接复用缓存
        if (_cloudCache.Count == count) return ToPayload();
        const double R = 2.4, r = 1.0;
        var pts = new float[count * 3];
        var colors = new byte[count * 3];
        var rnd = new Random(7);
        for (int i = 0; i < count; i++)
        {
            double u = rnd.NextDouble() * Math.PI * 2;
            double v = rnd.NextDouble() * Math.PI * 2;
            double x = (R + r * Math.Cos(v)) * Math.Cos(u);
            double y = r * Math.Sin(v);
            double z = (R + r * Math.Cos(v)) * Math.Sin(u);
            pts[i * 3] = (float)x; pts[i * 3 + 1] = (float)y; pts[i * 3 + 2] = (float)z;
            double t = (y + r) / (2 * r) * 4;
            (colors[i * 3], colors[i * 3 + 1], colors[i * 3 + 2]) = t switch
            {
                < 1 => Lerp((48, 60, 200), (0, 176, 240), t),
                < 2 => Lerp((0, 176, 240), (48, 208, 128), t - 1),
                < 3 => Lerp((48, 208, 128), (240, 208, 64), t - 2),
                _ => Lerp((240, 208, 64), (240, 64, 48), t - 3),
            };
        }
        _cloudCache = (pts, colors, count);
        return ToPayload();

        static CloudPayload ToPayload() => new()
        {
            Points = _cloudCache.Points, ColorsRgb = _cloudCache.Colors,
            Count = _cloudCache.Count, Source = "节点图·环面", Id = "环面",
        };
    }

    private static (double Size, double Step, CloudPayload Cloud)? _cubeCache;

    /// <summary>立方体表面点云（六面网格点，边线重复点由深度测试自然覆盖）。</summary>
    private static CloudPayload MakeCubeCloud(double size, double step)
    {
        if (_cubeCache is { } c && c.Size == size && c.Step == step) return c.Cloud;
        int n = (int)Math.Floor(size / step) + 1;
        var pts = new float[6 * n * n * 3];
        double half = size / 2;
        int k = 0;
        for (int face = 0; face < 6; face++)
        {
            for (int i = 0; i < n; i++)
            {
                for (int j = 0; j < n; j++)
                {
                    double u = -half + i * step, v = -half + j * step;
                    (float x, float y, float z) = face switch
                    {
                        0 => ((float)u, (float)half, (float)v),    // 顶面
                        1 => ((float)u, (float)-half, (float)v),   // 底面
                        2 => ((float)u, (float)v, (float)half),    // 前面 z+
                        3 => ((float)u, (float)v, (float)-half),   // 后面 z-
                        4 => ((float)half, (float)u, (float)v),    // 右面 x+
                        _ => ((float)-half, (float)u, (float)v),   // 左面 x-
                    };
                    pts[k++] = x; pts[k++] = y; pts[k++] = z;
                }
            }
        }
        var cloud = new CloudPayload { Id = "立方体", Count = pts.Length / 3, Points = pts, Source = "节点图·立方体" };
        _cubeCache = (size, step, cloud);
        return cloud;
    }

    private static ((int N, double Amp) Key, CloudPayload Cloud, ImagePayload Image)? _hmCache;

    /// <summary>关联高度图：同一高度场同时生成点云（x,z 平面 + y=高度）与伪彩图像，
    /// 点 (i,j) 与像素 (i,j) 一一对应——配合 sel/point3d 的 Uv 演示"点云选点↔图像高亮"。</summary>
    private static (CloudPayload, ImagePayload) MakeHeightMap(int n, double amp)
    {
        if (_hmCache is { } c && c.Key == (n, amp)) return (c.Cloud, c.Image);
        var h = new double[n * n];
        double hMin = double.MaxValue, hMax = double.MinValue;
        for (int j = 0; j < n; j++)
        {
            for (int i = 0; i < n; i++)
            {
                double u = i / (n - 1.0), v = j / (n - 1.0);
                double v2 =
                    0.55 * Math.Exp(-((u - 0.32) * (u - 0.32) + (v - 0.38) * (v - 0.38)) * 30) +
                    0.40 * Math.Exp(-((u - 0.68) * (u - 0.68) + (v - 0.60) * (v - 0.60)) * 45) -
                    0.35 * Math.Exp(-((u - 0.50) * (u - 0.50) + (v - 0.15) * (v - 0.15)) * 60) +
                    0.05 * Math.Sin(u * Math.PI * 6) * Math.Sin(v * Math.PI * 6);
                h[j * n + i] = v2 * amp;
                if (v2 < hMin) hMin = v2;
                if (v2 > hMax) hMax = v2;
            }
        }
        double span = Math.Max(1e-9, hMax - hMin);

        var pts = new float[n * n * 3];
        for (int j = 0; j < n; j++)
        {
            for (int i = 0; i < n; i++)
            {
                int k = (j * n + i) * 3;
                pts[k] = (float)((i / (n - 1.0) - 0.5) * 3.0);
                pts[k + 1] = (float)h[j * n + i];
                pts[k + 2] = (float)((j / (n - 1.0) - 0.5) * 3.0);
            }
        }
        var cloud = new CloudPayload { Id = "高度图", Count = n * n, Points = pts, Source = "节点图·关联高度图" };

        var px = new byte[n * n * 4];
        for (int p = 0; p < n * n; p++)
        {
            (byte r, byte g, byte b) = Jet((h[p] - hMin) / span);
            px[p * 4] = b; px[p * 4 + 1] = g; px[p * 4 + 2] = r; px[p * 4 + 3] = 255;
        }
        var image = new ImagePayload { Width = n, Height = n, PixelsBgra = px, Source = "节点图·关联高度图" };
        _hmCache = ((n, amp), cloud, image);
        return (cloud, image);
    }

    private static (byte, byte, byte) Jet(double t)
    {
        t = Math.Clamp(t, 0, 1);
        byte ch(double a, double b, double k) => (byte)Math.Clamp((a + (b - a) * k) * 255, 0, 255);
        if (t < 0.25) return (ch(0, 0, t / 0.25), ch(0, 1, t / 0.25), 255);
        if (t < 0.5) return (0, ch(1, 0.8, (t - 0.25) / 0.25), ch(1, 0, (t - 0.25) / 0.25));
        if (t < 0.75) return (ch(0, 1, (t - 0.5) / 0.25), ch(0.8, 1, (t - 0.5) / 0.25), 0);
        return (255, ch(1, 0, (t - 0.75) / 0.25), 0);
    }

    private static (byte, byte, byte) Lerp((byte, byte, byte) a, (byte, byte, byte) b, double k)
    {
        k = Math.Clamp(k, 0, 1);
        return (
            (byte)(a.Item1 + (b.Item1 - a.Item1) * k),
            (byte)(a.Item2 + (b.Item2 - a.Item2) * k),
            (byte)(a.Item3 + (b.Item3 - a.Item3) * k));
    }
}
