using System;
using System.Runtime.InteropServices;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using OpenTK.Graphics.OpenGL4;
using OpenTK.Mathematics;
using OpenTK.Windowing.Common;
using OpenTK.Windowing.Desktop;
using OpenTK.Windowing.GraphicsLibraryFramework;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// OpenTK 离屏点云渲染器：隐藏 GLFW 窗口承载 GL3.3 上下文。
/// 多实体管理（按 Id 的 VAO/VBO，供点云设备树增删显隐），点云 VBO（XYZ float3 + RGB ubyte3 + 选区 ubyte1），
/// 透视/正交投影，分块渐进上传，FBO 回读视口 BGRA。
/// </summary>
public sealed class GlCloudRenderer : IDisposable
{
    private sealed class Entity
    {
        public required string Id;
        public int Vao, PosVbo, ColVbo, SelVbo, NrmVbo;
        public int Count, Cap;
        public nint NativePts, NativeCol;   // 非托管点/色缓冲（worker 拷贝后供 GL 消费；Dispose 时释放）
        public bool Visible = true;
        public Vector3 BoundsMin, BoundsMax;
        public int ColorMode = 1;
        public float PointSize = 3f;
        public Matrix4 Xform = Matrix4.Identity;   // 行向量约定（与 MVP 一致）
        public Vector3 SingleColor = new(0.7f, 0.75f, 0.8f);
        public float? ColorMin, ColorMax;       // 伪彩色带域（null=用实体 Bounds——色带范围可调）
        public bool HasNormals;                 // 法线 attribute 已上传（location 3）
        public bool Lit;                        // 光照模式（法线 Lambert；无法线自动回退平光）
        public float[]? NormalsRef;             // 托管法线引用（导出用；同 Id 重发后由视图重传）
    }

    private NativeWindow? _win;
    private bool _ok;
    private int _prog;

    // ===== 后台线程分块上传（共享 GL 上下文，GLFW 主线程=UI 线程，worker 只做 MakeContextCurrent/上传） =====
    private sealed class UploadJob
    {
        public Entity? Ent;        // 目标实体（Delete 时为待删实体）
        public bool Delete;        // true = 延迟删除该实体
        public bool Begin;         // true = 原生拷贝 + 一次性 BufferData（含显存预分配）
        public int Count;
        public Vector3 BoundsMin, BoundsMax;
        public float[]? Pts;       // 托管源数组（仅 Begin 引用；拷入原生后即可被 GC 回收）
        public byte[]? Col;
        public float[]? Nrm;       // 逐点法线（可选；Begin 时与点同布局重建 NrmVbo）
        public int Start, N;
    }

    private unsafe OpenTK.Windowing.GraphicsLibraryFramework.Window* _uploadWin;   // 共享上下文窗口
    private volatile bool _bgOk;
    private BlockingCollection<UploadJob>? _jobs;
    private Thread? _uploadWorker;
    private byte[]? _bgGray;       // worker 专用灰度填充（无逐点色时），避免每块分配

    /// <summary>后台上传是否可用（共享上下文创建成功）。false 时回退 UI 线程分块上传。</summary>
    public bool BackgroundUpload => _ok && _bgOk;

    /// <summary>UI 线程揭示进度：数据已在 BeginUploadGl 一次性进显存，此处仅推进可见点数（渐进显示，无 GL 数据操作）。</summary>
    public void SetRevealed(string id, int count)
    {
        if (_entities.TryGetValue(id, out var e) && count > e.Count && count <= e.Cap)
            e.Count = count;
    }

    /// <summary>某实体全部块上传完成（worker 线程触发，订阅方需自行调度回 UI 线程）。</summary>
    public event Action<string, Vector3, Vector3>? UploadCompleted;
    private int _uMvp, _uModel, _uMode, _uMinY, _uMaxY, _uPointSize, _uSingle, _uEyeDist, _uStride, _uSelColor, _uClipOn, _uClipLo, _uClipHi, _uClipAxis, _uLit, _uLightStrength;
    private float _lightStrength = 1f;   // 法线光照强度（0=平光 1=全光照）
    private (int Axis, float Lo, float Hi)? _zClip;   // 切片带（轴 0=X 1=Y 2=Z；null=不切片）——状态式，Draw 每帧上传
    private int _fbo, _fboTex, _fboDepth;
    private int _vpW, _vpH;
    private float[] _posScratch = Array.Empty<float>();
    private byte[] _colScratch = Array.Empty<byte>();
    private readonly Dictionary<string, Entity> _entities = new();

    public bool Ok => _ok;
    public string Backend => _ok ? "OpenGL 3.3" : "无 GL";

    /// <summary>最近一次 Draw 的 MVP（OpenTK 行主序存储），供 CPU 端拾取/框选投影复用。</summary>
    public Matrix4 LastMvp { get; private set; } = Matrix4.Identity;

