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
#ifdef POINT3DALG_HAS_PCL   // 配准/量测算子头依赖 PCL；无 PCL 时相关 ComUI 入口同样不编译（见文内各守卫块）
#include "Point3DAlg/tools/Point3DAlg_Regist.h"
#include "Point3DAlg/tools/Point3DAlg_DomeFit.h"
#include "Point3DAlg/tools/Point3DAlg_RegionPlane.h"
#include "Point3DAlg/tools/Point3DAlg_CloudDiff.h"
#include "Point3DAlg/tools/Point3DAlg_CircleFit.h"
#include "Point3DAlg/tools/Point3DAlg_Profile.h"
#include "Point3DAlg/tools/Point3DAlg_BumpReport.h"
#include "ShapelyAlg.h"           // CloudProcessor 的 ICP（init_transform 支持）
#endif   // POINT3DALG_HAS_PCL（工具头 + ShapelyAlg.h，均间接依赖 PCL）

extern "C" {

// ---------------- 统一释放约定 ----------------

POINT3DALG_EXPORT void algo_free(void* p) {
    free(p);
}

// ---------------- 算子入口：体素下采样（node.json: alg.point3d.voxel_downsample） ----------------
// 参数（按 node.json params 声明顺序）：leaf_size(number，唯一参数)。


// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.voxel_downsample","name":"体素下采样","group":"3D 点云","category":"处理","description":"点云体素网格下采样：哈希格网取每格质心+颜色均值（PCL VoxelGrid 等价实现，SoA 直进直出）"},
//   "inputs": [
//     {"name":"输入","type":"cloud"}
//   ],
//   "params": [
//     {"name":"leaf_size","label":"体素边长","type":"number","default":0.01,"min":0.001,"max":1.0,"description":"体素格边长（mm）；每格输出质心+颜色均值，值越大点数越少"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"cloud"}
//   ],
//   "file": "point3d_voxel.node.json",
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h"], "includes": ["src"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.read_sick","name":"SICK 扫描读取","group":"IO","category":"源","description":"读取 SICK Ranger3 扫描文件夹（*.dat+*.xml+Scan3DPart*.json）生成点云(mm)，换算参数自动从数据文件读取"},
//   "inputs": [],
//   "params": [
//     {"name":"folder","label":"扫描数据文件夹","type":"path","default":"","description":"扫描数据文件夹（*.dat + *.xml + Scan3DPart*.json）；mm 换算参数自动从数据文件读取"},
//     {"name":"frame","label":"指定帧(-1=全部)","type":"int","default":-1,"min":-1,"description":"指定帧号（<0 = 全部帧合并）"}
//   ],
//   "outputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/Point3DAlg_SickIO.cpp","src/Point3DAlg/Point3DAlg_SickReader.cpp","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/include/Point3DAlg_SickReader.h","src/CommonAlg/include/CommonAlg_Json.h","thrid/nlohmann_json/single_include/nlohmann/json.hpp"], "includes": ["src","src/Point3DAlg/include","src/CommonAlg/include","thrid/nlohmann_json/single_include"]}
// }
// @comui-node-end
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
#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.manual_regist","name":"手动配准(ICP)","group":"3D 点云","category":"处理","description":"两片点云按初始变换+选定策略做 ICP 精配准；初始变换可用 Point3DAlg_RegistTool.exe 键盘粗调后导出；变换矩阵并入节点消息输出"},
//   "inputs": [
//     {"name":"源点云","type":"cloud"},
//     {"name":"目标点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"strategy","label":"配准策略(1-7)","type":"int","default":1,"min":1,"max":7,"description":"配准策略（1~7）"},
//     {"name":"init_transform","label":"初始变换(16值逗号分隔)","type":"text","default":"1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1","description":"初始变换 4×4 共 16 值逗号分隔（默认单位阵；建议用 Point3DAlg_RegistTool.exe 键盘粗调后导出的 regist_T_flat.txt 内容）"},
//     {"name":"max_distance","label":"对应点距离(mm,0=自适应)","type":"number","default":0.0,"min":0.0,"description":"ICP 最大对应点距离（mm）"},
//     {"name":"max_iteration","label":"最大迭代","type":"int","default":60,"min":1,"description":"最大迭代次数"}
//   ],
//   "outputs": [
//     {"name":"配准点云","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/tools/Point3DAlg_Regist.h","src/Point3DAlg/tools/Point3DAlg_Regist.cpp","src/ShapelyAlg/ShapelyAlg.h"], "includes": ["src","src/Point3DAlg/tools","src/ShapelyAlg"]}
// }
// @comui-node-end
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
    if (strategy < 1 || strategy > 7) strategy = 1;
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
    // 契约：未收敛（fitness < 0）时变换未定义，不得输出垃圾点云/T——返回 rc=2
    if (step.fitness < 0.0) {
        out->error = "配准未收敛（初值偏差超出策略覆盖范围或对应不足）";
        return 2;
    }

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
#endif   // POINT3DALG_HAS_PCL（ManualRegist）

// ---------------- 算子入口：拱形球顶拟合（node.json: alg.point3d.dome_fit） ----------------
// 参数（按 node.json params 声明顺序提取，同 kind 参数以出现次序区分）：
//   height_threshold(number) cluster_tolerance(number) min_points(int) max_fit_rms(number)
// 输入：一片点云（基面 + bump 顶点，mm）；输出：每 bump 一个点的点云（x=apex_x,
// y=apex_y, z=bump 高度 mm），消息通道返回高度统计（有效数/均值/σ/中位/范围）。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.dome_fit","name":"拱形球顶拟合(bump高度)","group":"3D 点云","category":"测量","description":"拟合基面上拱形球顶 bump 的高度：稳健基面→欧式聚类→球拟合，输出每 bump 一点（z=高度mm），统计并入节点消息"},
//   "inputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"height_threshold","label":"bump判定高度(mm,相对基面)","type":"number","default":0.02,"min":0.0,"description":"bump 与基面的分离高度阈值（mm）"},
//     {"name":"cluster_tolerance","label":"聚类连通距离(mm)","type":"number","default":0.12,"min":0.001,"max":5.0,"description":"欧式聚类邻域半径（mm）"},
//     {"name":"min_points","label":"每bump最少点数","type":"int","default":8,"min":4,"description":"每簇最少点数（<4 强制 4）"},
//     {"name":"max_fit_rms","label":"球拟合残差上限(mm)","type":"number","default":0.005,"min":0.0,"description":"球面拟合 RMS 上限（超限簇判无效）"}
//   ],
//   "outputs": [
//     {"name":"bump高度点云","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/tools/Point3DAlg_DomeFit.h","src/Point3DAlg/tools/Point3DAlg_DomeFit.cpp","src/ShapelyAlg/ShapelyAlg.h"], "includes": ["src","src/Point3DAlg/tools","src/ShapelyAlg"]}
// }
// @comui-node-end
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
#endif   // POINT3DALG_HAS_PCL（DomeFit）

// ---------------- 算子入口：ROI 平面量测（node.json: alg.point3d.region_plane） ----------------
// 参数（按 node.json params 声明顺序，全部 number）：x y w h（XY 矩形 ROI，mm）
// 输入：一片点云（mm，z 为高度向）；无云输出，量测结果并入节点消息：
//   高度（ROI 中心）/ 平面度 PV / σ / 内点数（ABI v1 无数值输出通道，manual_regist 同例）

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.region_plane","name":"ROI平面量测","group":"3D 点云","category":"测量","description":"XY 矩形区域内稳健拟合平面，量测高度与平面度(PV/σ)；台阶差=两个节点的高度相减；结果并入节点消息"},
//   "inputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"x","label":"ROI左上X(mm)","type":"number","default":0.0,"description":"ROI 左上角 X（mm）"},
//     {"name":"y","label":"ROI左上Y(mm)","type":"number","default":0.0,"description":"ROI 左上角 Y（mm）"},
//     {"name":"w","label":"ROI宽W(mm)","type":"number","default":1.0,"min":0.001,"description":"ROI 宽（mm）"},
//     {"name":"h","label":"ROI高H(mm)","type":"number","default":1.0,"min":0.001,"description":"ROI 高（mm）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/tools/Point3DAlg_RegionPlane.h","src/Point3DAlg/tools/Point3DAlg_RegionPlane.cpp","src/Point3DAlg/tools/Point3DAlg_FitUtil.h"], "includes": ["src","src/Point3DAlg/tools"]}
// }
// @comui-node-end
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
#endif   // POINT3DALG_HAS_PCL（RegionPlane）

// ---------------- 算子入口：点云高度差对比（node.json: alg.point3d.cloud_diff） ----------------
// 参数（按 node.json params 声明顺序，全部 number）：cell_size min_points diff_threshold
// 输入：两片点云（clouds[0]=基准，clouds[1]=被测，XY 已对齐）；输出：差值点云
//（每格一点 z=diff，正=被测高），统计并入消息（ABI v1 无数值输出通道）

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.cloud_diff","name":"点云高度差对比","group":"3D 点云","category":"测量","description":"基准/被测两片点云 XY 栅格化后逐格求 z 差（中位数），输出差值点云（z=差值，正=被测高）与统计；超差阈值判定 NG 占比；align_search>0 时先做 XY 粗对齐（精度 ≈±cell/2，含 z 常差的对比需台阶/凸台特征形貌）"},
//   "inputs": [
//     {"name":"基准点云","type":"cloud"},
//     {"name":"被测点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"cell_size","label":"栅格边长(mm)","type":"number","default":0.1,"min":0.01,"description":"对比格网边长（mm）"},
//     {"name":"min_points","label":"格最少点数","type":"int","default":3,"min":1,"description":"每格最少点数（不足的格不参与对比）"},
//     {"name":"diff_threshold","label":"超差阈值(mm,0=不判)","type":"number","default":0.02,"min":0.0,"description":"高度差判定阈值（mm；z=差值，正=被测高）"},
//     {"name":"align_search","label":"XY粗对齐搜索半径(mm,0=不对齐)","type":"number","default":0.0,"min":0.0,"description":"XY 自动对齐搜索半径（mm；≤0 = 不对齐）"}
//   ],
//   "outputs": [
//     {"name":"差值点云","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/tools/Point3DAlg_CloudDiff.h","src/Point3DAlg/tools/Point3DAlg_CloudDiff.cpp","src/Point3DAlg/tools/Point3DAlg_FitUtil.h"], "includes": ["src","src/Point3DAlg/tools"]}
// }
// @comui-node-end
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
#endif   // POINT3DALG_HAS_PCL（CloudDiff）

// ---------------- 算子入口：高度切片圆/孔位量测（node.json: alg.point3d.circle_fit） ----------------
// 参数（按 node.json params 声明顺序）：z_low(number) z_high(number) cluster_tolerance(number)
//                                        min_points(int) radius_min(number) radius_max(number)
// 输入：一片点云（mm）；输出：每孔一点（x=圆心x, y=圆心y, z=半径），统计并入消息

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.circle_fit","name":"圆孔位量测","group":"3D 点云","category":"测量","description":"高度切片[z_low,z_high]提取孔壁/圆柱壁点，聚类后逐簇 XY 圆拟合，输出每孔一点（z=半径），孔径/圆心定位"},
//   "inputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"z_low","label":"切片高度下限(mm)","type":"number","default":0.0,"description":"切片高度带下限（mm；须覆盖孔壁/圆柱壁高度）"},
//     {"name":"z_high","label":"切片高度上限(mm)","type":"number","default":0.1,"description":"切片高度带上限（mm）"},
//     {"name":"cluster_tolerance","label":"聚类连通距离(mm)","type":"number","default":0.2,"min":0.01,"description":"聚类邻域半径（mm）"},
//     {"name":"min_points","label":"每簇最少点数","type":"int","default":20,"min":3,"description":"每簇最少点数（<3 强制 3）"},
//     {"name":"radius_min","label":"半径下限(mm)","type":"number","default":0.05,"min":0.0,"description":"半径过滤下限（mm）"},
//     {"name":"radius_max","label":"半径上限(mm)","type":"number","default":50.0,"description":"半径过滤上限（mm）"}
//   ],
//   "outputs": [
//     {"name":"孔位点云","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/tools/Point3DAlg_CircleFit.h","src/Point3DAlg/tools/Point3DAlg_CircleFit.cpp","src/Point3DAlg/tools/Point3DAlg_FitUtil.h"], "includes": ["src","src/Point3DAlg/tools"]}
// }
// @comui-node-end
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
#endif   // POINT3DALG_HAS_PCL（CircleFit）

// ---------------- 算子入口：轮廓/截面量测（node.json: alg.point3d.profile） ----------------
// 参数（按 node.json params 声明顺序，全部 number）：x0 y0 x1 y1 band bin_size
// 输入：一片点云（mm）；输出：剖面点云（每格一点，实际线坐标 xy + z=格内中位高度），
// 统计（z 范围/位置、台阶落差 max_step 及位置）并入消息

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.profile","name":"轮廓截面量测","group":"3D 点云","category":"测量","description":"沿 XY 线段提取 z 剖面（带内投影+弧长分格中位数），输出剖面点云与台阶落差 max_step，统计并入消息"},
//   "inputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"x0","label":"起点X(mm)","type":"number","default":0.0,"description":"截面线段起点 X（mm）"},
//     {"name":"y0","label":"起点Y(mm)","type":"number","default":0.0,"description":"截面线段起点 Y（mm）"},
//     {"name":"x1","label":"终点X(mm)","type":"number","default":1.0,"description":"截面线段终点 X（mm）"},
//     {"name":"y1","label":"终点Y(mm)","type":"number","default":0.0,"description":"截面线段终点 Y（mm）"},
//     {"name":"band","label":"带半宽(mm)","type":"number","default":0.2,"min":0.01,"description":"带半宽（mm；线段两侧投影带宽）"},
//     {"name":"bin_size","label":"弧长分格(mm)","type":"number","default":0.1,"min":0.01,"description":"弧长分格尺寸（mm；每格取带内中位高度）"}
//   ],
//   "outputs": [
//     {"name":"剖面点云","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/tools/Point3DAlg_Profile.h","src/Point3DAlg/tools/Point3DAlg_Profile.cpp","src/Point3DAlg/tools/Point3DAlg_FitUtil.h"], "includes": ["src","src/Point3DAlg/tools"]}
// }
// @comui-node-end
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
#endif   // POINT3DALG_HAS_PCL（Profile）

// ---------------- 算子入口：bump 全参数报表（node.json: alg.point3d.bump_report） ----------------
// 参数（按 node.json 声明顺序）：height_threshold(number) cluster_tolerance(number)
//   height_min(number) height_max(number) diameter_min(number) diameter_max(number)
// 输入：一片点云；输出：每 bump 一点（x/y=球顶 XY，z=高度 mm），统计并入消息
// （NG 位 bit0 高度越界 bit1 直径越界 bit2 截面校验失败）

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.bump_report","name":"bump全参数报表","group":"3D 点云","category":"测量","description":"bump 阵列一次性全参数检测：高度（球顶拟合）+ 截面直径（距顶 0.35 处圆拟合）+ 共面性偏差 + NG 位；可选名义网格位置度（nominal_pitch_*>0 启用，平移-only 不含旋转）；输出每 bump 一点（z=高度）与统计消息"},
//   "inputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"height_threshold","label":"bump判定高度(mm)","type":"number","default":0.02,"min":0.0,"description":"bump 与基面的分离高度阈值（mm）"},
//     {"name":"cluster_tolerance","label":"聚类连通距离(mm)","type":"number","default":0.25,"min":0.01,"description":"欧式聚类邻域半径（mm）"},
//     {"name":"height_min","label":"高度NG下限(mm,0=不判)","type":"number","default":0.0,"description":"高度 NG 判定下限（mm；>0 才启用高度判定）"},
//     {"name":"height_max","label":"高度NG上限(mm,0=不判)","type":"number","default":0.0,"description":"高度 NG 判定上限（mm）"},
//     {"name":"diameter_min","label":"直径NG下限(mm,0=不判)","type":"number","default":0.0,"description":"直径 NG 判定下限（mm；>0 才启用直径判定）"},
//     {"name":"diameter_max","label":"直径NG上限(mm,0=不判)","type":"number","default":0.0,"description":"直径 NG 判定上限（mm）"},
//     {"name":"nominal_pitch_x","label":"名义网格X间距(mm,0=不判位置度)","type":"number","default":0.0,"min":0.0,"description":"标称间距 X（mm；XY 均 >0 启用位置度判定）"},
//     {"name":"nominal_pitch_y","label":"名义网格Y间距(mm)","type":"number","default":0.0,"min":0.0,"description":"标称间距 Y（mm）"},
//     {"name":"nominal_origin_x","label":"名义网格原点X(mm)","type":"number","default":0.0,"min":0.0,"description":"标称阵列原点 X（mm）"},
//     {"name":"nominal_origin_y","label":"名义网格原点Y(mm)","type":"number","default":0.0,"min":0.0,"description":"标称阵列原点 Y（mm）"},
//     {"name":"pos_threshold","label":"位置度NG阈值(mm)","type":"number","default":0.05,"min":0.0,"description":"位置度 NG 阈值（mm）"}
//   ],
//   "outputs": [
//     {"name":"bump点云","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/tools/Point3DAlg_BumpReport.h","src/Point3DAlg/tools/Point3DAlg_BumpReport.cpp","src/Point3DAlg/tools/Point3DAlg_DomeFit.h","src/Point3DAlg/tools/Point3DAlg_DomeFit.cpp","src/Point3DAlg/tools/Point3DAlg_CircleFit.h","src/Point3DAlg/tools/Point3DAlg_CircleFit.cpp","src/Point3DAlg/tools/Point3DAlg_FitUtil.h","src/ShapelyAlg/ShapelyAlg.h"], "includes": ["src","src/Point3DAlg/tools","src/ShapelyAlg"]}
// }
// @comui-node-end
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
#endif   // POINT3DALG_HAS_PCL（BumpReport）

