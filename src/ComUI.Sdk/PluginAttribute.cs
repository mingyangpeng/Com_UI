namespace ComUI.Sdk;

/// <summary>标记插件实现类。一个程序集可包含多个插件。</summary>
[AttributeUsage(AttributeTargets.Class, AllowMultiple = false, Inherited = false)]
public sealed class PluginAttribute : Attribute
{
    /// <summary>插件唯一标识，例如 "comui.charts2d"。</summary>
    public string Id { get; }

    /// <summary>显示名称（展示在插件列表 / 标签页）。</summary>
    public string Name { get; }

    /// <summary>版本号。</summary>
    public string Version { get; }

    /// <summary>功能描述（展示在插件列表）。</summary>
    public string Description { get; }

    public PluginAttribute(string id, string name, string version = "1.0.0", string description = "")
    {
        Id = id;
        Name = name;
        Version = version;
        Description = description;
    }
}
