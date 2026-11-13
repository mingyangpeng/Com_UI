// CommonAlg_Log.cpp —— 轻量日志实现（分级 + 时间戳 + 可选文件）。
// 线程安全：内部互斥保护级别/文件句柄/写操作。

#include "include/CommonAlg_Log.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace {

// 级别用 atomic：日志宏每次调用都读级别，必须无锁
std::atomic<int>    g_level { (int)CommonAlg_LogLevel::Info };
std::mutex          g_mutex;      // 保护 g_file / g_filePath / 写操作
FILE*               g_file = nullptr;
std::string         g_filePath;

const char* LevelName(CommonAlg_LogLevel lv)
{
    switch (lv) {
    case CommonAlg_LogLevel::Trace: return "TRACE";
    case CommonAlg_LogLevel::Debug: return "DEBUG";
    case CommonAlg_LogLevel::Info:  return "INFO ";
    case CommonAlg_LogLevel::Warn:  return "WARN ";
    case CommonAlg_LogLevel::Error: return "ERROR";
    default:                        return "OFF  ";
    }
}

// 当前本地时间，格式 "2026-11-05 14:03:22.123"
void NowStamp(char* buf, size_t cap)
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)ms);
}

// 取文件名部分（去掉目录，日志里只显示 a.cpp:123 更紧凑）
const char* ShortFile(const char* path)
{
    if (!path)
        return "?";
    const char* p = path;
    for (const char* q = path; *q; ++q)
        if (*q == '/' || *q == '\\')
            p = q + 1;
    return p;
}

} // namespace

// ---------------- 配置 ----------------

void CommonAlg_LogSetLevel(CommonAlg_LogLevel level)
{
    g_level.store((int)level, std::memory_order_relaxed);
}

CommonAlg_LogLevel CommonAlg_LogGetLevel()
{
    // 无锁读：日志宏热路径，避免每行日志都抢 mutex
    return (CommonAlg_LogLevel)g_level.load(std::memory_order_relaxed);
}

bool CommonAlg_LogSetFile(const std::string& path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_filePath.clear();
    if (path.empty())
        return true;

    // 目录由调用方保证（不自动创建，避免掩盖路径配置错误）
    g_file = fopen(path.c_str(), "ab");   // 追加写
    if (!g_file)
        return false;
    g_filePath = path;
    return true;
}

void CommonAlg_LogCloseFile()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_filePath.clear();
}

// ---------------- 输出 ----------------

void CommonAlg_LogWrite(CommonAlg_LogLevel level, const char* file, int line,
                        const char* fmt, ...)
{
    char stamp[32];
    NowStamp(stamp, sizeof(stamp));

    // 先格式化消息体
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char full[2304];
    snprintf(full, sizeof(full), "[%s][%s][%s:%d] %s\n",
             stamp, LevelName(level), ShortFile(file), line, msg);

    std::lock_guard<std::mutex> lock(g_mutex);
    // GUI 宿主（无控制台）下 stderr 可能为 NULL，直接 fputs 会崩
    if (FILE* e = stderr)
        fputs(full, e);
    if (g_file) {
        fputs(full, g_file);
        fflush(g_file);   // 立即落盘：崩溃时日志不丢
    }
}
