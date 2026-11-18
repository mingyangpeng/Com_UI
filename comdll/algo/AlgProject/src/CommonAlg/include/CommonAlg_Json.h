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

class JsonValue;   // 前置声明（导出宏标注在下方定义处）

// ---------------- 编码转换 ----------------
/// 是否合法 UTF-8（含多字节序列校验；BOM 由调用方自行处理）。
/// [in] s  待检测字节串（ASCII 恒为合法）
/// 返回：true = 整串为合法 UTF-8。
COMMONALG_EXPORT bool CommonAlg_IsValidUtf8(const std::string& s);
/// GBK（Windows 936 码页）→ UTF-8。
/// [in]  gbk   输入字节串（按 GBK 解码）
/// [out] utf8  转换结果（成功时覆盖）
/// 返回：false = 非 Windows 平台或转换失败。
COMMONALG_EXPORT bool CommonAlg_GbkToUtf8(const std::string& gbk, std::string& utf8);
/// 自动检测编码并统一转 UTF-8：UTF-8 BOM → 去 BOM；合法 UTF-8 → 原样；
/// 其余（GBK 等 ANSI）→ 按 GBK 转 UTF-8。
/// [in]  bytes 原始字节串
/// [out] utf8  统一后的 UTF-8 结果
/// 返回：false = 转换失败（GBK 分支失败时）。
COMMONALG_EXPORT bool CommonAlg_ToUtf8(const std::string& bytes, std::string& utf8);

/// JSON 文档：load 一次，随后 operator[] 逐层取值（根为数组/对象皆可）。
/// 未 load 或 load 失败时为空文档——所有取值走缺键语义（返回默认值，不抛异常）。
class COMMONALG_EXPORT JsonDoc
{
public:
    /// 读文件 → 检测编码 → 转 UTF-8 → 解析（GBK 容错见文件头）。
    /// [in] path  文件路径；NULL 按空串处理
    /// 返回：false = 文件不存在或解析失败（此时文档保持空状态）。
    bool load(const char* path);

    /// 取 key 对应的 value（缺键返回空值，不抛异常；只读视图，生命周期同 JsonDoc）。
    JsonValue operator[](const char* key) const;

private:
    friend class JsonValue;
    nlohmann::json root_ = nlohmann::json::value_t::discarded;
};

// 一个 value 的视图：所有取值方法对缺键/类型不符都返回默认值，不抛异常
class COMMONALG_EXPORT JsonValue
{
public:
    bool has() const;                                        // 键存在且非 null
    double float_(double def = 0.0) const;                   // 数值（缺键/非数值返回 def）
    std::string string_(const std::string& def = "") const;  // 字符串（缺键/非字符串返回 def）
    /// 数值数组：非数值元素被跳过（不填 0——0 在几何数据里是合法值，
    /// 静默填 0 会把解析错误伪装成真实数据）。
    /// 缺键/非数组返回空。
    std::vector<double> array() const;
    std::vector<double> group(int idx) const;                // 二维数值数组的第 idx 组（越界/非数组返回空）
    JsonValue at(size_t i) const;                            // 数组的第 i 个元素（越界返回空值）
    size_t size() const;                                     // 数组元素个数（非数组返回 0）

private:
    friend class JsonDoc;
    explicit JsonValue(const nlohmann::json* j) : j_(j) {}
    const nlohmann::json* j_ = nullptr;   // 空指针 = 缺键/越界
};

#endif // COMMONALG_JSON_H
