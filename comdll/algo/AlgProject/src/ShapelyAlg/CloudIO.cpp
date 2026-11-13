// CloudIO.cpp —— 点云文件读写（PCD/PLY/TXT）。
// 重型 IO 头只在 .cpp include；显式实例化白名单同 CloudProcessor。
// Boost 1.68 + MSVC 2022 兼容垫片见 BoostCompat.h（include pcl/io 前必须先引入）。

#include "BoostCompat.h"

#include "CloudIO.h"

#include <pcl/io/pcd_io.h>
#include <pcl/io/ply_io.h>

//#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace shapely {

// ---------------- PCD ----------------

template <typename PointT>
typename CloudIO<PointT>::CloudPtr CloudIO<PointT>::ioLoadPCD(const std::string& filename)
{
    CloudPtr out(new pcl::PointCloud<PointT>);
    if (pcl::io::loadPCDFile(filename, *out) < 0)
        return nullptr;
    return out;
}

template <typename PointT>
bool CloudIO<PointT>::ioSavePCD(const std::string& filename, const CloudPtr& cloud)
{
    if (!cloud)
        return false;
    return pcl::io::savePCDFileBinary(filename, *cloud) == 0;
}

// ---------------- PLY ----------------

template <typename PointT>
typename CloudIO<PointT>::CloudPtr CloudIO<PointT>::ioLoadPLY(const std::string& filename)
{
    CloudPtr out(new pcl::PointCloud<PointT>);
    if (pcl::io::loadPLYFile(filename, *out) < 0)
        return nullptr;
    return out;
}

template <typename PointT>
bool CloudIO<PointT>::ioSavePLY(const std::string& filename, const CloudPtr& cloud, bool is_binary)
{
    if (!cloud)
        return false;
    if (is_binary)
        return pcl::io::savePLYFileBinary(filename, *cloud) == 0;
    return pcl::io::savePLYFileASCII(filename, *cloud) == 0;
}

// ---------------- TXT ----------------

template <typename PointT>
typename CloudIO<PointT>::CloudPtr CloudIO<PointT>::ioLoadTXT(const std::string& filename,
                                                             const std::string& delimiter,
                                                             int col_x, int col_y, int col_z)
{
    // 负列号会在 tok[(size_t)col] 处越界，直接拒绝
    if (col_x < 0 || col_y < 0 || col_z < 0)
        return nullptr;
    std::ifstream f(filename);
    if (!f)
        return nullptr;
    CloudPtr out(new pcl::PointCloud<PointT>);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        // 分隔符切分（空格分隔时连续空格视为一个）
        std::vector<std::string> tok;
        if (delimiter == " " || delimiter.empty()) {
            std::istringstream ss(line);
            std::string t;
            while (ss >> t)
                tok.push_back(t);
        } else {
            size_t a = 0;
            while (true) {
                const size_t b = line.find(delimiter, a);
                tok.push_back(line.substr(a, (b == std::string::npos) ? std::string::npos : b - a));
                if (b == std::string::npos)
                    break;
                a = b + delimiter.size();
            }
        }
        const int need = std::max(col_x, std::max(col_y, col_z)) + 1;
        if ((int)tok.size() < need)
            continue;
        PointT p{};   // 值初始化：PointXYZI/XYZRGB 的 intensity/rgb 不能留未初始化内存
        // strtod + endptr 全串校验：非数字（表头/坏行）或 NaN/Inf 整行跳过——
        // 原 atof 把表头静默解析成 (0,0,0) 假点，污染 AABB/质心/高度统计
        char* end = nullptr;
        const double x = std::strtod(tok[(size_t)col_x].c_str(), &end);
        if (end == tok[(size_t)col_x].c_str() || *end != '\0')
            continue;
        const double y = std::strtod(tok[(size_t)col_y].c_str(), &end);
        if (end == tok[(size_t)col_y].c_str() || *end != '\0')
            continue;
        const double z = std::strtod(tok[(size_t)col_z].c_str(), &end);
        if (end == tok[(size_t)col_z].c_str() || *end != '\0')
            continue;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            continue;   // "nan"/"inf" 行跳过
        p.x = (float)x;
        p.y = (float)y;
        p.z = (float)z;
        out->push_back(p);
    }
    // 无有效数据行视为加载失败（与 ioLoadPCD/PLY 的失败语义一致）
    if (out->empty())
        return nullptr;
    return out;
}