    public GlCloudRenderer(bool allowBackgroundWorker = true)
    {
        try
        {
            var s = new NativeWindowSettings
            {
                Size = new Vector2i(8, 8), StartVisible = false,
                APIVersion = new Version(3, 3), Profile = ContextProfile.Core,
                Flags = ContextFlags.ForwardCompatible, Title = "comui-3d",
            };
            _win = new NativeWindow(s);
            _win.MakeCurrent();
            InitGl();
            _ok = true;
            // 内嵌预览视图禁用后台 worker：多视图各自持 worker 会在 GL 上下文上跨线程互卡
            //（UI 线程死锁——点面板/标签全无反应）；预览点云小，UI 线程同步上传足够
            if (allowBackgroundWorker)
                StartBackgroundUploader();
        }
        catch (Exception ex)
        {
            // 无 GL 环境走软件回退；记录真实异常，便于诊断"无 GL 上下文"（如上下文数量耗尽）
            try
            {
                // GL 初始化失败日志：仓库根 output/（原 next-to-exe 重建即丢——Sdk.AppPaths 统一）
                System.IO.Directory.CreateDirectory(ComUI.Sdk.AppPaths.OutputDir);
                var log = System.IO.Path.Combine(ComUI.Sdk.AppPaths.OutputDir, "gl_init_error.log");
                System.IO.File.AppendAllText(log,
                    $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] GlCloudRenderer ctor failed (allowBg={allowBackgroundWorker}): {ex}\n");
            }
            catch { }
        }
    }

    /// <summary>创建与主上下文共享对象命名空间的上传上下文 + 工作线程；失败则保持 UI 线程上传。</summary>
    private unsafe void StartBackgroundUploader()
    {
        try
        {
            // WindowHint 是全局状态；本宿主此后不再创建其他窗口
            GLFW.WindowHint(WindowHintBool.Visible, false);
            GLFW.WindowHint(WindowHintInt.ContextVersionMajor, 3);
            GLFW.WindowHint(WindowHintInt.ContextVersionMinor, 3);
            GLFW.WindowHint(WindowHintOpenGlProfile.OpenGlProfile, OpenGlProfile.Core);
            _uploadWin = GLFW.CreateWindow(1, 1, "comui-3d-upload", (OpenTK.Windowing.GraphicsLibraryFramework.Monitor*)nint.Zero, _win.WindowPtr);
            _bgOk = _uploadWin != null;
            if (_bgOk)
            {
                _jobs = new BlockingCollection<UploadJob>();
                _uploadWorker = new Thread(UploadWorkerLoop) { IsBackground = true, Name = "comui-3d-upload" };
                _uploadWorker.Start();
            }
        }
        catch
        {
            _bgOk = false;
        }
    }

    private void UploadWorkerLoop()
    {
        try
        {
            foreach (var job in _jobs!.GetConsumingEnumerable())
            {
                // 每个任务单独持有共享上下文并在完成后立即释放：
                // 终身持有会让 UI 线程的 GL 操作（视图选择/参数构建路径）与驱动层同步互卡（UI 挂起无异常）
                unsafe { GLFW.MakeContextCurrent(_uploadWin); }
                try
                {
                    if (job.Delete && job.Ent is not null) DeleteEntityGl(job.Ent);
                    else if (job.Begin && job.Ent is not null) BeginUploadGl(job);
                }
                finally
                {
                    try { unsafe { GLFW.MakeContextCurrent((OpenTK.Windowing.GraphicsLibraryFramework.Window*)nint.Zero); } }
                    catch { }
                }
            }
        }
        catch
        {
            _bgOk = false;   // worker 异常 → 之后回退 UI 线程上传（进度账本由视图持有，不丢显示）
            try { unsafe { GLFW.MakeContextCurrent((OpenTK.Windowing.GraphicsLibraryFramework.Window*)nint.Zero); } }
            catch { }
        }
    }

