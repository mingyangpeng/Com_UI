// Point3DAlg_BumpReportTest.cpp —— bump 全参数报表测试（合成数据，注册 ctest）。
// 用例：名义阵列（高度+直径双真值）/ 高度 NG / 直径 NG / coplanarity 偏差 / 边界。
// 退出码：0=通过 1=失败。

#include "Point3DAlg_BumpReport.h"

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

// 球冠 bump（基面 1.0）：h = 球顶高、R = 球半径；截面（半高）圆直径真值
static void AddBump(CloudXYZ::Ptr c, double cx, double cy, double h, double R,
                    std::mt19937& rng)
{
    // 可见上支：d ≤ R（球心不低于基面时）；基面以下回填 base+noise
    std::normal_distribution<double> noise(0.0, 0.002);
    const double zc = 1.0 + h - R;
    const double dMax = (zc >= 1.0)
        ? R
        : std::sqrt(std::max(0.0, R * R - (1.0 - zc) * (1.0 - zc)));
    for (double x = cx - dMax - 0.1; x <= cx + dMax + 0.1; x += 0.04)
        for (double y = cy - dMax - 0.1; y <= cy + dMax + 0.1; y += 0.04) {
            const double dx = x - cx, dy = y - cy;
            const double d2 = dx * dx + dy * dy;
            if (d2 < dMax * dMax) {
                const double zb = zc + std::sqrt(R * R - d2);
                c->push_back(pcl::PointXYZ((float)x, (float)y, (float)(zb + noise(rng))));
            } else {
                c->push_back(pcl::PointXYZ((float)x, (float)y,
                                           (float)(1.0 + noise(rng))));
            }
        }
}

