// ComUI 统一 C ABI 适配层（Image2DAlg）：
// 把本库的逐函数 C 接口包装成宿主 native 执行器要求的单一入口签名
// int32 method(const AlgoCInput*, AlgoCOutput*) + algo_free 释放约定（见 docs/THIRD_PARTY_OPS.md §5.5）。
// 输出缓冲一律 malloc（与 algo_free 的 free 配对）。
//
// 节点契约：
//   * 宿主图像恒为 BGRA 紧排（AlgoCImage.bgra）；核心算子的 1/3/4 通道结果
//     由 MakeBgraOut 统一转 BGRA 后输出；
//   * 参数一律按 node.json params 声明顺序 + kind 提取（FindParam 出现次序区分同 kind），
//     声明顺序/默认值/范围以各 node.json 为准（与下方注释逐字对应）；
//   * 测量类节点的结果并入 out->error 消息通道（返回 0 时也可非空，manual_regist 先例；
//     宿主当前仅 rc!=0 时读取——见 docs/HOST_VERIFY.md"待真机确认"）；
//   * 错误消息为 UTF-8 静态存储（宿主只读不释放）。
#include "AlgExport.h"
#include "Image2DAlg/include/Image2DAlg_api.h"
#include "comui_native_abi.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {

// ---------------- 统一释放约定 ----------------

IMAGE2DALG_EXPORT void algo_free(void* p) {
    free(p);
}

// ---------------- 参数读取辅助 ----------------

static double GetDoubleParam(const AlgoCInput* in, double def) {
    for (int i = 0; i < in->paramCount; i++)
        if (in->params[i].kind == 0) return in->params[i].d;
    return def;
}

static const char* GetTextParam(const AlgoCInput* in) {
    for (int i = 0; i < in->paramCount; i++)
        if (in->params[i].kind == 2) return in->params[i].s;
    return nullptr;
}

// 按声明顺序取第 occurrence（0 起）个 kind 类型参数（同 kind 以出现次序区分），
// 不存在返回 nullptr。宿主 marshal 不带参数名，仅按 node.json params 声明顺序 + kind 传值。
static const AlgoCParam* FindParam(const AlgoCInput* in, int kind, int occurrence) {
    int seen = 0;
    for (int i = 0; i < in->paramCount; i++)
        if (in->params[i].kind == kind && seen++ == occurrence) return &in->params[i];
    return nullptr;
}

static AlgoCImageOut* AllocImageOut(int width, int height, uint8_t* bgra) {
    AlgoCImageOut* arr = (AlgoCImageOut*)malloc(sizeof(AlgoCImageOut));
    if (!arr) return nullptr;
    arr->width = width;
    arr->height = height;
    arr->bgra = bgra;
    return arr;
}

// 宿主 BGRA 容器 -> 核心算子输入视图（零拷贝，算子不改动输入）
static Image2DAlg_Image CoreInputFrom(const AlgoCImage& c) {
    Image2DAlg_Image img;
    img.width = c.width;
    img.height = c.height;
    img.channels = 4;                      // 宿主输入恒为 BGRA
    img.data = (unsigned char*)c.bgra;
    return img;
}

// 核心算子结果（1/3/4 通道）-> 宿主 BGRA 紧排（malloc，配 algo_free）
static AlgoCImageOut* MakeBgraOut(const Image2DAlg_Image& img) {
    cv::Mat m(img.height, img.width, CV_8UC(img.channels), img.data);
    cv::Mat bgra;
    if (img.channels == 1)      cv::cvtColor(m, bgra, cv::COLOR_GRAY2BGRA);
    else if (img.channels == 3) cv::cvtColor(m, bgra, cv::COLOR_BGR2BGRA);
    else                        bgra = m;
    if (!bgra.isContinuous()) bgra = bgra.clone();
    const size_t n = (size_t)bgra.cols * bgra.rows * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) return nullptr;
    memcpy(px, bgra.data, n);
    AlgoCImageOut* o = AllocImageOut(bgra.cols, bgra.rows, px);
    if (!o) free(px);
    return o;
}

