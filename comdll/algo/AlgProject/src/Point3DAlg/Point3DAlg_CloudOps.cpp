// Point3DAlg_CloudOps.cpp
// 点云算子 C 封装（1.1.0 新增）：LoadCloud / SaveCloudEx / FilterCloud /
// ExtractCloud / TransformCloud。
// 1.2.0 追加（P1+P2 批次）：FitModel / SegmentCloud / KnnSearch /
// RadiusSearch / MeasureCloud / EstimateNormals / ComputeFeature /
// DetectKeypoints / ReconstructSurface / MeshOp / CloudToHeightMap /
// HeightMapToCloud / ThicknessMap + FreeClouds / FreeSearchResult /
// FreeMesh / FreeHeightMap。底层走 ShapelyAlg::CloudIO / CloudProcessor
// （点型固定 pcl::PointXYZ，显式实例化白名单），本层只做容器转换
// （SoA float 数组 ↔ pcl::PointCloud<pcl::PointXYZ>）与参数/错误码翻译。
// 两处经登记的组合管线（非新算法）：
//   * 表面重建（type 3~7）内部"KNN 估法线 + MST 朝向一致化"后再进底层
//     （§9.12 强制约束：Poisson 前必须统一法线朝向；SoA 容器无法传法线）；
//   * 凸包/凹包（type 0/1）直接调 PCL ConvexHull/ConcaveHull 取回面片
//     ——CloudProcessor 同名算子只回顶点云，无法满足 Mesh 出参契约。
//
// 依赖与守卫：本文件依赖 PCL 头与 ShapelyAlg（CloudProcessor 显式实例化），
// 由 CMakeLists.txt 按 PCL_FOUND 条件编入（与 tools/ 配准层同策略）——
// 无 PCL 构建时本文件不编译，上述符号不导出（见 Point3DAlg_api.h 守卫说明）。
//
// 错误码口径（本文件全部接口）：0 成功；-1 参数/空指针（含空点云输入）；
// -2 空结果/退化（含文件打不开/写失败）；-3 参数越界（method 枚举、params
// 数量与取值、索引越界）；-4 内存分配失败；-99 PCL 异常（C 边界兜底）。
// 与 params 布局表逐 method 的语义见 Point3DAlg_api.h 注释（权威口径）。

#include "include/Point3DAlg_api.h"

#include "CloudIO.h"
#include "CloudProcessor.h"

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <pcl/surface/convex_hull.h>
#include <pcl/surface/concave_hull.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using CloudXYZ = pcl::PointCloud<pcl::PointXYZ>;
using CloudPtr = CloudXYZ::Ptr;
using CP = shapely::CloudProcessor<pcl::PointXYZ>;
using CIO = shapely::CloudIO<pcl::PointXYZ>;

// SoA float 数组 → PCL 点云（整段复制；NaN/Inf 原样保留，is_dense=false）
CloudPtr ToPcl(const Point3DAlg_PointCloud* in)
{
    CloudPtr c(new CloudXYZ);
    c->reserve((size_t)in->count);
    for (int i = 0; i < in->count; ++i)
        c->push_back(pcl::PointXYZ(in->xyz[(size_t)i * 3 + 0],
                                   in->xyz[(size_t)i * 3 + 1],
                                   in->xyz[(size_t)i * 3 + 2]));
    c->width = (uint32_t)c->size();
    c->height = 1;
    c->is_dense = false;
    return c;
}

// PCL 点云 → SoA float 数组（malloc；失败返回 -4，out 保持已清零状态）。
// 空云（n=0）直接输出 {NULL,0} 而非 malloc(0)：malloc(0) 是否返回非空指针
// 平台相关，若返回 NULL 会被误判成 -4——空段是 SegmentCloud"恒 2 段"契约的
// 合法输出，不能让内存分配的口癖影响它。
int FromPcl(const CloudPtr& cloud, Point3DAlg_PointCloud* out)
{
    const size_t n = cloud->size();
    if (n == 0) {
        out->xyz = nullptr;
        out->count = 0;
        return 0;
    }
    float* xyz = (float*)malloc(n * 3 * sizeof(float));
    if (!xyz)
        return -4;
    for (size_t i = 0; i < n; ++i) {
        xyz[i * 3 + 0] = (*cloud)[i].x;
        xyz[i * 3 + 1] = (*cloud)[i].y;
        xyz[i * 3 + 2] = (*cloud)[i].z;
    }
    out->xyz = xyz;
    out->count = (int)n;
    return 0;
}

// 出参容器统一"先清零再填充"（见 api.h 容器注释）
void ResetOut(Point3DAlg_PointCloud* out)
{
    out->xyz = nullptr;
    out->count = 0;
}

// 输入容器基础校验：-1 = 参数/空指针/空点云/in 与 out 同指针（出参清零会
// 破坏输入，禁止别名传入）
int CheckIn(const Point3DAlg_PointCloud* in, Point3DAlg_PointCloud* out)
{
    if (!in || !out || in == out || !in->xyz || in->count <= 0)
        return -1;
    ResetOut(out);
    return 0;
}

// params 取整数参数（四舍五入）；越界语义由调用方各自校验
int ParamInt(const double* p, int i)
{
    return (int)std::lround(p[i]);
}

// ---------------- 1.2.0 P1/P2 容器转换辅助 ----------------

// Mesh 出参统一"先清零再填充"（同 ResetOut 契约）
void ResetMesh(Point3DAlg_Mesh* m)
{
    m->vertices = nullptr;
    m->vertex_count = 0;
    m->faces = nullptr;
    m->face_count = 0;
}

// HeightMap 出参统一"先清零再填充"
void ResetHm(Point3DAlg_HeightMap* h)
{
    h->width = 0;
    h->height = 0;
    h->x0 = 0.0;
    h->y0 = 0.0;
    h->cell = 0.0;
    h->cells = nullptr;
    h->mask = nullptr;
    h->valid = 0;
}

// shapely::Mesh3d → C Mesh（double 顶点转 float；三角面摊平 3F）。
// 返回 0 成功；-4 malloc 失败（out 保持已清零状态）。
int MeshToC(const shapely::Mesh3d& mesh, Point3DAlg_Mesh* out)
{
    const size_t nv = mesh.vertices.size();
    const size_t nf = mesh.triangles.size();
    float* verts = (float*)malloc(nv * 3 * sizeof(float));
    int* faces = (int*)malloc(nf * 3 * sizeof(int));
    if ((!verts && nv > 0) || (!faces && nf > 0)) {
        free(verts);
        free(faces);
        return -4;
    }
    for (size_t i = 0; i < nv; ++i) {
        verts[i * 3 + 0] = (float)mesh.vertices[i].x;
        verts[i * 3 + 1] = (float)mesh.vertices[i].y;
        verts[i * 3 + 2] = (float)mesh.vertices[i].z;
    }
    for (size_t i = 0; i < nf; ++i) {
        faces[i * 3 + 0] = mesh.triangles[i][0];
        faces[i * 3 + 1] = mesh.triangles[i][1];
        faces[i * 3 + 2] = mesh.triangles[i][2];
    }
    out->vertices = verts;
    out->vertex_count = (int)nv;
    out->faces = faces;
    out->face_count = (int)nf;
    return 0;
}

// C Mesh（只读输入）→ shapely::Mesh3d（float 顶点转 double）。
// 无效网格（字段缺失/空）返回 valid=false 的空结构，由调用方决定错误码。
shapely::Mesh3d MeshFromC(const Point3DAlg_Mesh* in)
{
    shapely::Mesh3d out;
    if (!in || !in->vertices || in->vertex_count <= 0 ||
        !in->faces || in->face_count <= 0)
        return out;
    out.vertices.reserve((size_t)in->vertex_count);
    for (int i = 0; i < in->vertex_count; ++i)
        out.vertices.push_back(shapely::Point3d{ (double)in->vertices[(size_t)i * 3 + 0],
                                                 (double)in->vertices[(size_t)i * 3 + 1],
                                                 (double)in->vertices[(size_t)i * 3 + 2] });
    out.triangles.reserve((size_t)in->face_count);
    for (int i = 0; i < in->face_count; ++i)
        out.triangles.push_back({ in->faces[(size_t)i * 3 + 0],
                                  in->faces[(size_t)i * 3 + 1],
                                  in->faces[(size_t)i * 3 + 2] });
    out.valid = true;
    return out;
}

// shapely::HeightMap → C HeightMap（vector → malloc 副本）。
// 返回 0 成功；-4 malloc 失败（out 保持已清零状态）。
int HeightMapToC(const shapely::HeightMap& hm, Point3DAlg_HeightMap* out)
{
    const size_t n = (size_t)hm.width * (size_t)hm.height;
    float* cells = (float*)malloc(n * sizeof(float));
    unsigned char* mask = (unsigned char*)malloc(n);
    if (!cells || !mask) {
        free(cells);
        free(mask);
        return -4;
    }
    std::copy(hm.cells.begin(), hm.cells.end(), cells);
    std::copy(hm.mask.begin(), hm.mask.end(), mask);
    out->width = hm.width;
    out->height = hm.height;
    out->x0 = hm.x0;
    out->y0 = hm.y0;
    out->cell = hm.cell;
    out->cells = cells;
    out->mask = mask;
    out->valid = hm.valid ? 1 : 0;
    return 0;
}

