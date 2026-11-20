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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.read","name":"图像读取","group":"2D 图像","category":"源","description":"读取图像（PNG/TIFF/JPG/JPEG）输出BGRA"},
//   "inputs": [],
//   "params": [
//     {"name":"img_path","label":"图像路径","type":"path","default":"","description":"图像文件路径（png/tiff/jpg/jpeg，ANSI 编码）；错误码 1=未设置路径、2=读取失败"}
//   ],
//   "outputs": [
//     {"name":"image","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.gamma","name":"Gamma 校正","group":"2D 图像","category":"处理","description":"图像 Gamma 非线性校正（>1 提亮、<1 压暗，LUT 实现）；输出 alpha 强制 255"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"gamma","label":"Gamma 值","type":"number","default":1.0,"min":0.1,"max":5.0,"description":"Gamma 系数：>1 提亮、<1 压暗、=1 原图（<0.05 核心层钳制；LUT 实现）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.threshold","name":"阈值化","group":"2D 图像","category":"处理","description":"灰度阈值二值化：mode 0 固定二值/1 反二值/2 OTSU/3 自适应均值/4 自适应高斯"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"mode","label":"模式","type":"int","default":0,"min":0,"max":4,"description":"0 固定二值 / 1 反二值 / 2 OTSU / 3 自适应均值 / 4 自适应高斯"},
//     {"name":"thresh","label":"阈值","type":"number","default":128,"min":0,"max":255,"description":"固定阈值（仅 mode 0/1 有效）"},
//     {"name":"maxval","label":"最大值","type":"number","default":255,"min":0,"max":255,"description":"前景赋值灰度"},
//     {"name":"block_size","label":"自适应窗口","type":"int","default":3,"min":3,"max":51,"description":"自适应窗口边长（偶数自动补奇；仅 mode 3/4 有效）"},
//     {"name":"c","label":"自适应偏移C","type":"number","default":0,"min":-100,"max":100,"description":"自适应偏移常数（仅 mode 3/4 有效）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// ---------------- 算子入口：统一二值化（node.json: alg.image2d.binarize） ----------------
// 参数（按 node.json params 声明顺序 + kind，同名 kind 用 FindParam occurrence 区分）：
//   mode       int     默认 0   [0,10]   0 二值/1 反二值/2 OTSU/3 自适应均值/4 自适应高斯/
//                                        5 TRUNC/6 TOZERO/7 TOZERO_INV/8 双阈值区间内/
//                                        9 双阈值区间外/10 双阈值三值化
//   thresh     number  默认 128 [0,255]  固定阈值；双阈值时为下限 low
//   thresh2    number  默认 200 [0,255]  双阈值上限 high（>= thresh）
//   maxval     number  默认 255 [0,255]  前景赋值（mode 10 为高值端）
//   mid_val    number  默认 128 [0,255]  三值化中间值（仅 mode 10）
//   block_size int     默认 3   [3,51]   自适应窗口（自动补奇）
//   c          number  默认 0   [-100,100] 自适应偏移
//   invert     int     默认 0   [0,1]    非 0 对结果取反
// 输出：BGRA（1 通道结果转 BGRA 显示）。错误码：1 输入未连线；2/3 处理失败。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.binarize","name":"二值化","group":"2D 图像","category":"处理","description":"统一二值化：0 二值/1 反二值/2 OTSU/3 自适应均值/4 自适应高斯/5 TRUNC/6 TOZERO/7 TOZERO_INV/8 双阈值区间内/9 双阈值区间外/10 双阈值三值化；支持反转"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"mode","label":"模式","type":"int","default":0,"min":0,"max":10,"description":"0 二值 / 1 反二值 / 2 OTSU / 3 自适应均值 / 4 自适应高斯 / 5 TRUNC / 6 TOZERO / 7 TOZERO_INV / 8 双阈值区间内 / 9 双阈值区间外 / 10 双阈值三值化"},
//     {"name":"thresh","label":"阈值/下限low","type":"number","default":128,"min":0,"max":255,"description":"固定阈值；双阈值模式时为下限 low"},
//     {"name":"thresh2","label":"上限high","type":"number","default":200,"min":0,"max":255,"description":"双阈值上限 high（须 ≥ low）"},
//     {"name":"maxval","label":"最大值","type":"number","default":255,"min":0,"max":255,"description":"前景赋值（mode 10 为高值端）"},
//     {"name":"mid_val","label":"三值中间值","type":"number","default":128,"min":0,"max":255,"description":"三值化中间灰度（仅 mode 10 有效）"},
//     {"name":"block_size","label":"自适应窗口","type":"int","default":3,"min":3,"max":51,"description":"自适应窗口边长（自动补奇；mode 3/4 有效）"},
//     {"name":"c","label":"自适应偏移C","type":"number","default":0,"min":-100,"max":100,"description":"自适应偏移常数（mode 3/4 有效）"},
//     {"name":"invert","label":"反转","type":"int","default":0,"min":0,"max":1,"description":"非 0 对结果取反"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiBinarize(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int mode = 0, block = 3, invert = 0;
    double thresh = 128.0, thresh2 = 200.0, maxval = 255.0, mid = 128.0, c = 0.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) mode = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) thresh = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) thresh2 = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) maxval = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) mid = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) block = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 4)) c = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) invert = p->i;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    const int rc = Image2DAlg_Binarize(&input, mode, thresh, thresh2, maxval, mid,
                                       block, c, invert, &result);
    if (rc == -3) { out->error = "二值化失败（mode 0~10；双阈值 high 须 >= low）"; return 3; }
    if (rc != 0) { out->error = "二值化失败"; return 2; }
    out->images = MakeBgraOut(result);
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.blur","name":"模糊降噪","group":"2D 图像","category":"处理","description":"模糊降噪：0 高斯 / 1 中值 / 2 双边（通道保持）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"method","label":"方法","type":"int","default":0,"min":0,"max":2,"description":"0 高斯 / 1 中值 / 2 双边"},
//     {"name":"ksize","label":"核尺寸","type":"int","default":3,"min":1,"max":51,"description":"核尺寸（自动补奇；双边滤波时=邻域直径）"},
//     {"name":"sigma1","label":"Sigma1","type":"number","default":0,"min":0,"max":100,"description":"高斯 X 方向 sigma；双边滤波时=颜色 sigma"},
//     {"name":"sigma2","label":"Sigma2","type":"number","default":0,"min":0,"max":100,"description":"高斯 Y 方向 sigma；双边滤波时=空间 sigma"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.morphology","name":"形态学","group":"2D 图像","category":"处理","description":"op 0 腐蚀/1 膨胀/2 开/3 闭/4 梯度/5 顶帽/6 黑帽；shape 0 矩形/1 十字/2 椭圆"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"op","label":"操作","type":"int","default":2,"min":0,"max":6,"description":"0 腐蚀 / 1 膨胀 / 2 开 / 3 闭 / 4 梯度 / 5 顶帽 / 6 黑帽"},
//     {"name":"shape","label":"结构元","type":"int","default":0,"min":0,"max":2,"description":"结构元形状：0 矩形 / 1 十字 / 2 椭圆"},
//     {"name":"ksize","label":"核尺寸","type":"int","default":3,"min":1,"max":51,"description":"结构元边长"},
//     {"name":"iterations","label":"迭代次数","type":"int","default":1,"min":1,"max":20,"description":"重复次数"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.canny","name":"Canny 边缘","group":"2D 图像","category":"处理","description":"Canny 边缘检测，输出二值边缘图"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"threshold1","label":"低阈值","type":"number","default":50,"min":0,"max":1000,"description":"滞后阈值下限"},
//     {"name":"threshold2","label":"高阈值","type":"number","default":150,"min":0,"max":1000,"description":"滞后阈值上限（经验取 2~3 倍 threshold1）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.absdiff","name":"批板差分","group":"2D 图像","category":"处理","description":"基准板与待检板逐像素差的绝对值（尺寸/通道须一致）——再接阈值化节点即缺陷检出链"},
//   "inputs": [
//     {"name":"基准板","type":"image"},
//     {"name":"待检板","type":"image"}
//   ],
//   "params": [],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.connected_components","name":"连通域统计","group":"2D 图像","category":"处理","description":"二值前景连通域计数与定位：输出叠加包围框，结果（数量/面积/位置）并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"connectivity","label":"连通性","type":"int","default":8,"min":4,"max":8,"description":"邻域定义（4 或 8）"},
//     {"name":"min_area","label":"最小面积","type":"number","default":1,"min":0,"max":10000000,"description":"面积过滤下限（像素²，闭区间）"},
//     {"name":"max_area","label":"最大面积(0不限)","type":"number","default":0,"min":0,"max":10000000,"description":"面积过滤上限（≤0 不设上限）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.sobel","name":"Sobel 梯度","group":"2D 图像","category":"处理","description":"Sobel 梯度幅值（|梯度| 灰度图）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"dx","label":"X 阶数","type":"int","default":1,"min":0,"max":2,"description":"X 方向求导阶数（dx+dy ≥ 1）"},
//     {"name":"dy","label":"Y 阶数","type":"int","default":0,"min":0,"max":2,"description":"Y 方向求导阶数（dx+dy ≥ 1）"},
//     {"name":"ksize","label":"核边长","type":"int","default":3,"min":1,"max":7,"description":"核边长（1/3/5/7）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.hsv_threshold","name":"HSV 分割","group":"2D 图像","category":"处理","description":"HSV 色彩阈值分割，输出 0/255 掩膜"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"h_lo","label":"色相下限","type":"number","default":0,"min":0,"max":180,"description":"色调下限 [0,180]（红色跨 0 需两节点取并集）"},
//     {"name":"h_hi","label":"色相上限","type":"number","default":180,"min":0,"max":180,"description":"色调上限 [0,180]"},
//     {"name":"s_lo","label":"饱和下限","type":"number","default":0,"min":0,"max":255,"description":"饱和度下限 [0,255]"},
//     {"name":"s_hi","label":"饱和上限","type":"number","default":255,"min":0,"max":255,"description":"饱和度上限 [0,255]"},
//     {"name":"v_lo","label":"明度下限","type":"number","default":0,"min":0,"max":255,"description":"明度下限 [0,255]"},
//     {"name":"v_hi","label":"明度上限","type":"number","default":255,"min":0,"max":255,"description":"明度上限 [0,255]"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.analyze_contours","name":"轮廓测量","group":"2D 图像","category":"处理","description":"轮廓几何量测量：输出叠加包围框(绿)+最小外接圆(黄)，结果并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"external_only","label":"只取外轮廓","type":"int","default":1,"min":0,"max":1,"description":"1=只取最外层轮廓"},
//     {"name":"min_area","label":"最小面积","type":"number","default":1,"min":0,"max":10000000,"description":"面积过滤（像素²）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.fit_circles","name":"多目标圆拟合","group":"2D 图像","category":"处理","description":"二值前景逐轮廓最小二乘圆拟合（孔位/焊球阵列）：输出叠加拟合圆(绿)，结果(圆心/半径/rms)并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"min_area","label":"最小面积","type":"number","default":10,"min":0,"max":10000000,"description":"面积过滤下限（像素²）"},
//     {"name":"max_area","label":"最大面积(0不限)","type":"number","default":0,"min":0,"max":10000000,"description":"面积过滤上限（≤0 不限）"},
//     {"name":"min_r","label":"最小半径","type":"number","default":0,"min":0,"max":10000,"description":"半径过滤下限（像素）"},
//     {"name":"max_r","label":"最大半径(0不限)","type":"number","default":0,"min":0,"max":10000,"description":"半径过滤上限（≤0 不限）"},
//     {"name":"subpixel","label":"亚像素细化","type":"int","default":1,"min":0,"max":1,"description":"1=灰度梯度亚像素细化（约 0.3 像素精度）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.to_gray","name":"灰度化","group":"2D 图像","category":"处理","description":"BGR(A) → 灰度（输出转 BGRA 显示）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.resize","name":"缩放","group":"2D 图像","category":"处理","description":"缩放到目标尺寸（插值 0 近邻/1 线性/2 立方/3 面积/4 Lanczos4）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"out_width","label":"目标宽","type":"int","default":640,"min":1,"max":10000,"description":"目标宽（像素）"},
//     {"name":"out_height","label":"目标高","type":"int","default":480,"min":1,"max":10000,"description":"目标高（像素）"},
//     {"name":"interpolation","label":"插值(0近邻/1线性/2立方/3面积/4Lanczos)","type":"int","default":1,"min":0,"max":4,"description":"0 近邻 / 1 线性 / 2 立方 / 3 面积 / 4 Lanczos4"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.crop","name":"裁剪","group":"2D 图像","category":"处理","description":"ROI 裁剪（越界自动钳制）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"x","label":"X","type":"int","default":0,"min":0,"max":100000,"description":"左上角 X"},
//     {"name":"y","label":"Y","type":"int","default":0,"min":0,"max":100000,"description":"左上角 Y"},
//     {"name":"width","label":"宽","type":"int","default":100,"min":1,"max":100000,"description":"宽（越界自动钳制到图内）"},
//     {"name":"height","label":"高","type":"int","default":100,"min":1,"max":100000,"description":"高（越界自动钳制到图内）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.rotate","name":"旋转","group":"2D 图像","category":"处理","description":"绕图像中心旋转并缩放（输出同尺寸）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"angle","label":"角度(度)","type":"number","default":0,"min":-360,"max":360,"description":"逆时针角度（度）"},
//     {"name":"scale","label":"缩放","type":"number","default":1,"min":0.01,"max":10,"description":"缩放系数"},
//     {"name":"interpolation","label":"插值","type":"int","default":1,"min":0,"max":4,"description":"0 近邻 / 1 线性 / 2 立方 / 3 面积 / 4 Lanczos4"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.enhance","name":"灰度增强","group":"2D 图像","category":"处理","description":"method 0 直方图均衡 / 1 CLAHE（彩色走 Lab 亮度）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"method","label":"方法","type":"int","default":1,"min":0,"max":1,"description":"0 直方图均衡 / 1 CLAHE（彩色走 Lab 亮度）"},
//     {"name":"clip_limit","label":"对比度上限","type":"number","default":2,"min":0.1,"max":10,"description":"CLAHE 对比度上限"},
//     {"name":"tile_size","label":"分块边长","type":"int","default":8,"min":2,"max":64,"description":"CLAHE 分块边长"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.hough_lines","name":"霍夫直线","group":"2D 图像","category":"处理","description":"概率霍夫直线检测：输入边缘图，输出检出线段标注，端点并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"rho","label":"距离分辨率","type":"number","default":1,"min":0.1,"max":10,"description":"距离分辨率（像素）"},
//     {"name":"theta_deg","label":"角度分辨率(度)","type":"number","default":1,"min":0.1,"max":10,"description":"角度分辨率（度）"},
//     {"name":"threshold","label":"投票阈值","type":"int","default":50,"min":1,"max":10000,"description":"投票阈值"},
//     {"name":"min_line_length","label":"最短线长(0不限)","type":"int","default":20,"min":0,"max":10000,"description":"最短线长（0 不限）"},
//     {"name":"max_line_gap","label":"最大断距(0不限)","type":"int","default":5,"min":0,"max":10000,"description":"线最大断距（0 不限）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.hough_circles","name":"霍夫圆","group":"2D 图像","category":"处理","description":"霍夫圆检测（内部自动 3x3 预平滑）：输出检出圆标注，圆心/半径并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"dp","label":"分辨率倒数","type":"number","default":1,"min":0.1,"max":4,"description":"分辨率倒数"},
//     {"name":"min_dist","label":"圆心最小间距","type":"number","default":10,"min":1,"max":10000,"description":"圆心最小间距（像素）"},
//     {"name":"param1","label":"Canny高阈值","type":"number","default":50,"min":1,"max":1000,"description":"内部 Canny 高阈值"},
//     {"name":"param2","label":"投票阈值","type":"number","default":20,"min":1,"max":1000,"description":"圆心投票阈值（越小检出圆越多）"},
//     {"name":"min_r","label":"最小半径","type":"number","default":5,"min":0,"max":10000,"description":"最小半径（像素）"},
//     {"name":"max_r","label":"最大半径(0不限)","type":"number","default":0,"min":0,"max":10000,"description":"最大半径（≤0 不限）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.match_template_ex","name":"Mark 点对位","group":"2D 图像","category":"处理","description":"多尺度×多角度模板匹配对位：输出中心十字标注，score/中心/尺度/角度并入节点消息"},
//   "inputs": [
//     {"name":"搜索图","type":"image"},
//     {"name":"模板","type":"image"}
//   ],
//   "params": [
//     {"name":"method","label":"匹配方法","type":"int","default":5,"min":0,"max":5,"description":"cv::TemplateMatchModes（默认 5=CCOEFF_NORMED 归一化相关）"},
//     {"name":"scale_lo","label":"尺度下限","type":"number","default":0.8,"min":0.01,"max":10,"description":"尺度搜索下限"},
//     {"name":"scale_hi","label":"尺度上限","type":"number","default":1.2,"min":0.01,"max":10,"description":"尺度搜索上限"},
//     {"name":"scale_step","label":"尺度步进","type":"number","default":0.05,"min":0.001,"max":10,"description":"尺度步距"},
//     {"name":"angle_lo","label":"角度下限(度)","type":"number","default":-15,"min":-360,"max":360,"description":"角度搜索下限（度，逆时针）"},
//     {"name":"angle_hi","label":"角度上限(度)","type":"number","default":15,"min":-360,"max":360,"description":"角度搜索上限（度）"},
//     {"name":"angle_step","label":"角度步进(度)","type":"number","default":1,"min":0.01,"max":90,"description":"角度步距（度）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.find_matches","name":"多目标匹配","group":"2D 图像","category":"处理","description":"多目标模板匹配（NMS 去重）：输出命中外框标注，位置/得分并入节点消息"},
//   "inputs": [
//     {"name":"搜索图","type":"image"},
//     {"name":"模板","type":"image"}
//   ],
//   "params": [
//     {"name":"method","label":"匹配方法","type":"int","default":5,"min":0,"max":5,"description":"cv::TemplateMatchModes（默认 5=归一化相关）"},
//     {"name":"score_threshold","label":"得分阈值","type":"number","default":0.8,"min":0,"max":1,"description":"得分阈值（SQDIFF 系=得分≤阈值命中，其余=≥）"},
//     {"name":"max_matches","label":"最大目标数","type":"int","default":10,"min":1,"max":100,"description":"最多输出数"},
//     {"name":"nms_radius","label":"NMS 半径","type":"int","default":5,"min":0,"max":100,"description":"非极大值抑制半径（像素）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.hsv_threshold_wrap","name":"HSV 分割(跨0)","group":"2D 图像","category":"处理","description":"HSV 阈值分割 wrap 版：h_lo>h_hi 自动环绕区间（红色跨 0）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"h_lo","label":"色相下限","type":"number","default":170,"min":0,"max":180,"description":"色调下限（h_lo > h_hi 自动按环绕区间 [h_lo,180]∪[0,h_hi]，红色跨 0 无需并集）"},
//     {"name":"h_hi","label":"色相上限","type":"number","default":10,"min":0,"max":180,"description":"色调上限"},
//     {"name":"s_lo","label":"饱和下限","type":"number","default":0,"min":0,"max":255,"description":"饱和度下限 [0,255]"},
//     {"name":"s_hi","label":"饱和上限","type":"number","default":255,"min":0,"max":255,"description":"饱和度上限 [0,255]"},
//     {"name":"v_lo","label":"明度下限","type":"number","default":0,"min":0,"max":255,"description":"明度下限 [0,255]"},
//     {"name":"v_hi","label":"明度上限","type":"number","default":255,"min":0,"max":255,"description":"明度上限 [0,255]"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.connected_components_roi","name":"连通域(ROI)","group":"2D 图像","category":"处理","description":"ROI 内连通域统计（结果坐标为全图系）：输出绿框标注，结果并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"roi_x","label":"ROI X","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 X"},
//     {"name":"roi_y","label":"ROI Y","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 Y"},
//     {"name":"roi_w","label":"ROI 宽(0=全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 宽（≤0 = 全图）"},
//     {"name":"roi_h","label":"ROI 高(0=全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 高（≤0 = 全图）"},
//     {"name":"connectivity","label":"连通性","type":"int","default":8,"min":4,"max":8,"description":"邻域定义（4 或 8）"},
//     {"name":"min_area","label":"最小面积","type":"number","default":1,"min":0,"max":10000000,"description":"面积过滤下限（像素²）"},
//     {"name":"max_area","label":"最大面积(0不限)","type":"number","default":0,"min":0,"max":10000000,"description":"面积过滤上限（≤0 不设上限）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.analyze_contours_roi","name":"轮廓测量(ROI)","group":"2D 图像","category":"处理","description":"ROI 内轮廓测量（结果坐标为全图系）：输出绿框+黄圆标注，结果并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"roi_x","label":"ROI X","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 X"},
//     {"name":"roi_y","label":"ROI Y","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 Y"},
//     {"name":"roi_w","label":"ROI 宽(0=全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 宽（≤0 = 全图）"},
//     {"name":"roi_h","label":"ROI 高(0=全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 高（≤0 = 全图）"},
//     {"name":"external_only","label":"只取外轮廓","type":"int","default":1,"min":0,"max":1,"description":"1=只取最外层轮廓"},
//     {"name":"min_area","label":"最小面积","type":"number","default":1,"min":0,"max":10000000,"description":"面积过滤（像素²）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.match_template_masked","name":"掩膜匹配","group":"2D 图像","category":"处理","description":"带掩膜模板匹配（遮挡/干扰背景鲁棒；method 仅 0/3）：输出命中外框，得分并入节点消息"},
//   "inputs": [
//     {"name":"搜索图","type":"image"},
//     {"name":"模板","type":"image"},
//     {"name":"掩膜","type":"image"}
//   ],
//   "params": [
//     {"name":"method","label":"匹配方法(仅0/3)","type":"int","default":3,"min":0,"max":5,"description":"匹配方法（仅 0/3 支持，默认 3=CCORR_NORMED）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.find_matches_ex","name":"Mark 阵列全检","group":"2D 图像","category":"处理","description":"多目标×旋转×尺度组合模板匹配（Mark 阵列全检，全局 NMS）：输出中心十字标注，位置/尺度/角度/得分并入节点消息"},
//   "inputs": [
//     {"name":"搜索图","type":"image"},
//     {"name":"模板","type":"image"}
//   ],
//   "params": [
//     {"name":"method","label":"匹配方法","type":"int","default":5,"min":0,"max":5,"description":"cv::TemplateMatchModes（默认 5=归一化相关）"},
//     {"name":"score_threshold","label":"得分阈值","type":"number","default":0.6,"min":0,"max":1,"description":"得分阈值（SQDIFF 系=得分≤阈值命中，其余=≥）"},
//     {"name":"max_matches","label":"最大目标数","type":"int","default":50,"min":1,"max":200,"description":"最多输出数"},
//     {"name":"nms_radius","label":"NMS 半径","type":"int","default":10,"min":0,"max":200,"description":"全局 NMS 半径（模板中心距，像素）"},
//     {"name":"scale_lo","label":"尺度下限","type":"number","default":0.9,"min":0.01,"max":10,"description":"尺度搜索下限"},
//     {"name":"scale_hi","label":"尺度上限","type":"number","default":1.1,"min":0.01,"max":10,"description":"尺度搜索上限"},
//     {"name":"scale_step","label":"尺度步进","type":"number","default":0.05,"min":0.001,"max":10,"description":"尺度步距"},
//     {"name":"angle_lo","label":"角度下限(度)","type":"number","default":-5,"min":-360,"max":360,"description":"角度搜索下限（度）"},
//     {"name":"angle_hi","label":"角度上限(度)","type":"number","default":5,"min":-360,"max":360,"description":"角度搜索上限（度）"},
//     {"name":"angle_step","label":"角度步进(度)","type":"number","default":1,"min":0.01,"max":90,"description":"角度步距（度）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.fit_ellipses","name":"多目标椭圆拟合","group":"2D 图像","category":"处理","description":"二值前景逐轮廓椭圆拟合（椭圆焊盘/异形孔）：输出椭圆标注，中心/轴长/角度并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"min_area","label":"最小面积","type":"number","default":10,"min":0,"max":10000000,"description":"面积过滤下限（像素²）"},
//     {"name":"max_area","label":"最大面积(0不限)","type":"number","default":0,"min":0,"max":10000000,"description":"面积过滤上限（≤0 不限）"},
//     {"name":"min_axis","label":"最小全轴长","type":"number","default":0,"min":0,"max":10000,"description":"全轴长过滤下限（像素）"},
//     {"name":"max_axis","label":"最大全轴长(0不限)","type":"number","default":0,"min":0,"max":10000,"description":"全轴长过滤上限（≤0 不限）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.match_template_subpix","name":"亚像素对位","group":"2D 图像","category":"处理","description":"模板匹配 + 三分点抛物线亚像素细化（±0.5px）：输出中心十字标注，score/位置并入节点消息"},
//   "inputs": [
//     {"name":"搜索图","type":"image"},
//     {"name":"模板","type":"image"}
//   ],
//   "params": [
//     {"name":"method","label":"匹配方法","type":"int","default":5,"min":0,"max":5,"description":"cv::TemplateMatchModes（默认 5=归一化相关）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.region_stats","name":"区域灰度统计","group":"2D 图像","category":"处理","description":"ROI 内灰度均值/标准差/最值统计（批次一致性监控）：结果并入节点消息，无图像输出"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"roi_x","label":"ROI X(0=全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 X"},
//     {"name":"roi_y","label":"ROI Y","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 Y"},
//     {"name":"roi_w","label":"ROI 宽(0=全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 宽（≤0 = 全图）"},
//     {"name":"roi_h","label":"ROI 高","type":"int","default":0,"min":0,"max":100000,"description":"ROI 高（≤0 = 全图）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
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


