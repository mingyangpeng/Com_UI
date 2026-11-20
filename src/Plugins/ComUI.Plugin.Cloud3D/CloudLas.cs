using System.Globalization;
using System.IO;
using ComUI.Sdk;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// LAS 1.2 点云格式导入/导出（纯手写零依赖）：point format 0（xyz+intensity）与 2（xyz+intensity+RGB）。
/// scale=0.001（μmm 级）、offset=各轴包围盒最小值（自动，int32 全域覆盖）；导入读回 scale/offset 还原坐标。
/// 对应 CloudCompare 的 LAS File I/O（主流测量交付格式）。
/// </summary>
public static class CloudLas
{
    private const int HeaderSize = 227;
    private const double Scale = 0.001;

    /// <summary>LAS 导入（format 0/2；1.4 format 6+ 不支持——抛错提示转 PLY/XYZ）。</summary>
    public static CloudPayload Load(string path)
    {
        using var fs = File.OpenRead(path);
        var header = new byte[HeaderSize];
        if (ReadFull(fs, header) < HeaderSize)
            throw new Exception($"LAS 头不完整（< 227 字节）：{Path.GetFileName(path)}");
        if (header[0] != (byte)'L' || header[0 + 1] != (byte)'A' || header[2] != (byte)'S' || header[3] != (byte)'F')
            throw new Exception($"不是 LAS 文件（缺 LASF 魔数）：{Path.GetFileName(path)}");
        int major = header[4], minor = header[5];
        byte fmt = header[84];
        int recLen = BitConverter.ToUInt16(header, 85);
        uint count = BitConverter.ToUInt32(header, 87);
        uint dataOffset = BitConverter.ToUInt32(header, 76);
        if (major != 1 || minor > 3)
            throw new Exception($"不支持的 LAS 版本 {major}.{minor}（支持 1.0~1.3）");
        if (fmt != 0 && fmt != 2)
            throw new Exception($"不支持的 point format {fmt}（支持 0=xyz+intensity / 2=+RGB）；其余格式请转 PLY/XYZ 导入");

        // LAS 1.2 字段偏移：legacy count(87-90) + by return×5(91-110) → scale(111/119/127) → offset(135/143/151)
        double sx = BitConverter.ToDouble(header, 111);
        double sy = BitConverter.ToDouble(header, 119);
        double sz = BitConverter.ToDouble(header, 127);
        double ox = BitConverter.ToDouble(header, 135);
        double oy = BitConverter.ToDouble(header, 143);
        double oz = BitConverter.ToDouble(header, 151);
        if (sx <= 0 || sy <= 0 || sz <= 0) throw new Exception("LAS scale factor 非法（≤0）");

        bool hasColor = fmt == 2;
        var pts = new float[count * 3];
        var cols = hasColor ? new byte[count * 3] : null;

        fs.Seek(dataOffset, SeekOrigin.Begin);
        var rec = new byte[recLen];
        for (uint i = 0; i < count; i++)
        {
            if (ReadFull(fs, rec) < recLen)
                throw new Exception($"LAS 数据不足（第 {i + 1} 点截断）：{Path.GetFileName(path)}");
            int x = BitConverter.ToInt32(rec, 0);
            int y = BitConverter.ToInt32(rec, 4);
            int z = BitConverter.ToInt32(rec, 8);
            pts[i * 3] = (float)(x * sx + ox);
            pts[i * 3 + 1] = (float)(y * sy + oy);
            pts[i * 3 + 2] = (float)(z * sz + oz);
            if (hasColor)
            {
                // format 2 RGB 位于记录尾部偏移 20（format 0 长度），ushort 高位为色值约定但多数工具存低 8 位——按 >>8 取高字节（LAS 规范色值占 ushort 全程）
                cols![i * 3] = (byte)(BitConverter.ToUInt16(rec, 20) >> 8);
                cols[i * 3 + 1] = (byte)(BitConverter.ToUInt16(rec, 22) >> 8);
                cols[i * 3 + 2] = (byte)(BitConverter.ToUInt16(rec, 24) >> 8);
            }
        }
        return new CloudPayload
        {
            Id = Path.GetFileNameWithoutExtension(path),
            Count = (int)Math.Min(count, int.MaxValue),
            Points = pts,
            ColorsRgb = cols,
            Source = "导入",
        };
    }

