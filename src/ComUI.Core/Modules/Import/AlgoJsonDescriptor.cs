using System.Text.Json;
using System.Text.Json.Serialization;

namespace ComUI.Core;

/// <summary>*.algo.json 描述文件模型（算法 DLL + JSON → 宿主自动生成命令）。</summary>
public sealed class AlgoJsonDescriptor
{
    [JsonPropertyName("plugin")]
    public PluginMeta Plugin { get; set; } = new();

    /// <summary>dotnet = C# 程序集（反射调用）；native = C 导出函数（P/Invoke）。</summary>
    [JsonPropertyName("runtime")]
    public string Runtime { get; set; } = "dotnet";

    [JsonPropertyName("assembly")]
    public string Assembly { get; set; } = "";

    [JsonPropertyName("type")]
    public string? Type { get; set; }

    [JsonPropertyName("method")]
    public string? Method { get; set; }

    [JsonPropertyName("params")]
    public List<AlgoJsonParam> Params { get; set; } = new();

    [JsonPropertyName("inputs")]
    public List<AlgoJsonInput> Inputs { get; set; } = new();

    [JsonPropertyName("outputs")]
    public List<AlgoJsonOutput> Outputs { get; set; } = new();

    [JsonPropertyName("cancel")]
    public AlgoJsonCancel? Cancel { get; set; }

    public sealed class PluginMeta
    {
        [JsonPropertyName("id")] public string Id { get; set; } = "";
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("version")] public string Version { get; set; } = "1.0.0";
        [JsonPropertyName("description")] public string Description { get; set; } = "";
    }

    public sealed class AlgoJsonParam
    {
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("label")] public string? Label { get; set; }
        /// <summary>number | int | text | bool | enum | path</summary>
        [JsonPropertyName("type")] public string Type { get; set; } = "number";
        [JsonPropertyName("default")] public JsonElement Default { get; set; }
        [JsonPropertyName("options")] public List<string>? Options { get; set; }
        [JsonPropertyName("min")] public double? Min { get; set; }
        [JsonPropertyName("max")] public double? Max { get; set; }
    }

    public sealed class AlgoJsonInput
    {
        /// <summary>与方法参数名对应。</summary>
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        /// <summary>目前支持 "topic"：从总线取该主题最新帧。</summary>
        [JsonPropertyName("source")] public string Source { get; set; } = "topic";
        [JsonPropertyName("topic")] public string Topic { get; set; } = "";
    }

    public sealed class AlgoJsonOutput
    {
        /// <summary>返回值上的属性路径，如 "Cloud" 或 "Result.Cloud"。</summary>
        [JsonPropertyName("from")] public string From { get; set; } = "";
        [JsonPropertyName("topic")] public string Topic { get; set; } = "";
    }

    public sealed class AlgoJsonCancel
    {
        /// <summary>token（方法含 CancellationToken 参数）| export:函数名（原生）| none</summary>
        [JsonPropertyName("mode")] public string Mode { get; set; } = "token";
    }
}

/// <summary>运行期参数值。</summary>
public sealed class AlgoParamValue
{
    public string Name { get; init; } = "";
    public ComUI.Sdk.AlgoParam Descriptor { get; init; } = new();
    public object? Value { get; set; }
}
