using System.IO;

namespace ComUI.Core;

/// <summary>
/// 目录解析：从 exe 所在目录逐级向上查找仓库根目录（包含 comdll / config 的目录），
/// 兼容 "dotnet run"（exe 在 bin 子目录）和发布后 exe 与 comdll 平级两种部署方式。
/// </summary>
public static class AppPaths
{
    public static string ExeDir { get; } = AppContext.BaseDirectory;

    public static string RepoRoot { get; } = FindUpward("comdll") ?? ExeDir;

    /// <summary>comdll 根目录（可被 config/host.json 的 pluginDirectory 覆盖）。</summary>
    public static string ComDllRoot { get; set; } = Path.Combine(RepoRoot, "comdll");

    /// <summary>共享契约程序集（算法 IO 接口 DLL 等），宿主最先加载。</summary>
    public static string CommonDir => Path.Combine(ComDllRoot, "common");

    /// <summary>UI 插件根目录（每个插件一个子文件夹）。</summary>
    public static string UiDir => Path.Combine(ComDllRoot, "ui");

    /// <summary>算法插件根目录（每个插件一个子文件夹：dll + *.algo.json）。</summary>
    public static string AlgoDir => Path.Combine(ComDllRoot, "algo");

    public static string ConfigDir { get; } = Path.Combine(RepoRoot, "config");

    public static string OutputDir { get; } = Path.Combine(RepoRoot, "output");

    public static string ErrorLogFile => Path.Combine(OutputDir, "error.log");

    private static string? FindUpward(string markerDirName)
    {
        var dir = new DirectoryInfo(ExeDir);
        for (int i = 0; i < 8 && dir != null; i++, dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, markerDirName);
            if (Directory.Exists(candidate)) return dir.FullName;
        }
        return null;
    }
}
