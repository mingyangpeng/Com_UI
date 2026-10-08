using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace ComUI.Core;

/// <summary>config/host.json 配置（JSONC，支持 // 注释）。缺省值保证无配置文件也能启动。</summary>
public sealed class HostConfig
{
    /// <summary>宿主版本（单一事实来源；UI 层 App.HostVersion 引用此常量）。</summary>
    public const string HostVersion = "0.3.0";

    public string PluginDirectory { get; init; } = "comdll";
    public bool AutoOpenPlugins { get; init; } = true;

    /// <summary>官方插件（不可卸载）Id 清单：加载后校验，缺失报错（产品基线，见 docs/ARCHITECTURE.md）。</summary>
    public IReadOnlyList<string> RequiredPlugins { get; init; } = Array.Empty<string>();

    /// <summary>读取 host.json；文件不存在/解析失败返回缺省值（错误由调用方记录）。</summary>
    public static HostConfig Load(string configDir, Action<string>? onError = null)
    {
        try
        {
            var path = Path.Combine(configDir, "host.json");
            if (!File.Exists(path)) return new HostConfig();
            var text = File.ReadAllText(path);
            var doc = JsonSerializer.Deserialize<HostConfigDto>(text, new JsonSerializerOptions
            {
                PropertyNameCaseInsensitive = true,
                ReadCommentHandling = JsonCommentHandling.Skip,
                AllowTrailingCommas = true,
            }) ?? new HostConfigDto();
            return new HostConfig
            {
                PluginDirectory = string.IsNullOrWhiteSpace(doc.PluginDirectory) ? "comdll" : doc.PluginDirectory,
                AutoOpenPlugins = doc.AutoOpenPlugins,
                RequiredPlugins = doc.RequiredPlugins ?? new List<string>(),
            };
        }
        catch (Exception ex)
        {
            onError?.Invoke($"host.json 读取失败（使用缺省配置）: {ex.Message}");
            return new HostConfig();
        }
    }

    private sealed class HostConfigDto
    {
        public string? PluginDirectory { get; set; }
        public bool AutoOpenPlugins { get; set; } = true;
        public List<string>? RequiredPlugins { get; set; }
    }
}