// ---------------- 通用点云算子接入（node.json: alg.point3d.load_cloud 等 4 个） ----------------
// 本节 4 个入口全部走 Point3DAlg_api.h 1.1.0 的通用点云算子 C 接口
//（LoadCloud / FilterCloud / ExtractCloud / TransformCloud，实现见
// Point3DAlg_CloudOps.cpp，底层 ShapelyAlg::CloudIO / CloudProcessor，点型
// PointXYZ——rgb 不随行，输出点云查看器按高度伪彩上色）。
// 适配层只做参数组装与错误码映射；method 分支沿用现有"固定参数集"约定：
// node.json 声明该节点全部参数（同 kind 按声明序区分），适配层按 method
// 只把该分支用到的参数组装成 C API params 数组，未用参数保持默认值跳过。

// 按声明顺序取第 occurrence（0 起）个 kind 类型参数（Image2DAlg_comui.cpp 同款辅助）。
static const AlgoCParam* FindParam(const AlgoCInput* in, int kind, int occurrence) {
    int seen = 0;
    for (int i = 0; i < in->paramCount; i++)
        if (in->params[i].kind == kind && seen++ == occurrence) return &in->params[i];
    return nullptr;
}

// 逗号分隔 double 列表解析（manual_regist init_transform 文本矩阵同款交互约定；
// Image2DAlg_comui.cpp 有同款拷贝，两份行为必须保持一致——文本参数格式即契约）。
//   NULL/空串 -> 0；连续/首尾分隔符（逗号/空白/分号/方括号）跳过；
//   非数字 token 或值个数超 max_n -> -1（数量不符统一 -1，由调用方换算 rc=3，
//   obb_params 15 值 / polygon_xy 偶数且 >=6 / transform_matrix 16 值等
//   校验因此不会越界读 vals）；返回实际解析个数。
static int ParseDoubleList(const char* text, double* vals, int max_n) {
    if (!text || !text[0]) return 0;
    int n = 0;
    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
               *p == ',' || *p == ';' || *p == '[' || *p == ']') ++p;
        if (!*p) break;
        char* end = nullptr;
        const double v = strtod(p, &end);
        if (end == p) return -1;   // 非数字 token
        if (n >= max_n) return -1; // 超出容量（数量不符）
        vals[n++] = v;
        p = end;
    }
    return n;
}

// 输入点云只读视图（C API 契约不修改输入；xyz 去 const 仅因容器字段为 float*）
static Point3DAlg_PointCloud CloudViewOf(const AlgoCCloud* c) {
    Point3DAlg_PointCloud cloud;
    cloud.xyz = (float*)c->xyz;
    cloud.count = c->count;
    return cloud;
}

// 结果点云转移：核心库 malloc 的 xyz 移交宿主（algo_free = free，与
// Point3DAlg_FreeCloud 的 free 同一分配器，都是 MSVC CRT 堆）。
// 所有权转移后把源指针置空防双重释放——调用方此后即使误调 FreeCloud 也安全
//（FreeCloud 对 NULL 指针安全）；count 保留，调用方成功消息还要用它。
static bool MoveCloudOut(Point3DAlg_PointCloud* cloud, AlgoCOutput* out) {
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { Point3DAlg_FreeCloud(cloud); return false; }   // 容器分配失败：xyz 归还核心库释放
    arr->count = cloud->count;
    arr->xyz = cloud->xyz;
    arr->rgb = nullptr;
    cloud->xyz = nullptr;   // 所有权已移交 out，置空源指针（count 保留供消息回读）
    out->clouds = arr;
    out->cloudCount = 1;
    return true;
}

// in/out 点数消息（成功时随行，contour_cloud 先例）
static void AppendCountMsg(char* buf, size_t cap, const char* op,
                           int in_count, int out_count, int method) {
    snprintf(buf, cap, "%s in=%d out=%d method=%d", op, in_count, out_count, method);
}

// ---------------- 算子入口：点云文件加载（node.json: alg.point3d.load_cloud） ----------------
// 参数（按 node.json params 声明顺序）：path(path) type(int 0=PCD 1=PLY 2=TXT)
// delimiter(text，仅 TXT 生效——字段分隔符；空 = 按连续空白切分，'#' 行跳过)。
// 输出：点云。消息通道："points=N type=PCD|PLY|TXT"。
// 错误码：1 未设置路径；2 读取失败（-2 文件不存在/无权限/格式错误/无有效数据，
//         -4/-99 内存不足或内部异常）；3 type 非法（-3）/输出组装内存不足。
//（-4 属"读取失败"口径归 2，与接口文档一致；输出容器组装 OOM 归 3——MoveCloudOut 先例）

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.load_cloud","name":"点云文件加载","group":"IO","category":"源","description":"加载 PCD/PLY/TXT 点云文件：TXT 每行 x<分隔符>y<分隔符>z，'#' 注释行跳过、非数字/NaN/Inf 行整行跳过；点数并入节点消息"},
//   "inputs": [],
//   "params": [
//     {"name":"path","label":"点云文件路径","type":"path","default":"","description":"点云文件路径（.pcd / .ply / .txt）"},
//     {"name":"type","label":"文件类型","type":"int","default":0,"min":0,"max":2,"description":"0=PCD(PCL 原生) 1=PLY 2=TXT(每行 x<分隔符>y<分隔符>z)"},
//     {"name":"delimiter","label":"TXT分隔符","type":"text","default":"","description":"仅 TXT 生效：字段分隔符（如 \" \" 或 \",\"；空 = 按连续空白切分）"}
//   ],
//   "outputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudIO.h","src/ShapelyAlg/CloudIO.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiLoadCloud(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    const AlgoCParam* p_path = FindParam(in, 2, 0);
    const char* path = p_path ? p_path->s : nullptr;
    if (!path || !path[0]) { out->error = "未设置点云文件路径"; return 1; }
    int type = 0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) type = p->i;
    const AlgoCParam* p_delim = FindParam(in, 2, 1);
    const char* delim = (p_delim && p_delim->s && p_delim->s[0]) ? p_delim->s : NULL;

    Point3DAlg_PointCloud cloud = {};
    const int rc = Point3DAlg_LoadCloud(path, type, delim, &cloud);
    if (rc == -3) { out->error = "加载失败（type 仅支持 0=PCD/1=PLY/2=TXT）"; return 3; }
    if (rc == -2) { out->error = "加载失败：文件不存在/无权限/格式错误/无有效数据"; return 2; }
    if (rc != 0)  { out->error = "加载失败（内存不足或内部异常）"; return 2; }

    if (!MoveCloudOut(&cloud, out)) { out->error = "内存不足"; return 3; }

    static char msg[128];
    snprintf(msg, sizeof(msg), "points=%d type=%s", cloud.count,
             type == 0 ? "PCD" : (type == 1 ? "PLY" : "TXT"));
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（LoadCloud）

// ---------------- 算子入口：点云滤波/降采样（node.json: alg.point3d.filter_cloud） ----------------
// method 与 params 布局以 Point3DAlg_api.h 注释表为权威口径（method 0~10 全量开放）。
// 固定参数集：node.json 声明 1 个 method(int) + 1 个 field(int) + 20 个 number，
// 适配层按 method 分支组装该布局的 params 数组：
//   0 voxel              [leaf_size, min_points_per_voxel]
//   1 random             [sample_size, seed]
//   2 uniform            [every_k]
//   3 farthest           [num_samples, start_index]
//   4 removeNonFinite    （无参数）
//   5 removeDuplicates   [tolerance]
//   6 passThrough        [field(0=x 1=y 2=z), limit_min, limit_max]
//   7 statisticalOutlier [mean_k, std_dev_mul]
//   8 radiusOutlier      [radius, min_neighbors]
//   9 conditionalXYZ     [x_min,x_max,y_min,y_max,z_min,z_max]（开区间）
//   10 approxVoxel       [leaf]（1.3.0 新增；leaf 复用 node.json 的 leaf_size
//                        参数——语义同为体素边长，格点质心与 method 0 可有
//                        微小偏差，PCL ApproximateVoxelGrid 大点云快速版）
// 消息通道："filter in=N out=M method=K"。
// 错误码：1 输入未连线；2 点云为空/核心层内存不足（-4/-99）；3 参数非法（-3）/输出组装内存不足；
//         4 滤波后无点（-2）。（接口文档口径：输出组装 OOM 归 3，MoveCloudOut 先例）

#ifdef POINT3DALG_HAS_PCL

// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.filter_cloud","name":"点云滤波","group":"3D 点云","category":"处理","description":"点云滤波/降采样（PCL，method 0~9 全量）：体素质心/随机/等间隔/最远点采样、剔除非有限点、去重、直通/条件滤波、统计/半径离群剔除；未用参数保持默认跳过；输出无颜色（查看器按高度伪彩上色），in/out 点数并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"cloud"}
//   ],
//   "params": [
//     {"name":"method","label":"滤波方法","type":"int","default":0,"min":0,"max":9,"description":"0=voxel体素质心降采样 1=random随机采样 2=uniform等间隔采样 3=farthest最远点采样 4=removeNonFinite剔除非有限点 5=removeDuplicates去重 6=passThrough直通滤波 7=statisticalOutlier统计离群剔除 8=radiusOutlier半径离群剔除 9=conditionalXYZ条件滤波"},
//     {"name":"leaf_size","label":"体素边长(method 0)","type":"number","default":0.01,"min":0.0001,"max":1000000,"description":"method 0：体素边长（三轴同值，>0；每格输出质心）"},
//     {"name":"min_points_per_voxel","label":"格最少点数(method 0)","type":"number","default":0,"min":0,"max":1000000,"description":"method 0 可选：每体素最少点数（0 = 不过滤）"},
//     {"name":"sample_size","label":"采样点数(method 1)","type":"number","default":10000,"min":1,"max":100000000,"description":"method 1：随机采样目标点数（>= 点数时返回副本）"},
//     {"name":"seed","label":"随机种子(method 1)","type":"number","default":0,"min":0,"max":2147483647,"description":"method 1 可选：随机种子（固定可复现）"},
//     {"name":"every_k","label":"间隔k(method 2)","type":"number","default":10,"min":1,"max":1000000,"description":"method 2：每 k 点保留 1 个（>0）"},
//     {"name":"num_samples","label":"采样点数(method 3)","type":"number","default":10000,"min":1,"max":100000000,"description":"method 3：最远点采样目标点数"},
//     {"name":"start_index","label":"首采样点(method 3)","type":"number","default":0,"min":0,"max":100000000,"description":"method 3 可选：首采样点索引（默认 0）"},
//     {"name":"tolerance","label":"去重容差(method 5)","type":"number","default":0,"min":0,"max":1000000,"description":"method 5 可选：去重容差（0 = 坐标完全相等才去重）"},
//     {"name":"field","label":"滤波字段(method 6)","type":"int","default":2,"min":0,"max":2,"description":"method 6：直通滤波字段 0=x 1=y 2=z"},
//     {"name":"limit_min","label":"下限(method 6)","type":"number","default":-1000000,"min":-1000000,"max":1000000,"description":"method 6：保留 [下限,上限] 闭区间（默认全量 = 不裁）"},
//     {"name":"limit_max","label":"上限(method 6)","type":"number","default":1000000,"min":-1000000,"max":1000000,"description":"method 6：上限（min > max 报参数错）"},
//     {"name":"mean_k","label":"近邻个数(method 7)","type":"number","default":10,"min":1,"max":1000,"description":"method 7：统计离群剔除的近邻个数（>=1）"},
//     {"name":"std_dev_mul","label":"标准差倍数(method 7)","type":"number","default":1.0,"min":0,"max":100,"description":"method 7：距离均值超过 倍数×σ 判离群（典型 1~2）"},
//     {"name":"radius","label":"搜索半径(method 8)","type":"number","default":0.5,"min":0.0001,"max":1000000,"description":"method 8：半径离群剔除的搜索半径（>0）"},
//     {"name":"min_neighbors","label":"最少邻居(method 8)","type":"number","default":10,"min":0,"max":1000000,"description":"method 8：半径内邻居数低于该值的点剔除"},
//     {"name":"x_min","label":"X下限(method 9)","type":"number","default":-1000000,"min":-1000000,"max":1000000,"description":"method 9：条件滤波 x 下限（开区间，默认全量 = 不裁）"},
//     {"name":"x_max","label":"X上限(method 9)","type":"number","default":1000000,"min":-1000000,"max":1000000,"description":"method 9：x 上限（任一 min>max 报参数错）"},
//     {"name":"y_min","label":"Y下限(method 9)","type":"number","default":-1000000,"min":-1000000,"max":1000000,"description":"method 9：y 下限"},
//     {"name":"y_max","label":"Y上限(method 9)","type":"number","default":1000000,"min":-1000000,"max":1000000,"description":"method 9：y 上限"},
//     {"name":"z_min","label":"Z下限(method 9)","type":"number","default":-1000000,"min":-1000000,"max":1000000,"description":"method 9：z 下限"},
//     {"name":"z_max","label":"Z上限(method 9)","type":"number","default":1000000,"min":-1000000,"max":1000000,"description":"method 9：z 上限"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiFilterCloud(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    int method = 0, field = 2;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) field = p->i;
    if (method < 0 || method > 10) { out->error = "method 仅支持 0~10（见节点参数说明）"; return 3; }
    if (field < 0 || field > 2) { out->error = "field 仅支持 0=x/1=y/2=z"; return 3; }

    double v[20] = { 0 };
    for (int i = 0; i < 20; ++i)
        if (const AlgoCParam* p = FindParam(in, 0, i)) v[i] = p->d;
    // v 声明序（= node.json params 中 number 的声明序）：
    //   0 leaf_size  1 min_points_per_voxel  2 sample_size  3 seed
    //   4 every_k    5 num_samples           6 start_index  7 tolerance
    //   8 limit_min  9 limit_max             10 mean_k      11 std_dev_mul
    //   12 radius    13 min_neighbors        14 x_min 15 x_max
    //   16 y_min 17 y_max  18 z_min 19 z_max
    double params[6];
    int pc = 0;
    switch (method) {
        case 0:  params[0] = v[0];  params[1] = v[1];  pc = 2; break;   // voxel
        case 1:  params[0] = v[2];  params[1] = v[3];  pc = 2; break;   // random
        case 2:  params[0] = v[4];                     pc = 1; break;   // uniform
        case 3:  params[0] = v[5];  params[1] = v[6];  pc = 2; break;   // farthest
        case 4:                                        pc = 0; break;   // removeNonFinite
        case 5:  params[0] = v[7];                     pc = 1; break;   // removeDuplicates
        case 6:  params[0] = (double)field;
                 params[1] = v[8];  params[2] = v[9];  pc = 3; break;   // passThrough
        case 7:  params[0] = v[10]; params[1] = v[11]; pc = 2; break;   // statisticalOutlier
        case 8:  params[0] = v[12]; params[1] = v[13]; pc = 2; break;   // radiusOutlier
        case 10: params[0] = v[0];                     pc = 1; break;   // approxVoxel（leaf 复用 leaf_size）
        default: params[0] = v[14]; params[1] = v[15]; params[2] = v[16];
                 params[3] = v[17]; params[4] = v[18]; params[5] = v[19];
                 pc = 6; break;                                         // conditionalXYZ
    }

    Point3DAlg_PointCloud pin = CloudViewOf(src);
    Point3DAlg_PointCloud outc = {};
    const int rc = Point3DAlg_FilterCloud(&pin, method, pc > 0 ? params : NULL, pc, &outc);
    if (rc == -3) { out->error = "滤波失败（参数与 method 布局不符或取值越界，见 api.h params 表）"; return 3; }
    if (rc == -2) { out->error = "滤波后无点（全部被剔除，检查过滤参数）"; return 4; }
    if (rc != 0)  { out->error = "滤波失败（内存不足或内部异常）"; return 2; }

    if (!MoveCloudOut(&outc, out)) { out->error = "内存不足"; return 3; }

    static char msg[128];
    AppendCountMsg(msg, sizeof(msg), "filter", src->count, outc.count, method);
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（FilterCloud）

