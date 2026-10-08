using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;
using Avalonia.Controls;
using ComUI.Sdk;

namespace ComUI.Core;

/// <summary>一个已加载的 UI 插件及其注册的面板。</summary>
public sealed class UiPluginRecord
{
    public required PluginAttribute Meta { get; init; }
    public required IPlugin Instance { get; init; }
    public required PluginLoadContext Context { get; init; }
    public required string Folder { get; init; }
    public required IPluginContext PluginContext { get; init; }
    public List<PanelDescriptor> Panels { get; } = new();
    public DateTime LoadTime { get; } = DateTime.Now;

    public string Name => Meta.Name;
    public string Version => Meta.Version;
    public string Description => Meta.Description;
}

/// <summary>一个已加载的算法插件（手写壳或 JSON 生成）及其命令。</summary>
public sealed class AlgoRecord
{
    public required string Name { get; init; }
    public required string Id { get; init; }
    public required string Version { get; init; }
    public required string Description { get; init; }
    public required string Folder { get; init; }
    public required PluginLoadContext? Context { get; init; }
    public required IAlgoContext AlgoContext { get; init; }
    public required List<AlgoCommand> Commands { get; init; }
    public bool IsJsonDriven { get; init; }
}

/// <summary>
/// 插件管理器 v2：
/// - 预加载 comdll/common（共享契约，进默认上下文）
/// - 扫描 comdll/ui/&lt;插件&gt;/（IPlugin，注册面板）
/// - 扫描 comdll/algo/&lt;插件&gt;/（IAlgoPlugin 手写壳 或 *.algo.json 交给通用执行器）
/// 每个插件文件夹独立可卸载 ALC，支持 F5 热重载。
/// </summary>
public sealed class PluginManager
{
    private readonly LogService _log;
    private readonly IBus _bus;
    private readonly Action<string>? _setStatus;
    private readonly List<UiPluginRecord> _uiPlugins = new();
    private readonly List<AlgoRecord> _algoPlugins = new();
    private readonly List<PluginLoadContext> _contexts = new();

    public IReadOnlyList<UiPluginRecord> UiPlugins => _uiPlugins;
    public IReadOnlyList<AlgoRecord> AlgoPlugins => _algoPlugins;

    /// <summary>UI 插件上下文创建完成（供宿主订阅 OpenDocument 等请求）。</summary>
    public event Action<IPluginContext>? UiContextCreated;

    /// <summary>官方插件（不可卸载）Id 清单（host.json requiredPlugins）；加载后校验缺失项。</summary>
    public IReadOnlyList<string> RequiredPlugins { get; set; } = Array.Empty<string>();

    /// <summary>本次加载缺失的官方插件 Id（LoadAll 后读取；空 = 基线完整）。</summary>
    public IReadOnlyList<string> MissingRequiredPlugins { get; private set; } = Array.Empty<string>();

    public PluginManager(LogService log, IBus bus, Action<string>? setStatus = null)
    {
        _log = log;
        _bus = bus;
        _setStatus = setStatus;
    }

    private HashSet<string> _defaultLoadedNames = new(StringComparer.OrdinalIgnoreCase);

    public void LoadAll()
    {
        // 记录当前【默认 ALC】已加载的程序集名（Sdk / 框架 / common 契约），插件扫描时跳过，
        // 防止类型身份分裂。注意：必须查 AssemblyLoadContext.Default 而不是 AppDomain——
        // collectible ALC 卸载不完全时旧插件/OpenTK 程序集仍挂在进程上，
        // 若按进程判定会把重载的插件自身误判成"宿主已有"而跳过（F5 后 UI 插件 0 个的根因）
        _defaultLoadedNames = new HashSet<string>(
            AssemblyLoadContext.Default.Assemblies.Select(a => a.GetName().Name ?? ""),
            StringComparer.OrdinalIgnoreCase);

        Directory.CreateDirectory(AppPaths.CommonDir);
        Directory.CreateDirectory(AppPaths.UiDir);
        Directory.CreateDirectory(AppPaths.AlgoDir);

        LoadCommon();
        LoadUiPlugins();
        LoadAlgoPlugins();

        _log.Info($"加载完成：common 见上，UI 插件 {_uiPlugins.Count} 个，算法插件 {_algoPlugins.Count} 个。");
        MissingRequiredPlugins = RequiredPlugins
            .Where(id => _uiPlugins.All(u => u.Meta.Id != id))
            .ToList();
    }

    // ==================== common ====================