// ---------------- 算子入口：Harris 角点检测（node.json: alg.image2d.harris_corners） ----------------
// 参数（按声明顺序）：
//   block_size   int     默认 5  [3,51]    邻域窗口（奇数自动补）
//   ksize        int     默认 3  [3,7]     Sobel 核
//   k            number  默认 0.04 [0.01,0.1] Harris 常数
//   thresh_ratio number  默认 0.05 [0,1]    相对最大响应的阈值比例
//   subpix       int     默认 1  [0,1]     亚像素细化
// 输出：输入图克隆叠加角点绿色十字标注。
// 消息通道："count=N; [i](x,y); ..."（前若干个，900 字节截断）。
// 错误码：1 输入未连线；2 检测失败；3 参数非法。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.harris_corners","name":"Harris 角点","group":"2D 图像","category":"处理","description":"Harris 角点检测（Mark 点自动定位）：输出角点十字标注，坐标并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"block_size","label":"邻域窗口","type":"int","default":5,"min":3,"max":51,"description":"角点检测邻域窗口（奇数自动补）"},
//     {"name":"ksize","label":"Sobel核","type":"int","default":3,"min":3,"max":7,"description":"Sobel 梯度核边长（3~7）"},
//     {"name":"k","label":"Harris常数","type":"number","default":0.04,"min":0.01,"max":0.1,"description":"Harris 响应常数（经验 0.04~0.06）"},
//     {"name":"thresh_ratio","label":"响应阈值比例","type":"number","default":0.05,"min":0,"max":1,"description":"响应阈值比例（相对最大响应，越小角点越多）"},
//     {"name":"subpix","label":"亚像素细化","type":"int","default":1,"min":0,"max":1,"description":"1=角点亚像素细化"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiHarrisCorners(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int block_size = 5, ksize = 3, subpix = 1;
    double k = 0.04, thresh_ratio = 0.05;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) block_size = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) ksize = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) k = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) thresh_ratio = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) subpix = p->i;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Point2f* corners = NULL;
    int count = 0;
    const int rc = Image2DAlg_HarrisCorners(&input, block_size, ksize, k, thresh_ratio,
                                            subpix, &corners, &count);
    if (rc == -3) { out->error = "角点检测失败（窗口/核参数非法）"; return 3; }
    if (rc != 0) { out->error = "角点检测失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i) {
        const cv::Point c((int)std::lround(corners[i].x), (int)std::lround(corners[i].y));
        cv::line(drawn, c + cv::Point(-4, 0), c + cv::Point(4, 0), cv::Scalar(0, 255, 0, 255), 1);
        cv::line(drawn, c + cv::Point(0, -4), c + cv::Point(0, 4), cv::Scalar(0, 255, 0, 255), 1);
    }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 48; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d](%.1f,%.1f)", i, corners[i].x, corners[i].y);
    Image2DAlg_FreePoint2f(corners);

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

// ---------------- 算子入口：ORB 特征对位（node.json: alg.image2d.orb_align） ----------------
// 输入 2 张图像：images[0]=基准图（in1），images[1]=待对位图（in2）。
// 参数（按声明顺序）：
//   n_features  int     默认 500 [50,2000]  ORB 特征点上限
//   ratio       number  默认 0.7 [0.1,0.9]  Lowe 比率测试系数
// 输出：待对位图克隆叠加匹配特征点十字标注（绿色）。
// 消息通道："inliers=N matches=M m=[..6 值..]"（仿射 in1→in2，RANSAC）。
// 错误码：1 输入不足；2 特征/匹配不足（-2，平坦图属正常）；3 参数非法。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.orb_align","name":"ORB 特征对位","group":"2D 图像","category":"处理","description":"ORB 特征匹配 + RANSAC 仿射对位（大形变/旋转/亮度鲁棒）：输出匹配点十字标注，仿射矩阵/内点数并入节点消息"},
//   "inputs": [
//     {"name":"基准图","type":"image"},
//     {"name":"待对位图","type":"image"}
//   ],
//   "params": [
//     {"name":"n_features","label":"特征点上限","type":"int","default":500,"min":50,"max":2000,"description":"ORB 特征点数量上限（越大匹配越稳、越慢）"},
//     {"name":"ratio","label":"Lowe比率","type":"number","default":0.7,"min":0.1,"max":0.9,"description":"Lowe 比率测试阈值（越大匹配越多、误配也随之增多）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiORBAlign(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（基准图/待对位图）";
        return 1;
    }
    int n_features = 500;
    double ratio = 0.7;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) n_features = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) ratio = p->d;

    Image2DAlg_Image i1 = CoreInputFrom(in->images[0]);
    Image2DAlg_Image i2 = CoreInputFrom(in->images[1]);
    double m[6] = { 0 };
    Image2DAlg_OrbMatch* matches = NULL;
    int count = 0, inliers = 0;
    const int rc = Image2DAlg_ORBAlign(&i1, &i2, n_features, ratio, m,
                                       &matches, &count, &inliers);
    if (rc == -2) { out->error = "特征/匹配不足（平坦图或形变过大）"; return 2; }
    if (rc == -3) { out->error = "对位失败（n_features/ratio 参数非法）"; return 3; }
    if (rc != 0) { out->error = "对位失败"; return 2; }

    cv::Mat vis(in->images[1].height, in->images[1].width, CV_8UC4,
                (void*)in->images[1].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i) {
        const cv::Point c((int)std::lround(matches[i].x2), (int)std::lround(matches[i].y2));
        cv::line(drawn, c + cv::Point(-4, 0), c + cv::Point(4, 0), cv::Scalar(0, 255, 0, 255), 1);
        cv::line(drawn, c + cv::Point(0, -4), c + cv::Point(0, 4), cv::Scalar(0, 255, 0, 255), 1);
    }

    static char msg[256];
    snprintf(msg, sizeof(msg),
             "inliers=%d matches=%d m=[%.3f %.3f %.3f; %.3f %.3f %.3f]",
             inliers, count, m[0], m[1], m[2], m[3], m[4], m[5]);
    Image2DAlg_FreeOrbMatches(matches);

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


// ---------------- 算子入口：多目标直线拟合（node.json: alg.image2d.fit_lines） ----------------
// 输入：二值语义图（非零为前景；彩色先转灰度）。
// 参数（按声明顺序）：min_area/max_area（面积过滤，<=0 不限）、min_length（方向投影跨度过滤，<=0 不限）。
// 输出：输入图克隆叠加拟合线段（绿色，中心 ±span/2）。
// 消息通道："count=N; [i]c=(x0,y0) dir=(vx,vy) rms=R; ..."（900 字节截断）。
// 错误码：1 输入未连线；2 拟合失败。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.fit_lines","name":"多目标直线拟合","group":"2D 图像","category":"处理","description":"二值前景逐轮廓直线拟合（板边/边缘线）：输出拟合线标注，方向/RMS 并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"min_area","label":"最小面积","type":"number","default":10,"min":0,"max":10000000,"description":"面积过滤下限（像素²）"},
//     {"name":"max_area","label":"最大面积(0不限)","type":"number","default":0,"min":0,"max":10000000,"description":"面积过滤上限（≤0 不限）"},
//     {"name":"min_length","label":"最小跨度(0不限)","type":"number","default":0,"min":0,"max":10000,"description":"轮廓跨度过滤下限（像素；≤0 不限）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiFitLines(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double min_area = 10.0, max_area = 0.0, min_length = 0.0;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) min_area = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) max_area = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) min_length = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_LineFit* lines = NULL;
    int count = 0;
    const int rc = Image2DAlg_FitLinesFromBinary(&input, (int)min_area, (int)max_area,
                                                 min_length, &lines, &count);
    if (rc != 0) { out->error = "直线拟合失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i) {
        // 沿方向画 ±span/2 的线段（span 用包围盒对角线近似上限）
        const double half = 0.5 * std::sqrt((double)in->images[0].width * in->images[0].width +
                                            (double)in->images[0].height * in->images[0].height);
        const cv::Point2f a((float)(lines[i].x0 - lines[i].vx * half),
                            (float)(lines[i].y0 - lines[i].vy * half));
        const cv::Point2f b((float)(lines[i].x0 + lines[i].vx * half),
                            (float)(lines[i].y0 + lines[i].vy * half));
        cv::line(drawn, a, b, cv::Scalar(0, 255, 0, 255), 1, cv::LINE_AA);
    }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 96; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]c=(%.1f,%.1f) dir=(%.3f,%.3f) rms=%.3f",
                        i, lines[i].x0, lines[i].y0, lines[i].vx, lines[i].vy, lines[i].rms);
    Image2DAlg_FreeLineFits(lines);

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

// ---------------- 算子入口：多目标旋转矩形（node.json: alg.image2d.fit_rects） ----------------
// 输入：二值语义图（非零为前景；彩色先转灰度）。
// 参数（按声明顺序）：min_area/max_area（面积过滤，<=0 不限）。
// 输出：输入图克隆叠加旋转矩形（绿色 4 边框）。
// 消息通道："count=N; [i]c=(cx,cy) size=(W,H) ang=A; ..."（900 字节截断）。
// 错误码：1 输入未连线；2 拟合失败。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.fit_rects","name":"多目标旋转矩形","group":"2D 图像","category":"处理","description":"二值前景逐轮廓旋转矩形拟合（异形件/料口）：输出矩形标注，中心/尺寸/角度并入节点消息"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"min_area","label":"最小面积","type":"number","default":10,"min":0,"max":10000000,"description":"面积过滤下限（像素²）"},
//     {"name":"max_area","label":"最大面积(0不限)","type":"number","default":0,"min":0,"max":10000000,"description":"面积过滤上限（≤0 不限）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiFitRects(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double min_area = 10.0, max_area = 0.0;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) min_area = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) max_area = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_RectFit* rects = NULL;
    int count = 0;
    const int rc = Image2DAlg_FitRectsFromBinary(&input, (int)min_area, (int)max_area,
                                                 &rects, &count);
    if (rc != 0) { out->error = "旋转矩形拟合失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    cv::Point2f pts4[4];
    for (int i = 0; i < count; ++i) {
        const cv::RotatedRect r(cv::Point2f((float)rects[i].cx, (float)rects[i].cy),
                                cv::Size2f((float)rects[i].width, (float)rects[i].height),
                                (float)rects[i].angle);
        r.points(pts4);
        for (int k = 0; k < 4; ++k)
            cv::line(drawn, pts4[k], pts4[(k + 1) % 4], cv::Scalar(0, 255, 0, 255), 1, cv::LINE_AA);
    }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 96; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]c=(%.1f,%.1f) size=(%.1f,%.1f) ang=%.1f",
                        i, rects[i].cx, rects[i].cy, rects[i].width, rects[i].height, rects[i].angle);
    Image2DAlg_FreeRectFits(rects);

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

// ---------------- 算子入口：轮廓点云输出（node.json: alg.image2d.contour_cloud） ----------------
// 输入：二值语义图（非零为前景；彩色先转灰度）。
// 参数（按声明顺序）：external_only（int，默认 1）、min_area（number，默认 1）。
// 输出：z=0 点云（AlgoCCloudOut，xyz=轮廓点坐标，rgb=NULL），供宿主 3D 工作台查看轮廓。
// 消息通道："count=N(轮廓) points=M(点)"。
// 错误码：1 输入未连线；2 导出失败。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.contour_cloud","name":"轮廓点云","group":"2D 图像","category":"处理","description":"轮廓原始点集导出为 z=0 点云（宿主 3D 工作台查看轮廓）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"external_only","label":"只取外轮廓","type":"int","default":1,"min":0,"max":1,"description":"1=只取最外层轮廓"},
//     {"name":"min_area","label":"最小面积","type":"number","default":1,"min":0,"max":10000000,"description":"面积过滤（像素²）"}
//   ],
//   "outputs": [
//     {"name":"点云","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiContourCloud(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int external_only = 1;
    double min_area = 1.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) external_only = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) min_area = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    float* xy = NULL;
    int points = 0, *offsets = NULL, contours = 0;
    const int rc = Image2DAlg_ContourPoints(&input, external_only, (int)min_area,
                                            &xy, &points, &offsets, &contours);
    if (rc != 0) { out->error = "轮廓点导出失败"; return 2; }

    // 扁平 xy → SoA 点云（z=0，rgb=NULL 由宿主按需着色）
    const int np = points;   // out_points = 点个数（非 float 数），修复砍半 bug
    float* xyz = (float*)malloc(sizeof(float) * np * 3);
    if (!xyz) {
        Image2DAlg_FreeContourPoints(xy, offsets);
        out->error = "内存不足";
        return 2;
    }
    for (int i = 0; i < np; ++i) {
        xyz[i * 3] = xy[i * 2];
        xyz[i * 3 + 1] = xy[i * 2 + 1];
        xyz[i * 3 + 2] = 0.0f;
    }
    Image2DAlg_FreeContourPoints(xy, offsets);

    out->clouds = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!out->clouds) { free(xyz); out->error = "内存不足"; return 2; }
    out->clouds[0].count = np;
    out->clouds[0].xyz = xyz;
    out->clouds[0].rgb = NULL;
    out->cloudCount = 1;

    static char msg[128];
    snprintf(msg, sizeof(msg), "contours=%d points=%d", contours, np);
    out->error = msg;
    return 0;
}

// ---------------- 点集输入/输出辅助（下方点集类节点共用） ----------------

// 输入点云 xyz（交错）-> 扁平 xy [x0,y0,x1,y1,...]（2D 点集语义，z 分量忽略）。
// malloc，配 free；失败返回 NULL。
static float* FlattenCloudXy(const AlgoCCloud& c) {
    float* xy = (float*)malloc(sizeof(float) * (size_t)c.count * 2);
    if (!xy) return nullptr;
    for (int i = 0; i < c.count; ++i) {
        xy[i * 2]     = c.xyz[i * 3];
        xy[i * 2 + 1] = c.xyz[i * 3 + 1];
    }
    return xy;
}

// 扁平 xy -> z=0 输出点云（AlgoCCloudOut，xyz=交错坐标，rgb=NULL）。
// malloc，配 algo_free；失败返回 NULL。
static AlgoCCloudOut* AllocCloudOutZ0(const float* xy, int count) {
    float* xyz = (float*)malloc(sizeof(float) * (size_t)count * 3);
    if (!xyz) return nullptr;
    for (int i = 0; i < count; ++i) {
        xyz[i * 3]     = xy[i * 2];
        xyz[i * 3 + 1] = xy[i * 2 + 1];
        xyz[i * 3 + 2] = 0.0f;
    }
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(xyz); return nullptr; }
    arr->count = count;
    arr->xyz = xyz;
    arr->rgb = NULL;
    return arr;
}

