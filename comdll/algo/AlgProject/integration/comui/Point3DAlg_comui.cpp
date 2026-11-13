// ComUI 统一 C ABI 适配层（Point3DAlg）：
// 单一入口签名 + algo_free 释放约定（见 docs/THIRD_PARTY_OPS.md §5.5）。
// 体素下采样：哈希格网 → 每格质心 + 颜色均值（PCL VoxelGrid 的等价简化实现，SoA 直进直出）。
#include "AlgExport.h"
#include "Point3DAlg/include/Point3DAlg_api.h"
#include "comui_native_abi.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unordered_map>

// 配准/球顶拟合/平面量测算子需要的 C++ 头（必须在 extern "C" 之外）
#include "Point3DAlg/tools/Point3DAlg_Regist.h"
#include "Point3DAlg/tools/Point3DAlg_DomeFit.h"
#include "Point3DAlg/tools/Point3DAlg_RegionPlane.h"
#include "Point3DAlg/tools/Point3DAlg_CloudDiff.h"
#include "Point3DAlg/tools/Point3DAlg_CircleFit.h"
#include "Point3DAlg/tools/Point3DAlg_Profile.h"
#include "Point3DAlg/tools/Point3DAlg_BumpReport.h"
#include "ShapelyAlg.h"           // CloudProcessor 的 ICP（init_transform 支持）

extern "C" {

// ---------------- 统一释放约定 ----------------

POINT3DALG_EXPORT void algo_free(void* p) {
    free(p);
}

// ---------------- 算子入口：体素下采样（node.json: alg.point3d.voxel_downsample） ----------------
// 参数（按 node.json params 声明顺序）：leaf_size(number，唯一参数)。

POINT3DALG_EXPORT int Point3DAlg_ComuiVoxel(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }

    double leaf = 0.01;
    for (int i = 0; i < in->paramCount; i++)
        if (in->params[i].kind == 0) { leaf = in->params[i].d; break; }
    if (leaf < 1e-6) leaf = 1e-6;

    const AlgoCCloud* src = &in->clouds[0];
    int n = src->count;
    const float* xyz = src->xyz;
    const uint8_t* rgb = src->rgb;
    if (n <= 0 || !xyz) { out->error = "输入点云为空"; return 2; }

    // 体素格网：格键 = (ix,iy,iz) 打包；格值 = 点索引（首点代表格）
    // 输出取每格质心与颜色均值（与 PCL VoxelGrid 语义一致）
    struct Acc { double x = 0, y = 0, z = 0; uint64_t r = 0, g = 0, b = 0; int cnt = 0; };
    std::unordered_map<uint64_t, Acc> grid;
    grid.reserve((size_t)n * 2 + 1);
    for (int i = 0; i < n; i++) {
        int64_t ix = (int64_t)floor(xyz[i * 3]     / leaf);
        int64_t iy = (int64_t)floor(xyz[i * 3 + 1] / leaf);
        int64_t iz = (int64_t)floor(xyz[i * 3 + 2] / leaf);
        uint64_t key = (uint64_t)(ix & 0x1FFFFF) << 42 | (uint64_t)(iy & 0x1FFFFF) << 21 | (uint64_t)(iz & 0x1FFFFF);
        auto& acc = grid[key];
        acc.x += xyz[i * 3]; acc.y += xyz[i * 3 + 1]; acc.z += xyz[i * 3 + 2];
        if (rgb) { acc.r += rgb[i * 3]; acc.g += rgb[i * 3 + 1]; acc.b += rgb[i * 3 + 2]; }
        acc.cnt++;
    }

    int m = (int)grid.size();
    float* outXyz = (float*)malloc((size_t)m * 3 * sizeof(float));
    uint8_t* outRgb = rgb ? (uint8_t*)malloc((size_t)m * 3) : nullptr;
    if (!outXyz || (rgb && !outRgb)) { free(outXyz); free(outRgb); out->error = "内存不足"; return 3; }

    int k = 0;
    for (const auto& kv : grid) {
        const auto& a = kv.second;
        outXyz[k * 3]     = (float)(a.x / a.cnt);
        outXyz[k * 3 + 1] = (float)(a.y / a.cnt);
        outXyz[k * 3 + 2] = (float)(a.z / a.cnt);
        if (outRgb) {
            outRgb[k * 3]     = (uint8_t)(a.r / a.cnt);
            outRgb[k * 3 + 1] = (uint8_t)(a.g / a.cnt);
            outRgb[k * 3 + 2] = (uint8_t)(a.b / a.cnt);
        }
        k++;
    }

    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(outXyz); free(outRgb); out->error = "内存不足"; return 3; }
    arr->count = m;
    arr->xyz = outXyz;
    arr->rgb = outRgb;

    out->clouds = arr;
    out->cloudCount = 1;
    return 0;
}