    /// <summary>LAS 1.2 导出：有 RGB → format 2，无 RGB → format 0；offset=各轴最小值、scale=0.001。</summary>
    public static void Save(string path, CloudPayload p)
    {
        if (p.Count == 0) throw new Exception("点云为空，无法导出 LAS");
        double minx = double.MaxValue, miny = double.MaxValue, minz = double.MaxValue;
        double maxx = double.MinValue, maxy = double.MinValue, maxz = double.MinValue;
        for (int i = 0; i < p.Count; i++)
        {
            double x = p.Points[i * 3], y = p.Points[i * 3 + 1], z = p.Points[i * 3 + 2];
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
            if (z < minz) minz = z;
            if (z > maxz) maxz = z;
        }
        // int32 覆盖检查（scale 0.001 → 范围 ±2147 万 mm）
        double lim = int.MaxValue * Scale;
        if (maxx - minx > 2 * lim || maxy - miny > 2 * lim || maxz - minz > 2 * lim)
            throw new Exception($"LAS scale=0.001 下坐标范围超 int32（{maxx - minx:F0}mm）——先抽稀或分区导出");

        bool hasColor = p.ColorsRgb is not null && p.ColorsRgb.Length >= p.Count * 3;
        byte fmt = hasColor ? (byte)2 : (byte)0;
        ushort recLen = hasColor ? (ushort)26 : (ushort)20;
        uint count = (uint)p.Count;

        using var w = new BinaryWriter(File.Create(path));
        // ===== header（227 字节）=====
        w.Write(new byte[] { (byte)'L', (byte)'A', (byte)'S', (byte)'F' });
        w.Write((byte)1); w.Write((byte)2);                       // version 1.2
        var sysId = new byte[32];                                 // system identifier（零填充）
        System.Text.Encoding.ASCII.GetBytes("ComUI".AsSpan(), sysId);
        w.Write(sysId);
        var swId = new byte[32];
        System.Text.Encoding.ASCII.GetBytes("ComUI 3D workbench".AsSpan(), swId);
        w.Write(swId);
        w.Write((ushort)1); w.Write((ushort)2026);                // creation day-of-year / year（占位）
        w.Write((ushort)HeaderSize);                              // header size
        w.Write((uint)(HeaderSize));                              // offset to point data（无 VLR）
        w.Write((uint)0);                                         // VLR count
        w.Write(fmt);                                             // point format
        w.Write(recLen);
        w.Write(count);                                           // legacy point count
        for (int i = 0; i < 5; i++) w.Write(count);               // points by return（占位全量）
        w.Write(Scale); w.Write(Scale); w.Write(Scale);           // scale factors
        w.Write(minx); w.Write(miny); w.Write(minz);              // offsets
        w.Write(maxx); w.Write(minx);                             // max/min X
        w.Write(maxy); w.Write(miny);                             // max/min Y
        w.Write(maxz); w.Write(minz);                             // max/min Z
        w.Write(new byte[HeaderSize - 207]);                      // 207-226 保留（LAS 1.2：207=写完 max/min 后的位置）
        // ===== point records =====
        for (int i = 0; i < p.Count; i++)
        {
            int ix = (int)Math.Round((p.Points[i * 3] - minx) / Scale);
            int iy = (int)Math.Round((p.Points[i * 3 + 1] - miny) / Scale);
            int iz = (int)Math.Round((p.Points[i * 3 + 2] - minz) / Scale);
            w.Write(ix); w.Write(iy); w.Write(iz);
            w.Write((ushort)0);                                   // intensity
            w.Write((byte)0);                                     // flags
            w.Write((byte)0);                                     // classification
            w.Write((byte)0);                                     // scan angle
            w.Write((byte)0);                                     // user data
            w.Write((ushort)0);                                   // point source id
            if (hasColor)
            {
                // LAS 规范：RGB 存 ushort 高 8 位有效——写入时左移 8 位保色值精度
                w.Write((ushort)(p.ColorsRgb![i * 3] << 8));
                w.Write((ushort)(p.ColorsRgb[i * 3 + 1] << 8));
                w.Write((ushort)(p.ColorsRgb[i * 3 + 2] << 8));
            }
        }
    }

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
}
