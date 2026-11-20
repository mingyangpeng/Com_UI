using OpenTK.Mathematics;

namespace ComUI.Plugin.Cloud3D;

/// <summary>选区纯函数（可测试）：全选/反选的字节掩码生成。</summary>
public static class CloudSelection
{
    /// <summary>全选掩码（loaded 个 255）。</summary>
    public static byte[] SelectAll(int loaded)
    {
        if (loaded < 0) throw new ArgumentOutOfRangeException(nameof(loaded));
        var r = new byte[loaded];
        Array.Fill(r, (byte)255);
        return r;
    }

    /// <summary>反选（返回新掩码：255↔0）。返回选中数。</summary>
    public static byte[] Invert(byte[] sel, int loaded, out int selected)
    {
        var r = new byte[loaded];
        int n = 0;
        for (int i = 0; i < loaded; i++)
        {
            r[i] = sel[i] == 255 ? (byte)0 : (byte)255;
            if (r[i] == 255) n++;
        }
        selected = n;
        return r;
    }
}


/// <summary>相机步进纯函数（可测试）：与拖拽 orbit 同公式（pitch 绕当前 right 轴），锁定竖直轴时忽略 pitch。</summary>
public static class CloudCamera
{
    public static Quaternion NudgeOrientation(
        Quaternion current, double yawDeg, double pitchDeg, bool lockPitch = false)
    {
        var qYaw = Quaternion.FromAxisAngle(
            Vector3.UnitY, (float)(yawDeg * Math.PI / 180.0));
        if (lockPitch) return Quaternion.Normalize(qYaw * current);
        var right = Vector3.Transform(Vector3.UnitX, current);
        var qPitch = Quaternion.FromAxisAngle(
            right, (float)(pitchDeg * Math.PI / 180.0));
        return Quaternion.Normalize(qPitch * qYaw * current);
    }
}