// ---------------- 算子入口：点云提取/裁剪（node.json: alg.point3d.extract_cloud） ----------------
// C API method 0~4 中按索引提取（0=byIndices 1=byIndicesInverted）不暴露——
// ComUI 节点间只能传 image/cloud/消息，点集索引无来源；本节点仅开放几何裁剪
// 2=cropBox / 3=cropOBB / 4=cropPolygonROI，method 取值与 C API 编号一致。
// 结构化参数沿用 manual_regist init_transform 的 text 承载先例：
//   cropOBB        obb_params（15 值逗号分隔：center xyz + extents xyz(半长,>0)
//                  + rotation 3x3 行主序）+ obb_invert（1=保留盒外点）
//   cropPolygonROI polygon_xy（顶点 XY 对逗号分隔 "x0,y0,x1,y1,..."，>= 3 顶点）
// 消息通道："extract in=N out=M method=K"。
// 错误码：1 输入未连线；2 点云为空/核心层内存不足（-4/-99）；3 参数非法（-3 / 文本解析失败
//         / 输出组装内存不足）；4 裁剪后无点（-2）。（输出组装 OOM 归 3，MoveCloudOut 先例）

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.extract_cloud","name":"点云裁剪","group":"3D 点云","category":"处理","description":"点云几何裁剪（PCL）：2=AABB 盒裁剪（6 参数） 3=有向盒裁剪（obb_params 15 值文本，可反选盒外） 4=多边形 ROI 裁剪（polygon_xy 顶点对文本，XY 投影射线法）；按索引提取不开放（节点间无点集索引来源）；裁剪后点数并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"cloud"}
//   ],
//   "params": [
//     {"name":"method","label":"裁剪方法","type":"int","default":2,"min":2,"max":4,"description":"2=cropBox AABB 盒裁剪(闭区间) 3=cropOBB 有向盒裁剪 4=cropPolygonROI 多边形裁剪；与 C API method 编号一致（0/1 按索引提取不开放）"},
//     {"name":"box_min_x","label":"盒X下限(method 2)","type":"number","default":-1000000,"min":-1000000,"max":1000000,"description":"method 2：AABB 盒 x 下限（默认全量 = 不裁）"},
//     {"name":"box_min_y","label":"盒Y下限(method 2)","type":"number","default":-1000000,"min":-1000000,"max":1000000,"description":"method 2：AABB 盒 y 下限"},
//     {"name":"box_min_z","label":"盒Z下限(method 2)","type":"number","default":-1000000,"min":-1000000,"max":1000000,"description":"method 2：AABB 盒 z 下限（任一 min>max 报参数错）"},
//     {"name":"box_max_x","label":"盒X上限(method 2)","type":"number","default":1000000,"min":-1000000,"max":1000000,"description":"method 2：AABB 盒 x 上限"},
//     {"name":"box_max_y","label":"盒Y上限(method 2)","type":"number","default":1000000,"min":-1000000,"max":1000000,"description":"method 2：AABB 盒 y 上限"},
//     {"name":"box_max_z","label":"盒Z上限(method 2)","type":"number","default":1000000,"min":-1000000,"max":1000000,"description":"method 2：AABB 盒 z 上限"},
//     {"name":"obb_params","label":"有向盒参数(method 3)","type":"text","default":"0,0,0,1000,1000,1000,1,0,0,0,1,0,0,0,1","description":"method 3：15 值逗号分隔 = 盒中心 xyz + 三主轴半长 xyz(>0) + 旋转 3x3 行主序（第 d 行 = 第 d 主轴方向）"},
//     {"name":"obb_invert","label":"反选盒外(method 3)","type":"int","default":0,"min":0,"max":1,"description":"method 3 可选：1=保留盒外点"},
//     {"name":"polygon_xy","label":"多边形顶点(method 4)","type":"text","default":"","description":"method 4：顶点 XY 对逗号分隔 \"x0,y0,x1,y1,...\"（>= 3 个顶点，XY 投影 + 射线法）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiExtractCloud(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    int method = 2, obb_invert = 0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) obb_invert = p->i;
    if (method < 2 || method > 4) {
        out->error = "method 仅支持 2=cropBox/3=cropOBB/4=cropPolygonROI（按索引提取无索引来源，不开放）";
        return 3;
    }

    double v[6] = { 0 };
    for (int i = 0; i < 6; ++i)
        if (const AlgoCParam* p = FindParam(in, 0, i)) v[i] = p->d;
    // v 声明序：0 box_min_x 1 box_min_y 2 box_min_z 3 box_max_x 4 box_max_y 5 box_max_z
    double box_obb[16];
    double poly[512];          // cropPolygonROI 顶点缓冲（最多 256 个顶点）
    const double* params = box_obb;
    int pc = 0;
    if (method == 2) {
        for (int i = 0; i < 6; ++i) box_obb[i] = v[i];
        pc = 6;
    } else if (method == 3) {
        const AlgoCParam* p = FindParam(in, 2, 0);
        const int n = p ? ParseDoubleList(p->s, box_obb, 15) : 0;
        if (n < 0) { out->error = "obb_params 含非数值内容（需逗号分隔数字）"; return 3; }
        if (n != 15) {
            out->error = "obb_params 需 15 个数值（center xyz, extents xyz, rotation 3x3 行主序）";
            return 3;
        }
        box_obb[15] = (obb_invert != 0) ? 1.0 : 0.0;
        pc = 16;
    } else {
        const AlgoCParam* p = FindParam(in, 2, 1);
        pc = p ? ParseDoubleList(p->s, poly, 512) : 0;
        if (pc < 0) { out->error = "polygon_xy 含非数值内容（需逗号分隔数字）"; return 3; }
        if (pc < 6 || (pc % 2) != 0) {
            out->error = "polygon_xy 需 >= 3 个顶点 XY 对（\"x0,y0,x1,y1,...\" 逗号分隔）";
            return 3;
        }
        params = poly;
    }

    Point3DAlg_PointCloud pin = CloudViewOf(src);
    Point3DAlg_PointCloud outc = {};
    const int rc = Point3DAlg_ExtractCloud(&pin, method, NULL, 0, params, pc, &outc);
    if (rc == -3) { out->error = "裁剪失败（盒参数 min>max / OBB extents<=0 / 顶点不足等，见 api.h 布局表）"; return 3; }
    if (rc == -2) { out->error = "裁剪后无点（盒/ROI 内无点，检查裁剪范围）"; return 4; }
    if (rc != 0)  { out->error = "裁剪失败（内存不足或内部异常）"; return 2; }

    if (!MoveCloudOut(&outc, out)) { out->error = "内存不足"; return 3; }

    static char msg[128];
    AppendCountMsg(msg, sizeof(msg), "extract", src->count, outc.count, method);
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（ExtractCloud）

// ---------------- 算子入口：点云空间变换（node.json: alg.point3d.transform_cloud） ----------------
// method 0~6 全量开放（布局以 Point3DAlg_api.h 注释表为权威口径）：
//   0 applyMatrix   transform_matrix（text，4x4 行主序 16 值逗号分隔——
//                   manual_regist init_transform 同款约定，默认单位阵）
//   1 translate     dx dy dz
//   2 rotate        axis xyz（过原点，内部归一化）+ rotate_angle（弧度，右手系）
//   3 rotateAround  pivot xyz + axis xyz + rotate_angle
//   4 scale         scale xyz
//   5 scaleAround   scale xyz + center xyz（p' = c + s·(p - c)）
//   6 project       平面 ax+by+cz+d=0（plane_a/b/c/d；法向零向量 → -3）
// 消息通道："transform in=N out=M method=K"。
// 错误码：1 输入未连线；2 点云为空/核心层内存不足（-4/-99）；3 参数非法（-3 / 矩阵解析失败
//         / 输出组装内存不足）。（输出组装 OOM 归 3，MoveCloudOut 先例）

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.transform_cloud","name":"点云变换","group":"3D 点云","category":"处理","description":"点云空间变换（PCL，method 0~6 全量）：应用 4x4 矩阵（文本，manual_regist 同款）/平移/绕轴旋转/绕点轴旋转/三轴缩放/绕点缩放/投影到平面；角度单位为弧度（API 口径）；变换前后点数并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"cloud"}
//   ],
//   "params": [
//     {"name":"method","label":"变换方法","type":"int","default":1,"min":0,"max":6,"description":"0=applyMatrix 应用4x4矩阵 1=translate平移 2=rotate绕轴旋转(过原点) 3=rotateAround绕点轴旋转 4=scale三轴缩放 5=scaleAround绕点缩放 6=project投影到平面"},
//     {"name":"transform_matrix","label":"4x4矩阵(method 0)","type":"text","default":"1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1","description":"method 0：4x4 行主序 16 值逗号分隔（[r00 r01 r02 tx | r10 r11 r12 ty | r20 r21 r22 tz | 0 0 0 1]，manual_regist init_transform 同款）"},
//     {"name":"dx","label":"平移dx(method 1)","type":"number","default":0,"description":"method 1：X 平移量"},
//     {"name":"dy","label":"平移dy(method 1)","type":"number","default":0,"description":"method 1：Y 平移量"},
//     {"name":"dz","label":"平移dz(method 1)","type":"number","default":0,"description":"method 1：Z 平移量"},
//     {"name":"axis_x","label":"旋转轴X(method 2/3)","type":"number","default":0,"description":"method 2/3：旋转轴 x 分量（过原点/pivot，不需单位化，内部归一；零向量报参数错）"},
//     {"name":"axis_y","label":"旋转轴Y(method 2/3)","type":"number","default":0,"description":"method 2/3：旋转轴 y 分量"},
//     {"name":"axis_z","label":"旋转轴Z(method 2/3)","type":"number","default":1,"description":"method 2/3：旋转轴 z 分量（默认绕 Z 轴）"},
//     {"name":"rotate_angle","label":"旋转角弧度(method 2/3)","type":"number","default":0,"description":"method 2/3：旋转角（弧度，右手系；度×π/180 换算）"},
//     {"name":"pivot_x","label":"轴过点X(method 3)","type":"number","default":0,"description":"method 3：旋转轴所过点 x（pivot）"},
//     {"name":"pivot_y","label":"轴过点Y(method 3)","type":"number","default":0,"description":"method 3：旋转轴所过点 y"},
//     {"name":"pivot_z","label":"轴过点Z(method 3)","type":"number","default":0,"description":"method 3：旋转轴所过点 z"},
//     {"name":"scale_x","label":"缩放sx(method 4/5)","type":"number","default":1,"description":"method 4/5：X 缩放系数"},
//     {"name":"scale_y","label":"缩放sy(method 4/5)","type":"number","default":1,"description":"method 4/5：Y 缩放系数"},
//     {"name":"scale_z","label":"缩放sz(method 4/5)","type":"number","default":1,"description":"method 4/5：Z 缩放系数"},
//     {"name":"center_x","label":"缩放中心X(method 5)","type":"number","default":0,"description":"method 5：缩放中心 x（p' = c + s·(p - c)）"},
//     {"name":"center_y","label":"缩放中心Y(method 5)","type":"number","default":0,"description":"method 5：缩放中心 y"},
//     {"name":"center_z","label":"缩放中心Z(method 5)","type":"number","default":0,"description":"method 5：缩放中心 z"},
//     {"name":"plane_a","label":"平面a(method 6)","type":"number","default":0,"description":"method 6：投影平面 ax+by+cz+d=0 的 a（法向 (a,b,c) 零向量报参数错）"},
//     {"name":"plane_b","label":"平面b(method 6)","type":"number","default":0,"description":"method 6：平面方程系数 b"},
//     {"name":"plane_c","label":"平面c(method 6)","type":"number","default":1,"description":"method 6：平面方程系数 c（默认投影到 z=0 平面）"},
//     {"name":"plane_d","label":"平面d(method 6)","type":"number","default":0,"description":"method 6：平面方程常数 d"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiTransformCloud(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    int method = 1;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (method < 0 || method > 6) { out->error = "method 仅支持 0~6（见节点参数说明）"; return 3; }

    double v[20] = { 0 };
    for (int i = 0; i < 20; ++i)
        if (const AlgoCParam* p = FindParam(in, 0, i)) v[i] = p->d;
    // v 声明序（= node.json params 中 number 的声明序）：
    //   0 dx  1 dy  2 dz  3 axis_x 4 axis_y 5 axis_z 6 rotate_angle
    //   7 pivot_x 8 pivot_y 9 pivot_z  10 scale_x 11 scale_y 12 scale_z
    //   13 center_x 14 center_y 15 center_z  16 plane_a 17 plane_b 18 plane_c 19 plane_d
    double params[16];
    int pc = 0;
    if (method == 0) {
        const AlgoCParam* p = FindParam(in, 2, 0);
        const int n = p ? ParseDoubleList(p->s, params, 16) : 0;
        if (n < 0) { out->error = "transform_matrix 含非数值内容（需逗号分隔数字）"; return 3; }
        if (n != 16) { out->error = "transform_matrix 需 16 个数值（4x4 行主序，逗号分隔）"; return 3; }
        pc = 16;
    } else if (method == 1) {                       // translate
        params[0] = v[0]; params[1] = v[1]; params[2] = v[2];
        pc = 3;
    } else if (method == 2) {                       // rotate（axis 过原点 + 弧度角）
        params[0] = v[3]; params[1] = v[4]; params[2] = v[5]; params[3] = v[6];
        pc = 4;
    } else if (method == 3) {                       // rotateAround（pivot + axis + 弧度角）
        params[0] = v[7]; params[1] = v[8]; params[2] = v[9];
        params[3] = v[3]; params[4] = v[4]; params[5] = v[5]; params[6] = v[6];
        pc = 7;
    } else if (method == 4) {                       // scale
        params[0] = v[10]; params[1] = v[11]; params[2] = v[12];
        pc = 3;
    } else if (method == 5) {                       // scaleAround（scale + center）
        params[0] = v[10]; params[1] = v[11]; params[2] = v[12];
        params[3] = v[13]; params[4] = v[14]; params[5] = v[15];
        pc = 6;
    } else {                                        // project（平面 ax+by+cz+d=0）
        params[0] = v[16]; params[1] = v[17]; params[2] = v[18]; params[3] = v[19];
        pc = 4;
    }

    Point3DAlg_PointCloud pin = CloudViewOf(src);
    Point3DAlg_PointCloud outc = {};
    const int rc = Point3DAlg_TransformCloud(&pin, method, params, pc, &outc);
    if (rc == -3) { out->error = "变换失败（参数与 method 布局不符，或旋转轴/平面法向为零向量）"; return 3; }
    if (rc != 0)  { out->error = "变换失败（内存不足或内部异常）"; return 2; }

    if (!MoveCloudOut(&outc, out)) { out->error = "内存不足"; return 3; }

    static char msg[128];
    AppendCountMsg(msg, sizeof(msg), "transform", src->count, outc.count, method);
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（TransformCloud）

// =====================================================================
// §10 零成本缺口节点接入（2026-11-19，4 个入口，追加在文件尾部）
//   alg.point3d.apply_transform  —— RegistApplyTransform（+Load/ParseTransformText）
//   alg.point3d.save_transform   —— RegistSaveTransform / RegistSaveTransformFlat
//   alg.point3d.dome_fit_base    —— DomeFitBase（全云稳健基面，诊断用）
//   alg.point3d.fit_circle_2d    —— FitCircle2D（XY 投影单圆拟合）
// 本段只做参数组装、错误码映射与消息拼装，不新增算法；变换文本
// 解析沿用 manual_regist init_transform 先例（RegistParseTransformText：
// 4x4 多行 / 单行 16 值 / 带键名整行均可，凑满 16 个数值即成功）。
// =====================================================================

// ---------------- 算子入口：应用已有变换（node.json: alg.point3d.apply_transform） ----------------
// 参数（按 node.json params 声明顺序）：init_transform(text) transform_file(path)
// 取值优先级：init_transform 非空 → RegistParseTransformText 解析文本；
//            否则 transform_file 非空 → RegistLoadTransform 读文件（RegistTool.exe
//            导出的 regist_T.txt / regist_T_flat.txt 或 save_transform 节点输出均可）；
//            两者均空 → 参数缺失（应用单位阵无意义，不静默放行）。
// 与 manual_regist 的边界：manual_regist 是"两片云 + ICP 求变换"的配准流程节点，
// 本节点不做任何配准计算，只把现成变换套到任意点云上（配准链路的下游一步）。
// 与 transform_cloud 的边界：transform_cloud 是通用变换节点（method 0~6，仅收
// 内联矩阵文本），本节点专做配准变换套用且支持变换文件（全库唯一能读变换
// 文件的节点），并可作 save_transform 的下游闭环。
// 输入：一片点云；输出：变换后点云（rgb 不随行，查看器按高度伪彩上色）。
// 消息通道：成功时回显 T 的 flat 16 值串（可直接粘进 manual_regist /
//           save_transform 等节点的矩阵参数——文件→参数互转通道）。
// 错误码：1 输入未连线 / 未提供变换；2 点云为空 / 变换文件读取失败；
//         3 矩阵文本解析失败 / 内存不足。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.apply_transform","name":"应用已有变换","group":"3D 点云","category":"处理","description":"把现成 4x4 变换套到任意点云（RegistApplyTransform）：优先解析 init_transform 文本（manual_regist 消息 T= 后内容可直接粘贴），否则读 transform_file（RegistTool.exe 导出或 save_transform 节点输出）；不做配准计算，是手动配准链路的下游一步；消息回显 flat 矩阵可粘回参数"},
//   "inputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"init_transform","label":"变换矩阵(16值,优先)","type":"text","default":"","description":"4x4 行主序 16 值（逗号/空格分隔，允许直接粘贴 4x4 多行文本或 manual_regist 消息中 T= 后内容）；非空时优先生效"},
//     {"name":"transform_file","label":"变换文件(文本为空时读取)","type":"path","default":"","description":"变换文件路径：RegistTool.exe 导出的 regist_T.txt / regist_T_flat.txt 或 save_transform 节点输出（4x4 多行或单行 16 值均可）；init_transform 为空时读取"}
//   ],
//   "outputs": [
//     {"name":"变换后点云","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/tools/Point3DAlg_Regist.h","src/Point3DAlg/tools/Point3DAlg_Regist.cpp","src/ShapelyAlg/ShapelyAlg.h"], "includes": ["src","src/Point3DAlg/tools","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiApplyTransform(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    // 参数提取（text 两个：第 1 个 = init_transform，第 2 个 = transform_file）
    const AlgoCParam* p_text = FindParam(in, 2, 0);
    const AlgoCParam* p_file = FindParam(in, 2, 1);
    const char* tfText = (p_text && p_text->s && p_text->s[0]) ? p_text->s : nullptr;
    const char* tfFile = (p_file && p_file->s && p_file->s[0]) ? p_file->s : nullptr;

    float T[16];
    if (tfText) {
        if (!Point3DAlg_RegistParseTransformText(tfText, T)) {
            out->error = "init_transform 解析失败（需 4x4 共 16 个数值，逗号/空格分隔或直接粘贴矩阵）";
            return 3;
        }
    } else if (tfFile) {
        if (!Point3DAlg_RegistLoadTransform(tfFile, T)) {
            out->error = "变换文件读取失败（文件不存在或内容凑不满 16 个数值）";
            return 2;
        }
    } else {
        out->error = "未提供变换：init_transform 与 transform_file 至少填一项";
        return 1;
    }

    // SoA → PCL → RegistApplyTransform → SoA（manual_regist 输出段同款）
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
    cloud->reserve(src->count);
    for (int i = 0; i < src->count; ++i)
        cloud->push_back(pcl::PointXYZ(src->xyz[i * 3], src->xyz[i * 3 + 1], src->xyz[i * 3 + 2]));
    pcl::PointCloud<pcl::PointXYZ>::Ptr moved = Point3DAlg_RegistApplyTransform(cloud, T);

    float* outXyz = (float*)malloc((size_t)moved->size() * 3 * sizeof(float));
    if (!outXyz) { out->error = "内存不足"; return 3; }
    for (size_t i = 0; i < moved->size(); ++i) {
        outXyz[i * 3]     = (*moved)[i].x;
        outXyz[i * 3 + 1] = (*moved)[i].y;
        outXyz[i * 3 + 2] = (*moved)[i].z;
    }
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(outXyz); out->error = "内存不足"; return 3; }
    arr->count = (int)moved->size();
    arr->xyz = outXyz;
    arr->rgb = nullptr;
    out->clouds = arr;
    out->cloudCount = 1;

    static char msgBuf[384];
    snprintf(msgBuf, sizeof(msgBuf), "apply in=%d out=%d | T=%s",
             src->count, arr->count, Point3DAlg_RegistFormatFlat(T).c_str());
    out->error = msgBuf;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（ApplyTransform）