// ---------------- 参数读取辅助（SICK 读取用） ----------------

static const char* GetTextParam(const AlgoCInput* in) {
    for (int i = 0; i < in->paramCount; i++)
        if (in->params[i].kind == 2) return in->params[i].s;
    return nullptr;
}

static int GetIntParam(const AlgoCInput* in, int def) {
    for (int i = 0; i < in->paramCount; i++)
        if (in->params[i].kind == 1) return in->params[i].i;
    return def;
}

static double GetDoubleParam(const AlgoCInput* in, double def) {
    for (int i = 0; i < in->paramCount; i++)
        if (in->params[i].kind == 0) return in->params[i].d;
    return def;
}

// ---------------- 算子入口：SICK 扫描读取（node.json: alg.point3d.read_sick） ----------------
// 参数（按 node.json params 声明顺序）：folder(path) frame(int, <0=全部帧)；
// 换算参数自动读取（recipe/common json，默认路径，见 Point3DAlg_api.h 注释）。

POINT3DALG_EXPORT int Point3DAlg_ComuiReadSick(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    const char* folder = GetTextParam(in);
    if (!folder || !folder[0]) { out->error = "未设置扫描数据文件夹"; return 1; }
    const int frame = GetIntParam(in, -1);

    float* xyz = nullptr;
    int count = 0;
    const int rc = Point3DAlg_ReadSickCloud(folder, frame, nullptr, nullptr, &xyz, &count);
    if (rc != 0) {
        // ReadSickCloud 现只返回 -1（参数空，此处已提前拦）与 -4（统一错误）
        static char err[512];
        if (rc == -4)
            snprintf(err, sizeof(err), "读取失败（配置或数据错误）：%s", folder);
        else
            snprintf(err, sizeof(err), "读取失败（错误码 %d）：%s", rc, folder);
        out->error = err;
        return 2;
    }

    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { Point3DAlg_FreeBuffer(xyz); out->error = "内存不足"; return 3; }
    arr->count = count;
    arr->xyz = xyz;
    arr->rgb = nullptr;   // 无颜色：查看器按高度伪彩上色
    out->clouds = arr;
    out->cloudCount = 1;
    return 0;
}

// ---------------- 算子入口：手动配准 ICP（node.json: alg.point3d.manual_regist） ----------------
// 参数（按 node.json params 声明顺序，宿主 marshal 不带参数名）：strategy(int 1-6)
// init_transform(text 16值逗号分隔) max_distance(number) max_iteration(int)；
// 同 kind 参数以出现次序区分：第 1 个 int=strategy、第 2 个 int=max_iteration
// 输入：两片点云（源/目标）；输出：配准后点云（ABI v1 无 text 通道，变换矩阵并入节点消息）
// 初值建议用 Point3DAlg_RegistTool.exe（键盘粗调）导出的 regist_T_flat.txt 内容。

} // extern "C"

