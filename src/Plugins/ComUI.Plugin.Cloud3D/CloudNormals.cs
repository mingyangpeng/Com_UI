namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 点云法线估计（纯函数，可测试）：格网加速半径邻域（上限 64 点）→ 局部 PCA（协方差 3×3 最小特征向量 = 法线）→
/// 朝向统一翻转至 +Y 半球（与平台"Y=高度轴"契约一致，顶面朝上）。
/// 对应 CloudCompare 的 Compute normals（radius 模式）。
/// </summary>
public static class CloudNormals
{
    private const int MaxNeighbors = 64;   // PCA 邻域上限（局部平面估计 32~64 点即稳定）

    /// <summary>估算邻域搜索半径：包围盒对角 / 点数的立方根（平均间距的两倍量级）。</summary>
    public static double EstimateRadius(float[] pts, int count)
    {
        if (count <= 0) return 1e-6;
        double minx = double.MaxValue, miny = double.MaxValue, minz = double.MaxValue;
        double maxx = double.MinValue, maxy = double.MinValue, maxz = double.MinValue;
        for (int i = 0; i < count; i++)
        {
            double x = pts[i * 3], y = pts[i * 3 + 1], z = pts[i * 3 + 2];
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
            if (z < minz) minz = z;
            if (z > maxz) maxz = z;
        }
        double diag = Math.Sqrt(
            (maxx - minx) * (maxx - minx) +
            (maxy - miny) * (maxy - miny) +
            (maxz - minz) * (maxz - minz));
        return Math.Max(diag / Math.Max(Math.Cbrt(count), 1) * 2.0, 1e-9);
    }

    /// <summary>计算逐点法线（float[3·count]，SoA nx,ny,nz）。radius=邻域半径（null=自动估算）。
    /// 孤立点（邻域 < 3 点）法线=(0,1,0) 兜底（朝上）。</summary>
    public static float[] Compute(float[] pts, int count, double? radius = null)
    {
        if (count <= 0) return Array.Empty<float>();
        double r = radius ?? EstimateRadius(pts, count);
        double r2 = r * r;

        var grid = new Dictionary<(int, int, int), List<int>>(count / 2 + 1);
        for (int i = 0; i < count; i++)
        {
            var key = ((int)Math.Floor(pts[i * 3] / r),
                       (int)Math.Floor(pts[i * 3 + 1] / r),
                       (int)Math.Floor(pts[i * 3 + 2] / r));
            if (!grid.TryGetValue(key, out var list)) grid[key] = list = new List<int>(8);
            list.Add(i);
        }

        var normals = new float[count * 3];
        var nb = new float[MaxNeighbors * 3];   // 邻域坐标缓冲（复用，每点重填）
        for (int i = 0; i < count; i++)
        {
            double x = pts[i * 3], y = pts[i * 3 + 1], z = pts[i * 3 + 2];
            var cell = ((int)Math.Floor(x / r), (int)Math.Floor(y / r), (int)Math.Floor(z / r));

            int m = 0;   // 收集半径内邻域点（上限 MaxNeighbors）
            for (int dz = -1; dz <= 1 && m < MaxNeighbors; dz++)
                for (int dy = -1; dy <= 1 && m < MaxNeighbors; dy++)
                    for (int dx = -1; dx <= 1 && m < MaxNeighbors; dx++)
                    {
                        if (!grid.TryGetValue((cell.Item1 + dx, cell.Item2 + dy, cell.Item3 + dz), out var list)) continue;
                        foreach (var j in list)
                        {
                            if (j == i) continue;
                            double ddx = pts[j * 3] - x, ddy = pts[j * 3 + 1] - y, ddz = pts[j * 3 + 2] - z;
                            if (ddx * ddx + ddy * ddy + ddz * ddz > r2) continue;
                            nb[m * 3] = pts[j * 3]; nb[m * 3 + 1] = pts[j * 3 + 1]; nb[m * 3 + 2] = pts[j * 3 + 2];
                            if (++m >= MaxNeighbors) break;
                        }
                    }

            if (m < 3)
            {
                normals[i * 3] = 0; normals[i * 3 + 1] = 1; normals[i * 3 + 2] = 0;   // 孤立点兜底朝上
                continue;
            }

            // 邻域质心
            double cx = 0, cy = 0, cz = 0;
            for (int t = 0; t < m; t++)
            {
                cx += nb[t * 3]; cy += nb[t * 3 + 1]; cz += nb[t * 3 + 2];
            }
            cx /= m; cy /= m; cz /= m;

            // 协方差 3×3（对称，只存上三角 6 项）
            double cxx = 0, cxy = 0, cxz = 0, cyy = 0, cyz = 0, czz = 0;
            for (int t = 0; t < m; t++)
            {
                double qx = nb[t * 3] - cx, qy = nb[t * 3 + 1] - cy, qz = nb[t * 3 + 2] - cz;
                cxx += qx * qx; cxy += qx * qy; cxz += qx * qz;
                cyy += qy * qy; cyz += qy * qz;
                czz += qz * qz;
            }

            var n = SmallestEigenvector3x3(cxx, cxy, cxz, cyy, cyz, czz);

            // 朝向契约：翻转至 +Y 半球（顶面朝上）
            if (n[1] < 0) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
            normals[i * 3] = (float)n[0];
            normals[i * 3 + 1] = (float)n[1];
            normals[i * 3 + 2] = (float)n[2];
        }
        return normals;
    }