// ---------------- 算子入口：变换持久化（node.json: alg.point3d.save_transform） ----------------
// 参数（按 node.json params 声明顺序）：init_transform(text) format(int) path(path)
// init_transform：16 值（manual_regist 消息 T= 后的内容可直接粘贴，宽容解析同款）；
// format：0 = RegistSaveTransform（4x4 多行文本，RegistTool.exe 可直接打开）
//         1 = RegistSaveTransformFlat（单行 16 值逗号分隔，可直接粘回 node.json 参数）
// path：输出文件路径（必填）。无输入/输出端口——纯副作用节点（THIRD_PARTY_OPS
//       §3.3 允许），结果走消息通道；与 apply_transform 组成"参数↔文件"互转闭环：
//       参数→文件用本节点，文件→参数用 apply_transform（消息回显 flat 串）。
// 错误码：1 未设置保存路径；2 保存失败（目录不存在/无写权限）；3 矩阵文本解析失败
//         / format 非法。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.save_transform","name":"变换保存","group":"3D 点云","category":"输出","description":"把 16 值变换矩阵写入文件（RegistSaveTransform/Flat）：format 0=4x4 多行文本（RegistTool.exe 可直接打开）或 1=单行 16 值逗号分隔（可直接粘回 node.json 参数）；无输入输出端口，确认与 flat 矩阵回显走节点消息；文件→参数方向用 应用已有变换 节点"},
//   "inputs": [],
//   "params": [
//     {"name":"init_transform","label":"变换矩阵(16值)","type":"text","default":"1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1","description":"4x4 行主序 16 值（逗号/空格分隔，允许直接粘贴 4x4 多行文本或 manual_regist 消息中 T= 后内容）"},
//     {"name":"format","label":"文件格式(0=4x4,1=单行)","type":"int","default":1,"min":0,"max":1,"description":"0=4x4 多行文本（RegistSaveTransform）；1=单行 16 值逗号分隔（RegistSaveTransformFlat）"},
//     {"name":"path","label":"保存路径","type":"path","default":"","description":"输出文件路径（必填）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/tools/Point3DAlg_Regist.h","src/Point3DAlg/tools/Point3DAlg_Regist.cpp","src/ShapelyAlg/ShapelyAlg.h"], "includes": ["src","src/Point3DAlg/tools","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiSaveTransform(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));

    // 参数提取（text 两个：第 1 个 = init_transform，第 2 个 = path；int 第 1 个 = format）
    float T[16];
    const AlgoCParam* p_text = FindParam(in, 2, 0);
    if (!p_text || !p_text->s || !Point3DAlg_RegistParseTransformText(p_text->s, T)) {
        out->error = "init_transform 解析失败（需 4x4 共 16 个数值；manual_regist 消息 T= 后内容可直接粘贴）";
        return 3;
    }
    int fmt = 1;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) fmt = p->i;
    if (fmt != 0 && fmt != 1) { out->error = "format 仅支持 0=4x4 多行 / 1=单行16值"; return 3; }
    const AlgoCParam* p_path = FindParam(in, 2, 1);
    const char* path = (p_path && p_path->s && p_path->s[0]) ? p_path->s : nullptr;
    if (!path) { out->error = "未设置保存路径"; return 1; }

    const bool ok = (fmt == 0) ? Point3DAlg_RegistSaveTransform(path, T)
                               : Point3DAlg_RegistSaveTransformFlat(path, T);
    if (!ok) { out->error = "保存失败（目录不存在或无写权限）"; return 2; }

    static char msgBuf[512];
    snprintf(msgBuf, sizeof(msgBuf), "已保存 %s（%s）| T=%s", path,
             fmt == 0 ? "4x4 文本" : "flat 16值",
             Point3DAlg_RegistFormatFlat(T).c_str());
    out->error = msgBuf;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（SaveTransform）

// ---------------- 算子入口：稳健基面拟合（node.json: alg.point3d.dome_fit_base） ----------------
// Point3DAlg_DomeFitBase：全点云迭代修剪最小二乘平面（每轮剔除 > 中位数 +
// factor·MAD 的上方出点再从全点重建内点集，拱形 bump/凸台不干扰基面）。
// 定位与既有节点的分工：
//   dome_fit        —— 必须检出 bump 才有输出（无 bump 簇即失败），本节点对
//                      纯平面云同样可用，专供基面诊断（法向/方程/σ/内点）；
//   region_plane    —— 矩形 ROI 内普通最小二乘（ROI 手填、不去凸点），本节点
//                      全云自动修剪，面向"带凸台的基面"。
// 参数（按 node.json params 声明顺序）：base_mad_factor(number) base_iterations(int)
//（DomeFitParams 其余字段不参与基面拟合，不暴露）
// 输入：一片点云（≥10 点，mm）；无云输出，结果并入节点消息：
//   n·p+d=0 与 z=a·x+b·y+c 两种口径 + σ + 内点数。
// 错误码：1 输入未连线；2 点云为空；3 内存不足；4 基面失败（<10 点/退化）。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.dome_fit_base","name":"稳健基面拟合","group":"3D 点云","category":"测量","description":"全点云迭代修剪最小二乘平面（剔除上方出点，拱形 bump/凸台不干扰，DomeFitBase）：法向/平面方程/σ/内点数并入节点消息；与 拱形球顶拟合 的分工是不需要检出 bump、纯平面云同样可用；与 ROI平面量测 的分工是全云自动去凸点而非矩形 ROI 内普通最小二乘"},
//   "inputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"base_mad_factor","label":"修剪因子","type":"number","default":3.0,"min":0.0,"description":"每轮剔除高度 > 中位数 + factor·MAD 的上方出点"},
//     {"name":"base_iterations","label":"修剪迭代次数","type":"int","default":5,"min":1,"max":100,"description":"基面修剪迭代轮数（每轮从全点重建内点集）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/tools/Point3DAlg_DomeFit.h","src/Point3DAlg/tools/Point3DAlg_DomeFit.cpp","src/ShapelyAlg/ShapelyAlg.h"], "includes": ["src","src/Point3DAlg/tools","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiDomeFitBase(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    double madFactor = 3.0;
    int iters = 5;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) madFactor = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) iters = p->i;
    if (madFactor < 0.0) madFactor = 0.0;
    if (iters < 1) iters = 1;

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
    cloud->reserve(src->count);
    for (int i = 0; i < src->count; ++i)
        cloud->push_back(pcl::PointXYZ(src->xyz[i * 3], src->xyz[i * 3 + 1], src->xyz[i * 3 + 2]));

    Point3DAlg_DomeFitParams p;   // 仅 base_mad_factor / base_iterations 参与基面拟合
    p.base_mad_factor = madFactor;
    p.base_iterations = iters;
    Point3DAlg_DomeBase base;
    if (!Point3DAlg_DomeFitBase(cloud, p, base)) {
        out->error = "基面拟合失败：点数 < 10 或平面退化（法向无法确定）";
        return 4;
    }

    // z=a·x+b·y+c 由 n·p+d=0 反解（本算法法向恒朝 z+，normal[2]>0，无除零）
    static char msgBuf[384];
    snprintf(msgBuf, sizeof(msgBuf),
             "基面 n=(%.6f, %.6f, %.6f) d=%.6f | z=%.6f%+.6f·x%+.6f·y | σ=%.6f 内点 %d/%d",
             base.normal[0], base.normal[1], base.normal[2], base.d,
             -base.d / base.normal[2],
             -base.normal[0] / base.normal[2],
             -base.normal[1] / base.normal[2],
             base.sigma, base.inlier_count, src->count);
    out->error = msgBuf;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（DomeFitBase）

// ---------------- 算子入口：XY 圆拟合（node.json: alg.point3d.fit_circle_2d） ----------------
// Point3DAlg_FitCircle2D：对输入点集的 XY 投影做单圆拟合（Kåsa 代数解 + 双侧
// MAD 修剪重拟合），z 不参与。与 circle_fit 节点的分工：
//   circle_fit   —— 绝对高度带切片 + 聚类 → 一次检出多孔（完整孔位检测流程）；
//   fit_circle_2d —— 假设输入已是同一圆的点（extract_cloud 裁出的孔壁/轮廓/
//                   边缘点），不做切片与聚类，对 z 混乱的点同样适用。
// 参数（按 node.json params 声明顺序，number×4 → int）：
//   radius_min radius_max rms_max mad_factor iterations
// 输入：一片点云（XY 投影，≥3 点）；输出：单点云（x=圆心x y=圆心y z=半径，
// circle_fit 节点同款口径），详情并入消息。
// 错误码：1 输入未连线；2 点云为空；3 内存不足；4 拟合失败（修剪后 <3 点/
//         退化/半径越界/残差超限——FitCircle2D 均以 false 表达）。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.fit_circle_2d","name":"XY圆拟合(单圆)","group":"3D 点云","category":"测量","description":"对输入点的 XY 投影做单圆拟合（Kåsa 代数解 + MAD 修剪重拟合，FitCircle2D，z 不参与）：输出单点（x=圆心x y=圆心y z=半径），详情并入消息；与 圆孔位量测(circle_fit) 的分工：circle_fit 切高度带+聚类一次检出多孔，本节点假设输入已是同一圆的点（extract_cloud 裁剪后的孔壁/轮廓/边缘点）"},
//   "inputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"radius_min","label":"半径下限(mm)","type":"number","default":0.05,"min":0.0,"description":"半径合理下限（越界判拟合失败）"},
//     {"name":"radius_max","label":"半径上限(mm)","type":"number","default":50.0,"min":0.01,"description":"半径合理上限（越界判拟合失败）"},
//     {"name":"rms_max","label":"残差上限(mm)","type":"number","default":0.02,"min":0.0,"description":"拟合残差 RMS 上限（超限判非圆/失败）"},
//     {"name":"mad_factor","label":"修剪因子","type":"number","default":3.0,"min":0.0,"description":"残差双侧 MAD 修剪因子（剔除 |dist-R| 离群点后重拟合）"},
//     {"name":"iterations","label":"修剪迭代次数","type":"int","default":3,"min":1,"max":100,"description":"修剪重拟合轮数"}
//   ],
//   "outputs": [
//     {"name":"圆心点","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/tools/Point3DAlg_CircleFit.h","src/Point3DAlg/tools/Point3DAlg_CircleFit.cpp","src/Point3DAlg/tools/Point3DAlg_FitUtil.h"], "includes": ["src","src/Point3DAlg/tools"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiFitCircle2D(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }
    if (src->count < 3) { out->error = "点数 < 3，无法拟合圆"; return 4; }

    double rMin = 0.05, rMax = 50.0, rmsMax = 0.02, madFactor = 3.0;
    int iters = 3;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) rMin = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) rMax = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) rmsMax = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) madFactor = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) iters = p->i;
    if (iters < 1) iters = 1;

    // XY 投影（z 不参与；x,y 交替 double SoA，FitCircle2D 入参口径）
    std::vector<double> xy((size_t)src->count * 2);
    for (int i = 0; i < src->count; ++i) {
        xy[(size_t)i * 2]     = src->xyz[i * 3];
        xy[(size_t)i * 2 + 1] = src->xyz[i * 3 + 1];
    }

    Point3DAlg_CircleFitResult c;
    if (!Point3DAlg_FitCircle2D(xy.data(), src->count, madFactor, iters,
                                rMin, rMax, rmsMax, c)) {
        static char errBuf[256];
        snprintf(errBuf, sizeof(errBuf),
                 "圆拟合失败：修剪后 <3 点/退化/半径越界（%.3f~%.3f）/残差超限（rms_max=%.4f）",
                 rMin, rMax, rmsMax);
        out->error = errBuf;
        return 4;
    }

    // 输出单点（x=圆心x, y=圆心y, z=半径；circle_fit 节点同款口径）
    float* outXyz = (float*)malloc(3 * sizeof(float));
    if (!outXyz) { out->error = "内存不足"; return 3; }
    outXyz[0] = (float)c.center[0];
    outXyz[1] = (float)c.center[1];
    outXyz[2] = (float)c.radius;
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(outXyz); out->error = "内存不足"; return 3; }
    arr->count = 1;
    arr->xyz = outXyz;
    arr->rgb = nullptr;
    out->clouds = arr;
    out->cloudCount = 1;

    static char msgBuf[320];
    snprintf(msgBuf, sizeof(msgBuf),
             "圆心 (%.4f, %.4f) R=%.4f rms=%.5f 内点 %d/%d",
             c.center[0], c.center[1], c.radius, c.rms, c.point_count, src->count);
    out->error = msgBuf;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（FitCircle2D）

// =====================================================================
// 1.2.0 批次接入（2026-11-19，5 个入口，追加在文件尾部）：
//   alg.point3d.fit_model        —— FitModel（out_coeffs 布局逐 method；
//                                   内点索引为库 malloc 配 FreeBuffer）
//   alg.point3d.measure_cloud    —— MeasureCloud（method 0~7 全量；方法 4 需第二片云）
//   alg.point3d.segment_cloud    —— SegmentCloud（+FreeClouds；输出点数最多的一段）
//   alg.point3d.height_map       —— CloudToHeightMap（+FreeHeightMap；3D→2D 显示桥）
//   alg.point3d.detect_keypoints —— DetectKeypoints（Harris3D/ISS3D）
// 本段只做参数组装、错误码映射与消息拼装，不新增算法；内存契约：二级指针输出
//（inliers/segments）由库 malloc 配 Free*/FreeBuffer；segment_cloud 取最大段的
// xyz 拷贝出后立即 FreeClouds 全量回收（不转移段指针所有权）；detect_keypoints
// 的输出 xyz 走 MoveCloudOut 转移（转移后不得 FreeCloud）。固定参数集沿用
// filter_cloud 先例：node.json 声明该节点全部参数（同 kind 按声明序区分），
// 适配层按 method 只组装该分支用到的 params 数组，未用参数保持默认跳过。
// =====================================================================

// ---------------- 算子入口：通用几何拟合（node.json: alg.point3d.fit_model） ----------------
// 参数（按 node.json params 声明顺序）：method(int) fit(int) distance_threshold(number)
// out_coeffs 布局（Point3DAlg_api.h 权威口径）：
//   method 0 plane    [a,b,c,d]（ax+by+cz+d=0，|n|=1）
//   method 1 sphere   [cx,cy,cz,r]
//   method 2 cylinder [px,py,pz, dx,dy,dz(单位轴), r]
//   method 3 circle3D [cx,cy,cz, nx,ny,nz(圆平面单位法向), r]
//   method 4 cone     [ax,ay,az, dx,dy,dz(单位轴), angle(半开角弧度)]
//   method 5 line3D   [px,py,pz, dx,dy,dz(单位方向)]（1.3.0 新增，6 值）
// 模型支持度（PCL 1.9 限制，底层自动回退不报错）：sphere 的 MSAC 回退 RANSAC；
// cylinder/cone 仅 RANSAC（fit 参数被忽略）；circle3D 的 fit/threshold 复用内部
// 平面拟合口径；plane/line3D 三种 fit 均支持。out_coeffs 预分配 8（>= 布局最长
// 7）免两段式调用。
// 输入：一片点云；无云输出，系数与内点数并入节点消息（manual_regist 先例）。
// 错误码：1 输入未连线；2 点云为空/内存不足；3 参数非法（-3）；4 拟合失败（-2：
//         无局内点/退化——点数不足、投影共线、圆锥不收敛）。

#ifdef POINT3DALG_HAS_PCL

// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.fit_model","name":"通用几何拟合","group":"3D 点云","category":"处理","description":"通用几何拟合（RANSAC/MSAC/LMS）：method 0=平面[a,b,c,d] 1=球[cx,cy,cz,r] 2=圆柱[轴点+单位轴向+半径] 3=3D圆[圆心+圆平面法向+半径] 4=圆锥[顶点+单位轴向+半开角弧度]；系数布局与内点数并入节点消息（圆柱/圆锥仅 RANSAC——PCL 1.9 限制，fit 参数被忽略；球 MSAC 自动回退 RANSAC）"},
//   "inputs": [
//     {"name":"输入","type":"cloud"}
//   ],
//   "params": [
//     {"name":"method","label":"拟合模型","type":"int","default":0,"min":0,"max":4,"description":"0=plane 平面 1=sphere 球 2=cylinder 圆柱 3=circle3D 三维圆 4=cone 圆锥"},
//     {"name":"fit","label":"拟合方法","type":"int","default":0,"min":0,"max":2,"description":"0=RANSAC 1=MSAC 2=LMS(LMEDS)；sphere 不支持 MSAC（回退 RANSAC），cylinder/cone 仅 RANSAC"},
//     {"name":"distance_threshold","label":"内点距离阈值","type":"number","default":0.05,"min":0.0001,"max":1000000,"description":"局内点判定距离阈值（与坐标同单位，>0）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiFitModel(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    int method = 0, fit = 0;
    double dist_thr = 0.05;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) fit = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) dist_thr = p->d;
    if (method < 0 || method > 5) {
        out->error = "method 仅支持 0=plane/1=sphere/2=cylinder/3=circle3D/4=cone/5=line3D"; return 3;
    }
    if (fit < 0 || fit > 2) { out->error = "fit 仅支持 0=RANSAC/1=MSAC/2=LMS"; return 3; }
    if (dist_thr <= 0.0) { out->error = "distance_threshold 须 > 0"; return 3; }

    Point3DAlg_PointCloud pin = CloudViewOf(src);
    double coeffs[8] = { 0 };
    int coeff_count = 0;
    int* inliers = NULL;
    int inlier_count = 0;
    const int rc = Point3DAlg_FitModel(&pin, method, fit, dist_thr, coeffs, 8,
                                       &inliers, &inlier_count);
    if (rc == -3) { out->error = "拟合失败（method/fit 越界或 distance_threshold<=0）"; return 3; }
    if (rc == -2) { out->error = "拟合失败：无局内点/退化（点数不足、投影共线、圆锥不收敛）"; return 4; }
    if (rc != 0)  { out->error = "拟合失败（内存不足或内部异常）"; return 2; }

    static char msg[512];
    switch (method) {
        case 0:
            snprintf(msg, sizeof(msg),
                     "plane a=%.6f b=%.6f c=%.6f d=%.6f | 内点 %d/%d fit=%d",
                     coeffs[0], coeffs[1], coeffs[2], coeffs[3],
                     inlier_count, src->count, fit);
            break;
        case 1:
            snprintf(msg, sizeof(msg),
                     "sphere c=(%.6f, %.6f, %.6f) r=%.6f | 内点 %d/%d fit=%d",
                     coeffs[0], coeffs[1], coeffs[2], coeffs[3],
                     inlier_count, src->count, fit);
            break;
        case 2:
            snprintf(msg, sizeof(msg),
                     "cylinder p=(%.6f, %.6f, %.6f) dir=(%.6f, %.6f, %.6f) r=%.6f | 内点 %d/%d",
                     coeffs[0], coeffs[1], coeffs[2], coeffs[3], coeffs[4], coeffs[5], coeffs[6],
                     inlier_count, src->count);
            break;
        case 3:
            snprintf(msg, sizeof(msg),
                     "circle3D c=(%.6f, %.6f, %.6f) n=(%.6f, %.6f, %.6f) r=%.6f | 内点 %d/%d",
                     coeffs[0], coeffs[1], coeffs[2], coeffs[3], coeffs[4], coeffs[5], coeffs[6],
                     inlier_count, src->count);
            break;
        case 4:
            snprintf(msg, sizeof(msg),
                     "cone apex=(%.6f, %.6f, %.6f) dir=(%.6f, %.6f, %.6f) angle=%.6f rad | 内点 %d/%d",
                     coeffs[0], coeffs[1], coeffs[2], coeffs[3], coeffs[4], coeffs[5], coeffs[6],
                     inlier_count, src->count);
            break;
        case 5:
            snprintf(msg, sizeof(msg),
                     "line3D p=(%.6f, %.6f, %.6f) dir=(%.6f, %.6f, %.6f) | 内点 %d/%d",
                     coeffs[0], coeffs[1], coeffs[2], coeffs[3], coeffs[4], coeffs[5],
                     inlier_count, src->count);
            break;
    }
    if (inliers) Point3DAlg_FreeBuffer(inliers);   // 内点索引为库 malloc，配 FreeBuffer
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（FitModel）

// ---------------- 算子入口：通用度量（node.json: alg.point3d.measure_cloud） ----------------
// 参数（按声明顺序）：method(int 0~7) distance_threshold(number，method 5 用)
//                     plane_params(text，method 6/7 用——两面方程 8 值逗号分隔)
// out_values 布局（Point3DAlg_api.h 权威口径）：
//   0 AABB               6  [minx,miny,minz,maxx,maxy,maxz]
//   1 OBB                15 [cx,cy,cz, ex,ey,ez(半长), R9 行主序(行=主轴)]（>=3 点）
//   2 centroid           3  [cx,cy,cz]
//   3 meanCovariance     12 [mx,my,mz, c00..c22 行主序]（>=2 点）
//   4 pointCloudDistance N（逐点到目标云最近欧氏距离；需第二片点云；
//                          消息给 n/min/max/mean 汇总——逐点值无消息通道承载）
//   5 flatness           3  [pv 峰峰值, sigma 局内点残差标准差, inlier_count]
//                           （distance_threshold = 局内点阈值 > 0）
//   6 planeAngle         1  [angle_deg 锐角 [0,90]]（plane_params 8 值；法向零向量 -3）
//   7 planeIntersection  6  [px,py,pz 交线上一点, dx,dy,dz 单位方向]（近平行 -2）
// 输入：clouds[0]=输入点云（method 0~5 必需），clouds[1]=目标点云（method 4 必需；
//       method 6/7 忽略主/目标云，允许不连线）。
// out_values 预分配：method 4 = 主云点数 N（逐点），其余 16（>= 布局最长 15）免两段式。
// 错误码：1 输入未连线；2 点云为空/点数不足/内存不足；3 参数非法（-3/文本解析失败）；
//         4 业务失败（-2：近平行无交线/平面拟合失败/内点不足）。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.measure_cloud","name":"通用点云度量","group":"3D 点云","category":"处理","description":"通用点云度量（method 0~7）：0=AABB 1=OBB(中心+半长+旋转3x3行主序，>=3点) 2=质心 3=均值协方差(>=2点) 4=逐点到目标云最近距离（需第二片点云，消息给 min/max/均值汇总） 5=平面度[pv,σ,内点数]（distance_threshold） 6=两平面夹角 7=两平面交线（plane_params 两面方程 8 值逗号分隔 a1,b1,c1,d1,a2,b2,c2,d2）；结果并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"cloud"},
//     {"name":"目标点云(方法4)","type":"cloud"}
//   ],
//   "params": [
//     {"name":"method","label":"度量方法","type":"int","default":0,"min":0,"max":7,"description":"0=AABB 1=OBB 2=质心 3=均值协方差 4=逐点最近距离(需目标云) 5=平面度 6=两平面夹角 7=两平面交线(6/7 忽略点云输入)"},
//     {"name":"distance_threshold","label":"内点阈值(方法5)","type":"number","default":0.05,"min":0.0001,"max":1000000,"description":"method 5：平面度局内点距离阈值（>0）"},
//     {"name":"plane_params","label":"两面方程(方法6/7)","type":"text","default":"","description":"method 6/7：两面方程 8 值逗号分隔 a1,b1,c1,d1,a2,b2,c2,d2（法向零向量报错；方法 7 近平行无交线报业务失败）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiMeasureCloud(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    int method = 0;
    double dist_thr = 0.05;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) dist_thr = p->d;
    if (method < 0 || method > 7) {
        out->error = "method 仅支持 0=AABB/1=OBB/2=质心/3=协方差/4=逐点距离/5=平面度/6=夹角/7=交线";
        return 3;
    }

    const AlgoCCloud* src = (in->cloudCount >= 1 && in->clouds) ? &in->clouds[0] : NULL;
    const AlgoCCloud* other = (in->cloudCount >= 2 && in->clouds) ? &in->clouds[1] : NULL;
    if (method <= 5 && (!src || src->count <= 0 || !src->xyz)) {
        out->error = "输入点云为空（method 0~5 需要）"; return 2;
    }
    if (method == 4 && (!other || other->count <= 0 || !other->xyz)) {
        out->error = "方法 4 需要第二片点云（目标点云）"; return 2;
    }

    double params[8] = { 0 };
    int pc = 0;
    if (method == 5) {
        if (dist_thr <= 0.0) { out->error = "distance_threshold 须 > 0（method 5 局内点阈值）"; return 3; }
        params[0] = dist_thr;
        pc = 1;
    } else if (method == 6 || method == 7) {
        const AlgoCParam* pp = FindParam(in, 2, 0);
        const int n = pp ? ParseDoubleList(pp->s, params, 8) : 0;
        if (n < 0) { out->error = "plane_params 含非数值内容（需逗号分隔数字）"; return 3; }
        if (n != 8) {
            out->error = "plane_params 需 8 个数值（两面方程 a1,b1,c1,d1,a2,b2,c2,d2，逗号分隔）";
            return 3;
        }
        pc = 8;
    }

    // out_values 预分配：method 4 = 逐点距离 N 个；其余最长布局 15，给 16 免两段式
    const int cap = (method == 4 && src) ? src->count : 16;
    double* vals = (double*)malloc(sizeof(double) * (size_t)(cap > 0 ? cap : 1));
    if (!vals) { out->error = "内存不足"; return 2; }
    int vn = 0;
    Point3DAlg_PointCloud pin, pother;
    pin.xyz = src ? (float*)src->xyz : NULL;       pin.count = src ? src->count : 0;
    pother.xyz = other ? (float*)other->xyz : NULL; pother.count = other ? other->count : 0;
    const int rc = Point3DAlg_MeasureCloud(src ? &pin : NULL, other ? &pother : NULL,
                                           method, pc > 0 ? params : NULL, pc,
                                           vals, cap, &vn);
    if (rc == -3) {
        free(vals);
        out->error = "度量失败（method/params 布局不符或取值非法：阈值<=0、法向零向量等）";
        return 3;
    }
    if (rc == -2) {
        free(vals);
        out->error = "度量失败：两平面近平行（无有限交线）或平面拟合失败/内点不足";
        return 4;
    }
    if (rc == -1) {
        free(vals);
        out->error = "度量失败（点数不足：OBB 需 >=3、均值协方差需 >=2；或方法 4 缺目标云）";
        return 2;
    }
    if (rc != 0) { free(vals); out->error = "度量失败（内存不足或内部异常）"; return 2; }

    static char msg[512];
    switch (method) {
        case 0:
            snprintf(msg, sizeof(msg),
                     "AABB min=(%.4f, %.4f, %.4f) max=(%.4f, %.4f, %.4f)",
                     vals[0], vals[1], vals[2], vals[3], vals[4], vals[5]);
            break;
        case 1:
            snprintf(msg, sizeof(msg),
                     "OBB c=(%.4f, %.4f, %.4f) e=(%.4f, %.4f, %.4f) "
                     "R=[%.4f %.4f %.4f; %.4f %.4f %.4f; %.4f %.4f %.4f]",
                     vals[0], vals[1], vals[2], vals[3], vals[4], vals[5],
                     vals[6], vals[7], vals[8], vals[9], vals[10], vals[11],
                     vals[12], vals[13], vals[14]);
            break;
        case 2:
            snprintf(msg, sizeof(msg), "centroid (%.6f, %.6f, %.6f)",
                     vals[0], vals[1], vals[2]);
            break;
        case 3:
            snprintf(msg, sizeof(msg),
                     "cov m=(%.6f, %.6f, %.6f) C=[%.6f %.6f %.6f; %.6f %.6f %.6f; %.6f %.6f %.6f]",
                     vals[0], vals[1], vals[2],
                     vals[3], vals[4], vals[5], vals[6], vals[7], vals[8],
                     vals[9], vals[10], vals[11]);
            break;
        case 4: {
            // 契约上 rc=0 时 vn = 主云点数 > 0；vn==0 属契约外情形——vals 为
            // 未初始化 malloc 缓冲，先判 vn>0 再取 vals[0]，避免读未初始化内存
            double dmin = 0.0, dmax = 0.0, dsum = 0.0;
            if (vn > 0) {
                dmin = dmax = vals[0];
                for (int i = 0; i < vn; ++i) {
                    if (vals[i] < dmin) dmin = vals[i];
                    if (vals[i] > dmax) dmax = vals[i];
                    dsum += vals[i];
                }
            }
            snprintf(msg, sizeof(msg),
                     "distance n=%d min=%.6f max=%.6f mean=%.6f",
                     vn, dmin, dmax, vn > 0 ? dsum / vn : 0.0);
            break;
        }
        case 5:
            snprintf(msg, sizeof(msg),
                     "flatness pv=%.6f sigma=%.6f inliers=%d",
                     vals[0], vals[1], (int)vals[2]);
            break;
        case 6:
            snprintf(msg, sizeof(msg), "plane_angle=%.4f deg", vals[0]);
            break;
        default:
            snprintf(msg, sizeof(msg),
                     "intersection p=(%.6f, %.6f, %.6f) dir=(%.6f, %.6f, %.6f)",
                     vals[0], vals[1], vals[2], vals[3], vals[4], vals[5]);
            break;
    }
    free(vals);
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（MeasureCloud）

// ---------------- 算子入口：分割与聚类（node.json: alg.point3d.segment_cloud） ----------------
// method 与 params 布局以 Point3DAlg_api.h 注释表为权威口径（method 0~4 全量开放）。
// 固定参数集（filter_cloud 先例）：node.json 声明 1 个 method(int) + 13 个 number，
// 适配层按 method 分支组装该布局的 params 数组（未用参数保持默认跳过）：
//   0 planeRANSAC      [distance_threshold=v0]
//   1 euclideanCluster [cluster_tolerance=v1, min_size=v2, max_size=v3]
//   2 dbscan           [eps=v4, min_points=v5]（噪声点丢弃）
//   3 regionGrowing    [k_neighbors=v6, smoothness_deg=v7, curvature_thresh=v8,
//                       min_size=v9, max_size=v10]（法线由接口内部按 k_neighbors 估计）
//   4 planesMulti      [distance_threshold=v0, max_planes=v11, min_inliers=v12]
// 输出：点数最多的一段点云——node.json 输出端口静态，无法声明可变段数（ABI v1
// 虽支持多 cloud 数组输出，宿主按 outputs 端口顺序只取第 1 个），故取最大段；
// 总段数与各段点数（段序 = C API 返回序，多为点数降序）并入节点消息。
// 平面方程等详情可接 fit_model 另取。
// 错误码：1 输入未连线；2 点云为空/内存不足；3 参数非法（-3）；4 无有效段（-2）。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.segment_cloud","name":"点云分割聚类","group":"3D 点云","category":"处理","description":"点云分割/聚类（method 0~4）：0=主平面RANSAC分割(恒2段：平面内点+其余) 1=欧式聚类 2=DBSCAN密度聚类(噪声丢弃) 3=区域生长(内部自动估计法线) 4=多平面迭代；输出点数最多的一段点云（输出端口静态、无法声明可变段数），总段数与各段点数并入节点消息；平面方程等详情接 fit_model 另取；未用参数保持默认跳过"},
//   "inputs": [
//     {"name":"输入","type":"cloud"}
//   ],
//   "params": [
//     {"name":"method","label":"分割方法","type":"int","default":1,"min":0,"max":4,"description":"0=planeRANSAC主平面分割 1=euclideanCluster欧式聚类 2=dbscan密度聚类 3=regionGrowing区域生长 4=planesMulti多平面迭代"},
//     {"name":"distance_threshold","label":"平面距离阈值(方法0/4)","type":"number","default":0.05,"min":0.0001,"max":1000000,"description":"method 0/4：平面分割距离阈值（>0）"},
//     {"name":"cluster_tolerance","label":"聚类容差(方法1)","type":"number","default":0.2,"min":0.0001,"max":1000000,"description":"method 1：欧式聚类容差（>0，典型 2~5 倍点距）"},
//     {"name":"min_size","label":"最小簇点数(方法1)","type":"number","default":10,"min":1,"max":100000000,"description":"method 1：最小簇点数（>=1）"},
//     {"name":"max_size","label":"最大簇点数(方法1)","type":"number","default":1000000,"min":1,"max":100000000,"description":"method 1：最大簇点数（>= 最小簇点数）"},
//     {"name":"eps","label":"邻域半径(方法2)","type":"number","default":0.2,"min":0.0001,"max":1000000,"description":"method 2：DBSCAN eps 邻域半径（>0）"},
//     {"name":"min_points","label":"核心点邻居数(方法2)","type":"number","default":5,"min":1,"max":100000000,"description":"method 2：核心点最小邻居数（>=1，含自身）"},
//     {"name":"k_neighbors","label":"近邻个数(方法3)","type":"number","default":10,"min":3,"max":1000,"description":"method 3：区域生长近邻个数（>=3，兼作内部法线估计的 KNN 数）"},
//     {"name":"smoothness_deg","label":"法线夹角阈值(方法3)","type":"number","default":10,"min":0.001,"max":180,"description":"method 3：相邻点法线夹角阈值（度，>0，越小区域越平）"},
//     {"name":"curvature_thresh","label":"曲率阈值(方法3)","type":"number","default":1,"min":0,"max":1000000,"description":"method 3：曲率阈值（>=0，超过则不并入区域）"},
//     {"name":"min_size_rg","label":"最小区域点数(方法3)","type":"number","default":10,"min":1,"max":100000000,"description":"method 3：最小区域点数（>=1）"},
//     {"name":"max_size_rg","label":"最大区域点数(方法3)","type":"number","default":1000000,"min":1,"max":100000000,"description":"method 3：最大区域点数（>= 最小区域点数）"},
//     {"name":"max_planes","label":"最多平面数(方法4)","type":"number","default":2,"min":1,"max":1000,"description":"method 4：最多检出的平面数（>=1）"},
//     {"name":"min_inliers","label":"最少平面内点(方法4)","type":"number","default":100,"min":3,"max":100000000,"description":"method 4：每个平面最少内点数（>=3）"}
//   ],
//   "outputs": [
//     {"name":"最大段","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiSegmentCloud(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    int method = 1;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (method < 0 || method > 4) {
        out->error = "method 仅支持 0=planeRANSAC/1=euclideanCluster/2=dbscan/3=regionGrowing/4=planesMulti";
        return 3;
    }

    double v[13] = { 0 };
    for (int i = 0; i < 13; ++i)
        if (const AlgoCParam* p = FindParam(in, 0, i)) v[i] = p->d;
    // v 声明序（= node.json params 中 number 的声明序）：
    //   0 distance_threshold  1 cluster_tolerance  2 min_size  3 max_size
    //   4 eps  5 min_points  6 k_neighbors  7 smoothness_deg  8 curvature_thresh
    //   9 min_size_rg  10 max_size_rg  11 max_planes  12 min_inliers
    double params[5];
    int pc = 0;
    switch (method) {
        case 0: params[0] = v[0];                                        pc = 1; break;
        case 1: params[0] = v[1]; params[1] = v[2]; params[2] = v[3];    pc = 3; break;
        case 2: params[0] = v[4]; params[1] = v[5];                      pc = 2; break;
        case 3: params[0] = v[6]; params[1] = v[7]; params[2] = v[8];
                params[3] = v[9]; params[4] = v[10];                     pc = 5; break;
        default: params[0] = v[0]; params[1] = v[11]; params[2] = v[12]; pc = 3; break;
    }

    Point3DAlg_PointCloud pin = CloudViewOf(src);
    Point3DAlg_PointCloud* segs = NULL;
    int seg_count = 0;
    const int rc = Point3DAlg_SegmentCloud(&pin, method, pc > 0 ? params : NULL, pc,
                                           &segs, &seg_count);
    if (rc == -3) { out->error = "分割失败（params 与 method 布局不符或取值非法，见 api.h 布局表）"; return 3; }
    if (rc == -2) { out->error = "分割失败：无有效段（DBSCAN 全噪声、平面拟合失败或区域生长为空）"; return 4; }
    if (rc != 0)  { out->error = "分割失败（内存不足或内部异常）"; return 2; }
    if (!segs || seg_count <= 0) { out->error = "分割失败：返回段为空"; return 2; }

    // 取点数最多的一段，拷贝 xyz 后全量回收（FreeClouds 逐段 free，不转移段指针所有权）
    int best = 0;
    for (int i = 1; i < seg_count; ++i)
        if (segs[i].count > segs[best].count) best = i;
    if (segs[best].count <= 0 || !segs[best].xyz) {
        Point3DAlg_FreeClouds(segs, seg_count);
        out->error = "分割失败：最大段为空";
        return 2;
    }
    const int out_count = segs[best].count;
    float* outXyz = (float*)malloc((size_t)out_count * 3 * sizeof(float));
    if (!outXyz) {
        Point3DAlg_FreeClouds(segs, seg_count);
        out->error = "内存不足"; return 2;
    }
    memcpy(outXyz, segs[best].xyz, (size_t)out_count * 3 * sizeof(float));

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "segments=%d out=seg%d(%d点) counts=[",
                       seg_count, best, out_count);
    for (int i = 0; i < seg_count && pos < (int)sizeof(msg) - 16; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos, "%s%d", i ? "," : "", segs[i].count);
    if (pos < (int)sizeof(msg)) pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos, "]");
    Point3DAlg_FreeClouds(segs, seg_count);

    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(outXyz); out->error = "内存不足"; return 2; }
    arr->count = out_count;
    arr->xyz = outXyz;
    arr->rgb = nullptr;
    out->clouds = arr;
    out->cloudCount = 1;
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（SegmentCloud）