template <typename PointT>
bool CloudIO<PointT>::ioSaveTXT(const std::string& filename, const CloudPtr& cloud,
                                const std::string& delimiter)
{
    if (!cloud)
        return false;
    std::ofstream f(filename);
    if (!f)
        return false;
    const std::string& d = delimiter.empty() ? " " : delimiter;
    // 9 位有效数字（float 的 max_digits10）：坐标往返无损；默认 6 位在 ≥1e4 量级丢亚毫米精度
    f << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (const PointT& p : cloud->points)
        f << p.x << d << p.y << d << p.z << "\n";
    return f.good();   // 写盘失败（磁盘满等）不再静默返回 true
}

// =====================================================================
// MeshIO：三角网格 OBJ 读写（纯文本解析，零 PCL 依赖）
// =====================================================================

bool MeshIO::saveOBJ(const std::string& filename, const Mesh3d& mesh)
{
    if (!mesh.valid || mesh.vertices.empty())
        return false;
    std::ofstream f(filename);
    if (!f)
        return false;
    // 17 位有效数字：double 坐标往返无损
    f << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (const Point3d& v : mesh.vertices)
        f << "v " << v.x << ' ' << v.y << ' ' << v.z << "\n";
    const int nv = (int)mesh.vertices.size();
    for (const std::array<int, 3>& t : mesh.triangles) {
        // 面索引越界 = 数据损坏：拒绝写出（不静默截断）
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 ||
            t[0] >= nv || t[1] >= nv || t[2] >= nv)
            return false;
        f << "f " << t[0] + 1 << ' ' << t[1] + 1 << ' ' << t[2] + 1 << "\n";
    }
    return f.good();
}

Mesh3d MeshIO::loadOBJ(const std::string& filename)
{
    Mesh3d out;
    std::ifstream f(filename);
    if (!f)
        return out;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();   // Windows 行尾残留
        std::istringstream ss(line);
        std::string tag;
        if (!(ss >> tag) || tag == "#")
            continue;
        if (tag == "v") {
            double x = 0, y = 0, z = 0;
            if (ss >> x >> y >> z)
                out.vertices.push_back(Point3d{ x, y, z });
        } else if (tag == "f") {
            // f 支持 v / v/vt / v/vt/vn / v//vn：取第一个 '/' 前的顶点索引
            std::vector<int> idx;
            std::string tok;
            while (ss >> tok) {
                const size_t slash = tok.find('/');
                if (slash != std::string::npos)
                    tok.resize(slash);
                char* end = nullptr;
                const long v = std::strtol(tok.c_str(), &end, 10);
                if (end == tok.c_str() || *end != '\0')
                    continue;   // 非数字 token 跳过
                idx.push_back((int)v);
            }
            if (idx.size() < 3)
                continue;
            // 负索引 = 从尾部倒数（OBJ 规范）；0 = 非法
            const int nv = (int)out.vertices.size();
            bool bad = false;
            for (int& v : idx) {
                v = (v > 0) ? v - 1 : nv + v;
                if (v < 0 || v >= nv) {
                    bad = true;   // 索引越界：整行丢弃
                    break;
                }
            }
            if (bad)
                continue;
            // >3 顶点的面扇形三角化（同 meshFromPolygonMesh 约定）
            for (size_t k = 1; k + 1 < idx.size(); ++k)
                out.triangles.push_back({ idx[0], idx[k], idx[k + 1] });
        }
        // 其余 tag（vn/vt/o/g/s/usemtl/...）忽略
    }
    out.valid = !out.vertices.empty() && !out.triangles.empty();
    return out;
}

// =====================================================================
// MeshIO：STL 读写（二进制 + ASCII，加载自动识别）
// =====================================================================