// C HeightMap（只读输入）→ shapely::HeightMap（malloc 副本 → vector）。
// 结构不一致（字段缺失/宽高非法）返回 false。
bool HeightMapFromC(const Point3DAlg_HeightMap* in, shapely::HeightMap& out)
{
    if (!in || in->width <= 0 || in->height <= 0 || !in->cells || !in->mask)
        return false;
    const size_t n = (size_t)in->width * (size_t)in->height;
    out.width = in->width;
    out.height = in->height;
    out.x0 = in->x0;
    out.y0 = in->y0;
    out.cell = in->cell;
    out.cells.assign(in->cells, in->cells + n);
    out.mask.assign(in->mask, in->mask + n);
    out.valid = in->valid != 0;
    return true;
}

// 表面重建统一法线管线：KNN k=10 估计 + MST 朝向一致化（k=30）。
// §9.12 强制约束（Poisson 前必须统一法线朝向）；SoA 容器无法传法线，
// 固定口径。失败返回 nullptr（由调用方映射 -2）。
// 说明：输入含非有限点时，对应点的法线为 NaN（KdTree 对 NaN 查询点的
// 邻域结果未定义，含 NaN 邻居的协方差直接 NaN）——不在此剔除：底层重建
// 入口（makePointNormalCloud）会统一剔除 NaN/Inf/零长法线的点，有效占比
// <50% 时整体拒绝（valid=false → 本层 -2），不会静默产出坏网格。
pcl::PointCloud<pcl::Normal>::Ptr EstimateOrientedNormals(const CloudPtr& src)
{
    if (!src || src->empty())
        return nullptr;
    pcl::PointCloud<pcl::Normal>::Ptr normals = CP::featureNormalEstimation(src, 10);
    if (!normals || normals->size() != src->size())
        return nullptr;
    pcl::PointCloud<pcl::Normal>::Ptr oriented =
        CP::featureOrientNormalsConsistentTangentPlane(src, normals, 30);
    // MST 仅在输入为空/规模不符时返回 nullptr（此处已排除）——防御性回退用
    // 未定向法线继续（重建仍可进行，朝向可能局部翻转），不映射 -2
    return oriented ? oriented : normals;
}

// PCL 凸包/凹包 reconstruct(points, polygons) 结果 → shapely::Mesh3d
//（>3 顶点多边形扇形三角化，与 CloudProcessor::meshFromPolygonMesh 同口径）。
// 绕向外向化：qhhull 输出面绕向不保证一致（实测凸包 divergence 体积失真），
// 按"面法向背离顶点质心"统一翻转（凸包质心恒在内部，外向化数学上成立；
// 凹包为尽力口径）。属容器转换级修正，不引入新算法。
shapely::Mesh3d HullMeshFromResult(const pcl::PointCloud<pcl::PointXYZ>& pts,
                                   const std::vector<pcl::Vertices>& polys)
{
    shapely::Mesh3d out;
    out.vertices.reserve(pts.size());
    for (const pcl::PointXYZ& v : pts.points)
        out.vertices.push_back(shapely::Point3d{ (double)v.x, (double)v.y, (double)v.z });
    const int nv = (int)out.vertices.size();
    for (const pcl::Vertices& poly : polys) {
        if (poly.vertices.size() < 3)
            continue;
        bool ok = true;
        for (uint32_t idx : poly.vertices)
            if ((int)idx < 0 || (int)idx >= nv) {
                ok = false;
                break;
            }
        if (!ok)
            continue;
        for (size_t k = 1; k + 1 < poly.vertices.size(); ++k)
            out.triangles.push_back({ (int)poly.vertices[0],
                                      (int)poly.vertices[k],
                                      (int)poly.vertices[k + 1] });
    }
    out.valid = !out.vertices.empty() && !out.triangles.empty();
    if (out.valid) {
        // 顶点质心 → 每面按外向判定翻转绕向
        double gx = 0, gy = 0, gz = 0;
        for (const shapely::Point3d& v : out.vertices) {
            gx += v.x;
            gy += v.y;
            gz += v.z;
        }
        const double inv = 1.0 / (double)out.vertices.size();
        gx *= inv;
        gy *= inv;
        gz *= inv;
        for (std::array<int, 3>& t : out.triangles) {
            const shapely::Point3d& A = out.vertices[(size_t)t[0]];
            const shapely::Point3d& B = out.vertices[(size_t)t[1]];
            const shapely::Point3d& C = out.vertices[(size_t)t[2]];
            const double ax = B.x - A.x, ay = B.y - A.y, az = B.z - A.z;
            const double bx = C.x - A.x, by = C.y - A.y, bz = C.z - A.z;
            const double nx = ay * bz - az * by;
            const double ny = az * bx - ax * bz;
            const double nz = ax * by - ay * bx;
            const double fx = (A.x + B.x + C.x) / 3.0 - gx;
            const double fy = (A.y + B.y + C.y) / 3.0 - gy;
            const double fz = (A.z + B.z + C.z) / 3.0 - gz;
            if (nx * fx + ny * fy + nz * fz < 0.0)
                std::swap(t[1], t[2]);
        }
    }
    return out;
}

// 输入点云基础校验（无 PointCloud 出参的重载；出参指针另由各接口自检）
int CheckInOnly(const Point3DAlg_PointCloud* in)
{
    if (!in || !in->xyz || in->count <= 0)
        return -1;
    return 0;
}

} // namespace

// =====================================================================
// 点云文件读写
// =====================================================================