// ---------------- 算子入口：Shi-Tomasi 角点检测（node.json: alg.image2d.good_features） ----------------
// 参数（按声明顺序）：
//   max_corners    int     默认 100  [1,10000]  最大角点数（按质量降序保留）
//   quality_level  number  默认 0.01 (0,1]     质量系数（相对最强角点响应的比例）
//   min_distance   number  默认 10   [1,10000] 角点最小间距（像素，内置 NMS 去重）
//   subpix         int     默认 1    [0,1]     亚像素细化
// 输出：输入图克隆叠加角点绿色十字标注（同 Harris 画法）。
// 消息通道："count=N; [i](x,y); ..."（900 字节截断；无检出 count=0 属正常）。
// 错误码：1 输入未连线；2 检测失败/内存不足；3 参数非法。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.good_features","name":"Shi-Tomasi 角点","group":"2D 图像","category":"处理","description":"Shi-Tomasi（goodFeaturesToTrack）角点检测：输出角点十字标注，坐标并入节点消息（与 Harris 角点同构，作为 corner_sub_pix 的初始点）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"max_corners","label":"最大角点数","type":"int","default":100,"min":1,"max":10000,"description":"最多输出的角点数（按质量降序保留）"},
//     {"name":"quality_level","label":"质量系数","type":"number","default":0.01,"min":0.001,"max":1,"description":"相对最强角点响应的比例（典型 0.01~0.1，越小角点越多）"},
//     {"name":"min_distance","label":"最小间距(像素)","type":"number","default":10,"min":1,"max":10000,"description":"角点最小间距（像素，内置 NMS 去重）"},
//     {"name":"subpix","label":"亚像素细化","type":"int","default":1,"min":0,"max":1,"description":"1=角点亚像素细化"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiGoodFeatures(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int max_corners = 100, subpix = 1;
    double quality_level = 0.01, min_distance = 10.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) max_corners = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) quality_level = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) min_distance = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) subpix = p->i;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Point2f* corners = NULL;
    int count = 0;
    const int rc = Image2DAlg_GoodFeatures(&input, max_corners, quality_level, min_distance,
                                           subpix, &corners, &count);
    if (rc == -3) { out->error = "角点检测失败（max_corners>=1、quality_level∈(0,1]、min_distance>=1）"; return 3; }
    if (rc != 0) { out->error = "角点检测失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i) {
        const cv::Point c((int)std::lround(corners[i].x), (int)std::lround(corners[i].y));
        cv::line(drawn, c + cv::Point(-4, 0), c + cv::Point(4, 0), cv::Scalar(0, 255, 0, 255), 1);
        cv::line(drawn, c + cv::Point(0, -4), c + cv::Point(0, 4), cv::Scalar(0, 255, 0, 255), 1);
    }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 48; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d](%.1f,%.1f)", i, corners[i].x, corners[i].y);
    Image2DAlg_FreePoint2f(corners);

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

// ---------------- 算子入口：角点亚像素细化（node.json: alg.image2d.corner_sub_pix） ----------------
// 输入：images[0]=图像，clouds[0]=初始角点（如 good_features/harris_corners 消息坐标
//       或 contour_cloud 轮廓点云；点集 xy 取自 xyz，z 分量忽略；count >= 1）。
// 参数（按声明顺序）：
//   win_size  int  默认 11  [3,51]  亚像素搜索窗口边长（典型 5~11）
// 输出：标注图（输入克隆叠加细化后角点绿色十字）+ 细化点集点云（z=0，供下游拟合/逼近）。
// 消息通道："count=N; [i](x,y); ..."（细化后坐标，900 字节截断；细化失败位置保持原值）。
// 错误码：1 输入未连线；2 细化失败/内存不足；3 参数非法。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.corner_sub_pix","name":"角点亚像素细化","group":"2D 图像","category":"处理","description":"对初始角点做梯度法亚像素细化（约 ±0.5px）：输出细化点集点云(z=0)与十字标注图，坐标并入节点消息；初始点可来自 good_features/harris_corners 消息坐标或 contour_cloud 轮廓点云"},
//   "inputs": [
//     {"name":"图像","type":"image"},
//     {"name":"初始点集","type":"cloud"}
//   ],
//   "params": [
//     {"name":"win_size","label":"搜索窗口","type":"int","default":11,"min":3,"max":51,"description":"亚像素搜索窗口边长（典型 5~11）"}
//   ],
//   "outputs": [
//     {"name":"标注图","type":"image"},
//     {"name":"细化点集","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiCornerSubPix(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线（图像）"; return 1; }
    if (in->cloudCount < 1 || !in->clouds || !in->clouds[0].xyz) {
        out->error = "需要点集输入（初始角点）"; return 1;
    }
    int win_size = 11;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) win_size = p->i;

    const AlgoCCloud* src = &in->clouds[0];
    if (src->count < 1) { out->error = "初始角点为空"; return 2; }

    Image2DAlg_Point2f* pts =
        (Image2DAlg_Point2f*)malloc(sizeof(Image2DAlg_Point2f) * (size_t)src->count);
    if (!pts) { out->error = "内存不足"; return 2; }
    for (int i = 0; i < src->count; ++i) {
        pts[i].x = src->xyz[i * 3];
        pts[i].y = src->xyz[i * 3 + 1];
    }

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Point2f* refined = NULL;
    const int count = src->count;
    const int rc = Image2DAlg_CornerSubPix(&input, pts, count, win_size, &refined);
    free(pts);
    if (rc == -3) { out->error = "亚像素细化失败（点数 >= 1，win_size >= 3）"; return 3; }
    if (rc != 0) { out->error = "亚像素细化失败"; return 2; }

    // 细化点集 -> z=0 点云输出（先组点云，再画标注图/组消息并释放 refined，最后出图像）
    float* xy = (float*)malloc(sizeof(float) * (size_t)count * 2);
    if (!xy) { Image2DAlg_FreePoint2f(refined); out->error = "内存不足"; return 2; }
    for (int i = 0; i < count; ++i) {
        xy[i * 2]     = refined[i].x;
        xy[i * 2 + 1] = refined[i].y;
    }
    out->clouds = AllocCloudOutZ0(xy, count);
    free(xy);
    if (!out->clouds) { Image2DAlg_FreePoint2f(refined); out->error = "内存不足"; return 2; }
    out->cloudCount = 1;

    // 标注图：输入克隆叠加细化后角点绿色十字（同 Harris 画法），并在释放前组消息
    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i) {
        const cv::Point c((int)std::lround(refined[i].x), (int)std::lround(refined[i].y));
        cv::line(drawn, c + cv::Point(-4, 0), c + cv::Point(4, 0), cv::Scalar(0, 255, 0, 255), 1);
        cv::line(drawn, c + cv::Point(0, -4), c + cv::Point(0, 4), cv::Scalar(0, 255, 0, 255), 1);
    }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 48; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d](%.2f,%.2f)", i, refined[i].x, refined[i].y);
    Image2DAlg_FreePoint2f(refined);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) {
        free(out->clouds[0].xyz); free(out->clouds);
        out->clouds = NULL; out->cloudCount = 0;
        out->error = "内存不足"; return 2;
    }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) {
        free(px);
        free(out->clouds[0].xyz); free(out->clouds);
        out->clouds = NULL; out->cloudCount = 0;
        out->error = "内存不足"; return 2;
    }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：轮廓顶点逼近（node.json: alg.image2d.approx_poly_dp） ----------------
// 输入：clouds[0]=轮廓点集（xy 取自 xyz，z 分量忽略；count >= 3）。
// 参数（按声明顺序）：
//   epsilon  number  默认 3  [0,10000]  Douglas-Peucker 容差（像素；0 = 仅去除共线冗余）
//   closed   int     默认 1  [0,1]      闭合轮廓（findContours 输出）
// 输出：逼近后多边形顶点点云（z=0，供形状判定/下游拟合节点）。
// 消息通道："count=N; [i](x,y); ..."（顶点坐标，900 字节截断）。
// 错误码：1 输入未连线；2 点集不足/内存不足；3 参数非法（epsilon<0 或逼近过度退化）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.approx_poly_dp","name":"多边形顶点逼近","group":"2D 图像","category":"处理","description":"Douglas-Peucker 轮廓顶点逼近（epsilon 像素容差）：输出多边形顶点点集(z=0)，形状判定前置（如矩形轮廓→4 角点）；输入点集可来自 contour_cloud 等节点"},
//   "inputs": [
//     {"name":"轮廓点集","type":"cloud"}
//   ],
//   "params": [
//     {"name":"epsilon","label":"逼近容差(像素)","type":"number","default":3,"min":0,"max":10000,"description":"Douglas-Peucker 容差（0=仅去除共线冗余；过大会退化到 <3 顶点）"},
//     {"name":"closed","label":"闭合轮廓","type":"int","default":1,"min":0,"max":1,"description":"1=闭合轮廓（findContours 输出）"}
//   ],
//   "outputs": [
//     {"name":"多边形点集","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiApproxPolyDP(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds || !in->clouds[0].xyz) {
        out->error = "输入未连线（轮廓点集）"; return 1;
    }
    double epsilon = 3.0;
    int closed = 1;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) epsilon = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) closed = p->i;

    const AlgoCCloud* src = &in->clouds[0];
    if (src->count < 3) { out->error = "轮廓点集至少需要 3 个点"; return 2; }

    float* xy = FlattenCloudXy(*src);
    if (!xy) { out->error = "内存不足"; return 2; }
    float* out_xy = NULL;
    int out_count = 0;
    const int rc = Image2DAlg_ApproxPolyDP(xy, src->count, epsilon, closed, &out_xy, &out_count);
    free(xy);
    if (rc == -3) { out->error = "顶点逼近失败（点数 >= 3、epsilon >= 0；epsilon 过大退化 <3 顶点）"; return 3; }
    if (rc != 0) { out->error = "顶点逼近失败"; return 2; }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", out_count);
    for (int i = 0; i < out_count && pos < (int)sizeof(msg) - 48; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d](%.1f,%.1f)", i, out_xy[i * 2], out_xy[i * 2 + 1]);

    out->clouds = AllocCloudOutZ0(out_xy, out_count);   // 顶点 -> z=0 点云
    Image2DAlg_FreeContourPoints(out_xy, NULL);         // 该缓冲登记用 FreeContourPoints 释放
    if (!out->clouds) { out->error = "内存不足"; return 2; }
    out->cloudCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：形状相似度（node.json: alg.image2d.match_shapes） ----------------
// 输入 2 个点集：clouds[0]=轮廓 A，clouds[1]=轮廓 B（各 >= 3 点；xy 取自 xyz，z 分量忽略）。
// 参数（按声明顺序）：
//   method  int  默认 1  [1,3]  1/2/3 = cv::CONTOURS_MATCH_I1~I3（常用 1）
// 输出：无端口（outputs 为空）；Hu 矩距离并入消息通道（region_stats 先例）：
//   "score=%.6f method=%d"（越小越相似，0 = 完全一致）。
// 错误码：1 输入不足（需 2 个点集）；2 点集不足；3 参数非法（method 越界）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.match_shapes","name":"形状相似度","group":"2D 图像","category":"处理","description":"Hu 矩形状匹配：两组轮廓点集的相似度（Hu 距离越小越相似，0=完全一致），结果并入节点消息，无端口输出"},
//   "inputs": [
//     {"name":"轮廓 A","type":"cloud"},
//     {"name":"轮廓 B","type":"cloud"}
//   ],
//   "params": [
//     {"name":"method","label":"匹配方法","type":"int","default":1,"min":1,"max":3,"description":"1/2/3 = cv::CONTOURS_MATCH_I1~I3（常用 1）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiMatchShapes(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 2 || !in->clouds || !in->clouds[1].xyz) {
        out->error = "需要 2 个点集输入（轮廓 A/轮廓 B）"; return 1;
    }
    int method = 1;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;

    const AlgoCCloud* a = &in->clouds[0];
    const AlgoCCloud* b = &in->clouds[1];
    if (a->count < 3 || b->count < 3) { out->error = "两组点集各至少需要 3 个点"; return 2; }

    float* xy1 = FlattenCloudXy(*a);
    float* xy2 = xy1 ? FlattenCloudXy(*b) : NULL;
    if (!xy2) { free(xy1); out->error = "内存不足"; return 2; }
    double score = 0.0;
    const int rc = Image2DAlg_MatchShapes(xy1, a->count, xy2, b->count, method, &score);
    free(xy1);
    free(xy2);
    if (rc == -3) { out->error = "形状匹配失败（各点集 >= 3 点，method 1~3）"; return 3; }
    if (rc != 0) { out->error = "形状匹配失败"; return 2; }

    static char msg[128];
    snprintf(msg, sizeof(msg), "score=%.6f method=%d", score, method);
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：单目标圆拟合（node.json: alg.image2d.fit_circle） ----------------
// 输入：clouds[0]=点集（xy 取自 xyz，z 分量忽略；count >= 3）。参数：无。
// 输出：无端口（outputs 为空）；结果并入消息通道，形态与 fit_circles 一致：
//   "count=1; [0]c=(cx,cy) r=R rms=RMS"
//   （Kåsa 代数法 + SVD；共线/重合等退化输入报错，结合 rms 判断拟合质量）。
// 错误码：1 输入未连线；2 点集不足/内存不足；3 拟合失败（点数不足/退化）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.fit_circle","name":"单目标圆拟合","group":"2D 图像","category":"处理","description":"点集最小二乘圆拟合（Kåsa 代数法 + SVD）：圆心/半径/残差 rms 并入节点消息，无端口输出；输入点集可来自 contour_cloud / corner_sub_pix 等节点"},
//   "inputs": [
//     {"name":"点集","type":"cloud"}
//   ],
//   "params": [],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiFitCircle(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds || !in->clouds[0].xyz) {
        out->error = "输入未连线（点集）"; return 1;
    }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count < 3) { out->error = "圆拟合至少需要 3 个点"; return 2; }

    float* xy = FlattenCloudXy(*src);
    if (!xy) { out->error = "内存不足"; return 2; }
    Image2DAlg_CircleFit st = {};
    const int rc = Image2DAlg_FitCircle(xy, src->count, &st);
    free(xy);
    if (rc == -3) { out->error = "圆拟合失败（点数不足或共线/重合退化）"; return 3; }
    if (rc != 0) { out->error = "圆拟合失败"; return 2; }

    static char msg[160];
    snprintf(msg, sizeof(msg), "count=1; [0]c=(%.2f,%.2f) r=%.2f rms=%.3f",
             st.cx, st.cy, st.r, st.rms);
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：单目标直线拟合（node.json: alg.image2d.fit_line） ----------------
// 输入：clouds[0]=点集（xy 取自 xyz，z 分量忽略；count >= 2）。参数：无。
// 输出：无端口（outputs 为空）；结果并入消息通道，形态与 fit_lines 一致：
//   "count=1; [0]c=(x0,y0) dir=(vx,vy) rms=R"（DIST_L2 最小二乘）。
// 错误码：1 输入未连线；2 点集不足/内存不足；3 拟合失败（点数不足）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.fit_line","name":"单目标直线拟合","group":"2D 图像","category":"处理","description":"点集 DIST_L2 最小二乘直线拟合：线上点/方向向量/残差 rms 并入节点消息，无端口输出；输入点集可来自 contour_cloud 等节点"},
//   "inputs": [
//     {"name":"点集","type":"cloud"}
//   ],
//   "params": [],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiFitLine(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds || !in->clouds[0].xyz) {
        out->error = "输入未连线（点集）"; return 1;
    }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count < 2) { out->error = "直线拟合至少需要 2 个点"; return 2; }

    float* xy = FlattenCloudXy(*src);
    if (!xy) { out->error = "内存不足"; return 2; }
    Image2DAlg_LineFit st = {};
    const int rc = Image2DAlg_FitLine(xy, src->count, &st);
    free(xy);
    if (rc == -3) { out->error = "直线拟合失败（点数不足）"; return 3; }
    if (rc != 0) { out->error = "直线拟合失败"; return 2; }

    static char msg[160];
    snprintf(msg, sizeof(msg), "count=1; [0]c=(%.1f,%.1f) dir=(%.3f,%.3f) rms=%.3f",
             st.x0, st.y0, st.vx, st.vy, st.rms);
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：FAST 角点检测（node.json: alg.image2d.fast_features） ----------------
// 参数（按声明顺序）：
//   threshold  int  默认 20  [1,255]  圆周连续像素比较的灰度差阈值（典型 10~40）
//   nonmax     int  默认 1   [0,1]    3x3 非极大值抑制开关（1 = 开启去重）
// 输出：标注图（输入克隆叠加角点绿色十字，同 Harris 画法）+ 点集点云（z=0，
//       供下游 corner_sub_pix/拟合类节点取用，corner_sub_pix 双输出同款）。
// 消息通道："count=N; [i](x,y); ..."（900 字节截断；无检出 count=0 属正常）。
// 错误码：1 输入未连线；2 检测失败/内存不足；3 参数非法（threshold <= 0）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.fast_features","name":"FAST 角点","group":"2D 图像","category":"处理","description":"FAST 角点检测（比 Harris 快一个数量级，实时/视频前处理）：输出角点十字标注图与点集点云(z=0)，坐标并入节点消息；无检出(平坦图)属正常"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"threshold","label":"灰度差阈值","type":"int","default":20,"min":1,"max":255,"description":"圆周连续像素比较的灰度差阈值（典型 10~40，越大角点越少越稳）"},
//     {"name":"nonmax","label":"非极大值抑制","type":"int","default":1,"min":0,"max":1,"description":"1=开启 3x3 非极大值抑制去重"}
//   ],
//   "outputs": [
//     {"name":"标注图","type":"image"},
//     {"name":"点集","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiFastFeatures(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int threshold = 20, nonmax = 1;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) threshold = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) nonmax = p->i;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Point2f* corners = NULL;
    int count = 0;
    const int rc = Image2DAlg_FastFeatures(&input, threshold, nonmax, &corners, &count);
    if (rc == -3) { out->error = "FAST 检测失败（threshold 需 > 0）"; return 3; }
    if (rc != 0) { out->error = "FAST 检测失败"; return 2; }

    // 点集 -> z=0 点云输出（corner_sub_pix 同款：先组点云，失败路径回收 corners）
    float* xy = (float*)malloc(sizeof(float) * (size_t)(count > 0 ? count : 1) * 2);
    if (!xy) { Image2DAlg_FreePoint2f(corners); out->error = "内存不足"; return 2; }
    for (int i = 0; i < count; ++i) {
        xy[i * 2]     = corners[i].x;
        xy[i * 2 + 1] = corners[i].y;
    }
    out->clouds = AllocCloudOutZ0(xy, count);
    free(xy);
    if (!out->clouds) { Image2DAlg_FreePoint2f(corners); out->error = "内存不足"; return 2; }
    out->cloudCount = 1;

    // 标注图：输入克隆叠加角点绿色十字（同 Harris 画法），并在释放前组消息
    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i) {
        const cv::Point c((int)std::lround(corners[i].x), (int)std::lround(corners[i].y));
        cv::line(drawn, c + cv::Point(-4, 0), c + cv::Point(4, 0), cv::Scalar(0, 255, 0, 255), 1);
        cv::line(drawn, c + cv::Point(0, -4), c + cv::Point(0, 4), cv::Scalar(0, 255, 0, 255), 1);
    }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 48; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d](%.1f,%.1f)", i, corners[i].x, corners[i].y);
    Image2DAlg_FreePoint2f(corners);

    const size_t n = (size_t)drawn.rows * drawn.cols * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) {
        free(out->clouds[0].xyz); free(out->clouds);
        out->clouds = NULL; out->cloudCount = 0;
        out->error = "内存不足"; return 2;
    }
    memcpy(px, drawn.data, n);
    out->images = AllocImageOut(drawn.cols, drawn.rows, px);
    if (!out->images) {
        free(px);
        free(out->clouds[0].xyz); free(out->clouds);
        out->clouds = NULL; out->cloudCount = 0;
        out->error = "内存不足"; return 2;
    }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：特征对位（node.json: alg.image2d.feature_align） ----------------
