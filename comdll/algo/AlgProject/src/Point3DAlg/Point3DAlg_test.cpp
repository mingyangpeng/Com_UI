// Point3DAlg_test.cpp —— Point3DAlg C API 测试入口（控制台 exe）
// 分两部分：
//   A. 通用 C API 测试（合成数据，恒跑）：版本/生命周期、点云容器、
//      LoadCloud/SaveCloudEx 往返、Filter/Extract/Transform 全 method 行为、
//      错误码与 NULL 安全；1.2.0 追加 A6~A13：体素一致性（FilterCloud(voxel)
//      ↔ 测试内参考实现）、FitModel、SegmentCloud、KNN/半径搜索、
//      MeasureCloud、法线/特征/关键点、表面重建/MeshOp、HeightMap 桥接。
//   B. SICK 端到端 + 深层接口（有 .dat 数据才跑，否则打印 SKIP）：
//      LoadSickConfig → SickReader 点云/深度图/保存（既有链路）+ 句柄族
//      LoadRawInto/GetSickInfo/GetSickRowY/GetSickRaw/GetSickIntensity/
//      GetFovBoxes/StitchQuality。
// 用法：Point3DAlg_test.exe [相机1目录] [相机2目录 ...]（每个目录含 *.dat；缺省 = 内置双相机路径）
// 退出码：0=通过（SICK 数据缺失时该部分打印 SKIP）  1=用例失败/配置读取失败
//          2=保存失败  3=处理异常
// 编译宏：POINT3DALG_TEST_HAS_PCL=1（CMake 在 PCL_FOUND 时定义）时才编译
// 点云计算族测试段（对应 DLL 内 PCL 守卫导出的符号，无 PCL 时符号不存在）。

#include "include/Point3DAlg_api.h"
#include "include/Point3DAlg_SickApi.h"
#include "include/Point3DAlg_SickReader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include <windows.h>   // FindFirstFileA（数据目录 .dat 探测）

// ---------------- 测试参数（缺省值，可用命令行参数覆盖） ----------------
// 用法：Point3DAlg_test.exe [相机1目录] [相机2目录 ...]
//   缺省相机目录 = Z:/ScanImage/3D、3D-2；输出文件写到**当前工作目录**
static const char* kFolders[] = { "Z:/ScanImage/3D", "Z:/ScanImage/3D-2" };   // 各相机扫描文件夹
static const int   kDeviceCount = 2;                                          // 相机数（1=单相机）
static const int   kFrame  = -1;                   // 指定帧（>=0），<0 = 全部帧
static const char* kOutCloud     = "sick_cloud.ply";        // 点云输出（PLY 二进制，cwd）
static const char* kOutDepth     = "sick_depth.png";        // 深度图输出（16 位 PNG，cwd）
static const char* kOutDepthView = "sick_depth_view.png";   // 深度图可视输出（8 位 PNG，cwd）
static const char* kOutDepthFov  = "sick_depth_fov.png";    // FOV 标注图（框+序号，cwd）

static int g_fail = 0;

static void check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_fail;
}

// =====================================================================
// A. 通用 C API 测试（不依赖外部数据）
// =====================================================================

// 生命周期 / 版本 / 容器释放（恒跑，无 PCL 依赖）
static void TestLifecycle()
{
    std::printf("---- A1. 版本与生命周期 ----\n");
    check(std::strcmp(Point3DAlg_GetVersion(), "1.3.0") == 0, "GetVersion 返回 1.3.0");
    check(Point3DAlg_Init() == 0, "Init 返回 0");
    Point3DAlg_Cleanup();   // 占位实现，可安全调用
    Point3DAlg_FreeCloud(nullptr);   // NULL 安全（不崩溃即通过）
    check(true, "FreeCloud(NULL) 安全");
}

#if POINT3DALG_TEST_HAS_PCL
// ---------------- 合成点云 ----------------
// 基底：20×20 规则网格（步距 0.5，x/y∈[0,9.5]，z≈1 带微噪声）+ 3 个远端
// 离群点（z 也远端，便于统计/半径离群与直通滤波判定）。共 403 点，全有限。
static const int kGridN = 20;         // 每轴网格点数 → 400 点
static const float kStep = 0.5f;      // 网格步距
static const int kOutlierN = 3;       // 离群点数
static const int kBaseCount = kGridN * kGridN + kOutlierN;   // 403

static void PushPt(std::vector<float>& v, float x, float y, float z)
{
    v.push_back(x);
    v.push_back(y);
    v.push_back(z);
}

static void FillSynthetic(std::vector<float>& v, bool with_nan)
{
    v.clear();
    for (int i = 0; i < kGridN; ++i)
        for (int j = 0; j < kGridN; ++j)
            PushPt(v, i * kStep, j * kStep,
                   1.0f + 0.01f * std::sin(i * 0.7f) * std::cos(j * 0.7f));
    PushPt(v, 100.0f, 100.0f, 50.0f);    // 离群 1
    PushPt(v, -80.0f, 60.0f, -40.0f);    // 离群 2
    PushPt(v, 90.0f, -70.0f, 30.0f);     // 离群 3
    if (with_nan) {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        PushPt(v, nan, 0.0f, 0.0f);      // NaN 点（removeNonFinite 用）
    }
}

static Point3DAlg_PointCloud MakeCloud(bool with_nan = false)
{
    std::vector<float> v;
    FillSynthetic(v, with_nan);
    Point3DAlg_PointCloud c;
    c.count = (int)(v.size() / 3);
    c.xyz = (float*)malloc(v.size() * sizeof(float));
    memcpy(c.xyz, v.data(), v.size() * sizeof(float));
    return c;
}

// 第 i 点坐标断言（tol 比较三个分量）
static bool PtClose(const Point3DAlg_PointCloud& c, int i, double x, double y, double z, double tol)
{
    if (i < 0 || i >= c.count)
        return false;
    return std::fabs((double)c.xyz[i * 3 + 0] - x) <= tol &&
           std::fabs((double)c.xyz[i * 3 + 1] - y) <= tol &&
           std::fabs((double)c.xyz[i * 3 + 2] - z) <= tol;
}

// ---------------- 滤波（9 个 method） ----------------
static void TestFilter()
{
    std::printf("---- A2. FilterCloud（9 method）----\n");
    Point3DAlg_PointCloud in = MakeCloud();

    // 参数/入参校验
    {
        Point3DAlg_PointCloud out;
        const Point3DAlg_PointCloud* nullIn = nullptr;
        check(Point3DAlg_FilterCloud(nullIn, 0, nullptr, 0, &out) == -1, "FilterCloud in=NULL → -1");
        Point3DAlg_PointCloud empty;
        empty.xyz = nullptr;
        empty.count = 0;
        check(Point3DAlg_FilterCloud(&empty, 0, nullptr, 0, &out) == -1, "FilterCloud 空点云 → -1");
        check(Point3DAlg_FilterCloud(&in, 11, nullptr, 0, &out) == -3, "method 越界(11) → -3");
        check(Point3DAlg_FilterCloud(&in, 10, nullptr, 0, &out) == -3,
              "method=10 无 params → -3（approxVoxel 需 leaf）");
        const double leafBad[1] = { 0.0 };
        check(Point3DAlg_FilterCloud(&in, 0, leafBad, 1, &out) == -3, "voxel leaf<=0 → -3");
        check(Point3DAlg_FilterCloud(&in, 0, leafBad, 0, &out) == -3, "voxel params 缺失 → -3");
        const double everyBad[1] = { 0.0 };
        check(Point3DAlg_FilterCloud(&in, 2, everyBad, 1, &out) == -3, "uniform every_k<=0 → -3");
        const double ptBad[3] = { 2.0, 1.5, 0.5 };
        check(Point3DAlg_FilterCloud(&in, 6, ptBad, 3, &out) == -3, "passThrough min>max → -3");
    }

    struct CloudGuard {
        Point3DAlg_PointCloud c;
        ~CloudGuard() { Point3DAlg_FreeCloud(&c); }
    };

    // 0 voxel：leaf=2.0 → 5×5 网格体素 + 3 离群体素 = 28（质心替代，减点）
    {
        CloudGuard out;
        const double p[1] = { 2.0 };
        const int rc = Point3DAlg_FilterCloud(&in, 0, p, 1, &out.c);
        check(rc == 0 && out.c.count >= 26 && out.c.count <= 28,
              "voxel leaf=2.0 减点（~28 体素质心）");
    }
    // 0 voxel：leaf=1000 → PCL 体素索引按绝对坐标 floor(p/leaf) 分格，
    //   网格与 2 个离群点同落 (0,0,0)，第 3 个离群点（负坐标）单独成格 → 3 点
    {
        CloudGuard out;
        const double p[1] = { 1000.0 };
        const int rc = Point3DAlg_FilterCloud(&in, 0, p, 1, &out.c);
        check(rc == 0 && out.c.count == 3, "voxel leaf=1000 → 3 点（绝对坐标分格）");
    }
    // 1 random：固定种子取 50 → 恰好 50 点
    {
        CloudGuard out;
        const double p[2] = { 50.0, 42.0 };
        const int rc = Point3DAlg_FilterCloud(&in, 1, p, 2, &out.c);
        check(rc == 0 && out.c.count == 50, "random sample=50 → 50 点");
    }
    // 2 uniform：每 10 点取 1 → 41 点
    {
        CloudGuard out;
        const double p[1] = { 10.0 };
        const int rc = Point3DAlg_FilterCloud(&in, 2, p, 1, &out.c);
        check(rc == 0 && out.c.count == 41, "uniform every_k=10 → 41 点");
    }
    // 3 farthest：取 30 → 恰好 30 点
    {
        CloudGuard out;
        const double p[1] = { 30.0 };
        const int rc = Point3DAlg_FilterCloud(&in, 3, p, 1, &out.c);
        check(rc == 0 && out.c.count == 30, "farthest num=30 → 30 点");
    }
    // 4 removeNonFinite：含 1 个 NaN 的云 → 恰好剔除 1 点
    {
        Point3DAlg_PointCloud inNan = MakeCloud(true);
        CloudGuard out;
        const int rc = Point3DAlg_FilterCloud(&inNan, 4, nullptr, 0, &out.c);
        check(rc == 0 && out.c.count == inNan.count - 1, "removeNonFinite 剔除 NaN 点");
        Point3DAlg_FreeCloud(&inNan);
    }
    // 5 removeDuplicates：追加 10 个重复点 → 恢复 403
    {
        Point3DAlg_PointCloud dup = MakeCloud();
        std::vector<float> v(dup.xyz, dup.xyz + (size_t)dup.count * 3);
        v.insert(v.end(), dup.xyz, dup.xyz + 10 * 3);   // 前 10 点原样复制
        Point3DAlg_FreeCloud(&dup);
        Point3DAlg_PointCloud inDup;
        inDup.xyz = (float*)malloc(v.size() * sizeof(float));
        memcpy(inDup.xyz, v.data(), v.size() * sizeof(float));
        inDup.count = (int)(v.size() / 3);
        CloudGuard out;
        const int rc = Point3DAlg_FilterCloud(&inDup, 5, nullptr, 0, &out.c);
        check(rc == 0 && out.c.count == kBaseCount, "removeDuplicates 去重 → 403");
        Point3DAlg_FreeCloud(&inDup);
    }
    // 6 passThrough：z∈[0.5,1.5] → 网格 400 点保留，离群（z=50/-40/30）剔除
    {
        CloudGuard out;
        const double p[3] = { 2.0, 0.5, 1.5 };
        const int rc = Point3DAlg_FilterCloud(&in, 6, p, 3, &out.c);
        check(rc == 0 && out.c.count == kGridN * kGridN, "passThrough z[0.5,1.5] → 400 点");
    }
    // 6 passThrough：区间全空 → -2
    {
        CloudGuard out;
        const double p[3] = { 2.0, 5.0, 6.0 };
        const int rc = Point3DAlg_FilterCloud(&in, 6, p, 3, &out.c);
        check(rc == -2, "passThrough 全滤除 → -2");
    }
    // 7 statisticalOutlier：mean_k=10, std=1.0 → 剔除 3 个离群 → 400
    {
        CloudGuard out;
        const double p[2] = { 10.0, 1.0 };
        const int rc = Point3DAlg_FilterCloud(&in, 7, p, 2, &out.c);
        check(rc == 0 && out.c.count == kGridN * kGridN, "statisticalOutlier 去噪 → 400 点");
    }
    // 8 radiusOutlier：radius=1.0, min=3 → 离群剔除 → 400
    {
        CloudGuard out;
        const double p[2] = { 1.0, 3.0 };
        const int rc = Point3DAlg_FilterCloud(&in, 8, p, 2, &out.c);
        check(rc == 0 && out.c.count == kGridN * kGridN, "radiusOutlier → 400 点");
    }
    // 9 conditionalXYZ：x∈(-0.1,10) 开区间 → 网格全保留，离群剔除
    {
        CloudGuard out;
        const double p[6] = { -0.1, 10.0, -100.0, 100.0, -100.0, 100.0 };
        const int rc = Point3DAlg_FilterCloud(&in, 9, p, 6, &out.c);
        check(rc == 0 && out.c.count == kGridN * kGridN, "conditionalXYZ x 范围 → 400 点");
    }
    Point3DAlg_FreeCloud(&in);
}