// ---------------- 算子入口：图像读取（node.json: alg.image2d.read） ----------------
// 参数（按 node.json params 声明顺序）：
//   img_path   path   默认空   图像文件路径（ANSI；png/tiff/jpg/jpeg）
// 输出：BGRA 图像（ReadImage 固定 4 通道）。
// 错误码：1 未设置路径；2 读取失败（路径不存在/格式不支持）；3 内存不足。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiRead(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    const char* path = GetTextParam(in);
    if (!path || !path[0]) { out->error = "未设置图像路径"; return 1; }

    Image2DAlg_Image img = {};
    int rc = Image2DAlg_ReadImage(path, &img);   // 内部已统一转 BGRA 紧排（OpenCV 模式契约）
    if (rc != 0) { out->error = "读取失败：路径不存在或格式不支持（png/tiff/jpg/jpeg）"; return 2; }

    out->images = AllocImageOut(img.width, img.height, img.data);   // data 由 ReadImage malloc，algo_free 配对
    if (!out->images) { Image2DAlg_FreeImage(&img); out->error = "内存不足"; return 3; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：Gamma 校正（node.json: alg.image2d.gamma） ----------------
// 参数（按 node.json params 声明顺序）：
//   gamma   number   默认 1.0   [0.1, 5.0]（< 0.05 由核心层钳制）
// 输出：BGRA（alpha 强制 255，与该节点既有行为一致）。
// 错误码：1 输入未连线；2 处理失败 / 内存不足。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiGamma(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }

    double gamma = GetDoubleParam(in, 1.0);

    const AlgoCImage* src = &in->images[0];

    // 复用核心算子（Gamma 的 LUT 与钳制在 Image2DAlg_Gamma 内实现）
    Image2DAlg_Image input = {};
    input.width = src->width;
    input.height = src->height;
    input.channels = 4;                       // 宿主输入恒为 BGRA
    input.data = (unsigned char*)src->bgra;

    Image2DAlg_Image result = {};
    if (Image2DAlg_Gamma(&input, gamma, &result) != 0) {
        out->error = "Gamma 处理失败";
        return 2;
    }

    // 节点契约：输出图像不透明（与改造前的逐像素实现一致）
    size_t n = (size_t)result.width * (size_t)result.height * 4;
    for (size_t i = 3; i < n; i += 4)
        result.data[i] = 255;

    out->images = AllocImageOut(result.width, result.height, result.data);
    if (!out->images) { Image2DAlg_FreeImage(&result); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：阈值化（node.json: alg.image2d.threshold） ----------------
// 参数（按 node.json params 声明顺序）：
//   mode        int     默认 0    [0,4]    0 固定二值 / 1 反二值 / 2 OTSU / 3 自适应均值 / 4 自适应高斯
//   thresh      number  默认 128  [0,255]  （mode 0/1 有效）
//   maxval      number  默认 255  [0,255]
//   block_size  int     默认 3    [3,51]   自适应窗口（偶数由核心层自动补奇；mode 3/4 有效）
//   c           number  默认 0    [-100,100]（自适应偏移常数；mode 3/4 有效）
// 输出：1 通道结果转 BGRA 显示（前景 = maxval 灰度）。
// 错误码：1 输入未连线；2 处理失败（mode 越界等）。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiThreshold(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int mode = 0, block = 3;
    double thresh = 128.0, maxval = 255.0, c = 0.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) mode = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) thresh = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) maxval = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) block = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) c = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_Threshold(&input, mode, thresh, maxval, block, c, &result) != 0) {
        out->error = "阈值化失败（mode 0~4，block_size 为奇数）";
        return 2;
    }
    out->images = MakeBgraOut(result);       // 1 通道结果转 BGRA 显示
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：模糊/降噪（node.json: alg.image2d.blur） ----------------
// 参数（按声明顺序）：
//   method  int     默认 0  [0,2]    0 高斯 / 1 中值 / 2 双边
//   ksize   int     默认 3  [1,51]   核尺寸（自动补奇；双边 = 邻域直径）
//   sigma1  number  默认 0  [0,100]  高斯 X 方向 sigma；双边 sigmaColor
//   sigma2  number  默认 0  [0,100]  高斯 Y 方向 sigma；双边 sigmaSpace
// 输出：BGRA（通道保持）。
// 错误码：1 输入未连线；2 处理失败。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiBlur(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int method = 0, ksize = 3;
    double sigma1 = 0.0, sigma2 = 0.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) ksize = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) sigma1 = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) sigma2 = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_Blur(&input, method, ksize, sigma1, sigma2, &result) != 0) {
        out->error = "模糊失败（method 0 高斯/1 中值/2 双边）";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：形态学（node.json: alg.image2d.morphology） ----------------
// 参数（按声明顺序）：
//   op          int  默认 2  [0,6]   0 腐蚀 / 1 膨胀 / 2 开 / 3 闭 / 4 梯度 / 5 顶帽 / 6 黑帽
//   shape       int  默认 0  [0,2]   0 矩形 / 1 十字 / 2 椭圆
//   ksize       int  默认 3  [1,51]  结构元边长
//   iterations  int  默认 1  [1,20]  重复次数
// 输出：BGRA（通道保持）。
// 错误码：1 输入未连线；2 处理失败（op/shape 越界等）。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiMorphology(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int op = 2, shape = 0, ksize = 3, iterations = 1;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) op = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) shape = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) ksize = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 3)) iterations = p->i;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_Morphology(&input, op, shape, ksize, iterations, &result) != 0) {
        out->error = "形态学失败（op 0~6，shape 0~2）";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：Canny 边缘（node.json: alg.image2d.canny） ----------------
// 参数（按声明顺序）：
//   threshold1  number  默认 50  [0,1000]  滞后阈值下限
//   threshold2  number  默认 150 [0,1000]  滞后阈值上限（经验取 2~3 倍 threshold1）
// 输出：边缘图转 BGRA（白线黑底）。
// 错误码：1 输入未连线；2 处理失败。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiCanny(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double t1 = 50.0, t2 = 150.0;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) t1 = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) t2 = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_Canny(&input, t1, t2, &result) != 0) {
        out->error = "Canny 失败";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：批板差分（node.json: alg.image2d.absdiff） ----------------
// 输入：2 张图像（images[0] = 基准板，images[1] = 待检板），尺寸/通道必须一致。
// 参数：无。
// 输出：逐像素差的绝对值（BGRA）——再接 threshold 节点即缺陷检出链。
// 错误码：1 输入不足（需 2 张）；2 处理失败（尺寸/通道不一致）。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiAbsDiff(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（基准板/待检板）";
        return 1;
    }
    Image2DAlg_Image a = CoreInputFrom(in->images[0]);
    Image2DAlg_Image b = CoreInputFrom(in->images[1]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_AbsDiff(&a, &b, &result) != 0) {
        out->error = "差分失败（两图尺寸/通道需一致）";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：连通域统计（node.json: alg.image2d.connected_components） ----------------
// 参数（按声明顺序）：
//   connectivity  int     默认 8  [4,8]      邻域定义
//   min_area      number  默认 1  [0,1e7]    面积过滤下限（像素，闭区间）
//   max_area      number  默认 0  [0,1e7]    面积过滤上限（<= 0 不设上限）
// 输出：输入图克隆叠加各 Blob 绿色包围框（BGRA）。
// 消息通道（error 字段，返回 0 时也有效；manual_regist 先例）：
//   "count=N; [i]area=A bbox=(x,y,w,h) c=(cx,cy); ..."（静态 900 字节，超长截断）
// 错误码：1 输入未连线；2 计算失败（connectivity 非法）/ 内存不足。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiConnectedComponents(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int connectivity = 8;
    double min_area = 1.0, max_area = 0.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) connectivity = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) min_area = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) max_area = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Blob* blobs = NULL;
    int count = 0;
    if (Image2DAlg_ConnectedComponents(&input, connectivity,
                                       (int)min_area, (int)max_area, &blobs, &count) != 0) {
        out->error = "连通域计算失败（connectivity 4/8）";
        return 2;
    }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i)
        cv::rectangle(drawn, cv::Rect(blobs[i].left, blobs[i].top, blobs[i].width, blobs[i].height),
                      cv::Scalar(0, 255, 0, 255), 1);

    // 结果并入节点消息（ABI v1 无数值端口，manual_regist 先例）；先组消息再释放
    static char msg[900];   // 静态存储（宿主只读不释放）
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 96; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]area=%d bbox=(%d,%d,%d,%d) c=(%.1f,%.1f)",
                        i, blobs[i].area, blobs[i].left, blobs[i].top,
                        blobs[i].width, blobs[i].height, blobs[i].cx, blobs[i].cy);
    Image2DAlg_FreeBlobs(blobs);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：Sobel 梯度（node.json: alg.image2d.sobel） ----------------