// ---------------- 算子入口：高度图（node.json: alg.point3d.height_map） ----------------
// 点云 XY 栅格化为高度图（3D AOI → 2D ImageProcessor 全部算子的桥，价值最高的
// 3D→2D 显示通道）：CloudToHeightMap 栅格取值后，入口内做高度归一化映射为 8U
// 灰度图（zmin→0、zmax→255；mask=0 空格填 0），转 BGRA 输出——下游可接 2D 全部
// 图像节点（阈值/连通域/拟合等）。mode=3 点计数密度图同样归一化（计数 zmin/zmax）。
// 参数（按声明顺序）：cell(number>0) mode(int 0~3)
// 输入：一片点云；输出：高度灰度图（BGRA）。
// 消息通道："width=W height=H cell=C zmin=Z0 zmax=Z1 valid=P/T(R%) mode=M"
//（坐标约定：row 0 = y 最小，与 Image2DAlg 图像坐标系一致，按行主序直转）。
// 错误码：1 输入未连线；2 点云为空/栅格化失败（-2：无有效点/超 2000 万格上限）/
//         内存不足；3 参数非法（cell<=0、mode 越界）。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.height_map","name":"点云高度图","group":"3D 点云","category":"处理","description":"点云栅格化为高度图（3D→2D 显示桥）：XY 栅格取格内 z（mode 0=最高 1=最低 2=平均 3=点计数密度），高度归一化映射为 8U 灰度图（zmin→0、zmax→255，mask=0 空格填 0）输出 BGRA——下游可接 2D 全部图像算子（阈值/连通域/拟合等）做 AOI 检出；栅格参数与有效率并入节点消息；row 0 = y 最小，与 2D 图像坐标系一致"},
//   "inputs": [
//     {"name":"输入","type":"cloud"}
//   ],
//   "params": [
//     {"name":"cell","label":"栅格边长","type":"number","default":0.1,"min":0.0001,"max":1000000,"description":"栅格边长（与点云坐标同单位，>0；总格数上限 2000 万，超出报错）"},
//     {"name":"mode","label":"栅格取值","type":"int","default":0,"min":0,"max":3,"description":"0=每格最高z 1=每格最低z 2=每格平均z 3=点计数密度（密度图同样归一化显示）"}
//   ],
//   "outputs": [
//     {"name":"高度图","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiHeightMap(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    double cell = 0.1;
    int mode = 0;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) cell = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) mode = p->i;
    if (cell <= 0.0) { out->error = "cell 须 > 0（栅格边长，与点云坐标同单位）"; return 3; }
    if (mode < 0 || mode > 3) {
        out->error = "mode 仅支持 0=每格最高z/1=每格最低z/2=每格平均z/3=点计数密度";
        return 3;
    }

    Point3DAlg_PointCloud pin = CloudViewOf(src);
    Point3DAlg_HeightMap hm = {};
    const int rc = Point3DAlg_CloudToHeightMap(&pin, cell, mode, &hm);
    if (rc == -3) { out->error = "栅格化失败（cell<=0 或 mode 越界）"; return 3; }
    if (rc == -2) { out->error = "栅格化失败：无有效点或超 2000 万格上限（调大 cell）"; return 4; }
    if (rc != 0)  { out->error = "栅格化失败（内存不足或内部异常）"; return 2; }
    if (hm.cells == NULL || hm.mask == NULL || hm.width <= 0 || hm.height <= 0) {
        Point3DAlg_FreeHeightMap(&hm);
        out->error = "栅格化失败：返回图无效";
        return 2;
    }

    // zmin/zmax（仅 mask=1 格，空格为 NaN 不参与），归一化映射 8U：zmin→0、zmax→255
    const int total = hm.width * hm.height;
    double zmin = 0.0, zmax = 0.0;
    int valid_n = 0;
    for (int i = 0; i < total; ++i) {
        if (!hm.mask[i]) continue;
        const double z = (double)hm.cells[i];
        if (valid_n == 0) { zmin = zmax = z; }
        else {
            if (z < zmin) zmin = z;
            if (z > zmax) zmax = z;
        }
        ++valid_n;
    }
    const double range = zmax - zmin;
    // 除零保护：zmin==zmax（纯平面/单值高度，或 mode=3 全格同计数）时 range=0，
    // 有效格按 0 灰度输出（mask 语义保留，不把整图拉成 255）
    uint8_t* px = (uint8_t*)malloc((size_t)total * 4);
    if (!px) { Point3DAlg_FreeHeightMap(&hm); out->error = "内存不足"; return 2; }
    for (int i = 0; i < total; ++i) {
        uint8_t g = 0;
        if (hm.mask[i] && range > 0.0) {
            double t = ((double)hm.cells[i] - zmin) / range;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            g = (uint8_t)(t * 255.0 + 0.5);
        }
        px[i * 4] = g; px[i * 4 + 1] = g; px[i * 4 + 2] = g; px[i * 4 + 3] = 255;
    }

    AlgoCImageOut* arr = (AlgoCImageOut*)malloc(sizeof(AlgoCImageOut));
    if (!arr) {
        free(px);
        Point3DAlg_FreeHeightMap(&hm);
        out->error = "内存不足"; return 2;
    }
    arr->width = hm.width;
    arr->height = hm.height;
    arr->bgra = px;
    out->images = arr;
    out->imageCount = 1;

    static char msg[256];
    snprintf(msg, sizeof(msg),
             "width=%d height=%d cell=%.4g zmin=%.4g zmax=%.4g valid=%d/%d(%.1f%%) mode=%d",
             hm.width, hm.height, hm.cell, zmin, zmax,
             valid_n, total, total > 0 ? 100.0 * valid_n / total : 0.0, mode);
    Point3DAlg_FreeHeightMap(&hm);
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（HeightMap）

// ---------------- 算子入口：关键点检测（node.json: alg.point3d.detect_keypoints） ----------------
// Point3DAlg_DetectKeypoints（Harris3D/ISS3D）：
//   type 0 Harris3D  radius = 邻域搜索半径（>0）；threshold = 响应阈值（>=0，
//                    越小保留越多；非极大值抑制恒开启）
//   type 1 ISS3D     radius = salient_radius 显著性半径（>0，典型点距数倍）；
//                    threshold = non_max_radius 非极大值抑制半径（>0，典型 < radius）
// 输入：一片点云；输出：关键点云（rgb 不随行，查看器按高度伪彩上色）。
// 消息通道："keypoints=N in=M type=Harris3D|ISS3D"。
// 数据注记（ISS3D）：无噪声的精确解析曲面（理想圆柱/平面）邻域严格共面 →
//   恒 0 关键点（-2）——真实扫描或带噪合成数据不受影响。
// 错误码：1 输入未连线；2 点云为空/内存不足；3 参数非法（type/radius/threshold）；
//         4 无关键点（-2：0 检出）。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.detect_keypoints","name":"点云关键点","group":"3D 点云","category":"处理","description":"点云关键点检测：type 0=Harris3D（radius=邻域搜索半径，threshold=响应阈值>=0 越小保留越多，NMS 恒开）1=ISS3D（radius=显著性半径>0 典型点距数倍，threshold=非极大值抑制半径>0 典型<radius）；输出关键点云（无颜色，查看器按高度伪彩上色），数量并入节点消息；理想解析曲面 ISS3D 恒 0 检出属已知限制"},
//   "inputs": [
//     {"name":"输入","type":"cloud"}
//   ],
//   "params": [
//     {"name":"type","label":"关键点类型","type":"int","default":0,"min":0,"max":1,"description":"0=Harris3D 1=ISS3D"},
//     {"name":"radius","label":"半径","type":"number","default":0.5,"min":0.0001,"max":1000000,"description":"Harris3D：邻域搜索半径（>0）/ ISS3D：显著性半径（>0，典型取点距数倍）"},
//     {"name":"threshold","label":"阈值","type":"number","default":0,"min":0,"max":1000000,"description":"Harris3D：响应阈值（>=0，越小保留越多）/ ISS3D：非极大值抑制半径（须 >0，典型 < radius）"}
//   ],
//   "outputs": [
//     {"name":"关键点","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiDetectKeypoints(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    int type = 0;
    double radius = 0.5, threshold = 0.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) type = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) radius = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) threshold = p->d;
    if (type < 0 || type > 1) { out->error = "type 仅支持 0=Harris3D/1=ISS3D"; return 3; }
    if (radius <= 0.0) { out->error = "radius 须 > 0（Harris3D 邻域半径 / ISS3D 显著性半径）"; return 3; }
    if (type == 0 && threshold < 0.0) {
        out->error = "Harris3D 响应阈值须 >= 0（越小保留越多）"; return 3;
    }
    if (type == 1 && threshold <= 0.0) {
        out->error = "ISS3D 非极大值抑制半径须 > 0（典型 < radius）"; return 3;
    }

    Point3DAlg_PointCloud pin = CloudViewOf(src);
    Point3DAlg_PointCloud kp = {};
    const int rc = Point3DAlg_DetectKeypoints(&pin, type, radius, threshold, &kp);
    if (rc == -3) { out->error = "关键点检测失败（type/radius/threshold 非法）"; return 3; }
    if (rc == -2) {
        out->error = "无关键点检出（0 检出；理想解析曲面 ISS3D 恒 0 属已知限制）";
        return 4;
    }
    if (rc != 0)  { out->error = "关键点检测失败（内存不足或内部异常）"; return 2; }

    if (!MoveCloudOut(&kp, out)) { out->error = "内存不足"; return 2; }   // xyz 所有权转移

    static char msg[128];
    snprintf(msg, sizeof(msg), "keypoints=%d in=%d type=%s",
             kp.count, src->count, type == 0 ? "Harris3D" : "ISS3D");
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（DetectKeypoints）

// ================ SICK 深层读取接入（node.json ×3：sick_quality/sick_intensity/sick_raw） ================
// 接入 src/Point3DAlg/include/Point3DAlg_SickApi.h（1.1.0 新增的 SICK 深层 C API，
// 实现见 Point3DAlg_SickIO.cpp，全部已测）：句柄族
//   CreateSickReader(cfg) → LoadRawInto(handle, folder, frame, recipe, common)
//   → GetSickInfo / GetSickRaw / GetSickIntensity / StitchQuality → FreeSickReader。
// 三个入口的句柄生命周期均在入口内闭环（create→load→查询→free，失败路径各自回收）——
// ABI v1 无句柄通道，不跨节点持有。
// SICK 族错误码口径（**与点云计算族不同**，见 SickApi.h 头注释）：
//   -1 参数/空指针  -2 数据缺失  -3 帧号越界  -4 配置/数据错误  -99 未预期异常
// 映射到 ComUI rc：1 未设路径；2 数据/配置无效；3 参数非法（frame 越界、threshold<=0）；
//                 4 量测失败/无强度通道（单相机自检、旧格式无强度 .dat）。

#include "Point3DAlg/include/Point3DAlg_SickApi.h"   // SICK 深层 C API（含 SickConfig/StitchQualityResult）

// ---- SICK 三入口共用装载序列：LoadSickConfig → CreateSickReader → LoadRawInto ----
// 参数提取约定（宿主 marshal 不带参数名，按 node.json params 声明序 + kind 出现次序，
// FindParam 先例）：text 第 0/1/2 个 = folder/recipe_path/common_path；int 第 0 个 = frame。
// recipe/common 空串或未传 = NULL = 默认路径（Z:\Calibrate\3D\MtxJuxin60003D.dll.user.json、
// Z:\Juxin60003D_CommonParams.json，见 SickReader.h LoadSickConfig 注释）；cfg 在此一次读好，
// LoadRawInto 传 NULL/NULL 沿用句柄 cfg（不重复读盘）。
// 成功：*out_h = 已加载数据的句柄（调用方负责 FreeSickReader），返回 0。
// 失败：*out_h = NULL、out->error 已置，返回 ComUI rc（1/2/3），调用方直接 return。
static int SickEntryLoad(const AlgoCInput* in, AlgoCOutput* out,
                         Point3DAlg_SickReaderH* out_h, const char** out_folder) {
    *out_h = nullptr;
    *out_folder = nullptr;

    const AlgoCParam* p_folder = FindParam(in, 2, 0);
    const char* folder = p_folder ? p_folder->s : nullptr;
    if (!folder || !folder[0]) { out->error = "未设置扫描数据文件夹"; return 1; }
    *out_folder = folder;

    const AlgoCParam* p_recipe = FindParam(in, 2, 1);
    const AlgoCParam* p_common = FindParam(in, 2, 2);
    const char* recipe = (p_recipe && p_recipe->s && p_recipe->s[0]) ? p_recipe->s : nullptr;
    const char* common = (p_common && p_common->s && p_common->s[0]) ? p_common->s : nullptr;
    const AlgoCParam* p_frame = FindParam(in, 1, 0);
    const int frame = p_frame ? p_frame->i : -1;

    Point3DAlg_SickConfig cfg;
    char cfgErr[256] = { 0 };
    if (Point3DAlg_LoadSickConfig(recipe, common, &cfg, cfgErr, (int)sizeof(cfgErr)) != 0) {
        static char err[512];
        snprintf(err, sizeof(err), "换算配置读取失败（recipe/common json）：%.200s", cfgErr);
        out->error = err;
        return 2;   // 配置/数据无效（ReadSickCloud 的 -4 同口径）
    }
    Point3DAlg_SickReaderH h = Point3DAlg_CreateSickReader(&cfg);
    if (!h) { out->error = "创建 SICK 读取器失败（内存不足）"; return 3; }

    const int rc = Point3DAlg_LoadRawInto(h, folder, frame, nullptr, nullptr);
    if (rc != 0) {
        static char err[512];
        switch (rc) {
            case -2:
                snprintf(err, sizeof(err), "扫描数据无效：文件夹无 .dat（%.200s）", folder);
                break;
            case -3:
                snprintf(err, sizeof(err), "帧号越界（frame=%d，按文件夹 .dat 数从 0 计）", frame);
                break;
            case -1:
                snprintf(err, sizeof(err), "装载参数非法（folder/frame）");
                break;
            default:
                snprintf(err, sizeof(err), "读取失败（配置或数据错误，错误码 %d）：%.200s", rc, folder);
                break;
        }
        out->error = err;
        Point3DAlg_FreeSickReader(h);   // 失败路径回收句柄
        return (rc == -3 || rc == -1) ? 3 : 2;
    }
    *out_h = h;
    return 0;
}

