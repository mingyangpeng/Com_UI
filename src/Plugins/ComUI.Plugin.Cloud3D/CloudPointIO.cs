using System.Globalization;
using System.IO;
using ComUI.Sdk;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 点云本地 IO 与基础处理（纯函数，可测试）：PLY（ascii / binary_little_endian）与 XYZ/CSV 文本
/// 导入导出、随机下采样、统计离群滤波（格网加速半径计数）、多实体合并。
/// 坐标契约与总线一致：XYZ SoA float[3n] + RGB SoA byte[3n]（可空）。
/// </summary>
public static class CloudPointIO
{
    // ==================== 导入 ====================

    /// <summary>按扩展名导入点云：.ply（ascii/binary_little_endian）、.las（1.2 format 0/2）与 .xyz/.txt/.csv/_pts（每行 x y z [r g b]）。</summary>
    public static CloudPayload LoadFrom(string path)
    {
        var ext = Path.GetExtension(path).ToLowerInvariant();
        if (ext == ".ply") return LoadPly(path);
        if (ext == ".las") return CloudLas.Load(path);
        return LoadXyz(path);
    }

    /// <summary>XYZ/CSV 文本：每行 x y z [r g b]（空格/逗号/制表符分隔；'#' 注释与空行跳过；RGB 取 0~255）。
    /// 流式逐行读（大文件不整载）；坐标非法行跳过并计数。</summary>
    public static CloudPayload LoadXyz(string path)
    {
        var pts = new List<float>(1 << 20);
        var cols = new List<byte>(1 << 20);
        bool sawColor = false;
        long skipped = 0;
        using var reader = new StreamReader(path);
        string? line;
        while ((line = reader.ReadLine()) is not null)
        {
            var t = line.Trim();
            if (t.Length == 0 || t.StartsWith('#')) continue;
            var parts = t.Split(new[] { ' ', '\t', ',', ';' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 3) { skipped++; continue; }
            if (!float.TryParse(parts[0], NumberStyles.Float, CultureInfo.InvariantCulture, out var x) ||
                !float.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture, out var y) ||
                !float.TryParse(parts[2], NumberStyles.Float, CultureInfo.InvariantCulture, out var z))
            { skipped++; continue; }
            pts.Add(x); pts.Add(y); pts.Add(z);
            if (parts.Length >= 6 &&
                byte.TryParse(parts[3], out var r) && byte.TryParse(parts[4], out var g) && byte.TryParse(parts[5], out var b))
            {
                cols.Add(r); cols.Add(g); cols.Add(b);
                sawColor = true;
            }
            else if (sawColor || parts.Length >= 6)
            {
                // 颜色缺失/非法行补占位色——保持 cols 与点数严格对齐（混行列文件防错位）
                cols.Add(200); cols.Add(200); cols.Add(200);
                if (parts.Length >= 6) sawColor = true;
            }
        }
        if (pts.Count == 0) throw new Exception($"文件不含有效点：{Path.GetFileName(path)}");
        if (!sawColor) cols.Clear();
        return new CloudPayload
        {
            Id = Path.GetFileNameWithoutExtension(path),
            Count = pts.Count / 3,
            Points = pts.ToArray(),
            ColorsRgb = sawColor ? cols.ToArray() : null,
            Source = "导入",
        };
    }