    private void LoadCommon()
    {
        foreach (var dll in Directory.EnumerateFiles(AppPaths.CommonDir, "*.dll", SearchOption.TopDirectoryOnly))
        {
            try
            {
                var asm = Assembly.LoadFrom(dll);
                _defaultLoadedNames.Add(asm.GetName().Name ?? "");
                _log.Info($"共享契约已加载: {asm.GetName().Name}");
            }
            catch (Exception ex)
            {
                _log.Error($"共享契约加载失败 {Path.GetFileName(dll)}: {ex.Message}");
            }
        }
    }

    // ==================== ui 插件 ====================

    private void LoadUiPlugins()
    {
        var dirs = Directory.EnumerateDirectories(AppPaths.UiDir).ToList();
        _log.Info($"扫描 UI 插件目录: {AppPaths.UiDir}（{dirs.Count} 个子文件夹）");
        foreach (var dir in dirs)
            LoadUiPluginFolder(dir);
    }

    private void LoadUiPluginFolder(string folder)
    {
        var folderName = Path.GetFileName(folder.TrimEnd(Path.DirectorySeparatorChar));
        var alc = new PluginLoadContext(folderName, folder, FindMainDll(folder));
        _contexts.Add(alc);

        try
        {
            UiPluginRecord? record = null;
            var dlls = Directory.EnumerateFiles(folder, "*.dll", SearchOption.TopDirectoryOnly).ToList();
            _log.Debug($"[{folderName}] 尝试 {dlls.Count} 个 DLL");
            foreach (var dll in dlls)
            {
                try
                {
                    // 跳过宿主默认上下文已有的程序集（Sdk / common 契约），
                    // 否则 LoadFromAssemblyPath 会把它们拉进插件 ALC，造成类型身份分裂
                    var simpleName = AssemblyName.GetAssemblyName(dll).Name ?? "";
                    if (_defaultLoadedNames.Contains(simpleName))
                    {
                        _log.Debug($"[{folderName}] 跳过宿主已有程序集 {simpleName}");
                        continue;
                    }

                    var asm = alc.LoadFromAssemblyPath(dll);
                    _log.Debug($"[{folderName}] 已加载程序集 {asm.GetName().Name}");
                    foreach (var t in SafeGetTypes(asm))
                    {
                        if (t.IsAbstract || t.IsInterface || !typeof(IPlugin).IsAssignableFrom(t)) continue;
                        var attr = t.GetCustomAttribute<PluginAttribute>();
                        if (attr is null) continue;

                        var instance = (IPlugin)Activator.CreateInstance(t)!;
                        var ctx = new HostPluginContext(attr.Id, _log, _bus, _setStatus) { PluginFolder = folder };
                        UiContextCreated?.Invoke(ctx);   // 先让宿主挂上上下文事件（OpenDocument/文档工厂），再初始化插件——否则 Initialize 期间的事件无人接收
                        instance.Initialize(ctx);
                        record = new UiPluginRecord
                        {
                            Meta = attr, Instance = instance, Context = alc,
                            Folder = folder, PluginContext = ctx,
                        };
                        foreach (var panel in instance.GetPanels())
                        {
                            if (string.IsNullOrWhiteSpace(panel.Id) || _uiPlugins.Any(u => u.Panels.Any(p => p.Id == panel.Id)))
                            {
                                _log.Error($"[{attr.Id}] 面板 Id 无效或重复：\"{panel.Id}\"，已忽略");
                                continue;
                            }
                            record.Panels.Add(panel);
                        }
                        _uiPlugins.Add(record);
                        _log.Info($"UI 插件已加载: {attr.Name} v{attr.Version} <{attr.Id}>（{record.Panels.Count} 个面板）");
                        break; // 每个文件夹取第一个插件实现
                    }
                    if (record != null) break;
                }
                catch (BadImageFormatException)
                {
                    _log.Debug($"跳过非托管/不兼容 DLL: {Path.GetFileName(dll)}");
                }
            }

            if (record is null)
            {
                TryUnload(alc);
                _contexts.Remove(alc);
                _log.Debug($"跳过非插件文件夹: {folderName}");
            }
        }
        catch (Exception ex)
        {
            _log.Error($"UI 插件加载失败 {folderName}: {ex.Message}");
        }
    }

    // ==================== algo 插件 ====================

    private void LoadAlgoPlugins()
    {
        foreach (var dir in Directory.EnumerateDirectories(AppPaths.AlgoDir))
            LoadAlgoPluginFolder(dir);
    }