int main()
{
    std::printf("== bump 全参数报表测试 ==\n");
    std::mt19937 rng(20261111);

    // ---- 用例 1：名义阵列（9 bump 同高同径）----
    // 真值：h=0.19、R=0.13 → 距顶 0.35 处截面直径 = 2×sqrt(R²−(R−0.35h)²) ≈ 0.2269
    // （纯球冠的 z 范围是 [顶, 冠底]——"半高"低于冠底无截面，故用距顶比例截面）
    {
        CloudXYZ::Ptr cloud(new CloudXYZ);
        std::normal_distribution<double> noise(0.0, 0.002);
        for (double x = 0.0; x <= 5.0; x += 0.04)
            for (double y = 0.0; y <= 5.0; y += 0.04) {
                double z = 1.0 + noise(rng);
                for (int i = 0; i < 9; ++i) {
                    const double cx = 1.0 + (i % 3) * 1.5, cy = 1.0 + (i / 3) * 1.5;
                    const double dx = x - cx, dy = y - cy;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < 0.13 * 0.13) {   // 可见上支 d ≤ R
                        const double zb = 1.0 + (0.19 - 0.13) + std::sqrt(0.13 * 0.13 - d2);
                        if (zb > z) z = zb + noise(rng);
                    }
                }
                cloud->push_back(pcl::PointXYZ((float)x, (float)y, (float)z));
            }

        Point3DAlg_BumpReportParams p;
        p.height_min = 0.15;
        p.height_max = 0.25;
        p.diameter_min = 0.15;
        p.diameter_max = 0.35;
        std::vector<Point3DAlg_BumpReportEntry> es;
        Point3DAlg_BumpReportStats st;
        const bool ok = Point3DAlg_BumpReport(cloud, p, es, &st);
        check(ok, "名义阵列报表成功");
        if (ok) {
            std::printf("  count=%d h=%.4f±%.4f d=%.4f±%.4f copσ=%.5f ngH=%d ngD=%d\n",
                        st.count, st.h_mean, st.h_sigma, st.d_mean, st.d_sigma,
                        st.cop_sigma, st.ng_height, st.ng_diameter);
            check(st.count == 9, "检出 9 个 bump");
            check(std::fabs(st.h_mean - 0.19) < 0.003, "高度均值 = 0.19 ± 3µm");
            // 直径真值 0.2269：距顶 0.35 截面圆
            check(std::fabs(st.d_mean - 0.2269) < 0.01, "直径均值 = 0.2269 ± 10µm");
            check(st.ng_height == 0 && st.ng_diameter == 0, "名义阵列零 NG");
            int nCircle = 0;
            for (const auto& e : es)
                if (e.diameter > 0.0) ++nCircle;
            check(nCircle == 9, "全部 bump 完成直径拟合");
            // coplanarity 偏差：同高阵列 → σ ≈ 高度噪声
            check(st.cop_sigma < 0.005, "共面性偏差 σ ≈ 噪声水平");
        }
    }

    // ---- 用例 2：高度 NG（1 个 +0.05）+ 直径 NG（1 个小直径）----
    {
        CloudXYZ::Ptr cloud(new CloudXYZ);
        std::normal_distribution<double> noise(0.0, 0.002);
        // bump 参数：[cx, cy, h, R]
        struct T { double cx, cy, h, R; };
        const T ts[3] = { { 1.0, 1.0, 0.19, 0.13 },    // 正常（截面直径 ≈0.227）
                          { 3.0, 1.0, 0.24, 0.13 },    // 高度 NG（> height_max 0.22）
                          { 5.0, 1.0, 0.19, 0.15 } };  // 直径 NG（截面径 ≈0.278 > max 0.25）
        //   第三个 R=0.15：截面径 = 2×sqrt(0.0225−(0.15−0.0665)²) = 2×0.139 = 0.278
        //   （初版 R=0.10 时 capR≈0.043 冠过小、DomeFit 检不出——几何须合法）
        for (double x = 0.0; x <= 6.0; x += 0.04)
            for (double y = 0.0; y <= 2.0; y += 0.04) {
                double z = 1.0 + noise(rng);
                for (const T& t : ts) {
                    const double dx = x - t.cx, dy = y - t.cy;
                    const double d2 = dx * dx + dy * dy;
                    // 可见上支：d ≤ R（球心高于基面时外圈也可见）
                    if (d2 < t.R * t.R) {
                        const double zb = 1.0 + (t.h - t.R) + std::sqrt(t.R * t.R - d2);
                        if (zb > z) z = zb + noise(rng);
                    }
                }
                cloud->push_back(pcl::PointXYZ((float)x, (float)y, (float)z));
            }

        Point3DAlg_BumpReportParams p;
        p.height_min = 0.15;
        p.height_max = 0.22;
        p.diameter_min = 0.18;
        p.diameter_max = 0.24;   // bump2 深截面径 0.244、bump3 大径 0.249 均超
        std::vector<Point3DAlg_BumpReportEntry> es;
        Point3DAlg_BumpReportStats st;
        const bool ok = Point3DAlg_BumpReport(cloud, p, es, &st);
        check(ok, "NG 阵列报表成功");
        if (ok) {
            std::printf("  count=%d ngH=%d ngD=%d\n", st.count, st.ng_height, st.ng_diameter);
            for (const auto& e : es)
                std::printf("    (%.3f,%.3f) h=%.4f d=%.4f rms=%.5f n=%d flags=%d\n",
                            e.x, e.y, e.height, e.diameter, e.circle_rms,
                            e.circle_points, e.ng_flags);
            check(st.count == 3, "检出 3 个 bump");
            check(st.ng_height == 1, "高度 NG = 1");
            check(st.ng_diameter == 2, "直径 NG = 2（bump2 深截面 + bump3 大径）");
            // NG 位逐 entry 验证
            int hNg = 0, dNg = 0;
            for (const auto& e : es) {
                if (e.ng_flags & 1) ++hNg;
                if (e.ng_flags & 2) ++dNg;
            }
            check(hNg == 1 && dNg == 2, "NG 位与统计一致");
        }
    }

    // ---- 用例 3：coplanarity 偏差（线性翘曲剥离后逐点残差）----
    {
        CloudXYZ::Ptr cloud(new CloudXYZ);
        std::normal_distribution<double> noise(0.0, 0.002);
        // 基面带线性翘曲：bump 高度沿 y 递增（+0.01/mm）→ dev 剥离后 ≈ 噪声
        for (double x = 0.0; x <= 5.0; x += 0.04)
            for (double y = 0.0; y <= 5.0; y += 0.04) {
                double z = 1.0 + 0.01 * y + noise(rng);
                for (int i = 0; i < 9; ++i) {
                    const double cx = 1.0 + (i % 3) * 1.5, cy = 1.0 + (i / 3) * 1.5;
                    const double dx = x - cx, dy = y - cy;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < 0.13 * 0.13) {   // 可见上支 d ≤ R
                        const double zb = 1.0 + 0.01 * y + (0.19 - 0.13)
                                        + std::sqrt(0.13 * 0.13 - d2);
                        if (zb > z) z = zb + noise(rng);
                    }
                }
                cloud->push_back(pcl::PointXYZ((float)x, (float)y, (float)z));
            }

        Point3DAlg_BumpReportParams p;
        std::vector<Point3DAlg_BumpReportEntry> es;
        Point3DAlg_BumpReportStats st;
        const bool ok = Point3DAlg_BumpReport(cloud, p, es, &st);
        check(ok, "翘曲阵列报表成功");
        if (ok) {
            std::printf("  copσ=%.5f（剥离前 std≈%.5f）\n", st.cop_sigma, st.h_sigma);
            check(st.cop_sigma < 0.005, "线性翘曲被拟合面剥离（cop σ ≈ 噪声）");
            // 剥离后 dev 残差只剩噪声（±2µm 级）；原始倾斜体现在剥离前的
            // height 场随 y 单调（+0.01/mm）——由 copσ ≈ 噪声反证剥离生效
            bool devAllSmall = true;
            for (const auto& e : es)
                if (std::fabs(e.coplanarity_dev) > 0.006) devAllSmall = false;
            check(devAllSmall, "剥离后逐点残差 |dev| ≤ 6µm");
        }
    }

    // ---- 用例 4：边界 ----
    {
        CloudXYZ::Ptr cloud(new CloudXYZ);
        std::vector<Point3DAlg_BumpReportEntry> es;
        Point3DAlg_BumpReportStats st;
        check(!Point3DAlg_BumpReport(nullptr, Point3DAlg_BumpReportParams{}, es, &st),
              "空点云 → false");
        std::normal_distribution<double> noise(0.0, 0.002);
        for (double x = 0; x < 2; x += 0.04)
            for (double y = 0; y < 2; y += 0.04)
                cloud->push_back(pcl::PointXYZ((float)x, (float)y, (float)(1.0 + noise(rng))));
        check(!Point3DAlg_BumpReport(cloud, Point3DAlg_BumpReportParams{}, es, &st),
              "纯平面（无 bump）→ false");
    }

    // ---- 用例 5：位置度（名义网格 pitch>0 启用）----
    // 名义 3×3 网格 pitch 0.5、origin (0.75,0.75)；bump 生成在名义位置（无偏差）
    // → pos_dev ≈ 噪声、ng_pos=0；场景 B：1 个 bump 偏移 +0.08（> 阈值 0.05）→
    //   pos_dev≈0.08 bit3、1 个名义点缺失 → nominal_unmatched=1
    {
        // 场景 A：无偏差
        CloudXYZ::Ptr cloud(new CloudXYZ);
        std::normal_distribution<double> noise(0.0, 0.002);
        auto addBump = [&cloud, &noise, &rng](double cx, double cy, double h, double R) {
            const double zc = 1.0 + h - R;
            const double dMax = (zc >= 1.0)
                ? R
                : std::sqrt(std::max(0.0, R * R - (1.0 - zc) * (1.0 - zc)));
            for (double x = cx - dMax - 0.1; x <= cx + dMax + 0.1; x += 0.04)
                for (double y = cy - dMax - 0.1; y <= cy + dMax + 0.1; y += 0.04) {
                    const double dx = x - cx, dy = y - cy;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < dMax * dMax) {
                        const double zb = zc + std::sqrt(R * R - d2);
                        cloud->push_back(pcl::PointXYZ((float)x, (float)y,
                                                       (float)(zb + noise(rng))));
                    } else {
                        cloud->push_back(pcl::PointXYZ((float)x, (float)y,
                                                       (float)(1.0 + noise(rng))));
                    }
                }
        };
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                addBump(0.75 + i * 0.5, 0.75 + j * 0.5, 0.19, 0.13);

        Point3DAlg_BumpReportParams p;
        p.cluster_tolerance = 0.15;   // 阵列间距 0.5/球径 0.26 的合适量级；
                                      // 默认 0.25 会把 +0.08 位移 bump 与邻簇（间距 0.22）合并
        p.nominal_pitch_x = 0.5;
        p.nominal_pitch_y = 0.5;
        p.nominal_origin_x = 0.75;
        p.nominal_origin_y = 0.75;
        p.pos_threshold = 0.05;
        std::vector<Point3DAlg_BumpReportEntry> es;
        Point3DAlg_BumpReportStats st;
        const bool ok = Point3DAlg_BumpReport(cloud, p, es, &st);
        check(ok, "位置度：无偏差阵列成功");
        if (ok) {
            std::printf("  A: nominal=%d matched=%d pos_mean=%.5f pos_max=%.5f ng_pos=%d\n",
                        st.nominal_total, st.nominal_matched, st.pos_mean,
                        st.pos_max, st.ng_pos);
            check(st.nominal_total == 9, "名义点总数 = 9");
            check(st.nominal_matched == 9 && st.nominal_unmatched == 0,
                  "全部 bump 匹配名义点");
            bool devSmall = true;
            for (const auto& e : es)
                if (e.matched_nom == 1 && e.pos_dev > 0.01) devSmall = false;
            check(devSmall, "无偏差：pos_dev ≈ 噪声（<10µm）");
            check(st.ng_pos == 0, "零位置度 NG");
        }

        // 场景 B：同一云但 1 个 bump 偏移 +0.08（沿 X，> 阈值 0.05）
        // 生成几何与场景 A 同构（整球冠到 dMax=R，保证每簇 ~20 点不被最小点数拒掉）
        CloudXYZ::Ptr cloud2(new CloudXYZ);
        for (double x = 0.0; x <= 5.0; x += 0.04)
            for (double y = 0.0; y <= 5.0; y += 0.04) {
                double z = 1.0 + noise(rng);
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) {
                        double cx = 0.75 + i * 0.5, cy = 0.75 + j * 0.5;
                        if (i == 1 && j == 1) cx += 0.08;   // 中心 bump 位置偏移
                        const double R = 0.13, zc = 1.0 + 0.19 - R;
                        const double dx = x - cx, dy = y - cy;
                        const double d2 = dx * dx + dy * dy;
                        if (d2 < R * R) {
                            const double zb = zc + std::sqrt(R * R - d2);
                            if (zb > z) z = zb + noise(rng);
                        }
                    }
                cloud2->push_back(pcl::PointXYZ((float)x, (float)y, (float)z));
            }
        Point3DAlg_BumpReportStats st2;
        std::vector<Point3DAlg_BumpReportEntry> es2;
        const bool ok2 = Point3DAlg_BumpReport(cloud2, p, es2, &st2);
        check(ok2, "位置度：偏移阵列成功");
        if (ok2) {
            std::printf("  B: matched=%d unmatched=%d ng_pos=%d\n",
                        st2.nominal_matched, st2.nominal_unmatched, st2.ng_pos);
            // 偏移 bump：pos_dev ≈ 0.08 > 0.05 → NG；且匹配到偏移后最近名义点仍记 1
            double maxDev = 0.0;
            int nOver = 0;
            for (const auto& e : es2)
                if (e.matched_nom == 1 && e.pos_dev > maxDev) maxDev = e.pos_dev;
            for (const auto& e : es2)
                if (e.matched_nom == 1 && e.pos_dev > 0.05) ++nOver;
            std::printf("  B: 偏移 bump pos_dev=%.3f\n", maxDev);
            check(maxDev > 0.05, "偏移 bump 位置度 > 0.05");
            check(st2.ng_pos >= 1, "位置度 NG ≥ 1");
        }
    }

    // ---- 用例 6：未启用名义网格（兼容性：pos_dev = -1、无 bit3）----
    {
        CloudXYZ::Ptr cloud(new CloudXYZ);
        std::normal_distribution<double> noise(0.0, 0.002);
        auto addBump = [&cloud, &noise, &rng](double cx, double cy) {
            const double h = 0.19, R = 0.13;
            const double zc = 1.0 + h - R;
            const double dMax = R;
            for (double x = cx - dMax - 0.1; x <= cx + dMax + 0.1; x += 0.04)
                for (double y = cy - dMax - 0.1; y <= cy + dMax + 0.1; y += 0.04) {
                    const double dx = x - cx, dy = y - cy;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < dMax * dMax) {
                        const double zb = zc + std::sqrt(R * R - d2);
                        cloud->push_back(pcl::PointXYZ((float)x, (float)y,
                                                       (float)(zb + noise(rng))));
                    } else {
                        cloud->push_back(pcl::PointXYZ((float)x, (float)y,
                                                       (float)(1.0 + noise(rng))));
                    }
                }
        };
        addBump(1.0, 1.0);
        Point3DAlg_BumpReportParams p;   // nominal 默认 pitch=0 = 不启用
        std::vector<Point3DAlg_BumpReportEntry> es;
        Point3DAlg_BumpReportStats st6;
        const bool ok = Point3DAlg_BumpReport(cloud, p, es, &st6);
        check(ok, "未启用名义网格报表成功");
        if (ok) {
            bool anyBit3 = false;
            for (const auto& e : es)
                if (e.ng_flags & 8) anyBit3 = true;
            check(!anyBit3, "未启用时无 bit3");
            bool allUnset = true;
            for (const auto& e : es)
                if (e.pos_dev >= 0.0) allUnset = false;
            check(allUnset, "未启用时 pos_dev 均为 -1");
        }
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
