using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using ComUI.Sdk;
using ComUI.Sdk.Native;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// 原生算子统一 C ABI 的 marshal 往返测试。
/// FakeNative 用托管委托**按 ABI 直接读写非托管内存**（与真实 C++ DLL 的行为一致），
/// 锁定：参数四种 kind、图像 BGRA 紧排、点云 SoA、输出拷回、algo_free 释放约定、错误路径。
/// </summary>
public class NativeAlgoTests
{
    private sealed class FakeNative
    {
        public List<IntPtr> Freed = new();

        /// <summary>模拟一个 C++ 算子：读参数/输入 → 伽马图像 + 点云透传 → DLL 侧分配输出。</summary>
        public int GammaAndPassthrough(IntPtr inputPtr, IntPtr outputPtr)
        {
            var input = Marshal.PtrToStructure<NativeAlgoAbi.AlgoCInput>(inputPtr);

            // 读参数（double gamma + int + text + bool）
            double gamma = 1.0;
            int intParam = 0;
            string textParam = "";
            bool boolParam = false;
            var pSize = Marshal.SizeOf<NativeAlgoAbi.AlgoCParam>();
            for (int i = 0; i < input.ParamCount; i++)
            {
                var p = Marshal.PtrToStructure<NativeAlgoAbi.AlgoCParam>(input.Params + i * pSize);
                switch (p.Kind)
                {
                    case NativeAlgoAbi.KindDouble: gamma = p.D; break;
                    case NativeAlgoAbi.KindInt32: intParam = p.I; break;
                    case NativeAlgoAbi.KindText: textParam = Marshal.PtrToStringUTF8(p.S) ?? ""; break;
                    case NativeAlgoAbi.KindBool: boolParam = p.I != 0; break;
                }
            }
            Assert.Equal(2.0, gamma);
            Assert.Equal(42, intParam);
            Assert.Equal("模式A", textParam);
            Assert.True(boolParam);

            // 读输入图像（BGRA 紧排）
            Assert.Equal(1, input.ImageCount);
            var iSize = Marshal.SizeOf<NativeAlgoAbi.AlgoCImage>();
            var inImg = Marshal.PtrToStructure<NativeAlgoAbi.AlgoCImage>(input.Images);
            Assert.Equal(4, inImg.Width);
            Assert.Equal(2, inImg.Height);
            var inPx = new byte[inImg.Width * inImg.Height * 4];
            Marshal.Copy(inImg.Bgra, inPx, 0, inPx.Length);

            // 读输入点云（SoA）
            Assert.Equal(1, input.CloudCount);
            var cSize = Marshal.SizeOf<NativeAlgoAbi.AlgoCCloud>();
            var inCloud = Marshal.PtrToStructure<NativeAlgoAbi.AlgoCCloud>(input.Clouds);
            Assert.Equal(3, inCloud.Count);
            var inXyz = new float[inCloud.Count * 3];
            Marshal.Copy(inCloud.Xyz, inXyz, 0, inXyz.Length);
            var inRgb = new byte[inCloud.Count * 3];
            Marshal.Copy(inCloud.Rgb, inRgb, 0, inRgb.Length);

            // DLL 侧分配输出：伽马后的图像 + 透传点云（拷贝一份，模拟 DLL 自有缓冲）
            var outPx = new byte[inPx.Length];
            for (int i = 0; i < outPx.Length; i += 4)
            {
                outPx[i] = (byte)Math.Clamp(255 * Math.Pow(inPx[i] / 255.0, 1.0 / gamma), 0, 255);
                outPx[i + 1] = (byte)Math.Clamp(255 * Math.Pow(inPx[i + 1] / 255.0, 1.0 / gamma), 0, 255);
                outPx[i + 2] = (byte)Math.Clamp(255 * Math.Pow(inPx[i + 2] / 255.0, 1.0 / gamma), 0, 255);
                outPx[i + 3] = 255;
            }
            var bgraPtr = Marshal.AllocHGlobal(outPx.Length);
            Marshal.Copy(outPx, 0, bgraPtr, outPx.Length);
            var xyzPtr = Marshal.AllocHGlobal(inXyz.Length * 4);
            Marshal.Copy(inXyz, 0, xyzPtr, inXyz.Length);
            var rgbPtr = Marshal.AllocHGlobal(inRgb.Length);
            Marshal.Copy(inRgb, 0, rgbPtr, inRgb.Length);

            var imgArr = Marshal.AllocHGlobal(Marshal.SizeOf<NativeAlgoAbi.AlgoCImageOut>());
            Marshal.StructureToPtr(new NativeAlgoAbi.AlgoCImageOut { Width = inImg.Width, Height = inImg.Height, Bgra = bgraPtr }, imgArr, false);
            var cloudArr = Marshal.AllocHGlobal(Marshal.SizeOf<NativeAlgoAbi.AlgoCCloudOut>());
            Marshal.StructureToPtr(new NativeAlgoAbi.AlgoCCloudOut { Count = inCloud.Count, Xyz = xyzPtr, Rgb = rgbPtr }, cloudArr, false);

            Marshal.StructureToPtr(new NativeAlgoAbi.AlgoCOutput
            {
                ImageCount = 1,
                Images = imgArr,
                CloudCount = 1,
                Clouds = cloudArr,
                Error = IntPtr.Zero,
            }, outputPtr, false);
            return 0;
        }