// 参数（按声明顺序）：
//   dx     int  默认 1  [0,2]  X 方向求导阶数
//   dy     int  默认 0  [0,2]  Y 方向求导阶数（dx+dy >= 1）
//   ksize  int  默认 3  [1,7]  核边长 1/3/5/7
// 输出：|梯度| 灰度转 BGRA（边缘亮、平坦黑）。
// 错误码：1 输入未连线；2 处理失败（dx=dy=0 / ksize 非法 → 核心层 -99）。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiSobel(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int dx = 1, dy = 0, ksize = 3;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) dx = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) dy = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) ksize = p->i;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_Sobel(&input, dx, dy, ksize, &result) != 0) {
        out->error = "Sobel 失败（dx+dy>=1，ksize 1/3/5/7）";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：HSV 阈值分割（node.json: alg.image2d.hsv_threshold） ----------------
// 参数（按声明顺序，6 个 number）：
//   h_lo 默认 0 [0,180]   h_hi 默认 180 [0,180]   （红色跨 0 需两节点取并集）
//   s_lo 默认 0 [0,255]   s_hi 默认 255 [0,255]
//   v_lo 默认 0 [0,255]   v_hi 默认 255 [0,255]
// 输出：0/255 掩膜转 BGRA（命中白、未命中黑）。灰度图输入 → 失败。
// 错误码：1 输入未连线；2 处理失败。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiHsvThreshold(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double h_lo = 0, h_hi = 180, s_lo = 0, s_hi = 255, v_lo = 0, v_hi = 255;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) h_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) h_hi = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) s_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) s_hi = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 4)) v_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 5)) v_hi = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_HSVThreshold(&input, h_lo, h_hi, s_lo, s_hi, v_lo, v_hi, &result) != 0) {
        out->error = "HSV 分割失败（需彩色输入）";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：轮廓测量（node.json: alg.image2d.analyze_contours） ----------------
// 参数（按声明顺序）：
//   external_only  int     默认 1  [0,1]  只取最外层轮廓
//   min_area       number  默认 1  [0,1e7] 面积过滤（像素²）
// 输出：输入图克隆叠加标注——绿色包围框 + 黄色最小外接圆。
// 消息通道："count=N; [i]area=A circ=C bbox=(x,y,w,h); ..."（900 字节截断）
// 错误码：1 输入未连线；2 计算失败/内存不足。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiAnalyzeContours(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int external_only = 1;
    double min_area = 1.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) external_only = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) min_area = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_ContourInfo* infos = NULL;
    int count = 0;
    if (Image2DAlg_AnalyzeContours(&input, external_only, (int)min_area, &infos, &count) != 0) {
        out->error = "轮廓测量失败";
        return 2;
    }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i) {
        cv::rectangle(drawn, cv::Rect(infos[i].bbox_x, infos[i].bbox_y,
                                      infos[i].bbox_w, infos[i].bbox_h),
                      cv::Scalar(0, 255, 0, 255), 1);
        cv::circle(drawn, cv::Point2f((float)infos[i].circle_cx, (float)infos[i].circle_cy),
                   (float)infos[i].circle_r, cv::Scalar(0, 255, 255, 255), 1, cv::LINE_AA);
    }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 96; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]area=%.1f circ=%.3f bbox=(%d,%d,%d,%d)",
                        i, infos[i].area, infos[i].circularity,
                        infos[i].bbox_x, infos[i].bbox_y, infos[i].bbox_w, infos[i].bbox_h);
    Image2DAlg_FreeContours(infos);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：多目标圆拟合（node.json: alg.image2d.fit_circles） ----------------
