using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace ComUI.Sdk.Native;

/// <summary>
/// 原生算子统一 C ABI（「JSON + C++ DLL 即插即用」的调用契约，宿主按 JSON 声明自动 marshal）。
/// 每个算子导出**一个统一签名入口**（JSON 的 method 指名），无需逐函数签名生成绑定：
///
/// <code>
/// extern "C" IMGIO_API int32_t &lt;method&gt;(const AlgoCInput* in, AlgoCOutput* out);
/// </code>
///
/// 约定：
/// - 返回 0 = 成功；非 0 = 失败（配合 out-&gt;error，UTF-8，DLL 静态存储）；
/// - **输入**由宿主构建（参数/图像/点云，DLL 只读；文本参数 UTF-8，调用期间有效）；
/// - **输出**缓冲由 DLL 分配，宿主拷贝成托管载荷后调 DLL 导出的 <c>algo_free(void*)</c>
///   逐指针释放（释放：out.images 数组、每个 images[i].bgra、out.clouds 数组、每个 xyz/rgb；
///   error 指针为 DLL 静态存储，宿主不释放）；
/// - 数据布局沿用平台契约：图像 = 紧排 BGRA（OpenCV CV_8UC4 模式），点云 = SoA（PCL 模式）。
/// C 头文件模板见 docs/THIRD_PARTY_OPS.md §5.5。
/// </summary>
public static class NativeAlgoAbi
{
    // AlgoCParam.kind：参数种类
    public const int KindDouble = 0;
    public const int KindInt32 = 1;
    public const int KindText = 2;    // const char*（UTF-8）
    public const int KindBool = 3;    // I != 0

    /// <summary>用户参数（宿主按 JSON 声明顺序构建，DLL 只读）。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct AlgoCParam
    {
        public int Kind;
        public double D;
        public int I;
        public IntPtr S;   // const char*（UTF-8，宿主分配，调用期间有效）
    }

    /// <summary>输入图像（紧排 BGRA，宿主 pin，调用期间有效）。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct AlgoCImage
    {
        public int Width;
        public int Height;
        public IntPtr Bgra;
    }

    /// <summary>输入点云（SoA：xyz 连续 float；rgb 可空）。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct AlgoCCloud
    {
        public int Count;
        public IntPtr Xyz;
        public IntPtr Rgb;
    }

    /// <summary>调用输入（宿主构建）。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct AlgoCInput
    {
        public int ParamCount;
        public IntPtr Params;    // const AlgoCParam[]
        public int ImageCount;
        public IntPtr Images;    // const AlgoCImage[]
        public int CloudCount;
        public IntPtr Clouds;    // const AlgoCCloud[]
    }

    /// <summary>输出图像（DLL 分配 bgra）。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct AlgoCImageOut
    {
        public int Width;
        public int Height;
        public IntPtr Bgra;
    }

    /// <summary>输出点云（DLL 分配 xyz/rgb，rgb 可空）。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct AlgoCCloudOut
    {
        public int Count;
        public IntPtr Xyz;
        public IntPtr Rgb;
    }

    /// <summary>调用输出（DLL 填写；数组与数据缓冲均 DLL 分配，宿主拷贝后经 algo_free 释放）。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct AlgoCOutput
    {
        public int ImageCount;
        public IntPtr Images;   // AlgoCImageOut*（可空=0 个）
        public int CloudCount;
        public IntPtr Clouds;   // AlgoCCloudOut*（可空=0 个）
        public IntPtr Error;    // const char*（DLL 静态存储，宿主只读不释放）
    }

    /// <summary>统一入口签名：int32 method(const AlgoCInput*, AlgoCOutput*)。</summary>
    public delegate int AlgoMainDelegate(IntPtr input, IntPtr output);

    /// <summary>释放约定签名：void algo_free(void*)（DLL 必须导出；缺省时宿主跳过释放并记一次警告）。</summary>
    public delegate void AlgoFreeDelegate(IntPtr p);
}