// ---------------- 提取与裁剪（5 个 method） ----------------
static void TestExtract()
{
    std::printf("---- A3. ExtractCloud（5 method）----\n");
    Point3DAlg_PointCloud in = MakeCloud();

    {
        Point3DAlg_PointCloud out;
        check(Point3DAlg_ExtractCloud(&in, 0, nullptr, 0, nullptr, 0, &out) == -1,
              "byIndices 索引缺失 → -1");
        const int badIdx[1] = { kBaseCount };
        check(Point3DAlg_ExtractCloud(&in, 0, badIdx, 1, nullptr, 0, &out) == -3,
              "byIndices 索引越界 → -3");
        const double boxBad[6] = { 5.0, 0, 0, -5.0, 0, 0 };
        check(Point3DAlg_ExtractCloud(&in, 2, nullptr, 0, boxBad, 6, &out) == -3,
              "cropBox min>max → -3");
        const double polyBad[4] = { 0, 0, 1, 1 };
        check(Point3DAlg_ExtractCloud(&in, 4, nullptr, 0, polyBad, 4, &out) == -3,
              "ROI 顶点 <3 → -3");
        check(Point3DAlg_ExtractCloud(&in, 5, nullptr, 0, nullptr, 0, &out) == -3,
              "method 越界 → -3");
    }

    struct CloudGuard {
        Point3DAlg_PointCloud c;
        ~CloudGuard() { Point3DAlg_FreeCloud(&c); }
    };

    // 0 byIndices：取 3 点且点值与输入一致
    {
        CloudGuard out;
        const int idx[3] = { 0, 5, 10 };
        const int rc = Point3DAlg_ExtractCloud(&in, 0, idx, 3, nullptr, 0, &out.c);
        const bool okPts = rc == 0 && out.c.count == 3 &&
                           PtClose(out.c, 0, in.xyz[0], in.xyz[1], in.xyz[2], 0.0) &&
                           PtClose(out.c, 1, in.xyz[15], in.xyz[16], in.xyz[17], 0.0) &&
                           PtClose(out.c, 2, in.xyz[30], in.xyz[31], in.xyz[32], 0.0);
        check(okPts, "byIndices 取点与输入一致");
    }
    // 1 byIndicesInverted：剔除 10 点 → 393
    {
        CloudGuard out;
        std::vector<int> idx(10);
        for (int i = 0; i < 10; ++i) idx[i] = i;
        const int rc = Point3DAlg_ExtractCloud(&in, 1, idx.data(), 10, nullptr, 0, &out.c);
        check(rc == 0 && out.c.count == kBaseCount - 10, "byIndicesInverted → 393 点");
    }
    // 2 cropBox：[(-1,-1,-1),(5,5,2)] → 网格 11×11=121，离群剔除
    {
        CloudGuard out;
        const double p[6] = { -1.0, -1.0, -1.0, 5.0, 5.0, 2.0 };
        const int rc = Point3DAlg_ExtractCloud(&in, 2, nullptr, 0, p, 6, &out.c);
        check(rc == 0 && out.c.count == 121, "cropBox 边界含 5.0 → 121 点");
    }
    // 3 cropOBB：单位旋转盒 center(5,5,0) 半长(5,5,2) → 网格全保留 400
    {
        CloudGuard out;
        double p[16] = { 0 };
        p[0] = 5.0; p[1] = 5.0; p[2] = 0.0;      // center
        p[3] = 5.0; p[4] = 5.0; p[5] = 2.0;      // extents（半长）
        p[6] = 1; p[10] = 1; p[14] = 1;          // rotation = 单位阵（行主序对角）
        p[15] = 0.0;                             // invert=0
        const int rc = Point3DAlg_ExtractCloud(&in, 3, nullptr, 0, p, 16, &out.c);
        check(rc == 0 && out.c.count == kGridN * kGridN, "cropOBB（单位旋转）→ 400 点");
        // invert=1 → 只剩 3 个盒外离群点
        CloudGuard outInv;
        p[15] = 1.0;
        const int rc2 = Point3DAlg_ExtractCloud(&in, 3, nullptr, 0, p, 16, &outInv.c);
        check(rc2 == 0 && outInv.c.count == kOutlierN, "cropOBB invert → 3 离群点");
    }
    // 4 cropPolygonROI：外扩方形 → 网格全保留，离群剔除
    {
        CloudGuard out;
        const double p[8] = { -0.1, -0.1, 10.1, -0.1, 10.1, 10.1, -0.1, 10.1 };
        const int rc = Point3DAlg_ExtractCloud(&in, 4, nullptr, 0, p, 8, &out.c);
        check(rc == 0 && out.c.count == kGridN * kGridN, "cropPolygonROI 方形 → 400 点");
    }
    Point3DAlg_FreeCloud(&in);
}

// ---------------- 空间变换（7 个 method） ----------------
static void TestTransform()
{
    std::printf("---- A4. TransformCloud（7 method）----\n");
    Point3DAlg_PointCloud in = MakeCloud();
    struct CloudGuard {
        Point3DAlg_PointCloud c;
        ~CloudGuard() { Point3DAlg_FreeCloud(&c); }
    };
    const double kTol = 1e-4;

    {
        Point3DAlg_PointCloud out;
        check(Point3DAlg_TransformCloud(&in, 0, nullptr, 15, &out) == -3,
              "applyMatrix 参数不足 → -3");
        const double axisBad[4] = { 0.0, 0.0, 0.0, 1.0 };
        check(Point3DAlg_TransformCloud(&in, 2, axisBad, 4, &out) == -3,
              "rotate 零轴 → -3");
        const double planeBad[4] = { 0.0, 0.0, 0.0, 1.0 };
        check(Point3DAlg_TransformCloud(&in, 6, planeBad, 4, &out) == -3,
              "project 零法向 → -3");
        // 回归（1.2.0 review）：NaN/Inf 轴或法向此前会被 ==0 判定漏过，
        // 底层归一化产出全 NaN 点云且返回 0（静默垃圾）——契约改为 -3
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double axisNan[4] = { nan, 0.0, 1.0, 1.0 };
        check(Point3DAlg_TransformCloud(&in, 2, axisNan, 4, &out) == -3,
              "rotate NaN 轴 → -3");
        const double angleNan[4] = { 0.0, 0.0, 1.0, nan };
        check(Point3DAlg_TransformCloud(&in, 2, angleNan, 4, &out) == -3,
              "rotate NaN 角度 → -3");
        const double projNan[4] = { nan, 0.0, 1.0, -1.0 };
        check(Point3DAlg_TransformCloud(&in, 6, projNan, 4, &out) == -3,
              "project NaN 法向 → -3");
        const double rotN3[7] = { 0.0, 0.0, 0.0, nan, 0.0, 1.0, 1.0 };
        check(Point3DAlg_TransformCloud(&in, 3, rotN3, 7, &out) == -3,
              "rotateAround NaN 轴 → -3");
        check(Point3DAlg_TransformCloud(&in, 7, nullptr, 0, &out) == -3,
              "method 越界 → -3");
    }

    // 1 translate：(1,2,3) → 第 0 点 (0,0,z0) → (1,2,z0+3)
    {
        CloudGuard out;
        const double p[3] = { 1.0, 2.0, 3.0 };
        const int rc = Point3DAlg_TransformCloud(&in, 1, p, 3, &out.c);
        check(rc == 0 && out.c.count == in.count &&
                  PtClose(out.c, 0, 1.0, 2.0, (double)in.xyz[2] + 3.0, kTol),
              "translate 数值正确");
    }
    // 0 applyMatrix：单位阵 → 原样；平移矩阵 → 与 translate 一致
    {
        CloudGuard out;
        double p[16] = { 0 };
        p[0] = p[5] = p[10] = p[15] = 1.0;
        const int rc = Point3DAlg_TransformCloud(&in, 0, p, 16, &out.c);
        check(rc == 0 && out.c.count == in.count &&
                  PtClose(out.c, 7, in.xyz[21], in.xyz[22], in.xyz[23], kTol),
              "applyMatrix 单位阵不变");
        CloudGuard outT;
        p[3] = 1.0; p[7] = 2.0; p[11] = 3.0;
        const int rc2 = Point3DAlg_TransformCloud(&in, 0, p, 16, &outT.c);
        check(rc2 == 0 && PtClose(outT.c, 0, 1.0, 2.0, (double)in.xyz[2] + 3.0, kTol),
              "applyMatrix 平移 = translate");
    }
    // 2 rotate：绕 Z 轴 90°。网格点 2 = (0,1,z)（x 外层 / y 内层填充），
    //   右手系 +90° → (−1, 0, z)
    {
        CloudGuard out;
        const double p[4] = { 0.0, 0.0, 1.0, 3.14159265358979323846 / 2.0 };
        const int rc = Point3DAlg_TransformCloud(&in, 2, p, 4, &out.c);
        check(rc == 0 && PtClose(out.c, 2, -1.0, 0.0, (double)in.xyz[8], kTol),
              "rotate Z 90° 数值正确");
    }
    // 3 rotateAround：绕 (5,5,0) Z 轴 180° → (6,5,z) → (4,5,z)
    {
        CloudGuard out;
        const double p[7] = { 5.0, 5.0, 0.0, 0.0, 0.0, 1.0, 3.14159265358979323846 };
        const int rc = Point3DAlg_TransformCloud(&in, 3, p, 7, &out.c);
        const int i = 12;   // 网格点 (6.0, 0.0)
        const double x0 = in.xyz[i * 3], y0 = in.xyz[i * 3 + 1], z0 = in.xyz[i * 3 + 2];
        check(rc == 0 && PtClose(out.c, i, 10.0 - x0, 10.0 - y0, z0, kTol),
              "rotateAround 绕 (5,5) 180° 数值正确");
    }
    // 4 scale：(2,2,1)，网格点 2 = (0,1,z) → (0,2,z)
    {
        CloudGuard out;
        const double p[3] = { 2.0, 2.0, 1.0 };
        const int rc = Point3DAlg_TransformCloud(&in, 4, p, 3, &out.c);
        const double z2 = in.xyz[8];   // 点 2 自身的 z
        check(rc == 0 && PtClose(out.c, 2, 0.0, 2.0, z2, kTol), "scale 数值正确");
    }
    // 5 scaleAround：center=(0,0,0) 等价 scale；center=(5,5,0) s=(2,2,1) →
    //   点 (0,0,z) → (-5,-5,z)
    {
        CloudGuard out;
        const double p[6] = { 2.0, 2.0, 1.0, 5.0, 5.0, 0.0 };
        const int rc = Point3DAlg_TransformCloud(&in, 5, p, 6, &out.c);
        const double z0 = in.xyz[2];
        check(rc == 0 && PtClose(out.c, 0, -5.0, -5.0, z0, kTol),
              "scaleAround 绕 (5,5,0) 数值正确");
    }
    // 6 project：z=1 平面 (0,0,1,-1) → 全部 z=1，x/y 不变
    {
        CloudGuard out;
        const double p[4] = { 0.0, 0.0, 1.0, -1.0 };
        const int rc = Point3DAlg_TransformCloud(&in, 6, p, 4, &out.c);
        bool ok = rc == 0 && out.c.count == in.count;
        for (int i = 0; ok && i < out.c.count; ++i) {
            const double z = out.c.xyz[i * 3 + 2];
            if (std::fabs(z - 1.0) > kTol ||
                std::fabs((double)out.c.xyz[i * 3] - in.xyz[i * 3]) > kTol ||
                std::fabs((double)out.c.xyz[i * 3 + 1] - in.xyz[i * 3 + 1]) > kTol)
                ok = false;
        }
        check(ok, "project z=1 平面（x/y 不变，z 全 1）");
    }
    Point3DAlg_FreeCloud(&in);
}

// ---------------- 文件读写往返 ----------------
static void TestCloudIO()
{
    std::printf("---- A5. LoadCloud / SaveCloudEx 往返 ----\n");
    Point3DAlg_PointCloud in = MakeCloud();
    struct CloudGuard {
        Point3DAlg_PointCloud c;
        ~CloudGuard() { Point3DAlg_FreeCloud(&c); }
    };

    // 错误路径
    {
        Point3DAlg_PointCloud out;
        check(Point3DAlg_LoadCloud(nullptr, 0, nullptr, &out) == -1, "LoadCloud path=NULL → -1");
        check(Point3DAlg_LoadCloud("", 0, nullptr, &out) == -1, "LoadCloud path 空串 → -1");
        check(Point3DAlg_LoadCloud("no_such_cloud_file.pcd", 0, nullptr, &out) == -2,
              "LoadCloud 文件不存在 → -2");
        check(Point3DAlg_LoadCloud("_p3d_rt.ply", 7, nullptr, &out) == -3, "LoadCloud type 越界 → -3");
        Point3DAlg_PointCloud empty;
        empty.xyz = nullptr;
        empty.count = 0;
        check(Point3DAlg_SaveCloudEx("_p3d_rt.ply", 1, nullptr, 1) == -1, "SaveCloudEx cloud=NULL → -1");
        check(Point3DAlg_SaveCloudEx("_p3d_rt.ply", 1, &empty, 1) == -1, "SaveCloudEx 空点云 → -1");
        check(Point3DAlg_SaveCloudEx("_p3d_rt.ply", 7, &in, 1) == -3, "SaveCloudEx type 越界 → -3");
    }

    // PLY 二进制往返（float 精确）
    {
        const int rcS = Point3DAlg_SaveCloudEx("_p3d_rt.ply", 1, &in, 1);
        CloudGuard out;
        const int rcL = Point3DAlg_LoadCloud("_p3d_rt.ply", 1, nullptr, &out.c);
        check(rcS == 0 && rcL == 0 && out.c.count == in.count &&
                  PtClose(out.c, 0, in.xyz[0], in.xyz[1], in.xyz[2], 0.0) &&
                  PtClose(out.c, in.count - 1,
                          (double)in.xyz[(in.count - 1) * 3],
                          (double)in.xyz[(in.count - 1) * 3 + 1],
                          (double)in.xyz[(in.count - 1) * 3 + 2], 0.0),
              "PLY 二进制往返点数与坐标一致");
    }
    // PLY ASCII 往返（9 位有效数字 → 1e-5 容差）
    {
        const int rcS = Point3DAlg_SaveCloudEx("_p3d_rt.ply", 1, &in, 0);
        CloudGuard out;
        const int rcL = Point3DAlg_LoadCloud("_p3d_rt.ply", 1, nullptr, &out.c);
        check(rcS == 0 && rcL == 0 && out.c.count == in.count &&
                  PtClose(out.c, 100, (double)in.xyz[300], (double)in.xyz[301],
                          (double)in.xyz[302], 1e-5),
              "PLY ASCII 往返一致");
    }
    // PCD 往返（恒二进制，binary 参数无效）
    {
        const int rcS = Point3DAlg_SaveCloudEx("_p3d_rt.pcd", 0, &in, 0);
        CloudGuard out;
        const int rcL = Point3DAlg_LoadCloud("_p3d_rt.pcd", 0, nullptr, &out.c);
        check(rcS == 0 && rcL == 0 && out.c.count == in.count, "PCD 往返点数一致");
    }
    // TXT 往返（空格分隔）
    {
        const int rcS = Point3DAlg_SaveCloudEx("_p3d_rt.txt", 2, &in, 1);
        CloudGuard out;
        const int rcL = Point3DAlg_LoadCloud("_p3d_rt.txt", 2, " ", &out.c);
        check(rcS == 0 && rcL == 0 && out.c.count == in.count &&
                  PtClose(out.c, 5, (double)in.xyz[15], (double)in.xyz[16],
                          (double)in.xyz[17], 1e-5),
              "TXT 往返（空格分隔）一致");
    }
    // TXT 分隔符语义：空格分隔的文件用逗号解析 → 无有效行 → -2
    {
        CloudGuard out;
        const int rcL = Point3DAlg_LoadCloud("_p3d_rt.txt", 2, ",", &out.c);
        check(rcL == -2, "TXT 分隔符不符 → -2");
    }

    std::remove("_p3d_rt.ply");
    std::remove("_p3d_rt.pcd");
    std::remove("_p3d_rt.txt");
    Point3DAlg_FreeCloud(&in);
}