// ---------------- 算子入口：SICK 拼接质量自检（node.json: alg.point3d.sick_quality） ----------------
// 参数（按 node.json params 声明顺序）：folder(path) frame(int,-1=全部帧)
// diff_threshold(number，mm，>0，默认 0.02) recipe_path(path,可空) common_path(path,可空)。
// 无输入端口、无输出端口；自检结果并入节点消息（ABI v1 无数值通道，region_plane 同例）。
// 内部序列：LoadSickConfig → CreateSickReader → LoadRawInto → StitchQuality（内部触发
// GetDepth）→ GetSickInfo（stitch 行/列随 GetDepth 生成，须在 StitchQuality 之后取）
// → FreeSickReader。
// StitchQuality 语义（Point3DAlg_SickReader.h）：多相机衔接带相邻 rank 物理同位置格的
// z 差统计——pairs=两相机均有有效 z 的配对格数；dh_mean/dh_sigma=差值均值/标准差；
// dh_max_abs=最大绝对差；outlier=|差|>阈值的格数；outlier_ratio=outlier/pairs。
// 差值大 = 相机标定漂移/丢帧/同步异常。单相机（<2 台）无衔接带 → rc=4。
// 错误码：1 未设路径；2 数据/配置无效；3 参数非法（frame 越界、threshold<=0）；
//         4 自检失败（单相机/无有效配对格）。

// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.sick_quality","name":"SICK 拼接质量自检","group":"IO","category":"源","description":"SICK 多相机拼接质量自检（产线每日标定漂移/丢帧诊断）：读扫描数据后统计相邻相机衔接带物理同位置格的 z 差（配对格数/差值均值/σ/最大|差|/超差数与占比），并带出设备数、原始宽×行、拼接行×列、有无强度；无输入无输出端口，结果并入节点消息；diff_threshold 为超差判定阈值(mm)；自检需 >=2 台相机（folder 用 ; 分隔多路径）"},
//   "inputs": [],
//   "params": [
//     {"name":"folder","label":"扫描数据文件夹","type":"path","default":"","description":"扫描数据文件夹；多相机用 ';' 分隔（如 Z:/ScanImage/3D;Z:/ScanImage/3D-2，顺序=相机序）"},
//     {"name":"frame","label":"指定帧(-1=全部)","type":"int","default":-1,"min":-1,"description":"指定帧号（>=0 只读每个相机第 frame 帧，按文件名顺序从 0 计；<0 = 全部帧）"},
//     {"name":"diff_threshold","label":"超差阈值(mm)","type":"number","default":0.02,"min":0.0001,"max":1000000,"description":"衔接带格级 z 差的超差判定阈值（mm，>0；|差|>阈值计超差格）"},
//     {"name":"recipe_path","label":"配方json(可空)","type":"path","default":"","description":"用户配方 json 路径（空 = 默认 Z:\\Calibrate\\3D\\MtxJuxin60003D.dll.user.json）"},
//     {"name":"common_path","label":"通用参数json(可空)","type":"path","default":"","description":"通用参数 json 路径（空 = 默认 Z:\\Juxin60003D_CommonParams.json）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/include/Point3DAlg_SickApi.h","src/Point3DAlg/include/Point3DAlg_SickReader.h","src/Point3DAlg/Point3DAlg_SickIO.cpp","src/Point3DAlg/Point3DAlg_SickReader.cpp","src/CommonAlg/include/CommonAlg_Json.h","thrid/nlohmann_json/single_include/nlohmann/json.hpp"], "includes": ["src","src/Point3DAlg/include","src/CommonAlg/include","thrid/nlohmann_json/single_include"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiSickQuality(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    const AlgoCParam* p_thr = FindParam(in, 0, 0);
    const double thr = p_thr ? p_thr->d : 0.02;   // 默认 0.02mm，与 Point3DAlg_test 同口径
    if (thr <= 0.0) { out->error = "diff_threshold 须 > 0（衔接带超差判定阈值，mm）"; return 3; }

    Point3DAlg_SickReaderH h = nullptr;
    const char* folder = nullptr;
    const int lrc = SickEntryLoad(in, out, &h, &folder);
    if (lrc != 0) return lrc;

    Point3DAlg_StitchQualityResult q;
    const int rc = Point3DAlg_StitchQuality(h, thr, &q);
    if (rc == -2) {
        Point3DAlg_FreeSickReader(h);
        out->error = "拼接质量自检失败：需 >=2 台相机（folder 用 ';' 分隔多路径）且存在有效配对格";
        return 4;
    }
    if (rc == -3) {
        Point3DAlg_FreeSickReader(h);
        out->error = "diff_threshold 须 > 0（衔接带超差判定阈值，mm）";
        return 3;
    }
    if (rc != 0) {
        Point3DAlg_FreeSickReader(h);
        out->error = "拼接质量自检失败（拼接深度图生成异常或内存不足）";
        return 2;
    }

    int width = 0, devices = 0, hasInt = 0;
    long long rows = 0, sRows = 0, sCols = 0;
    const int irc = Point3DAlg_GetSickInfo(h, &width, &rows, &sRows, &sCols, &devices, &hasInt);
    Point3DAlg_FreeSickReader(h);   // 信息取完即回收（后续只消费已拷贝的 q/irc）
    if (irc != 0) { out->error = "读取器信息获取失败（数据未加载？）"; return 2; }

    // 建议动作由 outlier_ratio 推导（适配层展示口径，非库输出）
    const char* advice = (q.outlier_ratio <= 0.0) ? "衔接差在阈值内，标定正常"
                       : (q.outlier_ratio < 0.01) ? "轻微超差（<1%），建议复测确认"
                       : "超差比例高：排查相机标定漂移/丢帧/同步异常";
    static char msg[512];
    snprintf(msg, sizeof(msg),
             "拼接质量自检：配对格 %lld 差值均值 %.4f σ %.4f 最大|差| %.4f mm | "
             "超差(>%.3gmm) %lld 格(%.2f%%) | 建议：%s | 设备 %d 台 原始 %dx%lld 拼接 %lldx%lld 强度=%s",
             q.pairs, q.dh_mean, q.dh_sigma, q.dh_max_abs,
             thr, q.outlier,
             q.pairs > 0 ? 100.0 * (double)q.outlier / (double)q.pairs : 0.0,
             advice, devices, width, rows, sRows, sCols, hasInt ? "有" : "无");
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：SICK 强度图（node.json: alg.point3d.sick_intensity） ----------------
// 参数（按声明顺序）：folder(path) frame(int,-1=全部帧) recipe_path(path,可空) common_path(path,可空)。
// 输出：强度图（uint8 灰度直转 BGRA：g→(g,g,g,255)，与拼接深度图同几何，0=无效黑格）；
// 逐点强度无逐点输出通道，仅计数并入消息。
// 内部序列：... → GetSickInfo（先探 has_intensity，旧格式 .dat 直接明确报 4）→
// GetSickIntensity（内部触发 GetDepth/GetCloud；逐点强度副本取出仅统计即 FreeBuffer）
// → FreeSickReader。
// 错误码：1 未设路径；2 数据/配置无效；3 参数非法（frame 越界）；4 该数据无强度通道。

// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.sick_intensity","name":"SICK 强度图","group":"IO","category":"源","description":"读取 SICK 强度图（仅新格式 .dat 有强度通道）：强度 uint8 直转 8U 灰度 BGRA 输出（与拼接深度图同几何，0=无效黑格），图尺寸与逐点强度点数并入节点消息；无强度通道明确报 rc=4；触发拼接深度图生成"},
//   "inputs": [],
//   "params": [
//     {"name":"folder","label":"扫描数据文件夹","type":"path","default":"","description":"扫描数据文件夹；多相机用 ';' 分隔（顺序=相机序）"},
//     {"name":"frame","label":"指定帧(-1=全部)","type":"int","default":-1,"min":-1,"description":"指定帧号（>=0 只读每个相机第 frame 帧，按文件名顺序从 0 计；<0 = 全部帧）"},
//     {"name":"recipe_path","label":"配方json(可空)","type":"path","default":"","description":"用户配方 json 路径（空 = 默认 Z:\\Calibrate\\3D\\MtxJuxin60003D.dll.user.json）"},
//     {"name":"common_path","label":"通用参数json(可空)","type":"path","default":"","description":"通用参数 json 路径（空 = 默认 Z:\\Juxin60003D_CommonParams.json）"}
//   ],
//   "outputs": [
//     {"name":"强度图","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/include/Point3DAlg_SickApi.h","src/Point3DAlg/include/Point3DAlg_SickReader.h","src/Point3DAlg/Point3DAlg_SickIO.cpp","src/Point3DAlg/Point3DAlg_SickReader.cpp","src/CommonAlg/include/CommonAlg_Json.h","thrid/nlohmann_json/single_include/nlohmann/json.hpp"], "includes": ["src","src/Point3DAlg/include","src/CommonAlg/include","thrid/nlohmann_json/single_include"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiSickIntensity(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    Point3DAlg_SickReaderH h = nullptr;
    const char* folder = nullptr;
    const int lrc = SickEntryLoad(in, out, &h, &folder);
    if (lrc != 0) return lrc;

    int hasInt = 0;
    if (Point3DAlg_GetSickInfo(h, nullptr, nullptr, nullptr, nullptr, nullptr, &hasInt) != 0) {
        Point3DAlg_FreeSickReader(h);
        out->error = "读取器信息获取失败（数据未加载？）";
        return 2;
    }
    if (!hasInt) {
        Point3DAlg_FreeSickReader(h);
        out->error = "该数据无强度通道（.dat 为旧格式，仅高度+编码器）";
        return 4;
    }

    unsigned char* img = nullptr;
    unsigned char* cloudInt = nullptr;
    int w = 0, pointCount = 0;
    long long rows = 0;
    // 逐点强度计数需同时取 cloud_int 副本（库契约：两者同非 NULL 才填充），取完即还
    const int rc = Point3DAlg_GetSickIntensity(h, &img, &w, &rows, &cloudInt, &pointCount);
    Point3DAlg_FreeSickReader(h);   // 副本已交付，句柄即回收
    if (rc == -2) { out->error = "强度图缺失或为空（未加载数据/无有效拼接结果）"; return 2; }
    if (rc != 0)  { out->error = "强度图获取失败（拼接/点云生成异常或内存不足）"; return 2; }
    if (w <= 0 || rows <= 0 || rows > 2147483647LL) {
        Point3DAlg_FreeBuffer(img); Point3DAlg_FreeBuffer(cloudInt);
        out->error = "强度图尺寸非法";
        return 2;
    }

    // uint8 灰度 → BGRA 紧排（宿主恒 BGRA 行主序 ×4、A=255，height_map 同款直转）
    const long long total = (long long)w * rows;
    uint8_t* px = (uint8_t*)malloc((size_t)total * 4);
    if (!px) {
        Point3DAlg_FreeBuffer(img); Point3DAlg_FreeBuffer(cloudInt);
        out->error = "内存不足"; return 3;
    }
    for (long long i = 0; i < total; ++i) {
        const uint8_t g = img[i];
        px[i * 4] = g; px[i * 4 + 1] = g; px[i * 4 + 2] = g; px[i * 4 + 3] = 255;
    }
    Point3DAlg_FreeBuffer(img);

    AlgoCImageOut* arr = (AlgoCImageOut*)malloc(sizeof(AlgoCImageOut));
    if (!arr) {
        free(px);
        Point3DAlg_FreeBuffer(cloudInt);
        out->error = "内存不足"; return 3;
    }
    arr->width = w;
    arr->height = (int)rows;
    arr->bgra = px;
    out->images = arr;
    out->imageCount = 1;

    static char msg[256];
    snprintf(msg, sizeof(msg), "强度图 %dx%d 逐点强度=%s(%d 点)",
             w, (int)rows, pointCount > 0 ? "有" : "无", pointCount);
    Point3DAlg_FreeBuffer(cloudInt);   // 仅统计入消息，副本即还
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：SICK 原始高度图（node.json: alg.point3d.sick_raw） ----------------
// 参数（按声明顺序）：folder(path) frame(int,-1=全部帧) recipe_path(path,可空) common_path(path,可空)。
// 输出：原始高度图（相机 0，uint16 → 8U 灰度 BGRA）。
//   归一化口径（与 height_map 的 zmin→0/zmax→255 一致）：min/max 取全部**有效格**
//   （库契约 raw==0 = 无效点，不参与），gray = round((v-vmin)/(vmax-vmin)×255)；
//   无效格 gray=0（黑）；vmin==vmax（单值）时有效格按 0 输出（不把整图拉成 255）。
// 消息：原始尺寸 w×h、行数（=h）、编码器行数（=h，库契约逐行一个 int32）、有效率与 raw 范围。
// 内部序列：... → GetSickRaw（懒构建副本）→ FreeSickReader → 转换（副本在手，句柄先还）。
// 错误码：1 未设路径；2 数据/配置无效；3 参数非法（frame 越界）。

// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.sick_raw","name":"SICK 原始高度图","group":"IO","category":"源","description":"读取 SICK 原始高度图（相机 0，uint16 原始高度值）：有效格按 raw 最小/最大归一化为 8U 灰度 BGRA（vmin→0、vmax→255；0=无效格填黑，不参与归一化），原始尺寸 w×h、行数、编码器行数、有效率与 raw 范围并入节点消息"},
//   "inputs": [],
//   "params": [
//     {"name":"folder","label":"扫描数据文件夹","type":"path","default":"","description":"扫描数据文件夹；多相机用 ';' 分隔（顺序=相机序）"},
//     {"name":"frame","label":"指定帧(-1=全部)","type":"int","default":-1,"min":-1,"description":"指定帧号（>=0 只读每个相机第 frame 帧，按文件名顺序从 0 计；<0 = 全部帧）"},
//     {"name":"recipe_path","label":"配方json(可空)","type":"path","default":"","description":"用户配方 json 路径（空 = 默认 Z:\\Calibrate\\3D\\MtxJuxin60003D.dll.user.json）"},
//     {"name":"common_path","label":"通用参数json(可空)","type":"path","default":"","description":"通用参数 json 路径（空 = 默认 Z:\\Juxin60003D_CommonParams.json）"}
//   ],
//   "outputs": [
//     {"name":"原始高度图","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/include/Point3DAlg_SickApi.h","src/Point3DAlg/include/Point3DAlg_SickReader.h","src/Point3DAlg/Point3DAlg_SickIO.cpp","src/Point3DAlg/Point3DAlg_SickReader.cpp","src/CommonAlg/include/CommonAlg_Json.h","thrid/nlohmann_json/single_include/nlohmann/json.hpp"], "includes": ["src","src/Point3DAlg/include","src/CommonAlg/include","thrid/nlohmann_json/single_include"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiSickRaw(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    Point3DAlg_SickReaderH h = nullptr;
    const char* folder = nullptr;
    const int lrc = SickEntryLoad(in, out, &h, &folder);
    if (lrc != 0) return lrc;

    unsigned short* raw = nullptr;
    int* enc = nullptr;
    int w = 0;
    long long rows = 0;
    const int rc = Point3DAlg_GetSickRaw(h, &raw, &w, &rows, &enc);
    Point3DAlg_FreeSickReader(h);   // 副本已交付，句柄即回收
    if (rc == -2) { out->error = "原始数据缺失或为空（未加载数据）"; return 2; }
    if (rc != 0)  { out->error = "原始数据获取失败（懒构建异常或内存不足）"; return 2; }
    if (w <= 0 || rows <= 0 || rows > 2147483647LL) {
        Point3DAlg_FreeBuffer(raw); Point3DAlg_FreeBuffer(enc);
        out->error = "原始数据尺寸非法";
        return 2;
    }

    // min/max（仅 raw!=0 有效格）
    const long long total = (long long)w * rows;
    unsigned vmin = 0, vmax = 0;
    long long valid = 0;
    for (long long i = 0; i < total; ++i) {
        const unsigned v = raw[i];
        if (!v) continue;
        if (valid == 0) { vmin = vmax = v; }
        else {
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
        }
        ++valid;
    }
    if (valid == 0) {
        Point3DAlg_FreeBuffer(raw); Point3DAlg_FreeBuffer(enc);
        out->error = "原始高度图无有效点（全 0）";
        return 2;
    }
    const unsigned range = vmax - vmin;   // unsigned 安全：vmax >= vmin

    uint8_t* px = (uint8_t*)malloc((size_t)total * 4);
    if (!px) {
        Point3DAlg_FreeBuffer(raw); Point3DAlg_FreeBuffer(enc);
        out->error = "内存不足"; return 3;
    }
    for (long long i = 0; i < total; ++i) {
        const unsigned v = raw[i];
        uint8_t g = 0;
        if (v && range > 0) {
            double t = (double)(v - vmin) / (double)range;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            g = (uint8_t)(t * 255.0 + 0.5);
        }
        px[i * 4] = g; px[i * 4 + 1] = g; px[i * 4 + 2] = g; px[i * 4 + 3] = 255;
    }
    Point3DAlg_FreeBuffer(raw);

    AlgoCImageOut* arr = (AlgoCImageOut*)malloc(sizeof(AlgoCImageOut));
    if (!arr) {
        free(px);
        Point3DAlg_FreeBuffer(enc);
        out->error = "内存不足"; return 3;
    }
    arr->width = w;
    arr->height = (int)rows;
    arr->bgra = px;
    out->images = arr;
    out->imageCount = 1;

    static char msg[256];
    snprintf(msg, sizeof(msg),
             "原始 %dx%lld 行数=%lld 编码器行数=%lld 有效=%lld/%lld(%.1f%%) raw=[%u~%u]",
             w, rows, rows, rows, valid, total,
             total > 0 ? 100.0 * (double)valid / (double)total : 0.0, vmin, vmax);
    Point3DAlg_FreeBuffer(enc);
    out->error = msg;
    return 0;
}