/// <summary>
/// 宿主侧通用原生调用器：把托管参数/图像/点云 marshal 成 C ABI、调用导出函数、
/// 把输出拷回托管载荷、按约定调 algo_free 释放 DLL 侧缓冲。
/// 由宿主在**插件加载时**按 JSON 声明创建（= 自动生成的调用壳），第三方零 C# 代码。
/// 线程安全：可在任意后台线程调用（无 UI 依赖）。
/// </summary>
public sealed class NativeAlgoInvoker
{
    private readonly NativeAlgoAbi.AlgoMainDelegate _main;
    private readonly NativeAlgoAbi.AlgoFreeDelegate? _free;
    private readonly string _entry;

    /// <summary>从 DLL 加载入口（algo_free 缺省时为 null，输出缓冲将不释放——DLL 按约定必须导出）。</summary>
    public NativeAlgoInvoker(string dllPath, string entry)
    {
        _entry = entry;
        var lib = NativeLibrary.Load(dllPath);
        var mainPtr = NativeLibrary.GetExport(lib, entry);
        _main = Marshal.GetDelegateForFunctionPointer<NativeAlgoAbi.AlgoMainDelegate>(mainPtr);
        if (NativeLibrary.TryGetExport(lib, "algo_free", out var freePtr))
            _free = Marshal.GetDelegateForFunctionPointer<NativeAlgoAbi.AlgoFreeDelegate>(freePtr);
    }

    /// <summary>委托注入构造（测试/进程内模拟原生实现用——签名须遵守统一 C ABI）。</summary>
    public NativeAlgoInvoker(NativeAlgoAbi.AlgoMainDelegate main, NativeAlgoAbi.AlgoFreeDelegate? free, string entry)
    {
        _main = main;
        _free = free;
        _entry = entry;
    }

    /// <summary>执行一次调用。参数按 JSON 声明顺序（CLR 类型映射：double/float→double、int/long→int32、
    /// string→UTF-8 文本、bool→bool）；输入/输出为图像与点云载荷。失败抛带 DLL 错误消息的异常。</summary>
    public (List<ImagePayload> Images, List<CloudPayload> Clouds) Run(
        IReadOnlyList<object?>? parameters,
        IReadOnlyList<ImagePayload>? inputImages,
        IReadOnlyList<CloudPayload>? inputClouds)
    {
        var pins = new List<GCHandle>();
        var allocs = new List<IntPtr>();   // 宿主侧 HGlobal（调用后统一释放）
        try
        {
            var input = BuildInput(parameters, inputImages, inputClouds, pins, allocs);
            var outputPtr = Marshal.AllocHGlobal(Marshal.SizeOf<NativeAlgoAbi.AlgoCOutput>());
            allocs.Add(outputPtr);
            ZeroStruct(outputPtr, Marshal.SizeOf<NativeAlgoAbi.AlgoCOutput>());

            int rc = _main(input, outputPtr);
            var output = Marshal.PtrToStructure<NativeAlgoAbi.AlgoCOutput>(outputPtr);
            if (rc != 0)
            {
                var err = output.Error != IntPtr.Zero ? Marshal.PtrToStringUTF8(output.Error) : null;
                throw new Exception($"原生算子 {_entry} 失败（返回码 {rc}）: {err ?? "（DLL 未提供错误消息）"}");
            }
            var result = ReadOutput(ref output);
            FreeDllBuffers(ref output);
            return result;
        }
        finally
        {
            foreach (var p in pins) p.Free();
            foreach (var a in allocs) Marshal.FreeHGlobal(a);
        }
    }

    // ==================== marshal：托管 → C ====================