    private unsafe void BeginUploadGl(UploadJob job)
    {
        var e = job.Ent!;
        // 释放上一帧的原生缓冲（同 Id 更新场景）
        if (e.NativePts != 0) NativeMemory.Free((void*)e.NativePts);
        if (e.NativeCol != 0) NativeMemory.Free((void*)e.NativeCol);
        e.NativePts = (nint)NativeMemory.Alloc((nuint)(job.Count * 12));
        e.NativeCol = (nint)NativeMemory.Alloc((nuint)Math.Max(1, job.Count * 3));
        if (job.Pts is not null && job.Pts.Length >= job.Count * 3)
            Marshal.Copy(job.Pts, 0, e.NativePts, job.Count * 3);
        else
            NativeMemory.Clear((void*)e.NativePts, (nuint)(job.Count * 12));
        if (job.Col is not null && job.Col.Length >= job.Count * 3)
            Marshal.Copy(job.Col, 0, e.NativeCol, job.Count * 3);
        else
            NativeMemory.Clear((void*)e.NativeCol, (nuint)(job.Count * 3));

        GL.BindVertexArray(e.Vao);
        GL.BindBuffer(BufferTarget.ArrayBuffer, e.PosVbo);
        GL.BufferData(BufferTarget.ArrayBuffer, job.Count * 12, e.NativePts, BufferUsageHint.StaticDraw);
        GL.BindBuffer(BufferTarget.ArrayBuffer, e.ColVbo);
        GL.BufferData(BufferTarget.ArrayBuffer, job.Count * 3, e.NativeCol, BufferUsageHint.StaticDraw);
        GL.BindBuffer(BufferTarget.ArrayBuffer, e.SelVbo);
        GL.BufferData(BufferTarget.ArrayBuffer, job.Count, new byte[job.Count], BufferUsageHint.DynamicDraw);
        BindNormalBuffer(e, job.Count, job.Nrm);   // 法线随点重分配（旧 Cap 与新 normals 不再错位/越界）
        GL.BindVertexArray(0);
        e.Cap = job.Count;
        e.Count = 0;
        // 精确边界：拷贝时顺带一遍 O(n) min/max（job 边界是采样预估值，UV 映射/jet 色域需要精确值）
        Vector3 min = new(float.MaxValue);
        Vector3 max = new(float.MinValue);
        unsafe
        {
            var p = (float*)e.NativePts;
            for (int i = 0; i < job.Count; i++)
            {
                float x = p[i * 3], y = p[i * 3 + 1], z = p[i * 3 + 2];
                if (x < min.X) min.X = x; if (y < min.Y) min.Y = y; if (z < min.Z) min.Z = z;
                if (x > max.X) max.X = x; if (y > max.Y) max.Y = y; if (z > max.Z) max.Z = z;
            }
        }
        e.BoundsMin = min;
        e.BoundsMax = max;
        GL.Flush();
        UploadCompleted?.Invoke(e.Id, min, max);   // 视图回填精确边界（UV 映射/拾取用）
    }


    private unsafe void DeleteEntityGl(Entity e)
    {
        try
        {
            GL.DeleteVertexArray(e.Vao);
            GL.DeleteBuffer(e.PosVbo);
            GL.DeleteBuffer(e.ColVbo);
            GL.DeleteBuffer(e.SelVbo);
            if (e.NrmVbo != 0) GL.DeleteBuffer(e.NrmVbo);
        }
        catch { }
        // 释放非托管点/色缓冲（漏了 = 每删一个大云泄漏数百 MB）
        if (e.NativePts != 0) { NativeMemory.Free((void*)e.NativePts); e.NativePts = 0; }
        if (e.NativeCol != 0) { NativeMemory.Free((void*)e.NativeCol); e.NativeCol = 0; }
    }

    private void InitGl()
    {
        _prog = Build(VertexSrc, FragSrc);
        _uMvp = GL.GetUniformLocation(_prog, "uMVP");
        _uModel = GL.GetUniformLocation(_prog, "uModel");
        _uMode = GL.GetUniformLocation(_prog, "uMode");
        _uMinY = GL.GetUniformLocation(_prog, "uMinY");
        _uMaxY = GL.GetUniformLocation(_prog, "uMaxY");
        _uPointSize = GL.GetUniformLocation(_prog, "uPointSize");
        _uSingle = GL.GetUniformLocation(_prog, "uSingle");
        _uEyeDist = GL.GetUniformLocation(_prog, "uEyeDist");
        _uStride = GL.GetUniformLocation(_prog, "uStride");
        _uSelColor = GL.GetUniformLocation(_prog, "uSelColor");
        _uClipOn = GL.GetUniformLocation(_prog, "uClipOn");
        _uClipLo = GL.GetUniformLocation(_prog, "uClipLo");
        _uClipHi = GL.GetUniformLocation(_prog, "uClipHi");
        _uClipAxis = GL.GetUniformLocation(_prog, "uClipAxis");
        _uLit = GL.GetUniformLocation(_prog, "uLit");
        _uLightStrength = GL.GetUniformLocation(_prog, "uLightStrength");

        GL.Enable(EnableCap.DepthTest);
        GL.DepthFunc(DepthFunction.Lequal);
        GL.Enable(EnableCap.ProgramPointSize);
        GL.ClearColor(0.118f, 0.118f, 0.125f, 1f);
    }


    /// <summary>设置实体独立样式（颜色模式/点大小/单色 RGB）。</summary>
    public void SetEntityStyle(string id, int colorMode, float pointSize, Vector3? singleColor = null)
    {
        if (_entities.TryGetValue(id, out var e))
        {
            e.ColorMode = colorMode;
            e.PointSize = pointSize;
            if (singleColor is { } sc) e.SingleColor = sc;
        }
    }