// ---------------- 1.2.0 测试辅助 ----------------

static const double kPi = 3.14159265358979323846;

// 由 float 数组构造输入点云（malloc，调用方用 FreeCloud 释放）
static Point3DAlg_PointCloud MakeCloudFrom(const std::vector<float>& v)
{
    Point3DAlg_PointCloud c;
    c.count = (int)(v.size() / 3);
    c.xyz = (float*)malloc(v.size() * sizeof(float));
    memcpy(c.xyz, v.data(), v.size() * sizeof(float));
    return c;
}

// 球面点云（含两极）：心 (cx,cy,cz) 半径 r，lat×lon 经纬采样
static void FillSphere(std::vector<float>& v, double cx, double cy, double cz,
                       double r, int lat, int lon)
{
    for (int i = 1; i < lat; ++i) {
        for (int j = 0; j < lon; ++j) {
            const double th = kPi * i / lat, ph = 2.0 * kPi * j / lon;
            PushPt(v, (float)(cx + r * std::sin(th) * std::cos(ph)),
                   (float)(cy + r * std::sin(th) * std::sin(ph)),
                   (float)(cz + r * std::cos(th)));
        }
    }
    PushPt(v, (float)cx, (float)cy, (float)(cz + r));
    PushPt(v, (float)cx, (float)cy, (float)(cz - r));
}

// 圆柱面点云：轴过 (cx,cy) 平行 z，半径 r，layers 层（层距 dz），sectors 角；
// zNoise > 0 时叠加确定性 z 抖动（打破"单层内邻域严格共面"——精确圆柱面上
// 邻域散布矩阵恒秩 2 → e3 = 0，ISS 显著性要求 e3 > 0 会全部拒绝，见 A11）
static void FillCylinder(std::vector<float>& v, double cx, double cy, double r,
                         int layers, int sectors, double dz, double zNoise = 0.0)
{
    for (int k = 0; k < layers; ++k)
        for (int j = 0; j < sectors; ++j) {
            const double a = 2.0 * kPi * j / sectors;
            const double z = k * dz + zNoise * std::sin(j * 0.7 + k * 1.3);
            PushPt(v, (float)(cx + r * std::cos(a)), (float)(cy + r * std::sin(a)),
                   (float)z);
        }
}

// 圆锥面点云：顶点在原点、轴 +Z、半开角 halfDeg（度），rings 环（首环高 z0）
static void FillCone(std::vector<float>& v, double halfDeg, int rings, int sectors,
                     double z0, double stepZ)
{
    const double tanHalf = std::tan(halfDeg * kPi / 180.0);
    for (int ring = 0; ring < rings; ++ring) {
        const double h = z0 + stepZ * ring;
        const double r = h * tanHalf;
        for (int k = 0; k < sectors; ++k) {
            const double t = 2.0 * kPi * k / sectors;
            PushPt(v, (float)(r * std::cos(t)), (float)(r * std::sin(t)), (float)h);
        }
    }
}

// 5×5 网格（步距 1，x/y∈[0,4]）：z = base + i + j；addHigh 追加 z = 100+i+j 层
static void FillGrid5(std::vector<float>& v, bool addHigh)
{
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            PushPt(v, (float)i, (float)j, (float)(i + j));
            if (addHigh)
                PushPt(v, (float)i, (float)j, (float)(100.0 + i + j));
        }
}

// ---------------- A6. 体素一致性（§12 遗留项 src 侧可做部分） ----------------
// FilterCloud(voxel) ↔ 测试内参考实现（floor(p/leaf) 绝对坐标分格取质心）。
// PCL VoxelGrid 输出按打包线性索引排序（z 主序），参考实现键序取 (iz,iy,ix)。
static void TestVoxelConsistency()
{
    std::printf("---- A6. FilterCloud(voxel) vs 参考实现一致性 ----\n");
    Point3DAlg_PointCloud in = MakeCloud();
    struct CloudGuard {
        Point3DAlg_PointCloud c;
        ~CloudGuard() { Point3DAlg_FreeCloud(&c); }
    };

    const double leaf = 2.0;
    CloudGuard out;
    const double p[1] = { leaf };
    const int rc = Point3DAlg_FilterCloud(&in, 0, p, 1, &out.c);
    check(rc == 0, "FilterCloud(voxel leaf=2.0) 成功");

    struct Acc {
        double sx = 0, sy = 0, sz = 0;
        int cnt = 0;
    };
    std::map<std::array<int, 3>, Acc> ref;
    for (int i = 0; i < in.count; ++i) {
        const double x = in.xyz[i * 3 + 0], y = in.xyz[i * 3 + 1], z = in.xyz[i * 3 + 2];
        const std::array<int, 3> key = { (int)std::floor(z / leaf),
                                         (int)std::floor(y / leaf),
                                         (int)std::floor(x / leaf) };
        Acc& a = ref[key];
        a.sx += x;
        a.sy += y;
        a.sz += z;
        ++a.cnt;
    }

    bool ok = rc == 0 && out.c.count == (int)ref.size();
    int i = 0;
    for (const auto& kv : ref) {
        if (!ok)
            break;
        const Acc& a = kv.second;
        if (i >= out.c.count ||
            std::fabs((double)out.c.xyz[i * 3 + 0] - a.sx / a.cnt) > 1e-3 ||
            std::fabs((double)out.c.xyz[i * 3 + 1] - a.sy / a.cnt) > 1e-3 ||
            std::fabs((double)out.c.xyz[i * 3 + 2] - a.sz / a.cnt) > 1e-3)
            ok = false;
        ++i;
    }
    std::printf("    (参考格数 %d, PCL 输出 %d 点)\n", (int)ref.size(), out.c.count);
    check(ok, "体素分格与质心和参考实现一致");
    Point3DAlg_FreeCloud(&in);
}

// ---------------- A7. FitModel（5 model × 3 fit + 内点 + 容量两段式） ----------------
static void TestFitModel()
{
    std::printf("---- A7. FitModel（plane/sphere/cylinder/circle3D/cone）----\n");
    struct CloudGuard {
        Point3DAlg_PointCloud c;
        ~CloudGuard() { Point3DAlg_FreeCloud(&c); }
    };

    // 错误路径（403 点通用合成云）
    {
        Point3DAlg_PointCloud in = MakeCloud();
        double c[8] = { 0 };
        int nC = 0, nI = 0;
        int* inl = nullptr;
        const Point3DAlg_PointCloud* nullIn = nullptr;
        check(Point3DAlg_FitModel(nullIn, 0, 0, 0.01, c, 8, &inl, &nC) == -1,
              "FitModel in=NULL → -1");
        check(Point3DAlg_FitModel(&in, 6, 0, 0.01, c, 8, &inl, &nC) == -3,
              "method 越界(6) → -3");
        check(Point3DAlg_FitModel(&in, 0, 3, 0.01, c, 8, &inl, &nC) == -3,
              "fit 越界 → -3");
        check(Point3DAlg_FitModel(&in, 0, 0, 0.0, c, 8, &inl, &nC) == -3,
              "threshold<=0 → -3");
        check(Point3DAlg_FitModel(&in, 0, 0, 0.01, c, 3, &inl, &nC) == -3 && nC == 4,
              "plane capacity=3 → -3 且回填所需=4");
        Point3DAlg_FreeCloud(&in);
    }

    // 0 plane：z=0.5 平面（15×15，噪声 ±0.003，阈值 0.02 → 全内点）
    {
        std::vector<float> v;
        for (int i = 0; i < 15; ++i)
            for (int j = 0; j < 15; ++j)
                PushPt(v, i * 0.5f, j * 0.5f,
                       0.5f + 0.003f * std::sin(i * 1.3f) * std::cos(j * 1.3f));
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        double c[8] = { 0 };
        int nC = 0;
        int* inl = nullptr;
        const int rc = Point3DAlg_FitModel(&in, 0, 0, 0.02, c, 8, &inl, &nC);
        // 口径：成功时 *out_inlier_count = 内点数（系数个数由 method 定，plane=4）
        const bool ok = rc == 0 && nC == 225 &&
                        std::fabs(c[0]) < 0.05 && std::fabs(c[1]) < 0.05 &&
                        std::fabs(std::fabs(c[2]) - 1.0) < 1e-3 &&
                        std::fabs(std::fabs(c[3]) - 0.5) < 0.01 &&
                        inl != nullptr;
        check(ok, "plane RANSAC 系数 [a,b,c,d] / 225 全内点");
        Point3DAlg_FreeBuffer(inl);
        // MSAC / LMS（LMEDS）同样收敛
        const int rc1 = Point3DAlg_FitModel(&in, 0, 1, 0.02, c, 8, nullptr, &nC);
        check(rc1 == 0 && std::fabs(std::fabs(c[2]) - 1.0) < 1e-3, "plane MSAC 收敛");
        const int rc2 = Point3DAlg_FitModel(&in, 0, 2, 0.02, c, 8, nullptr, &nC);
        check(rc2 == 0 && std::fabs(std::fabs(c[2]) - 1.0) < 1e-3, "plane LMS(LMEDS) 收敛");
        Point3DAlg_FreeCloud(&in);
    }

    // 5 line3D（1.3.0）：沿 X 轴直线 + 噪声，三种 fit 全收敛
    {
        std::vector<float> v;
        for (int i = 0; i < 120; ++i) {
            v.push_back((float)i);
            v.push_back((float)(i % 3) * 0.01f);
            v.push_back((float)(i % 5) * 0.01f);
        }
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        double c[8] = { 0 };
        int nC = 0;
        int* inl = nullptr;
        const int rc = Point3DAlg_FitModel(&in, 5, 0, 0.05, c, 6, &inl, &nC);
        check(rc == 0 && nC >= 100, "line3D RANSAC 收敛（内点 >= 100/120）");
        check(rc == 0 && std::fabs(std::fabs(c[3]) - 1.0) < 1e-3,
              "line3D 方向 ≈ ±X（单位方向分量 |dx| = 1）");
        check(rc == 0 && std::fabs(c[4]) * std::fabs(c[4]) + std::fabs(c[5]) * std::fabs(c[5]) < 1e-6,
              "line3D 方向 Z 分量 ≈ 0");
        const int rc1 = Point3DAlg_FitModel(&in, 5, 1, 0.05, c, 6, nullptr, &nC);
        check(rc1 == 0, "line3D MSAC 收敛");
        const int rcL = Point3DAlg_FitModel(&in, 5, 2, 0.05, c, 6, nullptr, &nC);
        check(rcL == 0, "line3D LMS(LMEDS) 收敛");
        check(Point3DAlg_FitModel(&in, 5, 0, 0.05, c, 4, nullptr, &nC) == -3 && nC == 6,
              "line3D capacity=4 → -3 两段式回填 6");
        Point3DAlg_FreeBuffer(inl);
        Point3DAlg_FreeCloud(&in);
    }

    // 1 sphere：心 (5,5,5) r=3（10×20 经纬 + 两极 = 172 点）
    {
        std::vector<float> v;
        FillSphere(v, 5.0, 5.0, 5.0, 3.0, 10, 20);
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        double c[8] = { 0 };
        int nC = 0;
        const int rc = Point3DAlg_FitModel(&in, 1, 0, 0.05, c, 8, nullptr, &nC);
        const bool ok = rc == 0 && nC >= 100 &&
                        std::fabs(c[0] - 5.0) < 0.2 && std::fabs(c[1] - 5.0) < 0.2 &&
                        std::fabs(c[2] - 5.0) < 0.2 && std::fabs(c[3] - 3.0) < 0.2;
        check(ok, "sphere RANSAC [cx,cy,cz,r] ≈ (5,5,5,3)");
        Point3DAlg_FreeCloud(&in);
    }

    // 2 cylinder：轴过 (2,3) 平行 z，r=4，20 层 × 40 角 = 800 点
    {
        std::vector<float> v;
        FillCylinder(v, 2.0, 3.0, 4.0, 20, 40, 0.5);
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        double c[8] = { 0 };
        int nC = 0;
        const int rc = Point3DAlg_FitModel(&in, 2, 0, 0.05, c, 8, nullptr, &nC);
        const double axisZ = std::fabs(c[5]);   // c[3..5] = 单位轴方向
        // 轴上点的 XY 应落在圆柱轴位 (2,3) 附近（轴竖直时任意高度的轴点 XY 相同）
        const double axisOff = std::sqrt((c[0] - 2.0) * (c[0] - 2.0) +
                                         (c[1] - 3.0) * (c[1] - 3.0));
        const bool ok = rc == 0 && nC >= 500 &&
                        std::fabs(c[6] - 4.0) < 0.2 && axisZ > 0.95 && axisOff < 0.3;
        check(ok, "cylinder [轴上点,单位轴,半径] r≈4 / 轴≈z");
        Point3DAlg_FreeCloud(&in);
    }

    // 3 circle3D：z=30 平面圆，心 (10,20,30) r=8，36 点
    {
        std::vector<float> v;
        for (int k = 0; k < 36; ++k) {
            const double t = k * 10.0 * kPi / 180.0;
            PushPt(v, (float)(10.0 + 8.0 * std::cos(t)),
                   (float)(20.0 + 8.0 * std::sin(t)), 30.0f);
        }
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        double c[8] = { 0 };
        int nC = 0;
        const int rc = Point3DAlg_FitModel(&in, 3, 0, 0.05, c, 8, nullptr, &nC);
        const bool ok = rc == 0 && nC >= 30 &&
                        std::fabs(c[0] - 10.0) < 0.2 && std::fabs(c[1] - 20.0) < 0.2 &&
                        std::fabs(c[2] - 30.0) < 0.2 &&
                        std::fabs(std::fabs(c[5]) - 1.0) < 1e-3 &&
                        std::fabs(c[6] - 8.0) < 0.15;
        check(ok, "circle3D [圆心,法向,半径] 心(10,20,30) r≈8");
        Point3DAlg_FreeCloud(&in);
    }

    // 4 cone：顶点原点、轴 +Z、半开角 30°（38 环 × 16 角 = 608 点，阈值 0.3）
    {
        std::vector<float> v;
        FillCone(v, 30.0, 38, 16, 5.0, 0.4);
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        double c[8] = { 0 };
        int nC = 0;
        const int rc = Point3DAlg_FitModel(&in, 4, 0, 0.3, c, 8, nullptr, &nC);
        const double angDeg = c[6] * 180.0 / kPi;
        const bool ok = rc == 0 && nC >= 500 &&
                        std::fabs(c[0]) < 0.3 && std::fabs(c[1]) < 0.3 &&
                        std::fabs(c[2]) < 0.3 &&
                        std::fabs(std::fabs(c[5]) - 1.0) < 1e-3 &&
                        std::fabs(angDeg - 30.0) < 0.5;
        check(ok, "cone [顶点,轴,半开角] apex≈原点 / 30°");
        Point3DAlg_FreeCloud(&in);
    }
}