        /// <summary>模拟 algo_free：记录被释放的指针。</summary>
        public void Free(IntPtr p) => Freed.Add(p);
    }

    private static ImagePayload MakeImage()
    {
        var px = new byte[4 * 2 * 4];
        for (int i = 0; i < px.Length; i += 4) { px[i] = 64; px[i + 1] = 128; px[i + 2] = 192; px[i + 3] = 255; }
        return new ImagePayload { Width = 4, Height = 2, PixelsBgra = px };
    }

    private static CloudPayload MakeCloud() => new()
    {
        Count = 3,
        Points = new float[] { 1, 2, 3, 4, 5, 6, 7, 8, 9 },
        ColorsRgb = new byte[] { 255, 0, 0, 0, 255, 0, 0, 0, 255 },
    };

    [Fact]
    public void Run_MarshalsParamsAndData_RoundTripsAndFrees()
    {
        var fake = new FakeNative();
        var invoker = new NativeAlgoInvoker(fake.GammaAndPassthrough, fake.Free, "demo");

        var (images, clouds) = invoker.Run(
            new object?[] { 2.0, 42, "模式A", true },
            new[] { MakeImage() },
            new[] { MakeCloud() });

        // 输出图像：伽马 2.0（64→127，128→180，192→221）
        var img = Assert.Single(images);
        Assert.Equal(4, img.Width);
        Assert.Equal(2, img.Height);
        Assert.Equal(127, img.PixelsBgra[0]);
        Assert.Equal(180, img.PixelsBgra[1]);
        Assert.Equal(221, img.PixelsBgra[2]);
        Assert.Equal(255, img.PixelsBgra[3]);

        // 输出点云：透传（值与颜色逐字节一致，且是拷贝——非原数组引用）
        var cloud = Assert.Single(clouds);
        Assert.Equal(3, cloud.Count);
        Assert.Equal(new float[] { 1, 2, 3, 4, 5, 6, 7, 8, 9 }, cloud.Points);
        Assert.Equal(new byte[] { 255, 0, 0, 0, 255, 0, 0, 0, 255 }, cloud.ColorsRgb);

        // 释放约定：bgra、xyz、rgb、两个结构数组——共 5 个指针全部经 algo_free
        Assert.Equal(5, fake.Freed.Count);
    }

    [Fact]
    public void Run_ErrorReturn_ThrowsWithDllMessage()
    {
        static int Fail(IntPtr _, IntPtr outputPtr)
        {
            var msg = System.Text.Encoding.UTF8.GetBytes("内存不足");
            var p = Marshal.AllocHGlobal(msg.Length + 1);
            Marshal.Copy(msg, 0, p, msg.Length);
            Marshal.WriteByte(p + msg.Length, 0);
            Marshal.WriteInt32(outputPtr, 0);   // ImageCount=0
            Marshal.WriteInt32(outputPtr, sizeof(int) * 2 + sizeof(int) * 2, 0);   // CloudCount=0（偏移按结构布局）
            Marshal.WriteIntPtr(outputPtr, Marshal.SizeOf<NativeAlgoAbi.AlgoCOutput>() - IntPtr.Size, p);   // Error 字段
            return 7;
        }

        var invoker = new NativeAlgoInvoker(Fail, null, "fail");
        var ex = Assert.Throws<Exception>(() => invoker.Run(null, null, null));
        Assert.Contains("返回码 7", ex.Message);
        Assert.Contains("内存不足", ex.Message);
    }

    [Fact]
    public void Run_EmptyParamsAndNoInputs_Works()
    {
        static int Ok(IntPtr _, IntPtr outputPtr)
        {
            Marshal.StructureToPtr(new NativeAlgoAbi.AlgoCOutput(), outputPtr, false);
            return 0;
        }
        var invoker = new NativeAlgoInvoker(Ok, null, "ok");
        var (images, clouds) = invoker.Run(null, null, null);
        Assert.Empty(images);
        Assert.Empty(clouds);
    }
}