// 手动配准入口：函数体含 C++（PCL/Eigen），放 extern "C" 块外；
// 导出名用 extern "C" 标注保持与 node.json method 一致
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiManualRegist(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 2 || !in->clouds) { out->error = "需要两片输入点云（源/目标）"; return 1; }

    const AlgoCCloud* csrc = &in->clouds[0];
    const AlgoCCloud* cdst = &in->clouds[1];
    if (csrc->count <= 0 || !csrc->xyz) { out->error = "源点云为空"; return 2; }
    if (cdst->count <= 0 || !cdst->xyz) { out->error = "目标点云为空"; return 2; }

    // 参数读取（按 node.json params 声明顺序提取；宿主 marshal 不带参数名）：
    // 声明顺序 = strategy(int) → init_transform(text) → max_distance(number) → max_iteration(int)，
    // 同 kind 参数以出现次序区分：第 1 个 int=strategy、第 2 个 int=max_iteration
    int strategy = 1;
    double max_dist = 0.0;
    int max_iter = 60;
    float init_T[16];
    init_T[0] = init_T[5] = init_T[10] = init_T[15] = 1.0f;
    int intSeen = 0;
    const char* tfText = nullptr;
    for (int i = 0; i < in->paramCount; i++) {
        const AlgoCParam& prm = in->params[i];
        if (prm.kind == 1) {
            if (intSeen == 0) strategy = prm.i;
            else if (intSeen == 1) max_iter = prm.i;
            ++intSeen;
        } else if (prm.kind == 2) {
            if (!tfText) tfText = prm.s;
        }
        // kind==0（max_distance）不在本循环处理：仅一个 number，
        // 由下方第二个循环单独取首个 kind==0
    }
    for (int i = 0; i < in->paramCount; i++)
        if (in->params[i].kind == 0) { max_dist = in->params[i].d; break; }
    // 简化实现：node.json params 顺序 = [strategy(int), init_transform(text),
    //                                     max_distance(number), max_iteration(int)]
    if (strategy < 1 || strategy > 6) strategy = 1;
    if (max_iter < 1) max_iter = 1;
    if (tfText && Point3DAlg_RegistParseTransformText(tfText, init_T)) {
        // 解析成功；失败时保持单位阵（参数注释里给出默认 16 值即单位阵）
    }

    // 载入点云到 PCL
    pcl::PointCloud<pcl::PointXYZ>::Ptr src(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr dst(new pcl::PointCloud<pcl::PointXYZ>);
    src->reserve(csrc->count); dst->reserve(cdst->count);
    for (int i = 0; i < csrc->count; ++i)
        src->push_back(pcl::PointXYZ(csrc->xyz[i*3], csrc->xyz[i*3+1], csrc->xyz[i*3+2]));
    for (int i = 0; i < cdst->count; ++i)
        dst->push_back(pcl::PointXYZ(cdst->xyz[i*3], cdst->xyz[i*3+1], cdst->xyz[i*3+2]));

    // 会话 + 配准
    Point3DAlg_RegistSession session;
    session.strategy = strategy;
    std::memcpy(session.transform, init_T, sizeof(init_T));
    Point3DAlg_RegistParams rp;
    rp.max_correspondence_dist = max_dist;
    rp.max_iteration = max_iter;
    Point3DAlg_RegistStep step = Point3DAlg_RegistRun(session, src, dst, rp);

    // 输出配准后的点云（源 × T）
    pcl::PointCloud<pcl::PointXYZ>::Ptr aligned =
        Point3DAlg_RegistApplyTransform(src, step.next.transform);

    float* outXyz = (float*)malloc((size_t)aligned->size() * 3 * sizeof(float));
    if (!outXyz) { out->error = "内存不足"; return 3; }
    for (size_t i = 0; i < aligned->size(); ++i) {
        outXyz[i*3]   = (*aligned)[i].x;
        outXyz[i*3+1] = (*aligned)[i].y;
        outXyz[i*3+2] = (*aligned)[i].z;
    }
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(outXyz); out->error = "内存不足"; return 3; }
    arr->count = (int)aligned->size();
    arr->xyz = outXyz;
    arr->rgb = nullptr;
    out->clouds = arr;
    out->cloudCount = 1;

    // ABI v1 无 text 输出通道（AlgoCOutput），变换矩阵并入节点消息随行（宿主显示 error 字段）
    static char msgBuf[900];
    snprintf(msgBuf, sizeof(msgBuf), "fitness=%.4f rmse=%.4f | T=%s | %s",
             step.fitness, step.inlier_rmse,
             Point3DAlg_RegistFormatFlat(step.next.transform).c_str(),
             step.message.c_str());
    out->error = msgBuf;   // 宿主把非空 error 显示为节点消息
    return 0;
}

