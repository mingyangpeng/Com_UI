// CommonAlg_Json.cpp —— JSON 读取封装实现（接口见 CommonAlg_Json.h）。

#include "include/CommonAlg_Json.h"
#include "include/CommonAlg_FileUtil.h"   // 文件读写复用

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// ---------------- 编码检测与转换 ----------------

// ---------------- 编码转换 ----------------

// 是否合法 UTF-8（含 4 字节序列校验；BOM 已由调用方处理）
bool CommonAlg_IsValidUtf8(const std::string& s)
{
    const unsigned char* p = (const unsigned char*)s.data();
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t len;                 // 后续字节个数
        unsigned char lo;           // 首字节允许的最小值
        if (c >= 0xC2 && c <= 0xDF)          { len = 1; lo = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF)     { len = 2; lo = (c == 0xE0) ? 0xA0 : 0x80; }
        else if (c >= 0xF0 && c <= 0xF4)     { len = 3; lo = (c == 0xF0) ? 0x90 : 0x80; }
        else                                 return false;   // 0x80-0xC1 / 0xF5+ 非法
        if (i + len >= n)
            return false;           // 截断
        if (p[i + 1] < lo || p[i + 1] > 0xBF)
            return false;
        for (size_t k = 2; k <= len; ++k)
            if (p[i + k] < 0x80 || p[i + k] > 0xBF)
                return false;
        i += len + 1;
    }
    return true;
}

// ANSI（GBK/936 码页）→ UTF-8（Windows）；非 Windows 平台无转换，返回 false
bool CommonAlg_GbkToUtf8(const std::string& gbk, std::string& utf8)
{
#ifdef _WIN32
    const int wlen = MultiByteToWideChar(936, 0, gbk.data(), (int)gbk.size(), nullptr, 0);
    if (wlen <= 0)
        return false;
    std::wstring w((size_t)wlen, L'\0');
    MultiByteToWideChar(936, 0, gbk.data(), (int)gbk.size(), &w[0], wlen);
    const int ulen = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (ulen <= 0)
        return false;
    utf8.resize((size_t)ulen);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &utf8[0], ulen, nullptr, nullptr);
    return true;
#else
    (void)gbk; (void)utf8;
    return false;
#endif
}

// 自动检测编码（BOM / UTF-8 / GBK）并统一转 UTF-8
bool CommonAlg_ToUtf8(const std::string& bytes, std::string& utf8)
{
    if (bytes.size() >= 3 &&
        (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB &&
        (unsigned char)bytes[2] == 0xBF) {
        utf8 = bytes.substr(3);                     // UTF-8 BOM：去 BOM
        return true;
    }
    if (CommonAlg_IsValidUtf8(bytes)) {
        utf8 = bytes;                               // 合法 UTF-8：直接用
        return true;
    }
    return CommonAlg_GbkToUtf8(bytes, utf8);        // ANSI/GBK → UTF-8
}

bool JsonDoc::load(const char* path)
{
    std::string bytes;
    if (!CommonAlg_ReadFile(path ? path : "", bytes))
        return false;

    // ---- 编码检测 → 统一转成 UTF-8 ----
    std::string utf8;
    if (!CommonAlg_ToUtf8(bytes, utf8))
        return false;

    // ---- parse（异常关闭，失败返回 false）----
    try {
        root_ = nlohmann::json::parse(utf8, nullptr, false);
    } catch (...) {
        root_ = nlohmann::json::value_t::discarded;
    }
    return !root_.is_discarded();
}

JsonValue JsonDoc::operator[](const char* key) const
{
    if (root_.is_discarded() || !root_.is_object())
        return JsonValue(nullptr);
    const auto it = root_.find(key);
    if (it == root_.end())
        return JsonValue(nullptr);
    return JsonValue(&*it);
}

// ---------------- JsonValue ----------------

bool JsonValue::has() const
{
    return j_ && !j_->is_null();
}

double JsonValue::float_(double def) const
{
    if (!j_ || !j_->is_number())
        return def;
    return j_->get<double>();
}

std::string JsonValue::string_(const std::string& def) const
{
    if (!j_ || !j_->is_string())
        return def;
    return j_->get<std::string>();
}

std::vector<double> JsonValue::array() const
{
    std::vector<double> out;
    if (!j_ || !j_->is_array())
        return out;
    out.reserve(j_->size());
    // 非数值元素跳过（不填 0——0 在几何/测量数据里是合法值，
    // 静默填 0 会把解析错误伪装成真实数据）
    for (const auto& v : *j_)
        if (v.is_number())
            out.push_back(v.get<double>());
    return out;
}

std::vector<double> JsonValue::group(int idx) const
{
    // 二维数组的第 idx 组：等价于 at(idx).array()
    if (!j_ || !j_->is_array() || idx < 0 || (size_t)idx >= j_->size())
        return {};
    const nlohmann::json& sub = (*j_)[(size_t)idx];
    if (!sub.is_array())
        return {};
    std::vector<double> out;
    out.reserve(sub.size());
    for (const auto& v : sub)
        out.push_back(v.is_number() ? v.get<double>() : 0.0);
    return out;
}

JsonValue JsonValue::at(size_t i) const
{
    if (!j_ || !j_->is_array() || i >= j_->size())
        return JsonValue(nullptr);
    return JsonValue(&(*j_)[i]);
}

size_t JsonValue::size() const
{
    if (!j_ || !j_->is_array())
        return 0;
    return j_->size();
}