// 参数（按声明顺序）：
//   min_area   number  默认 10  [0,1e7]   面积过滤下限
//   max_area   number  默认 0   [0,1e7]   面积过滤上限（<=0 不限）
//   min_r      number  默认 0   [0,1e4]   半径过滤下限（像素）
//   max_r      number  默认 0   [0,1e4]   半径过滤上限（<=0 不限）
//   subpixel   int     默认 1   [0,1]     灰度梯度亚像素细化（~0.3 像素精度）
// 输入：二值语义图（非零为前景；彩色先转灰度）。
// 输出：输入图克隆叠加拟合圆（绿色圆周）。
// 消息通道："count=N; [i]c=(cx,cy) r=R rms=RMS; ..."（900 字节截断）
// 错误码：1 输入未连线；2 拟合失败/内存不足。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiFitCircles(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double min_area = 10.0, max_area = 0.0, min_r = 0.0, max_r = 0.0;
    int subpixel = 1;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) min_area = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) max_area = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) min_r = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) max_r = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) subpixel = p->i;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_CircleFit* fits = NULL;
    int count = 0;
    if (Image2DAlg_FitCirclesFromBinary(&input, (int)min_area, (int)max_area,
                                        min_r, max_r, subpixel, &fits, &count) != 0) {
        out->error = "圆拟合失败";
        return 2;
    }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i)
        cv::circle(drawn, cv::Point2f((float)fits[i].cx, (float)fits[i].cy),
                   (float)fits[i].r, cv::Scalar(0, 255, 0, 255), 1, cv::LINE_AA);

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 96; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]c=(%.2f,%.2f) r=%.2f rms=%.3f",
                        i, fits[i].cx, fits[i].cy, fits[i].r, fits[i].rms);
    Image2DAlg_FreeCircles(fits);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：灰度化（node.json: alg.image2d.to_gray） ----------------
// 参数：无。输出：灰度图转 BGRA 显示。错误码：1 输入未连线；2 处理失败。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiToGray(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_ToGray(&input, &result) != 0) {
        out->error = "灰度化失败";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：缩放（node.json: alg.image2d.resize） ----------------
// 参数（按声明顺序）：
//   out_width      int     默认 640 [1,10000]
//   out_height     int     默认 480 [1,10000]
//   interpolation  int     默认 1   [0,4]  0 近邻 / 1 线性 / 2 立方 / 3 面积 / 4 Lanczos4
// 输出：BGRA（通道保持）。错误码：1 输入未连线；2 处理失败（尺寸 <= 0）。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiResize(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int w = 640, h = 480, interp = 1;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) w = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) h = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) interp = p->i;
    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_Resize(&input, w, h, interp, &result) != 0) {
        out->error = "缩放失败（目标尺寸需 > 0）";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：裁剪（node.json: alg.image2d.crop） ----------------
// 参数（按声明顺序）：
//   x/y      int  默认 0   [0,100000]  左上角
//   width    int  默认 100 [1,100000]  宽（越界自动钳制到图内）
//   height   int  默认 100 [1,100000]  高
// 输出：BGRA（通道保持）。错误码：1 输入未连线；2 处理失败（与图像无交集）。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiCrop(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int x = 0, y = 0, w = 100, h = 100;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) x = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) y = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) w = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 3)) h = p->i;
    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_Crop(&input, x, y, w, h, &result) != 0) {
        out->error = "裁剪失败（ROI 与图像无交集）";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：旋转（node.json: alg.image2d.rotate） ----------------
// 参数（按声明顺序）：
//   angle          number  默认 0  [-360,360]  逆时针角度（度）
//   scale          number  默认 1  [0.01,10]
//   interpolation  int     默认 1  [0,4]
// 输出：与输入同尺寸 BGRA（越界部分填黑）。错误码：1 输入未连线；2 处理失败。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiRotate(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double angle = 0.0, scale = 1.0;
    int interp = 1;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) angle = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) scale = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) interp = p->i;
    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_Rotate(&input, angle, scale, interp, &result) != 0) {
        out->error = "旋转失败";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：灰度增强（node.json: alg.image2d.enhance） ----------------
// 参数（按声明顺序）：
//   method      int     默认 1  [0,1]   0 直方图均衡 / 1 CLAHE
//   clip_limit  number  默认 2  [0.1,10] CLAHE 对比度上限
//   tile_size   int     默认 8  [2,64]  CLAHE 分块边长
// 输出：BGRA（method 0 输出灰度复制三通道；method 1 保持通道）。
// 错误码：1 输入未连线；2 处理失败。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiEnhance(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int method = 1, tile = 8;
    double clip = 2.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) clip = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) tile = p->i;
    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_Enhance(&input, method, clip, tile, &result) != 0) {
        out->error = "增强失败";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：霍夫直线（node.json: alg.image2d.hough_lines） ----------------
// 参数（按声明顺序）：
//   rho             number  默认 1   [0.1,10]   距离分辨率（像素）
//   theta_deg       number  默认 1   [0.1,10]   角度分辨率（度）
//   threshold       int     默认 50  [1,10000]  投票阈值
//   min_line_length int     默认 20  [0,10000]  最短线长（0 不限）
//   max_line_gap    int     默认 5   [0,10000]  线最大断距（0 不限）
// 输入：二值/边缘图（链路上先接 Canny/Sobel/Threshold）。
// 输出：输入图克隆叠加检出线段（绿色）。
// 消息通道："count=N; [i](x1,y1)-(x2,y2); ..."（900 字节截断）。
// 错误码：1 输入未连线；2 检测失败；3 参数非法。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiHoughLines(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double rho = 1.0, theta = 1.0;
    int threshold = 50, min_len = 20, max_gap = 5;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) rho = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) theta = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) threshold = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) min_len = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) max_gap = p->i;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Line4* lines = NULL;
    int count = 0;
    const int rc = Image2DAlg_HoughLinesP(&input, rho, theta, threshold, min_len, max_gap,
                                          &lines, &count);
    if (rc == -3) { out->error = "霍夫直线失败（rho/theta 需 > 0）"; return 3; }
    if (rc != 0) { out->error = "霍夫直线失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i)
        cv::line(drawn, cv::Point(lines[i].x1, lines[i].y1),
                 cv::Point(lines[i].x2, lines[i].y2), cv::Scalar(0, 255, 0, 255), 1);

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 64; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d](%d,%d)-(%d,%d)",
                        i, lines[i].x1, lines[i].y1, lines[i].x2, lines[i].y2);
    Image2DAlg_FreeLines(lines);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：霍夫圆（node.json: alg.image2d.hough_circles） ----------------