    /// <summary>PLY 导入：ascii 与 binary_little_endian；顶点属性取 x/y/z（float/double）+ 可选 red/green/blue（uchar）。
    /// 只读第一个 vertex element（faces 等后续 element 忽略）。
    /// ★ binary 数据起点必须字节级定位：StreamReader 有内部缓冲，fs.Position 会越过 end_header 实际
    /// 结束位置（数据整体错位）——header 手动逐字节读（ReadHeaderLine），数据段紧随其后的 \n。</summary>
    public static CloudPayload LoadPly(string path)
    {
        using var fs = File.OpenRead(path);
        string? fmt = null;
        int vertexCount = 0;
        var props = new List<(string Name, string Type)>();
        bool inVertex = false, headerDone = false;
        string? line;
        while ((line = ReadHeaderLine(fs)) is not null)
        {
            var t = line.Trim();
            if (t.StartsWith("format", StringComparison.Ordinal)) fmt = t;
            else if (t.StartsWith("element", StringComparison.Ordinal))
            {
                var ps = t.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                inVertex = ps.Length >= 3 && ps[1].Equals("vertex", StringComparison.OrdinalIgnoreCase);
                if (inVertex && ps[2].Length > 0) vertexCount = int.Parse(ps[2], CultureInfo.InvariantCulture);
            }
            else if (t.StartsWith("property", StringComparison.Ordinal) && inVertex)
            {
                var ps = t.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (ps.Length >= 3) props.Add((ps[^1], ps[1]));   // (name, type)
            }
            else if (t.StartsWith("end_header", StringComparison.Ordinal)) { headerDone = true; break; }
        }
        if (!headerDone || vertexCount <= 0 || fmt is null)
            throw new Exception($"PLY 头不完整或不支持：{Path.GetFileName(path)}");

        int xi = -1, yi = -1, zi = -1, ri = -1, gi = -1, bi = -1;
        for (int i = 0; i < props.Count; i++)
        {
            switch (props[i].Name.ToLowerInvariant())
            {
                case "x": xi = i; break;
                case "y": yi = i; break;
                case "z": zi = i; break;
                case "red": ri = i; break;
                case "green": gi = i; break;
                case "blue": bi = i; break;
            }
        }
        if (xi < 0 || yi < 0 || zi < 0)
            throw new Exception($"PLY 缺 x/y/z 顶点属性：{Path.GetFileName(path)}");

        var pts = new float[vertexCount * 3];
        byte[]? cols = ri >= 0 && gi >= 0 && bi >= 0 ? new byte[vertexCount * 3] : null;
        bool binary = fmt.Contains("binary_little_endian", StringComparison.OrdinalIgnoreCase);
        if (binary)
        {
            // end_header 行尾换行已由 ReadHeaderLine 精确吃掉——数据紧随其后，不可再读（会丢首字节错位）
            var sizes = props.Select(p => TypeSize(p.Type)).ToArray();
            int stride = sizes.Sum();
            var rec = new byte[stride];
            for (int v = 0; v < vertexCount; v++)
            {
                if (ReadFull(fs, rec) < stride)
                    throw new Exception($"PLY 数据不足（第 {v} 点截断）：{Path.GetFileName(path)}");
                // 属性索引 → 字节偏移（前缀和）：GetF 的第一参必须是字节偏移而非属性索引
                var offs = new int[props.Count];
                for (int i = 1; i < props.Count; i++) offs[i] = offs[i - 1] + sizes[i - 1];
                float GetF(int propIdx) => ReadFloatAt(rec, offs[propIdx], props[propIdx].Type, sizes[propIdx]);
                pts[v * 3] = GetF(xi); pts[v * 3 + 1] = GetF(yi); pts[v * 3 + 2] = GetF(zi);
                for (int i = 0; i < props.Count; i++)
                {
                    if (i == ri) cols![v * 3] = rec[offs[i]];
                    else if (i == gi) cols![v * 3 + 1] = rec[offs[i]];
                    else if (i == bi) cols![v * 3 + 2] = rec[offs[i]];
                }
            }
        }
        else
        {
            // ascii：从 header 后逐行读 token 流（属性可能跨行——按 token 队列消费）
            var rest = new StreamReader(fs).ReadToEnd();   // 数据段：ReadHeaderLine 后 fs.Position 精确指向首个数据字节
            var tokens = rest.Split(new[] { ' ', '\r', '\n', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            int ti = 0;
            for (int v = 0; v < vertexCount; v++)
            {
                var vals = new string[props.Count];
                for (int i = 0; i < props.Count; i++)
                {
                    if (ti >= tokens.Length) throw new Exception($"PLY ascii 数据不足：{Path.GetFileName(path)}");
                    vals[i] = tokens[ti++];
                }
                pts[v * 3] = float.Parse(vals[xi], CultureInfo.InvariantCulture);
                pts[v * 3 + 1] = float.Parse(vals[yi], CultureInfo.InvariantCulture);
                pts[v * 3 + 2] = float.Parse(vals[zi], CultureInfo.InvariantCulture);
                if (cols is not null)
                {
                    cols[v * 3] = byte.Parse(vals[ri], CultureInfo.InvariantCulture);
                    cols[v * 3 + 1] = byte.Parse(vals[gi], CultureInfo.InvariantCulture);
                    cols[v * 3 + 2] = byte.Parse(vals[bi], CultureInfo.InvariantCulture);
                }
            }
        }
        return new CloudPayload
        {
            Id = Path.GetFileNameWithoutExtension(path),
            Count = vertexCount,
            Points = pts,
            ColorsRgb = cols,
            Source = "导入",
        };
    }

    /// <summary>字节级读 PLY header 一行（CRLF/LF 都吃掉；返回后流位置精确停在下一行首——binary 数据起点可靠）。</summary>
    private static string? ReadHeaderLine(FileStream fs)
    {
        var buf = new List<byte>(128);
        int b;
        while ((b = fs.ReadByte()) >= 0)
        {
            if (b == 0x0A) return System.Text.Encoding.ASCII.GetString(buf.ToArray());
            if (b == 0x0D) continue;   // CRLF：吃 CR 等 LF
            buf.Add((byte)b);
        }
        return buf.Count == 0 ? null : System.Text.Encoding.ASCII.GetString(buf.ToArray());
    }

    private static int TypeSize(string t) => t.ToLowerInvariant() switch
    {
        "float" or "float32" => 4,
        "double" or "float64" => 8,
        "int" or "int32" => 4,
        "uint" or "uint32" => 4,
        "short" or "int16" => 2,
        "ushort" or "uint16" => 2,
        "char" or "int8" or "uchar" or "uint8" => 1,
        _ => throw new Exception($"不支持的 PLY 属性类型 {t}"),
    };

    private static float ReadFloatAt(byte[] rec, int off, string type, int size) => size switch
    {
        4 => BitConverter.ToSingle(rec, off),
        8 => (float)BitConverter.ToDouble(rec, off),
        _ => throw new Exception("坐标属性必须是 float/double"),
    };

    private static int ReadFull(FileStream fs, byte[] buf)
    {
        int total = 0;
        while (total < buf.Length)
        {
            int n = fs.Read(buf, total, buf.Length - total);
            if (n <= 0) break;
            total += n;
        }
        return total;
    }

    // ==================== 导出 ====================

    /// <summary>按扩展名导出：.ply（ascii，含 RGB 与可选法线）、.las（1.2）与 .xyz/.txt/.csv（x y z [r g b]）。</summary>
    public static void SaveTo(string path, CloudPayload p, float[]? normals = null)
    {
        var ext = Path.GetExtension(path).ToLowerInvariant();
        if (ext == ".ply") SavePly(path, p, normals);
        else if (ext == ".las") CloudLas.Save(path, p);
        else SaveXyz(path, p);
    }

    private static void SaveXyz(string path, CloudPayload p)
    {
        using var w = new StreamWriter(path);
        bool hasCol = p.ColorsRgb is not null && p.ColorsRgb.Length >= p.Count * 3;
        for (int i = 0; i < p.Count; i++)
        {
            if (hasCol)
                w.WriteLine(FormattableString.Invariant(
                    $"{p.Points[i * 3]:R} {p.Points[i * 3 + 1]:R} {p.Points[i * 3 + 2]:R} {p.ColorsRgb![i * 3]} {p.ColorsRgb[i * 3 + 1]} {p.ColorsRgb[i * 3 + 2]}"));
            else
                w.WriteLine(FormattableString.Invariant(
                    $"{p.Points[i * 3]:R} {p.Points[i * 3 + 1]:R} {p.Points[i * 3 + 2]:R}"));
        }
    }

    private static void SavePly(string path, CloudPayload p, float[]? normals = null)
    {
        using var w = new StreamWriter(path);
        bool hasCol = p.ColorsRgb is not null && p.ColorsRgb.Length >= p.Count * 3;
        bool hasNrm = normals is not null && normals.Length >= p.Count * 3;
        w.WriteLine("ply");
        w.WriteLine("format ascii 1.0");
        w.WriteLine("comment generated by ComUI 3D workbench");
        w.WriteLine($"element vertex {p.Count}");
        w.WriteLine("property float x");
        w.WriteLine("property float y");
        w.WriteLine("property float z");
        if (hasCol)
        {
            w.WriteLine("property uchar red");
            w.WriteLine("property uchar green");
            w.WriteLine("property uchar blue");
        }
        if (hasNrm)
        {
            w.WriteLine("property float nx");
            w.WriteLine("property float ny");
            w.WriteLine("property float nz");
        }
        w.WriteLine("end_header");
        for (int i = 0; i < p.Count; i++)
        {
            var baseLine = FormattableString.Invariant(
                $"{p.Points[i * 3]:R} {p.Points[i * 3 + 1]:R} {p.Points[i * 3 + 2]:R}");
            if (hasCol)
                baseLine += FormattableString.Invariant(
                    $" {p.ColorsRgb![i * 3]} {p.ColorsRgb[i * 3 + 1]} {p.ColorsRgb[i * 3 + 2]}");
            if (hasNrm)
                baseLine += FormattableString.Invariant(
                    $" {normals![i * 3]:R} {normals[i * 3 + 1]:R} {normals[i * 3 + 2]:R}");
            w.WriteLine(baseLine);
        }
    }

    // ==================== 基础处理 ====================

    /// <summary>随机下采样：目标点数 = ceil(count × ratio)。均匀随机近似——步长分桶每桶随机取 1 点（O(k)，不整排索引）。</summary>
    public static CloudPayload RandomSample(CloudPayload src, double ratio)
    {
        if (ratio >= 1.0 || ratio <= 0) return src;
        int target = Math.Max(1, (int)Math.Ceiling(src.Count * ratio));
        var rng = new Random(unchecked((int)DateTime.Now.Ticks));
        int step = Math.Max(1, src.Count / target);
        var pts = new float[target * 3];
        byte[]? cols = src.ColorsRgb is not null && src.ColorsRgb.Length >= src.Count * 3 ? new byte[target * 3] : null;
        int k = 0;
        for (int start = 0; start < src.Count && k < target; start += step)
        {
            int span = Math.Min(step, src.Count - start);
            int i = start + rng.Next(span);
            pts[k * 3] = src.Points[i * 3];
            pts[k * 3 + 1] = src.Points[i * 3 + 1];
            pts[k * 3 + 2] = src.Points[i * 3 + 2];
            if (cols is not null)
            {
                cols[k * 3] = src.ColorsRgb![i * 3];
                cols[k * 3 + 1] = src.ColorsRgb[i * 3 + 1];
                cols[k * 3 + 2] = src.ColorsRgb[i * 3 + 2];
            }
            k++;
        }
        return new CloudPayload { Id = src.Id, Count = k, Points = pts, ColorsRgb = cols, Source = src.Source };
    }

    /// <summary>半径离群滤波（SOR 的格网加速等价实现）：邻域内点数（含自身）≥ minNeighbors 保留。
    /// 大数据建议先体素/随机抽稀再滤波（格网查询 O(n·邻域桶)）。</summary>
    public static (CloudPayload Kept, int Removed) StatisticOutlierRemove(CloudPayload src, double radius, int minNeighbors)
    {
        int n = src.Count;
        if (n == 0 || radius <= 0) return (src, 0);
        double r2 = radius * radius;

        // 点 → 格网桶（桶边 = radius，邻域只需查 3³=27 桶）
        var grid = new Dictionary<(int, int, int), List<int>>(n / 2 + 1);
        for (int i = 0; i < n; i++)
        {
            var key = ((int)Math.Floor(src.Points[i * 3] / radius),
                       (int)Math.Floor(src.Points[i * 3 + 1] / radius),
                       (int)Math.Floor(src.Points[i * 3 + 2] / radius));
            if (!grid.TryGetValue(key, out var list)) grid[key] = list = new List<int>(4);
            list.Add(i);
        }

        var keptPts = new List<float>(n * 3);
        var keptCols = src.ColorsRgb is not null && src.ColorsRgb.Length >= n * 3 ? new List<byte>(n * 3) : null;
        int removed = 0;
        for (int i = 0; i < n; i++)
        {
            double x = src.Points[i * 3], y = src.Points[i * 3 + 1], z = src.Points[i * 3 + 2];
            var cell = ((int)Math.Floor(x / radius), (int)Math.Floor(y / radius), (int)Math.Floor(z / radius));
            int cnt = 0;
            for (int dz = -1; dz <= 1 && cnt < minNeighbors; dz++)
                for (int dy = -1; dy <= 1 && cnt < minNeighbors; dy++)
                    for (int dx = -1; dx <= 1 && cnt < minNeighbors; dx++)
                    {
                        if (!grid.TryGetValue((cell.Item1 + dx, cell.Item2 + dy, cell.Item3 + dz), out var list)) continue;
                        foreach (var j in list)
                        {
                            if (j == i) continue;
                            double ddx = src.Points[j * 3] - x, ddy = src.Points[j * 3 + 1] - y, ddz = src.Points[j * 3 + 2] - z;
                            if (ddx * ddx + ddy * ddy + ddz * ddz <= r2 && ++cnt >= minNeighbors) break;
                        }
                    }
            if (cnt < minNeighbors) { removed++; continue; }
            keptPts.Add(src.Points[i * 3]); keptPts.Add(src.Points[i * 3 + 1]); keptPts.Add(src.Points[i * 3 + 2]);
            keptCols?.Add(src.ColorsRgb![i * 3]); keptCols?.Add(src.ColorsRgb![i * 3 + 1]); keptCols?.Add(src.ColorsRgb![i * 3 + 2]);
        }
        return (new CloudPayload
        {
            Id = src.Id,
            Count = keptPts.Count / 3,
            Points = keptPts.ToArray(),
            ColorsRgb = keptCols?.ToArray(),
            Source = src.Source,
        }, removed);
    }

    /// <summary>RANSAC 平面分割：随机三点采样拟合平面，取内点（|距离| ≤ distThreshold）最多的平面。
    /// 返回（平面点集, 残差点集, 内点数）——迭代 O(iterations·n)，大数据建议先抽稀。纯函数可测试。</summary>
    public static (CloudPayload PlanePts, CloudPayload RestPts, int Inliers) RansacPlaneSplit(
        CloudPayload src, double distThreshold, int iterations)
    {
        int n = src.Count;
        if (n < 3 || distThreshold <= 0) throw new Exception("平面分割需要 ≥3 个点且距离阈值 > 0");
        iterations = Math.Clamp(iterations, 1, 10000);
        var rng = new Random(20261118);   // 固定种子：同一输入结果可复现

        int bestCount = -1;
        (double a, double b, double c, double d) bestPlane = default;
        for (int it = 0; it < iterations; it++)
        {
            int i0 = rng.Next(n), i1 = rng.Next(n), i2 = rng.Next(n);
            if (i0 == i1 || i1 == i2 || i0 == i2) continue;
            double ax = src.Points[i0 * 3], ay = src.Points[i0 * 3 + 1], az = src.Points[i0 * 3 + 2];
            double bx = src.Points[i1 * 3], by = src.Points[i1 * 3 + 1], bz = src.Points[i1 * 3 + 2];
            double cx = src.Points[i2 * 3], cy = src.Points[i2 * 3 + 1], cz = src.Points[i2 * 3 + 2];
            // 平面法向量 = (b-a)×(c-a)
            double ux = bx - ax, uy = by - ay, uz = bz - az;
            double vx = cx - ax, vy = cy - ay, vz = cz - az;
            double a = uy * vz - uz * vy, b2 = uz * vx - ux * vz, c2 = ux * vy - uy * vx;
            double len = Math.Sqrt(a * a + b2 * b2 + c2 * c2);
            if (len < 1e-12) continue;   // 三点共线退化 → 重采样
            a /= len; b2 /= len; c2 /= len;
            double d = -(a * ax + b2 * ay + c2 * az);

            int cnt = 0;
            double t2 = distThreshold;
            for (int i = 0; i < n; i++)
            {
                double dist = Math.Abs(a * src.Points[i * 3] + b2 * src.Points[i * 3 + 1] + c2 * src.Points[i * 3 + 2] + d);
                if (dist <= t2) cnt++;
            }
            if (cnt > bestCount) { bestCount = cnt; bestPlane = (a, b2, c2, d); }
        }
        if (bestCount < 3) throw new Exception("平面分割失败：迭代内未找到 ≥3 内点的平面（检查阈值/点云形态）");

        var planePts = new List<float>(bestCount * 3);
        var restPts = new List<float>((n - bestCount) * 3);
        var planeCols = src.ColorsRgb is not null ? new List<byte>(bestCount * 3) : null;
        var restCols = src.ColorsRgb is not null ? new List<byte>((n - bestCount) * 3) : null;
        for (int i = 0; i < n; i++)
        {
            double dist = Math.Abs(bestPlane.a * src.Points[i * 3] + bestPlane.b * src.Points[i * 3 + 1] + bestPlane.c * src.Points[i * 3 + 2] + bestPlane.d);
            bool inlier = dist <= distThreshold;
            var (ptList, colList) = inlier ? (planePts, planeCols) : (restPts, restCols);
            ptList.Add(src.Points[i * 3]); ptList.Add(src.Points[i * 3 + 1]); ptList.Add(src.Points[i * 3 + 2]);
            colList?.Add(src.ColorsRgb![i * 3]); colList?.Add(src.ColorsRgb![i * 3 + 1]); colList?.Add(src.ColorsRgb![i * 3 + 2]);
        }
        var plane = new CloudPayload { Id = src.Id, Count = planePts.Count / 3, Points = planePts.ToArray(), ColorsRgb = planeCols?.ToArray(), Source = src.Source };
        var rest = new CloudPayload { Id = src.Id, Count = restPts.Count / 3, Points = restPts.ToArray(), ColorsRgb = restCols?.ToArray(), Source = src.Source };
        return (plane, rest, bestCount);
    }

    /// <summary>体素下采样（确定性：每体素取首点，同输入同输出）。voxel<=0 抛错。</summary>
    public static (CloudPayload Kept, int KeptCount) VoxelDownsample(CloudPayload src, double voxel)
    {
        if (voxel <= 0) throw new Exception("体素尺寸必须 > 0");
        int n = src.Count;
        var seen = new HashSet<(int, int, int)>();
        var pts = new List<float>(n / 4 * 3);
        var cols = src.ColorsRgb is not null && src.ColorsRgb.Length >= n * 3 ? new List<byte>(n / 4 * 3) : null;
        int kept = 0;
        for (int i = 0; i < n; i++)
        {
            var key = ((int)Math.Floor(src.Points[i * 3] / voxel),
                       (int)Math.Floor(src.Points[i * 3 + 1] / voxel),
                       (int)Math.Floor(src.Points[i * 3 + 2] / voxel));
            if (!seen.Add(key)) continue;
            pts.Add(src.Points[i * 3]); pts.Add(src.Points[i * 3 + 1]); pts.Add(src.Points[i * 3 + 2]);
            if (cols is not null)
            {
                cols.Add(src.ColorsRgb![i * 3]); cols.Add(src.ColorsRgb[i * 3 + 1]); cols.Add(src.ColorsRgb[i * 3 + 2]);
            }
            kept++;
        }
        return (new CloudPayload { Id = src.Id, Count = kept, Points = pts.ToArray(), ColorsRgb = cols?.ToArray(), Source = src.Source }, kept);
    }

    /// <summary>合并多片点云（坐标已是同一坐标系下的最终值——调用方负责先过实体变换）：颜色任一片缺失 → 整体无色。</summary>
    public static CloudPayload Merge(IReadOnlyList<CloudPayload> clouds)
    {
        if (clouds.Count == 0) throw new Exception("无可合并的实体");
        long total = 0;
        bool allColored = clouds.All(c => c.ColorsRgb is not null && c.ColorsRgb.Length >= c.Count * 3);
        foreach (var c in clouds) total += c.Count;
        if (total > int.MaxValue / 3) throw new Exception($"合并后点数超出单实体上限（{total:N0} 点）——先抽稀再合并");
        var pts = new float[total * 3];
        byte[]? cols = allColored && total > 0 ? new byte[total * 3] : null;
        long off = 0;
        foreach (var c in clouds)
        {
            Array.Copy(c.Points, 0, pts, off, (long)c.Count * 3);
            if (cols is not null) Array.Copy(c.ColorsRgb!, 0, cols, off, (long)c.Count * 3);
            off += (long)c.Count * 3;
        }
        return new CloudPayload
        {
            Id = string.Join("+", clouds.Select(c => c.Id)),
            Count = (int)total,
            Points = pts,
            ColorsRgb = cols,
            Source = "合并",
        };
    }
}