// ---------------- 算子入口：拱形球顶拟合（node.json: alg.point3d.dome_fit） ----------------
// 参数（按 node.json params 声明顺序提取，同 kind 参数以出现次序区分）：
//   height_threshold(number) cluster_tolerance(number) min_points(int) max_fit_rms(number)
// 输入：一片点云（基面 + bump 顶点，mm）；输出：每 bump 一个点的点云（x=apex_x,
// y=apex_y, z=bump 高度 mm），消息通道返回高度统计（有效数/均值/σ/中位/范围）。

extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiDomeFit(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    // 参数提取（kind 顺序：number×2 → int → number）
    double heightThreshold = 0.02, clusterTol = 0.12, maxFitRms = 0.005;
    int minPoints = 8;
    int numberSeen = 0;
    for (int i = 0; i < in->paramCount; i++) {
        const AlgoCParam& prm = in->params[i];
        if (prm.kind == 0) {
            if (numberSeen == 0) heightThreshold = prm.d;
            else if (numberSeen == 1) clusterTol = prm.d;
            else if (numberSeen == 2) maxFitRms = prm.d;
            ++numberSeen;
        } else if (prm.kind == 1) {
            minPoints = prm.i;
        }
    }
    if (minPoints < 4) minPoints = 4;

    // 载入点云（SoA → PCL，与 ManualRegist 同模式）
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
    cloud->reserve(src->count);
    for (int i = 0; i < src->count; ++i)
        cloud->push_back(pcl::PointXYZ(src->xyz[i * 3], src->xyz[i * 3 + 1], src->xyz[i * 3 + 2]));

    // 拟合
    Point3DAlg_DomeFitParams p;
    p.height_threshold = heightThreshold;
    p.cluster_tolerance = clusterTol;
    p.min_points = minPoints;
    p.max_fit_rms = maxFitRms;
    std::vector<Point3DAlg_DomeBump> bumps;
    const bool ok = Point3DAlg_DomeFit(cloud, p, bumps, nullptr);
    if (!ok) { out->error = "拟合失败：点数不足或无有效 bump（检查基面上是否有凸台）"; return 4; }

    // 输出点云：每 bump 一点（x=apex_x, y=apex_y, z=高度）
    size_t nValid = 0;
    for (const Point3DAlg_DomeBump& b : bumps)
        if (b.valid) ++nValid;
    if (nValid == 0) { out->error = "无有效 bump（全部簇未通过合理性守卫）"; return 4; }

    float* outXyz = (float*)malloc(nValid * 3 * sizeof(float));
    if (!outXyz) { out->error = "内存不足"; return 3; }
    size_t k = 0;
    for (const Point3DAlg_DomeBump& b : bumps) {
        if (!b.valid) continue;
        outXyz[k * 3]     = (float)b.apex[0];
        outXyz[k * 3 + 1] = (float)b.apex[1];
        outXyz[k * 3 + 2] = (float)b.height;
        ++k;
    }
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(outXyz); out->error = "内存不足"; return 3; }
    arr->count = (int)nValid;
    arr->xyz = outXyz;
    arr->rgb = nullptr;
    out->clouds = arr;
    out->cloudCount = 1;

    // 高度统计并入消息（宿主显示 error 字段）
    const Point3DAlg_DomeStats st = Point3DAlg_DomeStatsOf(bumps);
    static char msgBuf[512];
    snprintf(msgBuf, sizeof(msgBuf),
             "bump %d 个：高度均值 %.4f σ %.5f 中位 %.5f [%.5f ~ %.5f] mm；"
             "共面性(极差) %.5f 拟合面σ %.5f；异常簇拦截 %zu",
             st.count, st.mean, st.stddev, st.p50, st.min, st.max,
             st.coplanarity, st.fit_sigma, bumps.size() - (size_t)st.count);
    out->error = msgBuf;
    return 0;
}