// 参数（按声明顺序）：
//   dp        number  默认 1   [0.1,4]     分辨率倒数
//   min_dist  number  默认 10  [1,10000]   圆心最小间距
//   param1    number  默认 50  [1,1000]    内部 Canny 高阈值（模糊后边缘梯度
//                                             偏弱，实测 100 会漏检合成圆）
//   param2    number  默认 20  [1,1000]    圆心投票阈值（越小圆越多）
//   min_r     number  默认 5   [0,10000]   最小半径
//   max_r     number  默认 0   [0,10000]   最大半径（<=0 不限）
// 内部自动 3x3 高斯预平滑（HOUGH_GRADIENT 对锐利二值图可能无果）。
// 输出：平滑后检出的圆叠加在原输入图上（绿色圆周）。
// 消息通道："count=N; [i]c=(cx,cy) r=R; ..."（900 字节截断）。
// 错误码：1 输入未连线；2 检测失败；3 参数非法。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiHoughCircles(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double dp = 1.0, min_dist = 10.0, param1 = 50.0, param2 = 20.0, min_r = 5.0, max_r = 0.0;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) dp = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) min_dist = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) param1 = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) param2 = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 4)) min_r = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 5)) max_r = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image blurred = {};   // HOUGH_GRADIENT 需平滑输入（锐利二值图可能无果）
    if (Image2DAlg_Blur(&input, 0, 3, 0, 0, &blurred) != 0) {
        out->error = "预平滑失败";
        return 2;
    }
    Image2DAlg_HoughCircle* circles = NULL;
    int count = 0;
    const int rc = Image2DAlg_HoughCircles(&blurred, dp, min_dist, param1, param2,
                                           min_r, max_r, &circles, &count);
    Image2DAlg_FreeImage(&blurred);
    if (rc == -3) { out->error = "霍夫圆失败（dp/min_dist 需 > 0）"; return 3; }
    if (rc != 0) { out->error = "霍夫圆失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i)
        cv::circle(drawn, cv::Point2f((float)circles[i].cx, (float)circles[i].cy),
                   (float)circles[i].r, cv::Scalar(0, 255, 0, 255), 1, cv::LINE_AA);

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 64; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]c=(%.1f,%.1f) r=%.1f",
                        i, circles[i].cx, circles[i].cy, circles[i].r);
    Image2DAlg_FreeCirclesHough(circles);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：Mark 点对位（node.json: alg.image2d.match_template_ex） ----------------
// 输入 2 张图像：images[0]=搜索图，images[1]=模板。
// 参数（按声明顺序）：
//   method      int     默认 5  [0,5]    cv::TemplateMatchModes（5=CCOEFF_NORMED 归一化相关）
//   scale_lo    number  默认 0.8 [0.01,10] 尺度下限
//   scale_hi    number  默认 1.2 [0.01,10] 尺度上限
//   scale_step  number  默认 0.05 [0.001,10]
//   angle_lo    number  默认 -15 [-360,360] 角度下限（度，逆时针）
//   angle_hi    number  默认 15  [-360,360] 角度上限
//   angle_step  number  默认 1   [0.01,90]
// 输出：搜索图克隆叠加中心十字标注（绿色）。
// 消息通道："score=%.4f c=(%.1f,%.1f) scale=%.2f angle=%.1f"（模板中心落点，亚像素）。
// 错误码：1 输入不足（需 2 张）；2 搜索失败（参数/尺寸非法）；3 任何尺度模板越界。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiMatchTemplateEx(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（搜索图/模板）";
        return 1;
    }
    int method = 5;
    double scale_lo = 0.8, scale_hi = 1.2, scale_step = 0.05;
    double angle_lo = -15.0, angle_hi = 15.0, angle_step = 1.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) scale_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) scale_hi = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) scale_step = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) angle_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 4)) angle_hi = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 5)) angle_step = p->d;

    Image2DAlg_Image img = CoreInputFrom(in->images[0]);
    Image2DAlg_Image tpl = CoreInputFrom(in->images[1]);
    double score = 0, cx = 0, cy = 0, scale = 0, angle = 0;
    const int rc = Image2DAlg_MatchTemplateEx(&img, &tpl, method,
                                              scale_lo, scale_hi, scale_step,
                                              angle_lo, angle_hi, angle_step,
                                              &score, &cx, &cy, &scale, &angle);
    if (rc == -3) { out->error = "对位失败（参数非法或任何尺度模板越界）"; return 3; }
    if (rc != 0) { out->error = "对位失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    const cv::Point c((int)std::lround(cx), (int)std::lround(cy));
    cv::line(drawn, c + cv::Point(-8, 0), c + cv::Point(8, 0), cv::Scalar(0, 255, 0, 255), 1);
    cv::line(drawn, c + cv::Point(0, -8), c + cv::Point(0, 8), cv::Scalar(0, 255, 0, 255), 1);

    static char msg[160];
    snprintf(msg, sizeof(msg), "score=%.4f c=(%.1f,%.1f) scale=%.2f angle=%.1f",
             score, cx, cy, scale, angle);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：多目标模板匹配（node.json: alg.image2d.find_matches） ----------------
// 输入 2 张图像：images[0]=搜索图，images[1]=模板。
// 参数（按声明顺序）：
//   method          int     默认 5   [0,5]
//   score_threshold number  默认 0.8 [0,1]   SQDIFF 系 = "得分<=阈值"命中，其余 = ">="
//   max_matches     int     默认 10  [1,100]  最多输出数
//   nms_radius      int     默认 5   [0,100]  非极大值抑制半径（像素）
// 输出：搜索图克隆叠加各命中位置的模板外框（绿色）。
// 消息通道："count=N; [i](x,y) score=S; ..."（左上角坐标，按得分从优到劣）。
// 错误码：1 输入不足；2 匹配失败；3 参数非法。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiFindMatches(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（搜索图/模板）";
        return 1;
    }
    int method = 5, max_matches = 10, nms = 5;
    double thr = 0.8;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) thr = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) max_matches = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) nms = p->i;

    Image2DAlg_Image img = CoreInputFrom(in->images[0]);
    Image2DAlg_Image tpl = CoreInputFrom(in->images[1]);
    Image2DAlg_Match* matches = NULL;
    int count = 0;
    const int rc = Image2DAlg_FindAllMatches(&img, &tpl, method, thr, max_matches, nms,
                                             &matches, &count);
    if (rc == -3) { out->error = "匹配失败（method/数量/半径参数非法或模板越界）"; return 3; }
    if (rc != 0) { out->error = "匹配失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i)
        cv::rectangle(drawn, cv::Rect((int)matches[i].x, (int)matches[i].y,
                                      in->images[1].width, in->images[1].height),
                      cv::Scalar(0, 255, 0, 255), 1);

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 64; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d](%d,%d) score=%.4f",
                        i, (int)matches[i].x, (int)matches[i].y, matches[i].score);
    Image2DAlg_FreeMatches(matches);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：HSV 分割 wrap 版（node.json: alg.image2d.hsv_threshold_wrap） ----------------
