using System;
using OpenTK.Graphics.OpenGL4;
using OpenTK.Mathematics;
using OpenTK.Windowing.Common;
using OpenTK.Windowing.Desktop;

namespace ComUI.Plugin.Image2D;

/// <summary>
/// OpenTK 离屏图像渲染器（M2.1）：
/// 隐藏 GLFW 窗口承载 GL 3.3 核心上下文，整图一张 GL 纹理（BGRA 直传 + mipmap 抗摩尔纹），
/// 按视图变换把图像四边形画进 FBO，glReadPixels 回读视口 BGRA 供 WriteableBitmap 呈现。
/// 仅在交互（缩放/平移/换图）时重绘，回读量 = 视口大小（约 2MP），与图像尺寸无关。
/// 无 GL 环境时 GlAvailable=false，由查看器走软件最近邻回退。
/// </summary>
public sealed class GlImageRenderer : IDisposable
{
    private OpenTK.Windowing.Desktop.NativeWindow? _win;
    private bool _glOk;

    private int _prog, _vao, _vbo, _uNearest;
    private readonly float[] _verts = new float[16];   // 每帧 4 顶点 × (pos2+uv2)
    private int _tex;               // 整图纹理
    private int _fbo, _fboTex;      // 视口离屏目标
    private int _vpW, _vpH;

    /// <summary>渲染后端名（状态栏显示用）。</summary>
    public string Backend => _glOk ? "OpenGL 3.3 (OpenTK 离屏)" : "软件回退（无 GL 上下文）";
    public bool GlAvailable => _glOk;

    public GlImageRenderer()
    {
        try
        {
            var settings = new NativeWindowSettings
            {
                Size = new Vector2i(8, 8),
                StartVisible = false,   // 隐藏窗口仅承载 GL 上下文
                APIVersion = new Version(3, 3),
                Profile = ContextProfile.Core,
                Flags = ContextFlags.ForwardCompatible,
                Title = "comui-offscreen",
            };
            _win = new OpenTK.Windowing.Desktop.NativeWindow(settings);
            _win.MakeCurrent();
            InitGl();
            _glOk = true;
        }
        catch
        {
            // 无显示/GPU 环境：走软件回退，查看器功能不受阻
            _glOk = false;
        }
    }

    private void InitGl()
    {
        _prog = BuildProgram(VertexSrc, FragmentSrc);
        _uNearest = GL.GetUniformLocation(_prog, "uNearest");

        _vao = GL.GenVertexArray();
        _vbo = GL.GenBuffer();
        GL.BindVertexArray(_vao);
        GL.BindBuffer(BufferTarget.ArrayBuffer, _vbo);
        // 每帧更新 4 顶点 × (pos2 + uv2)
        GL.BufferData(BufferTarget.ArrayBuffer, 16 * sizeof(float), IntPtr.Zero, BufferUsageHint.DynamicDraw);
        GL.EnableVertexAttribArray(0);
        GL.VertexAttribPointer(0, 2, VertexAttribPointerType.Float, false, 16, 0);
        GL.EnableVertexAttribArray(1);
        GL.VertexAttribPointer(1, 2, VertexAttribPointerType.Float, false, 16, 8);
        GL.BindVertexArray(0);
    }

    // ==================== 图像上传 ====================

