#ifndef COMMONALG_JSON_H
#define COMMONALG_JSON_H

// CommonAlg_Json —— JSON 读取封装（内部 nlohmann::json，见 thrid/nlohmann_json）。
// 用法：
//   JsonDoc doc;
//   if (!doc.load(path)) ...;                  // 文件不存在
//   double v = doc["scanpulses"].float_(0.0);   // 缺键/类型不符返回默认值
//   std::string s = doc["LensRatio"].string_(); // 缺键返回默认值
//   std::vector<double> a = doc["scaleXYZ"].array();
//   auto g = doc["compensate_All"].group(1);    // 二维数组第 1 组
//   auto row = doc["Coordinate 3D"].at(i);      // 数组第 i 个元素（越界为空值）
//   doc["Coordinate 3D"].size();
//
// GBK 容错：nlohmann 要求严格 UTF-8，而 Juxin 配置文件可能是 GBK 编码
// （如 "LensRatio 镜头倍率 5x 10x" 中文注释键）。load 时先检测编码：
//   1. UTF-8 BOM → 去 BOM 直接 parse；
//   2. 合法 UTF-8 → 直接 parse；
//   3. 其他（GBK 等 ANSI）→ 按 Windows 936 码页转 UTF-8 再 parse。
// 转换后中文键名正确还原（仅注释用途，不影响按键取值）。

#include "AlgExport.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

class COMMONALG_EXPORT JsonValue;

// ---------------- 编码转换 ----------------
// 是否合法 UTF-8（含多字节序列校验；BOM 由调用方自行处理）
COMMONALG_EXPORT bool CommonAlg_IsValidUtf8(const std::string& s);
// Windows 936 码页（GBK）→ UTF-8；非 Windows 平台返回 false
COMMONALG_EXPORT bool CommonAlg_GbkToUtf8(const std::string& gbk, std::string& utf8);
// 自动检测编码（BOM / UTF-8 / GBK）并统一转 UTF-8
COMMONALG_EXPORT bool CommonAlg_ToUtf8(const std::string& bytes, std::string& utf8);

class COMMONALG_EXPORT JsonDoc
{
public:
    // 读文件 → 检测编码 → 转 UTF-8 → 解析；文件不存在或解析失败返回 false
    bool load(const char* path);

    // 取 key 的 value（缺键返回空值，不抛异常）
    JsonValue operator[](const char* key) const;

private:
    friend class JsonValue;
    nlohmann::json root_ = nlohmann::json::value_t::discarded;
};

// 一个 value 的视图：所有取值方法对缺键/类型不符都返回默认值，不抛异常
class JsonValue
{
public:
    bool has() const;                                        // 键存在且非 null
    double float_(double def = 0.0) const;                   // 数值
    std::string string_(const std::string& def = "") const;  // 字符串
    /// 数值数组：非数值元素被跳过（不填 0——0 在几何数据里是合法值，
    /// 静默填 0 会把解析错误伪装成真实数据）。
    std::vector<double> array() const;
    std::vector<double> group(int idx) const;                // 二维数值数组的第 idx 组
    JsonValue at(size_t i) const;                            // 数组的第 i 个元素
    size_t size() const;                                     // 数组元素个数

private:
    friend class JsonDoc;
    explicit JsonValue(const nlohmann::json* j) : j_(j) {}
    const nlohmann::json* j_ = nullptr;   // 空指针 = 缺键/越界
};

#endif // COMMONALG_JSON_H