    /// <summary>设置实体变换矩阵（x 为 UI 行主序 16 元素，转 OpenTK 后传着色器）。</summary>
    public void SetEntityTransform(string id, double[] x)
    {
        if (!_entities.TryGetValue(id, out var e) || x.Length < 16) return;
        // OpenTK 行向量约定：v' = v * M；UI 行主序数学矩阵需转置存入
        e.Xform = new Matrix4(
            (float)x[0], (float)x[4], (float)x[8],  (float)x[12],
            (float)x[1], (float)x[5], (float)x[9],  (float)x[13],
            (float)x[2], (float)x[6], (float)x[10], (float)x[14],
            (float)x[3], (float)x[7], (float)x[11], (float)x[15]);
    }

    public bool IsVisible(string id) => _entities.TryGetValue(id, out var e) && e.Visible;

    public int VisiblePoints => _entities.Values.Where(e => e.Visible).Sum(e => e.Count);

    // ==================== 分块上传 ====================

    /// <summary>开始某实体的上传：不存在则建实体壳（VAO/VBO），入队 worker 完成数据拷贝与显存预分配；同 Id 重发=更新。</summary>
    public void BeginUpload(string id, int count, Vector3 boundsMin, Vector3 boundsMax, float[] points, byte[]? colorsRgb, float[]? normals = null)
    {
        if (!_ok || count <= 0) return;
        _win?.MakeCurrent();
        if (!_entities.TryGetValue(id, out var e))
        {
            e = new Entity { Id = id };
            e.Vao = GL.GenVertexArray();
            e.PosVbo = GL.GenBuffer();
            e.ColVbo = GL.GenBuffer();
            e.SelVbo = GL.GenBuffer();
            GL.BindVertexArray(e.Vao);
            GL.BindBuffer(BufferTarget.ArrayBuffer, e.PosVbo);
            GL.EnableVertexAttribArray(0);
            GL.VertexAttribPointer(0, 3, VertexAttribPointerType.Float, false, 12, 0);
            GL.BindBuffer(BufferTarget.ArrayBuffer, e.ColVbo);
            GL.EnableVertexAttribArray(1);
            GL.VertexAttribPointer(1, 3, VertexAttribPointerType.UnsignedByte, true, 3, 0);
            GL.BindBuffer(BufferTarget.ArrayBuffer, e.SelVbo);
            GL.EnableVertexAttribArray(2);
            GL.VertexAttribPointer(2, 1, VertexAttribPointerType.UnsignedByte, true, 1, 0);
            GL.BindVertexArray(0);
            _entities[id] = e;
        }
        e.Count = 0;
        e.BoundsMin = boundsMin;
        e.BoundsMax = boundsMax;
        if (BackgroundUpload)
        {
            // worker：原生拷贝（托管数组→NativeMemory）+ 一次性 BufferData；UI 零阻塞
            // 注意：Cap 不得在 UI 线程提前改——显存缓冲要等 worker BeginUploadGl 分配并设置，
            // 提前放大 Cap 会让 SetRevealed/DrawArrays 越界读旧尺寸缓冲（GPU 未定义行为）
            _jobs?.Add(new UploadJob { Ent = e, Begin = true, Count = count, Pts = points, Col = colorsRgb, Nrm = normals, BoundsMin = boundsMin, BoundsMax = boundsMax });
            return;
        }
        e.Cap = count;   // 同步路径：缓冲已分配，Cap 立即生效
        GL.BindVertexArray(e.Vao);
        GL.BindBuffer(BufferTarget.ArrayBuffer, e.PosVbo);
        GL.BufferData(BufferTarget.ArrayBuffer, count * 12, IntPtr.Zero, BufferUsageHint.StaticDraw);
        GL.BindBuffer(BufferTarget.ArrayBuffer, e.ColVbo);
        GL.BufferData(BufferTarget.ArrayBuffer, count * 3, IntPtr.Zero, BufferUsageHint.StaticDraw);
        GL.BindBuffer(BufferTarget.ArrayBuffer, e.SelVbo);
        GL.BufferData(BufferTarget.ArrayBuffer, count, new byte[count], BufferUsageHint.DynamicDraw);
        BindNormalBuffer(e, count, normals);
        GL.BindVertexArray(0);
    }

    /// <summary>法线 VBO 重建（Begin 场景随点数据重分配，杜绝旧 Cap 与新 normals 的越界读写；normals 空=移除光照数据）。</summary>
    private void BindNormalBuffer(Entity e, int count, float[]? normals)
    {
        if (normals is not null && normals.Length >= count * 3)
        {
            if (e.NrmVbo == 0) e.NrmVbo = GL.GenBuffer();
            GL.BindBuffer(BufferTarget.ArrayBuffer, e.NrmVbo);
            GL.BufferData(BufferTarget.ArrayBuffer, count * 12, normals, BufferUsageHint.StaticDraw);
            GL.EnableVertexAttribArray(3);
            GL.VertexAttribPointer(3, 3, VertexAttribPointerType.Float, false, 12, 0);
            e.HasNormals = true;
            e.NormalsRef = normals;
        }
        else
        {
            if (e.NrmVbo != 0) { GL.DeleteBuffer(e.NrmVbo); e.NrmVbo = 0; }
            e.HasNormals = false;
            e.NormalsRef = null;
        }
    }