// ---------------- 算子入口：ROI 平面量测（node.json: alg.point3d.region_plane） ----------------
// 参数（按 node.json params 声明顺序，全部 number）：x y w h（XY 矩形 ROI，mm）
// 输入：一片点云（mm，z 为高度向）；无云输出，量测结果并入节点消息：
//   高度（ROI 中心）/ 平面度 PV / σ / 内点数（ABI v1 无数值输出通道，manual_regist 同例）

extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiRegionPlane(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    double roi[4] = { 0.0, 0.0, 1.0, 1.0 };   // x y w h
    int seen = 0;
    for (int i = 0; i < in->paramCount && seen < 4; i++)
        if (in->params[i].kind == 0)
            roi[seen++] = in->params[i].d;

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
    cloud->reserve(src->count);
    for (int i = 0; i < src->count; ++i)
        cloud->push_back(pcl::PointXYZ(src->xyz[i * 3], src->xyz[i * 3 + 1], src->xyz[i * 3 + 2]));

    Point3DAlg_RegionPlaneParams p;
    Point3DAlg_RegionPlaneResult r;
    if (!Point3DAlg_RegionPlaneFit(cloud, roi[0], roi[1], roi[2], roi[3], p, r)) {
        out->error = "平面量测失败：ROI 内点 < 3 或退化（点共线）";
        return 4;
    }

    static char msgBuf[512];
    snprintf(msgBuf, sizeof(msgBuf),
             "ROI(%.2f,%.2f,%.2f,%.2f) 高度 %.5f mm | 平面度PV %.5f σ %.6f | 内点 %d/%d | z=%.5f%+.5f·x%+.5f·y",
             roi[0], roi[1], roi[2], roi[3], r.height, r.pv, r.sigma,
             r.fit_count, r.roi_count, r.c, r.a, r.b);
    out->error = msgBuf;
    return 0;
}

// ---------------- 算子入口：点云高度差对比（node.json: alg.point3d.cloud_diff） ----------------
// 参数（按 node.json params 声明顺序，全部 number）：cell_size min_points diff_threshold
// 输入：两片点云（clouds[0]=基准，clouds[1]=被测，XY 已对齐）；输出：差值点云
//（每格一点 z=diff，正=被测高），统计并入消息（ABI v1 无数值输出通道）

extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiCloudDiff(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 2 || !in->clouds) { out->error = "需要两片输入点云（基准/被测）"; return 1; }
    const AlgoCCloud* ca = &in->clouds[0];
    const AlgoCCloud* cb = &in->clouds[1];
    if (ca->count <= 0 || !ca->xyz) { out->error = "基准点云为空"; return 2; }
    if (cb->count <= 0 || !cb->xyz) { out->error = "被测点云为空"; return 2; }

    double cell = 0.1;
    int minPts = 3;
    double thr = 0.02;
    double alignSearch = 0.0;
    int numSeen = 0;
    for (int i = 0; i < in->paramCount; i++) {
        if (in->params[i].kind == 0) {
            if (numSeen == 0) cell = in->params[i].d;
            else if (numSeen == 1) thr = in->params[i].d;
            else if (numSeen == 2) alignSearch = in->params[i].d;
            ++numSeen;
        } else if (in->params[i].kind == 1 && minPts == 3) {
            minPts = in->params[i].i;   // 第 1 个 int
        }
    }
    if (!(cell > 0.0)) cell = 0.1;

    auto toCloud = [](const AlgoCCloud* c) {
        pcl::PointCloud<pcl::PointXYZ>::Ptr pc(new pcl::PointCloud<pcl::PointXYZ>);
        pc->reserve(c->count);
        for (int i = 0; i < c->count; ++i)
            pc->push_back(pcl::PointXYZ(c->xyz[i * 3], c->xyz[i * 3 + 1], c->xyz[i * 3 + 2]));
        return pc;
    };

    Point3DAlg_CloudDiffParams p;
    p.cell_size = cell;
    p.min_points_per_cell = minPts < 1 ? 1 : minPts;
    p.diff_threshold = thr;
    p.align_search = alignSearch > 0.0 ? alignSearch : 0.0;
    Point3DAlg_CloudDiffResult r;
    std::vector<float> diff;
    if (!Point3DAlg_CloudDiff(toCloud(ca), toCloud(cb), p, r, &diff)) {
        out->error = "高度差对比失败：两片云不重叠或格数据不足";
        return 4;
    }

    float* outXyz = (float*)malloc(diff.size() * sizeof(float));
    if (!outXyz) { out->error = "内存不足"; return 3; }
    memcpy(outXyz, diff.data(), diff.size() * sizeof(float));
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(outXyz); out->error = "内存不足"; return 3; }
    arr->count = (int)(diff.size() / 3);
    arr->xyz = outXyz;
    arr->rgb = nullptr;
    out->clouds = arr;
    out->cloudCount = 1;

    static char msgBuf[512];
    snprintf(msgBuf, sizeof(msgBuf),
             "cells=%lld mean=%+.5f σ=%.5f max|d|=%.5f(+%.5f/%.5f) 超差(>%.4f) %lld (%.2f%%)"
             "%s%+.3f,%+.3f",
             r.cells, r.mean, r.sigma, r.max_abs, r.max_pos, r.max_neg,
             thr, r.over_count, 100.0 * r.over_ratio,
             r.aligned ? " | 对齐" : "", r.aligned_dx, r.aligned_dy);
    out->error = msgBuf;
    return 0;
}

// ---------------- 算子入口：高度切片圆/孔位量测（node.json: alg.point3d.circle_fit） ----------------
// 参数（按 node.json params 声明顺序）：z_low(number) z_high(number) cluster_tolerance(number)
//                                        min_points(int) radius_min(number) radius_max(number)
// 输入：一片点云（mm）；输出：每孔一点（x=圆心x, y=圆心y, z=半径），统计并入消息

extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiCircleFit(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    double zLow = 0.0, zHigh = 0.1, tol = 0.2, rMin = 0.05, rMax = 50.0;
    int minPts = 20;
    int numSeen = 0;
    for (int i = 0; i < in->paramCount; i++) {
        const AlgoCParam& prm = in->params[i];
        if (prm.kind == 0) {
            if (numSeen == 0) zLow = prm.d;
            else if (numSeen == 1) zHigh = prm.d;
            else if (numSeen == 2) tol = prm.d;
            else if (numSeen == 3) rMin = prm.d;
            else if (numSeen == 4) rMax = prm.d;
            ++numSeen;
        } else if (prm.kind == 1) {
            minPts = prm.i;
        }
    }
    if (minPts < 3) minPts = 3;

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
    cloud->reserve(src->count);
    for (int i = 0; i < src->count; ++i)
        cloud->push_back(pcl::PointXYZ(src->xyz[i * 3], src->xyz[i * 3 + 1], src->xyz[i * 3 + 2]));

    Point3DAlg_CircleFitParams p;
    p.cluster_tolerance = tol;
    p.min_points = minPts;
    p.radius_min = rMin;
    p.radius_max = rMax;
    std::vector<Point3DAlg_CircleFitResult> circles;
    const int n = Point3DAlg_DetectCircles(cloud, zLow, zHigh, p, circles);
    if (n < 0) { out->error = "孔位检测失败：点云为空或参数非法"; return 4; }
    if (n == 0) { out->error = "高度带内无切片点（检查 z_low/z_high 是否覆盖孔壁/圆柱壁高度）"; return 4; }

    size_t nValid = 0;
    for (const auto& c : circles)
        if (c.valid) ++nValid;
    if (nValid == 0) { out->error = "无有效圆（半径越界或残差超限，检查半径范围/切片质量）"; return 4; }

    float* outXyz = (float*)malloc(nValid * 3 * sizeof(float));
    if (!outXyz) { out->error = "内存不足"; return 3; }
    size_t k = 0;
    for (const auto& c : circles) {
        if (!c.valid) continue;
        outXyz[k * 3] = (float)c.center[0];
        outXyz[k * 3 + 1] = (float)c.center[1];
        outXyz[k * 3 + 2] = (float)c.radius;
        ++k;
    }
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(outXyz); out->error = "内存不足"; return 3; }
    arr->count = (int)nValid;
    arr->xyz = outXyz;
    arr->rgb = nullptr;
    out->clouds = arr;
    out->cloudCount = 1;

    static char msgBuf[512];
    snprintf(msgBuf, sizeof(msgBuf),
             "检出圆/孔 %d 个（有效 %zu）：z 带 [%.3f, %.3f]", n, nValid, zLow, zHigh);
    out->error = msgBuf;
    return 0;
}

