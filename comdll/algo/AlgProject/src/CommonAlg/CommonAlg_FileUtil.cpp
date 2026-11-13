// CommonAlg_FileUtil.cpp —— 文件与目录基础工具实现（跨平台）。
// Windows 用 Win32 API（FindFirstFileA 支持通配），其他平台用 dirent。

#include "include/CommonAlg_FileUtil.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace {

bool EndsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 统一分隔符为 '/'（Windows 输入可能混用）
std::string NormalizeSeps(std::string p)
{
    for (char& c : p)
        if (c == '\\')
            c = '/';
    return p;
}

} // namespace

// ---------------- 文件枚举 ----------------

std::vector<std::string> CommonAlg_ListFiles(const std::string& folder,
                                             const std::string& ext)
{
    std::vector<std::string> out;
    const std::string dir = NormalizeSeps(folder);
    if (dir.empty())
        return out;

#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    const std::string pattern = dir + "/*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        const std::string name = fd.cFileName;
        if (!ext.empty() && !EndsWith(name, ext))
            continue;
        out.push_back(dir + "/" + name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d)
        return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        if (!ext.empty() && !EndsWith(name, ext))
            continue;
        out.push_back(dir + "/" + name);
    }
    closedir(d);
#endif
    std::sort(out.begin(), out.end());   // 按全路径排序 = 文件名自然顺序
    return out;
}

// ---------------- 文件读写 ----------------

bool CommonAlg_ReadFile(const std::string& path, std::string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return true;
}

bool CommonAlg_WriteFile(const std::string& path, const std::string& data)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

bool CommonAlg_AppendFile(const std::string& path, const std::string& data)
{
    FILE* f = fopen(path.c_str(), "ab");
    if (!f)
        return false;
    const bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

// ---------------- 路径操作 ----------------

bool CommonAlg_FileExists(const std::string& path)
{
#ifdef _WIN32
    const DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool CommonAlg_DirExists(const std::string& path)
{
#ifdef _WIN32
    const DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool CommonAlg_MakeDir(const std::string& path)
{
    if (path.empty())
        return false;
    if (CommonAlg_DirExists(path))
        return true;
    // 递归创建父目录
    const std::string p = NormalizeSeps(path);
    const size_t slash = p.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string parent = p.substr(0, slash);
        if (!CommonAlg_DirExists(parent) && !CommonAlg_MakeDir(parent))
            return false;
    }
#ifdef _WIN32
    return CreateDirectoryA(path.c_str(), nullptr) != 0 || CommonAlg_DirExists(path);
#else
    return mkdir(path.c_str(), 0755) == 0 || CommonAlg_DirExists(path);
#endif
}

std::string CommonAlg_FileName(const std::string& path)
{
    const std::string p = NormalizeSeps(path);
    const size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

std::string CommonAlg_FileStem(const std::string& path)
{
    const std::string name = CommonAlg_FileName(path);
    const size_t dot = name.find_last_of('.');
    return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}

std::string CommonAlg_DirName(const std::string& path)
{
    const std::string p = NormalizeSeps(path);
    const size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? std::string() : p.substr(0, slash);
}

std::string CommonAlg_Extension(const std::string& path)
{
    const std::string name = CommonAlg_FileName(path);
    const size_t dot = name.find_last_of('.');
    return (dot == std::string::npos || dot == 0) ? std::string() : name.substr(dot);
}

std::string CommonAlg_JoinPath(const std::string& dir, const std::string& name)
{
    if (dir.empty())
        return name;
    const char last = dir.back();
    if (last == '/' || last == '\\')
        return dir + name;
    return dir + "/" + name;
}

std::string CommonAlg_ReplaceExtension(const std::string& path, const std::string& ext)
{
    const std::string p = NormalizeSeps(path);
    const size_t slash = p.find_last_of('/');
    const size_t dot = p.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return p + ext;   // 无扩展名（或点在目录名里）→ 直接追加
    return p.substr(0, dot) + ext;
}