    /// <summary>上传整图纹理（BGRA32 直传，生成 mipmap 供缩小时抗摩尔纹）。</summary>
    public void UploadImage(int w, int h, byte[] bgra)
    {
        if (!_glOk) return;
        _win?.MakeCurrent();   // 同 GlCloudRenderer：UI 线程多上下文必须先抢回自己的
        if (_tex == 0) _tex = GL.GenTexture();
        GL.BindTexture(TextureTarget.Texture2D, _tex);
        GL.TexImage2D(TextureTarget.Texture2D, 0, PixelInternalFormat.Rgba8,
            w, h, 0, PixelFormat.Bgra, PixelType.UnsignedByte, bgra);
        GL.GenerateMipmap(GenerateMipmapTarget.Texture2D);
        GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter,
            (int)TextureMinFilter.LinearMipmapLinear);
        GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)TextureMagFilter.Linear);
        GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)TextureWrapMode.ClampToEdge);
        GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)TextureWrapMode.ClampToEdge);
    }

    // ==================== 绘制与回读 ====================

    /// <summary>按视图（原点/缩放，像素单位）绘制整图到 FBO 并回读视口 BGRA。</summary>
    public void Draw(int vw, int vh, int imgW, int imgH, double scale, double ox, double oy, byte[] dest)
    {
        _win?.MakeCurrent();   // 防 UI 线程上下文被其它组件切走
        EnsureViewport(vw, vh);

        GL.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo);
        GL.Viewport(0, 0, vw, vh);
        GL.ClearColor(0.118f, 0.118f, 0.125f, 1f);   // #1E1E20
        GL.Clear(ClearBufferMask.ColorBufferBit);

        float x0 = (float)ox, y0 = (float)oy;
        float x1 = (float)(ox + imgW * scale), y1 = (float)(oy + imgH * scale);
        float nx0 = 2f * x0 / vw - 1f, ny0 = 1f - 2f * y0 / vh;
        float nx1 = 2f * x1 / vw - 1f, ny1 = 1f - 2f * y1 / vh;

        Span<float> v = stackalloc float[16]
        {
            nx0, ny0, 0f, 0f,
            nx1, ny0, 1f, 0f,
            nx0, ny1, 0f, 1f,
            nx1, ny1, 1f, 1f,
        };

        GL.UseProgram(_prog);
        GL.BindTexture(TextureTarget.Texture2D, _tex);
        bool nearest = scale >= 1.0;   // 放大（≥100%）一律最近邻：像素级检视要的是锐利像素块；
                                       // 1~3× 区间此前用线性插值会发糊（用户体感"到不了像素级"）。
                                       // 缩小（<100%）仍走 mipmap+线性抗摩尔纹
        GL.Uniform1(_uNearest, nearest ? 1 : 0);
        GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter,
            (int)(nearest ? TextureMagFilter.Nearest : TextureMagFilter.Linear));

        v.CopyTo(_verts);
        GL.BindVertexArray(_vao);
        GL.BindBuffer(BufferTarget.ArrayBuffer, _vbo);
        GL.BufferData(BufferTarget.ArrayBuffer, 16 * sizeof(float), _verts, BufferUsageHint.DynamicDraw);
        GL.DrawArrays(PrimitiveType.TriangleStrip, 0, 4);
        GL.BindVertexArray(0);

        GL.ReadPixels(0, 0, vw, vh, PixelFormat.Bgra, PixelType.UnsignedByte, dest);
        GL.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
    }

    private void EnsureViewport(int vw, int vh)
    {
        if (_fbo != 0 && _vpW == vw && _vpH == vh) return;
        if (_fboTex != 0) GL.DeleteTexture(_fboTex);
        if (_fbo != 0) GL.DeleteFramebuffer(_fbo);

        _fboTex = GL.GenTexture();
        GL.BindTexture(TextureTarget.Texture2D, _fboTex);
        GL.TexImage2D(TextureTarget.Texture2D, 0, PixelInternalFormat.Rgba8, vw, vh, 0,
            PixelFormat.Bgra, PixelType.UnsignedByte, IntPtr.Zero);
        GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)TextureMinFilter.Linear);
        GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)TextureMagFilter.Linear);

        _fbo = GL.GenFramebuffer();
        GL.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo);
        GL.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
            TextureTarget.Texture2D, _fboTex, 0);
        _vpW = vw; _vpH = vh;
    }

    // ==================== 着色器 ====================

    private const string VertexSrc = @"#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV; }";

    private const string FragmentSrc = @"#version 330 core
in vec2 vUV;
out vec4 oColor;
uniform sampler2D uTex;
uniform int uNearest;
void main() {
    if (uNearest == 1) {
        ivec2 size = textureSize(uTex, 0);
        ivec2 tc = clamp(ivec2(vUV * vec2(size)), ivec2(0), size - 1);
        oColor = texelFetch(uTex, tc, 0);
    } else {
        oColor = texture(uTex, vUV);
    }
}";

    private static int BuildProgram(string vs, string fs)
    {
        int v = GL.CreateShader(ShaderType.VertexShader);
        GL.ShaderSource(v, vs); GL.CompileShader(v);
        int f = GL.CreateShader(ShaderType.FragmentShader);
        GL.ShaderSource(f, fs); GL.CompileShader(f);
        int p = GL.CreateProgram();
        GL.AttachShader(p, v); GL.AttachShader(p, f); GL.LinkProgram(p);
        GL.GetProgram(p, GetProgramParameterName.LinkStatus, out int ok);
        if (ok == 0) throw new Exception("着色器链接失败: " + GL.GetProgramInfoLog(p));
        GL.DeleteShader(v); GL.DeleteShader(f);
        return p;
    }

    public void Dispose()
    {
        if (_glOk)
        {
            try
            {
                if (_tex != 0) GL.DeleteTexture(_tex);
                if (_fboTex != 0) GL.DeleteTexture(_fboTex);
                if (_fbo != 0) GL.DeleteFramebuffer(_fbo);
                if (_vbo != 0) GL.DeleteBuffer(_vbo);
                if (_vao != 0) GL.DeleteVertexArray(_vao);
                if (_prog != 0) GL.DeleteProgram(_prog);
            }
            catch { /* 上下文可能已失效 */ }
        }
        try { _win?.Dispose(); } catch { }
        _glOk = false;
    }
}