    private static IntPtr BuildInput(IReadOnlyList<object?>? parameters,
        IReadOnlyList<ImagePayload>? images, IReadOnlyList<CloudPayload>? clouds,
        List<GCHandle> pins, List<IntPtr> allocs)
    {
        // 参数数组
        IntPtr paramsPtr = IntPtr.Zero;
        int paramCount = parameters?.Count ?? 0;
        if (paramCount > 0)
        {
            var size = Marshal.SizeOf<NativeAlgoAbi.AlgoCParam>();
            paramsPtr = Marshal.AllocHGlobal(paramCount * size);
            allocs.Add(paramsPtr);
            for (int i = 0; i < paramCount; i++)
            {
                var p = new NativeAlgoAbi.AlgoCParam();
                switch (parameters![i])
                {
                    case double d: p.Kind = NativeAlgoAbi.KindDouble; p.D = d; break;
                    case float f: p.Kind = NativeAlgoAbi.KindDouble; p.D = f; break;
                    case int n: p.Kind = NativeAlgoAbi.KindInt32; p.I = n; break;
                    case long l: p.Kind = NativeAlgoAbi.KindInt32; p.I = (int)l; break;
                    case bool b: p.Kind = NativeAlgoAbi.KindBool; p.I = b ? 1 : 0; break;
                    case string s:
                        p.Kind = NativeAlgoAbi.KindText;
                        p.S = AllocUtf8(s, allocs);
                        break;
                    case null: p.Kind = NativeAlgoAbi.KindInt32; p.I = 0; break;
                    default:
                        // 枚举等：按字符串传（与 JSON enum 参数一致）
                        p.Kind = NativeAlgoAbi.KindText;
                        p.S = AllocUtf8(parameters[i]!.ToString() ?? "", allocs);
                        break;
                }
                Marshal.StructureToPtr(p, paramsPtr + i * size, false);
            }
        }

        // 图像数组（pin 像素缓冲）
        IntPtr imagesPtr = IntPtr.Zero;
        int imageCount = images?.Count ?? 0;
        if (imageCount > 0)
        {
            var size = Marshal.SizeOf<NativeAlgoAbi.AlgoCImage>();
            imagesPtr = Marshal.AllocHGlobal(imageCount * size);
            allocs.Add(imagesPtr);
            for (int i = 0; i < imageCount; i++)
            {
                var img = images![i];
                var pin = GCHandle.Alloc(img.PixelsBgra, GCHandleType.Pinned);
                pins.Add(pin);
                Marshal.StructureToPtr(new NativeAlgoAbi.AlgoCImage
                {
                    Width = img.Width,
                    Height = img.Height,
                    Bgra = pin.AddrOfPinnedObject(),
                }, imagesPtr + i * size, false);
            }
        }

        // 点云数组（pin xyz/rgb）
        IntPtr cloudsPtr = IntPtr.Zero;
        int cloudCount = clouds?.Count ?? 0;
        if (cloudCount > 0)
        {
            var size = Marshal.SizeOf<NativeAlgoAbi.AlgoCCloud>();
            cloudsPtr = Marshal.AllocHGlobal(cloudCount * size);
            allocs.Add(cloudsPtr);
            for (int i = 0; i < cloudCount; i++)
            {
                var c = clouds![i];
                var xyzPin = GCHandle.Alloc(c.Points, GCHandleType.Pinned);
                pins.Add(xyzPin);
                var rgbPin = c.ColorsRgb is not null ? GCHandle.Alloc(c.ColorsRgb, GCHandleType.Pinned) : default;
                if (c.ColorsRgb is not null) pins.Add(rgbPin);
                Marshal.StructureToPtr(new NativeAlgoAbi.AlgoCCloud
                {
                    Count = c.Count,
                    Xyz = xyzPin.AddrOfPinnedObject(),
                    Rgb = c.ColorsRgb is not null ? rgbPin.AddrOfPinnedObject() : IntPtr.Zero,
                }, cloudsPtr + i * size, false);
            }
        }

        var input = new NativeAlgoAbi.AlgoCInput
        {
            ParamCount = paramCount,
            Params = paramsPtr,
            ImageCount = imageCount,
            Images = imagesPtr,
            CloudCount = cloudCount,
            Clouds = cloudsPtr,
        };
        var inputPtr = Marshal.AllocHGlobal(Marshal.SizeOf<NativeAlgoAbi.AlgoCInput>());
        allocs.Add(inputPtr);
        Marshal.StructureToPtr(input, inputPtr, false);
        return inputPtr;
    }