// 一站式特征链路（ORBAlign 泛化版）：内部串联 FastFeatures → ComputeDescriptors ×2 →
// MatchFeatures → EstimateAffinePartial / FindHomography——描述子为二进制块无法在
// 节点间流转（ABI v1 只有 image/cloud/消息通道），故检测/描述/匹配不拆节点。
// 输入 2 张图像：images[0]=基准图（in1），images[1]=待对位图（in2）。
// 参数（按声明顺序）：
//   type             int     默认 0   [0,3]     0=ORB 1=BRISK 2=AKAZE 3=SIFT
//   n_features       int     默认 500 [50,2000]  仅 ORB 有效（同 ORBAlign）；其他类型忽略
//   ratio            number  默认 0.7 [0.1,0.9]  Lowe 比率测试系数
//   motion           int     默认 0   [0,1]      0=部分仿射 4DOF（旋转+等比缩放+平移，工业对位推荐）
//                                           1=单应 8DOF（平面/大视角透视）
//   ransac_threshold number  默认 3   [0.1,100]  RANSAC 重投影阈值（像素）
//   fast_threshold   int     默认 20  [1,255]    内部 FAST 检测的灰度差阈值
// 输出：待对位图克隆叠加匹配连线（两图同尺寸时）+ 匹配点十字（orb_align 画法）。
// 消息通道："inliers=N matches=M m=[..6 值..]" / "h=[..9 值..]"（in1 → in2 方向）。
// 错误码：1 输入不足；2 特征/匹配/几何估计不足（-2，平坦图属正常）；3 参数非法。

// FAST 检出 + 剔除贴边关键点：核心层 ORB compute 对贴边点（邻域不足）会整批
// 判 -2，一站式节点与 ORBAlign 一致在入口消化（fast_features 节点输出仍忠实
// 于 C API 原始结果，不受影响）。剔除边距取 8px（核心层 ORB edgeThreshold 同值）。
// 保留口径：坐标 x,y ∈ [8, w-1-8]/[8, h-1-8]——关键点四周都有完整 8px 邻域，
// 两侧对称（左右各留 8、上下各留 8），与核心层描述子计算的可算范围严格一致。
// 原地压缩实现（不二次分配）：kept 直接覆写 *out_pts 前段，块所有权仍归
// FreePoint2f，调用方失败路径只需一次 FreePoint2f。
static int DetectTrimmedFast(const Image2DAlg_Image* img, int fast_threshold,
                             Image2DAlg_Point2f** out_pts, int* out_count) {
    const int margin = 8;
    const int rc = Image2DAlg_FastFeatures(img, fast_threshold, 1, out_pts, out_count);
    if (rc != 0 || !*out_pts || *out_count <= 0) return rc;
    const int w = img->width, h = img->height;
    int kept = 0;
    for (int i = 0; i < *out_count; ++i) {
        const float x = (*out_pts)[i].x, y = (*out_pts)[i].y;
        if (x >= (float)margin && y >= (float)margin &&
            x <= (float)(w - 1 - margin) && y <= (float)(h - 1 - margin)) {
            (*out_pts)[kept++] = (*out_pts)[i];   // 原地压缩（分配块仍归 FreePoint2f 释放）
        }
    }
    *out_count = kept;
    return 0;
}

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.feature_align","name":"特征对位","group":"2D 图像","category":"处理","description":"特征链路一站式对位（ORBAlign 泛化版）：内部串联 FAST 检测→描述子(ORB/BRISK/AKAZE/SIFT)→Lowe 比率匹配→RANSAC 几何估计（部分仿射 4DOF/单应 8DOF）；输出匹配连线标注，变换矩阵/内点数/匹配对数并入节点消息"},
//   "inputs": [
//     {"name":"基准图","type":"image"},
//     {"name":"待对位图","type":"image"}
//   ],
//   "params": [
//     {"name":"type","label":"描述子类型","type":"int","default":0,"min":0,"max":3,"description":"0=ORB(二进制,最快) 1=BRISK(二进制) 2=AKAZE(二进制) 3=SIFT(浮点128维,最稳)；两图使用同类型"},
//     {"name":"n_features","label":"特征点上限","type":"int","default":500,"min":50,"max":2000,"description":"仅 ORB 有效（同 ORBAlign）；其他类型忽略"},
//     {"name":"ratio","label":"Lowe比率","type":"number","default":0.7,"min":0.1,"max":0.9,"description":"Lowe 比率测试系数（越大匹配越多、误配也随之增多）"},
//     {"name":"motion","label":"运动模型","type":"int","default":0,"min":0,"max":1,"description":"0=部分仿射 4DOF（旋转+等比缩放+平移，约束强抗误配，工业对位推荐） 1=单应 8DOF（平面/大视角透视）"},
//     {"name":"ransac_threshold","label":"RANSAC阈值(px)","type":"number","default":3.0,"min":0.1,"max":100,"description":"RANSAC 重投影阈值（像素，典型 3.0；内点数 >= 10 视为可靠）"},
//     {"name":"fast_threshold","label":"FAST阈值","type":"int","default":20,"min":1,"max":255,"description":"内部 FAST 检测的灰度差阈值（典型 10~40；特征过密/误配时调大）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiFeatureAlign(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（基准图/待对位图）";
        return 1;
    }
    int type = 0, n_features = 500, motion = 0, fast_threshold = 20;
    double ratio = 0.7, ransac_threshold = 3.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) type = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) n_features = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) ratio = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) motion = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) ransac_threshold = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 3)) fast_threshold = p->i;
    if (type < 0 || type > 3) { out->error = "type 仅支持 0=ORB/1=BRISK/2=AKAZE/3=SIFT"; return 3; }
    if (motion != 0 && motion != 1) {
        out->error = "motion 仅支持 0=部分仿射(4DOF)/1=单应(8DOF)"; return 3;
    }

    Image2DAlg_Image i1 = CoreInputFrom(in->images[0]);
    Image2DAlg_Image i2 = CoreInputFrom(in->images[1]);

    // 两侧各自 FAST 检出（非极大值抑制开启）+ 贴边剔除
    Image2DAlg_Point2f* pts1 = NULL; int n1 = 0;
    Image2DAlg_Point2f* pts2 = NULL; int n2 = 0;
    int rc = DetectTrimmedFast(&i1, fast_threshold, &pts1, &n1);
    if (rc == -3) { out->error = "FAST 阈值非法（fast_threshold 需 > 0）"; return 3; }
    if (rc != 0) { out->error = "基准图特征检测失败"; return 2; }
    rc = DetectTrimmedFast(&i2, fast_threshold, &pts2, &n2);
    if (rc == -3) {
        Image2DAlg_FreePoint2f(pts1);
        out->error = "FAST 阈值非法（fast_threshold 需 > 0）"; return 3;
    }
    if (rc != 0) { Image2DAlg_FreePoint2f(pts1); out->error = "待对位图特征检测失败"; return 2; }
    if (n1 == 0 || n2 == 0) {
        Image2DAlg_FreePoint2f(pts1);
        Image2DAlg_FreePoint2f(pts2);
        out->error = "特征不足（平坦图或 fast_threshold 过高，属正常）"; return 2;
    }

    Image2DAlg_Descriptors d1 = {}, d2 = {};
    rc = Image2DAlg_ComputeDescriptors(&i1, pts1, n1, type, n_features, &d1);
    if (rc == -2) {
        Image2DAlg_FreePoint2f(pts1); Image2DAlg_FreePoint2f(pts2);
        out->error = "基准图描述子计算失败（关键点贴边或图像过小）"; return 2;
    }
    if (rc == -3) {
        Image2DAlg_FreePoint2f(pts1); Image2DAlg_FreePoint2f(pts2);
        out->error = "描述子类型非法（type 0~3）"; return 3;
    }
    if (rc != 0) {
        Image2DAlg_FreePoint2f(pts1); Image2DAlg_FreePoint2f(pts2);
        out->error = "基准图描述子计算失败"; return 2;
    }
    rc = Image2DAlg_ComputeDescriptors(&i2, pts2, n2, type, n_features, &d2);
    if (rc != 0) {
        Image2DAlg_FreeDescriptors(&d1);
        Image2DAlg_FreePoint2f(pts1); Image2DAlg_FreePoint2f(pts2);
        out->error = (rc == -2) ? "待对位图描述子计算失败（关键点贴边或图像过小）"
                                : "待对位图描述子计算失败";
        return 2;
    }

    Image2DAlg_OrbMatch* matches = NULL;
    int count = 0;
    rc = Image2DAlg_MatchFeatures(&d1, &d2, pts1, pts2, ratio, &matches, &count);
    Image2DAlg_FreeDescriptors(&d1);
    Image2DAlg_FreeDescriptors(&d2);
    Image2DAlg_FreePoint2f(pts1);
    Image2DAlg_FreePoint2f(pts2);
    if (rc == -2) { out->error = "匹配不足（< 2 对，弱纹理图属正常）"; return 2; }
    if (rc == -3) { out->error = "匹配失败（ratio 须在 (0,1)）"; return 3; }
    if (rc != 0) { out->error = "匹配失败"; return 2; }

    double m6[6] = { 0 }, h9[9] = { 0 };
    int inliers = 0;
    rc = (motion == 0)
        ? Image2DAlg_EstimateAffinePartial(matches, count, ransac_threshold, m6, &inliers)
        : Image2DAlg_FindHomography(matches, count, ransac_threshold, h9, &inliers);
    if (rc == -2) {
        Image2DAlg_FreeOrbMatches(matches);
        out->error = "几何估计失败（匹配对数不足或分布退化）"; return 2;
    }
    if (rc == -3) {
        Image2DAlg_FreeOrbMatches(matches);
        out->error = "几何估计失败（ransac_threshold 需 > 0）"; return 3;
    }
    if (rc != 0) {
        Image2DAlg_FreeOrbMatches(matches);
        out->error = "几何估计失败"; return 2;
    }

    // 标注图：待对位图克隆 + 匹配连线（x1/y1 为基准图坐标，两图同尺寸时可在
    // 待对位图系内连线展示位移；尺寸不同退化为仅画匹配点十字——orb_align 画法）
    cv::Mat vis(in->images[1].height, in->images[1].width, CV_8UC4,
                (void*)in->images[1].bgra);
    cv::Mat drawn = vis.clone();
    const bool same_size = in->images[0].width == in->images[1].width &&
                           in->images[0].height == in->images[1].height;
    for (int i = 0; i < count; ++i) {
        const cv::Point a((int)std::lround(matches[i].x1), (int)std::lround(matches[i].y1));
        const cv::Point b((int)std::lround(matches[i].x2), (int)std::lround(matches[i].y2));
        if (same_size)
            cv::line(drawn, a, b, cv::Scalar(0, 255, 0, 255), 1, cv::LINE_AA);
        cv::line(drawn, b + cv::Point(-4, 0), b + cv::Point(4, 0), cv::Scalar(0, 255, 0, 255), 1);
        cv::line(drawn, b + cv::Point(0, -4), b + cv::Point(0, 4), cv::Scalar(0, 255, 0, 255), 1);
    }

    static char msg[256];
    if (motion == 0)
        snprintf(msg, sizeof(msg),
                 "inliers=%d matches=%d m=[%.4f %.4f %.4f; %.4f %.4f %.4f]",
                 inliers, count, m6[0], m6[1], m6[2], m6[3], m6[4], m6[5]);
    else
        snprintf(msg, sizeof(msg),
                 "inliers=%d matches=%d h=[%.4f %.4f %.4f; %.4f %.4f %.4f; %.4f %.4f %.4f]",
                 inliers, count, h9[0], h9[1], h9[2], h9[3], h9[4], h9[5],
                 h9[6], h9[7], h9[8]);
    Image2DAlg_FreeOrbMatches(matches);

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

// ---------------- 算子入口：对应点变换估计（node.json: alg.image2d.estimate_transform） ----------------
// 输入 2 个点集：clouds[0]=对应点 A（in1 侧），clouds[1]=对应点 B（in2 侧）——
// 两组数量必须相等（一一对应），xy 取自 xyz（z 分量忽略，fit_circle 点集同款约定）。
// 给已有外部对应点的调用方独立复用几何估计（与 feature_align 的内部链路解耦）。
// 参数（按声明顺序）：
//   motion           int     默认 0  [0,1]     0=部分仿射 4DOF / 1=单应 8DOF
//   ransac_threshold number  默认 3  [0.1,100] RANSAC 重投影阈值（像素）
// 输出：无端口（outputs 为空）；变换矩阵与内点数并入消息通道（match_shapes 同款）：
//   "inliers=N pairs=M m=[..6 值..]" / "h=[..9 值..]"（A → B 方向）。
// 错误码：1 输入不足（需 2 个点集）；2 点集为空/数量不等/几何估计失败（-2）；
//         3 参数非法（motion 越界 / ransac_threshold <= 0）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.estimate_transform","name":"对应点变换估计","group":"2D 图像","category":"测量","description":"两组数量相等的 2D 对应点做 RANSAC 几何估计（0=部分仿射 4DOF / 1=单应 8DOF）；xy 取自点云 xyz（z 忽略），矩阵与内点数并入节点消息；供已有外部对应点的调用方独立复用几何估计"},
//   "inputs": [
//     {"name":"对应点A","type":"cloud"},
//     {"name":"对应点B","type":"cloud"}
//   ],
//   "params": [
//     {"name":"motion","label":"运动模型","type":"int","default":0,"min":0,"max":1,"description":"0=部分仿射 4DOF（旋转+等比缩放+平移，>=3 点） 1=单应 8DOF（平面/透视，>=4 点）"},
//     {"name":"ransac_threshold","label":"RANSAC阈值(px)","type":"number","default":3.0,"min":0.1,"max":100,"description":"RANSAC 重投影阈值（像素，典型 3.0）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiEstimateTransform(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 2 || !in->clouds || !in->clouds[1].xyz) {
        out->error = "需要 2 个点集输入（对应点 A/对应点 B）"; return 1;
    }
    int motion = 0;
    double ransac_threshold = 3.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) motion = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) ransac_threshold = p->d;
    if (motion != 0 && motion != 1) {
        out->error = "motion 仅支持 0=部分仿射(4DOF)/1=单应(8DOF)"; return 3;
    }

    const AlgoCCloud* a = &in->clouds[0];
    const AlgoCCloud* b = &in->clouds[1];
    if (a->count <= 0 || !a->xyz) { out->error = "对应点 A 为空"; return 2; }
    if (b->count <= 0 || !b->xyz) { out->error = "对应点 B 为空"; return 2; }
    if (a->count != b->count) { out->error = "两组对应点数量必须相等（一一对应）"; return 2; }

    Image2DAlg_OrbMatch* matches =
        (Image2DAlg_OrbMatch*)malloc(sizeof(Image2DAlg_OrbMatch) * (size_t)a->count);
    if (!matches) { out->error = "内存不足"; return 2; }
    for (int i = 0; i < a->count; ++i) {
        matches[i].x1 = a->xyz[i * 3];
        matches[i].y1 = a->xyz[i * 3 + 1];
        matches[i].x2 = b->xyz[i * 3];
        matches[i].y2 = b->xyz[i * 3 + 1];
        matches[i].distance = 0.0;
    }

    double m6[6] = { 0 }, h9[9] = { 0 };
    int inliers = 0;
    const int rc = (motion == 0)
        ? Image2DAlg_EstimateAffinePartial(matches, a->count, ransac_threshold, m6, &inliers)
        : Image2DAlg_FindHomography(matches, a->count, ransac_threshold, h9, &inliers);
    free(matches);
    if (rc == -2) { out->error = "几何估计失败（点数不足 3/4 或共线退化）"; return 2; }
    if (rc == -3) { out->error = "几何估计失败（ransac_threshold 需 > 0）"; return 3; }
    if (rc != 0) { out->error = "几何估计失败"; return 2; }

    static char msg[256];
    if (motion == 0)
        snprintf(msg, sizeof(msg),
                 "inliers=%d pairs=%d m=[%.4f %.4f %.4f; %.4f %.4f %.4f]",
                 inliers, a->count, m6[0], m6[1], m6[2], m6[3], m6[4], m6[5]);
    else
        snprintf(msg, sizeof(msg),
                 "inliers=%d pairs=%d h=[%.4f %.4f %.4f; %.4f %.4f %.4f; %.4f %.4f %.4f]",
                 inliers, a->count, h9[0], h9[1], h9[2], h9[3], h9[4], h9[5],
                 h9[6], h9[7], h9[8]);
    out->error = msg;
    return 0;
}

} // extern "C"

// =====================================================================
// 2.2.0 新增族接入（2026-11-19，7 个入口，追加在文件尾部）：
//   alg.image2d.blob_detect       —— BlobDetect（+FreeBlobFeatures）
//   alg.image2d.detect_and_match  —— DetectAndMatch（一站式检测+描述+匹配）
//   alg.image2d.optical_flow_lk   —— CalcOpticalFlowPyrLK（out_status 入口内预分配）
//   alg.image2d.ecc_align         —— FindTransformECC（m 方向=模板坐标取 input(m·x)）
//   alg.image2d.phase_correlate   —— PhaseCorrelate（b 相对 a 位移）
//   alg.image2d.stereo_match      —— StereoMatch（out_disparity 入口内预分配）
//   alg.image2d.solve_pnp         —— SolvePnP（点集×2 + camera_matrix text 先例）
// 本段只做参数组装、错误码映射与消息拼装，不新增算法；text 多值参数沿用
// manual_regist init_transform 的承载先例（ParseDoubleList 同款解析）。
// =====================================================================

// solve_pnp 的 cv::Rodrigues：calib3d 在 opencv_world440 内全含，主库
// Image2DAlg_Ops.cpp 已包含同一头文件，无新增链接依赖。
#include <opencv2/calib3d.hpp>

// 逗号分隔 double 列表解析（Point3DAlg_comui.cpp ParseDoubleList 同款实现，
// 两份拷贝行为必须保持一致——宿主 marshal 无参数名，文本参数格式即契约）：
//   NULL/空串            -> 0（可选参数缺省口径，如 solve_pnp 无畸变）
//   连续/首尾分隔符       -> 跳过（"1,,2," 与 "1,2" 等价；容错宿主/人工拼接尾巴）
//   非数字 token          -> -1（如 "1,2,abc"——数量校验前先报坏值，rc=3）
//   值个数超 max_n        -> -1（数量不符统一 -1，由调用方换算成 rc=3，
//                            保证 camera_matrix 9 值 / dist_coeffs 4/5/8/12/14
//                            等校验不越界读 vals）
// 返回值 n：实际解析个数，供调用方做"数量必须恰好 N"校验。
static int ParseDoubleList(const char* text, double* vals, int max_n) {
    if (!text || !text[0]) return 0;
    int n = 0;
    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
               *p == ',' || *p == ';' || *p == '[' || *p == ']') ++p;
        if (!*p) break;
        char* end = nullptr;
        const double v = strtod(p, &end);
        if (end == p) return -1;   // 非数字 token
        if (n >= max_n) return -1; // 超出容量（数量不符）
        vals[n++] = v;
        p = end;
    }
    return n;
}