// ---------------- 算子入口：轮廓/截面量测（node.json: alg.point3d.profile） ----------------
// 参数（按 node.json params 声明顺序，全部 number）：x0 y0 x1 y1 band bin_size
// 输入：一片点云（mm）；输出：剖面点云（每格一点，实际线坐标 xy + z=格内中位高度），
// 统计（z 范围/位置、台阶落差 max_step 及位置）并入消息

extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiProfile(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    double p[6] = { 0.0, 0.0, 1.0, 0.0, 0.2, 0.1 };   // x0 y0 x1 y1 band bin
    int seen = 0;
    for (int i = 0; i < in->paramCount && seen < 6; i++)
        if (in->params[i].kind == 0)
            p[seen++] = in->params[i].d;

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
    cloud->reserve(src->count);
    for (int i = 0; i < src->count; ++i)
        cloud->push_back(pcl::PointXYZ(src->xyz[i * 3], src->xyz[i * 3 + 1], src->xyz[i * 3 + 2]));

    Point3DAlg_ProfileParams pp;
    pp.band = p[4];
    pp.bin_size = p[5];
    Point3DAlg_ProfileResult r;
    std::vector<float> prof;
    if (!Point3DAlg_Profile(cloud, p[0], p[1], p[2], p[3], pp, r, &prof)) {
        out->error = "剖面提取失败：带内无点或参数非法（线段须落在点云上方）";
        return 4;
    }

    // 剖面点云：格中心沿线坐标（线上实际 xy）+ z=中位高度
    const double dx = p[2] - p[0], dy = p[3] - p[1];
    const double len = std::sqrt(dx * dx + dy * dy);
    float* outXyz = (float*)malloc(prof.size() / 2 * 3 * sizeof(float));
    if (!outXyz) { out->error = "内存不足"; return 3; }
    for (size_t i = 0; i < prof.size() / 2; ++i) {
        const double s = prof[i * 2];
        outXyz[i * 3] = (float)(p[0] + dx * s / len);
        outXyz[i * 3 + 1] = (float)(p[1] + dy * s / len);
        outXyz[i * 3 + 2] = prof[i * 2 + 1];
    }
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(outXyz); out->error = "内存不足"; return 3; }
    arr->count = (int)(prof.size() / 2);
    arr->xyz = outXyz;
    arr->rgb = nullptr;
    out->clouds = arr;
    out->cloudCount = 1;

    static char msgBuf[512];
    snprintf(msgBuf, sizeof(msgBuf),
             "剖面 %lld 格（L=%.2fmm band=%.2f）：z [%.5f @s%.2f ~ %.5f @s%.2f] "
             "max_step %.5f @s%.2f",
             r.points, r.length, p[4], r.z_min, r.s_at_min, r.z_max, r.s_at_max,
             r.max_step, r.s_at_step);
    out->error = msgBuf;
    return 0;
}