    private void LoadAlgoPluginFolder(string folder)
    {
        var folderName = Path.GetFileName(folder.TrimEnd(Path.DirectorySeparatorChar));
        var alc = new PluginLoadContext(folderName, folder, FindMainDll(folder));
        _contexts.Add(alc);
        var used = false;

        try
        {
            // 1) 手写壳 IAlgoPlugin
            foreach (var dll in Directory.EnumerateFiles(folder, "*.dll", SearchOption.TopDirectoryOnly))
            {
                try
                {
                    // 跳过宿主默认上下文已有的程序集（同 UI 插件扫描）
                    var simpleName = AssemblyName.GetAssemblyName(dll).Name ?? "";
                    if (_defaultLoadedNames.Contains(simpleName)) continue;

                    var asm = alc.LoadFromAssemblyPath(dll);
                    foreach (var t in SafeGetTypes(asm))
                    {
                        if (t.IsAbstract || t.IsInterface || !typeof(IAlgoPlugin).IsAssignableFrom(t)) continue;
                        var attr = t.GetCustomAttribute<PluginAttribute>();
                        if (attr is null) continue;

                        var instance = (IAlgoPlugin)Activator.CreateInstance(t)!;
                        var ctx = new HostAlgoContext(attr.Id, _log, _bus) { PluginFolder = folder };
                        instance.Initialize(ctx);

                        _algoPlugins.Add(new AlgoRecord
                        {
                            Id = attr.Id, Name = attr.Name, Version = attr.Version,
                            Description = attr.Description, Folder = folder,
                            Context = alc, AlgoContext = ctx,
                            Commands = instance.GetCommands().ToList(),
                        });
                        used = true;
                        _log.Info($"算法插件已加载: {attr.Name} v{attr.Version} <{attr.Id}>（{instance.GetCommands().Count()} 个命令）");
                        break;
                    }
                    if (used) break;
                }
                catch (BadImageFormatException) { /* 非托管 dll，交给 JSON 或跳过 */ }
            }

            // 2) *.algo.json → 通用执行器
            foreach (var jsonFile in Directory.EnumerateFiles(folder, "*.algo.json", SearchOption.TopDirectoryOnly))
            {
                try
                {
                    var json = JsonSerializer.Deserialize<AlgoJsonDescriptor>(
                        File.ReadAllText(jsonFile),
                        new JsonSerializerOptions { ReadCommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
                    if (json is null) continue;

                    var cmd = GenericAlgoRunner.Create(json, folder, alc, _bus, _log);
                    if (cmd is null) continue;

                    _algoPlugins.Add(new AlgoRecord
                    {
                        Id = json.Plugin.Id, Name = json.Plugin.Name, Version = json.Plugin.Version,
                        Description = json.Plugin.Description, Folder = folder,
                        Context = alc, AlgoContext = new HostAlgoContext(json.Plugin.Id, _log, _bus) { PluginFolder = folder },
                        Commands = new List<AlgoCommand> { cmd },
                        IsJsonDriven = true,
                    });
                    used = true;
                    _log.Info($"算法(JSON)已加载: {json.Plugin.Name} v{json.Plugin.Version} <{json.Plugin.Id}> ← {Path.GetFileName(jsonFile)}");
                }
                catch (Exception ex)
                {
                    _log.Error($"算法 JSON 解析失败 {Path.GetFileName(jsonFile)}: {ex.Message}");
                }
            }

            if (!used)
            {
                TryUnload(alc);
                _contexts.Remove(alc);
                _log.Debug($"跳过非插件文件夹: {folderName}");
            }
        }
        catch (Exception ex)
        {
            _log.Error($"算法插件加载失败 {folderName}: {ex.Message}");
        }
    }

    // ==================== 卸载 / 重载 ====================

    public void ShutdownAll()
    {
        foreach (var p in _uiPlugins)
        {
            try { p.Instance.Shutdown(); }
            catch (Exception ex) { _log.Warn($"UI 插件 {p.Name} Shutdown 异常: {ex.Message}"); }
        }
        // JSON 算法无状态，手写壳也没有统一 Shutdown 之外的资源（IAlgoPlugin.Shutdown 已在实例上）
        var unloaded = _contexts.Where(c => c is not null).ToList();
        var refs = unloaded.Select(c => new WeakReference(c)).ToList();
        foreach (var ctx in unloaded)
            TryUnload(ctx);
        _contexts.Clear();
        _uiPlugins.Clear();
        _algoPlugins.Clear();
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        // 卸载完整性验证：collectible ALC 卸载后应被 GC 回收；WeakReference 仍存活 = 有残留引用
        // （典型：插件订阅了总线/事件但 Shutdown 没退订）——下次加载会类型分裂（BUG-039 温床）
        for (int i = 0; i < unloaded.Count; i++)
            if (refs[i].IsAlive)
                _log.Warn($"插件 {unloaded[i].Name} 卸载不完全（存在残留引用，常见原因：总线/事件未退订）——下次加载可能类型分裂");
    }

    public void ReloadAll()
    {
        _log.Info("卸载全部插件…");
        ShutdownAll();
        LoadAll();
    }

    /// <summary>推断插件主程序集：与文件夹内 *.deps.json 同名的 .dll（供 AssemblyDependencyResolver 解析依赖与原生库）。</summary>
    private static string? FindMainDll(string folder)
        => Directory.EnumerateFiles(folder, "*.deps.json", SearchOption.TopDirectoryOnly)
            .Select(d => Path.Combine(folder, Path.GetFileNameWithoutExtension(d) + ".dll"))
            .FirstOrDefault(File.Exists);

    private static void TryUnload(PluginLoadContext alc)
    {
        try { alc.Unload(); } catch { /* 忽略卸载失败 */ }
    }

    private static IEnumerable<Type> SafeGetTypes(Assembly asm)
    {
        try { return asm.GetTypes(); }
        catch (ReflectionTypeLoadException ex) { return ex.Types.Where(t => t is not null).Select(t => t!); }
        catch { return Array.Empty<Type>(); }
    }
}

/// <summary>IPluginContext 宿主实现。</summary>
public sealed class HostPluginContext : IPluginContext
{
    private readonly LogService _log;
    private readonly IBus _bus;
    private readonly Action<string>? _setStatus;

    public HostPluginContext(string pluginId, LogService log, IBus bus, Action<string>? setStatus)
    {
        PluginId = pluginId;
        _log = log;
        _bus = bus;
        _setStatus = setStatus;
    }

    public string PluginId { get; }
    public string HostVersion => HostConfig.HostVersion;
    public string PluginFolder { get; init; } = "";
    public IBus Bus => _bus;

    public event Action<string>? PanelShown;
    public event Action<string>? PanelHidden;

    /// <summary>插件请求打开动态文档页（标题, 内容, 恢复键可空）。</summary>
    public event Action<string, Control, string?>? OpenDocumentRequested;

    /// <summary>插件登记动态文档页恢复工厂（类型, 键体→内容）——宿主会话恢复用。</summary>
    public event Action<string, Func<string, Control>>? DocumentFactoryRegistered;

    /// <summary>插件注册的「文件」菜单入口（在宿主加载完成后由主窗口读取渲染）。</summary>
    public List<(string Header, Action Execute)> FileMenuEntries { get; } = new();

    public void Log(string message) => _log.Info($"[{PluginId}] {message}");
    public void LogWarning(string message) => _log.Warn($"[{PluginId}] {message}");
    public void LogError(string message) => _log.Error($"[{PluginId}] {message}");
    public void SetStatus(string status) => _setStatus?.Invoke(status);

    public void AddFileMenuEntry(string header, Action execute)
    {
        FileMenuEntries.Add((header, execute));
        _log.Info($"注册文件菜单入口: {header}");
    }

    public void OpenDocument(string title, Control content, string? restoreKey = null)
    {
        _log.Info($"请求打开动态页面: {title}");
        OpenDocumentRequested?.Invoke(title, content, restoreKey);
    }

    public void RegisterDocumentFactory(string docType, Func<string, Control> reopen)
    {
        _log.Info($"注册文档恢复工厂: {docType}");
        DocumentFactoryRegistered?.Invoke(docType, reopen);
    }

    /// <summary>宿主 UI 层在面板显示/隐藏时回调（拆分后跨程序集，须 public）。</summary>
    public void RaisePanelShown(string panelId) => PanelShown?.Invoke(panelId);
    public void RaisePanelHidden(string panelId) => PanelHidden?.Invoke(panelId);
}

/// <summary>IAlgoContext 宿主实现。</summary>
public sealed class HostAlgoContext : IAlgoContext
{
    private readonly LogService _log;
    private readonly IBus _bus;

    public HostAlgoContext(string pluginId, LogService log, IBus bus)
    {
        PluginId = pluginId;
        _log = log;
        _bus = bus;
    }

    public string PluginId { get; }
    public string HostVersion => HostConfig.HostVersion;
    public string PluginFolder { get; init; } = "";
    public IBus Bus => _bus;

    public void Log(string message) => _log.Info($"[{PluginId}] {message}");
    public void LogWarning(string message) => _log.Warn($"[{PluginId}] {message}");
    public void LogError(string message) => _log.Error($"[{PluginId}] {message}");
    public void Publish<T>(string topic, T payload) => _bus.Publish(topic, payload);
}