    /// <summary>上传一块（start = 起始点索引，按 Id 定位实体）。完成后实体 Count = start + n，可渐进绘制；
    /// bounds 为该实体的当前精确边界（分块合并后由视图传入）。</summary>
    public void UploadChunk(string id, float[] points, byte[]? colorsRgb, int start, int n, Vector3 boundsMin, Vector3 boundsMax)
    {
        if (!_ok || !_entities.TryGetValue(id, out var e) || n <= 0 || start + n > e.Cap) return;
        _win?.MakeCurrent();
        if (_posScratch.Length < n * 3) _posScratch = new float[n * 3];
        if (_colScratch.Length < n * 3) _colScratch = new byte[n * 3];
        Array.Copy(points, start * 3, _posScratch, 0, n * 3);
        if (colorsRgb is not null && colorsRgb.Length >= (start + n) * 3)
            Array.Copy(colorsRgb, start * 3, _colScratch, 0, n * 3);
        else
            Array.Fill(_colScratch, (byte)200, 0, n * 3);
        GL.BindVertexArray(e.Vao);
        GL.BindBuffer(BufferTarget.ArrayBuffer, e.PosVbo);
        GL.BufferSubData(BufferTarget.ArrayBuffer, (IntPtr)(start * 12), n * 12, _posScratch);
        GL.BindBuffer(BufferTarget.ArrayBuffer, e.ColVbo);
        GL.BufferSubData(BufferTarget.ArrayBuffer, (IntPtr)(start * 3), n * 3, _colScratch);
        GL.BindVertexArray(0);
        e.Count = start + n;
        e.BoundsMin = boundsMin;
        e.BoundsMax = boundsMax;
    }

    /// <summary>上传逐点法线（float[3·count] SoA，一次性 BufferData + 启用 location=3 attribute）。云点数不变时可反复调用（重算覆盖）。</summary>
    public void SetNormals(string id, float[] normals)
    {
        // Cap<=0（后台 BeginUploadGl 尚未完成）时静默等就绪——否则会建空缓冲且 HasNormals 误置位
        if (!_ok || !_entities.TryGetValue(id, out var e) || e.Cap <= 0 || normals.Length < e.Cap * 3) return;
        _win?.MakeCurrent();
        GL.BindVertexArray(e.Vao);
        BindNormalBuffer(e, e.Cap, normals);   // BufferData 重分配（点数未变 → 等价覆盖；杜绝旧 Cap 越界）
        GL.BindVertexArray(0);
    }

    /// <summary>伪彩色带域覆盖（数据 Y 坐标；任一 null = 回退实体包围盒域）——薄高度差场景拉满对比度。</summary>
    public void SetEntityColorRange(string id, float? lo, float? hi)
    {
        if (_entities.TryGetValue(id, out var e))
        {
            if (lo is not null && hi is not null) { e.ColorMin = lo; e.ColorMax = hi; }
            else { e.ColorMin = null; e.ColorMax = null; }
        }
    }

    /// <summary>单独设置实体点大小（快捷键 ± 步进；不动颜色模式）。</summary>
    public void SetEntityPointSize(string id, float size)
    {
        if (_entities.TryGetValue(id, out var e)) e.PointSize = Math.Clamp(size, 1f, 10f);
    }

    /// <summary>法线光照强度（0=平光 1=全光照；全局统一，Lit 开启的实体生效）。</summary>
    public void SetLightStrength(float strength)
    {
        _lightStrength = Math.Clamp(strength, 0f, 1f);
    }

    /// <summary>实体光照开关（无 normals 的实体自动回退平光）。</summary>
    public void SetEntityLit(string id, bool lit)
    {
        if (_entities.TryGetValue(id, out var e)) e.Lit = lit;
    }

    public bool TryGetNormals(string id, out float[]? normals)
    {
        if (_entities.TryGetValue(id, out var e) && e.HasNormals && e.NormalsRef is not null)
        {
            normals = e.NormalsRef;
            return true;
        }
        normals = null;
        return false;
    }

    /// <summary>上传逐点选区掩码（0/255），选中的点用高亮色绘制。</summary>
    public void SetSelection(string id, byte[] mask)
    {
        if (!_ok || !_entities.TryGetValue(id, out var e) || e.Count == 0) return;
        _win?.MakeCurrent();
        GL.BindVertexArray(e.Vao);
        GL.BindBuffer(BufferTarget.ArrayBuffer, e.SelVbo);
        if (mask.Length >= e.Count)
            GL.BufferData(BufferTarget.ArrayBuffer, e.Count, mask, BufferUsageHint.DynamicDraw);
        else
            GL.BufferData(BufferTarget.ArrayBuffer, e.Count, new byte[e.Count], BufferUsageHint.DynamicDraw);
        GL.BindVertexArray(0);
    }

