using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;

namespace ComUI.Core;

/// <summary>
/// 每个插件文件夹一个可卸载的加载上下文（collectible ALC），支持"重新加载插件"。
/// 默认上下文里已有的程序集（ComUI.Sdk、comdll/common 契约、框架）一律回退到宿主版本，
/// 保证插件与宿主类型身份一致；comdll 中直接放置的依赖 DLL 也会被解析到。
/// </summary>
public sealed class PluginLoadContext : AssemblyLoadContext
{
    private readonly AssemblyDependencyResolver? _resolver;
    private readonly string _pluginDir;
    private readonly HashSet<string> _defaultNames;

    public PluginLoadContext(string folderName, string pluginDir, string? mainDllPath = null)
        : base(name: folderName + ".ctx", isCollectible: true)
    {
        _pluginDir = pluginDir;
        _defaultNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        // 只认【默认 ALC】里的程序集为宿主自有；collectible ALC 卸载残留的旧插件/依赖程序集
        // 不能算数（否则 F5 重载时 OpenTK、插件自身会被误判成宿主已有而无法再次加载）
        foreach (var a in AssemblyLoadContext.Default.Assemblies)
            _defaultNames.Add(a.GetName().Name ?? "");

        if (mainDllPath != null && File.Exists(mainDllPath))
            _resolver = new AssemblyDependencyResolver(mainDllPath);
    }

    protected override Assembly? Load(AssemblyName name)
    {
        var simple = name.Name ?? "";

        // Sdk / common 契约 / 框架程序集：用宿主已加载的版本
        if (simple.Equals("ComUI.Sdk", StringComparison.OrdinalIgnoreCase) ||
            _defaultNames.Contains(simple) ||
            simple.StartsWith("System") || simple.StartsWith("Microsoft") || simple.StartsWith("WindowsBase") ||
            simple.StartsWith("Avalonia") || simple.StartsWith("Dock.") ||
            simple.StartsWith("SkiaSharp") || simple.StartsWith("netstandard"))
            return null;

        var path = _resolver?.ResolveAssemblyToPath(name);
        if (path != null && File.Exists(path))
            return LoadFromAssemblyPath(path);

        // 插件文件夹中的自由依赖 DLL
        var loose = Path.Combine(_pluginDir, simple + ".dll");
        if (File.Exists(loose))
            return LoadFromAssemblyPath(loose);

        // comdll/common 中的共享契约
        var shared = Path.Combine(AppPaths.CommonDir, simple + ".dll");
        if (File.Exists(shared))
            return LoadFromAssemblyPath(shared);

        return null;
    }

    protected override nint LoadUnmanagedDll(string unmanagedDllName)
    {
        // 候选名：原名 + glfw→glfw3 这类常见别名（OpenTK 的 P/Invoke 名与包内文件名不同）
        var names = new List<string> { unmanagedDllName };
        if (unmanagedDllName == "glfw") names.Add("glfw3");

        foreach (var n in names)
        {
            var path = _resolver?.ResolveUnmanagedDllToPath(n);
            if (path != null)
                return LoadUnmanagedDllFromPath(path);

            // 插件文件夹中的自由原生库（与托管 loose 兜底对齐；P/Invoke 名可能不带扩展名）
            var loose = Path.Combine(_pluginDir, n);
            if (!File.Exists(loose)) loose += ".dll";
            if (File.Exists(loose))
                return LoadUnmanagedDllFromPath(loose);
        }
        return base.LoadUnmanagedDll(unmanagedDllName);
    }
}