extern "C" {

// ---------------- 算子入口：斑点检测（node.json: alg.image2d.blob_detect） ----------------
// 参数（按声明顺序）：
//   min_area        number  默认 100 [0,1e7]  面积过滤下限（像素²；<=0 = 无下限）
//   max_area        number  默认 0   [0,1e7]  面积过滤上限（<=0 = 不设上限）
//   min_circularity number  默认 0   [0,1]    最小圆度（<=0 显式关闭圆度过滤）
//   min_convexity   number  默认 0   [0,1]    最小凸性（<=0 显式关闭凸性过滤）
// 输出：输入图克隆叠加斑点绿色圆周标注（cx,cy,r）。
// 消息通道："count=N; [i]c=(cx,cy) r=R; ..."（900 字节截断；无检出 count=0 属正常）。
// 边界：快速定位（多阈值聚合+形状过滤）；精密测量接 fit_circles（逐轮廓最小二乘）。
// 错误码：1 输入未连线；2 检测失败/内存不足；3 参数/通道不支持。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.blob_detect","name":"斑点检测","group":"2D 图像","category":"处理","description":"SimpleBlobDetector 斑点检测（圆点/焊盘快速定位：灰度多阈值二值化+连通域形状过滤聚合）：输出圆标注图，圆心/半径并入节点消息；圆度/凸性 <=0 显式关闭对应过滤；精密测量请接 fit_circles（逐轮廓最小二乘拟合）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"min_area","label":"面积下限","type":"number","default":100,"min":0,"max":10000000,"description":"面积过滤下限（像素²，<=0 = 无下限）"},
//     {"name":"max_area","label":"面积上限","type":"number","default":0,"min":0,"max":10000000,"description":"面积过滤上限（<=0 = 不设上限）"},
//     {"name":"min_circularity","label":"最小圆度","type":"number","default":0,"min":0,"max":1,"description":"最小圆度 [0,1]（<=0 显式关闭圆度过滤）"},
//     {"name":"min_convexity","label":"最小凸性","type":"number","default":0,"min":0,"max":1,"description":"最小凸性 [0,1]（<=0 显式关闭凸性过滤；OpenCV 4.4 默认 Params 凸性过滤开启 0.95，会滤掉光栅化圆的锯齿轮廓）"}
//   ],
//   "outputs": [
//     {"name":"标注图","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiBlobDetect(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double min_area = 100.0, max_area = 0.0, min_circ = 0.0, min_conv = 0.0;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) min_area = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) max_area = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) min_circ = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) min_conv = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_BlobFeature* blobs = NULL;
    int count = 0;
    const int rc = Image2DAlg_BlobDetect(&input, min_area, max_area, min_circ, min_conv,
                                         &blobs, &count);
    if (rc == -3) { out->error = "斑点检测失败（通道不支持）"; return 3; }
    if (rc != 0) { out->error = "斑点检测失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i)
        cv::circle(drawn, cv::Point2f(blobs[i].x, blobs[i].y), blobs[i].radius,
                   cv::Scalar(0, 255, 0, 255), 1, cv::LINE_AA);

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 64; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]c=(%.1f,%.1f) r=%.1f", i, blobs[i].x, blobs[i].y, blobs[i].radius);
    Image2DAlg_FreeBlobFeatures(blobs);

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

// ---------------- 算子入口：一站式检测匹配（node.json: alg.image2d.detect_and_match） ----------------
// DetectAndMatch = 检测（detectAndCompute）+ 描述 + 匹配（BFMatcher kNN + Lowe 比率）
// 一站完成，复用 OrbMatch 结构——给不想管理描述子生命周期的调用方。
// 与 feature_align 的边界：feature_align（FAST 检出 → 任意 type 描述子 → 匹配 →
// RANSAC 几何估计，输出变换矩阵）；本节点泛化检测器但不做几何估计、不输出矩阵，
// 需要矩阵时接 feature_align，或把匹配对交 estimate_transform 二次求解。
// 输入 2 张图像：images[0]=基准图（in1），images[1]=待对位图（in2）。
// 参数（按声明顺序）：
//   type        int     默认 0   [0,3]      0=ORB 1=BRISK 2=AKAZE 3=SIFT
//   n_features  int     默认 500 [50,2000]  仅 ORB（<=0 取默认 500）/SIFT（<=0 不限）有效；
//                                           BRISK/AKAZE 无上限参数，忽略
//   ratio       number  默认 0.7 [0.1,0.9]  Lowe 比率测试系数
// 输出：待对位图克隆叠加匹配连线（两图同尺寸时）+ 匹配点十字（feature_align 画法）。
// 消息通道："count=N type=K; [i](x1,y1)->(x2,y2) d=D; ..."（900 字节截断）。
// 错误码：1 输入不足；2 特征/匹配不足（-2，平坦图属正常）；3 参数非法。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.detect_and_match","name":"特征检测匹配","group":"2D 图像","category":"处理","description":"一站式特征检测+描述+匹配（type 0=ORB/1=BRISK/2=AKAZE/3=SIFT，检测描述一次完成+BFMatcher 比率匹配）：输出匹配点标注图，匹配对样例并入节点消息；与 feature_align 的边界——本节点不做 RANSAC 几何估计、不输出变换矩阵，需矩阵时接 feature_align 或把匹配对交 estimate_transform 二次求解"},
//   "inputs": [
//     {"name":"基准图","type":"image"},
//     {"name":"待对位图","type":"image"}
//   ],
//   "params": [
//     {"name":"type","label":"特征类型","type":"int","default":0,"min":0,"max":3,"description":"0=ORB(二进制32字节) 1=BRISK(二进制64字节) 2=AKAZE(二进制61字节) 3=SIFT(浮点128维)"},
//     {"name":"n_features","label":"特征点上限","type":"int","default":500,"min":50,"max":2000,"description":"仅 ORB（<=0 取默认 500）/ SIFT（<=0 不限）有效；BRISK/AKAZE 无上限参数，忽略"},
//     {"name":"ratio","label":"Lowe比率","type":"number","default":0.7,"min":0.1,"max":0.9,"description":"Lowe 比率测试阈值（典型 0.7~0.8；越大匹配越多、误配也随之增多）"}
//   ],
//   "outputs": [
//     {"name":"标注图","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiDetectAndMatch(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（基准图/待对位图）";
        return 1;
    }
    int type = 0, n_features = 500;
    double ratio = 0.7;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) type = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) n_features = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) ratio = p->d;
    if (type < 0 || type > 3) { out->error = "type 仅支持 0=ORB/1=BRISK/2=AKAZE/3=SIFT"; return 3; }
    if (ratio <= 0.0 || ratio >= 1.0) { out->error = "ratio 须在 (0,1)（典型 0.7~0.8）"; return 3; }

    Image2DAlg_Image i1 = CoreInputFrom(in->images[0]);
    Image2DAlg_Image i2 = CoreInputFrom(in->images[1]);
    Image2DAlg_OrbMatch* matches = NULL;
    int count = 0;
    const int rc = Image2DAlg_DetectAndMatch(&i1, &i2, type, n_features, ratio,
                                             &matches, &count);
    if (rc == -2) { out->error = "特征/匹配不足（任一图无检出或比率测试后 < 2 对，平坦图属正常）"; return 2; }
    if (rc == -3) { out->error = "检测匹配失败（type 0~3、ratio∈(0,1)）"; return 3; }
    if (rc != 0) { out->error = "检测匹配失败"; return 2; }

    cv::Mat vis(in->images[1].height, in->images[1].width, CV_8UC4,
                (void*)in->images[1].bgra);
    cv::Mat drawn = vis.clone();
    const bool same_size = in->images[0].width == in->images[1].width &&
                           in->images[0].height == in->images[1].height;
    for (int i = 0; i < count; ++i) {
        const cv::Point a((int)std::lround(matches[i].x1), (int)std::lround(matches[i].y1));
        const cv::Point b((int)std::lround(matches[i].x2), (int)std::lround(matches[i].y2));
        if (same_size)
            cv::line(drawn, a, b, cv::Scalar(0, 255, 0, 255), 1, cv::LINE_AA);
        cv::line(drawn, b + cv::Point(-4, 0), b + cv::Point(4, 0), cv::Scalar(0, 255, 0, 255), 1);
        cv::line(drawn, b + cv::Point(0, -4), b + cv::Point(0, 4), cv::Scalar(0, 255, 0, 255), 1);
    }

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d type=%d", count, type);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 96; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d](%.1f,%.1f)->(%.1f,%.1f) d=%.1f",
                        i, matches[i].x1, matches[i].y1, matches[i].x2, matches[i].y2,
                        matches[i].distance);
    Image2DAlg_FreeOrbMatches(matches);

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

// ---------------- 算子入口：LK 光流跟踪（node.json: alg.image2d.optical_flow_lk） ----------------
// 金字塔 LK 光流（cv::calcOpticalFlowPyrLK）：视频/序列帧间点跟踪。
// 输入：images[0]=前帧，images[1]=后帧（尺寸须一致），clouds[0]=初始点（xy，z 忽略，>=1 点）。
// 参数（按声明顺序）：
//   win_size   int  默认 21 [5,101]  搜索窗边长（<=0 -> -3；偶数核心层自动 +1）
//   max_level  int  默认 3  [0,10]  金字塔层数（<0 -> -3）
// out_status 数组在入口内 malloc 预分配（count 字节，C API 一级指针契约——
// 本库不分配该缓冲），统计 tracked/lost 后 free。
// 输出：跟踪成功的点集点云（z=0，corner_sub_pix 同款；失败点位置无意义，不输出）。
// 消息通道："tracked=N lost=M total=K; [i](x,y); ..."（900 字节截断）。
// 错误码：1 输入未连线（2 图 + 1 点集）；2 初始点为空/跟踪失败/内存不足；3 参数非法。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.optical_flow_lk","name":"LK 光流跟踪","group":"2D 图像","category":"处理","description":"金字塔 LK 光流跟踪（视频/序列帧间点跟踪）：前帧+后帧+初始点 → 跟踪后点云（仅保留跟踪成功的点，z=0），tracked/lost 计数与点坐标并入节点消息；初始点可来自 fast_features/harris_corners 的点集输出"},
//   "inputs": [
//     {"name":"前帧","type":"image"},
//     {"name":"后帧","type":"image"},
//     {"name":"初始点","type":"cloud"}
//   ],
//   "params": [
//     {"name":"win_size","label":"搜索窗边长","type":"int","default":21,"min":5,"max":101,"description":"LK 搜索窗边长（典型 21；偶数由核心层自动 +1）"},
//     {"name":"max_level","label":"金字塔层数","type":"int","default":3,"min":0,"max":10,"description":"金字塔层数（典型 3；越大越能处理大位移，越慢）"}
//   ],
//   "outputs": [
//     {"name":"跟踪点","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiOpticalFlowLk(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（前帧/后帧）"; return 1;
    }
    if (in->cloudCount < 1 || !in->clouds || !in->clouds[0].xyz) {
        out->error = "需要点集输入（初始点）"; return 1;
    }
    int win_size = 21, max_level = 3;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) win_size = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) max_level = p->i;
    if (win_size <= 0 || max_level < 0) {
        out->error = "参数非法（win_size 须 > 0，max_level 须 >= 0）"; return 3;
    }

    const AlgoCCloud* src = &in->clouds[0];
    const int count = src->count;
    if (count < 1) { out->error = "初始点为空"; return 2; }

    Image2DAlg_Point2f* pts = (Image2DAlg_Point2f*)malloc(sizeof(Image2DAlg_Point2f) * (size_t)count);
    unsigned char* status = (unsigned char*)malloc((size_t)count);   // out_status 调用方预分配
    if (!pts || !status) { free(pts); free(status); out->error = "内存不足"; return 2; }
    for (int i = 0; i < count; ++i) {
        pts[i].x = src->xyz[i * 3];
        pts[i].y = src->xyz[i * 3 + 1];
    }

    Image2DAlg_Image prev = CoreInputFrom(in->images[0]);
    Image2DAlg_Image next = CoreInputFrom(in->images[1]);
    Image2DAlg_Point2f* tracked = NULL;
    const int rc = Image2DAlg_CalcOpticalFlowPyrLK(&prev, &next, pts, count,
                                                   win_size, max_level, &tracked, status);
    free(pts);
    if (rc == -3) {
        free(status);
        out->error = "光流跟踪失败（win_size/max_level 非法、两帧尺寸须一致或初始点为空）";
        return 3;
    }
    if (rc != 0) { free(status); out->error = "光流跟踪失败"; return 2; }

    int tracked_n = 0;
    for (int i = 0; i < count; ++i)
        if (status[i]) ++tracked_n;

    // 仅跟踪成功的点 -> z=0 点云输出（corner_sub_pix 同款：先组点云再释放 tracked）
    float* xy = (float*)malloc(sizeof(float) * (size_t)(tracked_n > 0 ? tracked_n : 1) * 2);
    if (!xy) {
        free(status); Image2DAlg_FreePoint2f(tracked);
        out->error = "内存不足"; return 2;
    }
    int k = 0;
    for (int i = 0; i < count; ++i) {
        if (!status[i]) continue;
        xy[k * 2]     = tracked[i].x;
        xy[k * 2 + 1] = tracked[i].y;
        ++k;
    }
    out->clouds = AllocCloudOutZ0(xy, tracked_n);
    free(xy);
    if (!out->clouds) {
        free(status); Image2DAlg_FreePoint2f(tracked);
        out->error = "内存不足"; return 2;
    }
    out->cloudCount = 1;

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "tracked=%d lost=%d total=%d",
                       tracked_n, count - tracked_n, count);
    k = 0;
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 48; ++i) {
        if (!status[i]) continue;
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d](%.1f,%.1f)", k, tracked[i].x, tracked[i].y);
        ++k;
    }
    free(status);
    Image2DAlg_FreePoint2f(tracked);
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：ECC 图像对齐（node.json: alg.image2d.ecc_align） ----------------
// ECC 迭代最大化增强相关系数（cv::findTransformECC）：亚像素精度、对光照变化较稳。
// 输入 2 张图像：images[0]=模板图（templ），images[1]=待配准图（input，尺寸须一致）。
// 参数（按声明顺序）：
//   motion    int     默认 1    [0,2]       0=平移(2x3) 1=仿射(2x3) 2=单应(3x3)
//                                           （注意与 cv::MOTION_* 原生枚举不同——
//                                           本 API 未暴露欧氏模型）
//   max_iters int     默认 50   [1,500]     迭代上限
//   epsilon   number  默认 1e-5 [1e-9,0.1]  收敛阈值（相邻迭代相关系数增量）
// 输出：无端口（outputs 为空）；m 方向 = 模板坐标 x 处取 input(m·x)——即
//   warpAffine(input, m, WARP_INVERSE_MAP) 可将 input 对齐到 templ（单应接
//   WarpPerspective 同理），矩阵与相关系数并入消息通道（region_stats 先例）：
//   "motion=K corr=%.4f m=[..6 或 9 值..]"。
// 错误码：1 输入不足；2 不收敛/相关性不足（-2，两图无结构/形变过大属正常，可重试）；
//         3 参数非法（motion/迭代/epsilon 越界或两图尺寸不一致）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.ecc_align","name":"ECC 图像对齐","group":"2D 图像","category":"处理","description":"ECC 迭代最大化增强相关系数对齐（cv::findTransformECC，亚像素精度、对光照变化较稳）：输入模板图/待配准图（尺寸须一致）；变换矩阵（m 方向=模板坐标取 input(m·x)，即 warpAffine(input, m, WARP_INVERSE_MAP) 对齐到模板）与相关系数并入节点消息；不收敛/相关性<0.5 报错（两图无结构或形变过大属正常，可重试）"},
//   "inputs": [
//     {"name":"模板图","type":"image"},
//     {"name":"待配准图","type":"image"}
//   ],
//   "params": [
//     {"name":"motion","label":"变换模型","type":"int","default":1,"min":0,"max":2,"description":"0=平移(2x3) 1=仿射(2x3) 2=单应(3x3)（注意与 cv::MOTION_* 原生枚举取值不同：cv 的 1 是欧氏，本 API 未暴露欧氏模型）"},
//     {"name":"max_iters","label":"迭代上限","type":"int","default":50,"min":1,"max":500,"description":"ECC 迭代上限（越大越慢、越可能收敛）"},
//     {"name":"epsilon","label":"收敛阈值","type":"number","default":1e-05,"min":1e-09,"max":0.1,"description":"收敛阈值（相邻迭代相关系数增量，典型 1e-5）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiEccAlign(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（模板图/待配准图）"; return 1;
    }
    int motion = 1, max_iters = 50;
    double epsilon = 1e-5;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) motion = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) max_iters = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) epsilon = p->d;
    if (motion < 0 || motion > 2) {
        out->error = "motion 仅支持 0=平移(2x3)/1=仿射(2x3)/2=单应(3x3)"; return 3;
    }
    if (max_iters < 1 || epsilon <= 0.0) {
        out->error = "参数非法（max_iters >= 1，epsilon > 0）"; return 3;
    }

    Image2DAlg_Image tpl = CoreInputFrom(in->images[0]);
    Image2DAlg_Image inp = CoreInputFrom(in->images[1]);
    double m[9] = { 0 };
    double corr = 0.0;
    const int rc = Image2DAlg_FindTransformECC(&tpl, &inp, motion, max_iters, epsilon,
                                               m, &corr);
    if (rc == -2) { out->error = "ECC 不收敛或相关性不足（两图无结构/形变过大，属正常可重试）"; return 2; }
    if (rc == -3) { out->error = "ECC 对齐失败（motion 0~2、max_iters>=1、epsilon>0、两图尺寸须一致）"; return 3; }
    if (rc != 0) { out->error = "ECC 对齐失败"; return 2; }

    static char msg[400];
    if (motion == 2)
        snprintf(msg, sizeof(msg),
                 "motion=2 corr=%.4f m=[%.6f %.6f %.6f; %.6f %.6f %.6f; %.6f %.6f %.6f] "
                 "(模板坐标取 input(m·x)，WARP_INVERSE_MAP)",
                 corr, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
    else
        snprintf(msg, sizeof(msg),
                 "motion=%d corr=%.4f m=[%.6f %.6f %.6f; %.6f %.6f %.6f] "
                 "(模板坐标取 input(m·x)，WARP_INVERSE_MAP)",
                 motion, corr, m[0], m[1], m[2], m[3], m[4], m[5]);
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：相位相关平移估计（node.json: alg.image2d.phase_correlate） ----------------
// FFT 频域互功率谱相位斜率（cv::phaseCorrelate）：对光照/亮度变化极稳、亚像素精度。
// 输入 2 张图像：images[0]=基准图（a），images[1]=平移后图（b，尺寸须一致）。
// 参数：无。
// 输出：无端口（outputs 为空）；平移量与峰值响应并入消息通道（region_stats 先例）：
//   "dx=%.3f dy=%.3f response=%.4f（b 相对 a 位移；response>0.3 可信）"。
// 注：不启用窗口函数（cv 默认 noArray），强边缘效应场景先接模糊节点平滑。
// 错误码：1 输入不足；2 计算失败；3 两图尺寸不一致。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.phase_correlate","name":"相位相关平移","group":"2D 图像","category":"处理","description":"相位相关平移估计（FFT 频域互功率谱，对光照/亮度变化极稳、亚像素精度）：输入基准图/平移后图（尺寸须一致）；dx/dy（b 相对 a 的位移）与峰值响应（5x5 质心区域能量占比，>0.3 可信）并入节点消息；不启用窗口函数，强边缘效应场景先接模糊节点平滑"},
//   "inputs": [
//     {"name":"基准图","type":"image"},
//     {"name":"平移后图","type":"image"}
//   ],
//   "params": [],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiPhaseCorrelate(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（基准图/平移后图）"; return 1;
    }
    Image2DAlg_Image a = CoreInputFrom(in->images[0]);
    Image2DAlg_Image b = CoreInputFrom(in->images[1]);
    double dx = 0.0, dy = 0.0, response = 0.0;
    const int rc = Image2DAlg_PhaseCorrelate(&a, &b, &dx, &dy, &response);
    if (rc == -3) { out->error = "相位相关失败（两图尺寸须一致）"; return 3; }
    if (rc != 0) { out->error = "相位相关失败"; return 2; }

    static char msg[160];
    snprintf(msg, sizeof(msg), "dx=%.3f dy=%.3f response=%.4f（b 相对 a 位移；response>0.3 可信）",
             dx, dy, response);
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：双目立体匹配（node.json: alg.image2d.stereo_match） ----------------
// SGBM 半全局块匹配输出视差图（cv::StereoSGBM；配合 Image2DAlg_Calib.h 的立体
// 校正结果可转深度）。
// 输入 2 张图像：images[0]=左图，images[1]=右图（已校正灰度语义图，尺寸须一致；
// 视差约定 = xL - xR >= 0）。
// 参数（按声明顺序）：
//   min_disparity   int  默认 0  [-128,128]  视差起点（可为负，通常 0）
//   num_disparity   int  默认 64 [16,512]    视差搜索数（须为 16 的倍数，否则 -3）
//   block_size      int  默认 11 [3,51]      匹配块边长（奇数，偶数自动补奇；
//                                           P1/P2 按 8/32·block² 自动设定）
// out_disparity 容器在入口内预分配（width/height 同左图、1 通道、malloc data 配
// Image2DAlg_FreeImage——沿用 ToGray 的容器约定，核心层不新增 Free 函数）。
// 输出：视差灰度图转 BGRA（像素值 = 整数视差，8U=/16 定标；无效视差 -> 0）。
// 消息通道："min=K num=N block=B（像素值=整数视差，0=无效；左图 x<num 条带不可靠）"。
// 错误码：1 输入不足；2 匹配失败/内存不足；3 参数非法（num 非 16 倍数、尺寸不一致）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.stereo_match","name":"双目立体匹配","group":"2D 图像","category":"处理","description":"SGBM 半全局块匹配输出视差图（输入已校正的左右灰度图，尺寸须一致，视差=xL-xR>=0）：输出视差灰度图（像素值=整数视差，8U=/16 定标，无效视差为 0），配合 Image2DAlg_Calib.h 立体校正结果可转深度；num_disparity 须为 16 的倍数；左图 x<min_disparity+num_disparity 条带内视差不可靠（SGBM 特性）"},
//   "inputs": [
//     {"name":"左图","type":"image"},
//     {"name":"右图","type":"image"}
//   ],
//   "params": [
//     {"name":"min_disparity","label":"视差起点","type":"int","default":0,"min":-128,"max":128,"description":"视差起点（可为负，通常 0）"},
//     {"name":"num_disparity","label":"视差搜索数","type":"int","default":64,"min":16,"max":512,"description":"视差搜索数（须为 16 的倍数，否则报参数错）"},
//     {"name":"block_size","label":"匹配块边长","type":"int","default":11,"min":3,"max":51,"description":"匹配块边长（奇数，偶数自动补奇；P1/P2 平滑惩罚按 8/32·block² 自动设定）"}
//   ],
//   "outputs": [
//     {"name":"视差图","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiStereoMatch(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（左图/右图）"; return 1;
    }
    int min_disp = 0, num_disp = 64, block = 11;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) min_disp = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) num_disp = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) block = p->i;
    if (num_disp <= 0 || num_disp % 16 != 0) {
        out->error = "num_disparity 须为 16 的倍数且 > 0"; return 3;
    }

    Image2DAlg_Image left = CoreInputFrom(in->images[0]);
    Image2DAlg_Image right = CoreInputFrom(in->images[1]);
    Image2DAlg_Image disp = {};   // 一级指针契约：调用方预分配容器（w/h 同左图、1 通道）
    disp.width = in->images[0].width;
    disp.height = in->images[0].height;
    disp.channels = 1;
    disp.data = (unsigned char*)malloc((size_t)disp.width * (size_t)disp.height);
    if (!disp.data) { out->error = "内存不足"; return 2; }

    const int rc = Image2DAlg_StereoMatch(&left, &right, min_disp, num_disp, block, &disp);
    if (rc == -3) {
        Image2DAlg_FreeImage(&disp);
        out->error = "立体匹配失败（num 须为 16 的倍数、两图尺寸须一致、out 容器不匹配）";
        return 3;
    }
    if (rc != 0) { Image2DAlg_FreeImage(&disp); out->error = "立体匹配失败"; return 2; }

    out->images = MakeBgraOut(disp);   // 1 通道视差 -> BGRA 显示
    Image2DAlg_FreeImage(&disp);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;

    static char msg[160];
    snprintf(msg, sizeof(msg),
             "min=%d num=%d block=%d（像素值=整数视差，0=无效；左图 x<num 条带不可靠）",
             min_disp, num_disp, block);
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：2D-3D 位姿估计（node.json: alg.image2d.solve_pnp） ----------------
// solvePnP（默认迭代法 SOLVEPNP_ITERATIVE）：已知 3D 物点与 2D 投影 + 相机内参求位姿。
// 输入 2 个点集：clouds[0]=3D 物点（xyz），clouds[1]=2D 像点（xy，z 忽略）——
// 数量相等、一一对应，各 >= 4 点（3 点欠定）。
// 结构化参数沿用 manual_regist init_transform 的 text 承载先例：
//   camera_matrix  内参 3x3 行主序 9 值逗号分隔：fx,0,cx,0,fy,cy,0,0,1
//   dist_coeffs    畸变系数（可选，空 = 无畸变）：4/5/8/12/14 值逗号分隔
// 输出：无端口（outputs 为空）；rvec/tvec 与 Rodrigues 旋转矩阵并入消息通道
// （cv::Rodrigues 在入口内做——calib3d 随 world440 全含，主库同款）：
//   "rvec=(...) tvec=(...) R=[..9 值..]（世界系→相机系）"。
// 错误码：1 输入不足；2 点集为空/数量不等/点数不足（<4）/求解失败（-2 退化配置）；
//         3 参数非法（矩阵文本解析失败 / 畸变系数个数不支持）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.solve_pnp","name":"PnP 位姿估计","group":"2D 图像","category":"处理","description":"2D-3D 位姿估计（solvePnP 迭代法）：输入 3D 物点与 2D 像点两组点集（数量相等、>=4 点一一对应）；camera_matrix 内参 9 值逗号分隔（fx,0,cx,0,fy,cy,0,0,1），畸变系数可选（4/5/8/12/14 值，空=无畸变）；rvec/tvec 与 Rodrigues 旋转矩阵 9 值并入节点消息（世界系→相机系）"},
//   "inputs": [
//     {"name":"3D物点","type":"cloud"},
//     {"name":"2D像点","type":"cloud"}
//   ],
//   "params": [
//     {"name":"camera_matrix","label":"相机内参(9值)","type":"text","default":"","description":"内参 3x3 行主序 9 值逗号分隔：fx,0,cx,0,fy,cy,0,0,1"},
//     {"name":"dist_coeffs","label":"畸变系数(可选)","type":"text","default":"","description":"畸变系数（空 = 无畸变）：4/5/8/12/14 值逗号分隔（OpenCV 畸变模型）"}
//   ],
//   "outputs": [],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiSolvePnp(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 2 || !in->clouds || !in->clouds[1].xyz) {
        out->error = "需要 2 个点集输入（3D 物点/2D 像点）"; return 1;
    }
    const AlgoCCloud* obj = &in->clouds[0];
    const AlgoCCloud* img = &in->clouds[1];
    if (obj->count <= 0 || !obj->xyz) { out->error = "3D 物点为空"; return 2; }
    if (img->count <= 0 || !img->xyz) { out->error = "2D 像点为空"; return 2; }
    if (obj->count != img->count) { out->error = "两组点集数量必须相等（一一对应）"; return 2; }
    if (obj->count < 4) { out->error = "PnP 至少需要 4 个对应点（3 点欠定）"; return 2; }

    double cam[9] = { 0 };
    const AlgoCParam* pc_ = FindParam(in, 2, 0);
    int n_cam = pc_ ? ParseDoubleList(pc_->s, cam, 9) : 0;
    if (n_cam < 0) { out->error = "camera_matrix 含非数值内容（需逗号分隔数字）"; return 3; }
    if (n_cam != 9) {
        out->error = "camera_matrix 需 9 个数值（fx,0,cx,0,fy,cy,0,0,1，逗号分隔）";
        return 3;
    }

    double dist[14] = { 0 };
    int dist_n = 0;
    const AlgoCParam* pd = FindParam(in, 2, 1);
    if (pd && pd->s && pd->s[0]) {
        dist_n = ParseDoubleList(pd->s, dist, 14);
        if (dist_n < 0) { out->error = "dist_coeffs 含非数值内容（需逗号分隔数字）"; return 3; }
        if (dist_n != 4 && dist_n != 5 && dist_n != 8 && dist_n != 12 && dist_n != 14) {
            out->error = "dist_coeffs 支持 4/5/8/12/14 个值（逗号分隔；空 = 无畸变）";
            return 3;
        }
    }

    const int count = obj->count;
    double* obj3 = (double*)malloc(sizeof(double) * (size_t)count * 3);
    Image2DAlg_Point2f* pts2 =
        (Image2DAlg_Point2f*)malloc(sizeof(Image2DAlg_Point2f) * (size_t)count);
    if (!obj3 || !pts2) { free(obj3); free(pts2); out->error = "内存不足"; return 2; }
    for (int i = 0; i < count; ++i) {
        obj3[i * 3]     = obj->xyz[i * 3];
        obj3[i * 3 + 1] = obj->xyz[i * 3 + 1];
        obj3[i * 3 + 2] = obj->xyz[i * 3 + 2];
        pts2[i].x = img->xyz[i * 3];
        pts2[i].y = img->xyz[i * 3 + 1];
    }

    double rvec[3] = { 0 }, tvec[3] = { 0 };
    const int rc = Image2DAlg_SolvePnP(obj3, pts2, count, cam,
                                       dist_n > 0 ? dist : NULL, dist_n, rvec, tvec);
    free(obj3);
    free(pts2);
    if (rc == -2) { out->error = "PnP 求解失败（点数不足或退化配置）"; return 2; }
    if (rc != 0) { out->error = "PnP 求解失败"; return 2; }

    // Rodrigues：旋转向量 -> 3x3 旋转矩阵（行主序）。
    // 这是本入口唯一在适配层直接调用的 OpenCV 函数（其余都经 src/ C API 的
    // try/catch 内壳）——cv::Mat 分配/Rodrigues 本体可能抛 cv::Exception，
    // 异常穿透 extern "C" 边界进 ctypes 属未定义行为（宿主进程直接崩溃），
    // 故就地捕获并按"求解失败"口径（rc=2）落地，正常路径行为不变。
    cv::Mat R;
    try {
        const cv::Mat rv(3, 1, CV_64F, rvec);
        cv::Rodrigues(rv, R);
    } catch (const cv::Exception&) {
        out->error = "PnP 求解失败（旋转向量换算旋转矩阵异常）";
        return 2;
    }

    static char msg[400];
    snprintf(msg, sizeof(msg),
             "rvec=(%.6f, %.6f, %.6f) tvec=(%.6f, %.6f, %.6f) "
             "R=[%.6f %.6f %.6f; %.6f %.6f %.6f; %.6f %.6f %.6f]（世界系→相机系）",
             rvec[0], rvec[1], rvec[2], tvec[0], tvec[1], tvec[2],
             R.at<double>(0, 0), R.at<double>(0, 1), R.at<double>(0, 2),
             R.at<double>(1, 0), R.at<double>(1, 1), R.at<double>(1, 2),
             R.at<double>(2, 0), R.at<double>(2, 1), R.at<double>(2, 2));
    out->error = msg;
    return 0;
}

} // extern "C"（2.2.0 追加段）

