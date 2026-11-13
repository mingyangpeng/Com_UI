// Point3DAlg_DomeFitTest.cpp —— 拱形球顶 bump 拟合测试（无 UI，控制台）。
// 两种模式：
//   1. 合成数据自检（默认，注册 ctest）：已知真值 → 验证基面/球心/半径/高度精度；
//   2. 真实数据（带路径参数）：D:/pmy/alg/output/moi_wafer.ply 等 PLY 文件，
//      输出统计供人工对照（Python 基准：2026 bumps，h p50≈0.187mm，σ≈2.0µm）。
// 用法：
//   Point3DAlg_DomeFitTest.exe                 —— 合成自检
//   Point3DAlg_DomeFitTest.exe <file.ply>      —— 真实数据
// 退出码：0=通过  1=有失败项  2=真实数据文件读取失败

#include "Point3DAlg_DomeFit.h"

#include "ShapelyAlg.h"   // CloudIO（读 PLY）

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using CloudXYZ = pcl::PointCloud<pcl::PointXYZ>;

static int g_fail = 0;

static void check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_fail;
}

// ---------------- 合成数据 ----------------
// 模拟 moi_wafer 形态：倾斜基面 + 噪声 + 若干拱形球冠 bump（真值已知）。
// bump 顶部为球冠：球心在基面下方 R−h 处，apex 高度 = h。
static void MakeSynthetic(CloudXYZ::Ptr& cloud,
                          std::vector<double>& truthH,
                          std::vector<double>& truthXY,
                          std::vector<double>& truthR,
                          unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, 0.003);   // σ=3µm（实测基面水平）

    cloud.reset(new CloudXYZ);
    const double tiltA = -0.0001, tiltB = 0.0002;         // 基面倾斜（弧度近似）
    const double step = 0.036;                            // 与实测网格间距一致
    const double x0 = 0.0, x1 = 6.0, y0 = 0.0, y1 = 6.0;  // 6×6mm ≈ 2.8 万点

    // 9 个 bump：3×3 网格，间距 1.5mm，h = 0.12~0.24mm、R = h/1.45
    // （h/R≈1.45 与实测形态一致：moi_wafer 上 h≈0.187mm、R≈0.13mm；
    //   必须 h < 2R 才能构成合法球冠——早期版本 h=0.24/R=0.10 生成 0 点）
    truthH.clear(); truthXY.clear(); truthR.clear();
    for (int bi = 0; bi < 3; ++bi)
        for (int bj = 0; bj < 3; ++bj) {
            const double h = 0.12 + 0.015 * (bi * 3 + bj);
            truthXY.push_back(1.5 + bi * 1.5);
            truthXY.push_back(1.5 + bj * 1.5);
            truthH.push_back(h);
            truthR.push_back(h / 1.45);
        }

    for (double x = x0; x <= x1 + 1e-9; x += step)
        for (double y = y0; y <= y1 + 1e-9; y += step) {
            double z = tiltA * x + tiltB * y + 0.5 + noise(rng);   // 基面 + 噪声
            // bump：球心 (cx, cy, apexZ − R)，apexZ = 基面 + h
            for (size_t b = 0; b < truthH.size(); ++b) {
                const double cx = truthXY[b * 2], cy = truthXY[b * 2 + 1];
                const double h = truthH[b], R = truthR[b];
                const double dx = x - cx, dy = y - cy;
                const double r2 = dx * dx + dy * dy;
                const double capR2 = 2.0 * R * h - h * h;   // 球冠底半径²（h < R）
                if (r2 < capR2 && capR2 > 0) {
                    const double baseZ = tiltA * cx + tiltB * cy + 0.5;   // bump 处基面
                    const double cz = baseZ + h - R;                       // 球心
                    const double dz2 = R * R - r2;
                    if (dz2 > 0) {
                        const double zb = cz + std::sqrt(dz2);             // 球面上表面
                        if (zb > z)
                            z = zb + noise(rng) * 0.5;   // bump 表面噪声减半
                    }
                }
            }
            cloud->push_back(pcl::PointXYZ((float)x, (float)y, (float)z));
        }
    cloud->width = (uint32_t)cloud->size();
    cloud->height = 1;
    cloud->is_dense = true;
}