// ---------------- A8. SegmentCloud（5 method + FreeClouds） ----------------
static void TestSegmentCloud()
{
    std::printf("---- A8. SegmentCloud（聚类/平面族）----\n");
    struct CloudGuard {
        Point3DAlg_PointCloud c;
        ~CloudGuard() { Point3DAlg_FreeCloud(&c); }
    };
    struct SegsGuard {
        Point3DAlg_PointCloud* s = nullptr;
        int n = 0;
        ~SegsGuard() { Point3DAlg_FreeClouds(s, n); }
    };

    // 错误路径
    {
        Point3DAlg_PointCloud in = MakeCloud();
        Point3DAlg_PointCloud* segs = nullptr;
        int nSeg = 0;
        const double p1[1] = { 0.05 };
        check(Point3DAlg_SegmentCloud(&in, 5, p1, 1, &segs, &nSeg) == -3,
              "SegmentCloud method 越界 → -3");
        check(Point3DAlg_SegmentCloud(&in, 1, p1, 1, &segs, &nSeg) == -3,
              "euclideanCluster params 数量不符 → -3");
        check(Point3DAlg_SegmentCloud(&in, 0, p1, 1, nullptr, &nSeg) == -1,
              "out_segments=NULL → -1");
        Point3DAlg_FreeCloud(&in);
        Point3DAlg_FreeClouds(nullptr, 0);   // NULL 安全
        check(true, "FreeClouds(NULL) 安全");
    }

    // 两团分离点云：团 A 4×4×2（32 点，原点附近）、团 B 5×5×2（50 点，x=30）
    {
        std::vector<float> v;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 2; ++k)
                    PushPt(v, (i - 1.5f) * 0.5f, (j - 1.5f) * 0.5f, (k - 0.5f) * 0.5f);
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j)
                for (int k = 0; k < 2; ++k)
                    PushPt(v, 30.0f + (i - 2) * 0.5f, (j - 2) * 0.5f, (k - 0.5f) * 0.5f);
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        const double pE[3] = { 2.0, 5.0, 1000.0 };
        SegsGuard g;
        const int rc = Point3DAlg_SegmentCloud(&in, 1, pE, 3, &g.s, &g.n);
        const bool ok = rc == 0 && g.n == 2 && g.s[0].count == 50 && g.s[1].count == 32;
        check(ok, "euclideanCluster → 2 簇（50+32，点数守恒）");
        const double pD[2] = { 2.0, 4.0 };
        SegsGuard g2;
        const int rc2 = Point3DAlg_SegmentCloud(&in, 2, pD, 2, &g2.s, &g2.n);
        const bool ok2 = rc2 == 0 && g2.n == 2 &&
                         g2.s[0].count + g2.s[1].count == 82;
        check(ok2, "dbscan → 2 簇（点数守恒 82）");
        Point3DAlg_FreeCloud(&in);
    }

    // planeRANSAC：z=1 平面网格（10×10）+ 10 个远端点 → [100, 10]
    {
        std::vector<float> v;
        for (int i = 0; i < 10; ++i)
            for (int j = 0; j < 10; ++j)
                PushPt(v, (float)i, (float)j, 1.0f);
        for (int i = 0; i < 10; ++i)
            PushPt(v, 50.0f + i, 0.0f, 5.0f);
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        const double p[1] = { 0.05 };
        SegsGuard g;
        const int rc = Point3DAlg_SegmentCloud(&in, 0, p, 1, &g.s, &g.n);
        const bool ok = rc == 0 && g.n == 2 &&
                        g.s[0].count == 100 && g.s[1].count == 10;
        check(ok, "planeRANSAC → [局内 100, 其余 10]");
        Point3DAlg_FreeCloud(&in);
    }

    // 回归（1.2.0 review）：契约"恒 2 段"——全部点都在平面上时 [1] 为空段
    // （count=0）而非收缩成 1 段，调用方按下标配段不需判段数分支
    {
        std::vector<float> v;
        for (int i = 0; i < 10; ++i)
            for (int j = 0; j < 10; ++j)
                PushPt(v, (float)i, (float)j, 1.0f);
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        const double p[1] = { 0.05 };
        SegsGuard g;
        const int rc = Point3DAlg_SegmentCloud(&in, 0, p, 1, &g.s, &g.n);
        const bool ok = rc == 0 && g.n == 2 && g.s[0].count == 100 &&
                        g.s[1].count == 0;
        check(ok, "planeRANSAC 全内点 → 恒 2 段（[1] 空段 count=0）");
        Point3DAlg_FreeCloud(&in);
    }

    // 回归（1.2.0 review）：dbscan 输入含 NaN 点 → 与其他分割 method 一致
    // 按有效点聚类（此前底层 labels 与剔除后的 clean 云对齐、长度不符被
    // 误报 -2；现封装层先剔除并映射回原始坐标）
    {
        std::vector<float> v;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 2; ++k)
                    PushPt(v, (i - 1.5f) * 0.5f, (j - 1.5f) * 0.5f, (k - 0.5f) * 0.5f);
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j)
                for (int k = 0; k < 2; ++k)
                    PushPt(v, 30.0f + (i - 2) * 0.5f, (j - 2) * 0.5f, (k - 0.5f) * 0.5f);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        PushPt(v, nan, 0.0f, 0.0f);   // 1 个 NaN 点（82 有效点 + 1）
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        const double pD[2] = { 2.0, 4.0 };
        SegsGuard g;
        const int rc = Point3DAlg_SegmentCloud(&in, 2, pD, 2, &g.s, &g.n);
        int total = 0;
        for (int i = 0; i < g.n; ++i)
            total += g.s[i].count;
        const bool ok = rc == 0 && g.n == 2 && total == 82;   // NaN 不计入任何段
        check(ok, "dbscan 含 NaN 点 → 2 簇点数守恒 82（NaN 剔除）");
        Point3DAlg_FreeCloud(&in);
    }

    // regionGrowing：水平面 + 45° 斜坡（法向差 45° > 阈值 5°）→ 2 区域各 100
    {
        std::vector<float> v;
        for (int i = 0; i < 10; ++i)
            for (int j = 0; j < 10; ++j) {
                PushPt(v, (float)i, (float)j, 1.0f);
                PushPt(v, 20.0f + i, (float)j, (float)i);
            }
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        const double p[5] = { 10.0, 5.0, 0.1, 10.0, 500.0 };
        SegsGuard g;
        const int rc = Point3DAlg_SegmentCloud(&in, 3, p, 5, &g.s, &g.n);
        bool ok = rc == 0 && g.n == 2;
        for (int i = 0; ok && i < g.n; ++i)
            ok = g.s[i].count == 100;
        check(ok, "regionGrowing → 水平面/斜坡 2 区域各 100");
        Point3DAlg_FreeCloud(&in);
    }

    // planesMulti：两个平行平面 z=1 / z=5（各 100 点）→ 2 段
    {
        std::vector<float> v;
        for (int i = 0; i < 10; ++i)
            for (int j = 0; j < 10; ++j) {
                PushPt(v, (float)i, (float)j, 1.0f);
                PushPt(v, (float)i, (float)j, 5.0f);
            }
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        const double p[3] = { 0.05, 2.0, 50.0 };
        SegsGuard g;
        const int rc = Point3DAlg_SegmentCloud(&in, 4, p, 3, &g.s, &g.n);
        bool ok = rc == 0 && g.n == 2;
        for (int i = 0; ok && i < g.n; ++i)
            ok = g.s[i].count == 100;
        check(ok, "planesMulti → 2 平面各 100 局内点");
        Point3DAlg_FreeCloud(&in);
    }
}