// =====================================================================
// 2.3.0 缺口族接入（2026-11-19，5 个入口，追加在文件尾部）：
//   alg.image2d.warp_affine            —— WarpAffine（矩阵 text 参数，ParseDoubleList）
//   alg.image2d.warp_perspective       —— WarpPerspective（9 值 text，同上）
//   alg.image2d.find_matches_roi       —— FindAllMatchesROI（ROI 版多目标模板匹配）
//   alg.image2d.match_template_pyramid —— MatchTemplatePyramid（Ex 的金字塔提速版）
//   alg.image2d.convexity_defects      —— ConvexityDefects（轮廓凸性缺陷，点集入/点集出）
// 本段为覆盖矩阵审计选出的 A 类缺口（ABI 可行且有真实管线价值），只做参数
// 组装、错误码映射与消息拼装，不新增算法；矩阵 text 沿用 solve_pnp
// camera_matrix 的承载先例（ParseDoubleList 同款解析）。
// =====================================================================

extern "C" {

// ---------------- 算子入口：仿射变换（node.json: alg.image2d.warp_affine） ----------------
// M 为前向映射（src→dst；cv::warpAffine 默认按 M 的逆采样，M 可直接取 estimate_transform / feature_align 消息里的
// m=[...]（in1→in2 方向）或 cv::getRotationMatrix2D 等外部来源——
// 宿主无消息自动接线时由用户把 6 值抄进 text 参数。
// 参数（按声明顺序）：
//   m              text  默认 "1,0,0,0,1,0"  2x3 行主序 6 值逗号分隔（非 6 值 → 3）
//   out_width      int   默认 0  [0,10000]  输出画布宽（<=0 = 同输入宽）
//   out_height     int   默认 0  [0,10000]  输出画布高（<=0 = 同输入高）
//   interpolation  int   默认 1  [0,4]      0 近邻/1 线性/2 立方/3 面积/4 Lanczos4
// 输出：BGRA（通道保持，越出源图部分填 0）。消息通道："out=WxH interp=K"。
// 错误码：1 输入未连线；2 变换失败；3 矩阵非 6 个数值。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.warp_affine","name":"仿射变换","group":"2D 图像","category":"处理","description":"仿射变换 dst(x,y)=src(M·[x,y,1]ᵀ)：矩阵可取 estimate_transform/feature_align 消息里的 m=[...]（6 值抄入 text 参数），输出画布默认同输入"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"m","label":"仿射矩阵(6值)","type":"text","default":"1,0,0,0,1,0","description":"2x3 行主序 6 值逗号分隔（m00,m01,m02,m10,m11,m12）；错误码 3=非 6 个数值"},
//     {"name":"out_width","label":"输出宽(0同输入)","type":"int","default":0,"min":0,"max":10000,"description":"输出画布宽（<=0 = 同输入宽）"},
//     {"name":"out_height","label":"输出高(0同输入)","type":"int","default":0,"min":0,"max":10000,"description":"输出画布高（<=0 = 同输入高）"},
//     {"name":"interpolation","label":"插值方式","type":"int","default":1,"min":0,"max":4,"description":"0 近邻 / 1 线性 / 2 立方 / 3 面积 / 4 Lanczos4"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiWarpAffine(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int out_w = 0, out_h = 0, interp = 1;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) out_w = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) out_h = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) interp = p->i;

    double m[6] = { 1, 0, 0, 0, 1, 0 };
    const AlgoCParam* pm = FindParam(in, 2, 0);
    const int n = pm ? ParseDoubleList(pm->s, m, 6) : 6;   // 缺省 = 恒等矩阵
    if (n < 0) { out->error = "仿射矩阵 m 含非数值内容（需 2x3 行主序 6 个逗号分隔数字）"; return 3; }
    if (n != 6) { out->error = "仿射矩阵 m 需 6 个数值（2x3 行主序，逗号分隔）"; return 3; }

    if (out_w <= 0) out_w = in->images[0].width;    // 0 = 同输入尺寸（核心层要求 > 0）
    if (out_h <= 0) out_h = in->images[0].height;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    const int rc = Image2DAlg_WarpAffine(&input, m, out_w, out_h, interp, &result);
    if (rc == -3) { out->error = "仿射变换失败（输出画布尺寸需 > 0）"; return 3; }
    if (rc != 0) { out->error = "仿射变换失败"; return 2; }

    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;

    static char msg[96];
    snprintf(msg, sizeof(msg), "out=%dx%d interp=%d", out_w, out_h, interp);
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：透视变换（node.json: alg.image2d.warp_perspective） ----------------
// dst(x,y) = 齐次投影 src(M·[x,y,1]ᵀ)。矩阵来源：estimate_transform（motion=1）
// 消息里的 h=[...]（3x3 行主序，h[8] 归一为 1，可直接使用）或
// cv::getPerspectiveTransform 等外部来源。
// 参数（按声明顺序）：
//   m              text  默认 "1,0,0,0,1,0,0,0,1"  3x3 行主序 9 值逗号分隔（非 9 值 → 3）
//   out_width      int   默认 0  [0,10000]  输出画布宽（<=0 = 同输入宽）
//   out_height     int   默认 0  [0,10000]  输出画布高（<=0 = 同输入高）
//   interpolation  int   默认 1  [0,4]      同 warp_affine
// 输出：BGRA（通道保持，越出源图部分填 0）。消息通道："out=WxH interp=K"。
// 错误码：1 输入未连线；2 变换失败；3 矩阵非 9 个数值。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.warp_perspective","name":"透视变换","group":"2D 图像","category":"处理","description":"透视变换 dst(x,y)=齐次投影 src(M·[x,y,1]ᵀ)：矩阵可取 estimate_transform(motion=1) 消息里的 h=[...]（9 值抄入 text 参数），输出画布默认同输入"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"m","label":"透视矩阵(9值)","type":"text","default":"1,0,0,0,1,0,0,0,1","description":"3x3 行主序 9 值逗号分隔（h[8] 归一为 1）；错误码 3=非 9 个数值"},
//     {"name":"out_width","label":"输出宽(0同输入)","type":"int","default":0,"min":0,"max":10000,"description":"输出画布宽（<=0 = 同输入宽）"},
//     {"name":"out_height","label":"输出高(0同输入)","type":"int","default":0,"min":0,"max":10000,"description":"输出画布高（<=0 = 同输入高）"},
//     {"name":"interpolation","label":"插值方式","type":"int","default":1,"min":0,"max":4,"description":"0 近邻 / 1 线性 / 2 立方 / 3 面积 / 4 Lanczos4"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiWarpPerspective(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int out_w = 0, out_h = 0, interp = 1;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) out_w = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) out_h = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) interp = p->i;

    double m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const AlgoCParam* pm = FindParam(in, 2, 0);
    const int n = pm ? ParseDoubleList(pm->s, m, 9) : 9;   // 缺省 = 恒等矩阵
    if (n < 0) { out->error = "透视矩阵 m 含非数值内容（需 3x3 行主序 9 个逗号分隔数字）"; return 3; }
    if (n != 9) { out->error = "透视矩阵 m 需 9 个数值（3x3 行主序，逗号分隔）"; return 3; }

    if (out_w <= 0) out_w = in->images[0].width;
    if (out_h <= 0) out_h = in->images[0].height;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    const int rc = Image2DAlg_WarpPerspective(&input, m, out_w, out_h, interp, &result);
    if (rc == -3) { out->error = "透视变换失败（输出画布尺寸需 > 0）"; return 3; }
    if (rc != 0) { out->error = "透视变换失败"; return 2; }

    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;

    static char msg[96];
    snprintf(msg, sizeof(msg), "out=%dx%d interp=%d", out_w, out_h, interp);
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：多目标模板匹配 ROI（node.json: alg.image2d.find_matches_roi） ----------------
// find_matches 的 ROI 版：仅在 ROI 内提取多目标（NMS 在 ROI 内进行），命中左上角
// 平移回全图坐标系——限区搜索既省时又屏蔽区外干扰；模板大于 ROI → 3。
// 输入 2 张图像：images[0]=搜索图，images[1]=模板。
// 参数（按声明顺序）：
//   roi_x/roi_y      int     默认 0  [0,100000]  ROI 左上角
//   roi_w/roi_h      int     默认 0  [0,100000]  ROI 尺寸（<=0 = 全图，同 find_matches）
//   method           int     默认 5  [0,5]       cv::TemplateMatchModes
//   score_threshold  number  默认 0.8 [0,1]      SQDIFF 系 = "得分<=阈值"命中，其余 = ">="
//   max_matches      int     默认 10 [1,100]     最多输出数
//   nms_radius       int     默认 5  [0,100]     非极大值抑制半径（像素，ROI 内）
// 输出：搜索图克隆叠加各命中位置的模板外框（绿色）。
// 消息通道："count=N; [i](x,y) score=S; ..."（左上角全图坐标，按得分从优到劣）。
// 错误码：1 输入不足；2 匹配失败；3 参数非法/ROI 与图像无交集/模板大于 ROI。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.find_matches_roi","name":"多目标模板匹配ROI","group":"2D 图像","category":"处理","description":"仅在 ROI 内提取多目标（NMS 在 ROI 内进行，命中坐标平移回全图系）：限区搜索省时并屏蔽区外干扰，Mark 阵列区域全检"},
//   "inputs": [
//     {"name":"搜索图","type":"image"},
//     {"name":"模板","type":"image"}
//   ],
//   "params": [
//     {"name":"roi_x","label":"ROI X","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 X"},
//     {"name":"roi_y","label":"ROI Y","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 Y"},
//     {"name":"roi_w","label":"ROI 宽(0全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 宽（<=0 = 全图，同 find_matches）"},
//     {"name":"roi_h","label":"ROI 高(0全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 高（<=0 = 全图）"},
//     {"name":"method","label":"匹配方法","type":"int","default":5,"min":0,"max":5,"description":"cv::TemplateMatchModes（5=CCOEFF_NORMED 归一化相关）"},
//     {"name":"score_threshold","label":"得分阈值","type":"number","default":0.8,"min":0,"max":1,"description":"SQDIFF 系 = 得分<=阈值命中，其余 = >=阈值"},
//     {"name":"max_matches","label":"最多输出数","type":"int","default":10,"min":1,"max":100,"description":"最多输出目标数（按得分从优到劣）"},
//     {"name":"nms_radius","label":"NMS半径","type":"int","default":5,"min":0,"max":100,"description":"非极大值抑制半径（像素，ROI 内）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiFindMatchesRoi(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（搜索图/模板）";
        return 1;
    }
    int rx = 0, ry = 0, rw = 0, rh = 0, method = 5, max_matches = 10, nms = 5;
    double thr = 0.8;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) rx = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) ry = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) rw = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 3)) rh = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 4)) method = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) thr = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 5)) max_matches = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 6)) nms = p->i;

    Image2DAlg_Image img = CoreInputFrom(in->images[0]);
    Image2DAlg_Image tpl = CoreInputFrom(in->images[1]);
    Image2DAlg_Match* matches = NULL;
    int count = 0;
    const int rc = Image2DAlg_FindAllMatchesROI(&img, rx, ry, rw, rh, &tpl, method,
                                                thr, max_matches, nms, &matches, &count);
    if (rc == -3) { out->error = "匹配失败（参数非法、ROI 与图像无交集或模板大于 ROI）"; return 3; }
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