// 面法向：叉积单位化，退化面返回 (0,0,0)
static void triNormal(const Point3d& a, const Point3d& b, const Point3d& c,
                      double out_n[3])
{
    const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    double nx = uy * vz - uz * vy;
    double ny = uz * vx - ux * vz;
    double nz = ux * vy - uy * vx;
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-12) {
        out_n[0] = out_n[1] = out_n[2] = 0.0;
        return;
    }
    out_n[0] = nx / len;
    out_n[1] = ny / len;
    out_n[2] = nz / len;
}

bool MeshIO::saveSTL(const std::string& filename, const Mesh3d& mesh, bool is_binary)
{
    if (!mesh.valid || mesh.triangles.empty())
        return false;
    const int nv = (int)mesh.vertices.size();
    for (const std::array<int, 3>& t : mesh.triangles)
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= nv || t[1] >= nv || t[2] >= nv)
            return false;   // 面索引越界：拒绝写出（同 saveOBJ 口径）

    if (is_binary) {
        std::ofstream f(filename, std::ios::binary);
        if (!f)
            return false;
        char header[80] = { 0 };
        const char tag[] = "shapely Mesh3d binary STL";
        std::memcpy(header, tag, sizeof(tag) - 1);
        f.write(header, 80);
        const uint32_t count = (uint32_t)mesh.triangles.size();
        f.write(reinterpret_cast<const char*>(&count), 4);
        for (const std::array<int, 3>& t : mesh.triangles) {
            const Point3d& a = mesh.vertices[(size_t)t[0]];
            const Point3d& b = mesh.vertices[(size_t)t[1]];
            const Point3d& c = mesh.vertices[(size_t)t[2]];
            double n[3];
            triNormal(a, b, c, n);
            const float rec[12] = {
                (float)n[0], (float)n[1], (float)n[2],
                (float)a.x, (float)a.y, (float)a.z,
                (float)b.x, (float)b.y, (float)b.z,
                (float)c.x, (float)c.y, (float)c.z,
            };
            f.write(reinterpret_cast<const char*>(rec), 48);
            const uint16_t attr = 0;
            f.write(reinterpret_cast<const char*>(&attr), 2);
        }
        return f.good();
    }

    std::ofstream f(filename);
    if (!f)
        return false;
    f << std::setprecision(std::numeric_limits<double>::max_digits10);
    f << "solid shapely\n";
    for (const std::array<int, 3>& t : mesh.triangles) {
        const Point3d& a = mesh.vertices[(size_t)t[0]];
        const Point3d& b = mesh.vertices[(size_t)t[1]];
        const Point3d& c = mesh.vertices[(size_t)t[2]];
        double n[3];
        triNormal(a, b, c, n);
        f << "  facet normal " << n[0] << ' ' << n[1] << ' ' << n[2] << "\n"
          << "    outer loop\n"
          << "      vertex " << a.x << ' ' << a.y << ' ' << a.z << "\n"
          << "      vertex " << b.x << ' ' << b.y << ' ' << b.z << "\n"
          << "      vertex " << c.x << ' ' << c.y << ' ' << c.z << "\n"
          << "    endloop\n"
          << "  endfacet\n";
    }
    f << "endsolid shapely\n";
    return f.good();
}