// ---------------- A9. KNN / 半径搜索（已知近邻 + NULL 安全） ----------------
static void TestSearch()
{
    std::printf("---- A9. KnnSearch / RadiusSearch ----\n");
    std::vector<float> v;
    for (int i = 0; i <= 10; ++i)
        PushPt(v, (float)i, 0.0f, 0.0f);   // 11 点线云 (i,0,0)
    Point3DAlg_PointCloud in = MakeCloudFrom(v);

    const Point3DAlg_PointCloud* nullIn = nullptr;
    int* idx = nullptr;
    float* dist = nullptr;
    int n = 0;
    const double q0[3] = { 0.6, 0.0, 0.0 };
    const double q1[3] = { 0.0, 0.0, 0.0 };
    check(Point3DAlg_KnnSearch(nullIn, q1, 3, &idx, &dist, &n) == -1,
          "KnnSearch in=NULL → -1");
    check(Point3DAlg_KnnSearch(&in, nullptr, 3, &idx, &dist, &n) == -1,
          "KnnSearch query=NULL → -1");
    check(Point3DAlg_KnnSearch(&in, q0, 0, &idx, &dist, &n) == -3, "k=0 → -3");
    check(Point3DAlg_RadiusSearch(&in, q1, 0.0, &idx, &dist, &n) == -3,
          "radius=0 → -3");

    // KNN：query (0.6,0,0) k=3 → 索引 {1,0,2}（0.4/0.6/1.4 升序，欧氏距离）
    const int rcK = Point3DAlg_KnnSearch(&in, q0, 3, &idx, &dist, &n);
    const bool okK = rcK == 0 && n == 3 && idx && dist &&
                     idx[0] == 1 && idx[1] == 0 && idx[2] == 2 &&
                     std::fabs(dist[0] - 0.4f) < 1e-5 &&
                     std::fabs(dist[1] - 0.6f) < 1e-5 &&
                     std::fabs(dist[2] - 1.4f) < 1e-5;
    check(okK, "KNN 已知近邻顺序与欧氏距离");
    Point3DAlg_FreeSearchResult(idx, dist);
    idx = nullptr;
    dist = nullptr;

    // KNN k 超过点数 → 按实际点数返回
    const int rcK2 = Point3DAlg_KnnSearch(&in, q1, 100, &idx, &dist, &n);
    check(rcK2 == 0 && n == 11, "KNN k>点数 → 返回 11");
    Point3DAlg_FreeSearchResult(idx, dist);
    idx = nullptr;
    dist = nullptr;

    // 半径：query (0,0,0) r=2.5 → {0,1,2}，距离 {0,1,2}
    const int rcR = Point3DAlg_RadiusSearch(&in, q1, 2.5, &idx, &dist, &n);
    const bool okR = rcR == 0 && n == 3 && idx && dist &&
                     idx[0] == 0 && idx[1] == 1 && idx[2] == 2 &&
                     std::fabs(dist[0]) < 1e-6 &&
                     std::fabs(dist[1] - 1.0f) < 1e-5 &&
                     std::fabs(dist[2] - 2.0f) < 1e-5;
    check(okR, "Radius 已知命中数与距离");
    Point3DAlg_FreeSearchResult(idx, dist);
    idx = nullptr;
    dist = nullptr;

    // 只要索引不要距离（out_dists=NULL 合法）
    const int rcN = Point3DAlg_KnnSearch(&in, q0, 2, &idx, nullptr, &n);
    check(rcN == 0 && n == 2 && idx != nullptr, "out_dists=NULL 只取索引");
    Point3DAlg_FreeSearchResult(idx, nullptr);
    idx = nullptr;
    dist = nullptr;

    // 回归（1.2.0 review）：契约"按距离升序"对两接口都成立——底层
    // radiusSearch（FLANN 半径搜索）不保证有序，封装层已排序修复。
    // 用 3D 网格云逐查询点断言 dists 非降且 idx/dist 保持一一对应
    {
        std::vector<float> gv;
        for (int i = 0; i < 7; ++i)
            for (int j = 0; j < 7; ++j)
                for (int k = 0; k < 7; ++k)
                    PushPt(gv, (float)i, (float)j, (float)k);   // 343 点网格
        Point3DAlg_PointCloud grid = MakeCloudFrom(gv);
        bool mono = true;
        for (int qi = 0; mono && qi < 343; qi += 17) {   // 抽样 21 个查询点
            const double qq[3] = { gv[(size_t)qi * 3] + 0.3, gv[(size_t)qi * 3 + 1] + 0.2,
                                   gv[(size_t)qi * 3 + 2] + 0.4 };
            int* gi = nullptr;
            float* gd = nullptr;
            int gn = 0;
            const int rcG = Point3DAlg_RadiusSearch(&grid, qq, 2.0, &gi, &gd, &gn);
            if (rcG != 0) {
                mono = false;
                continue;
            }
            for (int t = 1; t < gn; ++t)
                if (!(gd[t - 1] <= gd[t] + 1e-6f))
                    mono = false;
            // idx 与 dist 对应性：距离重算与索引处点一致（排序未拆对）
            for (int t = 0; mono && t < gn; ++t) {
                const double dx = grid.xyz[(size_t)gi[t] * 3] - qq[0];
                const double dy = grid.xyz[(size_t)gi[t] * 3 + 1] - qq[1];
                const double dz = grid.xyz[(size_t)gi[t] * 3 + 2] - qq[2];
                const double dd = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (std::fabs(dd - (double)gd[t]) > 1e-4)
                    mono = false;
            }
            Point3DAlg_FreeSearchResult(gi, gd);
        }
        check(mono, "RadiusSearch 距离升序且 idx/dist 一一对应（网格云抽样）");
        // KNN 升序同样守卫（底层本就有序，回归口径防退化）
        int* ki = nullptr;
        float* kd = nullptr;
        int kn = 0;
        const double kq[3] = { 3.2, 3.1, 3.9 };
        const int rcK3 = Point3DAlg_KnnSearch(&grid, kq, 40, &ki, &kd, &kn);
        bool kMono = rcK3 == 0 && kn == 40;
        for (int t = 1; kMono && t < kn; ++t)
            kMono = kd[t - 1] <= kd[t] + 1e-6f;
        check(kMono, "KnnSearch 距离升序（40 近邻）");
        Point3DAlg_FreeSearchResult(ki, kd);
        Point3DAlg_FreeCloud(&grid);
    }

    Point3DAlg_FreeSearchResult(nullptr, nullptr);   // NULL 安全
    check(true, "FreeSearchResult(NULL,NULL) 安全");
    Point3DAlg_FreeCloud(&in);
}

// ---------------- A10. MeasureCloud（8 method 数值验证） ----------------
static void TestMeasureCloud()
{
    std::printf("---- A10. MeasureCloud（AABB/OBB/质心/协方差/距离/平面度/夹角/交线）----\n");
    // 8 角点盒 (0,0,0)-(2,3,4)：AABB/OBB/质心/协方差全部有解析值
    std::vector<float> v;
    for (int i = 0; i < 8; ++i)
        PushPt(v, (float)(i & 1) * 2.0f, (float)((i >> 1) & 1) * 3.0f,
               (float)((i >> 2) & 1) * 4.0f);
    Point3DAlg_PointCloud box = MakeCloudFrom(v);

    const Point3DAlg_PointCloud* nullIn = nullptr;
    int nV = 0;
    check(Point3DAlg_MeasureCloud(nullIn, nullptr, 0, nullptr, 0, nullptr, 0, &nV) == -1,
          "MeasureCloud in=NULL → -1");
    check(Point3DAlg_MeasureCloud(&box, nullptr, 8, nullptr, 0, nullptr, 0, &nV) == -3,
          "method 越界 → -3");

    double val[32] = { 0 };
    const int rcA = Point3DAlg_MeasureCloud(&box, nullptr, 0, nullptr, 0, val, 32, &nV);
    check(rcA == 0 && nV == 6 &&
              std::fabs(val[0]) < 1e-6 && std::fabs(val[1]) < 1e-6 &&
              std::fabs(val[2]) < 1e-6 && std::fabs(val[3] - 2.0) < 1e-6 &&
              std::fabs(val[4] - 3.0) < 1e-6 && std::fabs(val[5] - 4.0) < 1e-6,
          "AABB = [0,0,0,2,3,4]");

    const int rcO = Point3DAlg_MeasureCloud(&box, nullptr, 1, nullptr, 0, val, 32, &nV);
    {
        // OBB：中心 = 盒心；半长排序 = {2,1.5,1}；旋转正交
        double ext[3] = { val[3], val[4], val[5] };
        std::sort(ext, ext + 3, std::greater<double>());
        double rr[3] = { 0, 0, 0 };   // R·R^T 对角（行模长平方）
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                rr[r] += val[6 + r * 3 + c] * val[6 + r * 3 + c];
        check(rcO == 0 && nV == 15 &&
                  std::fabs(val[0] - 1.0) < 1e-3 && std::fabs(val[1] - 1.5) < 1e-3 &&
                  std::fabs(val[2] - 2.0) < 1e-3 &&
                  std::fabs(ext[0] - 2.0) < 1e-3 && std::fabs(ext[1] - 1.5) < 1e-3 &&
                  std::fabs(ext[2] - 1.0) < 1e-3 &&
                  std::fabs(rr[0] - 1.0) < 1e-6 && std::fabs(rr[1] - 1.0) < 1e-6 &&
                  std::fabs(rr[2] - 1.0) < 1e-6,
              "OBB 中心/半长/正交旋转");
    }

    const int rcC = Point3DAlg_MeasureCloud(&box, nullptr, 2, nullptr, 0, val, 32, &nV);
    check(rcC == 0 && nV == 3 && std::fabs(val[0] - 1.0) < 1e-9 &&
              std::fabs(val[1] - 1.5) < 1e-9 && std::fabs(val[2] - 2.0) < 1e-9,
          "centroid = (1,1.5,2)");

    const int rcM = Point3DAlg_MeasureCloud(&box, nullptr, 3, nullptr, 0, val, 32, &nV);
    check(rcM == 0 && nV == 12 && std::fabs(val[0] - 1.0) < 1e-9 &&
              std::fabs(val[3 + 0] - 1.0) < 1e-3 &&
              std::fabs(val[3 + 4] - 2.25) < 1e-3 && std::fabs(val[3 + 8] - 4.0) < 1e-3,
          "meanCovariance 12 值（均值 + 对角方差 1/2.25/4）");

    // capacity 不足 → -3 且回填所需长度
    check(Point3DAlg_MeasureCloud(&box, nullptr, 1, nullptr, 0, val, 14, &nV) == -3 &&
              nV == 15, "OBB capacity=14 → -3 且所需=15");

    // pointCloudDistance：in=2 点，other=1 点 → [5,4]；capacity 1 → -3 所需=2
    {
        std::vector<float> a, b;
        PushPt(a, 0.0f, 0.0f, 0.0f);
        PushPt(a, 1.0f, 0.0f, 0.0f);
        PushPt(b, 5.0f, 0.0f, 0.0f);
        Point3DAlg_PointCloud ca = MakeCloudFrom(a);
        Point3DAlg_PointCloud cb = MakeCloudFrom(b);
        const int rc = Point3DAlg_MeasureCloud(&ca, &cb, 4, nullptr, 0, val, 32, &nV);
        check(rc == 0 && nV == 2 && std::fabs(val[0] - 5.0) < 1e-6 &&
                  std::fabs(val[1] - 4.0) < 1e-6, "pointCloudDistance = [5,4]");
        check(Point3DAlg_MeasureCloud(&ca, nullptr, 4, nullptr, 0, val, 32, &nV) == -1,
              "pointCloudDistance other=NULL → -1");
        check(Point3DAlg_MeasureCloud(&ca, &cb, 4, nullptr, 0, val, 1, &nV) == -3 &&
                  nV == 2, "pointCloudDistance capacity=1 → -3 所需=2");
        Point3DAlg_FreeCloud(&ca);
        Point3DAlg_FreeCloud(&cb);
    }

    // flatness：z≈1 平面网格（扰动 ±0.005）阈值 0.05 → [pv<0.05, sigma>=0, 100]
    {
        std::vector<float> f;
        for (int i = 0; i < 10; ++i)
            for (int j = 0; j < 10; ++j)
                PushPt(f, (float)i, (float)j,
                       1.0f + 0.005f * std::sin(i * 0.9f) * std::cos(j * 0.9f));
        Point3DAlg_PointCloud in = MakeCloudFrom(f);
        const double p[1] = { 0.05 };
        const int rc = Point3DAlg_MeasureCloud(&in, nullptr, 5, p, 1, val, 32, &nV);
        check(rc == 0 && nV == 3 && val[0] > 0.0 && val[0] < 0.05 &&
                  val[1] >= 0.0 && val[1] < 0.05 && (int)val[2] == 100,
              "flatness [pv, sigma, inlier=100]");
        check(Point3DAlg_MeasureCloud(&in, nullptr, 5, p, 0, val, 32, &nV) == -3,
              "flatness params 数量不符 → -3");
        Point3DAlg_FreeCloud(&in);
    }

    // planeAngle / planeIntersection（params 传两面方程；in/other 忽略）
    {
        const double p90[8] = { 0, 0, 1, 0, 1, 0, 0, 0 };
        const int rc = Point3DAlg_MeasureCloud(nullptr, nullptr, 6, p90, 8, val, 32, &nV);
        check(rc == 0 && nV == 1 && std::fabs(val[0] - 90.0) < 1e-6,
              "planeAngle 90°（z 面 × x 面）");
        const double p45[8] = { 0, 0, 1, 0, 1, 0, 1, 0 };
        const int rc45 = Point3DAlg_MeasureCloud(nullptr, nullptr, 6, p45, 8, val, 32, &nV);
        check(rc45 == 0 && std::fabs(val[0] - 45.0) < 1e-6, "planeAngle 45°");
        const double pPar[8] = { 0, 0, 1, 0, 0, 0, 2, -5 };
        const int rcPar = Point3DAlg_MeasureCloud(nullptr, nullptr, 6, pPar, 8, val, 32, &nV);
        check(rcPar == 0 && std::fabs(val[0]) < 1e-9, "planeAngle 平行 → 0°");
        const double pZero[8] = { 0, 0, 0, 1, 1, 0, 0, 0 };
        check(Point3DAlg_MeasureCloud(nullptr, nullptr, 6, pZero, 8, val, 32, &nV) == -3,
              "planeAngle 零法向 → -3");
        const double pX[8] = { 0, 0, 1, -1, 1, 0, 0, -2 };
        const int rcI = Point3DAlg_MeasureCloud(nullptr, nullptr, 7, pX, 8, val, 32, &nV);
        // z=1 面 × x=2 面 → 交线过 (2,0,1)，方向 ±y
        check(rcI == 0 && nV == 6 && std::fabs(val[0] - 2.0) < 1e-5 &&
                  std::fabs(val[1]) < 1e-5 && std::fabs(val[2] - 1.0) < 1e-5 &&
                  std::fabs(std::fabs(val[4]) - 1.0) < 1e-5,
              "planeIntersection 点(2,0,1) 方向 ±y");
        const double pPl[8] = { 0, 0, 1, 0, 0, 0, 1, -2 };
        check(Point3DAlg_MeasureCloud(nullptr, nullptr, 7, pPl, 8, val, 32, &nV) == -2,
              "planeIntersection 近平行 → -2");
        // 回归（1.2.0 review）：method 7 零法向此前被近平行分支误报 -2，
        // 契约（"同 method 6"）为 -3——已修复，两平面各自零法向都验
        const double pZ1[8] = { 0, 0, 0, 1, 1, 0, 0, 0 };
        const double pZ2[8] = { 0, 0, 1, 0, 0, 0, 0, -1 };
        check(Point3DAlg_MeasureCloud(nullptr, nullptr, 7, pZ1, 8, val, 32, &nV) == -3,
              "planeIntersection 平面1 零法向 → -3");
        check(Point3DAlg_MeasureCloud(nullptr, nullptr, 7, pZ2, 8, val, 32, &nV) == -3,
              "planeIntersection 平面2 零法向 → -3");
        check(Point3DAlg_MeasureCloud(nullptr, nullptr, 6, p90, 7, val, 32, &nV) == -3,
              "planeAngle params=7 → -3");
    }
    Point3DAlg_FreeCloud(&box);
}