    public void SetVisible(string id, bool visible)
    {
        if (_entities.TryGetValue(id, out var e)) e.Visible = visible;
    }

    /// <summary>重命名实体（仅改键与 Id，GL 缓冲不动）。</summary>
    public void RenameEntity(string oldId, string newId)
    {
        if (!_entities.TryGetValue(oldId, out var e) || _entities.ContainsKey(newId)) return;
        _entities.Remove(oldId);
        e.Id = newId;
        _entities[newId] = e;
    }

    public unsafe void RemoveEntity(string id)
    {
        if (!_entities.TryGetValue(id, out var e)) return;
        _entities.Remove(id);
        if (BackgroundUpload && _jobs is not null && !_jobs.IsAddingCompleted)
        {
            // 延迟删除：worker FIFO 先完成该实体在途块（写旧存储，不再被绘制），再删缓冲——避免与写入竞态
            _jobs.Add(new UploadJob { Ent = e, Delete = true });
            return;
        }
        if (_ok)
        {
            _win?.MakeCurrent();
            try
            {
                GL.DeleteVertexArray(e.Vao);
                GL.DeleteBuffer(e.PosVbo);
                GL.DeleteBuffer(e.ColVbo);
                GL.DeleteBuffer(e.SelVbo);
                if (e.NrmVbo != 0) GL.DeleteBuffer(e.NrmVbo);
            }
            catch { }
        }
        // 同步回退分支同样释放非托管点/色缓冲（worker 死亡后不再有 DeleteEntityGl 代劳）
        if (e.NativePts != 0) { NativeMemory.Free((void*)e.NativePts); e.NativePts = 0; }
        if (e.NativeCol != 0) { NativeMemory.Free((void*)e.NativeCol); e.NativeCol = 0; }
    }

    // ==================== 绘制 ====================

    /// <param name="travel">沿视线（fwd）推进量：&gt;0 相机持续前进、可越过 target 穿行到点云背面；
    /// 0 为原始位置。注视方向恒为 fwd，因此穿行时不会回头看 target（符合"一直往前靠近背面点"的直觉）。</param>
    /// <summary>切片带（axis 0=X 1=Y(高度) 2=Z；null=关闭切片）。渲染器状态式：每帧 Draw 自动上传，GPU 侧过滤零 CPU 成本。</summary>
    public void SetZClip(int axis, float? lo, float? hi)
    {
        if (lo is not null && hi is not null && lo < hi) _zClip = (Math.Clamp(axis, 0, 2), lo.Value, hi.Value);
        else _zClip = null;
    }

    public void Draw(int vw, int vh,
        Quaternion cam, float dist, Vector3 target, bool ortho,
        int stride, byte[] dest, float farClip = 0, float sceneRadius = 0, float travel = 0)
    {
        if (!_ok) return;
        _win?.MakeCurrent();
        EnsureFbo(vw, vh);

        var fwd = Vector3.Transform(-Vector3.UnitZ, cam);
        // 相机沿视线前进 travel：travel=0 时 eye=target-fwd*dist、LookAt 方向恰为 fwd（与原行为一致）。
        // travel>0 越过中心继续朝 fwd 前进；注视点取 eye+fwd（方向恒为 fwd，穿行时不回头）。
        float eyeDist = dist - travel;
        var eye = target - fwd * eyeDist;
        var up = Vector3.Transform(Vector3.UnitY, cam);
        var view = Matrix4.LookAt(eye, eye + fwd, up);

        float aspect = vw / (float)vh;
        // near 用固定小值（dist 为初始适配距离、不再随滚轮缩小）：贴近点时不会被近裁剪面切掉。
        float near = MathF.Max(dist * 1e-4f, 1e-6f);
        // far 覆盖整个场景：dist 固定为初始适配距离，eye 前进后前方最远点不超过场景直径，
        // max(dist*100, dist+半径*4) 足够大；用户 farClip>0 时以其为下限（仍保证覆盖场景）。
        float cover = dist + sceneRadius * 4f;
        float far = farClip > 0 ? MathF.Max(farClip, cover) : MathF.Max(cover, dist * 100f);
        far = MathF.Max(far, near * 2f);
        Matrix4 proj;
        if (ortho)
        {
            // 正交模式也随推进量缩放（eyeDist 越小视野越大），用绝对值兼容越过中心后的情形。
            float halfH = MathF.Max(MathF.Abs(eyeDist), 1e-4f) * MathF.Tan(MathF.PI / 8);
            float halfW = halfH * aspect;
            proj = Matrix4.CreateOrthographicOffCenter(-halfW, halfW, -halfH, halfH, near, far);
        }
        else
        {
            proj = Matrix4.CreatePerspectiveFieldOfView(MathHelper.DegreesToRadians(45f), aspect, near, far);
        }
        var mvp = view * proj;
        LastMvp = mvp;

        GL.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo);
        GL.Viewport(0, 0, vw, vh);
        GL.Clear(ClearBufferMask.ColorBufferBit | ClearBufferMask.DepthBufferBit);