// ---------------- 算子入口：bump 全参数报表（node.json: alg.point3d.bump_report） ----------------
// 参数（按 node.json 声明顺序）：height_threshold(number) cluster_tolerance(number)
//   height_min(number) height_max(number) diameter_min(number) diameter_max(number)
// 输入：一片点云；输出：每 bump 一点（x/y=球顶 XY，z=高度 mm），统计并入消息
// （NG 位 bit0 高度越界 bit1 直径越界 bit2 截面校验失败）

extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiBumpReport(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    double pv[11] = { 0.02, 0.25, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.05, 0.0 };
    // hThr clusterTol hMin hMax dMin dMax nomPx nomPy nomOx nomOy posThr
    int seen = 0;
    for (int i = 0; i < in->paramCount && seen < 11; i++)
        if (in->params[i].kind == 0) pv[seen++] = in->params[i].d;

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
    cloud->reserve(src->count);
    for (int i = 0; i < src->count; ++i)
        cloud->push_back(pcl::PointXYZ(src->xyz[i * 3], src->xyz[i * 3 + 1], src->xyz[i * 3 + 2]));

    Point3DAlg_BumpReportParams p;
    p.height_threshold = pv[0];
    p.cluster_tolerance = pv[1];
    p.height_min = pv[2];
    p.height_max = pv[3];
    p.diameter_min = pv[4];
    p.diameter_max = pv[5];
    p.nominal_pitch_x = pv[6];
    p.nominal_pitch_y = pv[7];
    p.nominal_origin_x = pv[8];
    p.nominal_origin_y = pv[9];
    p.pos_threshold = pv[10];
    std::vector<Point3DAlg_BumpReportEntry> es;
    Point3DAlg_BumpReportStats st;
    if (!Point3DAlg_BumpReport(cloud, p, es, &st)) {
        out->error = "报表失败：点云为空或未检出 bump";
        return 4;
    }

    float* outXyz = (float*)malloc(es.size() * 3 * sizeof(float));
    if (!outXyz) { out->error = "内存不足"; return 3; }
    for (size_t i = 0; i < es.size(); ++i) {
        outXyz[i * 3] = (float)es[i].x;
        outXyz[i * 3 + 1] = (float)es[i].y;
        outXyz[i * 3 + 2] = (float)es[i].height;
    }
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(outXyz); out->error = "内存不足"; return 3; }
    arr->count = (int)es.size();
    arr->xyz = outXyz;
    arr->rgb = nullptr;
    out->clouds = arr;
    out->cloudCount = 1;

    static char msgBuf[512];
    if (pv[6] > 0.0 && pv[7] > 0.0)
        snprintf(msgBuf, sizeof(msgBuf),
                 "bump %d 个：高度 %.4f±%.5f [%.4f~%.4f] 直径 %.4f±%.5f | "
                 "共面性σ %.5f | NG 高度 %d 直径 %d 位置度 %d（漏测 %d）",
                 st.count, st.h_mean, st.h_sigma, st.h_min, st.h_max,
                 st.d_mean, st.d_sigma, st.cop_sigma, st.ng_height, st.ng_diameter,
                 st.ng_pos, st.nominal_unmatched);
    else
        snprintf(msgBuf, sizeof(msgBuf),
                 "bump %d 个：高度 %.4f±%.5f [%.4f~%.4f] 直径 %.4f±%.5f | "
                 "共面性σ %.5f | NG 高度 %d 直径 %d",
                 st.count, st.h_mean, st.h_sigma, st.h_min, st.h_max,
                 st.d_mean, st.d_sigma, st.cop_sigma, st.ng_height, st.ng_diameter);
    out->error = msgBuf;
    return 0;
}
