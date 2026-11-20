using System.IO;
using System.Text.Json;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 3D 工作台视图设置持久化（config/cloud3d_view.json）：抽稀/截断/切片/色带域/投影/锁定轴。
/// Load 容错（坏 JSON/缺字段回默认）；变更即存由视图负责。
/// </summary>
public sealed class CloudViewSettings
{
    public int Stride { get; set; } = 1;
    public double FarClip { get; set; }
    public int ClipAxis { get; set; } = 1;
    public double? ClipLo { get; set; }
    public double? ClipHi { get; set; }
    public double? ColorMin { get; set; }
    public double? ColorMax { get; set; }
    public int Projection { get; set; }
    public bool LockPitch { get; set; }

    public static CloudViewSettings Load(string path)
    {
        var def = new CloudViewSettings();
        try
        {
            if (!File.Exists(path)) return def;
            var d = JsonSerializer.Deserialize<CloudViewSettings>(File.ReadAllText(path), JsonOpts);
            if (d is null) return def;
            // 容错钳制
            d.Stride = Math.Clamp(d.Stride, 1, 10000);
            d.ClipAxis = Math.Clamp(d.ClipAxis, 0, 2);
            d.Projection = Math.Clamp(d.Projection, 0, 1);
            if (d.ClipLo is not null && d.ClipHi is not null && d.ClipLo > d.ClipHi) (d.ClipLo, d.ClipHi) = (d.ClipHi, d.ClipLo);
            return d;
        }
        catch { return def; }
    }

    public static void Save(string path, CloudViewSettings s)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, JsonSerializer.Serialize(s, new JsonSerializerOptions
            {
                WriteIndented = true,
                Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
            }));
        }
        catch { }
    }

    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };
}