// ---------------- A11. 法线 / 特征 / 关键点 ----------------
static void TestFeatureKeypoints()
{
    std::printf("---- A11. EstimateNormals / ComputeFeature / DetectKeypoints ----\n");
    struct CloudGuard {
        Point3DAlg_PointCloud c;
        ~CloudGuard() { Point3DAlg_FreeCloud(&c); }
    };
    // 10×10 平面网格（步距 1，z=1）：法线 ≈ ±z、协方差 zz=0、点距 = 1
    std::vector<float> g;
    for (int i = 0; i < 10; ++i)
        for (int j = 0; j < 10; ++j)
            PushPt(g, (float)i, (float)j, 1.0f);
    Point3DAlg_PointCloud grid = MakeCloudFrom(g);

    // 法线估计：KNN k=8 → 100 点，|nz|≈1
    float* nrm = nullptr;
    int nN = 0;
    const int rc0 = Point3DAlg_EstimateNormals(&grid, 0, 8.0, &nrm, &nN);
    bool ok0 = rc0 == 0 && nN == 100 && nrm != nullptr;
    for (int i = 0; ok0 && i < nN; ++i)
        ok0 = std::fabs(std::fabs(nrm[i * 4 + 2]) - 1.0f) < 1e-3;
    check(ok0, "EstimateNormals KNN k=8 → N×4（法向 ≈ ±z）");
    Point3DAlg_FreeBuffer(nrm);
    // 半径模式
    const int rc1 = Point3DAlg_EstimateNormals(&grid, 1, 1.5, &nrm, &nN);
    check(rc1 == 0 && nN == 100, "EstimateNormals radius=1.5 → 100 点");
    Point3DAlg_FreeBuffer(nrm);
    nrm = nullptr;
    check(Point3DAlg_EstimateNormals(&grid, 0, 2.0, &nrm, &nN) == -3, "k=2 → -3");
    check(Point3DAlg_EstimateNormals(&grid, 1, 0.0, &nrm, &nN) == -3, "radius=0 → -3");
    check(Point3DAlg_EstimateNormals(&grid, 2, 8.0, &nrm, &nN) == -3, "method 越界 → -3");

    // 特征：FPFH（内部法线）/ 协方差 / 马氏 / 最近邻距离
    float* feat = nullptr;
    int dim = 0, nF = 0;
    const int rcF = Point3DAlg_ComputeFeature(&grid, 0, 2.5, &feat, &dim, &nF);
    check(rcF == 0 && dim == 33 && nF == 100 && feat != nullptr,
          "FPFH dim=33 × 100 点");
    Point3DAlg_FreeBuffer(feat);
    feat = nullptr;
    const int rcCov = Point3DAlg_ComputeFeature(&grid, 1, 5.0, &feat, &dim, &nF);
    bool okCov = rcCov == 0 && dim == 9 && nF == 100;
    for (int i = 0; okCov && i < nF; ++i)
        okCov = std::fabs(feat[i * 9 + 8]) < 1e-6;   // 平面 zz≈0
    check(okCov, "协方差 dim=9（平面 zz≈0）");
    Point3DAlg_FreeBuffer(feat);
    feat = nullptr;
    {
        std::vector<float> nz(g);
        PushPt(nz, 50.0f, 50.0f, 50.0f);   // 远端离群 → 马氏距离显著大
        Point3DAlg_PointCloud noisy = MakeCloudFrom(nz);
        const int rcMd = Point3DAlg_ComputeFeature(&noisy, 2, 0.0, &feat, &dim, &nF);
        double maxGrid = 0.0;
        for (int i = 0; i < nF - 1; ++i)
            maxGrid = (std::max)(maxGrid, (double)feat[i]);
        check(rcMd == 0 && dim == 1 && nF == 101 && feat[nF - 1] > 5.0 &&
                  feat[nF - 1] > maxGrid,
              "马氏距离：离群点显著大（dim=1）");
        Point3DAlg_FreeCloud(&noisy);
        Point3DAlg_FreeBuffer(feat);
        feat = nullptr;
    }
    const int rcD = Point3DAlg_ComputeFeature(&grid, 3, 1.0, &feat, &dim, &nF);
    bool okD = rcD == 0 && dim == 1 && nF == 100;
    if (okD)
        okD = std::fabs(feat[0] - 1.0f) < 1e-3;   // 角点 (0,0) 最近邻距 = 1
    check(okD, "最近邻距离 dim=1（网格间距 1）");
    Point3DAlg_FreeBuffer(feat);
    feat = nullptr;
    check(Point3DAlg_ComputeFeature(&grid, 4, 1.0, &feat, &dim, &nF) == -3,
          "desc_type 越界 → -3");
    check(Point3DAlg_ComputeFeature(&grid, 1, 2.0, &feat, &dim, &nF) == -3,
          "协方差 k=2 → -3");
    check(Point3DAlg_ComputeFeature(&grid, 3, 0.0, &feat, &dim, &nF) == -3,
          "最近邻距离 k=0 → -3");

    // 关键点：Harris3D（threshold=0 全保留 / 大阈值 → -2）+ ISS3D（圆柱面）
    {
        std::vector<float> v;
        for (int i = 0; i < 20; ++i)
            for (int j = 0; j < 20; ++j)
                PushPt(v, i * 0.5f, j * 0.5f, 1.0f);
        PushPt(v, 2.0f, 2.0f, 3.0f);      // 5 个尖峰（斑点）
        PushPt(v, 6.0f, 2.0f, 3.0f);
        PushPt(v, 10.0f, 2.0f, 3.0f);
        PushPt(v, 14.0f, 2.0f, 3.0f);
        PushPt(v, 10.0f, 10.0f, 3.0f);
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        CloudGuard kp;
        const int rcK = Point3DAlg_DetectKeypoints(&in, 0, 1.0, 0.0, &kp.c);
        check(rcK == 0 && kp.c.count >= 1, "Harris3D threshold=0 检出关键点");
        // PCL Harris3D 默认 refine（切平面交点重定位）——本数据（平行法线区）
        // NNT 奇异不重定位，实测输出包围盒 = 输入包围盒，按输入包围盒 + 微容差校验
        bool okB = true;
        for (int i = 0; okB && i < kp.c.count; ++i) {
            const double x = kp.c.xyz[i * 3], y = kp.c.xyz[i * 3 + 1], z = kp.c.xyz[i * 3 + 2];
            okB = x >= -0.1 && x <= 14.1 && y >= -0.1 && y <= 10.1 &&
                  z >= 0.9 && z <= 3.1;
        }
        check(okB, "Harris3D 关键点落在输入包围盒外扩邻域内");
        const int rcHi = Point3DAlg_DetectKeypoints(&in, 0, 1.0, 1e6, &kp.c);
        check(rcHi == -2, "Harris3D 大阈值 → 0 关键点 → -2");
        const int rcIssHi = Point3DAlg_DetectKeypoints(&in, 1, 0.0, 0.5, &kp.c);
        check(rcIssHi == -3, "ISS3D salient_radius<=0 → -3");
        // 回归（1.2.0 review）：in == out 别名传入 → -1 且输入容器不被破坏
        //（此前 ResetOut 先于校验执行，会把 in->xyz 置空并泄漏后返回 -1）
        {
            const int savedCount = in.count;
            float* const savedXyz = in.xyz;
            const int rcAlias = Point3DAlg_DetectKeypoints(&in, 0, 1.0, 0.0, &in);
            check(rcAlias == -1 && in.count == savedCount && in.xyz == savedXyz,
                  "DetectKeypoints in==out → -1（输入容器原样保留）");
        }
        Point3DAlg_FreeCloud(&in);
        // ISS3D：带微噪声的圆柱面（层距 0.5 < salient 半径 1.0 → 邻域跨 3 层；
        // z 抖动打破精确圆柱面上邻域严格共面 → e3 > 0，显著性条件可满足）
        std::vector<float> vc;
        FillCylinder(vc, 2.0, 3.0, 2.0, 20, 80, 0.5, 0.02);   // 1600 点
        Point3DAlg_PointCloud cyl = MakeCloudFrom(vc);
        const int rcIss = Point3DAlg_DetectKeypoints(&cyl, 1, 1.0, 0.5, &kp.c);
        check(rcIss == 0 && kp.c.count >= 1, "ISS3D 圆柱面检出关键点");
        Point3DAlg_FreeCloud(&cyl);
    }
    Point3DAlg_FreeCloud(&grid);
}