// 参数（按声明顺序，6 个 number，同 hsv_threshold）：
//   h_lo/h_hi/s_lo/s_hi/v_lo/v_hi
// 与 hsv_threshold 的差别：h_lo > h_hi 自动按环绕区间 [h_lo,180]∪[0,h_hi]，
// 红色跨 0 无需两节点取并集；h_lo <= h_hi 时语义完全相同。
// 错误码：1 输入未连线；2 处理失败（灰度图等）。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiHsvThresholdWrap(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double h_lo = 0, h_hi = 180, s_lo = 0, s_hi = 255, v_lo = 0, v_hi = 255;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) h_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) h_hi = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) s_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) s_hi = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 4)) v_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 5)) v_hi = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    if (Image2DAlg_HSVThresholdWrap(&input, h_lo, h_hi, s_lo, s_hi, v_lo, v_hi, &result) != 0) {
        out->error = "HSV 分割失败（需彩色输入）";
        return 2;
    }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：连通域 ROI（node.json: alg.image2d.connected_components_roi） ----------------
// 参数（按声明顺序）：
//   roi_x/roi_y      int     默认 0 [0,100000]  ROI 左上角
//   roi_w/roi_h      int     默认 0 [0,100000]  ROI 尺寸（<=0 = 全图）
//   connectivity     int     默认 8 [4,8]
//   min_area         number  默认 1 [0,1e7]
//   max_area         number  默认 0 [0,1e7]     （<=0 不设上限）
// 输出：ROI 视图叠加绿框标注（结果坐标为全图系）。
// 消息通道：同 connected_components（count/area/bbox/质心为全图坐标）。
// 错误码：1 输入未连线；2 计算失败；3 ROI 与图像无交集。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiConnectedComponentsRoi(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int rx = 0, ry = 0, rw = 0, rh = 0, connectivity = 8;
    double min_area = 1.0, max_area = 0.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) rx = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) ry = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) rw = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 3)) rh = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 4)) connectivity = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) min_area = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) max_area = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Blob* blobs = NULL;
    int count = 0;
    const int rc = Image2DAlg_ConnectedComponentsROI(&input, rx, ry, rw, rh, connectivity,
                                                     (int)min_area, (int)max_area,
                                                     &blobs, &count);
    if (rc == -3) { out->error = "连通域失败（connectivity 非法或 ROI 与图像无交集）"; return 3; }
    if (rc != 0) { out->error = "连通域计算失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i)
        cv::rectangle(drawn, cv::Rect(blobs[i].left, blobs[i].top,
                                      blobs[i].width, blobs[i].height),
                      cv::Scalar(0, 255, 0, 255), 1);

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 96; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]area=%d bbox=(%d,%d,%d,%d) c=(%.1f,%.1f)",
                        i, blobs[i].area, blobs[i].left, blobs[i].top,
                        blobs[i].width, blobs[i].height, blobs[i].cx, blobs[i].cy);
    Image2DAlg_FreeBlobs(blobs);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：轮廓测量 ROI（node.json: alg.image2d.analyze_contours_roi） ----------------
// 参数（按声明顺序）：
//   roi_x/roi_y     int     默认 0 [0,100000]
//   roi_w/roi_h     int     默认 0 [0,100000]  （<=0 = 全图）
//   external_only   int     默认 1 [0,1]
//   min_area        number  默认 1 [0,1e7]
// 输出：ROI 视图叠加绿框+黄最小外接圆（结果坐标为全图系）。
// 消息通道：同 analyze_contours（count/area/circularity/bbox 为全图坐标）。
// 错误码：1 输入未连线；2 计算失败；3 ROI 与图像无交集。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiAnalyzeContoursRoi(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int rx = 0, ry = 0, rw = 0, rh = 0, external_only = 1;
    double min_area = 1.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) rx = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) ry = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) rw = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 3)) rh = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 4)) external_only = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) min_area = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_ContourInfo* infos = NULL;
    int count = 0;
    const int rc = Image2DAlg_AnalyzeContoursROI(&input, rx, ry, rw, rh, external_only,
                                                 (int)min_area, &infos, &count);
    if (rc == -3) { out->error = "轮廓测量失败（ROI 与图像无交集）"; return 3; }
    if (rc != 0) { out->error = "轮廓测量失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i) {
        cv::rectangle(drawn, cv::Rect(infos[i].bbox_x, infos[i].bbox_y,
                                      infos[i].bbox_w, infos[i].bbox_h),
                      cv::Scalar(0, 255, 0, 255), 1);
        cv::circle(drawn, cv::Point2f((float)infos[i].circle_cx, (float)infos[i].circle_cy),
                   (float)infos[i].circle_r, cv::Scalar(0, 255, 255, 255), 1, cv::LINE_AA);
    }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 96; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]area=%.1f circ=%.3f bbox=(%d,%d,%d,%d)",
                        i, infos[i].area, infos[i].circularity,
                        infos[i].bbox_x, infos[i].bbox_y, infos[i].bbox_w, infos[i].bbox_h);
    Image2DAlg_FreeContours(infos);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：掩膜模板匹配（node.json: alg.image2d.match_template_masked） ----------------