    /// <summary>对称 3×3 协方差最小特征值对应的特征向量（解析式 + Jacobi 兜底混合：直接解析避免迭代漂移）。
    /// 行主序 a = {{cxx, cxy, cxz}, {cxy, cyy, cyz}, {cxz, cyz, czz}}。</summary>
    public static double[] SmallestEigenvector3x3(
        double cxx, double cxy, double cxz, double cyy, double cyz, double czz)
    {
        // 特征多项式 λ³ − tr·λ² + M·λ − det = 0 的三个实根（海ermann 公式，对称矩阵必全实根）
        double tr = cxx + cyy + czz;
        double q = tr / 3;
        double m00 = cxx - q, m11 = cyy - q, m22 = czz - q;
        double r2 = m00 * m00 + m11 * m11 + m22 * m22 + 2 * (cxy * cxy + cxz * cxz + cyz * cyz);
        if (r2 < 1e-24)
            return new double[] { 0, 1, 0 };   // 各向同性（退化）→ 朝上兜底
        double pFact = Math.Sqrt(r2 / 6);
        double invP = 1 / pFact;
        double b00 = invP * m00, b11 = invP * m11, b22 = invP * m22;
        double b01 = invP * cxy, b02 = invP * cxz, b12 = invP * cyz;
        double detB = b00 * (b11 * b22 - b12 * b12) - b01 * (b01 * b22 - b12 * b02) + b02 * (b01 * b12 - b11 * b02);
        double halfDetB = Math.Clamp(detB / 2, -1, 1);
        double angle = Math.Acos(halfDetB) / 3;
        // 三个特征值 = q + 2p·cos(angle + 2πk/3)，k=0,1,2；最小者对应特征向量 = N·(1,φ,φ²) 其中 φ 由两特征值联立解出
        double l1 = q + 2 * pFact * Math.Cos(angle + 2 * Math.PI / 3);   // 最小
        double l3 = q + 2 * pFact * Math.Cos(angle);                     // 最大
        double l2 = tr - l1 - l3;

        // 最小特征值对应特征向量 = adjugate 作用：n ∝ (M − λ₂I)·(M − λ₃I)·e_k（取非零分量）
        // 用 (M − λ₂I)·(M − λ₃I) 的任一列（该矩阵秩 1，列方向即特征向量）
        double A = cxx - l2, B = cxy, C = cxz;
        double D = cxy, E = cyy - l2, F = cyz;
        double G = cxz, H = cyz, I = czz - l2;
        double B2 = cxx - l3, C2 = cxy, D2 = cxz, E2 = cyy - l3, F2 = cyz, G2 = cxz, H2 = cyz, I2 = czz - l3;

        // C = (M−λ₂I)(M−λ₃I) 三列
        double col0x = A * B2 + B * D2 + C * G2, col0y = D * B2 + E * D2 + F * G2, col0z = G * B2 + H * D2 + I * G2;
        double col1x = A * C2 + B * E2 + C * H2, col1y = D * C2 + E * E2 + F * H2, col1z = G * C2 + H * E2 + I * H2;
        double col2x = A * C2 + B * F2 + C * I2, col2y = D * C2 + E * F2 + F * I2, col2z = G * C2 + H * F2 + I * I2;

        // 取模最大的列（数值稳定）
        double n0 = col0x * col0x + col0y * col0y + col0z * col0z;
        double n1 = col1x * col1x + col1y * col1y + col1z * col1z;
        double n2 = col2x * col2x + col2y * col2y + col2z * col2z;
        double[] col;
        if (n0 >= n1 && n0 >= n2) col = new[] { col0x, col0y, col0z };
        else if (n1 >= n2) col = new[] { col1x, col1y, col1z };
        else col = new[] { col2x, col2y, col2z };

        double len = Math.Sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
        if (len < 1e-12)
            return new double[] { 0, 1, 0 };   // 退化兜底
        return new double[] { col[0] / len, col[1] / len, col[2] / len };
    }
}
