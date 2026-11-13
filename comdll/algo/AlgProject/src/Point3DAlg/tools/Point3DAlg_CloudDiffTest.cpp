// Point3DAlg_CloudDiffTest.cpp —— 点云高度差对比测试（合成数据，注册 ctest）。
// 用例：自身对比（差≈0）/ 全局抬升 / 局部凸起检出 + 超差计数 / 不重叠报错 /
//       差值点云输出。退出码：0=通过 1=失败。

#include "Point3DAlg_CloudDiff.h"

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
    std::printf("== 点云高度差对比测试 ==\n");
    std::mt19937 rng(20261110);
    const double kSigma = 0.003;   // 单云噪声 3µm
    Point3DAlg_CloudDiffParams params;   // 默认 cell=0.1, min=3, thr=0.02

    auto flat = [](double, double) { return 1.0; };

    // ---- 用例 1：同一片云自比 → 差≈0 ----
    {
        auto c = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05, flat, rng, kSigma);
        Point3DAlg_CloudDiffResult r;
        const bool ok = Point3DAlg_CloudDiff(c, c, params, r);
        check(ok, "自比成功");
        if (ok) {
            std::printf("  cells=%lld mean=%.6f sigma=%.6f max=%.5f\n",
                        r.cells, r.mean, r.sigma, r.max_abs);
            check(r.cells > 1000, "格数合理");
            check(std::fabs(r.mean) < 1e-9 && r.sigma < 1e-9 && r.max_abs < 1e-9, "自比差恒为 0");
        }
    }

    // ---- 用例 2：全局抬升 +0.05（信号 >> 阈值）→ mean≈0.05、全部格超差 ----
    // （抬升恰等于阈值 0.02 时噪声对称分布，约一半格不超差——那是正确行为，不测）
    {
        auto ca = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05, flat, rng, kSigma);
        auto cb = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05, flat, rng, kSigma);
        for (auto& p : cb->points)
            p.z += 0.05f;
        Point3DAlg_CloudDiffResult r;
        const bool ok = Point3DAlg_CloudDiff(ca, cb, params, r);
        check(ok, "全局抬升对比成功");
        if (ok) {
            std::printf("  mean=%.5f sigma=%.5f over=%lld/%lld\n",
                        r.mean, r.sigma, r.over_count, r.cells);
            check(std::fabs(r.mean - 0.05) < 0.001, "均值 = 0.05 ± 1µm");
            check(r.sigma < 0.006 && r.sigma > 0.002, "σ ≈ √2×3µm（两云噪声合成）");
            check(r.over_count == r.cells, "抬升 0.05 > 阈值 0.02：全部格超差");
        }
    }

    // ---- 用例 3：局部凸起（1×1mm +0.05）→ 检出 + 超差计数 + 差值点云 ----
    {
        auto ca = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05, flat, rng, kSigma);
        auto cb = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05,
                           [](double x, double y) {
                               return (x >= 1.0 && x <= 2.0 && y >= 1.0 && y <= 2.0) ? 1.05 : 1.0;
                           },
                           rng, kSigma);
        std::vector<float> diffCloud;
        Point3DAlg_CloudDiffResult r;
        const bool ok = Point3DAlg_CloudDiff(ca, cb, params, r, &diffCloud);
        check(ok, "局部凸起对比成功");
        if (ok) {
            const double expectCells = 100.0;   // 1×1mm / 0.1mm 格
            std::printf("  cells=%lld over=%lld (%.2f%%) max=%.5f diffPts=%zu\n",
                        r.cells, r.over_count, 100.0 * r.over_ratio, r.max_abs, diffCloud.size() / 3);
            check(std::fabs(r.over_ratio - expectCells / (double)r.cells) < 0.03,
                  "超差占比 ≈ 1mm²/(4mm²) = 25%");
            check(std::fabs(r.max_pos - 0.05) < 0.005, "最大正差 = 0.05 ± 5µm");
            check(std::fabs(r.max_neg) < 0.01, "基准区无正负翻转");
            check(diffCloud.size() == (size_t)r.cells * 3, "差值点云与格数一致");
            // 差值点云 z 有 0.05 的凸起点
            bool hasBump = false;
            for (size_t i = 2; i < diffCloud.size(); i += 3)
                if (std::fabs(diffCloud[i] - 0.05) < 0.005) { hasBump = true; break; }
            check(hasBump, "差值点云含凸起点（NG 可视化数据）");
        }
    }

    // ---- 用例 4：负差（被测低 0.03）→ max_neg ----
    {
        auto ca = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05, flat, rng, kSigma);
        auto cb = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05, flat, rng, kSigma);
        for (auto& p : cb->points)
            p.z -= 0.03f;
        Point3DAlg_CloudDiffResult r;
        const bool ok = Point3DAlg_CloudDiff(ca, cb, params, r);
        check(ok, "负差对比成功");
        if (ok) {
            // 极值受尾部噪声影响（2500 格极值 ≈ ±3.5σ ≈ 15µm），容差 20µm
            check(std::fabs(r.max_neg + 0.03) < 0.02, "最大负差 = −0.03 ± 20µm");
            check(r.mean < -0.02, "均值为负");
        }
    }

    // ---- 用例 5a：XY 粗对齐——纯错位（dz=0，十字台阶形貌）----
    // 教训（实测两轮）：
    //   ①单台阶 z 只依赖 y，x 方向对齐不可观测（gi 维得分平坦）；
    //   ②**纯梯度形貌对 XY 平移仅产生常数漂移**（0.2(x-0.25)=0.2x-0.05，栅格化
    //     中位后无任何结构误差）——对齐可观测性必须有 z 场空间特征（台阶/凸台）；
    //   ③含 z 差（dz）的对齐需非单调形貌锚定，否则 dz 沿梯度方向被平移吸收。
    {
        auto surf = [](double x, double y) {
            return 1.0 + (x >= 2.0 ? 0.5 : 0.0) + (y >= 2.0 ? 0.3 : 0.0);
        };
        auto ca = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05, surf, rng, kSigma);
        auto cb = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05, surf, rng, kSigma);
        const double dx = 0.25, dy = -0.15;
        for (auto& p : cb->points) {
            p.x += (float)dx;
            p.y += (float)dy;
        }
        Point3DAlg_CloudDiffResult r0;
        Point3DAlg_CloudDiff(ca, cb, params, r0);
        Point3DAlg_CloudDiffParams pa = params;
        pa.align_search = 0.6;
        Point3DAlg_CloudDiffResult r;
        const bool ok = Point3DAlg_CloudDiff(ca, cb, pa, r);
        check(ok, "5a 纯错位对齐成功");
        if (ok) {
            std::printf("  5a aligned=(%.3f,%.3f) mean=%.5f (no-align %.5f)\n",
                        r.aligned_dx, r.aligned_dy, r.mean, r0.mean);
            check(r.aligned, "5a aligned 标志");
            check(std::fabs(r.aligned_dx + dx) < 0.03, "5a aligned_dx ≈ −0.25");
            check(std::fabs(r.aligned_dy - 0.15) < 0.03, "5a aligned_dy ≈ +0.15");
            check(r.mean < 0.005, "5a 对齐后差 ≈ 0");
            check(r0.max_abs > 0.3, "5a 错位使十字台阶边缘格 diff ≈ 0.5（对齐前结构误差）");
        }
    }

    // ---- 用例 5b：错位 + z 差（十字台阶锚定 x/y 两轴）----
    {
        // z = 1 + 0.5·(x≥2) + 0.3·(y≥2)：两个正交台阶边缘把 x、y 平移都锁死，
        // dz=0.03 不可被 XY 平移吸收（纯梯度形貌会被吸收——5a 注释）
        auto surf = [](double x, double y) {
            return 1.0 + (x >= 2.0 ? 0.5 : 0.0) + (y >= 2.0 ? 0.3 : 0.0);
        };
        auto ca = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05, surf, rng, kSigma);
        auto cb = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05, surf, rng, kSigma);
        const double dx = 0.25, dy = -0.15, dz = 0.03;
        for (auto& p : cb->points) {
            p.x += (float)dx;
            p.y += (float)dy;
            p.z += (float)dz;
        }
        Point3DAlg_CloudDiffResult r0;
        Point3DAlg_CloudDiff(ca, cb, params, r0);
        Point3DAlg_CloudDiffParams pa = params;
        pa.align_search = 0.6;
        Point3DAlg_CloudDiffResult r;
        const bool ok = Point3DAlg_CloudDiff(ca, cb, pa, r);
        check(ok, "5b 错位+z 差对齐成功");
        if (ok) {
            std::printf("  5b aligned=(%.3f,%.3f) mean=%.5f (no-align %.5f)\n",
                        r.aligned_dx, r.aligned_dy, r.mean, r0.mean);
            // 对齐精度受"格中位不变平台"限制：平移 < cell 时格内 z 中位不变，
            // RMS 简并（真值与 ±cell/2 内的解得分相同）→ 分辨率 ≈ ±cell/2 = 0.05
            check(std::fabs(r.aligned_dx + dx) < 0.06, "5b aligned_dx ≈ −0.25（±cell/2）");
            check(std::fabs(r.aligned_dy - 0.15) < 0.06, "5b aligned_dy ≈ +0.15（±cell/2）");
            check(std::fabs(r.mean - dz) < 0.004, "5b 对齐后 mean = 真实 z 差 0.03");
            // 虚高口径用 max_abs：错位边缘格 diff 达台阶高 0.5/0.3（mean 被
            // min_points 过滤边缘格稀释——5b 实测 no-align mean 反而更低）
            check(r0.max_abs > 0.3, "5b 不对齐时台阶边缘格 diff 达台阶高（max_abs 口径）");
            check(r.max_abs < 0.1, "5b 对齐后无结构错位尖峰");
        }
    }

    // ---- 用例 6：边界 ----
    {
        auto ca = MakeGrid(0.0, 4.0, 0.0, 4.0, 0.05, flat, rng, kSigma);
        auto cb = MakeGrid(10.0, 14.0, 10.0, 14.0, 0.05, flat, rng, kSigma);
        Point3DAlg_CloudDiffResult r;
        check(!Point3DAlg_CloudDiff(nullptr, ca, params, r), "空指针返回 false");
        check(!Point3DAlg_CloudDiff(ca, cb, params, r), "完全不重叠返回 false");
        Point3DAlg_CloudDiffParams bad = params;
        bad.cell_size = 0.0;
        check(!Point3DAlg_CloudDiff(ca, ca, bad, r), "cell=0 返回 false");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