    private static IntPtr AllocUtf8(string s, List<IntPtr> allocs)
    {
        var bytes = Encoding.UTF8.GetBytes(s);
        var p = Marshal.AllocHGlobal(bytes.Length + 1);
        allocs.Add(p);
        Marshal.Copy(bytes, 0, p, bytes.Length);
        Marshal.WriteByte(p + bytes.Length, 0);
        return p;
    }

    // ==================== marshal：C → 托管 ====================

    private (List<ImagePayload>, List<CloudPayload>) ReadOutput(ref NativeAlgoAbi.AlgoCOutput output)
    {
        var images = new List<ImagePayload>();
        if (output.ImageCount > 0 && output.Images != IntPtr.Zero)
        {
            var size = Marshal.SizeOf<NativeAlgoAbi.AlgoCImageOut>();
            for (int i = 0; i < output.ImageCount; i++)
            {
                var io = Marshal.PtrToStructure<NativeAlgoAbi.AlgoCImageOut>(output.Images + i * size);
                if (io.Width <= 0 || io.Height <= 0 || io.Bgra == IntPtr.Zero)
                    throw new Exception($"原生算子 {_entry} 输出图像[{i}] 非法（{io.Width}×{io.Height}）");
                var px = new byte[io.Width * io.Height * 4];
                Marshal.Copy(io.Bgra, px, 0, px.Length);
                images.Add(new ImagePayload { Width = io.Width, Height = io.Height, PixelsBgra = px });
            }
        }

        var clouds = new List<CloudPayload>();
        if (output.CloudCount > 0 && output.Clouds != IntPtr.Zero)
        {
            var size = Marshal.SizeOf<NativeAlgoAbi.AlgoCCloudOut>();
            for (int i = 0; i < output.CloudCount; i++)
            {
                var co = Marshal.PtrToStructure<NativeAlgoAbi.AlgoCCloudOut>(output.Clouds + i * size);
                if (co.Count < 0 || co.Xyz == IntPtr.Zero)
                    throw new Exception($"原生算子 {_entry} 输出点云[{i}] 非法（{co.Count} 点）");
                var xyz = new float[co.Count * 3];
                Marshal.Copy(co.Xyz, xyz, 0, xyz.Length);
                byte[]? rgb = null;
                if (co.Rgb != IntPtr.Zero)
                {
                    rgb = new byte[co.Count * 3];
                    Marshal.Copy(co.Rgb, rgb, 0, rgb.Length);
                }
                clouds.Add(new CloudPayload { Count = co.Count, Points = xyz, ColorsRgb = rgb });
            }
        }
        return (images, clouds);
    }

    /// <summary>按释放约定调 DLL 的 algo_free（数组与每个数据缓冲；error 不释放）。</summary>
    private void FreeDllBuffers(ref NativeAlgoAbi.AlgoCOutput output)
    {
        if (_free is null) return;
        if (output.Images != IntPtr.Zero)
        {
            var size = Marshal.SizeOf<NativeAlgoAbi.AlgoCImageOut>();
            for (int i = 0; i < output.ImageCount; i++)
            {
                var io = Marshal.PtrToStructure<NativeAlgoAbi.AlgoCImageOut>(output.Images + i * size);
                if (io.Bgra != IntPtr.Zero) _free(io.Bgra);
            }
            _free(output.Images);
        }
        if (output.Clouds != IntPtr.Zero)
        {
            var size = Marshal.SizeOf<NativeAlgoAbi.AlgoCCloudOut>();
            for (int i = 0; i < output.CloudCount; i++)
            {
                var co = Marshal.PtrToStructure<NativeAlgoAbi.AlgoCCloudOut>(output.Clouds + i * size);
                if (co.Xyz != IntPtr.Zero) _free(co.Xyz);
                if (co.Rgb != IntPtr.Zero) _free(co.Rgb);
            }
            _free(output.Clouds);
        }
    }

    private static void ZeroStruct(IntPtr p, int size)
    {
        for (int i = 0; i < size; i++) Marshal.WriteByte(p + i, 0);
    }
}