// 输入 3 张图像：images[0]=搜索图，images[1]=模板，images[2]=掩膜（1 通道语义，
// 非 0 = 参与匹配；宿主传 BGRA 时核心层按 4 通道视图处理会 -3——掩膜节点约定
// 掩膜图必须为灰度 BGRA（B=G=R=掩膜值））。
// 参数（按声明顺序）：method（int，默认 3=CCORR_NORMED [0,5]，仅 0/3 支持）。
// 输出：搜索图克隆叠加模板外框（绿色）。
// 消息通道："score=%.4f at=(x,y)"。
// 错误码：1 输入不足；2 匹配失败；3 参数/掩膜非法。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiMatchTemplateMasked(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 3 || !in->images || !in->images[1].bgra || !in->images[2].bgra) {
        out->error = "需要 3 个图像输入（搜索图/模板/掩膜）";
        return 1;
    }
    int method = 3;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;

    Image2DAlg_Image img = CoreInputFrom(in->images[0]);
    Image2DAlg_Image tpl = CoreInputFrom(in->images[1]);
    // 掩膜宿主侧为 BGRA（B=G=R 灰度语义），适配层转单通道后交核心（核心只收 1 通道掩膜）
    cv::Mat mkVis(in->images[2].height, in->images[2].width, CV_8UC4,
                  (void*)in->images[2].bgra);
    cv::Mat mkGray;
    cv::cvtColor(mkVis, mkGray, cv::COLOR_BGRA2GRAY);
    Image2DAlg_Image mk = {};
    mk.width = in->images[2].width;
    mk.height = in->images[2].height;
    mk.channels = 1;
    mk.data = mkGray.data;
    double score = 0;
    int x = -1, y = -1;
    const int rc = Image2DAlg_MatchTemplateMasked(&img, &tpl, &mk, method, &score, &x, &y);
    if (rc == -3) { out->error = "掩膜匹配失败（method 仅 0/3、掩膜须单通道语义、尺寸不一致）"; return 3; }
    if (rc != 0) { out->error = "掩膜匹配失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    cv::rectangle(drawn, cv::Rect(x, y, in->images[1].width, in->images[1].height),
                  cv::Scalar(0, 255, 0, 255), 1);

    static char msg[128];
    snprintf(msg, sizeof(msg), "score=%.4f at=(%d,%d)", score, x, y);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：Mark 阵列全检（node.json: alg.image2d.find_matches_ex） ----------------
// 输入 2 张图像：images[0]=搜索图，images[1]=模板。
// 多目标 × 旋转 × 尺度组合搜索（跨组合全局 NMS），按得分从优到劣输出。
// 参数（按声明顺序）：
//   method          int     默认 5    [0,5]
//   score_threshold number  默认 0.6  [0,1]    SQDIFF 系 = "得分<=阈值"，其余 = ">="
//   max_matches     int     默认 50   [1,200]  最多输出数
//   nms_radius      int     默认 10   [0,200]  全局 NMS 半径（模板中心距，像素）
//   scale_lo        number  默认 0.9  [0.01,10]
//   scale_hi        number  默认 1.1  [0.01,10]
//   scale_step      number  默认 0.05 [0.001,10]
//   angle_lo        number  默认 -5   [-360,360]
//   angle_hi        number  默认 5    [-360,360]
//   angle_step      number  默认 1    [0.01,90]
// 输出：搜索图克隆叠加各目标中心十字标注（绿色）。
// 消息通道："count=N; [i]c=(cx,cy) s=S a=A score=Q; ..."（900 字节截断）。
// 代价 = 尺度数 × 角度数 次全图匹配——范围勿开过大。
// 错误码：1 输入不足；2 搜索失败；3 参数非法/全部越界。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiFindMatchesEx(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（搜索图/模板）";
        return 1;
    }
    int method = 5, max_matches = 50, nms = 10;
    double thr = 0.6;
    double scale_lo = 0.9, scale_hi = 1.1, scale_step = 0.05;
    double angle_lo = -5.0, angle_hi = 5.0, angle_step = 1.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) thr = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) max_matches = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) nms = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) scale_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) scale_hi = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) scale_step = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 4)) angle_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 5)) angle_hi = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 6)) angle_step = p->d;

    Image2DAlg_Image img = CoreInputFrom(in->images[0]);
    Image2DAlg_Image tpl = CoreInputFrom(in->images[1]);
    Image2DAlg_MatchEx* matches = NULL;
    int count = 0;
    const int rc = Image2DAlg_FindAllMatchesEx(&img, &tpl, method, thr, max_matches, nms,
                                               scale_lo, scale_hi, scale_step,
                                               angle_lo, angle_hi, angle_step,
                                               &matches, &count);
    if (rc == -3) { out->error = "阵列全检失败（method/数量/半径/尺度/角度参数非法）"; return 3; }
    if (rc != 0) { out->error = "阵列全检失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i) {
        const cv::Point c((int)std::lround(matches[i].cx), (int)std::lround(matches[i].cy));
        cv::line(drawn, c + cv::Point(-8, 0), c + cv::Point(8, 0),
                 cv::Scalar(0, 255, 0, 255), 1);
        cv::line(drawn, c + cv::Point(0, -8), c + cv::Point(0, 8),
                 cv::Scalar(0, 255, 0, 255), 1);
    }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 96; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]c=(%.1f,%.1f) s=%.2f a=%.1f score=%.4f",
                        i, matches[i].cx, matches[i].cy,
                        matches[i].scale, matches[i].angle, matches[i].score);
    Image2DAlg_FreeMatchesEx(matches);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：多目标椭圆拟合（node.json: alg.image2d.fit_ellipses） ----------------
