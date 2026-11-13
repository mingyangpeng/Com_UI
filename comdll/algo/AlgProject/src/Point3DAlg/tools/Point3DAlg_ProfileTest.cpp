// Point3DAlg_ProfileTest.cpp —— 轮廓/截面量测测试（合成数据，注册 ctest）。
// 用例：水平面 / 台阶剖面（max_step 定位）/ 斜面线性 / 带外剔除 / 边界。
// 退出码：0=通过 1=失败。

#include "Point3DAlg_Profile.h"

#include <cmath>
#include <functional>
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
    std::printf("== 轮廓/截面量测测试 ==\n");
    std::mt19937 rng(20261111);
    const double kSigma = 0.003;
    Point3DAlg_ProfileParams params;   // 默认 band=0.2 bin=0.1 min=2

    // ---- 用例 1：水平面 z=1.0，线沿 +X ----
    {
        auto cloud = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05,
                              [](double, double) { return 1.0; }, rng, kSigma);
        Point3DAlg_ProfileResult r;
        std::vector<float> prof;
        const bool ok = Point3DAlg_Profile(cloud, 1.0, 2.0, 3.0, 2.0, params, r, &prof);
        check(ok, "水平面剖面成功");
        if (ok) {
            std::printf("  pts=%lld len=%.2f z=[%.4f,%.4f] max_step=%.5f\n",
                        r.points, r.length, r.z_min, r.z_max, r.max_step);
            // 20 或 21 格：线段端点 s=L 恰落在下一格下边界（量化效应，端点列单成格）
            check(r.points == 20 || r.points == 21, "格数 ≈ 2mm/0.1mm（±1 端点格）");
            check(std::fabs(r.z_min - 1.0) < 0.003 && std::fabs(r.z_max - 1.0) < 0.003,
                  "z 恒为 1.0 ± 3µm（格中位噪声）");
            check(r.max_step < 0.006, "无台阶：max_step ≈ √2×3µm 噪声");
            check((long long)prof.size() == r.points * 2, "剖面 SoA 长度 = 2×格数");
        }
    }

    // ---- 用例 2：台阶（y<2 → 1.0；y≥2 → 1.5），线沿 +Y 穿过台阶 ----
    {
        auto cloud = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05,
                              [](double, double y) { return y < 2.0 ? 1.0 : 1.5; },
                              rng, kSigma);
        Point3DAlg_ProfileResult r;
        std::vector<float> prof;
        const bool ok = Point3DAlg_Profile(cloud, 2.0, 0.0, 2.0, 4.0, params, r, &prof);
        check(ok, "台阶剖面成功");
        if (ok) {
            std::printf("  z=[%.4f,%.4f] max_step=%.5f @s=%.3f\n",
                        r.z_min, r.z_max, r.max_step, r.s_at_step);
            check(std::fabs(r.z_min - 1.0) < 0.003 && std::fabs(r.z_max - 1.5) < 0.003,
                  "z 范围 = 两台阶高度");
            check(std::fabs(r.max_step - 0.5) < 0.01, "台阶落差 = 0.5 ± 10µm");
            check(std::fabs(r.s_at_step - 2.0) < 0.11, "跳变位置 ≈ 台阶线 s=2.0");
            // 剖面两段高度抽查
            bool loOK = false, hiOK = false;
            for (size_t i = 0; i + 1 < prof.size(); i += 2) {
                if (prof[i] < 1.8 && std::fabs(prof[i + 1] - 1.0) < 0.005) loOK = true;
                if (prof[i] > 2.2 && std::fabs(prof[i + 1] - 1.5) < 0.005) hiOK = true;
            }
            check(loOK && hiOK, "两段高度独立正确");
        }
    }

    // ---- 用例 3：斜面线性剖面 ----
    {
        auto cloud = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05,
                              [](double, double y) { return 0.1 * y + 1.0; },
                              rng, kSigma);
        Point3DAlg_ProfileResult r;
        const bool ok = Point3DAlg_Profile(cloud, 2.0, 0.0, 2.0, 4.0, params, r);
        check(ok, "斜面剖面成功");
        if (ok) {
            std::printf("  z=[%.4f,%.4f] @s=[%.2f,%.2f]\n",
                        r.z_min, r.z_max, r.s_at_min, r.s_at_max);
            // 首末格中心 s=0.05/4.05（端点列单格），真值 0.1×s+1
            check(r.z_min > 1.002 && r.z_min < 1.008, "斜面起点高度 ≈ 1.005（格中心）");
            check(r.z_max > 1.395 && r.z_max < 1.410, "斜面终点高度 ≈ 1.40~1.405");
            check(r.s_at_min < 0.15 && r.s_at_max > 3.85, "极值在两端");
        }
    }

    // ---- 用例 4：带外剔除 ----
    {
        auto cloud = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05,
                              [](double, double y) { return y < 2.0 ? 1.0 : 1.5; },
                              rng, kSigma);
        // 带外剔除的正确口径 = used_points（格数由弧长 bin 决定，与 band 无关）：
        // y 网格步距 0.05，band=0.02 只进 1 行（~205 点），band=0.2 进 9 行（~1845 点）
        Point3DAlg_ProfileParams narrow = params;
        narrow.band = 0.02;
        Point3DAlg_ProfileResult rn;
        check(Point3DAlg_Profile(cloud, 2.0, 0.0, 2.0, 4.0, narrow, rn), "窄带剖面成功");
        Point3DAlg_ProfileResult rw;
        check(Point3DAlg_Profile(cloud, 2.0, 0.0, 2.0, 4.0, params, rw), "宽带剖面成功");
        std::printf("  used: nb=%lld wb=%lld  pts: nb=%lld wb=%lld\n",
                    rn.used_points, rw.used_points, rn.points, rw.points);
        check(rn.used_points < rw.used_points / 3, "band 收窄 → used_points 锐减（带外剔除生效）");
        // 格数由弧长 bin 决定（±1：端点格在窄带下可能只有 1 点被 min_points 滤掉）
        check(rn.points >= 40 && rw.points >= 40 && (rw.points - rn.points) <= 1,
              "格数 ≈ 4mm/0.1mm（端点格 ±1 量化）");
    }

    // ---- 用例 5：边界 ----
    {
        auto cloud = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05,
                              [](double, double) { return 1.0; }, rng, kSigma);
        Point3DAlg_ProfileResult r;
        std::vector<float> prof;
        check(!Point3DAlg_Profile(nullptr, 0, 0, 1, 0, params, r, &prof), "空点云 → false");
        check(!Point3DAlg_Profile(cloud, 1.0, 1.0, 1.0, 1.0, params, r, &prof), "零长度线 → false");
        Point3DAlg_ProfileParams bad = params;
        bad.band = 0.0;
        check(!Point3DAlg_Profile(cloud, 1.0, 2.0, 3.0, 2.0, bad, r, &prof), "band=0 → false");
        check(!Point3DAlg_Profile(cloud, 10.0, 20.0, 13.0, 20.0, params, r, &prof),
              "线段在云外 → false");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
