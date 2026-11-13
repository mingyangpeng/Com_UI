#ifndef SHAPELY_CLOUD_IO_H
#define SHAPELY_CLOUD_IO_H

// shapely::CloudIO —— 点云文件读写（独立成类以隔离重型 IO 依赖）。
// 支持 PCD / PLY / TXT；模板显式实例化白名单同 CloudProcessor。
// shapely::MeshIO —— 三角网格（Mesh3d）文件读写，见文件尾。

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include "AlgExport.h"
#include "CloudProcessor.h"   // Mesh3d

#include <string>

namespace shapely {

template <typename PointT>
class SHAPELYALG_EXPORT CloudIO
{
public:
    CloudIO() = delete;   // 纯静态工具类

    using CloudPtr = typename pcl::PointCloud<PointT>::Ptr;

    // ---------------- PCD ----------------

    /// 读取 PCD 点云文件（PCL 原生格式）。
    /// [in] filename  文件路径
    /// 返回：点云指针；文件不存在/格式错误返回 nullptr。
    static CloudPtr ioLoadPCD(const std::string& filename);

    /// 保存 PCD（二进制格式，体积小读写快）。
    /// 返回：false = cloud 为空或写文件失败。
    static bool ioSavePCD(const std::string& filename, const CloudPtr& cloud);

    // ---------------- PLY ----------------

    /// 读取 PLY 点云文件（PCL 写出的 camera 元数据警告可忽略）。
    static CloudPtr ioLoadPLY(const std::string& filename);

    /// 保存 PLY。
    /// [in] is_binary  true=二进制（体积小），false=ASCII（可文本查看）
    static bool ioSavePLY(const std::string& filename, const CloudPtr& cloud, bool is_binary);

    // ---------------- TXT ----------------

    /// 读取文本点云（逐行解析数值）。
    /// [in] delimiter  字段分隔符（如 " " / "," / "\t"）；空串或 " " 按连续空白切分
    /// [in] col_x/col_y/col_z  X/Y/Z 所在列号（0 起），默认前 3 列；
    ///                         列数不足 / 非数字列 / NaN·Inf 行整行跳过；'#' 开头行跳过
    /// 返回：点云指针；文件打不开或无有效数据行返回 nullptr。
    static CloudPtr ioLoadTXT(const std::string& filename, const std::string& delimiter,
                              int col_x = 0, int col_y = 1, int col_z = 2);

    /// 保存文本点云：每行 "x<delim>y<delim>z\n"。
    /// [in] delimiter  分隔符；空串默认为空格
    /// 精度：9 位有效数字（float 往返无损）。
    /// 返回：false = cloud 为空 / 文件打不开 / 写入失败。
    static bool ioSaveTXT(const std::string& filename, const CloudPtr& cloud,
                          const std::string& delimiter);
};

// =====================================================================
// MeshIO：三角网格（Mesh3d）文件读写 —— 表面重建结果的落盘与交换
// =====================================================================

class SHAPELYALG_EXPORT MeshIO
{
public:
    MeshIO() = delete;   // 纯静态工具类

    /// 保存三角网格为 OBJ（ASCII 文本，通用交换格式，CloudCompare/MeshLab 可读）。
    /// 格式："v x y z" 顶点行（1 索引）+ "f i j k" 三角面行。
    /// 返回：false = mesh 无效 / 无顶点 / 文件打不开 / 写入失败 / 面索引越界。
    static bool saveOBJ(const std::string& filename, const Mesh3d& mesh);

    /// 读取 OBJ 三角网格。
    /// 支持：v 行、f 行（v、v/vt、v/vt/vn、v//vn 四种索引写法；负索引=从尾部倒数，
    /// OBJ 规范）；'#' 注释行跳过；>3 顶点的面扇形三角化（同 Mesh3d 重建约定）；
    /// 索引越界/非数字的 f 行整行丢弃。
    /// 返回：mesh.valid=false = 文件打不开 / 无有效顶点 / 无有效三角面。
    static Mesh3d loadOBJ(const std::string& filename);

    /// 保存 STL（is_binary=true 默认二进制，体积小读写快；false = ASCII）。
    /// 面法向自动计算（叉积单位化，退化面写 0,0,0）。
    /// 返回：false = mesh 无效 / 无面 / 文件打不开 / 写入失败 / 面索引越界。
    static bool saveSTL(const std::string& filename, const Mesh3d& mesh, bool is_binary = true);

    /// 读取 STL（自动识别二进制/ASCII：按文件长度与 84+50n 匹配判定）。
    /// 二进制坐标为 float32（double→float 往返有 ~1e-6 相对损失，属格式固有）。
    /// 返回：mesh.valid=false = 文件打不开 / 无有效三角面。
    static Mesh3d loadSTL(const std::string& filename);
};

// =====================================================================
// HeightMapIO：高度图磁盘序列化（简单二进制格式）
// 布局：magic "SHMAP1\0"(8B) + int32 width/height + double x0/y0/cell
//       + float32 cells[width*height] + uint8 mask[width*height]（小端，x86）
// =====================================================================

class SHAPELYALG_EXPORT HeightMapIO
{
public:
    HeightMapIO() = delete;   // 纯静态工具类

    /// 保存高度图（布局见上）。
    /// 返回：false = hm invalid / 空图 / 文件打不开 / 写入失败。
    static bool save(const std::string& filename, const HeightMap& hm);

    /// 读取高度图（字段全量恢复，含 mask 与 NaN 空格）。
    /// 返回：valid=false = 打不开 / 魔数不符 / 长度不符。
    static HeightMap load(const std::string& filename);
};

} // namespace shapely

#endif // SHAPELY_CLOUD_IO_H
