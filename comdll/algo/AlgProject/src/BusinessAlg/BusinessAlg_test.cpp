// BusinessAlg_test.cpp —— 业务层验证（RegionRatio 区域缺陷率统计）。
// 用例自造数据（网格平面点云），无外部文件依赖；check 风格与 CommonAlg_test.cpp 一致。
// 说明：projectTo2D 为 XY 凸包，"缺陷比例" = 投影凸包与 ROI 交面积 / ROI 面积。

#include "include/BusinessAlg_api.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_fail;
}

// 造 [0,1]×[0,1] 网格平面点云（z=0），n 为每边点数
static std::vector<float> makePlaneGrid(int n)
{
    std::vector<float> xyz;
    xyz.reserve((size_t)n * n * 3);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
        {
            xyz.push_back((float)i / (n - 1));
            xyz.push_back((float)j / (n - 1));
            xyz.push_back(0.0f);
        }
    return xyz;
}

// ---------------- RegionRatio ----------------
static void testRegionRatio()
{
    std::printf("== RegionRatio ==\n");
    const std::vector<float> xyz = makePlaneGrid(100);
    const int count = (int)(xyz.size() / 3);
    double ratio = -1.0, area = -1.0;

    // a) 平面铺满 ROI [0,1]×[0,1] → 比例 ≈ 1、面积 ≈ 1
    {
        const int rc = BusinessAlg_ComputeRegionRatio(xyz.data(), count,
                                                      -0.1, 0.1,          // 高度区间含 z=0
                                                      0.0, 0.0, 1.0, 1.0, // ROI
                                                      &ratio, &area);
        check(rc == 0, "满铺平面 rc=0");
        check(std::fabs(ratio - 1.0) < 1e-3, "满铺平面 比例≈1");
        check(std::fabs(area - 1.0) < 1e-3, "满铺平面 面积≈1");
    }

    // a2) 平面只占 ROI [0,2]×[0,1] 的一半 → 比例 ≈ 0.5
    {
        const int rc = BusinessAlg_ComputeRegionRatio(xyz.data(), count,
                                                      -0.1, 0.1,
                                                      0.0, 0.0, 2.0, 1.0,
                                                      &ratio, &area);
        check(rc == 0, "半铺平面 rc=0");
        check(std::fabs(ratio - 0.5) < 1e-3, "半铺平面 比例≈0.5");
    }

    // b) 错误码：空点云 / 空指针 / 非法 ROI → -1
    {
        check(BusinessAlg_ComputeRegionRatio(nullptr, 0, 0, 1, 0, 0, 1, 1, &ratio, &area) == -1,
              "空点云（count=0）rc=-1");
        check(BusinessAlg_ComputeRegionRatio(nullptr, count, 0, 1, 0, 0, 1, 1, &ratio, &area) == -1,
              "xyz 空指针 rc=-1");
        check(BusinessAlg_ComputeRegionRatio(xyz.data(), count, 0, 1, 0, 0, 1, 1, nullptr, &area) == -1,
              "输出指针空 rc=-1");
        check(BusinessAlg_ComputeRegionRatio(xyz.data(), count, 0, 1, 0, 0, 0.0, 1.0, &ratio, &area) == -1,
              "ROI 宽 0 rc=-1");
    }

    // c) 零缺陷：高度区间与数据不相交 → 投影为空 → rc=2、输出清零
    {
        const int rc = BusinessAlg_ComputeRegionRatio(xyz.data(), count,
                                                      5.0, 6.0,           // z=0 不在区间内
                                                      0.0, 0.0, 1.0, 1.0,
                                                      &ratio, &area);
        check(rc == 2, "高度区间外 rc=2");
        check(ratio == 0.0 && area == 0.0, "rc=2 时输出清零");
    }
}

int main()
{
    testRegionRatio();
    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
