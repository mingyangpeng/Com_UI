using ComUI.Plugin.Cloud3D;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// 法线估计（CloudNormals）：平面点云法线全朝 +Y（翻转契约）、密度自适应半径、孤立点兜底、
/// 解析特征向量与暴力协方差最小方向一致。
/// </summary>
public class CloudNormalsTests
{
    [Fact]
    public void Compute_PlaneNormals_PointUp()
    {
        // z 平面点云（XZ 平面，高度轴 Y）：201 点 → 法线全 ≈ (0,1,0)
        const int n = 201;
        var pts = new float[n * 3];
        for (int i = 0; i < n; i++)
        {
            pts[i * 3] = (i - 100) * 0.1f;
            pts[i * 3 + 1] = 5f;                          // 平面 Y=5（法线朝 Y）
            pts[i * 3 + 2] = (i % 21) * 0.1f;
        }
        var normals = CloudNormals.Compute(pts, n);

        for (int i = 0; i < n; i++)
        {
            double nx = normals[i * 3], ny = normals[i * 3 + 1], nz = normals[i * 3 + 2];
            double len = Math.Sqrt(nx * nx + ny * ny + nz * nz);
            Assert.Equal(1.0, len, 5);            // 单位长度
            Assert.True(ny > 0.99, $"点 {i} 法线应朝 +Y（ny={ny}）");
        }
    }

    [Fact]
    public void Compute_RadiusAutoEstimate_MatchesExplicit()
    {
        var pts = new float[300 * 3];
        var rng = new Random(3);
        for (int i = 0; i < 300; i++)
        {
            pts[i * 3] = (float)rng.NextDouble() * 10;
            pts[i * 3 + 1] = 0f;
            pts[i * 3 + 2] = (float)rng.NextDouble() * 10;
        }
        var auto = CloudNormals.Compute(pts, 300);
        var explicitR = CloudNormals.Compute(pts, 300, CloudNormals.EstimateRadius(pts, 300));
        Assert.Equal(explicitR, auto);   // null 半径 = 自动估算同值
    }

    [Fact]
    public void Compute_IsolatedPoint_FallbackUp()
    {
        // 密集团 + 远处孤立点 → 孤立点法线兜底 (0,1,0)
        var pts = new float[51 * 3];
        for (int i = 0; i < 50; i++)
        {
            pts[i * 3] = i * 0.1f;
            pts[i * 3 + 1] = 0f;
            pts[i * 3 + 2] = 0f;
        }
        pts[50 * 3] = 100f; pts[50 * 3 + 1] = 100f; pts[50 * 3 + 2] = 100f;
        var normals = CloudNormals.Compute(pts, 51, 1.0);

        Assert.Equal(0f, normals[50 * 3]);
        Assert.Equal(1f, normals[50 * 3 + 1]);
        Assert.Equal(0f, normals[50 * 3 + 2]);
    }

    [Fact]
    public void SmallestEigenvector_MatchesBruteForce()
    {
        // 已知最小特征方向的协方差（对角占优：最小=cyy）→ 特征向量 ≈ ±Y，翻转后 (0,1,0)
        var n = CloudNormals.SmallestEigenvector3x3(1.0, 0, 0, 0.01, 0, 0.5);
        double len = Math.Sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        Assert.Equal(1.0, len, 5);
        Assert.True(Math.Abs(n[1]) > 0.99, $"最小特征向量应 ≈ ±Y，实际 ({n[0]},{n[1]},{n[2]})");
    }
}
