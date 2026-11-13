// Point3DAlg_CircleFitTest.cpp —— 高度切片圆/孔位量测测试（合成数据，注册 ctest）。
// 用例：理想整圆 / 圆柱壁切片检出 / 多孔 / 部分弧 / 边界。退出码：0=通过 1=失败。

#include "Point3DAlg_CircleFit.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using CloudXYZ = pcl::PointCloud<pcl::PointXYZ>;

static int g_fail = 0;

static void check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_fail;
}

// 圆柱壁点：角度均匀 + 高度层，z ∈ [zLow, zHigh]
static void AddCylWall(CloudXYZ::Ptr c, double cx, double cy, double R,
                       double zLow, double zHigh, double zStep,
                       int ptsPerLayer, std::mt19937& rng, double sigmaXY)
{
    std::normal_distribution<double> noise(0.0, sigmaXY);
    for (double z = zLow; z <= zHigh + 1e-9; z += zStep) {
        for (int i = 0; i < ptsPerLayer; ++i) {
            const double th = 2.0 * 3.14159265358979323846 * i / ptsPerLayer;
            c->push_back(pcl::PointXYZ((float)(cx + R * std::cos(th) + noise(rng)),
                                       (float)(cy + R * std::sin(th) + noise(rng)),
                                       (float)z));
        }
    }
}