        GL.UseProgram(_prog);
        GL.UniformMatrix4(_uMvp, false, ref mvp);
        GL.Uniform1(_uEyeDist, dist);
        GL.Uniform1(_uStride, stride);
        GL.Uniform3(_uSelColor, 1.0f, 0.15f, 1.0f);   // 选区高亮：洋红（jet 色带中不存在的色相）
        GL.Uniform1(_uClipOn, _zClip is not null ? 1 : 0);
        GL.Uniform1(_uClipLo, _zClip?.Lo ?? 0f);
        GL.Uniform1(_uClipHi, _zClip?.Hi ?? 0f);
        GL.Uniform1(_uClipAxis, _zClip?.Axis ?? 1);

        foreach (var e in _entities.Values)
        {
            if (!e.Visible || e.Count == 0) continue;
            GL.Uniform1(_uMinY, e.ColorMin ?? e.BoundsMin.Y);
            GL.Uniform1(_uMaxY, e.ColorMax ?? e.BoundsMax.Y);
            GL.Uniform1(_uMode, e.ColorMode);
            GL.Uniform1(_uPointSize, e.PointSize);
            GL.Uniform3(_uSingle, e.SingleColor.X, e.SingleColor.Y, e.SingleColor.Z);
            GL.Uniform1(_uLit, e.Lit && e.HasNormals ? 1 : 0);
            GL.Uniform1(_uLightStrength, _lightStrength);
            var model = e.Xform;
            GL.UniformMatrix4(_uModel, false, ref model);
            GL.BindVertexArray(e.Vao);
            GL.DrawArrays(PrimitiveType.Points, 0, e.Count);
        }
        GL.BindVertexArray(0);

        GL.ReadPixels(0, 0, vw, vh, PixelFormat.Bgra, PixelType.UnsignedByte, dest);
        GL.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
    }

    private void EnsureFbo(int vw, int vh)
    {
        if (_fbo != 0 && _vpW == vw && _vpH == vh) return;
        if (_fboTex != 0) GL.DeleteTexture(_fboTex);
        if (_fboDepth != 0) GL.DeleteRenderbuffer(_fboDepth);
        if (_fbo != 0) GL.DeleteFramebuffer(_fbo);
        _fboTex = GL.GenTexture();
        GL.BindTexture(TextureTarget.Texture2D, _fboTex);
        GL.TexImage2D(TextureTarget.Texture2D, 0, PixelInternalFormat.Rgba8, vw, vh, 0,
            PixelFormat.Bgra, PixelType.UnsignedByte, IntPtr.Zero);
        GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)TextureMinFilter.Linear);
        GL.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)TextureMagFilter.Linear);
        _fboDepth = GL.GenRenderbuffer();
        GL.BindRenderbuffer(RenderbufferTarget.Renderbuffer, _fboDepth);
        GL.RenderbufferStorage(RenderbufferTarget.Renderbuffer, RenderbufferStorage.DepthComponent24, vw, vh);
        _fbo = GL.GenFramebuffer();
        GL.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo);
        GL.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
            TextureTarget.Texture2D, _fboTex, 0);
        GL.FramebufferRenderbuffer(FramebufferTarget.Framebuffer, FramebufferAttachment.DepthAttachment,
            RenderbufferTarget.Renderbuffer, _fboDepth);
        _vpW = vw; _vpH = vh;
    }

    private const string VertexSrc = @"#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in float aSel;
layout(location=3) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform float uMinY;
uniform float uMaxY;
uniform float uPointSize;
uniform float uEyeDist;
uniform int uStride;
uniform int uClipOn;
uniform float uClipLo;
uniform float uClipHi;
uniform int uClipAxis;   // 0=X 1=Y(height) 2=Z
out vec3 vColor;
out float vHeightT;
out float vSel;
out vec3 vNormal;
void main() {
    gl_Position = uMVP * uModel * vec4(aPos, 1.0);
    if (uStride > 1 && gl_VertexID % uStride != 0)
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
    if (uClipOn == 1)
    {
        float cv = uClipAxis == 0 ? aPos.x : (uClipAxis == 1 ? aPos.y : aPos.z);
        if (cv < uClipLo || cv > uClipHi)
            gl_Position = vec4(0.0, 0.0, -2.0, 1.0);   // slice: move out-of-range points off clip volume (GPU filter)
    }
    gl_PointSize = uPointSize * (uEyeDist / max(gl_Position.w, 0.1));
    vHeightT = (aPos.y - uMinY) / max(uMaxY - uMinY, 1e-6);
    vColor = aColor;
    vSel = aSel;
    vNormal = aNormal;
}";

    private const string FragSrc = @"#version 330 core