// ---------------- 合成自检 ----------------

static int RunSynthetic()
{
    std::printf("== 拱形球顶拟合：合成自检 ==\n");
    CloudXYZ::Ptr cloud;
    std::vector<double> truthH, truthXY, truthR;
    MakeSynthetic(cloud, truthH, truthXY, truthR, 42u);
    std::printf("合成点云：%zu 点，%zu 个 bump（真值高度 0.12~0.24mm）\n",
                cloud->size(), truthH.size());

    Point3DAlg_DomeFitParams params;   // 默认参数
    Point3DAlg_DomeBase base;
    std::vector<Point3DAlg_DomeBump> bumps;
    const bool ok = Point3DAlg_DomeFit(cloud, params, bumps, &base);
    check(ok, "主流程成功");
    if (!ok) {
        std::printf("  合成数据拟合失败，中止\n");
        return 1;
    }

    check((int)bumps.size() == (int)truthH.size(), "bump 数量 = 真值数量");
    check(base.inlier_count > (int)(cloud->size() / 2), "基面内点占多数");
    check(base.sigma < 0.005, "基面噪声 σ < 5µm");

    // 每个 bump：高度/半径/位置误差（结果按点数降序，需按位置匹配真值）
    double maxHerr = 0, maxRerr = 0, maxPosErr = 0;
    int matched = 0;
    for (const auto& b : bumps) {
        if (!b.valid) continue;
        // 找最近真值
        int best = -1;
        double bestD2 = 1e9;
        for (size_t t = 0; t < truthH.size(); ++t) {
            const double dx = b.centroid[0] - truthXY[t * 2];
            const double dy = b.centroid[1] - truthXY[t * 2 + 1];
            const double d2 = dx * dx + dy * dy;
            if (d2 < bestD2) { bestD2 = d2; best = (int)t; }
        }
        if (best < 0 || bestD2 > 0.01) continue;
        ++matched;
        const double herr = std::fabs(b.height - truthH[best]);
        const double rerr = std::fabs(b.radius - truthR[best]);
        const double perr = std::sqrt(bestD2);
        if (herr > maxHerr) maxHerr = herr;
        if (rerr > maxRerr) maxRerr = rerr;
        if (perr > maxPosErr) maxPosErr = perr;
    }
    check(matched == (int)truthH.size(), "全部 bump 按位置匹配到真值");
    std::printf("  高度误差 max=%.2fµm  半径误差 max=%.2fµm  位置误差 max=%.4fmm  拟合 rms max=%.2fµm\n",
                maxHerr * 1000.0, maxRerr * 1000.0, maxPosErr,
                [&]{ double m = 0; for (const auto& b : bumps) if (b.valid && b.rms > m) m = b.rms; return m; }());
    check(maxHerr < 0.005, "高度误差 < 5µm（噪声 σ=3µm 水平）");
    check(maxRerr < 0.01, "半径误差 < 10µm");
    check(maxPosErr < 0.02, "位置误差 < 0.02mm");

    // 确定性：同输入跑两遍结果逐位一致
    {
        std::vector<Point3DAlg_DomeBump> bumps2;
        Point3DAlg_DomeFit(cloud, params, bumps2, nullptr);
        bool same = bumps2.size() == bumps.size();
        if (same)
            for (size_t i = 0; i < bumps.size(); ++i) {
                if (bumps[i].valid != bumps2[i].valid ||
                    bumps[i].height != bumps2[i].height ||
                    bumps[i].radius != bumps2[i].radius) { same = false; break; }
            }
        check(same, "确定性：两次运行结果一致");
    }

    // 统计接口
    {
        const Point3DAlg_DomeStats st = Point3DAlg_DomeStatsOf(bumps);
        check(st.count == (int)bumps.size(), "统计：有效数正确");
        check(st.min > 0.10 && st.max < 0.26, "统计：高度范围合理");
        check(st.p50 >= st.p5 && st.p95 >= st.p50, "统计：分位数单调");
        std::printf("  统计：n=%d mean=%.5f std=%.6f p5/p50/p95=%.5f/%.5f/%.5f\n",
                    st.count, st.mean, st.stddev, st.p5, st.p50, st.p95);
    }

    // 共面性/CPK 统计（直接构造 bumps 测纯函数 StatsOf）：
    // 高度 = 0.19 + 0.0002·x + 0.001·y（晶圆倾斜漂移）+ 噪声 σ=3µm，
    // 再混 1 个 +20µm 离群 bump——fit_sigma 应剥离倾斜且不被离群拉爆。
    {
        std::mt19937 rng(99);
        std::normal_distribution<double> noise(0.0, 0.003);
        std::vector<Point3DAlg_DomeBump> vb;
        for (int i = 0; i < 100; ++i) {
            Point3DAlg_DomeBump b;
            b.valid = true;
            b.apex[0] = (i % 10) * 0.5;      // 0..4.5
            b.apex[1] = (i / 10) * 0.5;      // 0..4.5
            b.height = 0.19 + 0.0002 * b.apex[0] + 0.001 * b.apex[1] + noise(rng);
            b.radius = 0.13;
            vb.push_back(b);
        }
        vb[50].height += 0.02;               // 单个离群 bump

        const Point3DAlg_DomeStats st = Point3DAlg_DomeStatsOf(vb);
        std::printf("  共面性：cop=%.5f std=%.6f fit_sigma=%.6f fit_p=%.6f fit_q=%.6f\n",
                    st.coplanarity, st.stddev, st.fit_sigma, st.fit_p, st.fit_q);
        check(std::fabs(st.coplanarity - (st.max - st.min)) < 1e-12, "coplanarity = max − min");
        // 倾斜在 4.5mm 对角贡献 ~6µm 漂移 + 噪声 → stddev > fit_sigma（剥离后应显著更小）
        check(st.fit_sigma < st.stddev, "fit_sigma 剥离倾斜后小于原始 stddev");
        check(st.fit_sigma < 0.005, "fit_sigma ≈ 本地噪声 3µm（+修剪余量）");
        // 离群 bump 不污染拟合面 σ（MAD 修剪）
        check(st.fit_sigma < 0.008, "离群 bump 被修剪，fit_sigma 不被拉爆");
        check(std::fabs(st.fit_p - 0.0002) < 3e-4 && std::fabs(st.fit_q - 0.001) < 3e-4,
              "拟合面倾斜系数 ≈ 真值（±3e-4 统计极限）");
        // 离群 bump 仍体现在极差法共面性里（两种口径的语义差异）
        check(st.coplanarity > 0.02, "极差法对单个离群敏感（≥离群幅度）");
    }

    // 空/退化输入
    {
        std::vector<Point3DAlg_DomeBump> eb;
        check(!Point3DAlg_DomeFit(nullptr, params, eb), "空点云返回 false");
        CloudXYZ::Ptr tiny(new CloudXYZ);
        for (int i = 0; i < 5; ++i) tiny->push_back(pcl::PointXYZ(i * 0.1f, 0, 0));
        check(!Point3DAlg_DomeFit(tiny, params, eb), "点数不足返回 false");
        // 纯平面（无 bump）
        CloudXYZ::Ptr flat(new CloudXYZ);
        std::mt19937 rng(7);
        std::normal_distribution<double> nz(0.0, 0.002);
        for (double x = 0; x < 4; x += 0.04)
            for (double y = 0; y < 4; y += 0.04)
                flat->push_back(pcl::PointXYZ((float)x, (float)y, (float)nz(rng)));
        check(!Point3DAlg_DomeFit(flat, params, eb), "纯平面（无 bump）返回 false");
    }

    // ---- BumpCompare：基准/被测匹配 + NG 判定 ----
    // ref：50 个（10×5 网格 pitch 0.4/0.5）；test：46 个同位（6 个 ±0.03 NG）
    //      + 2 个新位（多出），ref 缺 4 个（漏测）
    {
        std::vector<Point3DAlg_DomeBump> ref, test;
        auto mk = [](double x, double y, double h) {
            Point3DAlg_DomeBump b;
            b.valid = true;
            b.apex[0] = x; b.apex[1] = y;
            b.height = h;
            return b;
        };
        for (int i = 0; i < 50; ++i)
            ref.push_back(mk(0.5 + (i % 10) * 0.4, 0.5 + (i / 10) * 0.5, 0.19));
        int ngMade = 0;
        for (int i = 0; i < 46; ++i) {
            double h = 0.19;
            if (i % 7 == 0 && ngMade < 6) {   // 6 个 NG（±0.03）
                h += (ngMade % 2 == 0) ? 0.03 : -0.03;
                ++ngMade;
            }
            test.push_back(mk(0.5 + (i % 10) * 0.4, 0.5 + (i / 10) * 0.5, h));
        }
        test.push_back(mk(10.5, 10.5, 0.19));   // 多出
        test.push_back(mk(10.8, 10.5, 0.19));

        Point3DAlg_BumpCompareParams cp;
        Point3DAlg_BumpCompareResult cr;
        const bool ok = Point3DAlg_BumpCompare(ref, test, cp, cr);
        check(ok, "BumpCompare 成功");
        if (ok) {
            std::printf("  matched=%d miss_ref=%d extra=%d ng=%d (%.1f%%) dh_mean=%.5f max=%.5f\n",
                        cr.matched, cr.unmatched_ref, cr.unmatched_test,
                        cr.ng_count, 100.0 * cr.ng_ratio, cr.dh_mean, cr.dh_max_abs);
            check(cr.matched == 46, "一对一配对 46");
            check(cr.unmatched_ref == 4, "漏测 4");
            check(cr.unmatched_test == 2, "多出 2");
            check(cr.ng_count == 6, "NG = 6");
            check(std::fabs(cr.ng_ratio - 6.0 / 46.0) < 1e-9, "NG 比例");
            check(cr.diff_h.size() == 46 && cr.diff_ng.size() == 46, "差异明细长度");
            // 逐配对高度差与构造一致（apex xy 反查网格序号）
            int bad = 0;
            for (size_t i = 0; i < cr.diff_h.size(); ++i) {
                const int gi = (int)((cr.diff_xy[i * 2 + 1] - 0.5) / 0.5 + 0.5) * 10
                             + (int)((cr.diff_xy[i * 2] - 0.5) / 0.4 + 0.5);
                const double expect = test[gi].height - ref[gi].height;
                if (std::fabs(cr.diff_h[i] - expect) > 1e-9) ++bad;
            }
            check(bad == 0, "逐配对高度差与构造一致");
        }
        // 双空
        std::vector<Point3DAlg_DomeBump> e0;
        Point3DAlg_BumpCompareResult ce;
        check(!Point3DAlg_BumpCompare(e0, e0, cp, ce), "双空 → false");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}

// ---------------- 真实数据模式 ----------------

static int RunRealFile(const char* path)
{
    std::printf("== 拱形球顶拟合：真实数据 %s ==\n", path);
    CloudXYZ::Ptr cloud = shapely::CloudIO<pcl::PointXYZ>::ioLoadPLY(path);
    if (!cloud || cloud->empty()) {
        std::printf("PLY 读取失败：%s\n", path);
        return 2;
    }
    std::printf("点云：%zu 点\n", cloud->size());

    Point3DAlg_DomeFitParams params;
    Point3DAlg_DomeBase base;
    std::vector<Point3DAlg_DomeBump> bumps;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = Point3DAlg_DomeFit(cloud, params, bumps, &base);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    if (!ok) {
        std::printf("拟合失败（无有效 bump？）\n");
        return 1;
    }
    std::printf("耗时 %.0f ms；基面：内点 %d，σ=%.2fµm，法向=(%.4f,%.4f,%.4f)\n",
                ms, base.inlier_count, base.sigma * 1000.0,
                base.normal[0], base.normal[1], base.normal[2]);

    const Point3DAlg_DomeStats st = Point3DAlg_DomeStatsOf(bumps);
    std::printf("bump 数：%zu（无效 %zu）\n", bumps.size(),
                bumps.size() - (size_t)st.count);
    std::printf("高度 h：mean=%.5f std=%.5f（3σ=%.5f）min/max=%.5f/%.5f\n",
                st.mean, st.stddev, 3 * st.stddev, st.min, st.max);
    std::printf("共面性：极差=%.5f 最佳拟合面σ=%.5f（倾斜 p=%.6f q=%.6f）\n",
                st.coplanarity, st.fit_sigma, st.fit_p, st.fit_q);
    std::printf("分位数：p5=%.5f p50=%.5f p95=%.5f\n", st.p5, st.p50, st.p95);

    // 异常簇定位（h 超出正常 bump 范围的）
    {
        int nLow = 0, nHigh = 0;
        for (const auto& b : bumps) {
            if (!b.valid) continue;
            if (b.height < 0.15) ++nLow;
            if (b.height > 0.25) ++nHigh;
        }
        std::printf("异常簇：h<0.15 的 %d 个，h>0.25 的 %d 个\n", nLow, nHigh);
        int shown = 0;
        for (const auto& b : bumps) {
            if (!b.valid || (b.height >= 0.15 && b.height <= 0.25)) continue;
            std::printf("  异常: n=%d h=%.4f R=%.4f rms=%.1fµm centroid=(%.2f,%.2f) apex_z=%.4f\n",
                        b.point_count, b.height, b.radius, b.rms * 1000.0,
                        b.centroid[0], b.centroid[1], b.apex[2]);
            if (++shown >= 12) break;
        }
    }

    // Python 基准对照（moi_wafer.ply：2026 bumps，h p50≈0.187mm，σ≈0.002mm）
    double rmsMax = 0, rmsMed = 0;
    {
        std::vector<double> rms;
        for (const auto& b : bumps)
            if (b.valid) rms.push_back(b.rms);
        if (!rms.empty()) {
            std::sort(rms.begin(), rms.end());
            rmsMax = rms.back();
            rmsMed = rms[rms.size() / 2];
        }
    }
    std::printf("球拟合 rms：中位=%.2fµm max=%.2fµm\n", rmsMed * 1000.0, rmsMax * 1000.0);

    // ---- 自动断言（对照 Python 基准 scripts/_wafer_dome_baseline.py，moi_wafer.ply 专用）----
    // 基准：2019 簇（3D 聚类 tol=0.12 min=8），有效 2015 / 守卫拦截 4，
    //       h p50=0.18652 std=0.00151，rms 中位 1.11µm
    std::printf("\n对照 Python 基准断言：\n");
    check(st.count >= 2000 && st.count <= 2040, "有效 bump 数 ≈ 2015");
    check(std::fabs(st.p50 - 0.18652) < 0.0003, "高度中位与基准差 < 0.3µm");
    check(st.stddev < 0.003, "高度散差 σ < 3µm（基准 1.51µm）");
    check(st.p95 - st.p5 < 0.008, "p5~p95 覆盖 < 8µm（基准 4.9µm）");
    check(rmsMed < 0.003 && rmsMax < 0.02, "球拟合残差合理（中位 < 3µm）");
    size_t nInvalid = bumps.size() - (size_t)st.count;
    check(nInvalid < 10, "守卫拦截的非球冠簇 ≈ 4");

    // 前 5 个 bump 明细
    std::printf("前 5 个 bump（按点数降序）：\n");
    for (size_t i = 0; i < bumps.size() && i < 5; ++i) {
        const auto& b = bumps[i];
        std::printf("  #%zu n=%d h=%.5f R=%.5f apex=(%.3f,%.3f,%.4f) rms=%.2fµm valid=%d\n",
                    i, b.point_count, b.height, b.radius,
                    b.apex[0], b.apex[1], b.apex[2], b.rms * 1000.0, (int)b.valid);
    }
    std::printf("\n真实数据模式：%s（失败 %d 项）\n",
                g_fail == 0 ? "通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 2;
}

int main(int argc, char** argv)
{
    if (argc >= 2)
        return RunRealFile(argv[1]);
    return RunSynthetic();
}