Mesh3d MeshIO::loadSTL(const std::string& filename)
{
    Mesh3d out;
    std::ifstream f(filename, std::ios::binary);
    if (!f)
        return out;
    std::vector<char> data((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    if (data.size() < 15)
        return out;

    // 格式判定：二进制固定长 84 + 50n；"solid" 开头且长度不匹配 → ASCII
    //（二进制文件头也可能是 "solid"——长度匹配优先判二进制）
    auto binaryCount = [&data]() -> long {
        if (data.size() < 84)
            return -1;
        uint32_t n = 0;
        std::memcpy(&n, data.data() + 80, 4);   // x86 小端，STL 规范即小端
        const long count = (long)n;
        return (size_t)84 + (size_t)count * 50 == data.size() ? count : -1;
    }();

    if (binaryCount >= 0) {
        out.vertices.reserve((size_t)binaryCount * 3);
        for (long i = 0; i < binaryCount; ++i) {
            const char* rec = data.data() + 84 + (size_t)i * 50;
            const float* v = reinterpret_cast<const float*>(rec + 12);   // 跳过 12 字节法向
            for (int k = 0; k < 3; ++k)
                out.vertices.push_back(Point3d{ (double)v[k * 3],
                                                (double)v[k * 3 + 1],
                                                (double)v[k * 3 + 2] });
            const int base = (int)i * 3;
            out.triangles.push_back({ base, base + 1, base + 2 });
        }
    } else {
        // ASCII：收集全部 vertex 行，每连续 3 个为一面（STL 规范固定 3 顶点/面）
        std::istringstream ss(std::string(data.begin(), data.end()));
        std::string line;
        std::vector<Point3d> verts;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::istringstream ls(line);
            std::string tag;
            if (!(ls >> tag) || tag != "vertex")
                continue;
            double x = 0, y = 0, z = 0;
            if (ls >> x >> y >> z)
                verts.push_back(Point3d{ x, y, z });
        }
        if (verts.size() % 3 != 0)
            verts.resize(verts.size() - verts.size() % 3);   // 残缺尾面丢弃
        out.vertices = std::move(verts);
        out.triangles.reserve(out.vertices.size() / 3);
        for (size_t i = 0; i + 2 < out.vertices.size(); i += 3)
            out.triangles.push_back({ (int)i, (int)i + 1, (int)i + 2 });
    }
    out.valid = !out.vertices.empty() && !out.triangles.empty();
    return out;
}

// =====================================================================
// HeightMapIO：高度图序列化（magic "SHMAP1" + 元数据 + cells/mask）
// =====================================================================

bool HeightMapIO::save(const std::string& filename, const HeightMap& hm)
{
    if (!hm.valid || hm.width <= 0 || hm.height <= 0 ||
        hm.cells.size() != (size_t)hm.width * (size_t)hm.height ||
        hm.mask.size() != hm.cells.size())
        return false;
    std::ofstream f(filename, std::ios::binary);
    if (!f)
        return false;
    const char magic[8] = { 'S', 'H', 'M', 'A', 'P', '1', '\0', '\0' };
    f.write(magic, 8);
    const int32_t dims[2] = { hm.width, hm.height };
    f.write(reinterpret_cast<const char*>(dims), 8);
    const double meta[3] = { hm.x0, hm.y0, hm.cell };
    f.write(reinterpret_cast<const char*>(meta), 24);
    f.write(reinterpret_cast<const char*>(hm.cells.data()),
            (std::streamsize)(hm.cells.size() * sizeof(float)));
    f.write(reinterpret_cast<const char*>(hm.mask.data()),
            (std::streamsize)hm.mask.size());
    return f.good();
}

HeightMap HeightMapIO::load(const std::string& filename)
{
    HeightMap hm;
    std::ifstream f(filename, std::ios::binary);
    if (!f)
        return hm;
    char magic[8] = { 0 };
    f.read(magic, 8);
    if (!f.good() || std::memcmp(magic, "SHMAP1", 6) != 0)
        return HeightMap();   // 魔数不符
    int32_t dims[2] = { 0, 0 };
    f.read(reinterpret_cast<char*>(dims), 8);
    double meta[3] = { 0, 0, 0 };
    f.read(reinterpret_cast<char*>(meta), 24);
    if (!f.good())
        return HeightMap();
    const int w = dims[0], hgt = dims[1];
    if (w <= 0 || hgt <= 0 || (double)w * (double)hgt > 20000000.0)
        return HeightMap();
    const size_t n = (size_t)w * (size_t)hgt;
    hm.width = w;
    hm.height = hgt;
    hm.x0 = meta[0];
    hm.y0 = meta[1];
    hm.cell = meta[2];
    hm.cells.resize(n);
    hm.mask.resize(n);
    f.read(reinterpret_cast<char*>(hm.cells.data()),
           (std::streamsize)(n * sizeof(float)));
    f.read(reinterpret_cast<char*>(hm.mask.data()), (std::streamsize)n);
    if (!f.good())
        return HeightMap();   // 长度不符/截断
    hm.valid = true;
    return hm;
}

// =====================================================================
// 显式实例化白名单
// =====================================================================

template class CloudIO<pcl::PointXYZ>;
template class CloudIO<pcl::PointXYZI>;
template class CloudIO<pcl::PointXYZRGB>;

} // namespace shapely
