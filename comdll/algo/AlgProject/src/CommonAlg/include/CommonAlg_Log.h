#ifndef COMMONALG_LOG_H
#define COMMONALG_LOG_H

// CommonAlg_Log —— 轻量日志（分级 + 时间戳 + 可选文件输出）。
// 设计：无第三方依赖；线程安全（内部互斥）；格式固定便于 grep/滚动。
//
// 用法：
//   CommonAlg_LogSetFile("D:/logs/app.log");     // 可选：同时写文件
//   CommonAlg_LogSetLevel(CommonAlg_LogLevel::Info);
//   LOG_INFO("加载完成，共 %d 个文件", n);        // printf 风格可变参
//   LOG_WARN("跳过早退：%s", reason.c_str());
//   LOG_ERROR("打开失败: %s", path);
//
// 说明：
//   - 低于当前级别的日志直接丢弃（零格式化开销，先判级再格式化的宏实现）
//   - 输出格式：[2026-11-05 14:03:22.123][INFO ][文件:行] 消息
//   - 未调用 SetFile 时只输出到 stderr
//   - 日志级别默认 Info

#include "AlgExport.h"

#include <string>

enum class CommonAlg_LogLevel {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Off   = 5,   // 关闭全部日志
};

// ---------------- 配置 ----------------

/// 设置最低输出级别（低于该级别的日志被丢弃）。
COMMONALG_EXPORT void CommonAlg_LogSetLevel(CommonAlg_LogLevel level);

/// 当前级别。
COMMONALG_EXPORT CommonAlg_LogLevel CommonAlg_LogGetLevel();

/// 设置日志文件路径（追加写；目录需已存在）。传空串关闭文件输出。
/// 返回 false 表示文件打不开（仍会输出到 stderr）。
COMMONALG_EXPORT bool CommonAlg_LogSetFile(const std::string& path);

/// 关闭日志文件（之后只输出 stderr）。
COMMONALG_EXPORT void CommonAlg_LogCloseFile();

// ---------------- 输出（一般用下面的宏，不直接调用） ----------------

/// 写一条日志。
/// [in] level  级别
/// [in] file   源文件名（宏自动传 __FILE__）
/// [in] line   行号（宏自动传 __LINE__）
/// [in] fmt    printf 风格格式串
COMMONALG_EXPORT void CommonAlg_LogWrite(CommonAlg_LogLevel level, const char* file, int line,
                        const char* fmt, ...);

// ---------------- 便捷宏（先判级，避免无谓格式化） ----------------

#define COMMONALG_LOG_AT(lvl, ...)                                            \
    do {                                                                       \
        if ((int)(lvl) >= (int)CommonAlg_LogGetLevel())                        \
            CommonAlg_LogWrite((lvl), __FILE__, __LINE__, __VA_ARGS__);        \
    } while (0)

#define LOG_TRACE(...) COMMONALG_LOG_AT(CommonAlg_LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) COMMONALG_LOG_AT(CommonAlg_LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)  COMMONALG_LOG_AT(CommonAlg_LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(...)  COMMONALG_LOG_AT(CommonAlg_LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) COMMONALG_LOG_AT(CommonAlg_LogLevel::Error, __VA_ARGS__)

// 带模块前缀的别名（推荐使用：LOG_INFO 这类短名容易与 glog/spdlog/Qt 冲突，
// 若项目已引入同类日志库，请改用下面这组）
#define CALG_LOG_TRACE(...) LOG_TRACE(__VA_ARGS__)
#define CALG_LOG_DEBUG(...) LOG_DEBUG(__VA_ARGS__)
#define CALG_LOG_INFO(...)  LOG_INFO(__VA_ARGS__)
#define CALG_LOG_WARN(...)  LOG_WARN(__VA_ARGS__)
#define CALG_LOG_ERROR(...) LOG_ERROR(__VA_ARGS__)

#endif // COMMONALG_LOG_H