int Point3DAlg_LoadCloud(const char* path, int type,
                         const char* delimiter,
                         Point3DAlg_PointCloud* out)
{
    if (!path || !path[0] || !out)
        return -1;
    ResetOut(out);
    if (type < 0 || type > 2)
        return -3;
    try {
        // TXT 分隔符：NULL/空串传 " "（CloudIO 语义 = 按连续空白切分）
        const std::string delim = (delimiter && delimiter[0]) ? std::string(delimiter)
                                                              : std::string(" ");
        CloudPtr cloud;
        switch (type) {
        case 0: cloud = CIO::ioLoadPCD(path); break;
        case 1: cloud = CIO::ioLoadPLY(path); break;
        default: cloud = CIO::ioLoadTXT(path, delim); break;
        }
        if (!cloud || cloud->empty())
            return -2;   // 文件打不开/格式错误/无有效数据（CloudIO 统一 nullptr）
        const int rc = FromPcl(cloud, out);
        return (rc == 0) ? 0 : rc;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

int Point3DAlg_SaveCloudEx(const char* path, int type,
                           const Point3DAlg_PointCloud* cloud,
                           int binary /*0=ASCII 1=二进制（仅 PLY 生效）*/)
{
    if (!path || !path[0] || !cloud)
        return -1;
    if (type < 0 || type > 2)
        return -3;
    if (!cloud->xyz || cloud->count <= 0)
        return -1;   // 空点云（与"参数错"同口径，见 api.h 注释）
    try {
        CloudPtr src = ToPcl(cloud);
        bool ok = false;
        switch (type) {
        case 0: ok = CIO::ioSavePCD(path, src); break;                    // 恒二进制
        case 1: ok = CIO::ioSavePLY(path, src, binary == 1); break;       // binary 仅 PLY 生效
        default: ok = CIO::ioSaveTXT(path, src, std::string(" ")); break; // 恒文本，空格分隔
        }
        return ok ? 0 : -2;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// =====================================================================
// 滤波与降采样（method/params 布局表见 api.h）
// =====================================================================

int Point3DAlg_FilterCloud(const Point3DAlg_PointCloud* in, int method,
                           const double* params, int param_count,
                           Point3DAlg_PointCloud* out)
{
    const int rcIn = CheckIn(in, out);
    if (rcIn != 0)
        return rcIn;
    const double* p = params;
    const int n = param_count;
    try {
        CloudPtr src = ToPcl(in);
        CloudPtr dst;
        switch (method) {
        case 0: {   // voxel：leaf（>0）+ 可选 min_points_per_voxel（>=0）
            if (!p || n < 1 || n > 2) return -3;
            if (!(p[0] > 0.0)) return -3;
            const int minPts = (n >= 2) ? ParamInt(p, 1) : 0;
            if (minPts < 0) return -3;
            dst = CP::filterVoxelGrid(src, p[0], minPts);
            break;
        }
        case 1: {   // random：sample_size（>0）+ 可选 seed（>=0）
            if (!p || n < 1 || n > 2) return -3;
            const int sample = ParamInt(p, 0);
            if (sample <= 0) return -3;
            unsigned int seed = 0u;
            if (n >= 2) {
                if (p[1] < 0.0) return -3;
                seed = (unsigned int)ParamInt(p, 1);
            }
            dst = CP::filterRandomSample(src, sample, seed);
            break;
        }
        case 2: {   // uniform：every_k（>0）
            if (!p || n != 1) return -3;
            const int everyK = ParamInt(p, 0);
            if (everyK <= 0) return -3;
            dst = CP::filterUniformSample(src, everyK);
            break;
        }
        case 3: {   // farthest：num_samples（>0）+ 可选 start_index（>=0）
            if (!p || n < 1 || n > 2) return -3;
            const int num = ParamInt(p, 0);
            if (num <= 0) return -3;
            const int start = (n >= 2) ? ParamInt(p, 1) : 0;
            if (start < 0) return -3;
            dst = CP::filterFarthestPointSample(src, num, start);
            break;
        }
        case 4:     // removeNonFinite：无参数
            if (n != 0) return -3;
            dst = CP::filterRemoveNonFinite(src);
            break;
        case 5: {   // removeDuplicates：可选 tolerance（>=0，默认 0）
            if (n < 0 || n > 1) return -3;
            const double tol = (n >= 1) ? p[0] : 0.0;
            if (!(tol >= 0.0)) return -3;
            dst = CP::filterRemoveDuplicates(src, tol);
            break;
        }
        case 6: {   // passThrough：field(0=x 1=y 2=z) + min + max（闭区间）
            if (!p || n != 3) return -3;
            const int field = ParamInt(p, 0);
            if (field < 0 || field > 2) return -3;
            if (p[1] > p[2]) return -3;
            static const char* kFields[3] = { "x", "y", "z" };
            dst = CP::filterPassThrough(src, kFields[field], p[1], p[2]);
            break;
        }
        case 7: {   // statisticalOutlier：mean_k（>=1）+ std_dev_mul（>=0）
            if (!p || n != 2) return -3;
            const int meanK = ParamInt(p, 0);
            if (meanK < 1) return -3;
            if (!(p[1] >= 0.0)) return -3;
            dst = CP::filterStatisticalOutlier(src, meanK, p[1]);
            break;
        }
        case 8: {   // radiusOutlier：radius（>0）+ min_neighbors（>=0）
            if (!p || n != 2) return -3;
            if (!(p[0] > 0.0)) return -3;
            const int minN = ParamInt(p, 1);
            if (minN < 0) return -3;
            dst = CP::filterRadiusOutlier(src, p[0], minN);
            break;
        }
        case 9: {   // conditionalXYZ：6 边界值（开区间；任一 min>max → -3）
            if (!p || n != 6) return -3;
            if (p[0] > p[1] || p[2] > p[3] || p[4] > p[5]) return -3;
            dst = CP::filterConditionalXYZ(src, p[0], p[1], p[2], p[3], p[4], p[5]);
            break;
        }
        case 10: {  // approxVoxel：近似体素降采样（1.3.0 新增；大点云快速，格点质心
                    // 与精确 voxel 可有微小偏差/格数相差 1~2%）
            if (!p || n < 1) return -3;
            if (!(p[0] > 0.0)) return -3;
            dst = CP::filterApproxVoxelGrid(src, p[0]);
            break;
        }
        default:
            return -3;
        }
        if (!dst || dst->empty())
            return -2;   // 滤波后 0 点（全部被剔除）
        const int rc = FromPcl(dst, out);
        return (rc == 0) ? 0 : rc;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// =====================================================================
// 提取与裁剪（method/indices/params 布局表见 api.h）
// =====================================================================

// 校验索引数组全部落在 [0, count)：-3 = 越界
static int CheckIndices(const int* indices, int indices_count, int count)
{
    for (int i = 0; i < indices_count; ++i)
        if (indices[i] < 0 || indices[i] >= count)
            return -3;
    return 0;
}

int Point3DAlg_ExtractCloud(const Point3DAlg_PointCloud* in, int method,
                            const int* indices, int indices_count,
                            const double* params, int param_count,
                            Point3DAlg_PointCloud* out)
{
    const int rcIn = CheckIn(in, out);
    if (rcIn != 0)
        return rcIn;
    const double* p = params;
    const int n = param_count;
    try {
        CloudPtr src = ToPcl(in);
        CloudPtr dst;
        switch (method) {
        case 0: {   // byIndices：索引必填
            if (!indices || indices_count <= 0)
                return -1;
            const int rcIdx = CheckIndices(indices, indices_count, in->count);
            if (rcIdx != 0)
                return rcIdx;
            dst = CP::extractByIndices(src,
                                       std::vector<int>(indices, indices + indices_count));
            break;
        }
        case 1: {   // byIndicesInverted：索引可选（空 = 保留全部的副本）
            if (indices && indices_count > 0) {
                const int rcIdx = CheckIndices(indices, indices_count, in->count);
                if (rcIdx != 0)
                    return rcIdx;
                dst = CP::extractByIndicesInverted(
                    src, std::vector<int>(indices, indices + indices_count));
            } else {
                dst.reset(new CloudXYZ(*src));
            }
            break;
        }
        case 2: {   // cropBox：min/max 闭区间（任一 min>max → -3）
            if (!p || n != 6) return -3;
            if (p[0] > p[3] || p[1] > p[4] || p[2] > p[5]) return -3;
            const float minCorner[3] = { (float)p[0], (float)p[1], (float)p[2] };
            const float maxCorner[3] = { (float)p[3], (float)p[4], (float)p[5] };
            dst = CP::extractCropBox(src, minCorner, maxCorner);
            break;
        }
        case 3: {   // cropOBB：center3 + extents3(半长) + rotation9(行主序) + 可选 invert
            if (!p || n < 15 || n > 16) return -3;
            if (!(p[3] > 0.0) || !(p[4] > 0.0) || !(p[5] > 0.0)) return -3;
            const bool invert = (n >= 16) ? (p[15] != 0.0) : false;
            shapely::OBB3d box;
            box.center = shapely::Point3d{ p[0], p[1], p[2] };
            box.extents[0] = p[3];
            box.extents[1] = p[4];
            box.extents[2] = p[5];
            for (int i = 0; i < 9; ++i)
                box.rotation[i] = p[6 + i];
            box.valid = true;
            dst = CP::extractCropOBB(src, box, invert);
            break;
        }
        case 4: {   // cropPolygonROI：XY 顶点对（>=3 点，param_count = 2×顶点数）
            if (!p || n < 6 || (n % 2) != 0) return -3;
            std::vector<shapely::Point2d> poly;
            poly.reserve((size_t)(n / 2));
            for (int i = 0; i + 1 < n; i += 2)
                poly.push_back(shapely::Point2d{ p[i], p[i + 1] });
            dst = CP::extractByPolygonROI(src, poly);
            break;
        }
        default:
            return -3;
        }
        if (!dst || dst->empty())
            return -2;   // 提取后 0 点
        const int rc = FromPcl(dst, out);
        return (rc == 0) ? 0 : rc;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// =====================================================================
// 空间变换（method/params 布局表见 api.h）
// =====================================================================

int Point3DAlg_TransformCloud(const Point3DAlg_PointCloud* in, int method,
                              const double* params, int param_count,
                              Point3DAlg_PointCloud* out)
{
    const int rcIn = CheckIn(in, out);
    if (rcIn != 0)
        return rcIn;
    const double* p = params;
    const int n = param_count;
    try {
        CloudPtr src = ToPcl(in);
        CloudPtr dst;
        switch (method) {
        case 0: {   // applyMatrix：4x4 行主序（与 RegistApplyTransform 同约定）
            if (!p || n != 16) return -3;
            float m[16];
            for (int i = 0; i < 16; ++i)
                m[i] = (float)p[i];
            dst = CP::transApplyMatrix(src, m);
            break;
        }
        case 1:     // translate：dx,dy,dz
            if (!p || n != 3) return -3;
            dst = CP::transTranslate(src, p[0], p[1], p[2]);
            break;
        case 2: {   // rotate：axis xyz（过原点，零向量/含非有限分量 → -3）+ angle（弧度）
            if (!p || n != 4) return -3;
            // 零向量经 fabs 求和恒为 0；NaN 求和为 NaN、==0 判定不成立会漏过
            // ——NaN 轴在底层归一化后产出全 NaN 点云且返回 0（静默垃圾），故
            // 改用"!(sum > 0)"判定：零向量与 NaN 分量同按参数非法报 -3
            if (!(std::fabs(p[0]) + std::fabs(p[1]) + std::fabs(p[2]) > 0.0)) return -3;
            if (!std::isfinite(p[3])) return -3;   // NaN/Inf 角度同样只产出 NaN 点云
            dst = CP::transRotate(src, p[0], p[1], p[2], p[3]);
            break;
        }
        case 3: {   // rotateAround：pivot xyz + axis xyz + angle（轴零向量/非有限 → -3）
            if (!p || n != 7) return -3;
            if (!(std::fabs(p[3]) + std::fabs(p[4]) + std::fabs(p[5]) > 0.0)) return -3;
            if (!std::isfinite(p[6])) return -3;
            const float pivot[3] = { (float)p[0], (float)p[1], (float)p[2] };
            dst = CP::transRotateAround(src, pivot, p[3], p[4], p[5], p[6]);
            break;
        }
        case 4: {   // scale：sx,sy,sz
            if (!p || n != 3) return -3;
            const double s[3] = { p[0], p[1], p[2] };
            dst = CP::transScale(src, s);
            break;
        }
        case 5: {   // scaleAround：scale xyz + center xyz（p' = c + s·(p - c)）
            if (!p || n != 6) return -3;
            const double s[3] = { p[0], p[1], p[2] };
            const float center[3] = { (float)p[3], (float)p[4], (float)p[5] };
            dst = CP::transScaleAround(src, s, center);
            break;
        }
        case 6: {   // project：平面 a,b,c,d（法向零向量/含非有限分量 → -3）
            if (!p || n != 4) return -3;
            // 同 rotate：NaN 法向会被 ==0 判定漏过，底层除 |n|² 产出全 NaN 点云
            if (!(std::fabs(p[0]) + std::fabs(p[1]) + std::fabs(p[2]) > 0.0)) return -3;
            const float plane[4] = { (float)p[0], (float)p[1], (float)p[2], (float)p[3] };
            dst = CP::transProject(src, plane);
            break;
        }
        default:
            return -3;
        }
        if (!dst || dst->empty())
            return -2;
        const int rc = FromPcl(dst, out);
        return (rc == 0) ? 0 : rc;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// =====================================================================
// 通用几何拟合（1.2.0 P1；method/fit 枚举与 out_coeffs 布局表见 api.h）
// =====================================================================

int Point3DAlg_FitModel(const Point3DAlg_PointCloud* in, int method, int fit,
                        double distance_threshold,
                        double* out_coeffs, int coeffs_capacity,
                        int** out_inliers, int* out_inlier_count)
{
    if (!out_inlier_count)
        return -1;
    *out_inlier_count = 0;
    if (out_inliers)
        *out_inliers = nullptr;
    const int rcIn = CheckInOnly(in);
    if (rcIn != 0)
        return rcIn;
    if (method < 0 || method > 5 || fit < 0 || fit > 2)
        return -3;
    if (!(distance_threshold > 0.0))
        return -3;
    // 系数布局长度：plane/sphere = 4，line3D = 6，cylinder/circle3D/cone = 7（见 api.h 表）
    const int needed = (method <= 1) ? 4 : (method == 5 ? 6 : 7);
    if (!out_coeffs || coeffs_capacity < needed) {
        *out_inlier_count = needed;   // 两段式：先查所需长度再分配重调
        return -3;
    }
    // fit 枚举映射：C 口径 0=RANSAC/1=MSAC/2=LMS → shapely RANSAC/MSAC/LMEDS
    const shapely::FitMethod fm = (fit == 0) ? shapely::FitMethod::RANSAC
                                  : (fit == 1) ? shapely::FitMethod::MSAC
                                               : shapely::FitMethod::LMEDS;
    try {
        CloudPtr src = ToPcl(in);
        std::vector<int> inlierIdx;
        switch (method) {
        case 0: {   // plane：[a,b,c,d]
            std::vector<float> coeffs;
            if (!CP::fitPlane(src, fm, distance_threshold, coeffs, inlierIdx) ||
                coeffs.size() < 4)
                return -2;
            for (int i = 0; i < 4; ++i)
                out_coeffs[i] = (double)coeffs[(size_t)i];
            break;
        }
        case 1: {   // sphere：[cx,cy,cz,r]（MSAC 底层回退 RANSAC，不报错）
            std::vector<float> center;
            double radius = 0.0;
            if (!CP::fitSphere(src, fm, distance_threshold, center, radius, inlierIdx) ||
                center.size() < 3)
                return -2;
            out_coeffs[0] = (double)center[0];
            out_coeffs[1] = (double)center[1];
            out_coeffs[2] = (double)center[2];
            out_coeffs[3] = radius;
            break;
        }
        case 2: {   // cylinder：[px,py,pz, dx,dy,dz, r]（仅 RANSAC，fit 被忽略）
            std::vector<float> axis, point;
            double radius = 0.0;
            if (!CP::fitCylinder(src, fm, distance_threshold, axis, point, radius,
                                 inlierIdx) ||
                axis.size() < 3 || point.size() < 3)
                return -2;
            for (int i = 0; i < 3; ++i) {
                out_coeffs[i] = (double)point[(size_t)i];        // 轴上一点
                out_coeffs[3 + i] = (double)axis[(size_t)i];     // 单位轴方向
            }
            out_coeffs[6] = radius;
            break;
        }
        case 3: {   // circle3D：[cx,cy,cz, nx,ny,nz, r]（底层圆周 RMS 未暴露）
            shapely::Point3d center;
            float normal[3] = { 0.f, 0.f, 0.f };
            double radius = 0.0, rms = 0.0;
            if (!CP::fitCircle3D(src, fm, distance_threshold, center, normal, radius,
                                 inlierIdx, rms))
                return -2;
            out_coeffs[0] = center.x;
            out_coeffs[1] = center.y;
            out_coeffs[2] = center.z;
            out_coeffs[3] = (double)normal[0];
            out_coeffs[4] = (double)normal[1];
            out_coeffs[5] = (double)normal[2];
            out_coeffs[6] = radius;
            break;
        }
        case 5: {   // line3D：[px,py,pz, dx,dy,dz]（1.3.0 新增；三种 fit 均支持）
            std::vector<float> point, dir;
            if (!CP::fitLine3D(src, fm, distance_threshold, point, dir, inlierIdx) ||
                point.size() < 3 || dir.size() < 3)
                return -2;
            for (int i = 0; i < 3; ++i) {
                out_coeffs[i] = (double)point[(size_t)i];        // 线上一点
                out_coeffs[3 + i] = (double)dir[(size_t)i];      // 单位方向
            }
            break;
        }
        default: {  // cone：[ax,ay,az, dx,dy,dz, 半开角弧度]（仅 RANSAC）
            std::vector<float> apex, axis;
            double angle = 0.0;
            if (!CP::fitCone(src, fm, distance_threshold, apex, axis, angle, inlierIdx) ||
                apex.size() < 3 || axis.size() < 3)
                return -2;
            for (int i = 0; i < 3; ++i) {
                out_coeffs[i] = (double)apex[(size_t)i];
                out_coeffs[3 + i] = (double)axis[(size_t)i];
            }
            out_coeffs[6] = angle;
            break;
        }
        }
        *out_inlier_count = (int)inlierIdx.size();
        if (out_inliers && !inlierIdx.empty()) {
            int* idx = (int*)malloc(inlierIdx.size() * sizeof(int));
            if (!idx) {
                // 出错路径出参归零（*out_inlier_count 仅在 0 成功与 -3 两段式
                // 回填时承载有效数据，避免调用方把 -4 配上非零计数误用）
                *out_inlier_count = 0;
                return -4;
            }
            std::copy(inlierIdx.begin(), inlierIdx.end(), idx);
            *out_inliers = idx;
        }
        return 0;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// =====================================================================
// 分割与聚类（1.2.0 P1；method/params 布局表见 api.h）
// =====================================================================

int Point3DAlg_SegmentCloud(const Point3DAlg_PointCloud* in, int method,
                            const double* params, int param_count,
                            Point3DAlg_PointCloud** out_segments,
                            int* out_segment_count)
{
    if (!out_segments || !out_segment_count)
        return -1;
    *out_segments = nullptr;
    *out_segment_count = 0;
    const int rcIn = CheckInOnly(in);
    if (rcIn != 0)
        return rcIn;
    if (method < 0 || method > 4)
        return -3;
    const double* p = params;
    const int n = param_count;
    try {
        CloudPtr src = ToPcl(in);
        std::vector<CloudPtr> parts;   // 候选段列表（空段不入列）
        switch (method) {
        case 0: {   // planeRANSAC：局内点 + 其余点，恒 2 段
            if (!p || n != 1) return -3;
            if (!(p[0] > 0.0)) return -3;
            CloudPtr inl, outl;
            std::vector<float> coeffs;
            if (!CP::segmentPlaneRANSAC(src, p[0], inl, outl, coeffs))
                return -2;
            // 契约"恒 2 段"（api.h）：[0]=局内点（底层拟合成功保证非空）、
            // [1]=其余点。outl 允许为空（全部点都在平面上）——空段照常入列，
            // 保证调用方按下标取段不需要"段数可能为 1"的分支（FromPcl 对空云
            // 输出 {NULL,0}，FreeClouds NULL 安全）
            if (!inl || inl->empty())
                return -2;   // 防御：底层契约外情形按"无有效结果"报
            parts.push_back(inl);
            // outl 契约"空时输出空点云而非 nullptr"——仍做 nullptr 防御，
            // C 边界不允许空指针入列（FromPcl 会解引用）
            parts.push_back(outl ? outl : CloudPtr(new CloudXYZ));
            break;
        }
        case 1: {   // euclideanCluster：每簇一段（点数降序）
            if (!p || n != 3) return -3;
            if (!(p[0] > 0.0)) return -3;
            const int minS = ParamInt(p, 1);
            const int maxS = ParamInt(p, 2);
            if (minS < 1 || maxS < minS) return -3;
            parts = CP::segmentEuclideanCluster(src, p[0], minS, maxS);
            break;
        }
        case 2: {   // dbscan：每簇一段（噪声点丢弃；标签按簇大小降序编号）
            if (!p || n != 2) return -3;
            if (!(p[0] > 0.0)) return -3;
            const int minPts = ParamInt(p, 1);
            if (minPts < 1) return -3;
            // 底层 segmentDBSCAN 的 out_labels 与"剔除非有限点后的 clean 云"
            // 对齐（fitInputClean 语义），并不与 src 等长——含 NaN/Inf 输入时
            // 直接传 src 会因长度不符报 -2。本层先自行剔除并记录 clean 行 →
            // src 行的映射，再把簇索引映射回原始坐标提取段，与其他 method
            // （planeRANSAC/euclidean/planesMulti 内部做同样剔除+映射）行为对齐
            CloudPtr clean(new CloudXYZ);
            std::vector<int> map2src;
            clean->reserve((size_t)in->count);
            map2src.reserve((size_t)in->count);
            for (int i = 0; i < in->count; ++i) {
                const float x = in->xyz[(size_t)i * 3 + 0];
                const float y = in->xyz[(size_t)i * 3 + 1];
                const float z = in->xyz[(size_t)i * 3 + 2];
                if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {
                    clean->push_back(pcl::PointXYZ(x, y, z));
                    map2src.push_back(i);
                }
            }
            if (clean->empty())
                return -2;   // 全部点非有限 → 无有效结果
            std::vector<int> labels;
            const int nCluster = CP::segmentDBSCAN(clean, p[0], minPts, labels);
            if (nCluster <= 0 || labels.size() != clean->size())
                return -2;
            for (int c = 0; c < nCluster; ++c) {
                std::vector<int> idx;
                for (size_t i = 0; i < labels.size(); ++i)
                    if (labels[i] == c)
                        idx.push_back(map2src[i]);
                if (!idx.empty())
                    parts.push_back(CP::extractByIndices(src, idx));
            }
            break;
        }
        case 3: {   // regionGrowing：法线内部估计（KNN k=k_neighbors）后分割
            if (!p || n != 5) return -3;
            const int k = ParamInt(p, 0);
            const int minS = ParamInt(p, 3);
            const int maxS = ParamInt(p, 4);
            if (k < 3 || !(p[1] > 0.0) || !(p[2] >= 0.0) || minS < 1 || maxS < minS)
                return -3;
            pcl::PointCloud<pcl::Normal>::Ptr normals = CP::featureNormalEstimation(src, k);
            if (!normals || normals->size() != src->size())
                return -2;
            for (std::vector<int>& idx :
                 CP::segmentRegionGrowing(src, normals, k, p[1], p[2], minS, maxS))
                if (!idx.empty())
                    parts.push_back(CP::extractByIndices(src, idx));
            break;
        }
        default: {  // planesMulti：每平面局内点一段（检出顺序）
            if (!p || n != 3) return -3;
            if (!(p[0] > 0.0)) return -3;
            const int maxPlanes = ParamInt(p, 1);
            const int minInl = ParamInt(p, 2);
            if (maxPlanes < 1 || minInl < 3) return -3;
            std::vector<std::vector<float>> coeffs;
            std::vector<std::vector<int>> inliers;
            const int found = CP::segmentPlanesMulti(src, p[0], maxPlanes, minInl,
                                                     coeffs, inliers);
            (void)found;
            for (std::vector<int>& idx : inliers)
                if (!idx.empty())
                    parts.push_back(CP::extractByIndices(src, idx));
            break;
        }
        }
        if (parts.empty())
            return -2;
        Point3DAlg_PointCloud* arr = (Point3DAlg_PointCloud*)malloc(
            parts.size() * sizeof(Point3DAlg_PointCloud));
        if (!arr)
            return -4;
        for (size_t i = 0; i < parts.size(); ++i) {
            arr[i].xyz = nullptr;
            arr[i].count = 0;
        }
        int filled = 0;
        for (size_t i = 0; i < parts.size(); ++i) {
            const int rcF = FromPcl(parts[i], &arr[i]);
            if (rcF != 0) {
                // 半途失败：回滚已分配段后报 -4（FreeClouds 按 filled 清理）
                Point3DAlg_FreeClouds(arr, filled);
                free(arr);
                return -4;
            }
            ++filled;
        }
        *out_segments = arr;
        *out_segment_count = filled;
        return 0;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// 释放 SegmentCloud 输出：逐段 free xyz 后 free 数组本身（NULL 安全）。
// segments 必须是 SegmentCloud 出参（本库 malloc 的数组 + 段内 xyz）；
// count = SegmentCloud 返回的段数（free(ptr, size) 同型契约）。
void Point3DAlg_FreeClouds(Point3DAlg_PointCloud* segments, int count)
{
    if (!segments || count <= 0)
        return;
    for (int i = 0; i < count; ++i) {
        free(segments[i].xyz);
        segments[i].xyz = nullptr;
        segments[i].count = 0;
    }
    free(segments);
}

// =====================================================================
// 空间索引与近邻搜索（1.2.0 P1；距离口径见 api.h——欧氏距离非平方）
// =====================================================================

// KNN/半径搜索公共实现（mode: 0=KNN 1=radius；param = k 或 radius）
static int SearchCommon(const Point3DAlg_PointCloud* in, const double query[3],
                        int mode, double param,
                        int** out_indices, float** out_dists, int* out_count)
{
    if (!out_count || !query)
        return -1;
    *out_count = 0;
    if (out_indices)
        *out_indices = nullptr;
    if (out_dists)
        *out_dists = nullptr;
    const int rcIn = CheckInOnly(in);
    if (rcIn != 0)
        return rcIn;
    if (mode == 0 && (int)param <= 0)
        return -3;   // k<=0
    if (mode == 1 && !(param > 0.0))
        return -3;   // radius<=0
    try {
        CloudPtr src = ToPcl(in);
        const float q[3] = { (float)query[0], (float)query[1], (float)query[2] };
        std::vector<int> idx;
        std::vector<float> dist;
        if (mode == 0)
            CP::treeKdTreeKNN(src, q, (int)param, idx, dist);
        else
            CP::treeKdTreeRadiusSearch(src, q, param, idx, dist);
        const size_t n = idx.size();
        // 底层 KdTree 返回平方距离 → 本层开方为欧氏距离（api.h 口径）
        for (size_t i = 0; i < n; ++i)
            dist[i] = (float)std::sqrt((double)dist[i]);
        // 契约"按距离升序"（api.h，两接口共用）：KNN 底层 nearestKSearch 保证
        // 升序；但 radiusSearch 走 FLANN 半径搜索**不保证有序**（按树遍历序），
        // 本层对半径模式按距离升序稳定排序（indices/dists 同步重排，保持对应）
        if (mode == 1 && n > 1) {
            std::vector<size_t> order(n);
            for (size_t i = 0; i < n; ++i)
                order[i] = i;
            std::sort(order.begin(), order.end(),
                      [&dist](size_t a, size_t b) { return dist[a] < dist[b]; });
            std::vector<int> idxSorted(n);
            std::vector<float> distSorted(n);
            for (size_t i = 0; i < n; ++i) {
                idxSorted[i] = idx[order[i]];
                distSorted[i] = dist[order[i]];
            }
            idx.swap(idxSorted);
            dist.swap(distSorted);
        }
        int* idxOut = nullptr;
        float* distOut = nullptr;
        if (out_indices && n > 0) {
            idxOut = (int*)malloc(n * sizeof(int));
            if (!idxOut)
                return -4;
        }
        if (out_dists && n > 0) {
            distOut = (float*)malloc(n * sizeof(float));
            if (!distOut) {
                free(idxOut);
                return -4;
            }
        }
        for (size_t i = 0; i < n; ++i) {
            if (idxOut)
                idxOut[i] = idx[i];
            if (distOut)
                distOut[i] = dist[i];
        }
        if (idxOut)
            *out_indices = idxOut;
        if (distOut)
            *out_dists = distOut;
        *out_count = (int)n;
        return 0;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

int Point3DAlg_KnnSearch(const Point3DAlg_PointCloud* in,
                         const double query[3], int k,
                         int** out_indices, float** out_dists, int* out_count)
{
    return SearchCommon(in, query, 0, (double)k, out_indices, out_dists, out_count);
}

int Point3DAlg_RadiusSearch(const Point3DAlg_PointCloud* in,
                            const double query[3], double radius,
                            int** out_indices, float** out_dists, int* out_count)
{
    return SearchCommon(in, query, 1, radius, out_indices, out_dists, out_count);
}

// 释放 KnnSearch/RadiusSearch 输出（NULL 安全；只用于本库分配的缓冲）
void Point3DAlg_FreeSearchResult(int* indices, float* dists)
{
    free(indices);
    free(dists);
}

// =====================================================================
// 通用度量（1.2.0 P1；method/params/输出布局表见 api.h §9.10）
// =====================================================================

// 两个平面方程（8 double）→ float[4] ×2；返回 false = params 缺失
static bool ParseTwoPlanes(const double* p, int n, float plane1[4], float plane2[4])
{
    if (!p || n != 8)
        return false;
    for (int i = 0; i < 4; ++i) {
        plane1[i] = (float)p[i];
        plane2[i] = (float)p[4 + i];
    }
    return true;
}

int Point3DAlg_MeasureCloud(const Point3DAlg_PointCloud* in,
                            const Point3DAlg_PointCloud* other,
                            int method, const double* params, int param_count,
                            double* out_values, int values_capacity,
                            int* out_value_count)
{
    if (!out_value_count)
        return -1;
    *out_value_count = 0;
    if (method < 0 || method > 7)
        return -3;
    // method 6/7 用 params 传两面方程，in/other 忽略（可 NULL）；其余必须有 in
    //（入参校验先于 capacity 检查——空指针优先报 -1）
    if (method != 6 && method != 7) {
        const int rcIn = CheckInOnly(in);
        if (rcIn != 0)
            return rcIn;
    }
    // 各 method 所需输出长度（method 4 = 逐点距离，长度 = in 点数）
    int needed = 0;
    if (method == 0) needed = 6;
    else if (method == 1) needed = 15;
    else if (method == 2) needed = 3;
    else if (method == 3) needed = 12;
    else if (method == 4) needed = in->count;
    else if (method == 5) needed = 3;
    else needed = (method == 6) ? 1 : 6;
    if (!out_values || values_capacity < needed) {
        *out_value_count = needed;   // 两段式：先查所需长度再分配重调
        return -3;
    }
    const double* p = params;
    const int n = param_count;
    try {
        switch (method) {
        case 0: {   // AABB：[minx,miny,minz,maxx,maxy,maxz]
            const shapely::AABB3d box = CP::measureAABB(ToPcl(in));
            if (!box.valid)
                return -2;   // 全部点非有限
            out_values[0] = box.min_x;
            out_values[1] = box.min_y;
            out_values[2] = box.min_z;
            out_values[3] = box.max_x;
            out_values[4] = box.max_y;
            out_values[5] = box.max_z;
            break;
        }
        case 1: {   // OBB：[cx,cy,cz, ex,ey,ez, R9 行主序]（点数 >=3）
            if (in->count < 3)
                return -1;
            const shapely::OBB3d obb = CP::measureOBB(ToPcl(in));
            if (!obb.valid)
                return -2;
            out_values[0] = obb.center.x;
            out_values[1] = obb.center.y;
            out_values[2] = obb.center.z;
            out_values[3] = obb.extents[0];
            out_values[4] = obb.extents[1];
            out_values[5] = obb.extents[2];
            for (int i = 0; i < 9; ++i)
                out_values[6 + i] = obb.rotation[i];
            break;
        }
        case 2: {   // centroid：[cx,cy,cz]
            shapely::Point3d c;
            if (!CP::measureCentroid(ToPcl(in), c))
                return -2;
            out_values[0] = c.x;
            out_values[1] = c.y;
            out_values[2] = c.z;
            break;
        }
        case 3: {   // meanCovariance：[mx,my,mz, c00..c22 行主序]（点数 >=2）
            if (in->count < 2)
                return -1;
            shapely::Point3d mean;
            double cov[9] = { 0 };
            if (!CP::measureMeanAndCovariance(ToPcl(in), mean, cov))
                return -2;
            out_values[0] = mean.x;
            out_values[1] = mean.y;
            out_values[2] = mean.z;
            for (int i = 0; i < 9; ++i)
                out_values[3 + i] = cov[i];
            break;
        }
        case 4: {   // pointCloudDistance：in→other 逐点最近欧氏距离
            if (!other || !other->xyz || other->count <= 0)
                return -1;
            const std::vector<float> dists =
                CP::measurePointCloudDistance(ToPcl(in), ToPcl(other));
            if (dists.size() != (size_t)in->count)
                return -2;
            for (size_t i = 0; i < dists.size(); ++i)
                out_values[i] = (double)dists[i];
            break;
        }
        case 5: {   // flatness：[pv 峰峰值, sigma 局内点残差标准差, inlier_count]
            if (!p || n != 1) return -3;
            if (!(p[0] > 0.0)) return -3;
            CloudPtr src = ToPcl(in);
            double pv = 0.0;
            std::vector<float> coeffs;
            if (!CP::measureFlatness(src, p[0], pv, coeffs) || coeffs.size() < 4)
                return -2;
            // sigma/inlier_count 本层复算（api.h 注释口径）：全部点对该平面
            // 的 |残差| <= threshold 者计入，sigma 为其残差标准差
            float plane[4] = { coeffs[0], coeffs[1], coeffs[2], coeffs[3] };
            std::vector<float> residuals;
            int inlierCnt = 0;
            double ss = 0.0, mean = 0.0;
            if (CP::planeResiduals(src, plane, residuals)) {
                for (float r : residuals) {
                    if (!std::isfinite((double)r) || std::fabs((double)r) > p[0])
                        continue;
                    ss += (double)r * (double)r;
                    mean += (double)r;
                    ++inlierCnt;
                }
            }
            double sigma = 0.0;
            if (inlierCnt > 0) {
                mean /= (double)inlierCnt;
                // 方差 = E[r²] − E[r]²（浮点误差可能出负 → 钳 0）
                const double var = ss / (double)inlierCnt - mean * mean;
                sigma = std::sqrt(var > 0.0 ? var : 0.0);
            }
            out_values[0] = pv;
            out_values[1] = sigma;
            out_values[2] = (double)inlierCnt;
            break;
        }
        case 6: {   // planeAngle：[angle_deg]（锐角口径 [0,90]）
            float plane1[4], plane2[4];
            if (!ParseTwoPlanes(p, n, plane1, plane2))
                return -3;
            const double ang = CP::measurePlaneAngle(plane1, plane2);
            if (ang < 0.0)
                return -3;   // 法向非有限/零向量
            out_values[0] = ang;
            break;
        }
        default: {  // planeIntersection：[px,py,pz, dx,dy,dz]
            float plane1[4], plane2[4];
            if (!ParseTwoPlanes(p, n, plane1, plane2))
                return -3;
            // 法向零向量/非有限 → -3（api.h"同 method 6"的 params 口径）：
            // 与 method 6 的 measurePlaneAngle 同判定（长度 <1e-12 或非有限）。
            // 底层 measurePlaneIntersection 对这两类输入一律返回 false，本层
            // 若不预判会把"参数非法"误报成 -2 近平行
            for (int k = 0; k < 2; ++k) {
                const float* pl = (k == 0) ? plane1 : plane2;
                const double l = std::sqrt((double)pl[0] * pl[0] + (double)pl[1] * pl[1] +
                                           (double)pl[2] * pl[2]);
                if (!(l >= 1e-12))
                    return -3;
            }
            float pt[3] = { 0, 0, 0 }, dir[3] = { 0, 0, 0 };
            if (!CP::measurePlaneIntersection(plane1, plane2, pt, dir))
                return -2;   // 近平行（|n1×n2| < 1e-9，无有限交线）
            for (int i = 0; i < 3; ++i) {
                out_values[i] = (double)pt[i];
                out_values[3 + i] = (double)dir[i];
            }
            break;
        }
        }
        *out_value_count = needed;
        return 0;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// =====================================================================
// 法线估计（1.2.0 P2；输出 N×4 [nx,ny,nz,curvature]，见 api.h §9.11a）
// =====================================================================

int Point3DAlg_EstimateNormals(const Point3DAlg_PointCloud* in, int method,
                               double param, float** out_normals, int* out_count)
{
    if (!out_normals || !out_count)
        return -1;
    *out_normals = nullptr;
    *out_count = 0;
    const int rcIn = CheckInOnly(in);
    if (rcIn != 0)
        return rcIn;
    if (method < 0 || method > 1)
        return -3;
    try {
        CloudPtr src = ToPcl(in);
        pcl::PointCloud<pcl::Normal>::Ptr normals;
        if (method == 0) {          // KNN：param = k（>=3）
            const int k = (int)std::lround(param);
            if (k < 3)
                return -3;
            normals = CP::featureNormalEstimation(src, k);
        } else {                    // radius：param = 半径（>0），邻居上限 100
            if (!(param > 0.0))
                return -3;
            normals = CP::featureNormalEstimationRadius(src, param, 100);
        }
        if (!normals || normals->size() != (size_t)in->count)
            return -2;
        const size_t nCnt = normals->size();
        float* buf = (float*)malloc(nCnt * 4 * sizeof(float));
        if (!buf)
            return -4;
        for (size_t i = 0; i < nCnt; ++i) {
            const pcl::Normal& n = (*normals)[i];
            buf[i * 4 + 0] = n.normal_x;
            buf[i * 4 + 1] = n.normal_y;
            buf[i * 4 + 2] = n.normal_z;
            buf[i * 4 + 3] = n.curvature;
        }
        *out_normals = buf;
        *out_count = (int)nCnt;
        return 0;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// =====================================================================
// 特征描述（1.2.0 P2；desc_type/param/out_dim 布局表见 api.h §9.11b）
// =====================================================================

int Point3DAlg_ComputeFeature(const Point3DAlg_PointCloud* in, int desc_type,
                              double param, float** out_features,
                              int* out_dim, int* out_count)
{
    if (!out_features || !out_dim || !out_count)
        return -1;
    *out_features = nullptr;
    *out_dim = 0;
    *out_count = 0;
    const int rcIn = CheckInOnly(in);
    if (rcIn != 0)
        return rcIn;
    if (desc_type < 0 || desc_type > 3)
        return -3;
    try {
        CloudPtr src = ToPcl(in);
        const size_t nCnt = src->size();
        std::vector<float> feats;
        int dim = 1;
        switch (desc_type) {
        case 0: {   // FPFH：33 维/点；法线内部 KNN k=10 估计（固定口径）
            if (!(param > 0.0))
                return -3;
            pcl::PointCloud<pcl::Normal>::Ptr normals = CP::featureNormalEstimation(src, 10);
            if (!normals || normals->size() != nCnt)
                return -2;
            pcl::PointCloud<pcl::FPFHSignature33>::Ptr fpfh =
                CP::featureFPFH(src, normals, param);
            if (!fpfh || fpfh->size() != nCnt)
                return -2;
            dim = 33;
            feats.resize(nCnt * 33);
            for (size_t i = 0; i < nCnt; ++i)
                for (int d = 0; d < 33; ++d)
                    feats[i * 33 + (size_t)d] = (*fpfh)[i].histogram[d];
            break;
        }
        case 1: {   // 协方差：9 维/点（3x3 行主序）
            const int k = (int)std::lround(param);
            if (k < 3)
                return -3;
            std::vector<std::array<float, 9>> covs;
            if (!CP::featureCovariances(src, k, covs) || covs.size() != nCnt)
                return -2;
            dim = 9;
            feats.resize(nCnt * 9);
            for (size_t i = 0; i < nCnt; ++i)
                for (int d = 0; d < 9; ++d)
                    feats[i * 9 + (size_t)d] = covs[i][(size_t)d];
            break;
        }
        case 2: {   // 马氏距离：1 维/点（点数 >=4 且全有限，否则底层拒绝）
            std::vector<double> dists;
            if (!CP::featureMahalanobisDistance(src, dists) || dists.size() != nCnt)
                return -2;
            dim = 1;
            feats.resize(nCnt);
            for (size_t i = 0; i < nCnt; ++i)
                feats[i] = (float)dists[i];
            break;
        }
        default: {  // 最近邻距离：1 维/点（param = 第 k 近邻，>=1）
            const int k = (int)std::lround(param);
            if (k < 1)
                return -3;
            const std::vector<float> d = CP::featureNearestNeighborDistance(src, k);
            if (d.size() != nCnt)
                return -2;
            dim = 1;
            feats = d;
            break;
        }
        }
        float* buf = (float*)malloc(feats.size() * sizeof(float));
        if (!buf)
            return -4;
        std::copy(feats.begin(), feats.end(), buf);
        *out_features = buf;
        *out_dim = dim;
        *out_count = (int)nCnt;
        return 0;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// =====================================================================
// 关键点检测（1.2.0 P2；type/参数映射见 api.h §9.11c）
// =====================================================================

int Point3DAlg_DetectKeypoints(const Point3DAlg_PointCloud* in, int type,
                               double radius, double threshold,
                               Point3DAlg_PointCloud* out)
{
    // in == out 别名传入 → -1（与 FilterCloud 等的 CheckIn 口径一致）。
    // 必须在 ResetOut 之前判：先清零会把 in->xyz 置空且指针丢失（泄漏），
    // 输入数据在返回 -1 前就被破坏
    if (!out || in == out)
        return -1;
    ResetOut(out);
    const int rcIn = CheckInOnly(in);
    if (rcIn != 0)
        return rcIn;
    if (type < 0 || type > 1)
        return -3;
    // Harris3D：radius>0 / threshold>=0；ISS3D：radius=salient>0 /
    // threshold=non_max>0（见 api.h 参数映射表）
    if (type == 0) {
        if (!(radius > 0.0) || !(threshold >= 0.0))
            return -3;
    } else {
        if (!(radius > 0.0) || !(threshold > 0.0))
            return -3;
    }
    try {
        CloudPtr src = ToPcl(in);
        CloudPtr kp = (type == 0) ? CP::keypointHarris3D(src, radius, threshold)
                                  : CP::keypointISS3D(src, radius, threshold);
        if (!kp || kp->empty())
            return -2;   // 0 关键点
        const int rc = FromPcl(kp, out);
        return (rc == 0) ? 0 : rc;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// =====================================================================
// 表面重建（1.2.0 P2；type/params 布局表见 api.h §9.12）
// =====================================================================

int Point3DAlg_ReconstructSurface(const Point3DAlg_PointCloud* in, int type,
                                  const double* params, int param_count,
                                  Point3DAlg_Mesh* out_mesh)
{
    if (!out_mesh)
        return -1;
    ResetMesh(out_mesh);
    const int rcIn = CheckInOnly(in);
    if (rcIn != 0)
        return rcIn;
    if (type < 0 || type > 8)
        return -3;
    const double* p = params;
    const int n = param_count;
    try {
        CloudPtr src = ToPcl(in);
        shapely::Mesh3d mesh;   // valid=false 默认 → 映射 -2
        switch (type) {
        case 0: {   // convexHull：本层直接调 PCL（CloudProcessor 同名算子只回顶点云）
            pcl::ConvexHull<pcl::PointXYZ> hull;
            hull.setInputCloud(src);
            pcl::PointCloud<pcl::PointXYZ> hullPts;
            std::vector<pcl::Vertices> polys;
            hull.reconstruct(hullPts, polys);
            mesh = HullMeshFromResult(hullPts, polys);
            break;
        }
        case 1: {   // concaveHull：[alpha] 必填（>0）
            if (!p || n != 1) return -3;
            if (!(p[0] > 0.0)) return -3;
            pcl::ConcaveHull<pcl::PointXYZ> hull;
            hull.setInputCloud(src);
            hull.setAlpha(p[0]);
            pcl::PointCloud<pcl::PointXYZ> hullPts;
            std::vector<pcl::Vertices> polys;
            hull.reconstruct(hullPts, polys);
            mesh = HullMeshFromResult(hullPts, polys);
            break;
        }
        case 2: {   // MLS：[radius] 必填 + [order]/[voxel] 可选；输出仅顶点
            if (!p || n < 1 || n > 3) return -3;
            if (!(p[0] > 0.0)) return -3;
            const int order = (n >= 2) ? ParamInt(p, 1) : 2;
            if (order < 1) return -3;
            const double voxel = (n >= 3) ? p[2] : 0.0;
            CloudPtr smooth = CP::surfaceSmoothMLS(src, p[0], order, voxel, nullptr);
            if (!smooth || smooth->empty())
                return -2;
            // MLS 是点云算子：Mesh 出参只装顶点（face_count=0，api.h 已注明）
            for (const pcl::PointXYZ& q : smooth->points)
                mesh.vertices.push_back(shapely::Point3d{ (double)q.x, (double)q.y,
                                                          (double)q.z });
            mesh.valid = !mesh.vertices.empty();
            break;
        }
        case 3: {   // Poisson：法线自动估计 + 朝向一致化（§9.12 强制约束）
            if (n < 0 || n > 3) return -3;
            const int depth = (n >= 1) ? ParamInt(p, 0) : 9;
            const int isoDivide = (n >= 2) ? ParamInt(p, 1) : 8;
            const double samples = (n >= 3) ? p[2] : 1.0;
            if (depth < 2) return -3;
            if (isoDivide < 1) return -3;
            if (!(samples > 0.0)) return -3;
            pcl::PointCloud<pcl::Normal>::Ptr normals = EstimateOrientedNormals(src);
            if (!normals)
                return -2;
            mesh = CP::surfacePoisson(src, normals, depth, isoDivide, samples);
            break;
        }
        case 4: {   // greedyProjection：[search_radius] 必填 + [mu]/[max_nb] 可选
            if (!p || n < 1 || n > 3) return -3;
            if (!(p[0] > 0.0)) return -3;
            const double mu = (n >= 2) ? p[1] : 2.5;
            const int maxNb = (n >= 3) ? ParamInt(p, 2) : 100;
            if (!(mu > 0.0) || maxNb < 3) return -3;
            pcl::PointCloud<pcl::Normal>::Ptr normals = EstimateOrientedNormals(src);
            if (!normals)
                return -2;
            mesh = CP::surfaceGreedyProjection(src, normals, p[0], mu, maxNb);
            break;
        }
        case 5: {   // marchingCubes（Hoppe）：[resolution] 可选（>=8，默认 64）
            if (n < 0 || n > 1) return -3;
            const int res = (n >= 1) ? ParamInt(p, 0) : 64;
            if (res < 8) return -3;
            pcl::PointCloud<pcl::Normal>::Ptr normals = EstimateOrientedNormals(src);
            if (!normals)
                return -2;
            mesh = CP::surfaceMarchingCubes(src, normals, res);
            break;
        }
        case 6: {   // marchingCubesRBF：[resolution]/[off_surface_epsilon] 可选
            if (n < 0 || n > 2) return -3;
            const int res = (n >= 1) ? ParamInt(p, 0) : 64;
            const double eps = (n >= 2) ? p[1] : 0.1;
            if (res < 8) return -3;
            if (!(eps > 0.0)) return -3;
            pcl::PointCloud<pcl::Normal>::Ptr normals = EstimateOrientedNormals(src);
            if (!normals)
                return -2;
            mesh = CP::surfaceMarchingCubesRBF(src, normals, res, eps);
            break;
        }
        case 7: {   // gridProjection：[resolution] 必填（>0）+ [padding] 可选
            if (!p || n < 1 || n > 2) return -3;
            if (!(p[0] > 0.0)) return -3;
            const int padding = (n >= 2) ? ParamInt(p, 1) : 1;
            if (padding < 0 || padding > 3) return -3;
            pcl::PointCloud<pcl::Normal>::Ptr normals = EstimateOrientedNormals(src);
            if (!normals)
                return -2;
            mesh = CP::surfaceGridProjection(src, normals, p[0], padding);
            break;
        }
        default: {  // organizedFastMesh：SoA 容器恒无序（height=1）→ 底层必拒绝
            if (n < 0 || n > 2) return -3;
            const int tps = (n >= 1) ? ParamInt(p, 0) : 1;
            const bool adaptive = (n >= 2) ? (p[1] != 0.0) : true;
            if (tps < 1) return -3;
            mesh = CP::surfaceOrganizedFastMesh(src, tps, adaptive);
            break;
        }
        }
        if (!mesh.valid || mesh.vertices.empty())
            return -2;   // 重建失败/空网格（含 type 8 有序点云限制）
        const int rcM = MeshToC(mesh, out_mesh);
        return (rcM == 0) ? 0 : rcM;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// 释放 Point3DAlg_Mesh（NULL 安全；只用于本库分配的缓冲）
void Point3DAlg_FreeMesh(Point3DAlg_Mesh* mesh)
{
    if (!mesh)
        return;
    free(mesh->vertices);
    free(mesh->faces);
    ResetMesh(mesh);
}

// =====================================================================
// 网格处理与度量（1.2.0 P2；op/params/出参分流表见 api.h）
// =====================================================================

int Point3DAlg_MeshOp(const Point3DAlg_Mesh* in, int op,
                      const double* params, int param_count,
                      Point3DAlg_Mesh* out_mesh, double* out_scalar,
                      float** out_array, int* out_array_count)
{
    // 入参统一预清零（出参按 op 只填相关项；错误路径保持已清零状态）
    if (out_mesh)
        ResetMesh(out_mesh);
    if (out_array)
        *out_array = nullptr;
    if (out_array_count)
        *out_array_count = 0;
    if (!in)
        return -1;
    if (op < 0 || op > 7)
        return -3;
    // 输入网格有效性（api.h 口径：无效网格 → -2；NULL → -1）
    if (!in->vertices || in->vertex_count <= 0 || !in->faces || in->face_count <= 0)
        return -2;
    // op 所需出参就位检查
    if ((op <= 2 && !out_mesh) ||
        ((op == 3 || op == 4) && !out_scalar) ||
        (op >= 5 && (!out_array || !out_array_count)))
        return -1;
    const double* p = params;
    const int n = param_count;
    try {
        switch (op) {
        case 0: {   // laplacianSmooth：[iterations>=1, lambda(0,1)] 必填 2 个
            if (!p || n != 2) return -3;
            const int iters = ParamInt(p, 0);
            if (iters < 1 || !(p[1] > 0.0) || p[1] > 1.0) return -3;
            const shapely::Mesh3d smooth =
                CP::surfaceMeshLaplacianSmooth(MeshFromC(in), iters, p[1]);
            if (!smooth.valid || smooth.triangles.empty())
                return -2;
            const int rcM = MeshToC(smooth, out_mesh);
            return (rcM == 0) ? 0 : rcM;
        }
        case 1: {   // clusterDecimate：[cell>0] 必填 1 个
            if (!p || n != 1) return -3;
            if (!(p[0] > 0.0)) return -3;
            const shapely::Mesh3d dec = CP::surfaceMeshClusterDecimate(MeshFromC(in), p[0]);
            if (!dec.valid || dec.triangles.empty())
                return -2;
            const int rcM = MeshToC(dec, out_mesh);
            return (rcM == 0) ? 0 : rcM;
        }
        case 2: {   // cropByOBB：15 个 = center3 + extents3(半长>0) + rot9
            if (!p || n != 15) return -3;
            if (!(p[3] > 0.0) || !(p[4] > 0.0) || !(p[5] > 0.0)) return -3;
            shapely::OBB3d box;
            box.center = shapely::Point3d{ p[0], p[1], p[2] };
            box.extents[0] = p[3];
            box.extents[1] = p[4];
            box.extents[2] = p[5];
            for (int i = 0; i < 9; ++i)
                box.rotation[i] = p[6 + i];
            box.valid = true;
            const shapely::Mesh3d cropped = CP::meshCropByOBB(MeshFromC(in), box);
            if (!cropped.valid || cropped.triangles.empty())
                return -2;
            const int rcM = MeshToC(cropped, out_mesh);
            return (rcM == 0) ? 0 : rcM;
        }
        case 3:     // area：总表面积
            *out_scalar = CP::measureMeshArea(MeshFromC(in));
            return 0;
        case 4:     // volume：有向体积（仅闭合水密网格有物理意义）
            *out_scalar = CP::measureMeshVolume(MeshFromC(in));
            return 0;
        case 5: {   // pointToMeshDistance：params=查询点 [x,y,z,...]（3N 个）
            if (!p || n < 3 || (n % 3) != 0) return -3;
            const int nPts = n / 3;
            CloudPtr query(new CloudXYZ);
            query->reserve((size_t)nPts);
            for (int i = 0; i < nPts; ++i)
                query->push_back(pcl::PointXYZ((float)p[(size_t)i * 3 + 0],
                                               (float)p[(size_t)i * 3 + 1],
                                               (float)p[(size_t)i * 3 + 2]));
            const std::vector<float> dists =
                CP::measurePointToMeshDistance(query, MeshFromC(in));
            if (dists.size() != (size_t)nPts)
                return -2;
            float* buf = (float*)malloc(dists.size() * sizeof(float));
            if (!buf)
                return -4;
            std::copy(dists.begin(), dists.end(), buf);
            *out_array = buf;
            *out_array_count = nPts;
            return 0;
        }
        case 6: {   // faceNormals：3F 个 float（每面 [nx,ny,nz]，退化面 = 0）
            const std::vector<float> fn = CP::meshFaceNormals(MeshFromC(in));
            if (fn.empty())
                return -2;
            const int nFaces = (int)(fn.size() / 3);
            float* buf = (float*)malloc(fn.size() * sizeof(float));
            if (!buf)
                return -4;
            std::copy(fn.begin(), fn.end(), buf);
            *out_array = buf;
            *out_array_count = nFaces;
            return 0;
        }
        default: {  // vertexNormals：3V 个 float（面积加权平均）
            const std::vector<float> vn = CP::meshVertexNormals(MeshFromC(in));
            if (vn.empty())
                return -2;
            const int nVerts = (int)(vn.size() / 3);
            float* buf = (float*)malloc(vn.size() * sizeof(float));
            if (!buf)
                return -4;
            std::copy(vn.begin(), vn.end(), buf);
            *out_array = buf;
            *out_array_count = nVerts;
            return 0;
        }
        }
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// =====================================================================
// 3D→2D 桥接（1.2.0 P2；HeightMap 容器布局见 api.h §9.13）
// =====================================================================

int Point3DAlg_CloudToHeightMap(const Point3DAlg_PointCloud* in,
                                double cell, int mode,
                                Point3DAlg_HeightMap* out)
{
    if (!out)
        return -1;
    ResetHm(out);
    const int rcIn = CheckInOnly(in);
    if (rcIn != 0)
        return rcIn;
    if (!(cell > 0.0))
        return -3;
    if (mode < 0 || mode > 3)
        return -3;
    try {
        const shapely::HeightMap hm = CP::cloudToHeightMap(ToPcl(in), cell, mode);
        if (!hm.valid)
            return -2;   // 无有效点 / 超 2000 万格上限
        const int rcH = HeightMapToC(hm, out);
        return (rcH == 0) ? 0 : rcH;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

int Point3DAlg_HeightMapToCloud(const Point3DAlg_HeightMap* hm,
                                Point3DAlg_PointCloud* out)
{
    if (!out)
        return -1;
    ResetOut(out);
    if (!hm)
        return -1;
    if (!hm->cells || !hm->mask)
        return -1;   // 结构字段不一致
    if (hm->valid == 0 || hm->width <= 0 || hm->height <= 0)
        return -2;
    try {
        shapely::HeightMap cppHm;
        if (!HeightMapFromC(hm, cppHm))
            return -1;
        CloudPtr cloud = CP::heightMapToCloud(cppHm);
        if (!cloud || cloud->empty())
            return -2;   // 无有效格
        const int rc = FromPcl(cloud, out);
        return (rc == 0) ? 0 : rc;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

int Point3DAlg_ThicknessMap(const Point3DAlg_PointCloud* top,
                            const Point3DAlg_PointCloud* bottom,
                            double cell, Point3DAlg_HeightMap* out)
{
    if (!out)
        return -1;
    ResetHm(out);
    const int rcTop = CheckInOnly(top);
    if (rcTop != 0)
        return rcTop;
    const int rcBot = CheckInOnly(bottom);
    if (rcBot != 0)
        return rcBot;
    if (!(cell > 0.0))
        return -3;
    try {
        const shapely::HeightMap hm = CP::thicknessMap(ToPcl(top), ToPcl(bottom), cell);
        if (!hm.valid)
            return -2;
        const int rcH = HeightMapToC(hm, out);
        return (rcH == 0) ? 0 : rcH;
    } catch (const std::exception&) {
        return -99;
    } catch (...) {
        return -99;
    }
}

// 释放 Point3DAlg_HeightMap（NULL 安全；只用于本库分配的缓冲）
void Point3DAlg_FreeHeightMap(Point3DAlg_HeightMap* hm)
{
    if (!hm)
        return;
    free(hm->cells);
    free(hm->mask);
    ResetHm(hm);
}