// ---------------- 算子入口：金字塔对位（node.json: alg.image2d.match_template_pyramid） ----------------
// MatchTemplateEx 的提速版（单目标）：顶层粗化图全范围粗搜 → 逐级回原图仅在
// 粗解 ±1 步进邻域内精化，层数 2~3 典型提速 4~10 倍；输出语义与 match_template_ex
// 一致（模板中心落点，已映射回原图坐标系）。
// 输入 2 张图像：images[0]=搜索图，images[1]=模板。
// 参数（按声明顺序）：
//   method      int     默认 5   [0,5]      建议归一化类（SQDIFF 系得分跨层不可比）
//   scale_lo    number  默认 0.8 [0.01,10]  尺度下限
//   scale_hi    number  默认 1.2 [0.01,10]  尺度上限
//   scale_step  number  默认 0.05 [0.001,10]
//   angle_lo    number  默认 -15 [-360,360] 角度下限（度，逆时针）
//   angle_hi    number  默认 15  [-360,360] 角度上限
//   angle_step  number  默认 1   [0.01,90]
//   levels      int     默认 2   [1,6]      金字塔层数（>6 自动截断；模板最小边 <12 或
//                                           图像最小边 <32 时自动截断——小模板用 ex）
// 输出：搜索图克隆叠加中心十字标注（绿色）。
// 消息通道："score=%.4f c=(%.1f,%.1f) scale=%.2f angle=%.1f levels=%d"。
// 注意：粗搜在低分辨率进行——弱纹理/低对比模板可能粗搜失准（先用 ex 验证）。
// 错误码：1 输入不足；2 搜索失败；3 参数非法（levels<1 / 步进非法 / 全部尺度越界）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.match_template_pyramid","name":"金字塔对位","group":"2D 图像","category":"处理","description":"金字塔粗搜加速的旋转+尺度模板匹配（match_template_ex 提速版，2~3 层典型提速 4~10 倍）：输出模板中心落点，语义与 ex 一致"},
//   "inputs": [
//     {"name":"搜索图","type":"image"},
//     {"name":"模板","type":"image"}
//   ],
//   "params": [
//     {"name":"method","label":"匹配方法","type":"int","default":5,"min":0,"max":5,"description":"建议归一化类（SQDIFF 系得分跨层不可比）"},
//     {"name":"scale_lo","label":"尺度下限","type":"number","default":0.8,"min":0.01,"max":10,"description":"尺度搜索范围下限"},
//     {"name":"scale_hi","label":"尺度上限","type":"number","default":1.2,"min":0.01,"max":10,"description":"尺度搜索范围上限"},
//     {"name":"scale_step","label":"尺度步进","type":"number","default":0.05,"min":0.001,"max":10,"description":"尺度步进（范围勿开过大：代价=尺度数×角度数）"},
//     {"name":"angle_lo","label":"角度下限","type":"number","default":-15,"min":-360,"max":360,"description":"角度下限（度，逆时针）"},
//     {"name":"angle_hi","label":"角度上限","type":"number","default":15,"min":-360,"max":360,"description":"角度上限（度，逆时针）"},
//     {"name":"angle_step","label":"角度步进","type":"number","default":1,"min":0.01,"max":90,"description":"角度步进（度）"},
//     {"name":"levels","label":"金字塔层数","type":"int","default":2,"min":1,"max":6,"description":">6 自动截断；模板最小边<12 或图像最小边<32 时自动截断（小模板用 ex）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiMatchTemplatePyramid(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 2 || !in->images || !in->images[1].bgra) {
        out->error = "需要 2 个图像输入（搜索图/模板）";
        return 1;
    }
    int method = 5, levels = 2;
    double scale_lo = 0.8, scale_hi = 1.2, scale_step = 0.05;
    double angle_lo = -15.0, angle_hi = 15.0, angle_step = 1.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) method = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) scale_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) scale_hi = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) scale_step = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) angle_lo = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 4)) angle_hi = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 5)) angle_step = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) levels = p->i;

    Image2DAlg_Image img = CoreInputFrom(in->images[0]);
    Image2DAlg_Image tpl = CoreInputFrom(in->images[1]);
    double score = 0, cx = 0, cy = 0, scale = 0, angle = 0;
    const int rc = Image2DAlg_MatchTemplatePyramid(&img, &tpl, method,
                                                   scale_lo, scale_hi, scale_step,
                                                   angle_lo, angle_hi, angle_step,
                                                   levels, &score, &cx, &cy, &scale, &angle);
    if (rc == -3) { out->error = "对位失败（levels>=1、步进/范围非法或任何尺度模板越界）"; return 3; }
    if (rc != 0) { out->error = "对位失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    const cv::Point c((int)std::lround(cx), (int)std::lround(cy));
    cv::line(drawn, c + cv::Point(-8, 0), c + cv::Point(8, 0), cv::Scalar(0, 255, 0, 255), 1);
    cv::line(drawn, c + cv::Point(0, -8), c + cv::Point(0, 8), cv::Scalar(0, 255, 0, 255), 1);

    static char msg[160];
    snprintf(msg, sizeof(msg), "score=%.4f c=(%.1f,%.1f) scale=%.2f angle=%.1f levels=%d",
             score, cx, cy, scale, angle, levels);

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

// ---------------- 算子入口：凸性缺陷（node.json: alg.image2d.convexity_defects） ----------------
// 凸包与轮廓间的凹陷检测（引脚变形/缺料）：输入轮廓点集（如 contour_cloud 的输出），
// 输出各缺陷最深点 z=0 点云（corner_sub_pix 同款，供下游测量/判定取用），
// 深度与顶点索引并入消息（索引以内部调整后的点序为准——输入顺时针会被先反转）。
// 输入：clouds[0]=轮廓点集（xy 取自 xyz，z 分量忽略；count >= 3）。
// 参数：无。
// 输出：缺陷最深点 z=0 点云（无凹陷时 count=0 空点云，属正常）。
// 消息通道："count=N; [i]depth=D idx=(s,e,f) pt=(fx,fy); ..."（900 字节截断）。
// 错误码：1 输入未连线；2 点集不足/内存不足；3 参数非法。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.convexity_defects","name":"凸性缺陷","group":"2D 图像","category":"处理","description":"凸包与轮廓间凹陷检测（引脚变形/缺料）：输入轮廓点集（如 contour_cloud 输出），输出各缺陷最深点 z=0 点云，深度/顶点索引并入节点消息"},
//   "inputs": [
//     {"name":"轮廓点集","type":"cloud"}
//   ],
//   "params": [],
//   "outputs": [
//     {"name":"缺陷点","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiConvexityDefects(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds || !in->clouds[0].xyz) {
        out->error = "输入未连线（轮廓点集）"; return 1;
    }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count < 3) { out->error = "轮廓点集至少需要 3 个点"; return 2; }

    float* xy = FlattenCloudXy(*src);
    if (!xy) { out->error = "内存不足"; return 2; }
    Image2DAlg_Defect* defects = NULL;
    int count = 0;
    const int rc = Image2DAlg_ConvexityDefects(xy, src->count, &defects, &count);
    free(xy);
    if (rc == -3) { out->error = "凸性缺陷计算失败（点数 >= 3）"; return 3; }
    if (rc != 0) { out->error = "凸性缺陷计算失败"; return 2; }

    // 最深点 -> z=0 点云输出（fast_features 同款：无缺陷时输出空点云）
    float* pts = (float*)malloc(sizeof(float) * (size_t)(count > 0 ? count : 1) * 2);
    if (!pts) { Image2DAlg_FreeDefects(defects); out->error = "内存不足"; return 2; }
    for (int i = 0; i < count; ++i) {
        pts[i * 2]     = (float)defects[i].fx;
        pts[i * 2 + 1] = (float)defects[i].fy;
    }
    out->clouds = AllocCloudOutZ0(pts, count);
    free(pts);
    if (!out->clouds) { Image2DAlg_FreeDefects(defects); out->error = "内存不足"; return 2; }
    out->cloudCount = 1;

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 96; ++i)
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d]depth=%.1f idx=(%d,%d,%d) pt=(%.1f,%.1f)",
                        i, defects[i].depth, defects[i].start_index,
                        defects[i].end_index, defects[i].farthest_index,
                        defects[i].fx, defects[i].fy);
    Image2DAlg_FreeDefects(defects);

    out->error = msg;
    return 0;
}

} // extern "C"（2.3.0 追加段）

// =====================================================================
// 2.4.0 扩展/稳健/ROI 族接入（2026-11-19，4 个入口，追加在文件尾部，
// 覆盖矩阵 A 类首选批——与 2.3.0 段同规则：只做参数组装、错误码映射与
// 消息拼装，不新增算法）：
//   alg.image2d.fit_circle_ex     —— FitCircleEx（高精度单圆：Kåsa + Gauss-Newton）
//   alg.image2d.fit_circle_robust —— FitCircleRobust（RANSAC 抗外点单圆 + 内点回显）
//   alg.image2d.hough_lines_proi  —— HoughLinesPROI（ROI 概率霍夫直线）
//   alg.image2d.hough_circles_roi —— HoughCirclesROI（ROI 霍夫圆）
// 圆拟合族输出形态沿用 point3d_fit_circle_2d 先例：圆心+半径打包成单点云
//（x=圆心x y=圆心y z=半径）+ 详情进消息；ROI 族沿用 find_matches_roi 先例
//（roi_x/y/w/h ≤0 = 全图，结果坐标已由核心层平移回全图系）。
// =====================================================================

extern "C" {

// 单圆结果 -> 输出单点云（x=圆心x y=圆心y z=半径，rgb=NULL；malloc 配 algo_free）。
// 返回 false = 内存不足（out 保持空）。
static bool CircleToCloudOut(const Image2DAlg_CircleFit& st, AlgoCOutput* out) {
    float* xyz = (float*)malloc(sizeof(float) * 3);
    if (!xyz) return false;
    xyz[0] = (float)st.cx;
    xyz[1] = (float)st.cy;
    xyz[2] = (float)st.r;   // z 承载半径（point3d_fit_circle_2d 同款约定）
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(xyz); return false; }
    arr->count = 1;
    arr->xyz = xyz;
    arr->rgb = NULL;
    out->clouds = arr;
    out->cloudCount = 1;
    return true;
}

// ---------------- 算子入口：单目标圆拟合高精度版（node.json: alg.image2d.fit_circle_ex） ----------------
// 输入：clouds[0]=点集（xy 取自 xyz，z 分量忽略；count >= 3）。参数：无。
// 输出：单点云（x=圆心x y=圆心y z=半径）；详情并入消息通道，形态与 fit_circle 一致：
//   "count=1; [0]c=(cx,cy) r=R rms=RMS"
//   （Kåsa 初值 + Gauss-Newton 几何精化，残差比 fit_circle 的纯代数解更小；
//     共线/近共线/点数 < 3 报错，结合 rms 判断拟合质量）。
// 错误码：1 输入未连线；2 点集不足/内存不足；3 拟合失败（点数不足/共线退化）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.fit_circle_ex","name":"单目标圆拟合Ex","group":"2D 图像","category":"处理","description":"高精度圆拟合（Kåsa 初值 + Gauss-Newton 几何精化）：圆心/半径/残差 rms 并入消息，并输出单点云（x=圆心x y=圆心y z=半径）；与 fit_circle（Kåsa 纯代数版）的分工：本节点几何精化后残差更小，适合高精度孔径/定位测量；共线/点数<3 报错"},
//   "inputs": [
//     {"name":"点集","type":"cloud"}
//   ],
//   "params": [],
//   "outputs": [
//     {"name":"圆心点","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiFitCircleEx(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds || !in->clouds[0].xyz) {
        out->error = "输入未连线（点集）"; return 1;
    }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count < 3) { out->error = "圆拟合至少需要 3 个点"; return 2; }

    float* xy = FlattenCloudXy(*src);
    if (!xy) { out->error = "内存不足"; return 2; }
    Image2DAlg_CircleFit st = {};
    const int rc = Image2DAlg_FitCircleEx(xy, src->count, &st);
    free(xy);
    if (rc == -3) { out->error = "圆拟合失败（点数不足或共线/近共线退化）"; return 3; }
    if (rc != 0) { out->error = "圆拟合失败"; return 2; }
    if (!CircleToCloudOut(st, out)) { out->error = "内存不足"; return 2; }

    static char msg[160];
    snprintf(msg, sizeof(msg), "count=1; [0]c=(%.2f,%.2f) r=%.2f rms=%.3f",
             st.cx, st.cy, st.r, st.rms);
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：单目标圆拟合抗外点版（node.json: alg.image2d.fit_circle_robust） ----------------
// 输入：clouds[0]=点集（xy 取自 xyz，z 分量忽略；count >= 3）。
// 参数（按声明顺序）：
//   threshold  number  默认 2.0  [0.01,1000]  内点判定：点到圆周距离 <= threshold（像素；<=0 → 3）
//   max_iters  int     默认 1000 [1,100000]    RANSAC 迭代上限（<1 由核心层按 1000 处理）
//   seed       int     默认 42   [0,2^31-1]    随机种子（固定值结果可复现）
// 输出：单点云（x=圆心x y=圆心y z=半径）；内点索引无跨节点意义不输出，仅计数并入消息：
//   "count=1; [0]c=(cx,cy) r=R rms=RMS inliers=K/N"
//   （RANSAC 抗毛刺/粘连外点——纯最小二乘会被外点拉偏；内点 < 3 判无有效圆报错）。
// 错误码：1 输入未连线；2 点集不足/内存不足；3 参数非法/无有效圆。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.fit_circle_robust","name":"单目标圆拟合(抗外点)","group":"2D 图像","category":"处理","description":"RANSAC 抗外点圆拟合：毛刺/粘连外点场景（纯最小二乘会被拉偏），threshold 为内点判定像素距离（点到圆周 ≤ threshold），内点数并入消息；输出单点云（x=圆心x y=圆心y z=半径）；与 fit_circle（无参最小二乘）的分工：本节点需设 threshold/迭代/种子，对含外点点集更稳"},
//   "inputs": [
//     {"name":"点集","type":"cloud"}
//   ],
//   "params": [
//     {"name":"threshold","label":"内点阈值(px)","type":"number","default":2,"min":0.01,"max":1000,"description":"内点判定：点到圆周距离 ≤ threshold（像素；≤0 → 参数非法）"},
//     {"name":"max_iters","label":"迭代上限","type":"int","default":1000,"min":1,"max":100000,"description":"RANSAC 迭代上限（<1 由核心层按 1000 处理）"},
//     {"name":"seed","label":"随机种子","type":"int","default":42,"min":0,"max":2147483647,"description":"随机种子（固定值结果可复现）"}
//   ],
//   "outputs": [
//     {"name":"圆心点","type":"cloud"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiFitCircleRobust(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds || !in->clouds[0].xyz) {
        out->error = "输入未连线（点集）"; return 1;
    }
    const AlgoCCloud* src = &in->clouds[0];
    if (src->count < 3) { out->error = "圆拟合至少需要 3 个点"; return 2; }
    double threshold = 2.0;
    int max_iters = 1000, seed = 42;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) threshold = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) max_iters = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) seed = p->i;

    float* xy = FlattenCloudXy(*src);
    if (!xy) { out->error = "内存不足"; return 2; }
    Image2DAlg_CircleFit st = {};
    int* inliers = NULL;
    int inlier_count = 0;
    const int rc = Image2DAlg_FitCircleRobust(xy, src->count, threshold, max_iters,
                                              (unsigned int)seed, &st,
                                              &inliers, &inlier_count);
    free(xy);
    if (rc == -3) { out->error = "稳健圆拟合失败（threshold 需 > 0；点数不足或内点 < 3）"; return 3; }
    if (rc != 0) { out->error = "稳健圆拟合失败"; return 2; }
    Image2DAlg_FreeInliers(inliers);
    if (!CircleToCloudOut(st, out)) { out->error = "内存不足"; return 2; }

    static char msg[192];
    snprintf(msg, sizeof(msg), "count=1; [0]c=(%.2f,%.2f) r=%.2f rms=%.3f inliers=%d/%d",
             st.cx, st.cy, st.r, st.rms, inlier_count, src->count);
    out->error = msg;
    return 0;
}

// ---------------- 算子入口：ROI 概率霍夫直线（node.json: alg.image2d.hough_lines_proi） ----------------
// hough_lines 的 ROI 版：仅在 ROI 内检测（检测参数语义与全图版逐字一致），线段端点
// 已由核心层平移回全图坐标系——限区检测省时并屏蔽区外干扰（find_matches_roi 先例）。
// 输入：二值/边缘图（链路上先接 Canny/Sobel/Threshold）。
// 参数（按声明顺序）：
//   roi_x/roi_y      int     默认 0  [0,100000]  ROI 左上角
//   roi_w/roi_h      int     默认 0  [0,100000]  ROI 尺寸（<=0 = 全图，同 find_matches_roi）
//   rho              number  默认 1   [0.1,10]   距离分辨率（像素）
//   theta_deg        number  默认 1   [0.1,10]   角度分辨率（度）
//   threshold        int     默认 50  [1,10000]  投票阈值
//   min_line_length  int     默认 20  [0,10000]  最短线长（0 不限）
//   max_line_gap     int     默认 5   [0,10000]  线最大断距（0 不限）
// 输出：输入图克隆叠加检出线段（绿色，全图坐标）。
// 消息通道："count=N; [i](x1,y1)-(x2,y2) ang=A; ..."（900 字节截断；角度为端点
//            推算的线段方向角（度，0~180）；票数 C API 不回传，不进消息）。
// 错误码：1 输入未连线；2 检测失败；3 参数非法（rho/theta <= 0 或 ROI 与图像无交集）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.hough_lines_proi","name":"霍夫直线ROI","group":"2D 图像","category":"处理","description":"概率霍夫直线检测 ROI 版：仅在 ROI 内检测（线段端点平移回全图坐标系），roi_w/roi_h ≤0 = 全图；限区检测省时并屏蔽区外干扰；与 hough_lines（全图版）的分工：输入/参数语义相同，本节点多 roi_x/y/w/h 四个 ROI 参数；消息含端点与角度（票数 C API 不回传）"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"roi_x","label":"ROI X","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 X"},
//     {"name":"roi_y","label":"ROI Y","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 Y"},
//     {"name":"roi_w","label":"ROI 宽(0全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 宽（<=0 = 全图，同 connected_components_roi）"},
//     {"name":"roi_h","label":"ROI 高(0全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 高（<=0 = 全图）"},
//     {"name":"rho","label":"距离分辨率","type":"number","default":1,"min":0.1,"max":10,"description":"距离分辨率（像素）"},
//     {"name":"theta_deg","label":"角度分辨率(度)","type":"number","default":1,"min":0.1,"max":10,"description":"角度分辨率（度）"},
//     {"name":"threshold","label":"投票阈值","type":"int","default":50,"min":1,"max":10000,"description":"投票阈值"},
//     {"name":"min_line_length","label":"最短线长(0不限)","type":"int","default":20,"min":0,"max":10000,"description":"最短线长（0 不限）"},
//     {"name":"max_line_gap","label":"最大断距(0不限)","type":"int","default":5,"min":0,"max":10000,"description":"线最大断距（0 不限）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiHoughLinesPRoi(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int rx = 0, ry = 0, rw = 0, rh = 0, threshold = 50, min_len = 20, max_gap = 5;
    double rho = 1.0, theta = 1.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) rx = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) ry = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) rw = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 3)) rh = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) rho = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) theta = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 4)) threshold = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 5)) min_len = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 6)) max_gap = p->i;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Line4* lines = NULL;
    int count = 0;
    const int rc = Image2DAlg_HoughLinesPROI(&input, rx, ry, rw, rh, rho, theta,
                                             threshold, min_len, max_gap, &lines, &count);
    if (rc == -3) { out->error = "霍夫直线失败（rho/theta 需 > 0，或 ROI 与图像无交集）"; return 3; }
    if (rc != 0) { out->error = "霍夫直线失败"; return 2; }

    cv::Mat vis(in->images[0].height, in->images[0].width, CV_8UC4,
                (void*)in->images[0].bgra);
    cv::Mat drawn = vis.clone();
    for (int i = 0; i < count; ++i)
        cv::line(drawn, cv::Point(lines[i].x1, lines[i].y1),
                 cv::Point(lines[i].x2, lines[i].y2), cv::Scalar(0, 255, 0, 255), 1);

    static char msg[900];
    int pos = snprintf(msg, sizeof(msg), "count=%d", count);
    for (int i = 0; i < count && pos < (int)sizeof(msg) - 80; ++i) {
        const double ang = std::fmod(std::atan2((double)(lines[i].y2 - lines[i].y1),
                                                (double)(lines[i].x2 - lines[i].x1))
                                     * 180.0 / 3.14159265358979323846 + 360.0, 180.0);
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "; [%d](%d,%d)-(%d,%d) ang=%.1f",
                        i, lines[i].x1, lines[i].y1, lines[i].x2, lines[i].y2, ang);
    }
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