// ---------------- A12. 表面重建 / MeshOp ----------------
static void TestSurfaceMesh()
{
    std::printf("---- A12. ReconstructSurface / MeshOp / FreeMesh ----\n");
    struct MeshGuard {
        Point3DAlg_Mesh m{ nullptr, 0, nullptr, 0 };
        ~MeshGuard() { Point3DAlg_FreeMesh(&m); }
    };

    // 立方体手工网格（8 顶点 12 面，表面积 24 / 体积 8）——MeshOp 数值基准
    Point3DAlg_Mesh cube;
    {
        static const float vs[24] = { -1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1,
                                      -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1 };
        static const int fs[36] = { 0, 2, 1, 0, 3, 2,   4, 5, 6, 4, 6, 7,
                                    0, 1, 5, 0, 5, 4,   2, 3, 7, 2, 7, 6,
                                    0, 4, 7, 0, 7, 3,   1, 2, 6, 1, 6, 5 };
        cube.vertex_count = 8;
        cube.vertices = (float*)malloc(sizeof(vs));
        memcpy(cube.vertices, vs, sizeof(vs));
        cube.face_count = 12;
        cube.faces = (int*)malloc(sizeof(fs));
        memcpy(cube.faces, fs, sizeof(fs));
    }
    double scalar = 0.0;
    float* arr = nullptr;
    int nArr = 0;
    check(Point3DAlg_MeshOp(&cube, 3, nullptr, 0, nullptr, &scalar, nullptr, nullptr) == 0 &&
              std::fabs(scalar - 24.0) < 1e-6, "MeshOp area 立方体 = 24");
    check(Point3DAlg_MeshOp(&cube, 4, nullptr, 0, nullptr, &scalar, nullptr, nullptr) == 0 &&
              std::fabs(scalar - 8.0) < 1e-6, "MeshOp volume 立方体 = 8");
    check(Point3DAlg_MeshOp(&cube, 6, nullptr, 0, nullptr, nullptr, &arr, &nArr) == 0 &&
              nArr == 12, "MeshOp faceNormals → 12 条");
    Point3DAlg_FreeBuffer(arr);
    arr = nullptr;
    check(Point3DAlg_MeshOp(&cube, 7, nullptr, 0, nullptr, nullptr, &arr, &nArr) == 0 &&
              nArr == 8 && std::fabs(std::sqrt((double)arr[0] * arr[0] + arr[1] * arr[1] +
                                               arr[2] * arr[2]) - 1.0) < 1e-3,
          "MeshOp vertexNormals → 8 条（角点单位法线）");
    Point3DAlg_FreeBuffer(arr);
    arr = nullptr;
    // 点到网格距离：(0,0,0)→1（面心距），(5,0,0)→4
    {
        const double q[6] = { 0, 0, 0, 5, 0, 0 };
        const int rc = Point3DAlg_MeshOp(&cube, 5, q, 6, nullptr, nullptr, &arr, &nArr);
        check(rc == 0 && nArr == 2 && std::fabs(arr[0] - 1.0) < 1e-4 &&
                  std::fabs(arr[1] - 4.0) < 1e-4, "MeshOp pointToMeshDistance = [1,4]");
        Point3DAlg_FreeBuffer(arr);
        arr = nullptr;
        check(Point3DAlg_MeshOp(&cube, 5, q, 4, nullptr, nullptr, &arr, &nArr) == -3,
              "pointToMeshDistance params 非 3 倍数 → -3");
    }
    // cropByOBB：上切片（z∈[0.4,1.4]）→ 只剩顶面 2 面 4 顶点
    {
        MeshGuard cropped;
        double p[15] = { 0 };
        p[0] = 0.0; p[1] = 0.0; p[2] = 0.9;         // center
        p[3] = 2.0; p[4] = 2.0; p[5] = 0.5;         // extents 半长
        p[6] = 1.0; p[10] = 1.0; p[14] = 1.0;       // rotation = 单位阵
        const int rc = Point3DAlg_MeshOp(&cube, 2, p, 15, &cropped.m, nullptr,
                                         nullptr, nullptr);
        check(rc == 0 && cropped.m.face_count == 2 && cropped.m.vertex_count == 4,
              "MeshOp cropByOBB 顶面切片 → 2 面 4 顶点");
        p[5] = 0.0;   // extents 非法 → -3
        check(Point3DAlg_MeshOp(&cube, 2, p, 15, &cropped.m, nullptr, nullptr, nullptr) == -3,
              "cropByOBB extents<=0 → -3");
    }
    // laplacianSmooth / clusterDecimate
    {
        MeshGuard sm;
        const double p[2] = { 5.0, 0.5 };
        const int rc = Point3DAlg_MeshOp(&cube, 0, p, 2, &sm.m, nullptr, nullptr, nullptr);
        check(rc == 0 && sm.m.face_count == 12 && sm.m.vertex_count == 8,
              "laplacianSmooth 顶点/面数不变");
        const double pb[2] = { 1.5, 2.0 };
        check(Point3DAlg_MeshOp(&cube, 0, pb, 2, &sm.m, nullptr, nullptr, nullptr) == -3,
              "laplacianSmooth lambda>1 → -3");
        MeshGuard dec;
        const double pc[1] = { 3.0 };
        const int rcD = Point3DAlg_MeshOp(&cube, 1, pc, 1, &dec.m, nullptr, nullptr, nullptr);
        check(rcD == 0 && dec.m.face_count > 0 && dec.m.vertex_count <= 8,
              "clusterDecimate cell=3 → 顶点合并");
    }
    // 输入网格无效 → -2；op 越界 → -3
    {
        Point3DAlg_Mesh bad = cube;
        bad.faces = nullptr;
        check(Point3DAlg_MeshOp(&bad, 3, nullptr, 0, nullptr, &scalar, nullptr, nullptr) == -2,
              "MeshOp 无面网格 → -2");
        check(Point3DAlg_MeshOp(&cube, 8, nullptr, 0, nullptr, &scalar, nullptr, nullptr) == -3,
              "MeshOp op 越界 → -3");
    }
    Point3DAlg_FreeMesh(&cube);   // 测试自构造（malloc）网格用 FreeMesh 归零释放
    Point3DAlg_FreeMesh(nullptr); // NULL 安全
    check(true, "FreeMesh(NULL) 安全");

    // 球面（R=10，12×24 经纬 + 两极 = 266 点）重建
    std::vector<float> sv;
    FillSphere(sv, 0.0, 0.0, 0.0, 10.0, 12, 24);
    Point3DAlg_PointCloud sphere = MakeCloudFrom(sv);
    {
        const Point3DAlg_PointCloud* nullIn = nullptr;
        MeshGuard m;
        check(Point3DAlg_ReconstructSurface(nullIn, 0, nullptr, 0, &m.m) == -1,
              "ReconstructSurface in=NULL → -1");
        check(Point3DAlg_ReconstructSurface(&sphere, 9, nullptr, 0, &m.m) == -3,
              "type 越界 → -3");
        const double alphaNeg[1] = { -1.0 };
        check(Point3DAlg_ReconstructSurface(&sphere, 1, alphaNeg, 1, &m.m) == -3,
              "concaveHull alpha<=0 → -3");
        const double mlsBad[1] = { 0.0 };
        check(Point3DAlg_ReconstructSurface(&sphere, 2, mlsBad, 1, &m.m) == -3,
              "MLS radius<=0 → -3");
        check(Point3DAlg_ReconstructSurface(&sphere, 7, nullptr, 0, &m.m) == -3,
              "gridProjection 缺 params → -3");
    }
    // 0 convexHull → 面数>0，MeshOp area/volume 与解析值同量级（4πR²≈1257, 4πR³/3≈4189）
    {
        MeshGuard hull;
        const int rc = Point3DAlg_ReconstructSurface(&sphere, 0, nullptr, 0, &hull.m);
        check(rc == 0 && hull.m.vertex_count >= 4 && hull.m.face_count > 0,
              "convexHull 球 → 顶点/面数>0");
        if (rc == 0 && hull.m.face_count > 0) {
            double area = 0.0, vol = 0.0;
            Point3DAlg_MeshOp(&hull.m, 3, nullptr, 0, nullptr, &area, nullptr, nullptr);
            Point3DAlg_MeshOp(&hull.m, 4, nullptr, 0, nullptr, &vol, nullptr, nullptr);
            std::printf("    (凸包 area=%.1f volume=%.1f，解析 %.1f/%.1f)\n",
                        area, vol, 4.0 * kPi * 100.0, 4.0 / 3.0 * kPi * 1000.0);
            check(area > 700.0 && area < 1400.0 && std::fabs(vol) > 2500.0 &&
                      std::fabs(vol) < 4600.0,
                  "凸包 area/volume 与解析值同量级");
        }
        // laplacianSmooth 保持规模
        MeshGuard sm;
        const double p[2] = { 5.0, 0.3 };
        const int rcS = Point3DAlg_MeshOp(&hull.m, 0, p, 2, &sm.m, nullptr, nullptr, nullptr);
        check(rcS == 0 && sm.m.face_count == hull.m.face_count &&
                  sm.m.vertex_count == hull.m.vertex_count,
              "凸包 laplacianSmooth 规模不变");
    }
    // 1 concaveHull（平面网格 alpha=2）→ 面>0
    {
        std::vector<float> v;
        for (int i = 0; i < 10; ++i)
            for (int j = 0; j < 10; ++j)
                PushPt(v, (float)i, (float)j, 0.0f);
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        MeshGuard m;
        const double a[1] = { 2.0 };
        const int rc = Point3DAlg_ReconstructSurface(&in, 1, a, 1, &m.m);
        check(rc == 0 && m.m.face_count > 0 && m.m.vertex_count >= 3,
              "concaveHull 平面 → 面>0");
        Point3DAlg_FreeCloud(&in);
    }
    // 2 MLS：输出仅顶点（face_count=0），规模 = 输入 100 点
    {
        std::vector<float> v;
        for (int i = 0; i < 10; ++i)
            for (int j = 0; j < 10; ++j)
                PushPt(v, (float)i, (float)j, 1.0f + 0.05f * std::sin(i * 1.1f));
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        MeshGuard m;
        const double p[1] = { 2.0 };
        const int rc = Point3DAlg_ReconstructSurface(&in, 2, p, 1, &m.m);
        check(rc == 0 && m.m.vertex_count == 100 && m.m.face_count == 0,
              "MLS → 100 顶点 / 0 面（点云算子口径）");
        Point3DAlg_FreeCloud(&in);
    }
    // 3 Poisson（depth=6, iso=5）→ 顶点落在半径 10 球面附近
    {
        MeshGuard m;
        const double p[3] = { 6.0, 5.0, 1.0 };
        const int rc = Point3DAlg_ReconstructSurface(&sphere, 3, p, 3, &m.m);
        double maxErr = 0.0;
        for (int i = 0; m.m.vertices && i < m.m.vertex_count; ++i) {
            const double x = m.m.vertices[i * 3], y = m.m.vertices[i * 3 + 1],
                         z = m.m.vertices[i * 3 + 2];
            maxErr = (std::max)(maxErr,
                                std::fabs(std::sqrt(x * x + y * y + z * z) - 10.0));
        }
        check(rc == 0 && m.m.face_count > 0 && maxErr < 1.5,
              "Poisson 球面网格（顶点半径误差 <1.5）");
    }
    // 4 greedyProjection → 顶点取自原始点（严格在球面上）
    {
        MeshGuard m;
        const double p[1] = { 3.0 };
        const int rc = Point3DAlg_ReconstructSurface(&sphere, 4, p, 1, &m.m);
        double maxErr = 0.0;
        for (int i = 0; m.m.vertices && i < m.m.vertex_count; ++i) {
            const double x = m.m.vertices[i * 3], y = m.m.vertices[i * 3 + 1],
                         z = m.m.vertices[i * 3 + 2];
            maxErr = (std::max)(maxErr,
                                std::fabs(std::sqrt(x * x + y * y + z * z) - 10.0));
        }
        check(rc == 0 && m.m.face_count > 0 && maxErr < 1e-3,
              "greedyProjection 三角面（顶点在球面上）");
    }
    // 5 / 6 marchingCubes（Hoppe / RBF）→ 顶点近球面
    {
        MeshGuard m;
        const double p[1] = { 24.0 };
        const int rc = Point3DAlg_ReconstructSurface(&sphere, 5, p, 1, &m.m);
        double maxErr = 0.0;
        for (int i = 0; m.m.vertices && i < m.m.vertex_count; ++i) {
            const double x = m.m.vertices[i * 3], y = m.m.vertices[i * 3 + 1],
                         z = m.m.vertices[i * 3 + 2];
            maxErr = (std::max)(maxErr,
                                std::fabs(std::sqrt(x * x + y * y + z * z) - 10.0));
        }
        check(rc == 0 && m.m.vertex_count > 0 && maxErr < 1.5,
              "marchingCubes 球面（顶点半径误差 <1.5）");
        const double p6[2] = { 24.0, 0.1 };
        MeshGuard m6;
        const int rc6 = Point3DAlg_ReconstructSurface(&sphere, 6, p6, 2, &m6.m);
        check(rc6 == 0 && m6.m.face_count > 0, "marchingCubesRBF 球面网格");
    }
    // 7 gridProjection：稠密体点云（R=2，步距 0.5）→ 面>0
    {
        std::vector<float> v;
        for (int x = -4; x <= 4; ++x)
            for (int y = -4; y <= 4; ++y)
                for (int z = -4; z <= 4; ++z)
                    if (x * x + y * y + z * z <= 16)
                        PushPt(v, x * 0.5f, y * 0.5f, z * 0.5f);
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        MeshGuard m;
        const double p[2] = { 0.1, 3.0 };
        const int rc = Point3DAlg_ReconstructSurface(&in, 7, p, 2, &m.m);
        check(rc == 0 && m.m.face_count > 0, "gridProjection 稠密体 → 面>0");
        Point3DAlg_FreeCloud(&in);
    }
    // 8 organizedFastMesh：SoA 恒无序点云 → 恒 -2（口径见 api.h）
    {
        MeshGuard m;
        const int rc = Point3DAlg_ReconstructSurface(&sphere, 8, nullptr, 0, &m.m);
        check(rc == -2, "organizedFastMesh（无序点云）→ -2");
    }
    Point3DAlg_FreeCloud(&sphere);
}

// ---------------- A13. HeightMap 桥接 ----------------
static void TestHeightMap()
{
    std::printf("---- A13. CloudToHeightMap / HeightMapToCloud / ThicknessMap ----\n");
    struct HmGuard {
        Point3DAlg_HeightMap h{ 0, 0, 0, 0, 0, nullptr, nullptr, 0 };
        ~HmGuard() { Point3DAlg_FreeHeightMap(&h); }
    };

    // 错误路径
    {
        Point3DAlg_PointCloud in = MakeCloud();
        HmGuard hm;
        check(Point3DAlg_CloudToHeightMap(&in, 0.0, 0, &hm.h) == -3, "cell=0 → -3");
        check(Point3DAlg_CloudToHeightMap(&in, 1.0, 4, &hm.h) == -3, "mode=4 → -3");
        check(Point3DAlg_CloudToHeightMap(nullptr, 1.0, 0, &hm.h) == -1, "in=NULL → -1");
        check(Point3DAlg_HeightMapToCloud(nullptr, nullptr) == -1, "hm=NULL → -1");
        hm.h.width = 5;
        hm.h.height = 5;
        hm.h.cells = (float*)malloc(25 * sizeof(float));
        hm.h.mask = (unsigned char*)malloc(25);
        hm.h.valid = 0;
        Point3DAlg_PointCloud back;
        check(Point3DAlg_HeightMapToCloud(&hm.h, &back) == -2, "valid=0 → -2");
        Point3DAlg_FreeCloud(&back);
        Point3DAlg_FreeHeightMap(&hm.h);
        Point3DAlg_FreeCloud(&in);
        Point3DAlg_FreeHeightMap(nullptr);   // NULL 安全
        check(true, "FreeHeightMap(NULL) 安全");
    }

    // 5×5 网格 z=i+j（双层 100+i+j）→ mode 0/1/2/3 四种聚合
    {
        std::vector<float> v;
        FillGrid5(v, true);   // 50 点
        Point3DAlg_PointCloud in = MakeCloudFrom(v);
        HmGuard hm;
        const int rc0 = Point3DAlg_CloudToHeightMap(&in, 1.0, 0, &hm.h);
        bool ok0 = rc0 == 0 && hm.h.valid == 1 && hm.h.width == 5 && hm.h.height == 5 &&
                   std::fabs(hm.h.x0) < 1e-9 && std::fabs(hm.h.y0) < 1e-9 &&
                   std::fabs(hm.h.cell - 1.0) < 1e-9;
        for (int i = 0; ok0 && i < 25; ++i)
            ok0 = hm.h.mask[i] == 1 &&
                  std::fabs(hm.h.cells[i] - (100.0f + (float)(i / 5) + (float)(i % 5))) < 1e-6;
        check(ok0, "mode=0 每格最高 z（100+i+j）");
        HmGuard hm1;
        const int rc1 = Point3DAlg_CloudToHeightMap(&in, 1.0, 1, &hm1.h);
        check(rc1 == 0 && std::fabs(hm1.h.cells[24] - 8.0f) < 1e-6,
              "mode=1 每格最低 z（i+j）");
        HmGuard hm2;
        const int rc2 = Point3DAlg_CloudToHeightMap(&in, 1.0, 2, &hm2.h);
        check(rc2 == 0 && std::fabs(hm2.h.cells[0] - 50.0f) < 1e-6,
              "mode=2 每格平均 z（50）");
        HmGuard hm3;
        const int rc3 = Point3DAlg_CloudToHeightMap(&in, 1.0, 3, &hm3.h);
        check(rc3 == 0 && std::fabs(hm3.h.cells[12] - 2.0f) < 1e-6,
              "mode=3 点计数密度（每格 2）");
        // 反向点云化：25 有效格 → 25 点（格心坐标 x/y∈{0.5..4.5}，z=i+j 还原）
        Point3DAlg_PointCloud back;
        const int rcB = Point3DAlg_HeightMapToCloud(&hm1.h, &back);
        bool okB = rcB == 0 && back.count == 25;
        for (int i = 0; okB && i < back.count; ++i) {
            const double x = back.xyz[i * 3], y = back.xyz[i * 3 + 1], z = back.xyz[i * 3 + 2];
            const int cx = (int)std::floor(x), cy = (int)std::floor(y);
            okB = std::fabs(x - (cx + 0.5)) < 1e-6 && std::fabs(y - (cy + 0.5)) < 1e-6 &&
                  std::fabs(z - (double)(cx + cy)) < 1e-6;
        }
        check(okB, "HeightMapToCloud 25 格心点（z=i+j 还原）");
        Point3DAlg_FreeCloud(&back);
        Point3DAlg_FreeCloud(&in);
    }

    // 厚度图：上表面 z=5 / 下表面 z=1 → 每格 4
    {
        std::vector<float> vt, vb;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j) {
                PushPt(vt, (float)i, (float)j, 5.0f);
                PushPt(vb, (float)i, (float)j, 1.0f);
            }
        Point3DAlg_PointCloud top = MakeCloudFrom(vt);
        Point3DAlg_PointCloud bot = MakeCloudFrom(vb);
        HmGuard hm;
        const int rc = Point3DAlg_ThicknessMap(&top, &bot, 1.0, &hm.h);
        bool ok = rc == 0 && hm.h.valid == 1 && hm.h.width == 5 && hm.h.height == 5;
        for (int i = 0; ok && i < 25; ++i)
            ok = hm.h.mask[i] == 1 && std::fabs(hm.h.cells[i] - 4.0f) < 1e-6;
        check(ok, "ThicknessMap 已知厚度 4（25 格全有效）");
        check(Point3DAlg_ThicknessMap(&top, &bot, 0.0, &hm.h) == -3, "厚度 cell=0 → -3");
        Point3DAlg_FreeCloud(&top);
        Point3DAlg_FreeCloud(&bot);
    }
}

