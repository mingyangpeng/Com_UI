using ComUI.Plugin.Cloud3D;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// 点对刚体配准数学（CloudRegist/Horn 四元数法）：已知旋转+平移的点对 → 恢复变换与 RMS；
/// 共面退化场景（4 平面点对）；<3 对拒绝。
/// </summary>
public class CloudRegistTests
{
    private static void Apply(double[] m, float[] pts)
    {
        for (int i = 0; i < pts.Length / 3; i++)
        {
            double x = pts[i * 3], y = pts[i * 3 + 1], z = pts[i * 3 + 2];
            pts[i * 3] = (float)(m[0] * x + m[1] * y + m[2] * z + m[3]);
            pts[i * 3 + 1] = (float)(m[4] * x + m[5] * y + m[6] * z + m[7]);
            pts[i * 3 + 2] = (float)(m[8] * x + m[9] * y + m[10] * z + m[11]);
        }
    }

    [Fact]
    public void ComputeRigid_RecoversKnownRotationAndTranslation()
    {
        // 已知变换：R = Ry(20°)·Rz(30°)（先绕 Z 后绕 Y）+ 平移 (2.5, -1.3, 7.7)
        double a = 30 * Math.PI / 180, b = 20 * Math.PI / 180;
        double cA = Math.Cos(a), sA = Math.Sin(a), cB = Math.Cos(b), sB = Math.Sin(b);
        var truth = new double[]
        {
            cB * cA, -cB * sA, sB, 2.5,
            sA, cA, 0, -1.3,
            -sB * cA, sB * sA, cB, 7.7,
            0, 0, 0, 1,
        };
        float[] moving = { 1, 0, 0, 0, 2, 0, 3, 1, 1, 2, 2, 2, 5, 1, 3 };
        var reference = (float[])moving.Clone();
        Apply(truth, reference);

        var (m, rms) = CloudRegist.ComputeRigid(moving, reference);

        Assert.True(rms < 1e-4, $"RMS 应 ≈0，实际 {rms}");
        for (int i = 0; i < 16; i++)
            Assert.Equal(truth[i], m[i], 4);   // 矩阵逐元素恢复（1e-4 精度）
    }

    [Fact]
    public void ComputeRigid_CoplanarPairs_Works()
    {
        // 4 个共面点对（XY 平面 + Z 旋转 45°）：Horn 对共面输入仍给出刚体解
        float[] moving = { 0, 0, 0, 2, 0, 0, 2, 2, 0, 0, 2, 0 };
        var reference = (float[])moving.Clone();
        double rad = 45 * Math.PI / 180;
        var truth = new double[]
        {
            Math.Cos(rad), -Math.Sin(rad), 0, 3,
            Math.Sin(rad), Math.Cos(rad), 0, -1,
            0, 0, 1, 0,
            0, 0, 0, 1,
        };
        Apply(truth, reference);

        var (m, rms) = CloudRegist.ComputeRigid(moving, reference);
        Assert.True(rms < 1e-4, $"RMS 应 ≈0，实际 {rms}");
        // 旋转分量逐元素对照（平移独立可解）
        Assert.Equal(Math.Cos(rad), m[0], 4);
        Assert.Equal(Math.Sin(rad), m[4], 4);
        Assert.Equal(3, m[3], 4);
        Assert.Equal(-1, m[7], 4);
    }

    [Fact]
    public void ComputeRigid_TooFewPairs_Throws()
    {
        Assert.Throws<Exception>(() => CloudRegist.ComputeRigid(new float[] { 1, 0, 0, 0, 1, 0 }, new float[6]));
    }
}