// ---------------- 算子入口：ROI 霍夫圆（node.json: alg.image2d.hough_circles_roi） ----------------
// hough_circles 的 ROI 版：仅在 ROI 内检测（dp/param/半径语义与全图版逐字一致），
// 圆心已由核心层平移回全图坐标系；同样内部 3x3 高斯预平滑（HOUGH_GRADIENT
// 对锐利二值图可能无果），圆叠加在原输入图上。
// 输入：灰度语义图（彩色先转灰度）。
// 参数（按声明顺序）：
//   roi_x/roi_y  int     默认 0  [0,100000]  ROI 左上角
//   roi_w/roi_h  int     默认 0  [0,100000]  ROI 尺寸（<=0 = 全图）
//   dp           number  默认 1   [0.1,4]     分辨率倒数
//   min_dist     number  默认 10  [1,10000]   圆心最小间距
//   param1       number  默认 50  [1,1000]    内部 Canny 高阈值（同全图版实测口径）
//   param2       number  默认 20  [1,1000]    圆心投票阈值（越小圆越多）
//   min_r        number  默认 5   [0,10000]   最小半径
//   max_r        number  默认 0   [0,10000]   最大半径（<=0 不限）
// 输出：平滑后检出的圆叠加在原输入图上（绿色圆周，全图坐标）。
// 消息通道："count=N; [i]c=(cx,cy) r=R; ..."（900 字节截断）。
// 错误码：1 输入未连线；2 检测失败/预平滑失败；3 参数非法（dp/min_dist <= 0 或 ROI 与图像无交集）。

// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.hough_circles_roi","name":"霍夫圆ROI","group":"2D 图像","category":"处理","description":"霍夫圆检测 ROI 版（内部自动 3x3 预平滑，同 hough_circles）：仅在 ROI 内检测（圆心平移回全图坐标系），roi_w/roi_h ≤0 = 全图；限区检测省时并屏蔽区外干扰；与 hough_circles（全图版）的分工：输入/参数语义相同，本节点多 roi_x/y/w/h 四个 ROI 参数"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"roi_x","label":"ROI X","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 X"},
//     {"name":"roi_y","label":"ROI Y","type":"int","default":0,"min":0,"max":100000,"description":"ROI 左上角 Y"},
//     {"name":"roi_w","label":"ROI 宽(0全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 宽（<=0 = 全图，同 connected_components_roi）"},
//     {"name":"roi_h","label":"ROI 高(0全图)","type":"int","default":0,"min":0,"max":100000,"description":"ROI 高（<=0 = 全图）"},
//     {"name":"dp","label":"分辨率倒数","type":"number","default":1,"min":0.1,"max":4,"description":"分辨率倒数"},
//     {"name":"min_dist","label":"圆心最小间距","type":"number","default":10,"min":1,"max":10000,"description":"圆心最小间距（像素）"},
//     {"name":"param1","label":"Canny高阈值","type":"number","default":50,"min":1,"max":1000,"description":"内部 Canny 高阈值"},
//     {"name":"param2","label":"投票阈值","type":"number","default":20,"min":1,"max":1000,"description":"圆心投票阈值（越小检出圆越多）"},
//     {"name":"min_r","label":"最小半径","type":"number","default":5,"min":0,"max":10000,"description":"最小半径（像素）"},
//     {"name":"max_r","label":"最大半径(0不限)","type":"number","default":0,"min":0,"max":10000,"description":"最大半径（≤0 不限）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiHoughCirclesRoi(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int rx = 0, ry = 0, rw = 0, rh = 0;
    double dp = 1.0, min_dist = 10.0, param1 = 50.0, param2 = 20.0, min_r = 5.0, max_r = 0.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) rx = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) ry = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 2)) rw = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 3)) rh = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) dp = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) min_dist = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 2)) param1 = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 3)) param2 = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 4)) min_r = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 5)) max_r = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image blurred = {};   // HOUGH_GRADIENT 需平滑输入（同全图版口径）
    if (Image2DAlg_Blur(&input, 0, 3, 0, 0, &blurred) != 0) {
        out->error = "预平滑失败";
        return 2;
    }
    Image2DAlg_HoughCircle* circles = NULL;
    int count = 0;
    const int rc = Image2DAlg_HoughCirclesROI(&blurred, rx, ry, rw, rh, dp, min_dist,
                                              param1, param2, min_r, max_r,
                                              &circles, &count);
    Image2DAlg_FreeImage(&blurred);
    if (rc == -3) { out->error = "霍夫圆失败（dp/min_dist 需 > 0，或 ROI 与图像无交集）"; return 3; }
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

} // extern "C"（2.4.0 追加段）

// =====================================================================
// 2.3.0 滤波扩展接入（2026-11-19 起，3 个入口，追加在 2.4.0 段之后；封装
// Image2DAlg 2.3.0 滤波扩展 C API。中值/双边/高斯不设节点——Blur 节点
// method 0/1/2 已覆盖（含 4ch 双边拆 BGR 保 alpha），避免同一语义双入口
// 漂移，见 api.h 2.3.0 版本说明）：
//   alg.image2d.laplacian —— Laplacian（二阶导：边缘/纹理增强）
//   alg.image2d.filter2d  —— Filter2D（自定义 k×k 卷积核：锐化/平滑/边缘核）
//   alg.image2d.denoise_nlmeans —— DenoiseNLMeans（非局部均值降噪；Blur 未
//                                  覆盖 NLMeans 块匹配语义，单设入口）
// 与 2.4.0 段同规则：只做参数组装、错误码映射与消息拼装，不新增算法。
// =====================================================================

extern "C" {

// ---------------- 算子入口：Laplacian 边缘增强（node.json: alg.image2d.laplacian） ----------------
// 二阶导算子：对灰度突变（边缘/孤立点/细线）响应最强、平坦区响应≈0；
// 与 Sobel（一阶导幅值）互补，对噪声更敏感——强噪声输入先接 blur。
// 参数（按声明顺序）：
//   ksize  int     默认 3   [1,7]         核边长（1/3/5/7，其余核心层 -3）
//   scale  number  默认 1.0 [0.001,1000]  响应缩放系数（1 = 原始值；弱边缘
//                                         可放大、强噪声场景可衰减）
// 输出：|响应| 图转 BGRA（边缘亮、平坦黑）。深度规则（核心层口径，api.h 2.3.0）：
//   浮点输入同深度保留符号；8U/16U 输入 CV_16S 中间取 |响应|×scale 饱和回
//   原深度（负响应折叠为正，工业边缘增强常用口径）。
// 错误码：1 输入未连线；2 处理失败（核心层 -99 异常）；3 参数非法（ksize 非
//         1/3/5/7 或 scale<=0，核心层 -3）/内存不足。
// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.laplacian","name":"Laplacian 边缘增强","group":"2D 图像","category":"处理","description":"Laplacian 二阶导边缘/纹理增强：输出 |响应| 灰度图（边缘亮、平坦黑）。与 Sobel（一阶导幅值）互补——对孤立点/细线更敏感、对噪声也更敏感，强噪声输入先接 blur；8U/16U 输入 CV_16S 中间取绝对值×scale 饱和回原深度（负响应折叠为正，工业边缘增强常用口径），浮点输入同深度保符号；ksize 1/3/5/7"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"ksize","label":"核边长","type":"int","default":3,"min":1,"max":7,"description":"核边长（1/3/5/7，其余报参数错）"},
//     {"name":"scale","label":"响应缩放","type":"number","default":1.0,"min":0.001,"max":1000,"description":"响应缩放系数（1 = 原始值；弱边缘可放大，强噪声场景可衰减）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiLaplacian(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int ksize = 3;
    double scale = 1.0;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) ksize = p->i;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) scale = p->d;

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    const int rc = Image2DAlg_Laplacian(&input, ksize, scale, &result);
    if (rc == -3) { out->error = "Laplacian 失败（ksize 仅 1/3/5/7，scale 须 > 0）"; return 3; }
    if (rc != 0)  { out->error = "Laplacian 处理失败"; return 2; }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：自定义卷积核滤波（node.json: alg.image2d.filter2d） ----------------
// filter2D 任意线性滤波单一入口：锐化（Laplacian 锐化核）/平滑（高斯核）/
// 边缘核/模板核。核布局 k×k 行主序，锚点 = 核中心。
// 参数（按声明顺序）：
//   kernel  text  默认 "0,0,0,0,1,0,0,0,0"（3×3 恒等核 = 原图直通）——
//                 ksize*ksize 个逗号分隔数值（ParseDoubleList 解析，同
//                 warp_affine m 先例；常用核示例——3×3 锐化
//                 0,-1,0,-1,5,-1,0,-1,0；3×3 高斯须预归一化（÷16）
//                 0.0625,0.125,0.0625,0.125,0.25,0.125,0.0625,0.125,0.0625，
//                 未归一化核会把亮度整体放大至饱和）
//   ksize   int   默认 3  [1,51]  核边长（奇数；偶数核心层 -3）
// 输出：|响应| 图转 BGRA。深度规则（核心层口径）：浮点输入同深度保留符号；
//   8U/16U 输入 CV_16S 中间取 |响应| 饱和回原深度（核全 0 合法 = 输出全 0 图）。
// 错误码：1 输入未连线；2 处理失败（核心层 -99 异常）；3 参数非法（kernel
//         文本非数值 / 值个数 ≠ ksize*ksize / ksize 非 1~51 奇数）/内存不足。
// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.filter2d","name":"自定义卷积核滤波","group":"2D 图像","category":"处理","description":"filter2D 任意线性滤波（k×k 行主序卷积核，锚点=核中心）：锐化核 0,-1,0,-1,5,-1,0,-1,0、高斯平滑核（须预归一化 ÷16）0.0625,0.125,0.0625,0.125,0.25,0.125,0.0625,0.125,0.0625——未归一化核会把亮度整体放大至饱和；默认恒等核（原图直通）。8U/16U 输入 CV_16S 中间取绝对值饱和回原深度，浮点输入同深度保符号；ksize 1~51 奇数，kernel 值个数须 = ksize×ksize"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"kernel","label":"卷积核","type":"text","default":"0,0,0,0,1,0,0,0,0","description":"ksize×ksize 个逗号分隔数值，行主序（锚点=核中心）；3×3 即 9 值"},
//     {"name":"ksize","label":"核边长","type":"int","default":3,"min":1,"max":51,"description":"核边长（奇数 1~51，偶数报参数错）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiFilter2D(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    int ksize = 3;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) ksize = p->i;
    if (ksize < 1 || ksize > 51 || ksize % 2 == 0) {
        out->error = "ksize 须为 1~51 奇数（卷积核边长）";
        return 3;
    }
    // 先按上限容量解析（ksize 已校验 ≤51，kd/kern 上限 51*51 恒够），
    // 个数不符与坏值分别报错——ParseDoubleList 超容量统一 -1，故容量取
    // 全域上限 51*51 而非 ksize*ksize，区分"非数值"与"个数多给了"。
    double kd[51 * 51];
    const AlgoCParam* pk = FindParam(in, 2, 0);
    const int n = pk ? ParseDoubleList(pk->s, kd, 51 * 51) : 0;
    if (n < 0) { out->error = "kernel 含非数值内容或个数超上限（需逗号分隔数字）"; return 3; }
    if (n != ksize * ksize) {
        static char ebuf[128];
        snprintf(ebuf, sizeof(ebuf),
                 "kernel 需 %d 个数值（ksize=%d 的 k×k 行主序），实得 %d 个",
                 ksize * ksize, ksize, n);
        out->error = ebuf;
        return 3;
    }
    float kern[51 * 51];
    for (int i = 0; i < n; ++i) kern[i] = (float)kd[i];

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    const int rc = Image2DAlg_Filter2D(&input, kern, ksize, &result);
    if (rc == -3) { out->error = "Filter2D 失败（ksize 须 1~51 奇数）"; return 3; }
    if (rc != 0)  { out->error = "Filter2D 处理失败"; return 2; }
    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    return 0;
}

// ---------------- 算子入口：非局部均值降噪（node.json: alg.image2d.denoise_nlmeans） ----------------
// NLMeans 块匹配加权去高斯白噪声：在搜索窗内找与模板块最相似的块做加权平均，
// 孤立噪声点权重趋 0、结构块相似块权重高——保细节优于局部滤波，与 Blur 双边
// 互补的另一条降噪路径（Blur 未覆盖 NLMeans 语义，故单设节点）。
// 参数（按 node.json params 声明顺序）：
//   h                number  默认 3.0  [0.1,100]  亮度分量强度（大 = 去噪强但
//                                                     细节损失多，建议 3 起调）
//   h_color          number  默认 3.0  [0.1,100]  彩色分量强度（3/4 通道生效，
//                                                     通常 = h）
//   template_window  int     默认 7    [3,15]     模板窗边长（奇数，偶数自动补奇）
//   search_window    int     默认 21   [5,51]     搜索窗边长（奇数，须 >
//                                                     template_window，性能随其
//                                                     线性下降）
// 输出：BGRA（核心层 1/3/4 通道结果经 MakeBgraOut 统一转 BGRA；4ch 输入 alpha
//       原样保留——核心层拆 BGR 走 Colored 后回贴，同 Blur 双边先例）。
// 消息：降噪前后灰度均值/标准差对比（stddev 下降 ≈ 噪声减弱的简单质量指标）+
//       参数回显（成功时并入 error 消息通道，manual_regist 先例）。
// 错误码：1 输入未连线；2 处理失败（核心层 -99 异常）/内存不足；3 参数非法
//         （h/h_color<=0、search_window<=template_window，核心层 -3）。
// @comui-node-begin
// {
//   "node": {"id":"alg.image2d.denoise_nlmeans","name":"非局部均值降噪","group":"2D 图像","category":"处理","description":"NLMeans 非局部均值降噪：搜索窗内找相似块加权平均去高斯白噪声，孤立噪声点权重趋 0、结构块权重高——保细节优于局部滤波（1/3ch 分别走 fastNlMeansDenoising/Colored，4ch 拆 BGR 后回贴 alpha）；h 越大去噪越强、细节损失越多；search_window 越大越慢"},
//   "inputs": [
//     {"name":"输入","type":"image"}
//   ],
//   "params": [
//     {"name":"h","label":"亮度强度","type":"number","default":3.0,"min":0.1,"max":100,"description":"亮度分量滤波强度（常用 3~10；大 = 去噪强但细节损失多）"},
//     {"name":"h_color","label":"彩色强度","type":"number","default":3.0,"min":0.1,"max":100,"description":"彩色分量强度（仅 3/4 通道生效，通常 = h）"},
//     {"name":"template_window","label":"模板窗","type":"int","default":7,"min":3,"max":15,"description":"模板窗边长（奇数；偶数/过小自动补奇，下限 3）"},
//     {"name":"search_window","label":"搜索窗","type":"int","default":21,"min":5,"max":51,"description":"搜索窗边长（奇数；须 > 模板窗，偶数自动补奇；性能随其线性下降）"}
//   ],
//   "outputs": [
//     {"name":"输出","type":"image"}
//   ],
//   "source": {"files": ["integration/comui/Image2DAlg_comui.cpp","integration/comui/comui_native_abi.h","src/AlgExport.h","src/Image2DAlg/Image2DAlg_api.cpp","src/Image2DAlg/Image2DAlg_Ops.cpp","src/Image2DAlg/include/Image2DAlg_api.h"], "includes": ["src","src/Image2DAlg/include"]}
// }
// @comui-node-end
IMAGE2DALG_EXPORT int Image2DAlg_ComuiDenoiseNLMeans(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->imageCount < 1 || !in->images) { out->error = "输入未连线"; return 1; }
    double h = 3.0, h_color = 3.0;
    int template_window = 7, search_window = 21;
    if (const AlgoCParam* p = FindParam(in, 0, 0)) h = p->d;
    if (const AlgoCParam* p = FindParam(in, 0, 1)) h_color = p->d;
    if (const AlgoCParam* p = FindParam(in, 1, 0)) template_window = p->i;
    if (const AlgoCParam* p = FindParam(in, 1, 1)) search_window = p->i;
    if (h <= 0 || h_color <= 0) {
        out->error = "h / h_color 须 > 0（降噪强度）";
        return 3;
    }
    if (search_window <= template_window) {
        out->error = "search_window 须 > template_window（降噪窗口）";
        return 3;
    }

    Image2DAlg_Image input = CoreInputFrom(in->images[0]);
    Image2DAlg_Image result = {};
    const int rc = Image2DAlg_DenoiseNLMeans(&input, h, h_color, template_window,
                                             search_window, &result);
    if (rc == -3) {
        out->error = "NLMeans 失败（h/h_color 须 > 0，search_window 须 > template_window）";
        return 3;
    }
    if (rc != 0)  { out->error = "NLMeans 处理失败"; return 2; }

    // 质量消息：降噪前后灰度均值/标准差对比 + 参数回显（宿主 rc=0 时也可能回读）
    cv::Mat before(input.height, input.width, CV_8UC4, input.data);
    cv::Mat after(result.height, result.width, CV_8UC(result.channels), result.data);
    cv::Mat g0, g1;
    cv::cvtColor(before, g0, cv::COLOR_BGRA2GRAY);
    if (result.channels == 1)      g1 = after.clone();
    else if (result.channels == 3) cv::cvtColor(after, g1, cv::COLOR_BGR2GRAY);
    else                           cv::cvtColor(after, g1, cv::COLOR_BGRA2GRAY);
    cv::Scalar m0, s0, m1, s1;
    cv::meanStdDev(g0, m0, s0);
    cv::meanStdDev(g1, m1, s1);
    static char msg[256];   // 静态存储（宿主只读不释放）
    snprintf(msg, sizeof(msg),
             "denoise: mean %.1f->%.1f stddev %.1f->%.1f; h=%.1f h_color=%.1f tw=%d sw=%d",
             m0[0], m1[0], s0[0], s1[0], h, h_color, template_window, search_window);

    out->images = MakeBgraOut(result);
    Image2DAlg_FreeImage(&result);
    if (!out->images) { out->error = "内存不足"; return 2; }
    out->imageCount = 1;
    out->error = msg;
    return 0;
}

} // extern "C"（2.3.0 滤波扩展追加段）