in vec3 vColor;
in float vHeightT;
in float vSel;
in vec3 vNormal;
uniform int uMode;
uniform vec3 uSingle;
uniform vec3 uSelColor;
uniform int uLit;
uniform float uLightStrength;
out vec4 oColor;
vec3 jet(float t) {
    t = clamp(t, 0.0, 1.0);
    if (t < 0.25) return mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), t / 0.25);
    if (t < 0.5)  return mix(vec3(0.0, 1.0, 1.0), vec3(0.0, 0.8, 0.0), (t - 0.25) / 0.25);
    if (t < 0.75) return mix(vec3(0.0, 0.8, 0.0), vec3(1.0, 1.0, 0.0), (t - 0.5) / 0.25);
    return mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), (t - 0.75) / 0.25);
}
void main() {
    vec3 c = uMode == 0 ? vColor : (uMode == 1 ? jet(vHeightT) : uSingle);
    if (vSel > 0.5) c = uSelColor;
    // normal lighting (half-lambert + ambient floor): length guard (entities without normals read (0,0,0), fall back to flat)
    float nl = length(vNormal);
    if (uLit == 1 && nl > 0.5 && uLightStrength > 0.001)
    {
        vec3 n = vNormal / nl;
        float diff = max(dot(n, normalize(vec3(0.25, -0.92, 0.30))), 0.0);
        c = mix(c, c * (0.45 + 0.55 * diff), clamp(uLightStrength, 0.0, 1.0));
    }
    oColor = vec4(c, 1.0);
}";

    private static int Build(string vs, string fs)
    {
        // GLSL 源码禁止非 ASCII 字符：NVIDIA 编译器遇到中文会 internal corruption（C0000/C9000），
        // 报错只给 <EOF> 无法定位——这里提前拦截并指出具体行，见 docs/BUGS.md（GLSL 中文注释坑）
        CheckAscii(vs, "VertexSrc");
        CheckAscii(fs, "FragSrc");
        int v = GL.CreateShader(ShaderType.VertexShader);
        GL.ShaderSource(v, vs); GL.CompileShader(v);
        int f = GL.CreateShader(ShaderType.FragmentShader);
        GL.ShaderSource(f, fs); GL.CompileShader(f);
        int p = GL.CreateProgram();
        GL.AttachShader(p, v); GL.AttachShader(p, f); GL.LinkProgram(p);
        GL.GetProgram(p, GetProgramParameterName.LinkStatus, out int ok);
        if (ok == 0) throw new Exception("shader link: " + GL.GetProgramInfoLog(p));
        GL.DeleteShader(v); GL.DeleteShader(f);
        return p;
    }

    private static void CheckAscii(string src, string name)
    {
        for (int i = 0; i < src.Length; i++)
            if (src[i] > 127)
                throw new Exception($"{name}: GLSL contains non-ASCII char '{src[i]}' (U+{(int)src[i]:X4}) — Chinese comments break the NVIDIA GLSL compiler");
    }

    public unsafe void Dispose()
    {
        try
        {
            _jobs?.CompleteAdding();
            // worker 退出并释放共享上下文后才能销毁 GL 资源——Join(2000) 超时后必须继续等：
            // 大云（1.2GB 拷贝）在途时提前释放会与 worker 并发写已释放原生内存（堆破坏/AV）
            if (_uploadWorker is not null)
                while (_uploadWorker.IsAlive)
                    if (!_uploadWorker.Join(500)) continue;
        }
        catch { }
        if (_ok)
        {
            try
            {
                _win?.MakeCurrent();
                foreach (var e in _entities.Values)
                {
                    GL.DeleteVertexArray(e.Vao);
                    GL.DeleteBuffer(e.PosVbo);
                    GL.DeleteBuffer(e.ColVbo);
                    GL.DeleteBuffer(e.SelVbo);
                    if (e.NrmVbo != 0) GL.DeleteBuffer(e.NrmVbo);
                    if (e.NativePts != 0) NativeMemory.Free((void*)e.NativePts);
                    if (e.NativeCol != 0) NativeMemory.Free((void*)e.NativeCol);
                }
                _entities.Clear();
                GL.DeleteTexture(_fboTex); GL.DeleteFramebuffer(_fbo); GL.DeleteRenderbuffer(_fboDepth);
                GL.DeleteProgram(_prog);
            }
            catch { }
        }
        try
        {
            unsafe
            {
                if (_uploadWin != null) GLFW.DestroyWindow(_uploadWin);
                _uploadWin = null;
            }
        }
        catch { }
        try { _win?.Dispose(); } catch { }
        _ok = false;
    }
}
