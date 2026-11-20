namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 点对刚体配准（Horn 四元数法，纯函数可测试）：给定对应点对（移动点/参考点，世界坐标），
/// 求最优刚体变换 M（v' = M·v，行主序 4×4 末列平移——与 CloudData.Xform 同契约）与 RMS。
/// 交互流程：用户在两片点云上交替拾取 ≥3 对对应点 → 本函数求粗配准 → 衔接「手动配准(ICP)」算子精配。
/// </summary>
public static class CloudRegist
{
    /// <param name="moving">移动点云上的拾取点（XYZ 紧排，n×3）</param>
    /// <param name="reference">参考点云上的对应点（与 moving 一一对应）</param>
    public static (double[] M, double Rms) ComputeRigid(float[] moving, float[] reference)
    {
        int n = moving.Length / 3;
        if (n < 3) throw new Exception("点对配准至少需要 3 对对应点");
        if (reference.Length != moving.Length) throw new Exception("移动/参考点数不一致");

        double mx = 0, my = 0, mz = 0, rx = 0, ry = 0, rz = 0;
        for (int i = 0; i < n; i++)
        {
            mx += moving[i * 3]; my += moving[i * 3 + 1]; mz += moving[i * 3 + 2];
            rx += reference[i * 3]; ry += reference[i * 3 + 1]; rz += reference[i * 3 + 2];
        }
        mx /= n; my /= n; mz /= n;
        rx /= n; ry /= n; rz /= n;

        // Horn 4×4 对称矩阵 N（中心化坐标的相关项）
        double Sxx = 0, Sxy = 0, Sxz = 0, Syx = 0, Syy = 0, Syz = 0, Szx = 0, Szy = 0, Szz = 0;
        for (int i = 0; i < n; i++)
        {
            double px = moving[i * 3] - mx, py = moving[i * 3 + 1] - my, pz = moving[i * 3 + 2] - mz;
            double qx = reference[i * 3] - rx, qy = reference[i * 3 + 1] - ry, qz = reference[i * 3 + 2] - rz;
            Sxx += px * qx; Sxy += px * qy; Sxz += px * qz;
            Syx += py * qx; Syy += py * qy; Syz += py * qz;
            Szx += pz * qx; Szy += pz * qy; Szz += pz * qz;
        }
        var N = new double[16]
        {
            Sxx + Syy + Szz, Syz - Szy,       Szx - Sxz,       Sxy - Syx,
            Syz - Szy,       Sxx - Syy - Szz, Sxy + Syx,       Szx + Sxz,
            Szx - Sxz,       Sxy + Syx,       -Sxx + Syy - Szz, Syz + Szy,
            Sxy - Syx,       Szx + Sxz,       Syz + Szy,       -Sxx - Syy + Szz,
        };

        // 最大特征值对应的特征向量 = 最优四元数 (q0 标量, q1/q2/q3 向量)
        var q = MaxEigenvector(N);
        double norm = Math.Sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (norm < 1e-12) throw new Exception("点对配准退化：四元数范数为零（点对共面或对称，建议增加点对/改变分布）");
        double q0 = q[0] / norm, q1 = q[1] / norm, q2 = q[2] / norm, q3 = q[3] / norm;

        // 四元数 → 旋转矩阵 R（q0 = w）
        double R00 = 1 - 2 * (q2 * q2 + q3 * q3), R01 = 2 * (q1 * q2 - q0 * q3), R02 = 2 * (q1 * q3 + q0 * q2);
        double R10 = 2 * (q1 * q2 + q0 * q3), R11 = 1 - 2 * (q1 * q1 + q3 * q3), R12 = 2 * (q2 * q3 - q0 * q1);
        double R20 = 2 * (q1 * q3 - q0 * q2), R21 = 2 * (q2 * q3 + q0 * q1), R22 = 1 - 2 * (q1 * q1 + q2 * q2);

        // 平移：t = 质心参考 − R·质心移动
        double tx = rx - (R00 * mx + R01 * my + R02 * mz);
        double ty = ry - (R10 * mx + R11 * my + R12 * mz);
        double tz = rz - (R20 * mx + R21 * my + R22 * mz);

        var M = new double[16]
        {
            R00, R01, R02, tx,
            R10, R11, R12, ty,
            R20, R21, R22, tz,
            0, 0, 0, 1,
        };

        // RMS = sqrt(Σ|M·p − q|² / n)
        double sum2 = 0;
        for (int i = 0; i < n; i++)
        {
            double px = moving[i * 3], py = moving[i * 3 + 1], pz = moving[i * 3 + 2];
            double ex = R00 * px + R01 * py + R02 * pz + tx - reference[i * 3];
            double ey = R10 * px + R11 * py + R12 * pz + ty - reference[i * 3 + 1];
            double ez = R20 * px + R21 * py + R22 * pz + tz - reference[i * 3 + 2];
            sum2 += ex * ex + ey * ey + ez * ez;
        }
        return (M, Math.Sqrt(sum2 / n));
    }

    /// <summary>对称 4×4 矩阵最大特征值对应的特征向量（循环 Jacobi 旋转至对角占优）。</summary>
    internal static double[] MaxEigenvector(double[] a)
    {
        var v = new double[16] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };   // 累积旋转（列主序存列向量）
        for (int sweep = 0; sweep < 64; sweep++)
        {
            double off = 0;
            for (int i = 0; i < 4; i++)
                for (int j = i + 1; j < 4; j++)
                    off += a[i * 4 + j] * a[i * 4 + j];
            if (off < 1e-20) break;

            for (int p = 0; p < 4; p++)
            {
                for (int q = p + 1; q < 4; q++)
                {
                    double apq = a[p * 4 + q];
                    if (Math.Abs(apq) < 1e-15) continue;
                    double theta = (a[q * 4 + q] - a[p * 4 + p]) / (2 * apq);
                    double t = Math.Sign(theta) / (Math.Abs(theta) + Math.Sqrt(theta * theta + 1));
                    if (theta == 0) t = 1;
                    double c = 1 / Math.Sqrt(t * t + 1), s = t * c;

                    // A ← JᵀAJ（J = (p,q) 平面旋转）
                    for (int k = 0; k < 4; k++)
                    {
                        double akp = a[k * 4 + p], akq = a[k * 4 + q];
                        a[k * 4 + p] = c * akp - s * akq;
                        a[k * 4 + q] = s * akp + c * akq;
                    }
                    for (int k = 0; k < 4; k++)
                    {
                        double apk = a[p * 4 + k], aqk = a[q * 4 + k];
                        a[p * 4 + k] = c * apk - s * aqk;
                        a[q * 4 + k] = s * apk + c * aqk;
                    }
                    for (int k = 0; k < 4; k++)
                    {
                        double vkp = v[k * 4 + p], vkq = v[k * 4 + q];
                        v[k * 4 + p] = c * vkp - s * vkq;
                        v[k * 4 + q] = s * vkp + c * vkq;
                    }
                }
            }
        }

        // 最大对角元所在列 = 最大特征向量
        int best = 0;
        for (int i = 1; i < 4; i++)
            if (a[i * 4 + i] > a[best * 4 + best]) best = i;
        var vec = new double[4];
        for (int k = 0; k < 4; k++) vec[k] = v[k * 4 + best];
        return vec;
    }
}