// 点云计算族测试总入口
static void TestCloudOps()
{
    TestFilter();
    TestExtract();
    TestTransform();
    TestCloudIO();
    TestVoxelConsistency();
    TestFitModel();
    TestSegmentCloud();
    TestSearch();
    TestMeasureCloud();
    TestFeatureKeypoints();
    TestSurfaceMesh();
    TestHeightMap();
}
#endif /* POINT3DALG_TEST_HAS_PCL */

// =====================================================================
// B. SICK 深层接口（句柄族；有 .dat 数据才跑）
// =====================================================================

static void TestSickDeep(const std::vector<std::string>& folders, const Point3DAlg_SickConfig& cfg)
{
    std::printf("---- B1. SICK 深层接口（句柄族）----\n");
    // 路径列表 ';' 连接（多相机）
    std::string joined;
    for (size_t i = 0; i < folders.size(); ++i) {
        if (i) joined += ";";
        joined += folders[i];
    }

    check(Point3DAlg_CreateSickReader(nullptr) == nullptr, "CreateSickReader(NULL) → NULL");
    check(Point3DAlg_LoadRawInto(nullptr, "x", -1, nullptr, nullptr) == -1, "LoadRawInto(NULL 句柄) → -1");

    Point3DAlg_SickReaderH h = Point3DAlg_CreateSickReader(&cfg);
    if (!h) {
        check(false, "CreateSickReader 创建句柄");
        return;
    }
    check(true, "CreateSickReader 创建句柄");

    {
        int w = 0;
        check(Point3DAlg_GetSickInfo(h, &w, nullptr, nullptr, nullptr, nullptr, nullptr) == -2,
              "未加载时 GetSickInfo → -2");
        check(Point3DAlg_LoadRawInto(h, "no_such_dir_xyz", -1, nullptr, nullptr) == -2,
              "LoadRawInto 无 .dat → -2");
        check(Point3DAlg_LoadRawInto(h, joined.c_str(), 999999, nullptr, nullptr) == -3,
              "LoadRawInto 帧号越界 → -3");
    }

    const int rcLoad = Point3DAlg_LoadRawInto(h, joined.c_str(), kFrame, nullptr, nullptr);
    check(rcLoad == 0, "LoadRawInto 加载成功");
    if (rcLoad != 0) {
        Point3DAlg_FreeSickReader(h);
        return;
    }

    // 信息查询
    int w = 0, dev = 0, hasInt = 0;
    long long rows = 0, sRows = 0, sCols = 0;
    check(Point3DAlg_GetSickInfo(h, &w, &rows, &sRows, &sCols, &dev, &hasInt) == 0,
          "GetSickInfo 成功");
    check(w == 2560, "Width == 2560");
    check(rows > 0, "Rows > 0");
    check(dev == (int)folders.size(), "DeviceCount == 文件夹数");
    check(sRows == 0 && sCols == 0, "GetDepth 前 Stitch* == 0（不触发拼接）");

    // 行中心 Y
    {
        double* rowY = nullptr;
        int nRowY = 0;
        const int rc = Point3DAlg_GetSickRowY(h, &rowY, &nRowY);
        bool ok = rc == 0 && nRowY > 0 && rowY != nullptr;
        for (int i = 0; ok && i < nRowY; ++i)
            if (!(rowY[i] > -1e9 && rowY[i] < 1e9))   // 有限值粗判
                ok = false;
        check(ok, "GetSickRowY 有限值");
        Point3DAlg_FreeBuffer(rowY);
    }

    // 原始图 + 编码器
    {
        unsigned short* raw = nullptr;
        int rw = 0;
        long long rh = 0;
        int* enc = nullptr;
        const int rc = Point3DAlg_GetSickRaw(h, &raw, &rw, &rh, &enc);
        check(rc == 0 && raw && enc && rw == w && rh == rows, "GetSickRaw 布局 = Rows×Width");
        Point3DAlg_FreeBuffer(raw);
        Point3DAlg_FreeBuffer(enc);
    }

    // 强度图 + 逐点强度（无强度格式 → -2，两种结果都合法）
    {
        unsigned char* img = nullptr;
        int iw = 0;
        long long ih = 0;
        unsigned char* ci = nullptr;
        int pc = 0;
        const int rc = Point3DAlg_GetSickIntensity(h, &img, &iw, &ih, &ci, &pc);
        if (hasInt) {
            check(rc == 0 && img && iw == (int)sCols && ih == sRows && ci && pc > 0,
                  "GetSickIntensity（有强度）布局 = StitchRows×StitchCols");
        } else {
            check(rc == -2, "GetSickIntensity（无强度 → -2）");
        }
        Point3DAlg_FreeBuffer(img);
        Point3DAlg_FreeBuffer(ci);
    }

    // FOV 框
    {
        int* rects = nullptr;
        int* ids = nullptr;
        int nFov = 0;
        const int rc = Point3DAlg_GetFovBoxes(h, &rects, &ids, &nFov);
        bool ok = rc == 0 && nFov > 0 && rects && ids;
        for (int i = 0; ok && i < nFov; ++i)
            if (rects[i * 4 + 2] <= 0 || rects[i * 4 + 3] <= 0)   // w/h 必为正
                ok = false;
        check(ok, "GetFovBoxes 框数与尺寸");
        Point3DAlg_FreeBuffer(rects);
        Point3DAlg_FreeBuffer(ids);
    }

    // 拼接质量（单相机无衔接带 → -2 属预期）
    {
        Point3DAlg_StitchQualityResult q;
        const int rc = Point3DAlg_StitchQuality(h, 0.02, &q);
        if (dev >= 2)
            check(rc == 0 && q.pairs > 0, "StitchQuality（多相机）配对数 > 0");
        else
            check(rc == -2, "StitchQuality（单相机 → -2）");
        check(Point3DAlg_StitchQuality(h, 0.0, &q) == -3, "threshold<=0 → -3");
    }

    Point3DAlg_FreeSickReader(nullptr);   // NULL 安全
    Point3DAlg_FreeSickReader(h);
}

// =====================================================================
// main
// =====================================================================

int main(int argc, char* argv[])
{
    // ---- A. 通用 C API 测试（合成数据，恒跑） ----
    TestLifecycle();
#if POINT3DALG_TEST_HAS_PCL
    TestCloudOps();
#else
    std::printf("SKIP: 无 PCL 构建，点云计算族测试段（Load/Save/Filter/Extract/Transform）不编译\n");
#endif

    // ---- B. SICK 端到端 + 深层接口（有 .dat 才跑，否则 SKIP） ----
    std::vector<std::string> folders;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i)
            folders.push_back(argv[i]);
    } else {
        folders.assign(kFolders, kFolders + kDeviceCount);
    }

    // 数据目录不存在或没有任何 .dat → 打印 SKIP（既有约定），
    // A 部分结果仍有效（A 失败时返回 1）
    for (const std::string& f : folders) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((f + "/*.dat").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("SKIP: 扫描数据目录不存在或无 .dat 文件: %s\n", f.c_str());
            std::printf("      （用法: Point3DAlg_test.exe <相机1目录> [相机2目录 ...]，缺省 %s）\n",
                        kFolders[0]);
            if (g_fail) {
                std::printf("%d 个用例失败\n", g_fail);
                return 1;
            }
            std::printf("全部测试通过（SICK 部分跳过）\n");
            return 0;
        }
        FindClose(h);
    }

    // ---- 1. 读 json 配置（提取不到必需值报错退出） ----
    Point3DAlg_SickConfig cfg;
    char err[512] = { 0 };
    const int rc = Point3DAlg_LoadSickConfig(nullptr, nullptr, &cfg, err, sizeof(err));
    if (rc != 0) {
        std::printf("配置读取失败(错误码 %d): %s\n", rc, err);
        return 1;
    }
    std::printf("配置读取成功: mm_pulse=%g x_scale=%g y_scale=%g z_scale=%g z_offset=%g comp(%g,%g)+%d阶\n",
                cfg.mm_pulse, cfg.x_scale, cfg.y_scale, cfg.z_scale, cfg.z_offset,
                cfg.comp_fit0, cfg.comp_fit1, cfg.comp_count);

    // ---- 2. B1：SICK 深层接口（句柄族） ----
    TestSickDeep(folders, cfg);

    // ---- 3. 既有端到端链路：创建对象 → 加载 → 深度图/点云 → 保存 ----
    try {
        Point3DAlg_SickReader reader(cfg);
        std::printf("---- B2. SICK 端到端（既有链路）----\n");
        std::printf("加载: %d 相机, frame=%d\n", (int)folders.size(), kFrame);
        for (const std::string& f : folders)
            std::printf("  %s\n", f.c_str());
        auto t0 = std::chrono::steady_clock::now();
        std::fflush(stdout);
        reader.LoadRaw(folders, kFrame);
        std::printf("  LoadRaw 完成\n"); std::fflush(stdout);
        auto t1 = std::chrono::steady_clock::now();
        std::printf("  LoadRaw: %.0f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count());
        std::printf("  原始图(相机0): %lld 行 × %d 列, 编码器首行 A=%d, 扫描行(pass)数=%d\n",
                    reader.Rows(), reader.Width(), reader.Encoder()[0],
                    (int)reader.RowY().size());
        std::fflush(stdout);

        // ---- 矫正后的拼接深度图 ----
        auto t2 = std::chrono::steady_clock::now();
        std::printf("  GetDepth 开始\n"); std::fflush(stdout);
        const float* depth = reader.GetDepth();
        std::printf("  GetDepth 返回\n"); std::fflush(stdout);
        auto t3 = std::chrono::steady_clock::now();
        std::printf("  GetDepth: %.0f ms\n", std::chrono::duration<double, std::milli>(t3 - t2).count());
        const long long sRows = reader.StitchRows();
        const long long sCols = reader.StitchCols();
        std::printf("  拼接深度图: %lld 行 × %lld 列\n", sRows, sCols);
        float zmin = 1e30f, zmax = -1e30f;
        for (long long i = 0; i < sRows * sCols; ++i) {
            if (depth[i] == 0.0f) continue;
            if (depth[i] < zmin) zmin = depth[i];
            if (depth[i] > zmax) zmax = depth[i];
        }
        std::printf("  深度图: z [%.5f, %.5f] mm\n", zmin, zmax);

        // ---- 基于拼接深度图生成点云 ----
        auto t4 = std::chrono::steady_clock::now();
        int count = 0;
        const float* cloud = reader.GetCloud(&count);
        auto t5 = std::chrono::steady_clock::now();
        std::printf("  GetCloud: %.0f ms\n", std::chrono::duration<double, std::milli>(t5 - t4).count());
        float xmin = cloud[0], xmax = cloud[0];
        float ymin = cloud[1], ymax = cloud[1];
        for (int i = 1; i < count; ++i) {
            const float x = cloud[i * 3], y = cloud[i * 3 + 1];
            if (x < xmin) xmin = x; if (x > xmax) xmax = x;
            if (y < ymin) ymin = y; if (y > ymax) ymax = y;
        }
        std::printf("  点云: %d 点, x [%.4f, %.4f]  y [%.4f, %.4f]  z [%.5f, %.5f] (mm)\n",
                    count, xmin, xmax, ymin, ymax, zmin, zmax);

        // ---- 保存点云(PLY) / 深度图(16位 + 可视8位 + FOV标注) ----
        const int rc1 = Point3DAlg_SaveCloud(kOutCloud, cloud, count, 1);
        std::printf("  保存点云 %s: %s\n", kOutCloud, rc1 == 0 ? "成功" : "失败");
        const int rc2 = Point3DAlg_SaveDepth(kOutDepth, depth,
                                            (int)sRows, (int)sCols, 0, 0);
        std::printf("  保存深度图 %s: %s\n", kOutDepth, rc2 == 0 ? "成功" : "失败");
        const int rc3 = Point3DAlg_SaveDepthView(kOutDepthView, depth,
                                                (int)sRows, (int)sCols, 0, 0);
        std::printf("  保存可视深度图 %s: %s\n", kOutDepthView, rc3 == 0 ? "成功" : "失败");
        // FOV 标注图：框出每个 FOV 的覆盖范围 + 序号（相机:FOV）
        std::vector<int> fovRects, fovIds;
        const int nFov = reader.FovBoxes(fovRects, fovIds);
        std::printf("  FOV 数量: %d\n", nFov);
        const int rc4 = Point3DAlg_DrawFovBoxes(kOutDepthFov, depth,
                                               (int)sRows, (int)sCols, 0, 0,
                                               fovRects.data(), fovIds.data(), nFov);
        std::printf("  保存FOV标注图 %s: %s\n", kOutDepthFov, rc4 == 0 ? "成功" : "失败");
        if (rc1 != 0 || rc2 != 0 || rc3 != 0 || rc4 != 0) {
            std::printf("%d 个用例失败\n", g_fail);
            return 2;
        }
    } catch (const std::exception& e) {
        std::printf("失败: %s\n", e.what());
        return 3;
    }

    if (g_fail) {
        std::printf("%d 个用例失败\n", g_fail);
        return 1;
    }
    std::printf("全部测试通过\n");
    return 0;
}
