using ComUI.Sdk;

namespace ComUI.Core;

/// <summary>
/// 兼容垫片：AppPaths 实现已下沉到 ComUI.Sdk（2026-11-22，随 Sdk 单份加载、
/// 插件与宿主共用）。本类逐成员委托，宿主既有调用点（ComUI.Core.AppPaths）零改动。
/// 新代码请直接用 ComUI.Sdk.AppPaths。
/// </summary>
public static class AppPaths
{
    public static string ExeDir => Sdk.AppPaths.ExeDir;

    public static string RepoRoot => Sdk.AppPaths.RepoRoot;

    /// <summary>comdll 根目录（可被 config/host.json 的 pluginDirectory 覆盖）。</summary>
    public static string ComDllRoot
    {
        get => Sdk.AppPaths.ComDllRoot;
        set => Sdk.AppPaths.ComDllRoot = value;
    }

    public static string CommonDir => Sdk.AppPaths.CommonDir;

    public static string UiDir => Sdk.AppPaths.UiDir;

    public static string AlgoDir => Sdk.AppPaths.AlgoDir;

    public static string ConfigDir => Sdk.AppPaths.ConfigDir;

    public static string OutputDir => Sdk.AppPaths.OutputDir;

    public static string ErrorLogFile => Sdk.AppPaths.ErrorLogFile;
}
