// Point3DAlg_RegionPlaneTest.cpp —— ROI 平面量测测试（合成数据，注册 ctest）。
// 用例：水平面高度/平面度、倾斜面系数、台阶高度差、跨台阶稳健性、边界（空 ROI/点不足）。
// 退出码：0=通过 1=失败。

#include "Point3DAlg_RegionPlane.h"

#include <cmath>
#include <cstdio>
#include <functional>
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

// 网格点云：z = f(x,y) + 噪声
static CloudXYZ::Ptr MakeGrid(double x0, double x1, double y0, double y1, double step,
                              const std::function<double(double, double)>& zfun,
                              std::mt19937& rng, double sigma)
{
    CloudXYZ::Ptr c(new CloudXYZ);
    std::normal_distribution<double> noise(0.0, sigma);
    for (double x = x0; x <= x1 + 1e-9; x += step)
        for (double y = y0; y <= y1 + 1e-9; y += step)
            c->push_back(pcl::PointXYZ((float)x, (float)y, (float)(zfun(x, y) + noise(rng))));
    c->width = (uint32_t)c->size();
    c->height = 1;
    c->is_dense = true;
    return c;
}

int main()
{
    std::printf("== ROI 平面量测测试 ==\n");
    std::mt19937 rng(20261110);
    Point3DAlg_RegionPlaneParams params;   // 默认参数

    // ---- 用例 1：水平面 z=1.0 + 噪声 σ=2µm → 高度/PV/σ ----
    {
        auto cloud = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05,
                              [](double, double) { return 1.0; }, rng, 0.002);
        Point3DAlg_RegionPlaneResult r;
        const bool ok = Point3DAlg_RegionPlaneFit(cloud, 1.0, 1.0, 2.0, 2.0, params, r);
        check(ok, "水平面拟合成功");
        if (ok) {
            std::printf("  height=%.5f pv=%.5f sigma=%.5f in=%d/%d\n",
                        r.height, r.pv, r.sigma, r.fit_count, r.roi_count);
            check(std::fabs(r.height - 1.0) < 0.001, "高度误差 < 1µm");
            // 系数容差 3e-4：ROI 内斜率信号 6µm vs 噪声 2µm，系数标准误 ≈ 9e-5（统计极限）
            check(std::fabs(r.a) < 3e-4 && std::fabs(r.b) < 3e-4, "水平面倾斜≈0（±3e-4）");
            check(r.pv > 0.004 && r.pv < 0.012, "平面度 PV ≈ 4~6σ（实测噪声带）");
            check(r.sigma > 0.0015 && r.sigma < 0.0025, "σ ≈ 噪声水平 2µm");
            check(r.fit_count >= r.roi_count * 9 / 10, "内点占绝对多数");
        }
    }

    // ---- 用例 2：倾斜面 z = 0.003x − 0.002y + 1 → 系数验证 ----
    {
        auto cloud = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05,
                              [](double x, double y) { return 0.003 * x - 0.002 * y + 1.0; },
                              rng, 0.002);
        Point3DAlg_RegionPlaneResult r;
        const bool ok = Point3DAlg_RegionPlaneFit(cloud, 1.0, 1.0, 2.0, 2.0, params, r);
        check(ok, "倾斜面拟合成功");
        if (ok) {
            std::printf("  a=%.6f b=%.6f height=%.5f\n", r.a, r.b, r.height);
            check(std::fabs(r.a - 0.003) < 3e-4, "a 系数 = 0.003（±3e-4 统计极限）");
            check(std::fabs(r.b + 0.002) < 3e-4, "b 系数 = −0.002（±3e-4 统计极限）");
            // ROI 中心 (2,2)：height = 0.003*2 − 0.002*2 + 1 = 1.002
            check(std::fabs(r.height - 1.002) < 0.001, "中心高度 = 1.002");
        }
    }

    // ---- 用例 3：台阶高度差（两 ROI 各拟合一次相减）----
    {
        auto cloud = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05,
                              [](double, double y) { return y < 2.0 ? 1.0 : 1.5; },
                              rng, 0.002);
        Point3DAlg_RegionPlaneResult ra, rb;
        const bool oka = Point3DAlg_RegionPlaneFit(cloud, 0.5, 0.5, 1.0, 1.0, params, ra);
        const bool okb = Point3DAlg_RegionPlaneFit(cloud, 2.5, 2.5, 1.0, 1.0, params, rb);
        check(oka && okb, "台阶两侧拟合成功");
        if (oka && okb) {
            const double step = rb.height - ra.height;
            std::printf("  step=%.5f（真值 0.5）\n", step);
            check(std::fabs(step - 0.5) < 0.001, "台阶高度差 = 0.5 ± 1µm");
        }
    }

    // ---- 用例 4：ROI 内混入台阶（20% minority 上层点 → 稳健剔除）----
    // 注：MAD 修剪的稳健边界 ≈30% 污染；40% 时首轮双峰使 MAD≈半间距，剔不动（实测）。
    // 设计域： minority ≤20%；更高比例的 ROI 应避开台阶。
    {
        // 主平面 z=1.0；ROI 右侧 20% 区域是 z=1.3 的台阶
        auto cloud = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05,
                              [](double x, double) { return x < 3.8 ? 1.0 : 1.3; },
                              rng, 0.002);
        Point3DAlg_RegionPlaneResult r;
        const bool ok = Point3DAlg_RegionPlaneFit(cloud, 3.0, 1.0, 1.0, 2.0, params, r);
        check(ok, "混台阶 ROI 拟合成功");
        if (ok) {
            std::printf("  height=%.5f（真值 1.0）fit=%d roi=%d pv=%.5f\n",
                        r.height, r.fit_count, r.roi_count, r.pv);
            check(std::fabs(r.height - 1.0) < 0.001, "高度不被 minority 台阶带偏");
            check(r.fit_count < r.roi_count * 9 / 10, "台阶点被修剪（20% minority）");
        }
    }

    // ---- 用例 5：边界 ----
    {
        auto cloud = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05,
                              [](double, double) { return 1.0; }, rng, 0.002);
        Point3DAlg_RegionPlaneResult r;
        check(!Point3DAlg_RegionPlaneFit(nullptr, 0, 0, 1, 1, params, r), "空点云返回 false");
        check(!Point3DAlg_RegionPlaneFit(cloud, 10.0, 10.0, 1.0, 1.0, params, r), "ROI 无点返回 false");
        check(!Point3DAlg_RegionPlaneFit(cloud, 1.0, 1.0, 0.0, 1.0, params, r), "w=0 返回 false");
        // 极小 ROI（含 3~8 点）
        const bool ok = Point3DAlg_RegionPlaneFit(cloud, 1.0, 1.0, 0.12, 0.12, params, r);
        check(ok && r.roi_count >= 3, "小 ROI（含数点）可用");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