// 输入：二值语义图（非零为前景；彩色先转灰度）。
// 参数（按声明顺序）：
//   min_area   number  默认 10  [0,1e7]   面积过滤下限
//   max_area   number  默认 0   [0,1e7]   面积过滤上限（<=0 不限）
//   min_axis   number  默认 0   [0,1e4]   全轴长过滤下限（像素）
//   max_axis   number  默认 0   [0,1e4]   全轴长过滤上限（<=0 不限）
// 输出：输入图克隆叠加拟合椭圆（绿色，cv::ellipse 画线）。
// 消息通道："count=N; [i]c=(cx,cy) axis=(W,H) ang=A; ..."（900 字节截断）。
// 错误码：1 输入未连线；2 拟合失败；3 参数非法。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiFitEllipses(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double min_area = 10.0, max_area = 0.0, min_axis = 0.0, max_axis = 0.0;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) min_area = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) max_area = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) min_axis = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) max_axis = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_EllipseFit* fits = NULL;
    int count = 0;
    const int rc = Image2DAlg_FitEllipsesFromBinary(&input, (int)min_area, (int)max_area,
                                                    min_axis, max_axis, &fits, &count);
    if (rc != 0) { out->error = "椭圆拟合失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i)
        cv::ellipse(drawn, cv::Point2f((float)fits[i].cx, (float)fits[i].cy),
                    cv::Size2f((float)(fits[i].width / 2), (float)(fits[i].height / 2)),
                    fits[i].angle, 0, 360, cv::Scalar(0, 255, 0, 255), 1, cv::LINE_AA);

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 96; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]c=(%.1f,%.1f) axis=(%.1f,%.1f) ang=%.1f",
                        i, fits[i].cx, fits[i].cy, fits[i].width, fits[i].height, fits[i].angle);
    Image2DAlg_FreeEllipses(fits);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：亚像素对位（node.json: alg.image2d.match_template_subpix） ----------------
// 输入 2 张图像：images[0]=搜索图，images[1]=模板。
// 参数（按声明顺序）：method（int，默认 5 [0,5]）。
// 输出：搜索图克隆叠加中心十字标注（绿色）。
// 消息通道："score=%.4f at=(%.2f,%.2f)"（亚像素左上角）。
// 错误码：1 输入不足；2 匹配失败；3 参数非法。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiMatchTemplateSubPix(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（搜索图/模板）";
        return 1;
    }
    int method = 5;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;

    Image2DAlg_Image img = CoreInputFrom(in->images[0]);
    Image2DAlg_Image tpl = CoreInputFrom(in->images[1]);
    double score = 0, sx = 0, sy = 0;
    const int rc = Image2DAlg_MatchTemplateSubPix(&img, &tpl, method, &score, &sx, &sy);
    if (rc == -3) { out->error = "亚像素匹配失败（method 非法或模板越界）"; return 3; }
    if (rc != 0) { out->error = "亚像素匹配失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    const cv::Point c((int)std::lround(sx), (int)std::lround(sy));
    cv::line(drawn, c + cv::Point(-8, 0), c + cv::Point(8, 0), cv::Scalar(0, 255, 0, 255), 1);
    cv::line(drawn, c + cv::Point(0, -8), c + cv::Point(0, 8), cv::Scalar(0, 255, 0, 255), 1);

    static char msg[128];
    snprintf(msg, sizeof(msg), "score=%.4f at=(%.2f,%.2f)", score, sx, sy);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { out->error = "内存不足"; return 2; }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) { free(px); out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：区域灰度统计（node.json: alg.image2d.region_stats） ----------------
// 参数（按声明顺序）：roi_x/roi_y/roi_w/roi_h（4 个 int；w/h <= 0 = 全图）。
// 输入：灰度语义图（彩色先转灰度）。
// 输出：无图像端口（outputs 为空）；结果并入消息通道：
//   "mean=%.2f stddev=%.2f min=%d max=%d"（批次一致性监控）。
// 错误码：1 输入未连线；2 统计失败；3 ROI 与图像无交集。

IMAGE2DALG_EXPORT int Image2DAlg_ComuiRegionStats(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int rx = 0, ry = 0, rw = 0, rh = 0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) rx = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) ry = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) rw = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 3)) rh = p->i;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_GrayStats st = {};
    const int rc = Image2DAlg_RegionStats(&input, rx, ry, rw, rh, &st);
    if (rc == -3) { out->error = "统计失败（ROI 与图像无交集）"; return 3; }
    if (rc != 0) { out->error = "区域统计失败"; return 2; }

    static char msg[128];
    snprintf(msg, sizeof(msg), "mean=%.2f stddev=%.2f min=%d max=%d",
             st.mean, st.stddev, st.min, st.max);
    out->error = msg;
    return 0;
}

} // extern "C"