// =====================================================================
// A 类覆盖补缺接入（2026-11-19，4 个入口，追加在文件尾部）：
//   alg.point3d.save_cloud_ex  —— SaveCloudEx（load_cloud 的反向闭环：处理结果落盘交付）
//   alg.point3d.knn_search     —— KnnSearch + FreeSearchResult（KdTree K 近邻，欧氏距离）
//   alg.point3d.radius_search  —— RadiusSearch + FreeSearchResult（半径搜索，0 命中属成功）
//   alg.point3d.thickness_map  —— ThicknessMap + FreeHeightMap（HeightMap 容器无法跨
//                                 节点流转，入口内归一化出 8U BGRA，height_map 同款口径）
// 本段只做参数组装、错误码映射与消息拼装，不新增算法；Knn/Radius 的
// out_indices/out_dists 为库 malloc（api.h §9.9 契约），成功路径拼完消息、
// 失败路径出口前均配对 FreeSearchResult 回收；thickness_map 的 HeightMap
// 全部路径 FreeHeightMap 回收（height_map 先例）。knn/radius 无输出端口，
// 结果走消息通道——900 字节截断安全：k 恒 ≤30，消息列前 min(k,30) 组
// "idx:dist"，总数恒在消息头部。
// =====================================================================

// ---------------- 算子入口：点云文件保存（node.json: alg.point3d.save_cloud_ex） ----------------
// Point3DAlg_SaveCloudEx（1.1.0 泛化保存；既有 SaveCloud 只写 PLY，本接口 PCD/PLY/TXT）：
//   type 0=PCD（恒二进制）1=PLY（binary 参数生效）2=TXT（恒文本，分隔符固定单空格）。
// 参数（按 node.json params 声明顺序）：path(path) type(int 0/1/2) binary(int 0=ASCII/1=二进制)。
// 输入：一片点云；无输出端口——纯副作用节点（save_transform 先例），落盘确认
// 并入节点消息："saved points=N type=.. mode=.. file=.."（mode 回显实际生效口径）。
// C API 错误码口径见 Point3DAlg_api.h SaveCloudEx 注释块。
// 错误码：1 输入未连线 / 未设置保存路径；2 点云为空 / 写文件失败（-2：路径
//         不存在/无权限/磁盘满）/ 内存不足或内部异常（-4/-99）；3 type 非法（-3）。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.save_cloud_ex","name":"点云文件保存","group":"IO","category":"输出","description":"保存点云到文件（PCD/PLY/TXT 泛化版，load_cloud 的反向闭环：处理结果落盘交付）：type 0=PCD（恒二进制）1=PLY（binary 参数生效）2=TXT（恒文本，分隔符固定单空格）；无输出端口，落盘确认（点数/格式/路径）并入节点消息"},
//   "inputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"path","label":"保存路径","type":"path","default":"","description":"输出文件路径（必填；目录不存在/无权限/磁盘满报保存失败）"},
//     {"name":"type","label":"文件类型","type":"int","default":1,"min":0,"max":2,"description":"0=PCD(PCL 原生，恒二进制) 1=PLY 2=TXT(每行 \"x y z\" 单空格分隔)"},
//     {"name":"binary","label":"二进制(仅PLY生效)","type":"int","default":1,"min":0,"max":1,"description":"1=二进制 0=ASCII；仅 PLY 生效（PCD 恒二进制、TXT 恒文本，其他值按 0 处理）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudIO.h","src/ShapelyAlg/CloudIO.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiSaveCloudEx(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    const AlgoCParam* p_path = FindParam(in, 2, 0);
    const char* path = (p_path && p_path->s && p_path->s[0]) ? p_path->s : nullptr;
    if (!path) { out->error = "未设置保存路径"; return 1; }
    int type = 0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) type = p->i;
    int binary = 0;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) binary = p->i;

    Point3DAlg_PointCloud pin = CloudViewOf(src);
    const int rc = Point3DAlg_SaveCloudEx(path, type, &pin, binary);
    if (rc == -3) { out->error = "保存失败（type 仅支持 0=PCD/1=PLY/2=TXT）"; return 3; }
    if (rc == -2) { out->error = "保存失败：路径不存在/无权限/磁盘满或格式器拒绝"; return 2; }
    if (rc != 0)  { out->error = "保存失败（内存不足或内部异常）"; return 2; }

    // mode 回显实际生效口径：PCD 恒二进制、TXT 恒文本（binary 仅 PLY 生效，api.h 注释）
    const char* mode = (type == 0) ? "binary(PCD恒)"
                     : (type == 1) ? (binary == 1 ? "binary" : "ascii")
                                   : "ascii(TXT恒)";
    static char msg[512];
    snprintf(msg, sizeof(msg), "saved points=%d type=%s mode=%s file=%.300s",
             src->count,
             type == 0 ? "PCD" : (type == 1 ? "PLY" : "TXT"),
             mode, path);
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（SaveCloudEx）

// ---------------- 算子入口：KdTree K 近邻搜索（node.json: alg.point3d.knn_search） ----------------
// Point3DAlg_KnnSearch（api.h §9.9 tree 组）：query 3 个 double 转 float 进 KdTree；
// out_dists 为欧氏距离（底层 KdTree 平方距离由核心层开方，非 PCL 原始口径），升序。
// 参数（按 node.json params 声明顺序）：query_x query_y query_z(number) k(int)。
// 输入：一片点云；无输出端口，近邻列表并入节点消息：
//   "knn k=K hits=H query=(x,y,z) idx:dist idx:dist ..."——消息头恒含总 k 与命中
//   数，随后列 min(k,30) 组（900 字节截断安全：k 上限 30，超出按 30 取）。
// 定位：估点距选参（voxel leaf / 聚类 tolerance 等）/ 局部诊断。
// 错误码：1 输入未连线；2 点云为空 / 内存不足或内部异常（-4/-99）；3 k<1（-3 同口径）。
// 内存契约：out_indices/out_dists 库 malloc，成功与失败路径均 FreeSearchResult 回收。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.knn_search","name":"K近邻搜索","group":"3D 点云","category":"处理","description":"KdTree K 近邻搜索（query 查询点 + k）：返回按欧氏距离升序的最近邻列表（距离为核心层开方口径，非 PCL 平方距离原始值）；无输出端口，消息恒以 knn k/hits 开头并列出全部命中组 idx:dist（k 上限 30——消息通道 900 字节截断安全，超出按 30 取）；定位：估点距选参（voxel leaf / 聚类 tolerance）/ 局部诊断"},
//   "inputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"query_x","label":"查询点X","type":"number","default":0.0,"description":"查询点 X 坐标（与点云同单位）"},
//     {"name":"query_y","label":"查询点Y","type":"number","default":0.0,"description":"查询点 Y 坐标"},
//     {"name":"query_z","label":"查询点Z","type":"number","default":0.0,"description":"查询点 Z 坐标"},
//     {"name":"k","label":"近邻个数","type":"int","default":10,"min":1,"max":30,"description":"K 近邻个数（>=1；上限 30，超出按 30 取——消息通道截断安全）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiKnnSearch(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    double q[3] = { 0.0, 0.0, 0.0 };
    for (int i = 0; i < 3; ++i)
        if (const AlgoCParam* p = FindParam(in, 0, i)) q[i] = p->d;
    int k = 10;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) k = p->i;
    if (k < 1) { out->error = "k 须 >= 1（K 近邻个数）"; return 3; }
    if (k > 30) k = 30;   // 消息通道 900 字节截断安全上限（30 组 idx:dist 恒 < 900B）

    Point3DAlg_PointCloud pin = CloudViewOf(src);
    int* idxs = NULL;
    float* dists = NULL;
    int count = 0;
    const int rc = Point3DAlg_KnnSearch(&pin, q, k, &idxs, &dists, &count);
    if (rc == -3) { out->error = "k 须 >= 1（K 近邻个数）"; return 3; }
    if (rc != 0)  { out->error = "KNN 搜索失败（内存不足或内部异常）"; return 2; }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "knn k=%d hits=%d query=(%.4g,%.4g,%.4g) ",
                       k, count, q[0], q[1], q[2]);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 32; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos, "%s%d:%.4g",
                        i ? " " : "", idxs[i], (double)dists[i]);
    Point3DAlg_FreeSearchResult(idxs, dists);   // 库 malloc，配对回收（NULL 安全）
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（KnnSearch）

// ---------------- 算子入口：KdTree 半径搜索（node.json: alg.point3d.radius_search） ----------------
// Point3DAlg_RadiusSearch（api.h §9.9 tree 组）：半径内全部近邻（欧氏距离，升序）。
// 参数（按 node.json params 声明顺序）：query_x query_y query_z radius(number)。
// 输入：一片点云；无输出端口，消息形态与 knn_search 同（消息头含命中总数 +
// 前 30 组 "idx:dist"）；0 命中属正常成功（api.h 口径："半径内无点 → 0 成功 +
// *out_count=0"），不报错。
// 错误码：1 输入未连线；2 点云为空 / 内存不足或内部异常（-4/-99）；3 radius<=0（-3 同口径）。
// 内存契约：同 knn_search——FreeSearchResult 成功/失败路径配对回收。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.radius_search","name":"半径搜索","group":"3D 点云","category":"处理","description":"KdTree 半径搜索（query 查询点 + radius）：返回半径内全部近邻，按欧氏距离升序（距离为核心层开方口径）；无输出端口，消息恒以 radius/hits 开头并列出前 30 组 idx:dist；半径内无点属正常成功（hits=0，不报错）；定位：局部密度/邻域诊断"},
//   "inputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"query_x","label":"查询点X","type":"number","default":0.0,"description":"查询点 X 坐标（与点云同单位）"},
//     {"name":"query_y","label":"查询点Y","type":"number","default":0.0,"description":"查询点 Y 坐标"},
//     {"name":"query_z","label":"查询点Z","type":"number","default":0.0,"description":"查询点 Z 坐标"},
//     {"name":"radius","label":"搜索半径","type":"number","default":0.5,"min":1e-06,"description":"搜索半径（>0，与点云坐标同单位；<=0 报参数非法）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiRadiusSearch(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    double q[3] = { 0.0, 0.0, 0.0 };
    for (int i = 0; i < 3; ++i)
        if (const AlgoCParam* p = FindParam(in, 0, i)) q[i] = p->d;
    double radius = 0.5;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) radius = p->d;
    if (radius <= 0.0) { out->error = "radius 须 > 0（搜索半径，与坐标同单位）"; return 3; }

    Point3DAlg_PointCloud pin = CloudViewOf(src);
    int* idxs = NULL;
    float* dists = NULL;
    int count = 0;
    const int rc = Point3DAlg_RadiusSearch(&pin, q, radius, &idxs, &dists, &count);
    if (rc != 0)  { out->error = "半径搜索失败（内存不足或内部异常）"; return 2; }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "radius=%.4g hits=%d query=(%.4g,%.4g,%.4g) ",
                       radius, count, q[0], q[1], q[2]);
    for (int i = 0; i < count && i < 30 && pos < (int)sizeof(msg) - 32; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos, "%s%d:%.4g",
                        i ? " " : "", idxs[i], (double)dists[i]);
    Point3DAlg_FreeSearchResult(idxs, dists);   // 库 malloc，配对回收（NULL 安全）
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（RadiusSearch）

// ---------------- 算子入口：厚度图（node.json: alg.point3d.thickness_map） ----------------
// Point3DAlg_ThicknessMap（api.h §9.13 heightmap 组）：同一工件上/下表面两点云
// 沿 Z 配对求差（板厚/锡膏厚度）——栅格范围 = 两云有效点 XY 包围盒并集，上云
// 每格取最高 z、下云每格取最低 z，同格双有效 → 厚度 = zTop - zBottom（**负厚度
// 保留不钳制**，数据异常由业务按消息里 min/max 判定）。
// HeightMap 容器无法跨节点流转（height_map 节点同款限制），本入口在节点内
// 归一化出 8U BGRA image：厚度 tmin→0 / tmax→255、无效格黑（口径与 height_map
// 的 zmin/zmax 归一化一致；tmin==tmax 等厚时有效格按 0 灰度，不把整图拉满）。
// 参数（按 node.json params 声明顺序）：cell(number>0)。
// 输入：两片点云（clouds[0]=顶面/上表面，clouds[1]=底面/下表面——cloud_diff
// 双云输入先例）；输出：厚度灰度图（BGRA，row 0 = y 最小）。
// 消息通道："width=W height=H cell=C thickness=[tmin~tmax] valid=P/T(R%)"。
// 错误码：1 输入未连线；2 点云为空 / 内存不足或内部异常；3 cell<=0（-3）；
//         4 栅格化失败（-2：无有效点/超 2000 万格上限）或上下表面无重叠格。
// 内存契约：hm 由库 malloc，全部路径 FreeHeightMap 回收（height_map 先例）。

#ifdef POINT3DALG_HAS_PCL
// @comui-node-begin
// {
//   "node": {"id":"alg.point3d.thickness_map","name":"厚度图","group":"3D 点云","category":"测量","description":"同一工件上/下表面两点云沿 Z 配对求差（板厚/锡膏厚度测量）：栅格范围取两云 XY 包围盒并集，顶面每格取最高 z、底面每格取最低 z，同格双有效 → 厚度 = zTop - zBottom（负厚度保留不钳制，按消息 min/max 判定数据异常）；输出厚度归一化 8U 灰度图（tmin→0、tmax→255，无效格黑，row 0 = y 最小，与 2D 图像坐标系一致，下游可接 2D 全部图像算子）；尺寸/cell/厚度 min/max/有效率并入节点消息"},
//   "inputs": [
//     {"name":"顶面点云","type":"cloud"},
//     {"name":"底面点云","type":"cloud"}
//   ],
//   "params": [
//     {"name":"cell","label":"栅格边长","type":"number","default":0.1,"min":0.0001,"max":1000000,"description":"栅格边长（与点云坐标同单位，>0；总格数上限 2000 万，超出报错）"}
//   ],
//   "outputs": [
//     {"name":"厚度图","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Point3DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Point3DAlg/include/Point3DAlg_api.h","src/Point3DAlg/Point3DAlg_CloudOps.cpp","src/ShapelyAlg/CloudProcessor.h","src/ShapelyAlg/CloudProcessor.cpp"], "includes": ["src","src/ShapelyAlg"]}
// }
// @comui-node-end
extern "C" POINT3DALG_EXPORT int Point3DAlg_ComuiThicknessMap(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 2 || !in->clouds) { out->error = "需要两片输入点云（顶面/底面）"; return 1; }
    const AlgoCCloud* ctop = &in->clouds[0];
    const AlgoCCloud* cbot = &in->clouds[1];
    if (ctop->count <= 0 || !ctop->xyz) { out->error = "顶面点云为空"; return 2; }
    if (cbot->count <= 0 || !cbot->xyz) { out->error = "底面点云为空"; return 2; }

    double cell = 0.1;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) cell = p->d;
    if (cell <= 0.0) { out->error = "cell 须 > 0（栅格边长，与点云坐标同单位）"; return 3; }

    Point3DAlg_PointCloud ptop = CloudViewOf(ctop);
    Point3DAlg_PointCloud pbot = CloudViewOf(cbot);
    Point3DAlg_HeightMap hm = {};
    const int rc = Point3DAlg_ThicknessMap(&ptop, &pbot, cell, &hm);
    if (rc == -3) { out->error = "栅格化失败（cell<=0）"; return 3; }
    if (rc == -2) { out->error = "栅格化失败：无有效点或超 2000 万格上限（调大 cell）"; return 4; }
    if (rc != 0)  { out->error = "厚度图失败（内存不足或内部异常）"; return 2; }
    if (hm.cells == NULL || hm.mask == NULL || hm.width <= 0 || hm.height <= 0) {
        Point3DAlg_FreeHeightMap(&hm);
        out->error = "厚度图失败：返回图无效";
        return 2;
    }

    // 厚度 min/max（仅 mask=1 同格双有效格参与；负值保留不钳制）
    const int total = hm.width * hm.height;
    double tmin = 0.0, tmax = 0.0;
    int valid_n = 0;
    for (int i = 0; i < total; ++i) {
        if (!hm.mask[i]) continue;
        const double t = (double)hm.cells[i];
        if (valid_n == 0) { tmin = tmax = t; }
        else {
            if (t < tmin) tmin = t;
            if (t > tmax) tmax = t;
        }
        ++valid_n;
    }
    if (valid_n == 0) {   // rc=0 但无同格双有效格：两云 XY 范围不相交（接口语义允许）
        Point3DAlg_FreeHeightMap(&hm);
        out->error = "上下表面无重叠格（两云 XY 范围不相交或无同格有效对），厚度图为空";
        return 4;
    }
    const double range = tmax - tmin;
    uint8_t* px = (uint8_t*)malloc((size_t)total * 4);
    if (!px) { Point3DAlg_FreeHeightMap(&hm); out->error = "内存不足"; return 2; }
    for (int i = 0; i < total; ++i) {
        uint8_t g = 0;
        if (hm.mask[i] && range > 0.0) {
            double t = ((double)hm.cells[i] - tmin) / range;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            g = (uint8_t)(t * 255.0 + 0.5);
        }
        px[i * 4] = g; px[i * 4 + 1] = g; px[i * 4 + 2] = g; px[i * 4 + 3] = 255;
    }

    AlgoCImageOut* arr = (AlgoCImageOut*)malloc(sizeof(AlgoCImageOut));
    if (!arr) {
        free(px);
        Point3DAlg_FreeHeightMap(&hm);
        out->error = "内存不足"; return 2;
    }
    arr->width = hm.width;
    arr->height = hm.height;
    arr->bgra = px;
    out->images = arr;
    out->imageCount = 1;

    static char msg[256];
    snprintf(msg, sizeof(msg),
             "width=%d height=%d cell=%.4g thickness=[%.4g~%.4g] valid=%d/%d(%.1f%%)",
             hm.width, hm.height, hm.cell, tmin, tmax,
             valid_n, total, total > 0 ? 100.0 * valid_n / total : 0.0);
    Point3DAlg_FreeHeightMap(&hm);
    out->error = msg;
    return 0;
}
#endif   // POINT3DALG_HAS_PCL（ThicknessMap）