int main()
{
    std::printf("== 高度切片圆/孔位量测测试 ==\n");
    std::mt19937 rng(20261111);
    Point3DAlg_CircleFitParams params;   // 默认参数

    // ---- 用例 1：理想整圆（FitCircle2D 核心）----
    {
        const int N = 72;
        std::vector<double> xy((size_t)N * 2);
        std::normal_distribution<double> noise(0.0, 0.005);
        for (int i = 0; i < N; ++i) {
            const double th = 2.0 * 3.14159265358979323846 * i / N;
            xy[(size_t)i * 2] = 2.0 + 0.5 * std::cos(th) + noise(rng);
            xy[(size_t)i * 2 + 1] = 2.0 + 0.5 * std::sin(th) + noise(rng);
        }
        Point3DAlg_CircleFitResult r;
        const bool ok = Point3DAlg_FitCircle2D(xy.data(), N, 3.0, 3, 0.05, 50.0, 0.02, r);
        check(ok, "整圆拟合成功");
        if (ok) {
            std::printf("  c=(%.5f,%.5f) R=%.5f rms=%.6f n=%d\n",
                        r.center[0], r.center[1], r.radius, r.rms, r.point_count);
            check(std::fabs(r.center[0] - 2.0) < 0.002 && std::fabs(r.center[1] - 2.0) < 0.002,
                  "圆心误差 < 2µm");
            check(std::fabs(r.radius - 0.5) < 0.002, "半径误差 < 2µm");
            check(r.rms < 0.008, "残差 ≈ 噪声 5µm 水平");
        }
    }

    // ---- 用例 2：点云切片检出圆柱壁（孔）----
    {
        CloudXYZ::Ptr cloud(new CloudXYZ);
        std::normal_distribution<double> noise(0.0, 0.002);
        // 基面 z=1.0（σ=2µm，z 带 [1.05,1.15] 外被切掉）
        for (double x = 0.0; x <= 4.0; x += 0.05)
            for (double y = 0.0; y <= 4.0; y += 0.05)
                cloud->push_back(pcl::PointXYZ((float)x, (float)y, (float)(1.0 + noise(rng))));
        // 孔壁：R=0.3 @ (2,2)，z ∈ [1.0, 1.2]
        AddCylWall(cloud, 2.0, 2.0, 0.3, 1.0, 1.2, 0.02, 48, rng, 0.005);

        std::vector<Point3DAlg_CircleFitResult> circles;
        const int n = Point3DAlg_DetectCircles(cloud, 1.05, 1.15, params, circles);
        check(n >= 1, "切片检出簇");
        bool found = false;
        for (const auto& r : circles) {
            if (r.valid && std::fabs(r.center[0] - 2.0) < 0.01 &&
                std::fabs(r.center[1] - 2.0) < 0.01)
                found = true;
        }
        check(found, "检出孔位圆心 (2,2)");
        for (const auto& r : circles) {
            if (!r.valid) continue;
            std::printf("  c=(%.4f,%.4f) R=%.4f rms=%.5f n=%d mean_z=%.3f\n",
                        r.center[0], r.center[1], r.radius, r.rms, r.point_count, r.mean_z);
            check(std::fabs(r.radius - 0.3) < 0.005, "孔半径 = 0.3 ± 5µm");
        }
    }

    // ---- 用例 3：三孔（不同位置/半径）----
    {
        CloudXYZ::Ptr cloud(new CloudXYZ);
        // 只放壁点（切片链路已在用例 2 验证，这里聚焦多目标分离）
        AddCylWall(cloud, 1.0, 1.0, 0.20, 1.0, 1.2, 0.04, 48, rng, 0.005);
        AddCylWall(cloud, 3.0, 1.0, 0.30, 1.0, 1.2, 0.04, 48, rng, 0.005);
        AddCylWall(cloud, 2.0, 3.0, 0.40, 1.0, 1.2, 0.04, 48, rng, 0.005);
        std::vector<Point3DAlg_CircleFitResult> circles;
        const int n = Point3DAlg_DetectCircles(cloud, 1.05, 1.15, params, circles);
        check(n == 3, "三孔全检出");
        int nValid = 0;
        const double truth[3][3] = { { 1.0, 1.0, 0.20 }, { 3.0, 1.0, 0.30 }, { 2.0, 3.0, 0.40 } };
        for (const auto& r : circles) {
            if (!r.valid) continue;
            ++nValid;
            bool matched = false;
            for (const auto& t : truth) {
                if (std::fabs(r.center[0] - t[0]) < 0.01 && std::fabs(r.center[1] - t[1]) < 0.01) {
                    check(std::fabs(r.radius - t[2]) < 0.005, "孔半径匹配真值");
                    matched = true;
                }
            }
            check(matched, "圆心匹配真值位置");
        }
        check(nValid == 3, "三孔全部 valid");
    }

    // ---- 用例 4：部分弧（240°）----
    {
        const int N = 60;
        std::vector<double> xy((size_t)N * 2);
        std::normal_distribution<double> noise(0.0, 0.005);
        for (int i = 0; i < N; ++i) {
            const double th = (240.0 / 180.0) * 3.14159265358979323846 * i / N;
            xy[(size_t)i * 2] = 1.0 + 0.4 * std::cos(th) + noise(rng);
            xy[(size_t)i * 2 + 1] = 1.0 + 0.4 * std::sin(th) + noise(rng);
        }
        Point3DAlg_CircleFitResult r;
        const bool ok = Point3DAlg_FitCircle2D(xy.data(), N, 3.0, 3, 0.05, 50.0, 0.02, r);
        check(ok, "240° 弧拟合成功");
        if (ok) {
            std::printf("  弧: c=(%.5f,%.5f) R=%.5f\n", r.center[0], r.center[1], r.radius);
            check(std::fabs(r.center[0] - 1.0) < 0.005 && std::fabs(r.center[1] - 1.0) < 0.005,
                  "弧圆心误差 < 5µm（部分弧半径略病态，圆心更稳）");
            check(std::fabs(r.radius - 0.4) < 0.01, "弧半径误差 < 10µm");
        }
    }

    // ---- 用例 5：边界 ----
    {
        CloudXYZ::Ptr cloud(new CloudXYZ);
        std::normal_distribution<double> noise(0.0, 0.002);
        for (double x = 0.0; x <= 2.0; x += 0.05)
            for (double y = 0.0; y <= 2.0; y += 0.05)
                cloud->push_back(pcl::PointXYZ((float)x, (float)y, (float)(1.0 + noise(rng))));
        std::vector<Point3DAlg_CircleFitResult> circles;
        check(Point3DAlg_DetectCircles(nullptr, 1.0, 1.1, params, circles) == -1, "空点云 → -1");
        check(Point3DAlg_DetectCircles(cloud, 5.0, 1.0, params, circles) == -1, "z_low>z_high → -1");
        check(Point3DAlg_DetectCircles(cloud, 1.05, 1.15, params, circles) == 0, "切片无点 → 0");

        Point3DAlg_CircleFitResult r;
        const double two[4] = { 0.0, 0.0, 1.0, 0.0 };
        check(!Point3DAlg_FitCircle2D(two, 2, 3.0, 3, 0.05, 50.0, 0.02, r), "点数 < 3 → false");
        // 半径越界 → invalid
        const int N = 72;
        std::vector<double> xy((size_t)N * 2);
        for (int i = 0; i < N; ++i) {
            const double th = 2.0 * 3.14159265358979323846 * i / N;
            xy[(size_t)i * 2] = 2.0 + 0.5 * std::cos(th);
            xy[(size_t)i * 2 + 1] = 2.0 + 0.5 * std::sin(th);
        }
        check(!Point3DAlg_FitCircle2D(xy.data(), N, 3.0, 3, 0.6, 50.0, 0.02, r),
              "半径 < radius_min → false（R≈0.5 < 0.6）");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
