// Image2DAlg_test.cpp —— Image2DAlg C 接口验证（GetVersion / Init / Cleanup / FreeImage / ReadImage）。

// check 风格与 CommonAlg_test 一致，main 返回非零 = 失败。

// 临时 png 写在工作目录（ctest WORKING_DIRECTORY = build/output/Release），跑完自清理。



#include "include/Image2DAlg_api.h"



#include <opencv2/core.hpp>

#include <opencv2/imgproc.hpp>

#include <opencv2/imgcodecs.hpp>

#include <opencv2/calib3d.hpp>   // projectPoints（SolvePnP 真值投影用）



#include <algorithm>   // std::max initializer-list（NLMeans 噪声偏离度用）

#include <cmath>

#include <cstdio>

#include <cstdlib>

#include <cstring>

#include <string>

#include <vector>



static int g_fail = 0;



static void check(bool ok, const char* name)

{

    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);

    if (!ok)

        ++g_fail;

}



// 构造 4x4 BGR 渐变图（值已知、无越界回绕）：

//   B = 40x+10（0..130）  G = 40y+10（0..130）  R = 30x+30y+20（20..200）

// png 无损 → ReadImage 后 BGRA 各通道应与构造值逐像素相等，A=255。

static cv::Mat MakeGradientBgr()

{

    cv::Mat src(4, 4, CV_8UC3);

    for (int y = 0; y < 4; ++y) {

        for (int x = 0; x < 4; ++x) {

            cv::Vec3b& p = src.at<cv::Vec3b>(y, x);

            p[0] = (unsigned char)(40 * x + 10);

            p[1] = (unsigned char)(40 * y + 10);

            p[2] = (unsigned char)(30 * x + 30 * y + 20);

        }

    }

    return src;

}



// 取像素 (x,y)：BGRA 紧排，行步长 = width*4 字节

static bool PixelMatches(const unsigned char* data, int width, int x, int y,

                         unsigned char b, unsigned char g, unsigned char r,

                         unsigned char a)

{

    const unsigned char* p = data + ((size_t)y * width + x) * 4;

    return p[0] == b && p[1] == g && p[2] == r && p[3] == a;

}



// ---------------- 算子测试辅助 ----------------



// cv::Mat -> Image2DAlg_Image（拷贝到 malloc 缓冲，用 FreeImage 释放）

static Image2DAlg_Image MakeImage(const cv::Mat& src)

{

    cv::Mat m = src.isContinuous() ? src : src.clone();

    Image2DAlg_Image img;

    img.width = m.cols;

    img.height = m.rows;

    img.channels = m.channels();

    size_t size = m.total() * m.elemSize();

    img.data = (unsigned char*)std::malloc(size);

    std::memcpy(img.data, m.data, size);

    return img;

}



// 单通道像素值

static int Pix(const Image2DAlg_Image& img, int x, int y)

{

    return img.data[(size_t)y * img.width + x];

}



// 单通道非零像素个数 / 最小最大灰度

static int CountNonZero(const Image2DAlg_Image& img)

{

    int count = 0;

    size_t n = (size_t)img.width * img.height;

    for (size_t i = 0; i < n; ++i)

        if (img.data[i] != 0) ++count;

    return count;

}



static void MinMax(const Image2DAlg_Image& img, int& lo, int& hi)

{

    lo = 255;

    hi = 0;

    size_t n = (size_t)img.width * img.height;

    for (size_t i = 0; i < n; ++i) {

        int v = img.data[i];

        if (v < lo) lo = v;

        if (v > hi) hi = v;

    }

}



static int AbsInt(int v)

{

    return v < 0 ? -v : v;

}



static void testVersionAndLifecycle()

{

    std::printf("== GetVersion ==\n");

    const char* ver = Image2DAlg_GetVersion();

    check(ver != NULL && ver[0] != '\0', "GetVersion 非空");

    check(ver != NULL && std::strcmp(ver, "2.3.0") == 0, "GetVersion = 2.3.0（2.3.0 滤波扩展：Laplacian/Filter2D）");

    bool hasDigit = false;

    if (ver) {

        for (const char* c = ver; *c; ++c) {

            if (*c >= '0' && *c <= '9') { hasDigit = true; break; }

        }

    }

    check(hasDigit, "GetVersion 含数字");



    std::printf("== Init/Cleanup ==\n");

    check(Image2DAlg_Init() == 0, "Init 返回 0");

    check(Image2DAlg_Init() == 0, "重复 Init 幂等返回 0");

    Image2DAlg_Cleanup();

    check(Image2DAlg_Init() == 0, "Cleanup 后再 Init 返回 0（往返）");

}



static void testReadImage()

{

    std::printf("== ReadImage 参数容错 ==\n");

    Image2DAlg_Image img;

    std::memset(&img, 0, sizeof(img));

    check(Image2DAlg_ReadImage(NULL, &img) < 0, "path 为 NULL -> rc<0");

    check(Image2DAlg_ReadImage("whatever.png", NULL) < 0, "out 为 NULL -> rc<0");



    std::printf("== ReadImage 错误路径 ==\n");

    check(Image2DAlg_ReadImage("no_such_file_image2dalg.png", &img) < 0,

          "不存在路径 rc<0");



    std::printf("== ReadImage 正路径（4x4 BGR png -> BGRA） ==\n");

    const std::string png = "image2dalg_test_tmp.png";

    const cv::Mat src = MakeGradientBgr();

    check(cv::imwrite(png, src), "imwrite 生成临时 png");



    check(Image2DAlg_ReadImage(png.c_str(), &img) == 0, "ReadImage 返回 0");

    check(img.width == 4 && img.height == 4 && img.channels == 4,

          "宽高 4x4、通道数 4");

    check(img.data != NULL, "data 非空");

    check(PixelMatches(img.data, 4, 0, 0, 10, 10, 20, 255) &&

          PixelMatches(img.data, 4, 3, 0, 130, 10, 110, 255) &&

          PixelMatches(img.data, 4, 0, 3, 10, 130, 110, 255) &&

          PixelMatches(img.data, 4, 3, 3, 130, 130, 200, 255),

          "四角像素符合构造预期（验证行步长 16 字节紧排）");

    check(PixelMatches(img.data, 4, 2, 2, 90, 90, 140, 255) &&

          PixelMatches(img.data, 4, 1, 1, 50, 50, 80, 255),

          "中心像素符合构造预期");

    bool alphaAll = true;

    for (int i = 0; i < 16 && alphaAll; ++i)

        alphaAll = img.data[i * 4 + 3] == 255;

    check(alphaAll, "全图 alpha=255");



    Image2DAlg_FreeImage(&img);

    check(img.data == NULL, "FreeImage 后 data 置空（实现约定）");

    Image2DAlg_FreeImage(&img);   // data 已 NULL -> no-op

    Image2DAlg_FreeImage(NULL);   // NULL 入参安全

    check(img.data == NULL, "重复/NULL FreeImage 安全");



    std::printf("== ReadImage 灰度分支（2x2 灰度 png -> BGRA） ==\n");

    const cv::Mat gray(2, 2, CV_8UC1, cv::Scalar(77));

    check(cv::imwrite(png, gray), "imwrite 生成临时灰度 png");

    check(Image2DAlg_ReadImage(png.c_str(), &img) == 0, "灰度 png ReadImage 返回 0");

    check(img.width == 2 && img.height == 2 && img.channels == 4 &&

          PixelMatches(img.data, 2, 0, 0, 77, 77, 77, 255) &&

          PixelMatches(img.data, 2, 1, 1, 77, 77, 77, 255),

          "灰度转 BGRA：B=G=R=灰度值、A=255");

    Image2DAlg_FreeImage(&img);



    check(std::remove(png.c_str()) == 0, "临时 png 已删除");

}



static void testGrayAndGamma()

{

    std::printf("== ToGray ==\n");

    cv::Mat bgr(1, 2, CV_8UC3);

    bgr.at<cv::Vec3b>(0, 0) = cv::Vec3b(0, 0, 255);     // R=255 -> 76

    bgr.at<cv::Vec3b>(0, 1) = cv::Vec3b(255, 0, 0);     // B=255 -> 29

    Image2DAlg_Image in = MakeImage(bgr);

    Image2DAlg_Image out;

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_ToGray(&in, &out) == 0, "3 通道灰度化返回 0");

    check(out.channels == 1 && out.width == 2 && out.height == 1, "灰度输出 1 通道");

    check(AbsInt(Pix(out, 0, 0) - 76) <= 1, "红像素灰度约 76");

    check(AbsInt(Pix(out, 1, 0) - 29) <= 1, "蓝像素灰度约 29");

    Image2DAlg_FreeImage(&out);

    Image2DAlg_FreeImage(&in);



    cv::Mat bgra(1, 1, CV_8UC4);

    bgra.at<cv::Vec4b>(0, 0) = cv::Vec4b(0, 0, 255, 128);

    in = MakeImage(bgra);

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_ToGray(&in, &out) == 0 && out.channels == 1,

          "4 通道转 1 通道（丢弃 alpha）");

    Image2DAlg_FreeImage(&out);

    Image2DAlg_FreeImage(&in);



    std::printf("== Gamma ==\n");

    cv::Mat gray(1, 4, CV_8UC1);

    gray.at<unsigned char>(0, 0) = 0;

    gray.at<unsigned char>(0, 1) = 64;

    gray.at<unsigned char>(0, 2) = 128;

    gray.at<unsigned char>(0, 3) = 255;

    in = MakeImage(gray);

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Gamma(&in, 1.0, &out) == 0, "gamma=1 返回 0");

    check(out.channels == 1 && Pix(out, 0, 0) == 0 && Pix(out, 1, 0) == 64 &&

          Pix(out, 2, 0) == 128 && Pix(out, 3, 0) == 255, "gamma=1 恒等映射");

    Image2DAlg_FreeImage(&out);



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Gamma(&in, 2.0, &out) == 0, "gamma=2 返回 0");

    check(AbsInt(Pix(out, 1, 0) - 128) <= 1 && AbsInt(Pix(out, 2, 0) - 181) <= 1,

          "gamma=2 曲线值（64->128, 128->181）");

    Image2DAlg_FreeImage(&out);

    Image2DAlg_FreeImage(&in);



    in = MakeImage(bgr);

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Gamma(&in, 2.0, &out) == 0 && out.channels == 3,

          "3 通道 gamma 保持 3 通道");

    Image2DAlg_FreeImage(&out);

    Image2DAlg_FreeImage(&in);

}



static void testThreshold()

{

    std::printf("== Threshold ==\n");

    cv::Mat gray(1, 4, CV_8UC1);

    gray.at<unsigned char>(0, 0) = 0;

    gray.at<unsigned char>(0, 1) = 100;

    gray.at<unsigned char>(0, 2) = 200;

    gray.at<unsigned char>(0, 3) = 255;

    Image2DAlg_Image in = MakeImage(gray);

    Image2DAlg_Image out;



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Threshold(&in, 0, 128, 255, 0, 0, &out) == 0, "固定二值返回 0");

    check(out.channels == 1 && Pix(out, 0, 0) == 0 && Pix(out, 1, 0) == 0 &&

          Pix(out, 2, 0) == 255 && Pix(out, 3, 0) == 255,

          "阈值 128 -> 0/255 且输出 1 通道");

    Image2DAlg_FreeImage(&out);



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Threshold(&in, 1, 128, 255, 0, 0, &out) == 0, "反二值返回 0");

    check(Pix(out, 0, 0) == 255 && Pix(out, 1, 0) == 255 &&

          Pix(out, 2, 0) == 0 && Pix(out, 3, 0) == 0, "反二值结果反转");

    Image2DAlg_FreeImage(&out);

    Image2DAlg_FreeImage(&in);



    cv::Mat bimodal(1, 4, CV_8UC1);

    bimodal.at<unsigned char>(0, 0) = 0;

    bimodal.at<unsigned char>(0, 1) = 0;

    bimodal.at<unsigned char>(0, 2) = 200;

    bimodal.at<unsigned char>(0, 3) = 200;

    in = MakeImage(bimodal);

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Threshold(&in, 2, 0, 255, 0, 0, &out) == 0, "OTSU 返回 0");

    check(Pix(out, 0, 0) == 0 && Pix(out, 2, 0) == 255, "OTSU 双峰自动分割");

    Image2DAlg_FreeImage(&out);

    Image2DAlg_FreeImage(&in);



    cv::Mat grad(8, 8, CV_8UC1);

    for (int y = 0; y < 8; ++y)

        for (int x = 0; x < 8; ++x)

            grad.at<unsigned char>(y, x) = (unsigned char)(x * 30 + y * 10);

    in = MakeImage(grad);

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Threshold(&in, 3, 0, 255, 3, 2, &out) == 0, "自适应阈值返回 0");

    check(out.channels == 1 && out.width == 8 && CountNonZero(out) > 0,

          "自适应阈值输出 1 通道且有前景");

    Image2DAlg_FreeImage(&out);

    check(Image2DAlg_Threshold(&in, 9, 0, 255, 0, 0, &out) == -3, "非法 mode -> -3");

    Image2DAlg_FreeImage(&in);

}



static void testBinarize()

{

    std::printf("== Binarize ==\n");

    // 灰度阶梯 0/100/200/255

    cv::Mat gray(1, 4, CV_8UC1);

    gray.at<unsigned char>(0, 0) = 0;

    gray.at<unsigned char>(0, 1) = 100;

    gray.at<unsigned char>(0, 2) = 200;

    gray.at<unsigned char>(0, 3) = 255;

    Image2DAlg_Image in = MakeImage(gray);

    Image2DAlg_Image out;



    // mode 0 固定二值（与 Threshold 一致）

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Binarize(&in, 0, 128, 0, 255, 0, 0, 0, 0, &out) == 0, "Binarize 固定二值返回 0");

    check(out.channels == 1 && Pix(out, 0, 0) == 0 && Pix(out, 1, 0) == 0 &&

          Pix(out, 2, 0) == 255 && Pix(out, 3, 0) == 255, "Binarize mode0 阈值 128 -> 0/255");

    Image2DAlg_FreeImage(&out);



    // mode 5 TRUNC：> thresh 钳制为 thresh

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Binarize(&in, 5, 150, 0, 255, 0, 0, 0, 0, &out) == 0, "Binarize TRUNC 返回 0");

    check(Pix(out, 0, 0) == 0 && Pix(out, 1, 0) == 100 &&

          Pix(out, 2, 0) == 150 && Pix(out, 3, 0) == 150, "Binarize mode5 TRUNC 钳制 150");

    Image2DAlg_FreeImage(&out);



    // mode 8 双阈值·区间内前景：low=50 high=220 -> 仅 100/200 命中

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Binarize(&in, 8, 50, 220, 255, 0, 0, 0, 0, &out) == 0, "Binarize 双阈值区间内返回 0");

    check(Pix(out, 0, 0) == 0 && Pix(out, 1, 0) == 255 &&

          Pix(out, 2, 0) == 255 && Pix(out, 3, 0) == 0, "Binarize mode8 [50,220] 仅中间命中");

    Image2DAlg_FreeImage(&out);



    // mode 9 双阈值·区间外前景：low=50 high=220 -> 仅 0/255 命中

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Binarize(&in, 9, 50, 220, 255, 0, 0, 0, 0, &out) == 0, "Binarize 双阈值区间外返回 0");

    check(Pix(out, 0, 0) == 255 && Pix(out, 1, 0) == 0 &&

          Pix(out, 2, 0) == 0 && Pix(out, 3, 0) == 255, "Binarize mode9 [50,220] 仅两端命中");

    Image2DAlg_FreeImage(&out);



    // mode 10 双阈值三值化：low=50 high=220 mid=128 -> 0 / 128 / 255

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Binarize(&in, 10, 50, 220, 255, 128, 0, 0, 0, &out) == 0, "Binarize 三值化返回 0");

    check(Pix(out, 0, 0) == 0 && Pix(out, 1, 0) == 128 &&

          Pix(out, 2, 0) == 128 && Pix(out, 3, 0) == 255, "Binarize mode10 三值 0/128/255");

    Image2DAlg_FreeImage(&out);



    // invert：对 mode0 结果取反

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Binarize(&in, 0, 128, 0, 255, 0, 0, 0, 1, &out) == 0, "Binarize 反转返回 0");

    check(Pix(out, 0, 0) == 255 && Pix(out, 1, 0) == 255 &&

          Pix(out, 2, 0) == 0 && Pix(out, 3, 0) == 0, "Binarize invert 结果取反");

    Image2DAlg_FreeImage(&out);



    // 非法参数：双阈值 high < low -> -3；mode 越界 -> -3

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Binarize(&in, 8, 200, 50, 255, 0, 0, 0, 0, &out) == -3, "Binarize high<low -> -3");

    check(Image2DAlg_Binarize(&in, 11, 0, 0, 255, 0, 0, 0, 0, &out) == -3, "Binarize mode 越界 -> -3");

    Image2DAlg_FreeImage(&in);

}



static void testBlurAndMorphology()

{

    std::printf("== Blur ==\n");

    cv::Mat color(5, 5, CV_8UC3, cv::Scalar(50, 60, 70));

    Image2DAlg_Image in = MakeImage(color);

    Image2DAlg_Image out;



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Blur(&in, 0, 3, 0, 0, &out) == 0, "高斯模糊返回 0");

    check(out.channels == 3 && out.width == 5 && out.height == 5,

          "高斯保持 3 通道与尺寸");

    check(out.data[0] == 50 && out.data[1] == 60 && out.data[2] == 70,

          "常量图高斯后像素不变");

    Image2DAlg_FreeImage(&out);



    cv::Mat noise(5, 5, CV_8UC1, cv::Scalar(100));

    noise.at<unsigned char>(2, 2) = 255;

    Image2DAlg_Image noisy = MakeImage(noise);

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Blur(&noisy, 1, 3, 0, 0, &out) == 0, "中值滤波返回 0");

    check(out.channels == 1 && Pix(out, 2, 2) == 100, "中值滤除单点噪声");

    Image2DAlg_FreeImage(&out);

    Image2DAlg_FreeImage(&noisy);



    cv::Mat bgra(5, 5, CV_8UC4, cv::Scalar(50, 60, 70, 128));

    Image2DAlg_Image trans = MakeImage(bgra);

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Blur(&trans, 2, 5, 10, 10, &out) == 0, "4 通道双边返回 0");

    check(out.channels == 4 && out.data[3] == 128, "双边保持 4 通道且 alpha 不变");

    Image2DAlg_FreeImage(&out);

    check(Image2DAlg_Blur(&trans, 9, 3, 0, 0, &out) == -3, "非法 method -> -3");

    Image2DAlg_FreeImage(&trans);

    Image2DAlg_FreeImage(&in);



    std::printf("== Morphology ==\n");

    cv::Mat dot(5, 5, CV_8UC1, cv::Scalar(0));

    dot.at<unsigned char>(2, 2) = 255;

    in = MakeImage(dot);



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Morphology(&in, 1, 0, 3, 1, &out) == 0, "膨胀返回 0");

    check(CountNonZero(out) == 9 && Pix(out, 1, 1) == 255,

          "3x3 矩形膨胀单点 -> 9 像素");

    Image2DAlg_FreeImage(&out);



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Morphology(&in, 0, 0, 3, 1, &out) == 0, "腐蚀返回 0");

    check(CountNonZero(out) == 0, "3x3 腐蚀单点 -> 全零");

    Image2DAlg_FreeImage(&out);



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Morphology(&in, 2, 2, 3, 1, &out) == 0, "开运算返回 0");

    check(CountNonZero(out) == 0, "开运算去掉孤立点");

    Image2DAlg_FreeImage(&out);



    check(Image2DAlg_Morphology(&in, 9, 0, 3, 1, &out) == -3, "非法 op -> -3");

    Image2DAlg_FreeImage(&in);

}



static void testCanny()

{

    std::printf("== Canny ==\n");

    cv::Mat img(32, 32, CV_8UC1, cv::Scalar(0));

    for (int y = 0; y < 32; ++y)

        for (int x = 16; x < 32; ++x)

            img.at<unsigned char>(y, x) = 255;



    Image2DAlg_Image in = MakeImage(img);

    Image2DAlg_Image out;

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Canny(&in, 50, 150, &out) == 0, "Canny 返回 0");

    check(out.channels == 1 && out.width == 32 && out.height == 32,

          "Canny 输出 1 通道同尺寸");

    check(CountNonZero(out) > 0, "检测到竖直边缘");

    check(Pix(out, 2, 2) == 0 && Pix(out, 28, 2) == 0, "平坦区域无边缘");

    Image2DAlg_FreeImage(&out);

    Image2DAlg_FreeImage(&in);

}



static void testSobel()

{

    std::printf("== Sobel ==\n");

    cv::Mat img(32, 32, CV_8UC1, cv::Scalar(0));

    for (int y = 0; y < 32; ++y)

        for (int x = 16; x < 32; ++x)

            img.at<unsigned char>(y, x) = 255;



    Image2DAlg_Image in = MakeImage(img);

    Image2DAlg_Image out;

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Sobel(&in, 1, 0, 3, &out) == 0, "Sobel dx=1 返回 0");

    check(out.channels == 1 && out.width == 32 && out.height == 32,

          "Sobel 输出 1 通道同尺寸");

    check(CountNonZero(out) > 0, "检测到竖直阶跃梯度");

    check(Pix(out, 2, 2) == 0 && Pix(out, 28, 2) == 0, "平坦区域梯度为 0");

    Image2DAlg_FreeImage(&out);



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Sobel(&in, 0, 1, 3, &out) == 0 && CountNonZero(out) == 0,

          "水平阶跃图 dy 求导梯度为 0");

    Image2DAlg_FreeImage(&out);



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Sobel(&in, 0, 0, 3, &out) == -99, "dx=dy=0 -> -99");

    Image2DAlg_FreeImage(&in);

}



static void testGeometry()

{

    std::printf("== Resize/Crop/Rotate ==\n");

    cv::Mat g(4, 4, CV_8UC1);

    for (int y = 0; y < 4; ++y)

        for (int x = 0; x < 4; ++x)

            g.at<unsigned char>(y, x) = (unsigned char)(x + y);



    Image2DAlg_Image in = MakeImage(g);

    Image2DAlg_Image out;



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Resize(&in, 8, 8, 0, &out) == 0, "缩放返回 0");

    check(out.width == 8 && out.height == 8 && out.channels == 1,

          "缩放到 8x8 单通道");

    check(Pix(out, 0, 0) == 0 && Pix(out, 7, 7) == 6, "最近邻采样值正确");

    Image2DAlg_FreeImage(&out);



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Crop(&in, 1, 1, 2, 2, &out) == 0, "裁剪返回 0");

    check(out.width == 2 && out.height == 2 && Pix(out, 0, 0) == 2 && Pix(out, 1, 1) == 4,

          "裁剪像素正确");

    Image2DAlg_FreeImage(&out);



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Crop(&in, -2, -2, 4, 4, &out) == 0 && out.width == 2 &&

          out.height == 2, "越界裁剪钳制到图内");

    Image2DAlg_FreeImage(&out);

    check(Image2DAlg_Crop(&in, 10, 10, 2, 2, &out) == -3, "完全越界裁剪 -> -3");



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Rotate(&in, 0, 1, 0, &out) == 0, "旋转 0 度返回 0");

    check(out.width == 4 && out.height == 4 && out.channels == 1 && Pix(out, 1, 1) == 2,

          "旋转 0 度保持原值");

    Image2DAlg_FreeImage(&out);



    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Rotate(&in, 90, 1, 1, &out) == 0 && out.width == 4 &&

          out.height == 4 && out.channels == 1, "旋转 90 度保持尺寸与通道");

    Image2DAlg_FreeImage(&out);

    Image2DAlg_FreeImage(&in);

}



static void testEnhance()

{

    std::printf("== Enhance ==\n");

    cv::Mat low(16, 16, CV_8UC1);

    for (int y = 0; y < 16; ++y)

        for (int x = 0; x < 16; ++x)

            low.at<unsigned char>(y, x) = (unsigned char)(100 + (x + y) % 11);



    Image2DAlg_Image in = MakeImage(low);

    Image2DAlg_Image out;

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Enhance(&in, 0, 0, 0, &out) == 0, "直方图均衡返回 0");

    int lo = 0, hi = 0;

    MinMax(out, lo, hi);

    check(out.channels == 1 && lo == 0 && hi == 255, "均衡后动态范围扩大到 0~255");

    Image2DAlg_FreeImage(&out);

    Image2DAlg_FreeImage(&in);



    cv::Mat low3(16, 16, CV_8UC3);

    for (int y = 0; y < 16; ++y)

        for (int x = 0; x < 16; ++x) {

            unsigned char v = (unsigned char)(100 + (x + y) % 11);

            low3.at<cv::Vec3b>(y, x) = cv::Vec3b(v, v, v);

        }

    in = MakeImage(low3);

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_Enhance(&in, 1, 2.0, 8, &out) == 0 && out.channels == 3,

          "CLAHE 3 通道返回 3 通道");

    Image2DAlg_FreeImage(&out);

    Image2DAlg_FreeImage(&in);

}



static void testConnectedComponents()

{

    std::printf("== ConnectedComponents ==\n");

    cv::Mat bin(32, 32, CV_8UC1, cv::Scalar(0));

    for (int y = 2; y < 12; ++y)

        for (int x = 2; x < 12; ++x)

            bin.at<unsigned char>(y, x) = 255;

    for (int y = 20; y < 30; ++y)

        for (int x = 20; x < 30; ++x)

            bin.at<unsigned char>(y, x) = 255;



    Image2DAlg_Image in = MakeImage(bin);

    Image2DAlg_Blob* blobs = NULL;

    int count = 0;

    check(Image2DAlg_ConnectedComponents(&in, 8, 0, 0, &blobs, &count) == 0,

          "连通域返回 0");

    check(count == 2, "两个分离方块 -> 2 个连通域");

    check(blobs != NULL && blobs[0].area == 100 && blobs[1].area == 100,

          "两个连通域面积各 100");

    check(blobs != NULL && blobs[0].left == 2 && blobs[0].top == 2 &&

          blobs[0].width == 10 && blobs[0].height == 10, "包围盒正确");

    check(blobs != NULL && std::fabs(blobs[0].cx - 6.5) < 1e-6 &&

          std::fabs(blobs[0].cy - 6.5) < 1e-6, "质心约 (6.5,6.5)");

    Image2DAlg_FreeBlobs(blobs);



    blobs = NULL;

    count = 0;

    check(Image2DAlg_ConnectedComponents(&in, 8, 150, 0, &blobs, &count) == 0 &&

          count == 0 && blobs == NULL, "min_area 过滤全部连通域");

    Image2DAlg_FreeImage(&in);



    cv::Mat diag(8, 8, CV_8UC1, cv::Scalar(0));

    diag.at<unsigned char>(1, 1) = 255;

    diag.at<unsigned char>(2, 2) = 255;

    Image2DAlg_Image d = MakeImage(diag);

    blobs = NULL;

    count = 0;

    check(Image2DAlg_ConnectedComponents(&d, 8, 0, 0, &blobs, &count) == 0 &&

          count == 1 && blobs[0].area == 2, "8 连通对角像素合并");

    Image2DAlg_FreeBlobs(blobs);



    blobs = NULL;

    count = 0;

    check(Image2DAlg_ConnectedComponents(&d, 4, 0, 0, &blobs, &count) == 0 &&

          count == 2, "4 连通对角像素分离");

    Image2DAlg_FreeBlobs(blobs);

    check(Image2DAlg_ConnectedComponents(&d, 6, 0, 0, &blobs, &count) == -3,

          "非法 connectivity -> -3");

    Image2DAlg_FreeImage(&d);

}



static void testContours()

{

    std::printf("== AnalyzeContours ==\n");

    cv::Mat bin(32, 32, CV_8UC1, cv::Scalar(0));

    for (int y = 5; y < 15; ++y)

        for (int x = 5; x < 15; ++x)

            bin.at<unsigned char>(y, x) = 255;



    Image2DAlg_Image in = MakeImage(bin);

    Image2DAlg_ContourInfo* info = NULL;

    int count = 0;

    check(Image2DAlg_AnalyzeContours(&in, 1, 0, &info, &count) == 0 && count == 1,

          "单方块 -> 1 条外轮廓");

    check(info != NULL && std::fabs(info[0].area - 81.0) <= 2.0, "轮廓面积约 81");

    check(info != NULL && std::fabs(info[0].perimeter - 36.0) <= 1.0, "轮廓周长约 36");

    check(info != NULL && info[0].bbox_x == 5 && info[0].bbox_y == 5 &&

          info[0].bbox_w == 10 && info[0].bbox_h == 10, "包围盒 (5,5,10,10)");

    check(info != NULL && std::fabs(info[0].circularity - 0.785) < 0.05,

          "圆度约 0.785（方形）");

    check(info != NULL && std::fabs(info[0].center_x - 9.5) < 0.5 &&

          std::fabs(info[0].center_y - 9.5) < 0.5, "质心约 (9.5,9.5)");

    check(info != NULL && std::fabs(info[0].min_rect_w - 9.0) <= 1.0 &&

          std::fabs(info[0].min_rect_h - 9.0) <= 1.0, "最小外接矩形约 9x9");

    check(info != NULL && std::fabs(info[0].circle_r - 6.36) <= 1.0,

          "最小外接圆半径约 6.36");

    Image2DAlg_FreeContours(info);



    info = NULL;

    count = 0;

    check(Image2DAlg_AnalyzeContours(&in, 1, 100, &info, &count) == 0 &&

          count == 0 && info == NULL, "min_area 过滤小轮廓");

    Image2DAlg_FreeImage(&in);

}



static void testMatchTemplate()

{

    std::printf("== MatchTemplate ==\n");

    cv::Mat img(20, 20, CV_8UC1, cv::Scalar(10));

    cv::Mat tpl(4, 4, CV_8UC1);

    for (int r = 0; r < 4; ++r)

        for (int c = 0; c < 4; ++c) {

            unsigned char v = (unsigned char)(50 + 10 * r + c);

            tpl.at<unsigned char>(r, c) = v;

            img.at<unsigned char>(7 + r, 5 + c) = v;

        }



    Image2DAlg_Image in = MakeImage(img);

    Image2DAlg_Image t = MakeImage(tpl);

    double score = 0;

    int x = -1, y = -1;

    check(Image2DAlg_MatchTemplate(&in, &t, 0, &score, &x, &y) == 0,

          "模板匹配返回 0");

    check(x == 5 && y == 7, "最佳匹配位置 (5,7)");

    check(std::fabs(score) < 1e-6, "SQDIFF 得分约 0");

    Image2DAlg_FreeImage(&t);



    cv::Mat big(21, 21, CV_8UC1, cv::Scalar(0));

    Image2DAlg_Image bigger = MakeImage(big);

    check(Image2DAlg_MatchTemplate(&in, &bigger, 0, &score, &x, &y) == -3,

          "模板大于图像 -> -3");

    Image2DAlg_FreeImage(&bigger);



    cv::Mat tpl3(4, 4, CV_8UC3, cv::Scalar(0, 0, 0));

    Image2DAlg_Image t3 = MakeImage(tpl3);

    check(Image2DAlg_MatchTemplate(&in, &t3, 0, &score, &x, &y) == -3,

          "通道数不一致 -> -3");

    Image2DAlg_FreeImage(&t3);

    Image2DAlg_FreeImage(&in);

}



static void testErrorPaths()

{

    std::printf("== 错误路径 ==\n");

    Image2DAlg_Image out;

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_ToGray(NULL, &out) == -1, "in=NULL -> -1");



    cv::Mat g(2, 2, CV_8UC1, cv::Scalar(1));

    Image2DAlg_Image in = MakeImage(g);

    check(Image2DAlg_ToGray(&in, NULL) == -1, "out=NULL -> -1");



    cv::Mat two(2, 2, CV_8UC2, cv::Scalar(1, 2));

    Image2DAlg_Image in2 = MakeImage(two);

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_ToGray(&in2, &out) == -3, "2 通道 -> -3");



    Image2DAlg_FreeImage(&in2);

    Image2DAlg_FreeImage(&in);

    Image2DAlg_FreeBlobs(NULL);

    Image2DAlg_FreeContours(NULL);

    check(true, "FreeBlobs/FreeContours NULL 安全");

}



static void testFitAndColor()

{

    std::printf("== FitCircle/FitLine ==\n");

    // 12 个点精确落在 r=10 的圆上

    float xy[24];

    for (int i = 0; i < 12; ++i) {

        const double a = 2.0 * 3.14159265358979323846 * i / 12.0;

        xy[i * 2] = (float)(10.0 * std::cos(a) + 100.0);

        xy[i * 2 + 1] = (float)(10.0 * std::sin(a) + 50.0);

    }

    Image2DAlg_CircleFit circle;

    check(Image2DAlg_FitCircle(xy, 12, &circle) == 0, "圆拟合返回 0");

    check(std::fabs(circle.cx - 100.0) < 1e-3 && std::fabs(circle.cy - 50.0) < 1e-3 &&

          std::fabs(circle.r - 10.0) < 1e-3, "圆心 (100,50) 半径 10");

    check(circle.rms < 1e-6, "精确圆残差约 0");



    check(Image2DAlg_FitCircle(xy, 2, &circle) == -3, "点数不足 -> -3");

    check(Image2DAlg_FitCircle(NULL, 12, &circle) == -1, "xy=NULL -> -1");



    for (int i = 0; i < 10; ++i) {

        xy[i * 2] = (float)(i * 2);

        xy[i * 2 + 1] = 5.0f;

    }

    Image2DAlg_LineFit line;

    check(Image2DAlg_FitLine(xy, 10, &line) == 0, "线拟合返回 0");

    check(std::fabs(std::fabs(line.vy) - 0.0) < 1e-3 && std::fabs(line.y0 - 5.0) < 0.2,

          "水平线方向与位置正确");

    check(line.rms < 1e-6, "精确线残差约 0");

    check(Image2DAlg_FitLine(xy, 1, &line) == -3, "单点 -> -3");



    std::printf("== HSVThreshold/AbsDiff ==\n");

    cv::Mat color(1, 2, CV_8UC3);

    color.at<cv::Vec3b>(0, 0) = cv::Vec3b(0, 0, 255);    // 红：H=0

    color.at<cv::Vec3b>(0, 1) = cv::Vec3b(255, 0, 0);    // 蓝：H=120

    Image2DAlg_Image in = MakeImage(color);

    Image2DAlg_Image out;

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_HSVThreshold(&in, 0, 10, 100, 255, 100, 255, &out) == 0,

          "HSV 阈值返回 0");

    check(out.channels == 1 && Pix(out, 0, 0) == 255 && Pix(out, 1, 0) == 0,

          "红色入掩膜、蓝色排除");

    Image2DAlg_FreeImage(&out);

    check(Image2DAlg_HSVThreshold(&in, 200, 210, 0, 255, 0, 255, &out) == 0 &&

          CountNonZero(out) == 0, "范围外全排除");

    Image2DAlg_FreeImage(&out);



    cv::Mat gray(2, 2, CV_8UC1, cv::Scalar(10));

    Image2DAlg_Image g = MakeImage(gray);

    check(Image2DAlg_HSVThreshold(&g, 0, 180, 0, 255, 0, 255, &out) == -3,

          "灰度图无 HSV 语义 -> -3");

    Image2DAlg_FreeImage(&g);

    Image2DAlg_FreeImage(&in);



    cv::Mat a(2, 2, CV_8UC1, cv::Scalar(200));

    cv::Mat b(2, 2, CV_8UC1, cv::Scalar(100));

    Image2DAlg_Image ia = MakeImage(a);

    Image2DAlg_Image ib = MakeImage(b);

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_AbsDiff(&ia, &ib, &out) == 0 && out.channels == 1 &&

          Pix(out, 0, 0) == 100, "差分值 100");

    Image2DAlg_FreeImage(&out);



    cv::Mat big(3, 3, CV_8UC1, cv::Scalar(0));

    Image2DAlg_Image ialias = MakeImage(a);

    Image2DAlg_Image ibig = MakeImage(big);

    check(Image2DAlg_AbsDiff(&ia, &ibig, &out) == -3, "尺寸不一致 -> -3");

    Image2DAlg_FreeImage(&ibig);

    Image2DAlg_FreeImage(&ialias);

    Image2DAlg_FreeImage(&ib);

    Image2DAlg_FreeImage(&ia);

}



static void testShapesDefectsMulti()

{

    std::printf("== MatchShapes ==\n");

    // 方形（4 角点）与圆（24 点）

    const float square[8] = { 0.f, 0.f, 10.f, 0.f, 10.f, 10.f, 0.f, 10.f };

    float circle[48];

    for (int i = 0; i < 24; ++i) {

        const double a = 2.0 * 3.14159265358979323846 * i / 24.0;

        circle[i * 2] = (float)(10.0 * std::cos(a) + 10.0);

        circle[i * 2 + 1] = (float)(10.0 * std::sin(a) + 10.0);

    }

    double score = -1;

    check(Image2DAlg_MatchShapes(square, 4, square, 4, 1, &score) == 0 &&

          std::fabs(score) < 1e-6, "同形 Hu 距离约 0");

    check(Image2DAlg_MatchShapes(square, 4, circle, 24, 1, &score) == 0 && score > 0.01,

          "方形 vs 圆距离显著");

    check(Image2DAlg_MatchShapes(square, 4, circle, 24, 9, &score) == -3,

          "非法 method -> -3");

    check(Image2DAlg_MatchShapes(square, 2, circle, 24, 1, &score) == -3,

          "点数不足 -> -3");



    std::printf("== ConvexityDefects ==\n");

    // ∪ 形轮廓（开口朝上，腔深 15）

    const float u[] = { 0.f, 0.f, 0.f, 20.f, 20.f, 20.f, 20.f, 0.f,

                        15.f, 0.f, 15.f, 15.f, 5.f, 15.f, 5.f, 0.f };

    Image2DAlg_Defect* defects = NULL;

    int count = 0;

    check(Image2DAlg_ConvexityDefects(u, 8, &defects, &count) == 0, "缺陷提取返回 0");

    check(count == 1 && defects != NULL, "∪ 形恰有 1 处凸性缺陷");

    check(defects != NULL && std::fabs(defects[0].depth - 15.0) < 0.1, "腔深约 15");

    check(defects != NULL && defects[0].fy == 15.0 && (defects[0].fx == 5.0 ||

          defects[0].fx == 15.0), "最深点在腔底 (5,15)/(15,15)");

    Image2DAlg_FreeDefects(defects);



    const float rect[8] = { 0.f, 0.f, 20.f, 0.f, 20.f, 20.f, 0.f, 20.f };

    defects = NULL;

    count = 0;

    check(Image2DAlg_ConvexityDefects(rect, 4, &defects, &count) == 0 &&

          count == 0 && defects == NULL, "凸四边形无缺陷");

    check(Image2DAlg_ConvexityDefects(rect, 2, &defects, &count) == -3, "点数不足 -> -3");

    Image2DAlg_FreeDefects(NULL);

    check(true, "FreeDefects NULL 安全");

    // L 形顺/逆时针两种遍历序（review 回归：旧实现顺时针先反转点序，
    // 返回索引落在反转后的数组上，与 api.h "输入点序" 契约相悖）
    const float lccw[] = { 0.f, 0.f, 0.f, 4.f, 2.f, 4.f, 2.f, 2.f, 4.f, 2.f, 4.f, 0.f };  // OpenCV 有向面积为负
    const float lcw[]  = { 4.f, 0.f, 4.f, 2.f, 2.f, 2.f, 2.f, 4.f, 0.f, 4.f, 0.f, 0.f };  // 反向遍历

    for (int dir = 0; dir < 2; ++dir) {
        const float* lp = (dir == 0) ? lccw : lcw;
        defects = NULL;
        count = 0;
        check(Image2DAlg_ConvexityDefects(lp, 6, &defects, &count) == 0 && count == 1,
              dir == 0 ? "L 形（顺时针序）恰有 1 处缺陷" : "L 形（逆时针序）恰有 1 处缺陷");
        check(defects != NULL &&
              defects[0].farthest_index >= 0 && defects[0].farthest_index < 6 &&
              defects[0].start_index >= 0 && defects[0].start_index < 6 &&
              defects[0].end_index >= 0 && defects[0].end_index < 6,
              "缺陷索引均在 [0,6)（输入点序契约）");
        check(defects != NULL &&
              lp[defects[0].farthest_index * 2] == defects[0].fx &&
              lp[defects[0].farthest_index * 2 + 1] == defects[0].fy,
              "farthest_index 按输入点序回切 == (fx,fy)");
        check(defects != NULL && std::fabs(defects[0].depth - 1.414) < 0.01, "L 形腔深 ≈ √2");
        Image2DAlg_FreeDefects(defects);
    }

    // 退化输入：全部共线（凸包 <3 点）→ 无凹陷口径 0/NULL/0（不再透出 -99）
    const float collinear[] = { 0.f, 0.f, 1.f, 1.f, 2.f, 2.f, 3.f, 3.f };
    defects = NULL;
    count = 0;
    check(Image2DAlg_ConvexityDefects(collinear, 4, &defects, &count) == 0 &&
          count == 0 && defects == NULL, "共线退化输入 -> 0/NULL/0");

    // ---- 2.3.0 滤波扩展：Laplacian / Filter2D ----
    {
        // 8U 阶梯图：左黑右白（边界在 x=10）
        cv::Mat step(10, 20, CV_8UC1, cv::Scalar(0));
        for (int y = 0; y < step.rows; ++y)
            for (int x = 10; x < step.cols; ++x)
                step.at<unsigned char>(y, x) = 255;
        Image2DAlg_Image in = MakeImage(step);
        Image2DAlg_Image out;
        std::memset(&out, 0, sizeof(out));

        check(Image2DAlg_Laplacian(&in, 3, 1.0, &out) == 0, "Laplacian 8U 返回 0");
        check(out.width == 20 && out.height == 10 && out.channels == 1,
              "Laplacian 尺寸/通道与输入一致");
        int flatZero = 1, maxResp = 0;
        for (int y = 0; y < 10; ++y) {
            if (out.data[y * 20 + 5] != 0) flatZero = 0;
            for (int x = 8; x <= 12; ++x)
                if (out.data[y * 20 + x] > maxResp) maxResp = out.data[y * 20 + x];
        }
        check(flatZero, "Laplacian 平坦区响应 0");
        check(maxResp >= 200, "Laplacian 边界强响应（8U 走 CV_16S 中间取绝对值）");
        check(Image2DAlg_Laplacian(&in, 2, 1.0, &out) == -3, "Laplacian 偶数核 -> -3");
        check(Image2DAlg_Laplacian(&in, 3, 0.0, &out) == -3, "Laplacian scale<=0 -> -3");
        Image2DAlg_FreeImage(&out);

        float ident[9] = { 0, 0, 0, 0, 1, 0, 0, 0, 0 };
        std::memset(&out, 0, sizeof(out));
        check(Image2DAlg_Filter2D(&in, ident, 3, &out) == 0, "Filter2D 恒等核返回 0");
        int same = 1;
        for (int i = 0; i < 20 * 10; ++i)
            if (out.data[i] != in.data[i]) { same = 0; break; }
        check(same, "Filter2D 恒等核输出逐像素 == 输入");

        float kern[9] = { 0, -1, 0, -1, 5, -1, 0, -1, 0 };   // 5x 锐化核
        std::memset(&out, 0, sizeof(out));
        check(Image2DAlg_Filter2D(&in, kern, 3, &out) == 0, "Filter2D 锐化核返回 0");
        check(out.data[5 * 20 + 5] == 0, "锐化核平坦黑区保持 0");
        int edgeMax = 0;
        for (int y = 0; y < 10; ++y)
            for (int x = 9; x <= 11; ++x)
                if (out.data[y * 20 + x] > edgeMax) edgeMax = out.data[y * 20 + x];
        check(edgeMax >= 200, "锐化核边界增强（CV_16S 中间取 |.|）");
        check(Image2DAlg_Filter2D(&in, NULL, 3, &out) == -1, "Filter2D kernel=NULL -> -1");
        check(Image2DAlg_Filter2D(&in, ident, 2, &out) == -3, "Filter2D 偶数核 -> -3");
        Image2DAlg_FreeImage(&out);
        Image2DAlg_FreeImage(&in);
    }

    // ---- 2.3.0 滤波扩展：DenoiseNLMeans（非局部均值降噪） ----
    {
        // 灰度：常量底色 200 + 200 个 LCG 撒出的单点盐椒噪声——孤立点的块
        // 自权重恒为 1（块与自身距离 0），小 h 下其余相似块权重趋 0 仍会保留
        // 孤立点，需大 h 强降噪（h=50）让平坦块权重参与平均，把单点拉回底色
        cv::Mat noisy(64, 64, CV_8UC1, cv::Scalar(200));
        unsigned seed = 12345u;
        for (int i = 0; i < 200; ++i) {
            seed = seed * 1103515245u + 12345u;
            const int x = (int)((seed >> 16) % 64);
            seed = seed * 1103515245u + 12345u;
            const int y = (int)((seed >> 16) % 64);
            noisy.at<unsigned char>(y, x) = (unsigned char)((i & 1) ? 0 : 255);
        }
        Image2DAlg_Image in = MakeImage(noisy);
        Image2DAlg_Image out;
        std::memset(&out, 0, sizeof(out));

        check(Image2DAlg_DenoiseNLMeans(&in, 50.0, 50.0, 7, 21, &out) == 0,
              "NLMeans 8U 返回 0");
        check(out.width == 64 && out.height == 64 && out.channels == 1,
              "NLMeans 尺寸/通道与输入一致");
        int cntBefore = 0, cntAfter = 0;
        long long absBefore = 0, absAfter = 0;
        for (int i = 0; i < 64 * 64; ++i) {
            const int d0 = std::abs((int)in.data[i] - 200);
            const int d1 = std::abs((int)out.data[i] - 200);
            if (d0 > 20) ++cntBefore;
            if (d1 > 20) ++cntAfter;
            absBefore += d0;
            absAfter += d1;
        }
        check(cntAfter * 5 <= cntBefore,
              "NLMeans 噪声点显著减弱（偏离底色>20 的像素 5 倍以上收敛）");
        check(absAfter < absBefore, "NLMeans 输出整体更接近常量底色（平均偏差收窄）");
        Image2DAlg_FreeImage(&out);

        // 偶数窗口自动补奇：template 6 -> 7、search 20 -> 21（补奇后仍 > template）
        std::memset(&out, 0, sizeof(out));
        check(Image2DAlg_DenoiseNLMeans(&in, 10.0, 10.0, 6, 20, &out) == 0,
              "NLMeans 偶数窗口自动补奇返回 0");
        check(out.channels == 1, "NLMeans 补奇调用输出通道不变");
        Image2DAlg_FreeImage(&out);

        // 3ch 彩色同验：常量底色 (180,120,90) + 单点噪声——BGR 各通道均更接近底色
        cv::Mat noisy3(64, 64, CV_8UC3, cv::Scalar(90, 120, 180));
        seed = 54321u;
        for (int i = 0; i < 200; ++i) {
            seed = seed * 1103515245u + 12345u;
            const int x = (int)((seed >> 16) % 64);
            seed = seed * 1103515245u + 12345u;
            const int y = (int)((seed >> 16) % 64);
            noisy3.at<cv::Vec3b>(y, x) = (i & 1) ? cv::Vec3b(0, 0, 0)
                                                 : cv::Vec3b(255, 255, 255);
        }
        Image2DAlg_Image in3 = MakeImage(noisy3);
        std::memset(&out, 0, sizeof(out));
        check(Image2DAlg_DenoiseNLMeans(&in3, 50.0, 50.0, 7, 21, &out) == 0,
              "NLMeans 3ch 返回 0（走 Colored）");
        check(out.width == 64 && out.height == 64 && out.channels == 3,
              "NLMeans 3ch 尺寸/通道与输入一致");
        int cnt3Before = 0, cnt3After = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const cv::Vec3b& p0 = noisy3.at<cv::Vec3b>(y, x);
                const unsigned char* p1 = out.data + ((size_t)y * 64 + x) * 3;
                const int dev0 = std::max({ std::abs((int)p0[0] - 90),
                                            std::abs((int)p0[1] - 120),
                                            std::abs((int)p0[2] - 180) });
                const int dev1 = std::max({ std::abs((int)p1[0] - 90),
                                            std::abs((int)p1[1] - 120),
                                            std::abs((int)p1[2] - 180) });
                if (dev0 > 20) ++cnt3Before;
                if (dev1 > 20) ++cnt3After;
            }
        }
        check(cnt3After * 5 <= cnt3Before,
              "NLMeans 3ch 噪声点显著减弱（Colored 路径生效）");
        Image2DAlg_FreeImage(&out);

        // 4ch alpha 保持：BGR 噪声参与降噪、alpha 逐像素原样回贴（拆 BGR 先例）
        cv::Mat noisy4;
        cv::cvtColor(noisy3, noisy4, cv::COLOR_BGR2BGRA);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                noisy4.at<cv::Vec4b>(y, x)[3] = (unsigned char)((x * 3 + y) % 256);
        Image2DAlg_Image in4 = MakeImage(noisy4);
        std::memset(&out, 0, sizeof(out));
        check(Image2DAlg_DenoiseNLMeans(&in4, 50.0, 50.0, 7, 21, &out) == 0,
              "NLMeans 4ch 返回 0（拆 BGR 走 Colored）");
        check(out.width == 64 && out.height == 64 && out.channels == 4,
              "NLMeans 4ch 尺寸/通道与输入一致");
        int alphaKeep = 1;
        for (int i = 0; i < 64 * 64; ++i)
            if (out.data[i * 4 + 3] != in4.data[i * 4 + 3]) { alphaKeep = 0; break; }
        check(alphaKeep, "NLMeans 4ch alpha 逐像素原样保留");
        int cnt4After = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const unsigned char* p = out.data + ((size_t)y * 64 + x) * 4;
                const int dev = std::max({ std::abs((int)p[0] - 90),
                                           std::abs((int)p[1] - 120),
                                           std::abs((int)p[2] - 180) });
                if (dev > 20) ++cnt4After;
            }
        check(cnt4After * 5 <= cnt3Before,
              "NLMeans 4ch BGR 分量降噪生效（拆 BGR 路径与 3ch 同幅度收敛）");
        Image2DAlg_FreeImage(&out);

        // 错误路径
        std::memset(&out, 0, sizeof(out));
        check(Image2DAlg_DenoiseNLMeans(&in, 0.0, 10.0, 7, 21, &out) == -3,
              "NLMeans h<=0 -> -3");
        check(Image2DAlg_DenoiseNLMeans(&in, -1.0, 10.0, 7, 21, &out) == -3,
              "NLMeans h<0 -> -3");
        check(Image2DAlg_DenoiseNLMeans(&in, 10.0, 0.0, 7, 21, &out) == -3,
              "NLMeans h_color<=0 -> -3");
        check(Image2DAlg_DenoiseNLMeans(&in, 10.0, 10.0, 21, 21, &out) == -3,
              "NLMeans search<=template -> -3");
        check(Image2DAlg_DenoiseNLMeans(&in, 10.0, 10.0, 7, 5, &out) == -3,
              "NLMeans search<template -> -3");
        check(Image2DAlg_DenoiseNLMeans(NULL, 10.0, 10.0, 7, 21, &out) == -1,
              "NLMeans in=NULL -> -1");
        check(Image2DAlg_DenoiseNLMeans(&in, 10.0, 10.0, 7, 21, NULL) == -1,
              "NLMeans out=NULL -> -1");

        cv::Mat two(4, 4, CV_8UC2, cv::Scalar(1, 2));
        Image2DAlg_Image in2 = MakeImage(two);
        std::memset(&out, 0, sizeof(out));
        check(Image2DAlg_DenoiseNLMeans(&in2, 10.0, 10.0, 7, 21, &out) == -3,
              "NLMeans 2 通道容器不匹配 -> -3");
        Image2DAlg_FreeImage(&out);
        Image2DAlg_FreeImage(&in2);
        Image2DAlg_FreeImage(&out);
        Image2DAlg_FreeImage(&in3);
        Image2DAlg_FreeImage(&in4);
        Image2DAlg_FreeImage(&in);
    }



    std::printf("== MatchTemplateMulti ==\n");

    // 8x8 模板，在 24x24 图中按 2 倍尺寸贴在 (4,4)

    cv::Mat tpl(8, 8, CV_8UC1);

    for (int y = 0; y < 8; ++y)

        for (int x = 0; x < 8; ++x)

            tpl.at<unsigned char>(y, x) = (unsigned char)(20 * x + 3 * y);

    cv::Mat scaled;

    cv::resize(tpl, scaled, cv::Size(16, 16), 0, 0, 1);

    cv::Mat img(24, 24, CV_8UC1, cv::Scalar(7));

    scaled.copyTo(img(cv::Rect(4, 4, 16, 16)));



    Image2DAlg_Image ia = MakeImage(img);

    Image2DAlg_Image it = MakeImage(tpl);

    double mscore = 0, mscale = 0;

    int mx = -1, my = -1;

    check(Image2DAlg_MatchTemplateMulti(&ia, &it, 5, 1.5, 2.5, 0.1,

                                        &mscore, &mx, &my, &mscale) == 0,

          "多尺度匹配返回 0");

    check(mx == 4 && my == 4, "跨尺度最佳位置 (4,4)");

    check(std::fabs(mscale - 2.0) < 1e-6, "最佳尺度 2.0");

    check(mscore > 0.95, "正确尺度下得分约 1");

    check(Image2DAlg_MatchTemplateMulti(&ia, &it, 5, 2.5, 1.5, 0.1,

                                        &mscore, &mx, &my, &mscale) == -3,

          "lo>hi -> -3");

    check(Image2DAlg_MatchTemplateMulti(&ia, &it, 5, 1.5, 2.5, 0.0,

                                        &mscore, &mx, &my, &mscale) == -3,

          "step=0 -> -3");

    check(Image2DAlg_MatchTemplateMulti(&ia, &it, 5, 3.5, 4.0, 0.1,

                                        &mscore, &mx, &my, &mscale) == -3,

          "所有尺度模板越界 -> -3");

    Image2DAlg_FreeImage(&ia);

    Image2DAlg_FreeImage(&it);

}



static void testRoiAndMultiCircle()

{

    std::printf("== ROI 版测量 ==\n");

    // 两个方块：(2,2)-(12,12) 与 (20,20)-(28,28)

    cv::Mat bin(40, 40, CV_8UC1, cv::Scalar(0));

    for (int y = 2; y < 12; ++y)

        for (int x = 2; x < 12; ++x)

            bin.at<unsigned char>(y, x) = 255;

    for (int y = 20; y < 28; ++y)

        for (int x = 20; x < 28; ++x)

            bin.at<unsigned char>(y, x) = 255;



    Image2DAlg_Image in = MakeImage(bin);

    Image2DAlg_Blob* blobs = NULL;

    int count = 0;



    std::memset(&blobs, 0, sizeof(blobs));   // 占位，实际由接口赋值

    blobs = NULL;

    check(Image2DAlg_ConnectedComponentsROI(&in, 0, 0, 0, 0, 8, 0, 0, &blobs, &count) == 0 &&

          count == 2, "ROI 全图(0尺寸)等价 CC");

    Image2DAlg_FreeBlobs(blobs);



    blobs = NULL;

    count = 0;

    check(Image2DAlg_ConnectedComponentsROI(&in, 15, 15, 20, 20, 8, 0, 0, &blobs, &count) == 0 &&

          count == 1, "ROI 只含第二个方块");

    check(blobs != NULL && blobs[0].left == 20 && blobs[0].top == 20 &&

          blobs[0].width == 8 && blobs[0].height == 8, "ROI 结果坐标已平移回全图系");

    check(blobs != NULL && std::fabs(blobs[0].cx - 23.5) < 1e-6, "ROI 质心为全图坐标（8px 方块质心 23.5）");

    Image2DAlg_FreeBlobs(blobs);



    blobs = NULL;

    count = 0;

    check(Image2DAlg_ConnectedComponentsROI(&in, 0, 0, 40, 40, 8, 0, 0, &blobs, &count) == 0 &&

          count == 2, "ROI 覆盖全图等价 CC");

    Image2DAlg_FreeBlobs(blobs);

    check(Image2DAlg_ConnectedComponentsROI(&in, 100, 100, 5, 5, 8, 0, 0, &blobs, &count) == -3,

          "ROI 完全越界 -> -3");

    check(Image2DAlg_ConnectedComponentsROI(&in, 0, 0, 10, 10, 6, 0, 0, &blobs, &count) == -3,

          "非法 connectivity -> -3");



    // AC ROI：方块 (12,12)-(20,20)，ROI (10,10,15,15)

    cv::Mat bin2(30, 30, CV_8UC1, cv::Scalar(0));

    for (int y = 12; y < 20; ++y)

        for (int x = 12; x < 20; ++x)

            bin2.at<unsigned char>(y, x) = 255;

    Image2DAlg_Image in2 = MakeImage(bin2);

    Image2DAlg_ContourInfo* info = NULL;

    int cc = 0;

    check(Image2DAlg_AnalyzeContoursROI(&in2, 10, 10, 15, 15, 1, 0, &info, &cc) == 0 && cc == 1,

          "AC ROI 返回 1 条轮廓");

    check(info != NULL && info[0].bbox_x == 12 && info[0].bbox_y == 12 &&

          info[0].bbox_w == 8, "AC ROI 包围盒为全图坐标");

    Image2DAlg_FreeContours(info);

    info = NULL;

    check(Image2DAlg_AnalyzeContours(&in2, 1, 0, &info, &cc) == 0 && cc == 1 &&

          info != NULL && info[0].bbox_x == 12, "与全图版 AC 结果一致");

    Image2DAlg_FreeContours(info);

    Image2DAlg_FreeImage(&in2);

    Image2DAlg_FreeImage(&in);



    std::printf("== FitCirclesFromBinary ==\n");

    // 两个抗锯齿圆：r=10@(14,14)，r=6@(34,30)。

    // 注意 findContours 返回顺序自下而上（小圆在前），断言按最近圆心配对，不假设顺序。

    cv::Mat g(48, 48, CV_8UC1, cv::Scalar(0));

    cv::circle(g, cv::Point(14, 14), 10, cv::Scalar(255), -1, cv::LINE_AA);

    cv::circle(g, cv::Point(34, 30), 6, cv::Scalar(255), -1, cv::LINE_AA);

    Image2DAlg_Image gin = MakeImage(g);

    Image2DAlg_CircleFit* circles = NULL;

    int nc = 0;

    check(Image2DAlg_FitCirclesFromBinary(&gin, 10, 0, 0, 0, 0, &circles, &nc) == 0 && nc == 2,

          "双圆拟合返回 2 个");

    // 取距期望圆心最近的拟合结果（顺序无关）

    auto nearest = [&](double ex, double ey) -> const Image2DAlg_CircleFit* {

        const Image2DAlg_CircleFit* best = NULL;

        double bd = 1e18;

        for (int i = 0; i < nc && circles; ++i) {

            const double d = std::hypot(circles[i].cx - ex, circles[i].cy - ey);

            if (d < bd) { bd = d; best = &circles[i]; }

        }

        return best;

    };

    const Image2DAlg_CircleFit* big = nearest(14.0, 14.0);

    const Image2DAlg_CircleFit* small = nearest(34.0, 30.0);

    check(big != NULL && std::fabs(big->cx - 14.0) < 0.8 && std::fabs(big->cy - 14.0) < 0.8,

          "大圆圆心误差 < 0.8 像素");

    check(big != NULL && std::fabs(big->r - 10.0) < 1.6,

          "大圆半径在 AA 覆盖偏差内（<1.6：cv::circle AA fringe 使二值半径偏大 ~1px，亚像素可回收）");

    check(small != NULL && std::fabs(small->cx - 34.0) < 0.8 &&

          std::fabs(small->r - 6.0) < 1.0, "小圆圆心/半径在抗锯齿偏差内");



    circles = NULL;

    nc = 0;

    check(Image2DAlg_FitCirclesFromBinary(&gin, 10, 0, 8, 0, 0, &circles, &nc) == 0 && nc == 1 &&

          circles != NULL && circles[0].r > 8, "min_r 过滤掉小圆");

    Image2DAlg_FreeCircles(circles);



    circles = NULL;

    nc = 0;

    check(Image2DAlg_FitCirclesFromBinary(&gin, 10, 0, 0, 0, 1, &circles, &nc) == 0 && nc == 2,

          "亚像素模式返回 2 个");

    if (nc == 2) {

        // 亚像素相对像素级：大圆半径更接近真值、残差更小（合成数据确定性成立）

        const Image2DAlg_CircleFit* pb = NULL;

        double bd = 1e18;

        for (int i = 0; i < nc; ++i) {

            const double d = std::hypot(circles[i].cx - 14.0, circles[i].cy - 14.0);

            if (d < bd) { bd = d; pb = &circles[i]; }

        }

        check(pb != NULL && std::fabs(pb->r - 10.0) < std::fabs(big->r - 10.0),

              "亚像素半径比像素级更接近真值");

        check(pb != NULL && pb->rms < big->rms, "亚像素残差小于像素级残差");

        check(pb != NULL && std::hypot(pb->cx - 14.0, pb->cy - 14.0) < 0.5,

              "亚像素圆心误差 < 0.5 像素");

    }

    Image2DAlg_FreeCircles(circles);



    circles = (Image2DAlg_CircleFit*)1;

    nc = -1;

    check(Image2DAlg_FitCirclesFromBinary(&gin, 0, 0, 0, 0, 0, NULL, &nc) == -1,

          "out_circles=NULL -> -1");

    Image2DAlg_FreeCircles(NULL);

    check(true, "FreeCircles NULL 安全");

    Image2DAlg_FreeImage(&gin);

}



static void testContourPoints()

{

    std::printf("== ContourPoints ==\n");

    // 10x10 方块 (5,5)-(14,14)：CHAIN_APPROX_SIMPLE 压缩为 4 个角点

    cv::Mat bin(30, 30, CV_8UC1, cv::Scalar(0));

    for (int y = 5; y < 15; ++y)

        for (int x = 5; x < 15; ++x)

            bin.at<unsigned char>(y, x) = 255;



    Image2DAlg_Image in = MakeImage(bin);

    float* xy = NULL;

    int points = 0, *offs = NULL, ncont = 0;

    check(Image2DAlg_ContourPoints(&in, 1, 0, &xy, &points, &offs, &ncont) == 0,

          "轮廓点导出返回 0");

    check(ncont == 1 && offs != NULL && offs[0] == 0, "单条轮廓偏移 0");

    check(points == 4, "SIMPLE 压缩后方块 4 角点");

    bool corners = false;

    if (xy) {

        int hit = 0;

        for (int i = 0; i < points; ++i) {   // points = 点个数（每点 2 个 float）

            const float x = xy[i * 2], y = xy[i * 2 + 1];

            if ((x == 5 && y == 5) || (x == 14 && y == 5) ||

                (x == 14 && y == 14) || (x == 5 && y == 14))

                ++hit;

        }

        corners = (hit == 4);

    }

    check(corners, "四角点全部在点集中");

    Image2DAlg_FreeContourPoints(xy, offs);



    // 两个图形 -> 2 条轮廓 + 偏移表

    cv::circle(bin, cv::Point(22, 22), 4, cv::Scalar(255), -1);

    Image2DAlg_Image in2 = MakeImage(bin);

    xy = NULL; offs = NULL; points = 0; ncont = 0;

    check(Image2DAlg_ContourPoints(&in2, 1, 0, &xy, &points, &offs, &ncont) == 0 && ncont == 2,

          "两图形 -> 2 条轮廓");

    check(offs != NULL && offs[0] == 0 && offs[1] > 0 && offs[1] < points,

          "偏移表首项 0 且严格递增（轮廓顺序不假设，findContours 自下而上）");

    check(points > 4, "总点数 = 方块 4 角点 + 圆轮廓");

    Image2DAlg_FreeContourPoints(xy, offs);



    xy = (float*)1; offs = (int*)1; points = -1; ncont = -1;

    check(Image2DAlg_ContourPoints(&in2, 1, 0, NULL, &points, &offs, &ncont) == -1,

          "out_xy=NULL -> -1");

    Image2DAlg_FreeContourPoints(NULL, NULL);

    check(true, "FreeContourPoints NULL 安全");

    Image2DAlg_FreeImage(&in2);

    Image2DAlg_FreeImage(&in);

}



static void testWarpAndHsvWrap()

{

    std::printf("== WarpAffine/WarpPerspective ==\n");

    cv::Mat g(8, 8, CV_8UC1, cv::Scalar(0));

    g.at<unsigned char>(1, 1) = 200;   // 标记点 (1,1)

    Image2DAlg_Image in = MakeImage(g);

    Image2DAlg_Image out;



    const double ident6[6] = { 1, 0, 0, 0, 1, 0 };

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_WarpAffine(&in, ident6, 8, 8, 0, &out) == 0, "仿射恒等返回 0");

    check(out.width == 8 && out.height == 8 && out.channels == 1 && Pix(out, 1, 1) == 200,

          "仿射恒等保持原图");

    Image2DAlg_FreeImage(&out);



    const double shift[6] = { 1, 0, 2, 0, 1, 3 };   // 平移 (2,3)

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_WarpAffine(&in, shift, 8, 8, 0, &out) == 0, "仿射平移返回 0");

    check(Pix(out, 3, 4) == 200 && Pix(out, 1, 1) == 0, "标记点移到 (3,4)");

    Image2DAlg_FreeImage(&out);



    const double ident9[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

    std::memset(&out, 0, sizeof(out));

    check(Image2DAlg_WarpPerspective(&in, ident9, 8, 8, 0, &out) == 0 &&

          Pix(out, 1, 1) == 200, "透视恒等保持原图");

    Image2DAlg_FreeImage(&out);



    check(Image2DAlg_WarpAffine(&in, ident6, 0, 8, 0, &out) == -3, "画布宽 0 -> -3");

    check(Image2DAlg_WarpAffine(&in, NULL, 8, 8, 0, &out) == -1, "m=NULL -> -1");

    check(Image2DAlg_WarpPerspective(&in, NULL, 8, 8, 0, &out) == -1, "透视 m=NULL -> -1");

    Image2DAlg_FreeImage(&in);



    std::printf("== HSVThresholdWrap ==\n");

    // 用 cvtColor 构造 H=2 与 H=178 两个红色系像素（S=V=255）

    cv::Mat hsv(1, 2, CV_8UC3);

    hsv.at<cv::Vec3b>(0, 0) = cv::Vec3b(2, 255, 255);

    hsv.at<cv::Vec3b>(0, 1) = cv::Vec3b(178, 255, 255);

    cv::Mat bgr;

    cv::cvtColor(hsv, bgr, cv::COLOR_HSV2BGR);

    Image2DAlg_Image hin = MakeImage(bgr);

    Image2DAlg_Image hout;



    std::memset(&hout, 0, sizeof(hout));

    check(Image2DAlg_HSVThreshold(&hin, 175, 5, 200, 255, 200, 255, &hout) == 0 &&

          CountNonZero(hout) == 0, "非 wrap 版 lo>hi 全空（跨 0 痛点复现）");

    Image2DAlg_FreeImage(&hout);



    std::memset(&hout, 0, sizeof(hout));

    check(Image2DAlg_HSVThresholdWrap(&hin, 175, 5, 200, 255, 200, 255, &hout) == 0 &&

          CountNonZero(hout) == 2, "wrap 版红色跨 0 两点全命中");

    Image2DAlg_FreeImage(&hout);



    std::memset(&hout, 0, sizeof(hout));

    check(Image2DAlg_HSVThresholdWrap(&hin, 10, 170, 200, 255, 200, 255, &hout) == 0 &&

          CountNonZero(hout) == 0, "wrap 版正常区间语义不变");

    Image2DAlg_FreeImage(&hout);

    Image2DAlg_FreeImage(&hin);

}



static void testMatchEnhancedAndFits()

{

    std::printf("== MatchTemplateMasked/Rotated/AllMatches/Ex ==\n");

    // 12x12 独特图案模板 + 40x40 图像贴一处；掩膜挡住左下角

    cv::Mat tpl(12, 12, CV_8UC1);

    for (int y = 0; y < 12; ++y)

        for (int x = 0; x < 12; ++x)

            tpl.at<unsigned char>(y, x) = (unsigned char)(17 * x + 23 * y + 5);

    cv::Mat img(40, 40, CV_8UC1, cv::Scalar(3));

    tpl.copyTo(img(cv::Rect(4, 4, 12, 12)));



    cv::Mat mask(12, 12, CV_8UC1, cv::Scalar(255));

    for (int y = 8; y < 12; ++y)

        for (int x = 0; x < 4; ++x)

            mask.at<unsigned char>(y, x) = 0;



    Image2DAlg_Image ia = MakeImage(img);

    Image2DAlg_Image it = MakeImage(tpl);

    Image2DAlg_Image im = MakeImage(mask);

    double score = 0;

    int x = -1, y = -1;

    check(Image2DAlg_MatchTemplateMasked(&ia, &it, &im, 3, &score, &x, &y) == 0,

          "掩膜匹配返回 0");

    check(x == 4 && y == 4, "掩膜匹配位置 (4,4)");

    check(Image2DAlg_MatchTemplateMasked(&ia, &it, &im, 5, &score, &x, &y) == -3,

          "掩膜不支持 CCOEFF_NORMED -> -3");



    // 多目标：单实例 + 双实例

    Image2DAlg_Match* matches = NULL;

    int nmatch = 0;

    check(Image2DAlg_FindAllMatches(&ia, &it, 5, 0.9, 4, 4, NULL, &nmatch) == -1,

          "out_matches=NULL -> -1");

    check(Image2DAlg_FindAllMatches(&ia, &it, 5, 0.9, 4, 4, &matches, &nmatch) == 0 && nmatch == 1,

          "单实例 NMS 后 1 目标");

    Image2DAlg_FreeMatches(matches);



    cv::Mat img3(40, 40, CV_8UC1, cv::Scalar(3));

    tpl.copyTo(img3(cv::Rect(4, 4, 12, 12)));

    tpl.copyTo(img3(cv::Rect(22, 20, 12, 12)));

    Image2DAlg_Image ia3 = MakeImage(img3);

    matches = NULL;

    nmatch = 0;

    check(Image2DAlg_FindAllMatches(&ia3, &it, 5, 0.9, 4, 3, &matches, &nmatch) == 0 && nmatch == 2,

          "双实例 NMS 后 2 目标");

    check(matches != NULL && matches[0].score >= matches[1].score, "按得分从优到劣");

    Image2DAlg_FreeMatches(matches);



    // 旋转搜索：模板旋转 30 度，旋转画布外扩避免裁切，中心落在 (20,16)

    cv::Mat m30 = cv::getRotationMatrix2D(cv::Point2f(6, 6), 30, 1);

    m30.at<double>(0, 2) += 3.0;   // 旋转内容平移到 18x18 画布中心 (9,9)

    m30.at<double>(1, 2) += 3.0;

    cv::Mat rot;

    cv::warpAffine(tpl, rot, m30, cv::Size(18, 18), 1, cv::BORDER_CONSTANT, cv::Scalar(3));

    cv::Mat img2(40, 40, CV_8UC1, cv::Scalar(3));

    rot.copyTo(img2(cv::Rect(11, 7, 18, 18)));

    Image2DAlg_Image ir = MakeImage(img2);

    Image2DAlg_Image itp = MakeImage(tpl);

    double rs = 0, rcx = 0, rcy = 0, ra = 0;

    check(Image2DAlg_MatchTemplateRotated(&ir, &itp, 0, 45, 15, 5, &rs, &rcx, &rcy, &ra) == 0,

          "旋转搜索返回 0");

    check(std::fabs(ra - 30.0) < 15.1, "命中角度 30 度附近（一个步进内）");

    check(std::hypot(rcx - 20.0, rcy - 16.0) < 3.0, "中心落点误差 < 3 像素");

    check(Image2DAlg_MatchTemplateRotated(&ir, &itp, 45, 0, 15, 5, &rs, &rcx, &rcy, &ra) == -3,

          "end<start -> -3");



    // 组合搜索 Ex：仅角度维度（scale 固定 1）

    double es = 0, esx = 0, esy = 0, esc = 0, esa = 0;

    check(Image2DAlg_MatchTemplateEx(&ir, &itp, 5, 1, 1, 1, 0, 45, 15,

                                     &es, &esx, &esy, &esc, &esa) == 0,

          "Ex 组合搜索返回 0");

    check(std::fabs(esa - 30.0) < 15.1, "Ex 命中角度 30 度附近");

    check(Image2DAlg_MatchTemplateEx(&ir, &itp, 5, 2.5, 1.5, 0.5, 0, 45, 15,

                                     &es, &esx, &esy, &esc, &esa) == -3,

          "Ex lo>hi -> -3");

    Image2DAlg_FreeImage(&ir);

    Image2DAlg_FreeImage(&itp);

    Image2DAlg_FreeImage(&ia);

    Image2DAlg_FreeImage(&it);

    Image2DAlg_FreeImage(&im);



    std::printf("== FitCircleEx/Robust/Ellipse/Rect/Poly ==\n");

    float fxy[30];

    for (int i = 0; i < 12; ++i) {

        const double a = 2.0 * 3.14159265358979323846 * i / 12.0;

        fxy[i * 2] = (float)(10.0 * std::cos(a) + 100.0);

        fxy[i * 2 + 1] = (float)(10.0 * std::sin(a) + 50.0);

    }

    Image2DAlg_CircleFit cf;

    check(Image2DAlg_FitCircleEx(fxy, 12, &cf) == 0 &&

          std::fabs(cf.cx - 100.0) < 1e-3 && std::fabs(cf.r - 10.0) < 1e-3 && cf.rms < 1e-6,

          "FitCircleEx 精确圆（GN 精化）");



    // 外点：3 个远离点，RANSAC 后圆参数仍准确、内点 12 个

    fxy[24] = 200; fxy[25] = 200;

    fxy[26] = 0;   fxy[27] = 200;

    fxy[28] = 200; fxy[29] = 0;

    int* inl = NULL;

    int ninl = 0;

    check(Image2DAlg_FitCircleRobust(fxy, 15, 0.5, 0, 7, &cf, &inl, &ninl) == 0,

          "RANSAC 圆拟合返回 0");

    check(std::fabs(cf.cx - 100.0) < 0.5 && std::fabs(cf.r - 10.0) < 0.5,

          "RANSAC 抗外点（圆心/半径不受污染）");

    check(ninl == 12, "内点 12 个");

    Image2DAlg_FreeInliers(inl);

    inl = NULL;

    ninl = 0;

    check(Image2DAlg_FitCircleRobust(fxy, 15, 0, 0, 7, &cf, &inl, &ninl) == -3,

          "threshold<=0 -> -3");

    Image2DAlg_FreeInliers(NULL);

    check(true, "FreeInliers NULL 安全");



    // 椭圆：长轴 40（x 向）短轴 20

    float exy[40];

    for (int i = 0; i < 20; ++i) {

        const double a = 2.0 * 3.14159265358979323846 * i / 20.0;

        exy[i * 2] = (float)(20.0 * std::cos(a) + 30.0);

        exy[i * 2 + 1] = (float)(10.0 * std::sin(a) + 30.0);

    }

    Image2DAlg_EllipseFit ef;

    check(Image2DAlg_FitEllipse(exy, 20, &ef) == 0, "椭圆拟合返回 0");

    check(std::fabs(ef.cx - 30.0) < 0.5 && std::fabs(ef.cy - 30.0) < 0.5,

          "椭圆中心正确");

    // OpenCV RotatedRect 的 size 宽高顺序随角度约定可能互换，按长短轴集合断言

    check(std::fabs(std::min(ef.width, ef.height) - 20.0) < 1.5 &&

          std::fabs(std::max(ef.width, ef.height) - 40.0) < 1.5,

          "椭圆全轴长 20/40（顺序无关）");

    check(Image2DAlg_FitEllipse(exy, 4, &ef) == -3, "椭圆点数 < 5 -> -3");



    const float rect[8] = { 0.f, 0.f, 20.f, 0.f, 20.f, 10.f, 0.f, 10.f };

    Image2DAlg_RectFit rf;

    check(Image2DAlg_FitRect(rect, 4, &rf) == 0 && std::fabs(rf.cx - 10.0) < 1e-6 &&

          std::fabs(rf.cy - 5.0) < 1e-6, "矩形拟合中心正确");

    check(std::fabs(std::min(rf.width, rf.height) - 10.0) < 1e-6 &&

          std::fabs(std::max(rf.width, rf.height) - 20.0) < 1e-6,

          "矩形边长 10/20（顺序无关，OpenCV 角度约定）");

    check(Image2DAlg_FitRect(rect, 2, &rf) == -3, "矩形点数 < 3 -> -3");



    // 多项式：y = 2x + 1；y = x^2

    float pxy[12];

    for (int i = 0; i < 5; ++i) {

        pxy[i * 2] = (float)i;

        pxy[i * 2 + 1] = (float)(2 * i + 1);

    }

    Image2DAlg_PolyFit pf;

    check(Image2DAlg_FitPoly(pxy, 5, 1, &pf) == 0 &&

          std::fabs(pf.coef[0] - 1.0) < 1e-6 && std::fabs(pf.coef[1] - 2.0) < 1e-6 &&

          pf.rms < 1e-6, "一次多项式 y=2x+1 精确");

    for (int i = 0; i < 6; ++i) {

        pxy[i * 2] = (float)(i - 3);

        pxy[i * 2 + 1] = (float)((i - 3) * (i - 3));

    }

    check(Image2DAlg_FitPoly(pxy, 6, 2, &pf) == 0 &&

          std::fabs(pf.coef[2] - 1.0) < 1e-6 && std::fabs(pf.coef[0]) < 1e-6 &&

          std::fabs(pf.coef[1]) < 1e-6, "二次多项式 y=x^2 精确");

    check(Image2DAlg_FitPoly(pxy, 6, 0, &pf) == -3, "degree=0 -> -3");

    check(Image2DAlg_FitPoly(pxy, 6, 6, &pf) == -3, "degree=6 -> -3");

    // 2 点 + 1 阶 = 恰定可解（两点定一线），非错误

    check(Image2DAlg_FitPoly(pxy, 2, 1, &pf) == 0 && pf.rms < 1e-6, "2 点 1 阶恰定拟合");

    check(Image2DAlg_FitPoly(pxy, 1, 1, &pf) == -3, "点数不足 -> -3");

}



static void testHough()

{

    std::printf("== HoughLinesP/HoughCircles ==\n");

    // 水平亮线段 y=8, x=2..14

    cv::Mat bin(24, 24, CV_8UC1, cv::Scalar(0));

    cv::line(bin, cv::Point(2, 8), cv::Point(14, 8), cv::Scalar(255), 1);

    Image2DAlg_Image in = MakeImage(bin);

    Image2DAlg_Line4* lines = NULL;

    int count = 0;

    check(Image2DAlg_HoughLinesP(&in, 1.0, 1.0, 10, 5, 0, &lines, &count) == 0,

          "HoughLinesP 返回 0");

    check(count >= 1 && lines != NULL, "检出线段 >= 1");

    bool on_line = false;

    for (int i = 0; i < count && lines; ++i)

        if ((lines[i].y1 == 8 && lines[i].y2 == 8) &&

            std::min(lines[i].x1, lines[i].x2) <= 4 && std::max(lines[i].x1, lines[i].x2) >= 12)

            on_line = true;

    check(on_line, "线段落在 y=8 且覆盖 x 2..14");

    Image2DAlg_FreeLines(lines);

    check(Image2DAlg_HoughLinesP(&in, 0.0, 1.0, 10, 5, 0, &lines, &count) == -3,

          "rho<=0 -> -3");

    Image2DAlg_FreeLines(NULL);

    check(true, "FreeLines NULL 安全");

    Image2DAlg_FreeImage(&in);



    // 圆：填充圆 + 高斯平滑（HOUGH_GRADIENT 对锐利二值图可能无果）

    cv::Mat g(64, 64, CV_8UC1, cv::Scalar(0));

    cv::circle(g, cv::Point(32, 32), 10, cv::Scalar(255), -1);

    cv::GaussianBlur(g, g, cv::Size(5, 5), 0);

    Image2DAlg_Image gin = MakeImage(g);

    Image2DAlg_HoughCircle* circles = NULL;

    int nc = 0;

    check(Image2DAlg_HoughCircles(&gin, 1, 10, 100, 30, 5, 14, &circles, &nc) == 0,

          "HoughCircles 返回 0");

    bool hit = false;

    for (int i = 0; i < nc && circles; ++i)

        if (std::hypot(circles[i].cx - 32, circles[i].cy - 32) < 3.0 &&

            std::fabs(circles[i].r - 10) < 3.0)

            hit = true;

    check(hit, "检出圆心/半径接近真值");

    Image2DAlg_FreeCirclesHough(circles);

    check(Image2DAlg_HoughCircles(&gin, 0, 10, 100, 30, 5, 14, &circles, &nc) == -3,

          "dp<=0 -> -3");

    Image2DAlg_FreeCirclesHough(NULL);

    check(true, "FreeCirclesHough NULL 安全");

    Image2DAlg_FreeImage(&gin);

}



static void testFindAllMatchesExAndSubPix()

{

    std::printf("== MatchTemplateSubPix ==\n");

    cv::Mat tpl(8, 8, CV_8UC1);

    for (int y = 0; y < 8; ++y)

        for (int x = 0; x < 8; ++x)

            tpl.at<unsigned char>(y, x) = (unsigned char)(29 * x + 13 * y + 7);

    cv::Mat img(24, 24, CV_8UC1, cv::Scalar(3));

    tpl.copyTo(img(cv::Rect(4, 4, 8, 8)));

    Image2DAlg_Image ia = MakeImage(img);

    Image2DAlg_Image it = MakeImage(tpl);

    double score = 0, sx = 0, sy = 0;

    check(Image2DAlg_MatchTemplateSubPix(&ia, &it, 5, &score, &sx, &sy) == 0,

          "亚像素匹配返回 0");

    check(std::fabs(sx - 4.0) < 0.6 && std::fabs(sy - 4.0) < 0.6,

          "亚像素位置≈(4,4)");

    check(score > 0.9, "亚像素得分 > 0.9");

    check(Image2DAlg_MatchTemplateSubPix(&ia, &it, 9, &score, &sx, &sy) == -3, "method 越界 -> -3");



    std::printf("== FindAllMatchesEx ==\n");

    // 未旋转双实例（12x12 模板贴 (4,4) 与 (22,20)，中心 (10,10)/(28,26)）

    cv::Mat img2(40, 40, CV_8UC1, cv::Scalar(3));

    tpl = tpl.clone();

    cv::resize(tpl, tpl, cv::Size(12, 12));   // 8x8 → 12x12 保持图案

    tpl.copyTo(img2(cv::Rect(4, 4, 12, 12)));

    tpl.copyTo(img2(cv::Rect(22, 20, 12, 12)));

    Image2DAlg_Image ia2 = MakeImage(img2);

    Image2DAlg_Image it12 = MakeImage(tpl);

    Image2DAlg_MatchEx* mex = NULL;

    int nmx = 0;

    check(Image2DAlg_FindAllMatchesEx(&ia2, &it12, 5, 0.9, 10, 10,

                                      1, 1, 1, 0, 0, 1, &mex, &nmx) == 0 && nmx == 2,

          "未旋转双实例 -> 2 目标");

    check(mex != NULL && mex[0].score >= mex[1].score, "按得分从优到劣");

    bool hit1 = false, hit2 = false;

    for (int i = 0; i < nmx && mex; ++i) {

        if (std::hypot(mex[i].cx - 10.0, mex[i].cy - 10.0) < 2.0) hit1 = true;

        if (std::hypot(mex[i].cx - 28.0, mex[i].cy - 26.0) < 2.0) hit2 = true;

    }

    check(hit1 && hit2, "两中心 (10,10)/(28,26) 均命中");

    check(mex != NULL && std::fabs(mex[0].scale - 1.0) < 1e-9 &&

          std::fabs(mex[0].angle - 0.0) < 1e-9, "scale/angle 回填正确");

    Image2DAlg_FreeMatchesEx(mex);



    // 旋转单实例：30 度模板中心 (20,16)，角度搜索命中且跨组合 NMS 后仅 1 目标

    cv::Mat m30 = cv::getRotationMatrix2D(cv::Point2f(6, 6), 30, 1);

    m30.at<double>(0, 2) += 3.0;

    m30.at<double>(1, 2) += 3.0;

    cv::Mat tpl8 = tpl.clone();

    cv::resize(tpl8, tpl8, cv::Size(8, 8));   // 回到 8x8 作为旋转源

    cv::Mat rot;

    cv::warpAffine(tpl8, rot, m30, cv::Size(18, 18), 1, cv::BORDER_CONSTANT, cv::Scalar(3));

    cv::Mat img3(40, 40, CV_8UC1, cv::Scalar(3));

    rot.copyTo(img3(cv::Rect(11, 7, 18, 18)));

    Image2DAlg_Image ia3 = MakeImage(img3);

    Image2DAlg_Image it8 = MakeImage(tpl8);

    mex = NULL;

    nmx = 0;

    check(Image2DAlg_FindAllMatchesEx(&ia3, &it8, 5, 0.6, 10, 6,

                                      1, 1, 1, 0, 45, 15, &mex, &nmx) == 0 && nmx == 1,

          "旋转单实例跨组合 NMS 后 1 目标");

    check(mex != NULL && std::fabs(mex[0].angle - 30.0) < 15.1, "命中角度 30 度附近");

    check(mex != NULL && std::hypot(mex[0].cx - 20.0, mex[0].cy - 16.0) < 3.0,

          "中心落点误差 < 3 像素");

    Image2DAlg_FreeMatchesEx(mex);



    // 尺度维度：2x 缩放实例，尺度搜索命中 2.0

    cv::Mat scaled;

    cv::resize(tpl8, scaled, cv::Size(16, 16));

    cv::Mat img4(40, 40, CV_8UC1, cv::Scalar(3));

    scaled.copyTo(img4(cv::Rect(16, 16, 16, 16)));   // 中心 (24,24)

    Image2DAlg_Image ia4 = MakeImage(img4);

    mex = NULL;

    nmx = 0;

    check(Image2DAlg_FindAllMatchesEx(&ia4, &it8, 5, 0.6, 10, 6,

                                      1.5, 2.5, 0.5, 0, 0, 1, &mex, &nmx) == 0 && nmx >= 1,

          "尺度搜索返回目标");

    check(mex != NULL && std::fabs(mex[0].scale - 2.0) < 1e-9 &&

          std::hypot(mex[0].cx - 24.0, mex[0].cy - 24.0) < 2.5,

          "命中尺度 2.0、中心 (24,24)");

    Image2DAlg_FreeMatchesEx(mex);



    // 错误路径

    mex = (Image2DAlg_MatchEx*)1;

    nmx = -1;

    check(Image2DAlg_FindAllMatchesEx(&ia4, &it8, 5, 0.6, 0, 6,

                                      1, 2, 0.5, 0, 0, 1, &mex, &nmx) == -3,

          "max_matches=0 -> -3");

    check(Image2DAlg_FindAllMatchesEx(&ia4, &it8, 5, 0.6, 10, 6,

                                      2.5, 1.5, 0.5, 0, 0, 1, &mex, &nmx) == -3,

          "scale lo>hi -> -3");

    check(Image2DAlg_FindAllMatchesEx(&ia4, &it8, 5, 0.6, 10, 6,

                                      1, 2, 0.5, 45, 0, 1, &mex, &nmx) == -3,

          "angle end<start -> -3");

    check(Image2DAlg_FindAllMatchesEx(&ia4, &it8, 5, 0.6, 10, 6,

                                      1, 2, 0.5, 0, 0, 1, NULL, &nmx) == -1,

          "out_matches=NULL -> -1");

    Image2DAlg_FreeMatchesEx(NULL);

    check(true, "FreeMatchesEx NULL 安全");

    Image2DAlg_FreeImage(&ia4);

    Image2DAlg_FreeImage(&it8);

    Image2DAlg_FreeImage(&ia3);

    Image2DAlg_FreeImage(&it12);

    Image2DAlg_FreeImage(&ia2);

    Image2DAlg_FreeImage(&it);

    Image2DAlg_FreeImage(&ia);

}



static void testFitEllipsesFromBinary()

{

    std::printf("== FitEllipsesFromBinary ==\n");

    // 两个抗锯齿椭圆：全轴 40x20@(30,30) 与 16x8@(10,10)（用 cv::ellipse 画）

    cv::Mat g(64, 64, CV_8UC1, cv::Scalar(0));

    cv::ellipse(g, cv::Point(30, 30), cv::Size(20, 10), 0, 0, 360, cv::Scalar(255), -1, cv::LINE_AA);

    cv::ellipse(g, cv::Point(10, 10), cv::Size(8, 4), 0, 0, 360, cv::Scalar(255), -1, cv::LINE_AA);

    Image2DAlg_Image in = MakeImage(g);

    Image2DAlg_EllipseFit* fits = NULL;

    int count = 0;

    check(Image2DAlg_FitEllipsesFromBinary(&in, 10, 0, 0, 0, &fits, &count) == 0 && count == 2,

          "双椭圆拟合返回 2 个");

    bool hit_big = false, hit_small = false;

    for (int i = 0; i < count && fits; ++i) {

        if (std::hypot(fits[i].cx - 30, fits[i].cy - 30) < 1.5 &&

            std::fabs(std::max(fits[i].width, fits[i].height) - 40.0) < 3.5 &&

            std::fabs(std::min(fits[i].width, fits[i].height) - 20.0) < 3.5)

            hit_big = true;

        if (std::hypot(fits[i].cx - 10, fits[i].cy - 10) < 1.5 &&

            std::fabs(std::max(fits[i].width, fits[i].height) - 16.0) < 3.5)

            hit_small = true;

    }

    check(hit_big, "大椭圆中心正确、全轴长在 AA 覆盖偏差内（cv::ellipse AA fringe 使二值区域外扩 ~1px/侧，轴容差 3.5）");

    check(hit_small, "小椭圆中心/全轴长正确");

    Image2DAlg_FreeEllipses(fits);



    fits = NULL;

    count = 0;

    check(Image2DAlg_FitEllipsesFromBinary(&in, 10, 0, 30, 0, &fits, &count) == 0 && count == 1,

          "min_axis 过滤掉小椭圆");

    Image2DAlg_FreeEllipses(fits);



    fits = (Image2DAlg_EllipseFit*)1;

    count = -1;

    check(Image2DAlg_FitEllipsesFromBinary(&in, 10, 0, 0, 0, NULL, &count) == -1,

          "out_ellipses=NULL -> -1");

    Image2DAlg_FreeEllipses(NULL);

    check(true, "FreeEllipses NULL 安全");

    Image2DAlg_FreeImage(&in);

}



static void testRoiFamily()

{

    std::printf("== ROI 族补全 ==\n");

    // 场景：左半区一条水平线（y=6, x=2..20），右半区一个方块 (28,28)-(40,40)

    cv::Mat g(48, 48, CV_8UC1, cv::Scalar(0));

    cv::line(g, cv::Point(2, 6), cv::Point(20, 6), cv::Scalar(255), 1);

    for (int y = 28; y < 40; ++y)

        for (int x = 28; x < 40; ++x)

            g.at<unsigned char>(y, x) = 255;

    Image2DAlg_Image in = MakeImage(g);



    // HoughLinesPROI：ROI 限定左半区 -> 线段坐标回全图系

    Image2DAlg_Line4* lines = NULL;

    int count = 0;

    check(Image2DAlg_HoughLinesPROI(&in, 0, 0, 24, 48, 1.0, 1.0, 10, 5, 0,

                                    &lines, &count) == 0 && count >= 1,

          "HoughLinesPROI 左半区检出线段");

    bool on_line = false;

    for (int i = 0; i < count && lines; ++i)

        if (lines[i].y1 == 6 && lines[i].y2 == 6 && lines[i].x1 >= 2 && lines[i].x2 <= 20)

            on_line = true;

    check(on_line, "线段端点为全图坐标且在 y=6");

    Image2DAlg_FreeLines(lines);

    check(Image2DAlg_HoughLinesPROI(&in, 30, 30, 5, 5, 1.0, 1.0, 10, 5, 0,

                                    &lines, &count) == 0 && count == 0,

          "ROI 内无线段 -> count=0");

    Image2DAlg_FreeLines(lines);



    // HoughCirclesROI：圆在右半区，ROI 限定右半区

    cv::circle(g, cv::Point(36, 12), 8, cv::Scalar(255), -1);

    cv::GaussianBlur(g, g, cv::Size(5, 5), 0);

    Image2DAlg_Image in2 = MakeImage(g);

    Image2DAlg_HoughCircle* circles = NULL;

    int nc = 0;

    check(Image2DAlg_HoughCirclesROI(&in2, 24, 0, 24, 48, 1.0, 10.0, 50, 20, 5, 12,

                                     &circles, &nc) == 0 && nc >= 1,

          "HoughCirclesROI 右半区检出圆");

    bool hit_c = false;

    for (int i = 0; i < nc && circles; ++i)

        if (std::hypot(circles[i].cx - 36, circles[i].cy - 12) < 3.0) hit_c = true;

    check(hit_c, "圆心为全图坐标 (36,12) 附近");

    Image2DAlg_FreeCirclesHough(circles);

    Image2DAlg_FreeImage(&in2);



    // MatchTemplateROI：模板贴在 (10,10)，ROI 限定该区域

    cv::Mat tpl(8, 8, CV_8UC1);

    for (int y = 0; y < 8; ++y)

        for (int x = 0; x < 8; ++x)

            tpl.at<unsigned char>(y, x) = (unsigned char)(29 * x + 13 * y + 7);

    cv::Mat img3(40, 40, CV_8UC1, cv::Scalar(3));

    tpl.copyTo(img3(cv::Rect(10, 10, 8, 8)));

    Image2DAlg_Image ia3 = MakeImage(img3);

    Image2DAlg_Image it3 = MakeImage(tpl);

    double score = 0;

    int x = -1, y = -1;

    check(Image2DAlg_MatchTemplateROI(&ia3, 5, 5, 20, 20, &it3, 5, &score, &x, &y) == 0 &&

          x == 10 && y == 10, "MatchTemplateROI 命中全图坐标 (10,10)");

    check(Image2DAlg_MatchTemplateROI(&ia3, 30, 30, 7, 7, &it3, 5, &score, &x, &y) == -3,

          "模板大于 ROI（8x8 vs 7x7）-> -3");

    check(Image2DAlg_MatchTemplateROI(&ia3, 38, 38, 5, 5, &it3, 5, &score, &x, &y) == -3,

          "ROI 与图像无交集 -> -3");



    // FindAllMatchesROI：双实例只取 ROI 内的

    cv::Mat img4(40, 40, CV_8UC1, cv::Scalar(3));

    tpl.copyTo(img4(cv::Rect(10, 10, 8, 8)));

    tpl.copyTo(img4(cv::Rect(28, 28, 8, 8)));

    Image2DAlg_Image ia4 = MakeImage(img4);

    Image2DAlg_Match* matches = NULL;

    int nmatch = 0;

    check(Image2DAlg_FindAllMatchesROI(&ia4, 5, 5, 20, 20, &it3, 5, 0.9, 4, 3,

                                       &matches, &nmatch) == 0 && nmatch == 1,

          "FindAllMatchesROI 只取 ROI 内目标");

    check(matches != NULL && matches[0].x == 10 && matches[0].y == 10,

          "命中位置为全图坐标 (10,10)");

    Image2DAlg_FreeMatches(matches);

    Image2DAlg_FreeImage(&ia4);

    Image2DAlg_FreeImage(&it3);

    Image2DAlg_FreeImage(&ia3);

    Image2DAlg_FreeImage(&in2);

    Image2DAlg_FreeImage(&in);

}



static void testMatchPyramid()

{

    std::printf("== MatchTemplatePyramid ==\n");

    cv::Mat tpl(8, 8, CV_8UC1);

    for (int y = 0; y < 8; ++y)

        for (int x = 0; x < 8; ++x)

            tpl.at<unsigned char>(y, x) = (unsigned char)(29 * x + 13 * y + 7);

    cv::Mat img(40, 40, CV_8UC1, cv::Scalar(3));

    tpl.copyTo(img(cv::Rect(10, 10, 8, 8)));

    Image2DAlg_Image ia = MakeImage(img);

    Image2DAlg_Image it = MakeImage(tpl);

    double score = 0, cx = 0, cy = 0, scale = 0, angle = 0;



    // levels=1 与 MatchTemplateEx 一致性（同参数同结果）

    double es = 0, ex = 0, ey = 0, esc = 0, ea = 0;

    check(Image2DAlg_MatchTemplateEx(&ia, &it, 5, 1, 1, 1, 0, 0, 1,

                                     &es, &ex, &ey, &esc, &ea) == 0, "Ex 基线返回 0");

    check(Image2DAlg_MatchTemplatePyramid(&ia, &it, 5, 1, 1, 1, 0, 0, 1, 1,

                                          &score, &cx, &cy, &scale, &angle) == 0,

          "Pyramid levels=1 返回 0");

    check(std::fabs(cx - ex) < 1e-6 && std::fabs(cy - ey) < 1e-6 &&

          std::fabs(score - es) < 1e-9, "levels=1 与 Ex 结果一致");



    // 旋转 30 度 + levels=2：8x8 模板粗化会截断（min 边 12 约束）→ 等价 Ex 全搜

    cv::Mat m30 = cv::getRotationMatrix2D(cv::Point2f(4, 4), 30, 1);

    m30.at<double>(0, 2) += 4.0;   // 旋转内容平移到 12x12 画布中心 (8,8)

    m30.at<double>(1, 2) += 4.0;

    cv::Mat rot;

    cv::warpAffine(tpl, rot, m30, cv::Size(12, 12), 1, cv::BORDER_CONSTANT, cv::Scalar(3));

    cv::Mat img2(40, 40, CV_8UC1, cv::Scalar(3));

    rot.copyTo(img2(cv::Rect(14, 10, 12, 12)));   // 旋转模板中心 (20,16)

    Image2DAlg_Image ia2 = MakeImage(img2);

    check(Image2DAlg_MatchTemplatePyramid(&ia2, &it, 5, 1, 1, 1, 0, 45, 15, 2,

                                          &score, &cx, &cy, &scale, &angle) == 0,

          "Pyramid levels=2 旋转搜索返回 0");

    check(std::fabs(angle - 30.0) < 15.1, "命中角度 30 度附近");

    check(std::hypot(cx - 20.0, cy - 16.0) < 3.0, "中心落点误差 < 3 像素（已映射回原图）");

    Image2DAlg_FreeImage(&ia2);



    // 16x16 模板 + levels=2：真实走金字塔路径（顶层 8x8 判别力足够）

    cv::Mat tpl16(16, 16, CV_8UC1);

    for (int y = 0; y < 16; ++y)

        for (int x = 0; x < 16; ++x)

            tpl16.at<unsigned char>(y, x) = (unsigned char)(29 * x + 13 * y + 7);

    cv::Mat m30b = cv::getRotationMatrix2D(cv::Point2f(8, 8), 30, 1);

    m30b.at<double>(0, 2) += 4.0;   // 旋转内容平移到 24x24 画布中心 (12,12)

    m30b.at<double>(1, 2) += 4.0;

    cv::Mat rot16;

    cv::warpAffine(tpl16, rot16, m30b, cv::Size(24, 24), 1, cv::BORDER_CONSTANT, cv::Scalar(3));

    cv::Mat img3(48, 48, CV_8UC1, cv::Scalar(3));

    rot16.copyTo(img3(cv::Rect(14, 10, 24, 24)));   // 旋转模板中心 (26,22)

    Image2DAlg_Image ia3 = MakeImage(img3);

    Image2DAlg_Image it16 = MakeImage(tpl16);

    check(Image2DAlg_MatchTemplatePyramid(&ia3, &it16, 5, 1, 1, 1, 0, 45, 15, 2,

                                          &score, &cx, &cy, &scale, &angle) == 0,

          "16x16 模板 Pyramid levels=2 返回 0");

    check(std::fabs(angle - 30.0) < 15.1, "16x16 模板命中角度 30 度附近");

    check(std::hypot(cx - 26.0, cy - 22.0) < 3.0, "16x16 模板中心落点误差 < 3 像素");

    Image2DAlg_FreeImage(&ia3);

    Image2DAlg_FreeImage(&it16);



    // 错误路径

    check(Image2DAlg_MatchTemplatePyramid(&ia, &it, 5, 1, 1, 1, 0, 0, 1, 0,

                                          &score, &cx, &cy, &scale, &angle) == -3,

          "levels=0 -> -3");

    check(Image2DAlg_MatchTemplatePyramid(&ia, &it, 5, 2.5, 1.5, 0.5, 0, 0, 1, 2,

                                          &score, &cx, &cy, &scale, &angle) == -3,

          "scale lo>hi -> -3");

    Image2DAlg_FreeImage(&it);

    Image2DAlg_FreeImage(&ia);

}



static void testRegionStats()

{

    std::printf("== RegionStats/Histogram/CompareHist ==\n");

    // 8x8 常量 100 + 右下角 4x4 常量 200（ROI 语义与直方图计数可手算）

    cv::Mat g(8, 8, CV_8UC1, cv::Scalar(100));

    for (int y = 4; y < 8; ++y)

        for (int x = 4; x < 8; ++x)

            g.at<unsigned char>(y, x) = 200;

    Image2DAlg_Image in = MakeImage(g);

    Image2DAlg_GrayStats st;



    check(Image2DAlg_RegionStats(&in, 0, 0, 0, 0, &st) == 0, "全图统计返回 0");

    // 48 点 100 + 16 点 200：mean=125，总体 stddev=sqrt(1875)≈43.301

    check(std::fabs(st.mean - 125.0) < 1e-6 && std::fabs(st.stddev - 43.3013) < 0.01 &&

          st.min == 100 && st.max == 200, "全图 mean/stddev/min/max 精确");



    check(Image2DAlg_RegionStats(&in, 0, 0, 4, 4, &st) == 0 && st.mean == 100.0 &&

          st.stddev == 0.0 && st.min == 100 && st.max == 100, "ROI 内常量区统计精确");

    check(Image2DAlg_RegionStats(&in, 4, 4, 4, 4, &st) == 0 && st.mean == 200.0,

          "ROI 常量 200 区统计精确");

    check(Image2DAlg_RegionStats(&in, 20, 20, 4, 4, &st) == -3, "ROI 无交集 -> -3");



    int h1[256] = { 0 }, h2[256] = { 0 };

    check(Image2DAlg_Histogram(&in, h1) == 0, "直方图返回 0");

    int total = 0;

    for (int i = 0; i < 256; ++i) total += h1[i];

    check(total == 64 && h1[100] == 48 && h1[200] == 16, "直方图计数精确（100:48 / 200:16）");



    // CompareHist：同直方图 vs 平移直方图（200->201）

    int h3[256] = { 0 };

    for (int i = 0; i < 256; ++i) h2[i] = h1[i];

    h2[100] = 47; h2[200] = 15; h2[101] = 1; h2[201] = 1;

    for (int i = 0; i < 256; ++i) h3[i] = h1[i];

    double s1 = 0, s2 = 0;

    check(Image2DAlg_CompareHist(h1, h1, 0, &s1) == 0 && std::fabs(s1 - 1.0) < 1e-9,

          "CORREL 同直方图 = 1");

    check(Image2DAlg_CompareHist(h1, h2, 0, &s2) == 0 && s2 < 1.0 && s2 > 0.9,

          "CORREL 近似直方图 0.9~1.0");

    check(Image2DAlg_CompareHist(h1, h2, 3, &s2) == 0 && s2 < 0.2,

          "BHATTACHARYYA 近似直方图 < 0.2");

    check(Image2DAlg_CompareHist(h1, h2, 9, &s2) == -3, "method 越界 -> -3");

    check(Image2DAlg_CompareHist(NULL, h2, 0, &s2) == -1, "hist1=NULL -> -1");

    Image2DAlg_FreeImage(&in);

}



static void testFeaturePointsAndFits()

{

    std::printf("== HarrisCorners/GoodFeatures/CornerSubPix ==\n");

    cv::Mat g(48, 48, CV_8UC1, cv::Scalar(0));

    cv::rectangle(g, cv::Rect(9, 9, 30, 30), cv::Scalar(255), -1);   // 白方块四角

    cv::GaussianBlur(g, g, cv::Size(5, 5), 0);   // 平滑增强角点梯度稳定性

    Image2DAlg_Image in = MakeImage(g);

    Image2DAlg_Point2f* pts = NULL;

    int count = 0;

    const double trueC[4][2] = { {9, 9}, {38, 9}, {38, 38}, {9, 38} };

    check(Image2DAlg_HarrisCorners(&in, 5, 3, 0.04, 0.05, 0, &pts, &count) == 0,

          "Harris 返回 0");

    check(count >= 4, "Harris 检出 >= 4 角点");

    int covered = 0;

    for (int c = 0; c < 4 && pts; ++c)

        for (int i = 0; i < count; ++i)

            if (std::hypot(pts[i].x - trueC[c][0], pts[i].y - trueC[c][1]) < 4.0) { ++covered; break; }

    check(covered == 4, "四个真角点均有检出覆盖");

    Image2DAlg_FreePoint2f(pts);



    pts = NULL;

    count = 0;

    check(Image2DAlg_GoodFeatures(&in, 20, 0.05, 4, 0, &pts, &count) == 0 && count >= 2,

          "GoodFeatures 检出 >= 2");

    check(count <= 20, "GoodFeatures 尊重 max_corners");

    Image2DAlg_FreePoint2f(pts);



    pts = NULL;

    count = 0;

    check(Image2DAlg_GoodFeatures(&in, 0, 0.05, 4, 0, &pts, &count) == -3, "max_corners=0 -> -3");

    check(Image2DAlg_HarrisCorners(&in, 5, 3, 0.04, 1.5, 0, &pts, &count) == -3,

          "thresh_ratio>1 -> -3");

    Image2DAlg_Point2f init[2] = { {10.0f, 10.0f}, {20.0f, 20.0f} };

    Image2DAlg_Point2f* refined = NULL;

    // 亚像素细化用硬边图（5x5 模糊图角点弥散，cornerSubPix 会向扩散角心漂移）

    cv::Mat sharp(48, 48, CV_8UC1, cv::Scalar(0));

    cv::rectangle(sharp, cv::Rect(10, 10, 20, 20), cv::Scalar(255), -1);

    Image2DAlg_Image isharp = MakeImage(sharp);

    Image2DAlg_Point2f* refined2 = NULL;

    check(Image2DAlg_CornerSubPix(&isharp, init, 2, 7, &refined2) == 0, "CornerSubPix 返回 0");

    check(refined2 != NULL && std::hypot(refined2[0].x - 10.0, refined2[0].y - 10.0) < 1.0,

          "真角点亚像素细化仍在 (10,10) 附近");

    check(refined2 != NULL && std::hypot(refined2[1].x - 20.0, refined2[1].y - 20.0) < 0.5,

          "平坦区细化保持原位");

    Image2DAlg_FreePoint2f(refined2);

    refined2 = NULL;   // 释放后置空：下面的 -3 路径不写 out 指针，避免二次释放

    check(Image2DAlg_CornerSubPix(&isharp, init, 2, 2, &refined2) == -3, "win_size<3 -> -3");

    check(Image2DAlg_CornerSubPix(&isharp, init, 2, 7, NULL) == -1, "out=NULL -> -1");

    Image2DAlg_FreePoint2f(refined2);

    Image2DAlg_FreePoint2f(NULL);

    check(true, "FreePoint2f NULL 安全");

    Image2DAlg_FreeImage(&isharp);

    Image2DAlg_FreeImage(&in);



    std::printf("== ApproxPolyDP/FitRectsFromBinary ==\n");

    // 12x8 矩形边界逆时针 36 点

    float poly[72];

    int pn = 0;

    for (int x = 0; x < 12; ++x) { poly[pn++] = (float)(5 + x); poly[pn++] = 5; }

    for (int y = 1; y < 8; ++y) { poly[pn++] = 16; poly[pn++] = (float)(5 + y); }

    for (int x = 10; x >= 0; --x) { poly[pn++] = (float)(5 + x); poly[pn++] = 12; }

    for (int y = 6; y >= 1; --y) { poly[pn++] = 5; poly[pn++] = (float)(5 + y); }

    float* out_xy = NULL;

    int out_n = 0;

    check(Image2DAlg_ApproxPolyDP(poly, pn / 2, 2.0, 1, &out_xy, &out_n) == 0 && out_n == 4,

          "矩形轮廓逼近为 4 角点");

    Image2DAlg_FreeContourPoints(out_xy, NULL);

    out_xy = NULL;

    out_n = 0;

    check(Image2DAlg_ApproxPolyDP(poly, pn / 2, 0.0, 1, &out_xy, &out_n) == 0 && out_n == 4,

          "epsilon=0 共线点剔除后 4 角点");

    Image2DAlg_FreeContourPoints(out_xy, NULL);

    check(Image2DAlg_ApproxPolyDP(poly, 2, 2.0, 1, &out_xy, &out_n) == -3, "点数 < 3 -> -3");

    check(Image2DAlg_ApproxPolyDP(poly, pn, -1.0, 1, &out_xy, &out_n) == -3, "epsilon<0 -> -3");



    // 多目标旋转矩形：两方块 12x8 与 12x12

    cv::Mat bin(48, 48, CV_8UC1, cv::Scalar(0));

    cv::rectangle(bin, cv::Rect(4, 4, 12, 8), cv::Scalar(255), -1);

    cv::rectangle(bin, cv::Rect(28, 28, 12, 12), cv::Scalar(255), -1);

    Image2DAlg_Image ib = MakeImage(bin);

    Image2DAlg_RectFit* rects = NULL;

    int nr = 0;

    check(Image2DAlg_FitRectsFromBinary(&ib, 10, 0, &rects, &nr) == 0 && nr == 2,

          "旋转矩形拟合返回 2 个");

    bool hitA = false, hitB = false;

    for (int i = 0; i < nr && rects; ++i) {

        const double mn = std::min(rects[i].width, rects[i].height);

        const double mx = std::max(rects[i].width, rects[i].height);

        if (std::hypot(rects[i].cx - 9.5, rects[i].cy - 7.5) < 1.0 &&

            std::fabs(mn - 8.0) < 1.5 && std::fabs(mx - 12.0) < 1.5) hitA = true;

        if (std::hypot(rects[i].cx - 33.5, rects[i].cy - 33.5) < 1.0 &&

            std::fabs(mn - 12.0) < 1.5 && std::fabs(mx - 12.0) < 1.5) hitB = true;

    }

    check(hitA && hitB, "两矩形中心/边长正确（AA 偏差容差 1.5）");

    Image2DAlg_FreeRectFits(rects);

    rects = NULL;

    nr = 0;

    check(Image2DAlg_FitRectsFromBinary(&ib, 100, 0, &rects, &nr) == 0 && nr == 1,

          "min_area 过滤掉 12x8 小矩形");

    Image2DAlg_FreeRectFits(rects);

    check(Image2DAlg_FitRectsFromBinary(&ib, 10, 0, NULL, &nr) == -1, "out=NULL -> -1");

    Image2DAlg_FreeRectFits(NULL);

    check(true, "FreeRectFits NULL 安全");

    Image2DAlg_FreeImage(&ib);



    std::printf("== ORBAlign ==\n");

    // 富纹理合成图（伪随机高频），B = A 平移 (5,7)

    cv::setRNGSeed(42);   // 种子化随机纹理：特征丰富且确定

    cv::Mat tex(64, 64, CV_8UC1);

    cv::randu(tex, cv::Scalar(40), cv::Scalar(220));

    cv::Mat shifted(64, 64, CV_8UC1, cv::Scalar(40));

    tex(cv::Rect(0, 0, 59, 57)).copyTo(shifted(cv::Rect(5, 7, 59, 57)));

    Image2DAlg_Image i1 = MakeImage(tex);

    Image2DAlg_Image i2 = MakeImage(shifted);

    double m[6] = { 0 };

    Image2DAlg_OrbMatch* om = NULL;

    int nom = 0, ninl = 0;

    cv::setRNGSeed(42);   // RANSAC 可复现（与纹理种子一致）

    check(Image2DAlg_ORBAlign(&i1, &i2, 500, 0.7, m, &om, &nom, &ninl) == 0,

          "ORBAlign 返回 0");

    check(ninl >= 10, "RANSAC 内点 >= 10");

    check(std::fabs(m[2] - 5.0) < 1.5 && std::fabs(m[5] - 7.0) < 1.5 &&

          std::fabs(m[0] - 1.0) < 0.1 && std::fabs(m[4] - 1.0) < 0.1,

          "仿射 ≈ 平移 (5,7)");

    check(om != NULL && om[0].distance >= 0, "匹配点对含 Hamming 距离");

    Image2DAlg_FreeOrbMatches(om);



    // 平坦图 -> 特征不足 -2

    cv::Mat flat(64, 64, CV_8UC1, cv::Scalar(128));

    Image2DAlg_Image ifl = MakeImage(flat);

    Image2DAlg_Image ifl2 = MakeImage(flat);

    cv::setRNGSeed(0);

    check(Image2DAlg_ORBAlign(&ifl, &ifl2, 500, 0.7, m, &om, &nom, &ninl) == -2,

          "平坦图特征不足 -> -2");

    check(Image2DAlg_ORBAlign(&i1, &i2, 0, 0.7, m, &om, &nom, &ninl) == -3, "n_features<=0 -> -3");

    check(Image2DAlg_ORBAlign(&i1, &i2, 500, 1.5, m, &om, &nom, &ninl) == -3, "ratio 越界 -> -3");

    check(Image2DAlg_ORBAlign(&i1, &i2, 500, 0.7, NULL, &om, &nom, &ninl) == -1, "m=NULL -> -1");

    Image2DAlg_FreeOrbMatches(NULL);

    check(true, "FreeOrbMatches NULL 安全");

    Image2DAlg_FreeImage(&ifl);

    Image2DAlg_FreeImage(&ifl2);

    Image2DAlg_FreeImage(&i2);

    Image2DAlg_FreeImage(&i1);

}


// ---------------- 2.1.0 特征链路拆分：FAST / 描述子 / 匹配 / 几何估计 ----------------



static void testFeaturePipeline()

{

    std::printf("== FastFeatures ==\n");

    // 富纹理合成图（与 ORBAlign 测试同一构造）：A 与 B = A 平移 (5,7)

    cv::setRNGSeed(42);

    cv::Mat tex(96, 96, CV_8UC1);

    cv::randu(tex, cv::Scalar(40), cv::Scalar(220));

    cv::Mat shifted(96, 96, CV_8UC1, cv::Scalar(40));

    tex(cv::Rect(0, 0, 91, 89)).copyTo(shifted(cv::Rect(5, 7, 91, 89)));

    Image2DAlg_Image i1 = MakeImage(tex);

    Image2DAlg_Image i2 = MakeImage(shifted);

    Image2DAlg_Point2f* fpts = NULL;

    int nf = 0;

    check(Image2DAlg_FastFeatures(&i1, 20, 1, &fpts, &nf) == 0, "FAST 检测返回 0");

    check(nf > 0 && fpts != NULL, "FAST 检出特征点");

    bool inb = true;

    for (int i = 0; i < nf && fpts; ++i)

        if (fpts[i].x < 0 || fpts[i].y < 0 || fpts[i].x >= 96 || fpts[i].y >= 96) inb = false;

    check(inb, "FAST 点均在图像内");

    // 平坦图：无检出属正常（0 / NULL / 返回 0）

    cv::Mat flat(64, 64, CV_8UC1, cv::Scalar(128));

    Image2DAlg_Image ifl = MakeImage(flat);

    Image2DAlg_Point2f* fpts_f = NULL;

    int nf_f = 0;

    check(Image2DAlg_FastFeatures(&ifl, 20, 1, &fpts_f, &nf_f) == 0 &&
          nf_f == 0 && fpts_f == NULL,

          "平坦图无检出 -> 0/0（非错误）");

    Image2DAlg_FreePoint2f(fpts_f);

    check(Image2DAlg_FastFeatures(&i1, 0, 1, &fpts, &nf) == -3, "threshold<=0 -> -3");

    check(Image2DAlg_FastFeatures(&i1, 20, 1, NULL, &nf) == -1, "out_points=NULL -> -1");

    Image2DAlg_FreePoint2f(NULL);

    check(true, "FreePoint2f NULL 安全");



    std::printf("== ComputeDescriptors ==\n");

    // 取 FAST 点中 16px 边距内的子集（描述子邻域充足），两图同取保持对应

    std::vector<Image2DAlg_Point2f> p1v, p2v;

    for (int i = 0; i < nf && fpts; ++i)

        if (fpts[i].x >= 16 && fpts[i].x < 80 && fpts[i].y >= 16 && fpts[i].y < 80)

            p1v.push_back(fpts[i]);

    Image2DAlg_Point2f* fpts2 = NULL;

    int nf2 = 0;

    check(Image2DAlg_FastFeatures(&i2, 20, 1, &fpts2, &nf2) == 0 && nf2 > 0,

          "图2 FAST 检测返回 0");

    for (int i = 0; i < nf2 && fpts2; ++i)

        if (fpts2[i].x >= 16 && fpts2[i].x < 80 && fpts2[i].y >= 16 && fpts2[i].y < 80)

            p2v.push_back(fpts2[i]);

    Image2DAlg_FreePoint2f(fpts2);

    fpts2 = NULL;

    Image2DAlg_FreePoint2f(fpts);

    fpts = NULL;

    check((int)p1v.size() >= 10 && (int)p2v.size() >= 10, "边距过滤后仍有 >= 10 个点");



    Image2DAlg_Descriptors d1, d2;

    std::memset(&d1, 0, sizeof(d1));

    std::memset(&d2, 0, sizeof(d2));

    check(Image2DAlg_ComputeDescriptors(&i1, p1v.data(), (int)p1v.size(), 0, 500, &d1) == 0,

          "ORB 描述子计算返回 0");

    check(d1.type == 0 && d1.dim == 32 && d1.is_float == 0 && d1.count == (int)p1v.size(),

          "ORB：二进制 32 字节、数量与输入一致");

    check(Image2DAlg_ComputeDescriptors(&i2, p2v.data(), (int)p2v.size(), 0, 500, &d2) == 0,

          "图2 ORB 描述子计算返回 0");

    // 四类描述子枚举与维度（OpenCV 4.4 默认输出长度）

    Image2DAlg_Descriptors db;

    std::memset(&db, 0, sizeof(db));

    check(Image2DAlg_ComputeDescriptors(&i1, p1v.data(), (int)p1v.size(), 1, 0, &db) == 0 &&

          db.type == 1 && db.dim == 64 && db.is_float == 0, "BRISK：二进制 64 字节");

    Image2DAlg_FreeDescriptors(&db);

    std::memset(&db, 0, sizeof(db));

    check(Image2DAlg_ComputeDescriptors(&i1, p1v.data(), (int)p1v.size(), 2, 0, &db) == 0 &&

          db.type == 2 && db.dim == 61 && db.is_float == 0, "AKAZE：二进制 61 字节");

    Image2DAlg_FreeDescriptors(&db);

    std::memset(&db, 0, sizeof(db));

    check(Image2DAlg_ComputeDescriptors(&i1, p1v.data(), (int)p1v.size(), 3, 0, &db) == 0 &&

          db.type == 3 && db.dim == 128 && db.is_float == 1 && db.fdata != NULL &&
          db.data == NULL,

          "SIFT：浮点 128 维（is_float=1）");

    Image2DAlg_FreeDescriptors(&db);

    // 错误路径

    Image2DAlg_Descriptors dob;

    std::memset(&dob, 0, sizeof(dob));

    const Image2DAlg_Point2f oob[2] = { {32, 32}, {200, 200} };

    check(Image2DAlg_ComputeDescriptors(&i1, oob, 2, 0, 500, &dob) == -2, "关键点越界 -> -2");

    // 复用场景回归：已填充的 out 在 -2 出口不得被清零（否则已有 data 指针丢失
    // 即泄漏——校验失败必须不动 out，见头文件契约）
    check(d1.data != NULL && d1.count == (int)p1v.size() && d1.dim == 32,
          "前置：d1 已持有有效缓冲");
    const Image2DAlg_Point2f oob2[1] = { {5.0f, 200.0f} };

    check(Image2DAlg_ComputeDescriptors(&i1, oob2, 1, 0, 500, &d1) == -2,
          "越界 -2 复用同一 out 返回 -2");

    check(d1.data != NULL && d1.count == (int)p1v.size() && d1.dim == 32 &&
          d1.type == 0 && d1.is_float == 0,
          "-2 后 d1 缓冲指针/元数据保持不变");

    // 贴边点回归：扩图后贴边点应正常出描述子（此前贴边点会触发算子丢弃 -> -2
    // 或产生不可复现结果）
    {
        const Image2DAlg_Point2f near_edge[2] = { {2.0f, 2.0f}, {94.0f, 94.0f} };
        Image2DAlg_Descriptors dn;
        std::memset(&dn, 0, sizeof(dn));
        check(Image2DAlg_ComputeDescriptors(&i1, near_edge, 2, 0, 500, &dn) == 0,
              "贴边点 (2,2)/(94,94) ORB 描述子计算返回 0（扩图采样）");
        check(dn.count == 2 && dn.dim == 32 && dn.data != NULL,
              "贴边点描述子数量/维度正确");
        Image2DAlg_FreeDescriptors(&dn);
    }

    const Image2DAlg_Point2f one[1] = { {32, 32} };

    check(Image2DAlg_ComputeDescriptors(&i1, one, 0, 0, 500, &dob) == -3, "count=0 -> -3");

    check(Image2DAlg_ComputeDescriptors(&i1, one, 1, 4, 500, &dob) == -3, "type 越界 -> -3");

    check(Image2DAlg_ComputeDescriptors(&i1, one, 1, 0, 500, NULL) == -1, "out=NULL -> -1");

    check(Image2DAlg_ComputeDescriptors(&i1, NULL, 1, 0, 500, &dob) == -1, "pts=NULL -> -1");

    Image2DAlg_FreeDescriptors(&dob);   // 未填充的结构体（全 NULL）释放安全

    Image2DAlg_FreeDescriptors(NULL);

    check(true, "FreeDescriptors NULL 安全");



    std::printf("== MatchFeatures ==\n");

    Image2DAlg_OrbMatch* fm = NULL;

    int nfm = 0;

    check(Image2DAlg_MatchFeatures(&d1, &d2, p1v.data(), p2v.data(), 0.75, &fm, &nfm) == 0,

          "描述子匹配返回 0");

    check(nfm >= 2 && fm != NULL, "比率测试后 >= 2 对");

    int consistent = 0;

    for (int i = 0; i < nfm && fm; ++i)

        if (std::hypot(fm[i].x1 + 5.0 - fm[i].x2, fm[i].y1 + 7.0 - fm[i].y2) < 1.5)

            ++consistent;

    check(consistent >= 1, "存在 p2 = p1 + (5,7) 的正确匹配");

    check(fm != NULL && fm[0].distance >= 0, "匹配对含描述子距离");

    Image2DAlg_FreeOrbMatches(fm);

    fm = NULL;

    nfm = 0;

    check(Image2DAlg_MatchFeatures(&d1, &d2, p1v.data(), p2v.data(), 0.0, &fm, &nfm) == -3,

          "ratio=0 -> -3");

    check(Image2DAlg_MatchFeatures(&d1, &d2, p1v.data(), p2v.data(), 1.5, &fm, &nfm) == -3,

          "ratio>=1 -> -3");

    // type 不一致（ORB vs SIFT）

    Image2DAlg_Descriptors ds;

    std::memset(&ds, 0, sizeof(ds));

    check(Image2DAlg_ComputeDescriptors(&i1, p1v.data(), (int)p1v.size(), 3, 0, &ds) == 0,

          "SIFT 描述子计算返回 0（匹配用）");

    check(Image2DAlg_MatchFeatures(&d1, &ds, p1v.data(), p2v.data(), 0.75, &fm, &nfm) == -3,

          "type 不一致 -> -3");

    // 一致性校验组合补全：dim 不一致（同 type）/ is_float 不一致（同 type）
    {
        Image2DAlg_Descriptors dx1 = d1, dx2 = d2;
        dx2.dim = 31;   // 人为破坏 dim（其余字段与 dx1 一致）
        check(Image2DAlg_MatchFeatures(&dx1, &dx2, p1v.data(), p2v.data(), 0.75, &fm, &nfm) == -3,
              "dim 不一致 -> -3");
        Image2DAlg_Descriptors dy1 = d1;
        dy1.is_float = 1;   // 人为破坏 is_float（与 dx2 不一致）
        check(Image2DAlg_MatchFeatures(&dy1, &dx2, p1v.data(), p2v.data(), 0.75, &fm, &nfm) == -3,
              "is_float 不一致 -> -3");
        // is_float 一致但无 fdata 缓冲 -> -1
        Image2DAlg_Descriptors dz1 = d1, dz2 = d2;
        dz1.is_float = 1;
        dz2.is_float = 1;
        check(Image2DAlg_MatchFeatures(&dz1, &dz2, p1v.data(), p2v.data(), 0.75, &fm, &nfm) == -1,
              "is_float=1 且 fdata=NULL -> -1");
    }

    Image2DAlg_FreeDescriptors(&ds);

    // 平坦图：描述子无区分性 -> 0 对通过比率测试 -> -2

    Image2DAlg_Descriptors df1, df2;

    std::memset(&df1, 0, sizeof(df1));

    std::memset(&df2, 0, sizeof(df2));

    const Image2DAlg_Point2f fpts3[3] = { {32, 32}, {34, 32}, {32, 34} };

    check(Image2DAlg_ComputeDescriptors(&ifl, fpts3, 3, 0, 500, &df1) == 0,

          "平坦图 ORB 描述子计算返回 0");

    check(Image2DAlg_ComputeDescriptors(&ifl, fpts3, 3, 0, 500, &df2) == 0,

          "平坦图第二套描述子返回 0");

    check(Image2DAlg_MatchFeatures(&df1, &df2, fpts3, fpts3, 0.75, &fm, &nfm) == -2,

          "平坦图无区分性匹配 -> -2");

    Image2DAlg_FreeDescriptors(&df1);

    Image2DAlg_FreeDescriptors(&df2);

    check(Image2DAlg_MatchFeatures(NULL, &d2, p1v.data(), p2v.data(), 0.75, &fm, &nfm) == -1,

          "d1=NULL -> -1");

    check(Image2DAlg_MatchFeatures(&d1, &d2, p1v.data(), p2v.data(), 0.75, NULL, &nfm) == -1,

          "out=NULL -> -1");



    std::printf("== 链路：匹配 -> 部分仿射/单应（合成平移真值） ==\n");

    check(Image2DAlg_MatchFeatures(&d1, &d2, p1v.data(), p2v.data(), 0.75, &fm, &nfm) == 0 &&
          nfm >= 2, "重新匹配供几何估计");

    cv::setRNGSeed(42);   // RANSAC 可复现

    double m6[6] = { 0 };

    int ninl = 0;

    check(Image2DAlg_EstimateAffinePartial(fm, nfm, 3.0, m6, &ninl) == 0,

          "部分仿射估计返回 0");

    check(std::fabs(m6[0] - 1.0) < 0.1 && std::fabs(m6[4] - 1.0) < 0.1 &&
          std::fabs(m6[1]) < 0.1 && std::fabs(m6[3]) < 0.1 &&
          std::fabs(m6[2] - 5.0) < 1.0 && std::fabs(m6[5] - 7.0) < 1.0,

          "部分仿射 ≈ 平移 (5,7)");

    check(ninl >= 3, "部分仿射内点 >= 3");

    cv::setRNGSeed(42);

    double h9[9] = { 0 };

    check(Image2DAlg_FindHomography(fm, nfm, 3.0, h9, &ninl) == 0, "单应估计返回 0");

    const double hs = h9[8] != 0.0 ? h9[8] : 1.0;   // 归一化防御

    check(std::fabs(h9[0] / hs - 1.0) < 0.05 && std::fabs(h9[2] / hs - 5.0) < 1.0 &&
          std::fabs(h9[4] / hs - 1.0) < 0.05 && std::fabs(h9[5] / hs - 7.0) < 1.0,

          "单应 ≈ 平移 (5,7)");

    check(ninl >= 3, "单应内点 >= 3");

    Image2DAlg_FreeOrbMatches(fm);

    fm = NULL;

    nfm = 0;



    std::printf("== EstimateAffinePartial/FindHomography 已知变换正解 ==\n");

    // 相似变换真值：p2 = 1.2·R(15°)·p1 + (3,4)，5x4 网格点精确满足

    const double rad = 15.0 * 3.14159265358979323846 / 180.0;

    const double ca = 1.2 * std::cos(rad), sa = 1.2 * std::sin(rad);

    std::vector<Image2DAlg_OrbMatch> sim;

    for (int i = 0; i < 20; ++i) {

        const double x = (i % 5) * 10.0 - 20.0;

        const double y = (i / 5) * 8.0 - 12.0;

        Image2DAlg_OrbMatch mm;

        mm.x1 = x;

        mm.y1 = y;

        mm.x2 = ca * x - sa * y + 3.0;

        mm.y2 = sa * x + ca * y + 4.0;

        mm.distance = 0;

        sim.push_back(mm);

    }

    cv::setRNGSeed(42);

    check(Image2DAlg_EstimateAffinePartial(sim.data(), 20, 3.0, m6, &ninl) == 0,

          "已知相似变换估计返回 0");

    check(std::fabs(m6[0] - ca) < 1e-4 && std::fabs(m6[1] + sa) < 1e-4 &&
          std::fabs(m6[2] - 3.0) < 1e-4 && std::fabs(m6[3] - sa) < 1e-4 &&
          std::fabs(m6[4] - ca) < 1e-4 && std::fabs(m6[5] - 4.0) < 1e-4,

          "恢复 1.2·R(15°) + (3,4)");

    check(ninl == 20, "精确数据内点 = 20");

    // 单应真值：p2 = H·p1，H = [[1.01,0.02,5],[0.01,1.03,7],[0.001,0.0005,1]]

    std::vector<Image2DAlg_OrbMatch> homo;

    for (int i = 0; i < 20; ++i) {

        const double x = (i % 5) * 10.0 - 20.0;

        const double y = (i / 5) * 8.0 - 12.0;

        const double w = 0.001 * x + 0.0005 * y + 1.0;

        Image2DAlg_OrbMatch mm;

        mm.x1 = x;

        mm.y1 = y;

        mm.x2 = (1.01 * x + 0.02 * y + 5.0) / w;

        mm.y2 = (0.01 * x + 1.03 * y + 7.0) / w;

        mm.distance = 0;

        homo.push_back(mm);

    }

    cv::setRNGSeed(42);

    check(Image2DAlg_FindHomography(homo.data(), 20, 3.0, h9, &ninl) == 0,

          "已知单应估计返回 0");

    check(std::fabs(h9[0] - 1.01) < 1e-4 && std::fabs(h9[2] - 5.0) < 1e-4 &&
          std::fabs(h9[4] - 1.03) < 1e-4 && std::fabs(h9[5] - 7.0) < 1e-4 &&
          std::fabs(h9[8] - 1.0) < 1e-6,

          "恢复投影单应矩阵");

    check(ninl == 20, "精确数据内点 = 20");

    // 错误路径

    check(Image2DAlg_EstimateAffinePartial(sim.data(), 2, 3.0, m6, &ninl) == -2, "count<3 -> -2");

    check(Image2DAlg_EstimateAffinePartial(sim.data(), 20, 0.0, m6, &ninl) == -3,

          "threshold<=0 -> -3");

    check(Image2DAlg_EstimateAffinePartial(NULL, 20, 3.0, m6, &ninl) == -1, "matches=NULL -> -1");

    check(Image2DAlg_EstimateAffinePartial(sim.data(), 20, 3.0, NULL, &ninl) == -1, "m=NULL -> -1");

    check(Image2DAlg_FindHomography(homo.data(), 3, 3.0, h9, &ninl) == -2, "count<4 -> -2");

    check(Image2DAlg_FindHomography(homo.data(), 20, -1.0, h9, &ninl) == -3,

          "threshold<=0 -> -3");

    check(Image2DAlg_FindHomography(NULL, 20, 3.0, h9, &ninl) == -1, "matches=NULL -> -1");

    // 共线退化：所有点在同一直线上 -> 单应无法求解

    std::vector<Image2DAlg_OrbMatch> coll;

    for (int i = 0; i < 12; ++i) {

        Image2DAlg_OrbMatch mm;

        mm.x1 = (double)(i * 4);

        mm.y1 = 5.0;

        mm.x2 = (double)(i * 4) + 2.0;

        mm.y2 = 6.0;

        mm.distance = 0;

        coll.push_back(mm);

    }

    cv::setRNGSeed(42);

    check(Image2DAlg_FindHomography(coll.data(), 12, 3.0, h9, &ninl) == -2, "共线退化 -> -2");

    Image2DAlg_FreeOrbMatches(NULL);

    check(true, "FreeOrbMatches NULL 安全");

    Image2DAlg_FreeDescriptors(&d1);

    Image2DAlg_FreeDescriptors(&d2);

    Image2DAlg_FreeImage(&ifl);

    Image2DAlg_FreeImage(&i2);

    Image2DAlg_FreeImage(&i1);

}





// ---------------- 2.2.0 批次：P1 特征扩展（检测器 / 斑点 / 一站式匹配） ----------------

static void testFeatureExtensions()

{

    std::printf("== BriskFeatures / AkazeFeatures / SiftFeatures ==\n");

    // 富纹理合成图（96x96，值域 40..220）与平移 (5,7) 副本（同 testFeaturePipeline 构造）

    cv::setRNGSeed(42);

    cv::Mat tex(96, 96, CV_8UC1);

    cv::randu(tex, cv::Scalar(40), cv::Scalar(220));

    cv::Mat shifted(96, 96, CV_8UC1, cv::Scalar(40));

    tex(cv::Rect(0, 0, 91, 89)).copyTo(shifted(cv::Rect(5, 7, 91, 89)));

    Image2DAlg_Image i1 = MakeImage(tex);

    Image2DAlg_Image i2 = MakeImage(shifted);

    cv::Mat flat(64, 64, CV_8UC1, cv::Scalar(128));

    Image2DAlg_Image ifl = MakeImage(flat);

    Image2DAlg_Point2f* fpts = NULL;

    int fc = 0;

    Image2DAlg_Descriptors fd;

    std::memset(&fd, 0, sizeof(fd));

    check(Image2DAlg_BriskFeatures(&i1, 500, 0, &fpts, &fc, &fd) == 0, "BRISK 检测+描述返回 0（contrast<=0 取默认）");

    check(fc > 0 && fpts != NULL, "BRISK 检出特征点");

    check(fd.type == 1 && fd.dim == 64 && fd.is_float == 0 && fd.count == fc && fd.data != NULL,

          "BRISK 描述子：二进制 64 字节、数量与点数一致");

    bool inb = true;

    for (int i = 0; i < fc && fpts; ++i)

        if (fpts[i].x < 0 || fpts[i].y < 0 || fpts[i].x >= 96 || fpts[i].y >= 96) inb = false;

    check(inb, "BRISK 点均在图像内");

    Image2DAlg_FreePoint2f(fpts);

    fpts = NULL;

    fc = 0;

    Image2DAlg_FreeDescriptors(&fd);

    std::memset(&fd, 0, sizeof(fd));

    check(Image2DAlg_BriskFeatures(&i1, 500, 40, &fpts, &fc, &fd) == 0 && fc > 0,

          "BRISK contrast_threshold=40（映射 thresh，仍有点）");

    Image2DAlg_FreePoint2f(fpts);

    fpts = NULL;

    fc = 0;

    Image2DAlg_FreeDescriptors(&fd);

    std::memset(&fd, 0, sizeof(fd));

    // AKAZE 非线性扩散会平滑 iid 噪声（randu 图仅 2 点）——改用 4px 马赛克结构纹理

    cv::setRNGSeed(4);

    cv::Mat mos(128, 128, CV_8UC1);

    for (int my = 0; my < 128; my += 4)

        for (int mx = 0; mx < 128; mx += 4) {

            const int shade = 20 + cv::theRNG().uniform(0, 214);

            mos(cv::Rect(mx, my, 4, 4)).setTo(cv::Scalar(shade));

        }

    Image2DAlg_Image imos = MakeImage(mos);

    check(Image2DAlg_AkazeFeatures(&imos, 0, 0, &fpts, &fc, &fd) == 0, "AKAZE 检测+描述返回 0（contrast<=0 取默认）");

    check(fc > 0 && fpts != NULL, "AKAZE 检出特征点");

    check(fd.type == 2 && fd.dim == 61 && fd.is_float == 0 && fd.count == fc && fd.data != NULL,

          "AKAZE 描述子：二进制 61 字节、数量与点数一致");

    Image2DAlg_FreePoint2f(fpts);

    fpts = NULL;

    fc = 0;

    Image2DAlg_FreeDescriptors(&fd);

    std::memset(&fd, 0, sizeof(fd));

    check(Image2DAlg_AkazeFeatures(&imos, 0, 0.005, &fpts, &fc, &fd) == 0 && fc > 0,

          "AKAZE contrast_threshold=0.005（映射 threshold，仍有点）");

    Image2DAlg_FreePoint2f(fpts);

    fpts = NULL;

    fc = 0;

    Image2DAlg_FreeDescriptors(&fd);

    std::memset(&fd, 0, sizeof(fd));

    check(Image2DAlg_SiftFeatures(&i1, 300, 0, &fpts, &fc, &fd) == 0, "SIFT 检测+描述返回 0（contrast<=0 取默认）");

    check(fc > 0 && fpts != NULL, "SIFT 检出特征点");

    check(fd.type == 3 && fd.dim == 128 && fd.is_float == 1 && fd.count == fc &&

          fd.fdata != NULL && fd.data == NULL,

          "SIFT 描述子：浮点 128 维（is_float=1）、数量与点数一致");

    Image2DAlg_FreePoint2f(fpts);

    fpts = NULL;

    fc = 0;

    Image2DAlg_FreeDescriptors(&fd);

    std::memset(&fd, 0, sizeof(fd));

    check(Image2DAlg_SiftFeatures(&i1, 0, 0.1, &fpts, &fc, &fd) == 0 && fc > 0,

          "SIFT contrast_threshold=0.1（映射 contrastThreshold，仍有点）");

    Image2DAlg_FreePoint2f(fpts);

    fpts = NULL;

    fc = 0;

    Image2DAlg_FreeDescriptors(&fd);

    std::memset(&fd, 0, sizeof(fd));

    // out_desc = NULL 只检测（便捷用法）；平坦图无检出属正常

    check(Image2DAlg_BriskFeatures(&i1, 500, 0, &fpts, &fc, NULL) == 0 && fc > 0 && fpts != NULL,

          "out_desc=NULL 只检测不取描述子");

    Image2DAlg_FreePoint2f(fpts);

    fpts = NULL;

    fc = 0;

    check(Image2DAlg_BriskFeatures(&ifl, 500, 0, &fpts, &fc, &fd) == 0 && fc == 0 &&

          fpts == NULL && fd.count == 0 && fd.data == NULL,

          "平坦图无检出 -> 0/0（描述子同样为空）");

    check(Image2DAlg_BriskFeatures(NULL, 500, 0, &fpts, &fc, &fd) == -1, "in=NULL -> -1");

    check(Image2DAlg_BriskFeatures(&i1, 500, 0, NULL, &fc, &fd) == -1, "out_points=NULL -> -1");

    check(Image2DAlg_BriskFeatures(&i1, 500, 0, &fpts, NULL, &fd) == -1, "out_count=NULL -> -1");

    check(Image2DAlg_SiftFeatures(&i1, 500, 0, &fpts, NULL, &fd) == -1, "SIFT out_count=NULL -> -1");

    Image2DAlg_FreeDescriptors(&fd);

    Image2DAlg_FreeDescriptors(NULL);

    check(true, "FreeDescriptors NULL 安全（检测器族）");

    Image2DAlg_FreeImage(&imos);



    std::printf("== BlobDetect ==\n");

    // 亮背景（240）暗斑（30）：SimpleBlobDetector 默认 blobColor=0 检测暗斑

    cv::Mat bg(128, 128, CV_8UC1, cv::Scalar(240));

    cv::circle(bg, cv::Point(30, 30), 6, cv::Scalar(30), -1);

    cv::circle(bg, cv::Point(80, 40), 8, cv::Scalar(30), -1);

    cv::circle(bg, cv::Point(60, 100), 10, cv::Scalar(30), -1);

    Image2DAlg_Image ib = MakeImage(bg);

    Image2DAlg_BlobFeature* blobs = NULL;

    int nb = 0;

    check(Image2DAlg_BlobDetect(&ib, 50, 2000, 0.5, 0.5, &blobs, &nb) == 0 && nb == 3,

          "BlobDetect 返回 0 且检出 3 个已知圆");

    const double tctr[3][2] = { {30, 30}, {80, 40}, {60, 100} };

    const double trad[3] = { 6, 8, 10 };

    bool blob_ok = (nb == 3);

    if (nb == 3) {

        for (int i = 0; i < 3; ++i) {

            int best = -1;

            double bd = 1e9;

            for (int k = 0; k < 3; ++k) {

                const double d = std::hypot(blobs[i].x - tctr[k][0], blobs[i].y - tctr[k][1]);

                if (d < bd) { bd = d; best = k; }

            }

            if (best < 0 || bd > 2.0 || std::fabs(blobs[i].radius - trad[best]) > 2.0) blob_ok = false;

        }

    }

    check(blob_ok, "斑点中心误差 < 2px、半径误差 < 2px");

    // 面积过滤生效：r6 圆约 113px、r8 约 201px、r10 约 314px

    Image2DAlg_FreeBlobFeatures(blobs);

    blobs = NULL;

    nb = 0;

    check(Image2DAlg_BlobDetect(&ib, 250, 0, 0, 0, &blobs, &nb) == 0 && nb == 1 &&

          std::fabs(blobs[0].radius - 10.0) <= 2.0,

          "min_area=250 过滤后只剩半径 10 的圆");

    Image2DAlg_FreeBlobFeatures(blobs);

    blobs = NULL;

    nb = 0;

    check(Image2DAlg_BlobDetect(&ib, 50, 150, 0, 0, &blobs, &nb) == 0 && nb == 1 &&

          std::fabs(blobs[0].radius - 6.0) <= 2.0,

          "max_area=150 过滤后只剩半径 6 的圆");

    Image2DAlg_FreeBlobFeatures(blobs);

    blobs = NULL;

    nb = 0;

    check(Image2DAlg_BlobDetect(&ifl, 50, 2000, 0.5, 0.5, &blobs, &nb) == 0 && nb == 0 && blobs == NULL,

          "平坦图无斑点 -> 0/0");

    check(Image2DAlg_BlobDetect(NULL, 50, 2000, 0.5, 0.5, &blobs, &nb) == -1, "in=NULL -> -1");

    check(Image2DAlg_BlobDetect(&ib, 50, 2000, 0.5, 0.5, NULL, &nb) == -1, "out_blobs=NULL -> -1");

    Image2DAlg_FreeBlobFeatures(NULL);

    check(true, "FreeBlobFeatures NULL 安全");

    Image2DAlg_FreeImage(&ib);



    std::printf("== DetectAndMatch（一站式检测+描述+匹配） ==\n");

    // 已知平移 (5,7)：四种类型均应恢复大量正确点对

    Image2DAlg_OrbMatch* dm = NULL;

    int ndm = 0;

    check(Image2DAlg_DetectAndMatch(&i1, &i2, 0, 500, 0.75, &dm, &ndm) == 0 && ndm > 10,

          "DetectAndMatch ORB 匹配对 > 10");

    int consistent = 0;

    for (int i = 0; i < ndm && dm; ++i)

        if (std::hypot(dm[i].x1 + 5.0 - dm[i].x2, dm[i].y1 + 7.0 - dm[i].y2) < 1.5) ++consistent;

    check(consistent * 2 >= ndm && consistent >= 5, "DetectAndMatch ORB 过半点对满足平移 (5,7)");

    Image2DAlg_FreeOrbMatches(dm);

    dm = NULL;

    ndm = 0;

    check(Image2DAlg_DetectAndMatch(&i1, &i2, 1, 500, 0.75, &dm, &ndm) == 0 && ndm > 10,

          "DetectAndMatch BRISK 匹配对 > 10");

    consistent = 0;

    for (int i = 0; i < ndm && dm; ++i)

        if (std::hypot(dm[i].x1 + 5.0 - dm[i].x2, dm[i].y1 + 7.0 - dm[i].y2) < 1.5) ++consistent;

    check(consistent * 2 >= ndm && consistent >= 5, "DetectAndMatch BRISK 过半点对满足平移 (5,7)");

    Image2DAlg_FreeOrbMatches(dm);

    dm = NULL;

    ndm = 0;

    // AKAZE 在 iid 噪声上检出稀少——马赛克纹理（同上，平移 (5,7)）

    cv::Mat mosh(128, 128, CV_8UC1, cv::Scalar(128));

    mos(cv::Rect(0, 0, 123, 121)).copyTo(mosh(cv::Rect(5, 7, 123, 121)));

    Image2DAlg_Image imos1 = MakeImage(mos);

    Image2DAlg_Image imos2 = MakeImage(mosh);

    check(Image2DAlg_DetectAndMatch(&imos1, &imos2, 2, 500, 0.75, &dm, &ndm) == 0 && ndm > 10,

          "DetectAndMatch AKAZE 匹配对 > 10");

    consistent = 0;

    for (int i = 0; i < ndm && dm; ++i)

        if (std::hypot(dm[i].x1 + 5.0 - dm[i].x2, dm[i].y1 + 7.0 - dm[i].y2) < 1.5) ++consistent;

    check(consistent * 2 >= ndm && consistent >= 5, "DetectAndMatch AKAZE 过半点对满足平移 (5,7)");

    Image2DAlg_FreeImage(&imos2);

    Image2DAlg_FreeImage(&imos1);

    Image2DAlg_FreeOrbMatches(dm);

    dm = NULL;

    ndm = 0;

    check(Image2DAlg_DetectAndMatch(&i1, &i2, 3, 500, 0.75, &dm, &ndm) == 0 && ndm > 10,

          "DetectAndMatch SIFT 匹配对 > 10");

    consistent = 0;

    for (int i = 0; i < ndm && dm; ++i)

        if (std::hypot(dm[i].x1 + 5.0 - dm[i].x2, dm[i].y1 + 7.0 - dm[i].y2) < 1.5) ++consistent;

    check(consistent * 2 >= ndm && consistent >= 5, "DetectAndMatch SIFT 过半点对满足平移 (5,7)");

    Image2DAlg_FreeOrbMatches(dm);

    dm = NULL;

    ndm = 0;

    // 错误路径

    check(Image2DAlg_DetectAndMatch(&i1, &i2, 4, 500, 0.75, &dm, &ndm) == -3, "type 越界 -> -3");

    check(Image2DAlg_DetectAndMatch(&i1, &i2, 0, 500, 0.0, &dm, &ndm) == -3, "ratio=0 -> -3");

    check(Image2DAlg_DetectAndMatch(&i1, &i2, 0, 500, 1.5, &dm, &ndm) == -3, "ratio>=1 -> -3");

    check(Image2DAlg_DetectAndMatch(NULL, &i2, 0, 500, 0.75, &dm, &ndm) == -1, "in1=NULL -> -1");

    check(Image2DAlg_DetectAndMatch(&i1, &i2, 0, 500, 0.75, NULL, &ndm) == -1, "out=NULL -> -1");

    check(Image2DAlg_DetectAndMatch(&ifl, &ifl, 0, 500, 0.75, &dm, &ndm) == -2, "平坦图无特征 -> -2");

    Image2DAlg_FreeOrbMatches(dm);

    Image2DAlg_FreeOrbMatches(NULL);

    check(true, "FreeOrbMatches NULL 安全（DetectAndMatch）");

    Image2DAlg_FreeImage(&ifl);

    Image2DAlg_FreeImage(&i2);

    Image2DAlg_FreeImage(&i1);

}




// ---------------- 2.2.0 批次：P2 跟踪 / 配准 / 立体 / 位姿 ----------------

static void testTrackRegisterStereo()

{

    std::printf("== CalcOpticalFlowPyrLK ==\n");

    // 前后帧 = 同一纹理平移 (3,2)，点网格留 16px 边距（win 21 窗口完整落在图内）

    cv::setRNGSeed(7);

    cv::Mat tex(96, 96, CV_8UC1);

    cv::randu(tex, cv::Scalar(40), cv::Scalar(220));

    cv::Mat moved(96, 96, CV_8UC1, cv::Scalar(40));

    tex(cv::Rect(0, 0, 93, 94)).copyTo(moved(cv::Rect(3, 2, 93, 94)));

    Image2DAlg_Image ip = MakeImage(tex);

    Image2DAlg_Image iq = MakeImage(moved);

    std::vector<Image2DAlg_Point2f> tin;

    for (int y = 16; y <= 80; y += 16)

        for (int x = 16; x <= 80; x += 16) {

            Image2DAlg_Point2f pt;

            pt.x = (float)x;

            pt.y = (float)y;

            tin.push_back(pt);

        }

    check((int)tin.size() == 25, "LK 跟踪点网格 25 点");

    Image2DAlg_Point2f* np = NULL;

    unsigned char status[25];

    std::memset(status, 0xAA, sizeof(status));

    check(Image2DAlg_CalcOpticalFlowPyrLK(&ip, &iq, tin.data(), 25, 21, 3, &np, status) == 0,

          "LK 光流跟踪返回 0");

    bool st_ok = true;

    bool disp_ok = true;

    for (int i = 0; i < 25 && np; ++i) {

        if (status[i] == 0) st_ok = false;

        if (std::fabs(np[i].x - (tin[i].x + 3.0)) > 1.0 ||

            std::fabs(np[i].y - (tin[i].y + 2.0)) > 1.0) disp_ok = false;

    }

    check(st_ok, "LK status 全部非零");

    check(disp_ok, "LK 跟踪位移 ≈ (3,2)");

    Image2DAlg_FreePoint2f(np);

    np = NULL;

    check(Image2DAlg_CalcOpticalFlowPyrLK(&ip, &iq, tin.data(), 25, 20, 3, &np, status) == 0 && np != NULL,

          "win_size=20 偶数自动补奇返回 0");

    Image2DAlg_FreePoint2f(np);

    np = NULL;

    check(Image2DAlg_CalcOpticalFlowPyrLK(&ip, &iq, tin.data(), 25, 0, 3, &np, status) == -3, "win_size<=0 -> -3");

    check(Image2DAlg_CalcOpticalFlowPyrLK(&ip, &iq, tin.data(), 25, 21, -1, &np, status) == -3, "max_level<0 -> -3");

    check(Image2DAlg_CalcOpticalFlowPyrLK(&ip, &iq, tin.data(), 0, 21, 3, &np, status) == -3, "count=0 -> -3");

    check(Image2DAlg_CalcOpticalFlowPyrLK(&ip, &iq, tin.data(), 25, 21, 3, &np, NULL) == -1, "out_status=NULL -> -1");

    check(Image2DAlg_CalcOpticalFlowPyrLK(&ip, &iq, NULL, 25, 21, 3, &np, status) == -1, "pts=NULL -> -1");

    Image2DAlg_FreePoint2f(NULL);

    check(true, "FreePoint2f NULL 安全（LK）");

    Image2DAlg_FreeImage(&iq);

    Image2DAlg_FreeImage(&ip);



    std::printf("== FindTransformECC ==\n");

    cv::setRNGSeed(11);

    cv::Mat etex(120, 120, CV_8UC1);

    cv::randu(etex, cv::Scalar(40), cv::Scalar(220));

    Image2DAlg_Image et = MakeImage(etex);

    double em[9] = { 0 };

    double corr = 0.0;

    // 平移：input = templ 内容平移 (4,3)（warpAffine 前向 + 复制边框避免黑边）。

    // 注：cv 4.4 的 ECC 在 iid 噪声图对上直接抛不收敛异常（旋转用例可收敛），

    // 平移用例改用高斯模糊结构纹理对（模板与输入同纹理；原生核验 corr≈0.999、

    // 恢复 (4,3)）

    cv::Mat etexb;

    cv::GaussianBlur(etex, etexb, cv::Size(9, 9), 2.0);

    Image2DAlg_Image etb = MakeImage(etexb);

    cv::Mat tin2(120, 120, CV_8UC1);

    cv::Mat T = (cv::Mat_<double>(2, 3) << 1, 0, 4, 0, 1, 3);

    cv::warpAffine(etexb, tin2, T, tin2.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);

    Image2DAlg_Image ein = MakeImage(tin2);

    em[6] = em[7] = em[8] = 99.0;   // 2x3 出口应清零（回归：防误按 3x3 读残留）

    check(Image2DAlg_FindTransformECC(&etb, &ein, 0, 200, 1e-5, em, &corr) == 0, "ECC 平移对齐返回 0");

    check(std::fabs(em[2] - 4.0) < 0.5 && std::fabs(em[5] - 3.0) < 0.5 &&

          std::fabs(em[0] - 1.0) < 0.05 && std::fabs(em[4] - 1.0) < 0.05,

          "ECC 恢复平移 (4,3)");

    check(em[6] == 0.0 && em[7] == 0.0 && em[8] == 0.0, "ECC 2x3 出口 m[6..8] 清零");

    check(corr > 0.9, "ECC 平移相关系数 > 0.9");

    // 仿射：input = templ 内容旋转 2° + 平移 (2,1)，恢复前向矩阵

    cv::Mat ain(120, 120, CV_8UC1);

    const double rad2 = 2.0 * 3.14159265358979323846 / 180.0;

    cv::Mat A = (cv::Mat_<double>(2, 3) << std::cos(rad2), -std::sin(rad2), 2,

                                           std::sin(rad2), std::cos(rad2), 1);

    cv::warpAffine(etex, ain, A, ain.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);

    Image2DAlg_Image eain = MakeImage(ain);

    std::memset(em, 0, sizeof(em));

    corr = 0.0;

    check(Image2DAlg_FindTransformECC(&et, &eain, 1, 200, 1e-5, em, &corr) == 0, "ECC 仿射对齐返回 0");

    check(std::fabs(em[0] - std::cos(rad2)) < 0.03 && std::fabs(em[1] + std::sin(rad2)) < 0.03 &&

          std::fabs(em[3] - std::sin(rad2)) < 0.03 && std::fabs(em[4] - std::cos(rad2)) < 0.03,

          "ECC 恢复 2° 旋转");

    check(corr > 0.9, "ECC 仿射相关系数 > 0.9");

    // 单应：相同图 -> 恒等矩阵、相关系数 ≈ 1

    Image2DAlg_Image esame = MakeImage(etex);

    std::memset(em, 0, sizeof(em));

    corr = 0.0;

    check(Image2DAlg_FindTransformECC(&et, &esame, 2, 200, 1e-5, em, &corr) == 0, "ECC 单应对齐返回 0");

    check(std::fabs(em[0] - 1.0) < 1e-4 && std::fabs(em[1]) < 1e-4 && std::fabs(em[2]) < 1e-4 &&

          std::fabs(em[3]) < 1e-4 && std::fabs(em[4] - 1.0) < 1e-4 && std::fabs(em[5]) < 1e-4 &&

          std::fabs(em[6]) < 1e-4 && std::fabs(em[7]) < 1e-4 && std::fabs(em[8] - 1.0) < 1e-4,

          "ECC 相同图恢复恒等单应");

    check(corr > 0.99, "ECC 相同图相关系数 ≈ 1");

    // 不收敛：平坦输入无结构 -> -2；错误路径

    cv::Mat eflat(120, 120, CV_8UC1, cv::Scalar(128));

    Image2DAlg_Image efl = MakeImage(eflat);

    check(Image2DAlg_FindTransformECC(&et, &efl, 0, 200, 1e-5, em, &corr) == -2, "ECC 平坦图不收敛 -> -2");

    check(Image2DAlg_FindTransformECC(&et, &ein, 3, 200, 1e-5, em, &corr) == -3, "motion_type 越界 -> -3");

    check(Image2DAlg_FindTransformECC(&et, &ein, 0, 0, 1e-5, em, &corr) == -3, "max_iters<1 -> -3");

    check(Image2DAlg_FindTransformECC(&et, &ein, 0, 200, 0.0, em, &corr) == -3, "epsilon<=0 -> -3");

    check(Image2DAlg_FindTransformECC(&et, &ein, 0, 200, 1e-5, NULL, &corr) == -1, "m=NULL -> -1");

    check(Image2DAlg_FindTransformECC(&et, &ein, 0, 200, 1e-5, em, NULL) == -1, "correlation=NULL -> -1");

    Image2DAlg_FreeImage(&efl);

    Image2DAlg_FreeImage(&esame);

    Image2DAlg_FreeImage(&eain);

    Image2DAlg_FreeImage(&ein);

    Image2DAlg_FreeImage(&etb);

    Image2DAlg_FreeImage(&et);



    std::printf("== PhaseCorrelate ==\n");

    cv::setRNGSeed(13);

    cv::Mat ptex(128, 128, CV_8UC1);

    cv::randu(ptex, cv::Scalar(40), cv::Scalar(220));

    cv::Mat psh(128, 128, CV_8UC1, cv::Scalar(40));

    ptex(cv::Rect(0, 0, 122, 124)).copyTo(psh(cv::Rect(6, 4, 122, 124)));

    Image2DAlg_Image pa = MakeImage(ptex);

    Image2DAlg_Image pb = MakeImage(psh);

    double pdx = 0, pdy = 0, presp = 0;

    check(Image2DAlg_PhaseCorrelate(&pa, &pb, &pdx, &pdy, &presp) == 0, "相位相关返回 0");

    check(std::fabs(pdx - 6.0) < 0.5 && std::fabs(pdy - 4.0) < 0.5,

          "相位相关平移 ≈ (6,4)");

    check(presp > 0.1, "相位相关峰值响应 > 0.1");

    cv::Mat psmall(96, 96, CV_8UC1, cv::Scalar(0));

    Image2DAlg_Image ps = MakeImage(psmall);

    check(Image2DAlg_PhaseCorrelate(&pa, &ps, &pdx, &pdy, &presp) == -3, "尺寸不等 -> -3");

    check(Image2DAlg_PhaseCorrelate(&pa, &pb, NULL, &pdy, &presp) == -1, "out_dx=NULL -> -1");

    Image2DAlg_FreeImage(&ps);

    Image2DAlg_FreeImage(&pb);

    Image2DAlg_FreeImage(&pa);



    std::printf("== StereoMatch ==\n");

    // 左右图 = 同一纹理水平错位 10px（视差约定 xL - xR = 10，右图取 stex(+10)），

    // 两图全宽有内容；采样区取 x >= num_disparities 的深部条带

    //（SGBM 搜索窗随视差数越出右图边界，左条带视差不可靠，见头文件注释）

    cv::setRNGSeed(17);

    cv::Mat stex(120, 210, CV_8UC1);

    cv::randu(stex, cv::Scalar(40), cv::Scalar(220));

    cv::Mat sleft = stex(cv::Rect(0, 0, 200, 120)).clone();

    cv::Mat sright = stex(cv::Rect(10, 0, 200, 120)).clone();

    Image2DAlg_Image sl = MakeImage(sleft);

    Image2DAlg_Image sr = MakeImage(sright);

    Image2DAlg_Image disp;

    disp.width = 200;

    disp.height = 120;

    disp.channels = 1;

    disp.data = (unsigned char*)std::malloc((size_t)200 * 120);

    std::memset(disp.data, 0, (size_t)200 * 120);

    check(Image2DAlg_StereoMatch(&sl, &sr, 0, 96, 5, &disp) == 0, "SGBM 立体匹配返回 0");

    int good_px = 0, total_px = 0;

    for (int y = 20; y < 100; y += 2)

        for (int x = 120; x < 190; x += 2) {

            ++total_px;

            const int v = disp.data[(size_t)y * 200 + x];

            if (v >= 9 && v <= 11) ++good_px;

        }

    check(total_px > 0 && good_px * 20 >= total_px * 19, "SGBM 深部采样区 >= 95% 像素视差 ≈ 10（方向为正）");

    // 无效视差 -> 0 回归：min_disparity=2 时 SGBM 的无效标记 (minD-1)*16 = 16
    // 换算后是 1 而非负数，旧实现靠负数钳位兜不住——左条带会以 1 的合法假象漏出
    {
        Image2DAlg_Image disp2;
        disp2.width = 200;
        disp2.height = 120;
        disp2.channels = 1;
        disp2.data = (unsigned char*)std::malloc((size_t)200 * 120);
        std::memset(disp2.data, 0xAB, (size_t)200 * 120);
        check(Image2DAlg_StereoMatch(&sl, &sr, 2, 96, 5, &disp2) == 0,
              "min_disparity=2 立体匹配返回 0");
        int strip_bad = 0, deep_ok = 0, deep_total = 0;
        for (int y = 20; y < 100; y += 2) {
            for (int x = 4; x < 40; x += 2)
                if (disp2.data[(size_t)y * 200 + x] != 0) ++strip_bad;
            for (int x = 120; x < 190; x += 2) {
                ++deep_total;
                const int v = disp2.data[(size_t)y * 200 + x];
                if (v >= 9 && v <= 11) ++deep_ok;
            }
        }
        check(strip_bad == 0, "min_disparity=2 左条带无效视差 -> 0");
        check(deep_total > 0 && deep_ok * 10 >= deep_total * 9,
              "min_disparity=2 深部视差 ≈ 10 仍正确");
        Image2DAlg_FreeImage(&disp2);
    }

    // 错误路径：num_disparities 与容器校验

    check(Image2DAlg_StereoMatch(&sl, &sr, 0, 40, 5, &disp) == -3, "num_disparities 非 16 倍数 -> -3");

    check(Image2DAlg_StereoMatch(&sl, &sr, 0, 0, 5, &disp) == -3, "num_disparities<=0 -> -3");

    Image2DAlg_Image badh = disp;

    badh.height = 100;

    check(Image2DAlg_StereoMatch(&sl, &sr, 0, 96, 5, &badh) == -3, "容器高度不匹配 -> -3");

    Image2DAlg_Image badc = disp;

    badc.channels = 3;

    check(Image2DAlg_StereoMatch(&sl, &sr, 0, 96, 5, &badc) == -3, "容器 channels=3 -> -3");

    Image2DAlg_Image badd = disp;

    badd.data = NULL;

    check(Image2DAlg_StereoMatch(&sl, &sr, 0, 96, 5, &badd) == -1, "容器 data=NULL -> -1");

    cv::Mat sbig(130, 160, CV_8UC1, cv::Scalar(0));

    Image2DAlg_Image sb = MakeImage(sbig);

    check(Image2DAlg_StereoMatch(&sl, &sb, 0, 96, 5, &disp) == -3, "左右图尺寸不等 -> -3");

    check(Image2DAlg_StereoMatch(&sl, &sr, 0, 96, 5, NULL) == -1, "out_disparity=NULL -> -1");

    Image2DAlg_FreeImage(&sb);

    Image2DAlg_FreeImage(&disp);

    Image2DAlg_FreeImage(&sr);

    Image2DAlg_FreeImage(&sl);



    std::printf("== SolvePnP ==\n");

    // 已知内参 + 立方体 8 角点，位姿真值 rvec=(0.1,0.2,0.3) tvec=(0,0,5)

    const double fx = 380.0, fy = 380.0, cx = 320.0, cy = 240.0;

    double cam[9] = { fx, 0, cx, 0, fy, cy, 0, 0, 1 };

    const double cube[8][3] = {

        { -0.3, -0.3, -0.3 }, { 0.3, -0.3, -0.3 }, { 0.3, 0.3, -0.3 }, { -0.3, 0.3, -0.3 },

        { -0.3, -0.3, 0.3 }, { 0.3, -0.3, 0.3 }, { 0.3, 0.3, 0.3 }, { -0.3, 0.3, 0.3 }

    };

    double obj[24];

    for (int i = 0; i < 8; ++i) {

        obj[i * 3] = cube[i][0];

        obj[i * 3 + 1] = cube[i][1];

        obj[i * 3 + 2] = cube[i][2];

    }

    cv::Mat rvec_t = (cv::Mat_<double>(3, 1) << 0.1, 0.2, 0.3);

    cv::Mat tvec_t = (cv::Mat_<double>(3, 1) << 0.0, 0.0, 5.0);

    cv::Mat camm = (cv::Mat_<double>(3, 3) << fx, 0, cx, 0, fy, cy, 0, 0, 1);

    // 注：OpenCV 4.4 projectPoints 对 Nx1 CV_64FC3 InputArray 触发内部断言，
    // 用 vector<Point3f> 输入（float 精度足够）
    std::vector<cv::Point3f> objf;

    for (int i = 0; i < 8; ++i)

        objf.push_back(cv::Point3f((float)cube[i][0], (float)cube[i][1], (float)cube[i][2]));

    std::vector<cv::Point2f> proj;

    cv::projectPoints(objf, rvec_t, tvec_t, camm, cv::noArray(), proj);

    std::vector<Image2DAlg_Point2f> imgpts(proj.size());
    for (size_t i = 0; i < proj.size(); ++i) {
        imgpts[i].x = proj[i].x;
        imgpts[i].y = proj[i].y;
    }

    double rvec[3] = { 0 }, tvec[3] = { 0 };

    check(Image2DAlg_SolvePnP(obj, imgpts.data(), 8, cam, NULL, 0, rvec, tvec) == 0,

          "SolvePnP 返回 0（无畸变、dist=NULL）");

    check(std::fabs(rvec[0] - 0.1) < 0.05 && std::fabs(rvec[1] - 0.2) < 0.05 &&

          std::fabs(rvec[2] - 0.3) < 0.05 && std::fabs(tvec[0]) < 0.05 &&

          std::fabs(tvec[1]) < 0.05 && std::fabs(tvec[2] - 5.0) < 0.05,

          "SolvePnP 恢复位姿 (0.1,0.2,0.3)/(0,0,5)");

    // 带畸变（4 项 [k1,k2,p1,p2]）：投影与求解用同一套系数

    const double dist4[4] = { 0.1, -0.05, 0.001, -0.001 };

    cv::Mat distm = (cv::Mat_<double>(4, 1) << 0.1, -0.05, 0.001, -0.001);

    std::vector<cv::Point2f> proj2;

    cv::projectPoints(objf, rvec_t, tvec_t, camm, distm, proj2);

    std::vector<Image2DAlg_Point2f> imgpts2(proj2.size());
    for (size_t i = 0; i < proj2.size(); ++i) {
        imgpts2[i].x = proj2[i].x;
        imgpts2[i].y = proj2[i].y;
    }

    double rvec2[3] = { 0 }, tvec2[3] = { 0 };

    check(Image2DAlg_SolvePnP(obj, imgpts2.data(), 8, cam, dist4, 4, rvec2, tvec2) == 0,

          "SolvePnP 带畸变返回 0");

    check(std::fabs(rvec2[0] - 0.1) < 0.05 && std::fabs(rvec2[1] - 0.2) < 0.05 &&

          std::fabs(rvec2[2] - 0.3) < 0.05 && std::fabs(tvec2[2] - 5.0) < 0.05,

          "SolvePnP 带畸变恢复位姿");

    // 错误路径

    check(Image2DAlg_SolvePnP(obj, imgpts.data(), 3, cam, NULL, 0, rvec, tvec) == -2, "count<4 -> -2");

    check(Image2DAlg_SolvePnP(obj, imgpts.data(), 0, cam, NULL, 0, rvec, tvec) == -2, "count=0 -> -2");

    // dist_count 非法回归：非 0/4/5/8/12/14 的正值显式拦截为 -3（此前落入 cv
    // 断言异常 -> -99，且断言前可能已对调用方短缓冲构造超长视图越界读）
    {
        const double dist3[3] = { 0.1, -0.05, 0.001 };
        rvec[0] = 42.0;
        tvec[0] = 42.0;
        check(Image2DAlg_SolvePnP(obj, imgpts.data(), 8, cam, dist3, 3, rvec, tvec) == -3,
              "dist_count=3 非法 -> -3");
        check(rvec[0] == 42.0 && tvec[0] == 42.0, "dist_count 非法出口不写 rvec/tvec");
        check(Image2DAlg_SolvePnP(obj, imgpts.data(), 8, cam, dist4, 6, rvec, tvec) == -3,
              "dist_count=6 非法 -> -3");
    }

    check(Image2DAlg_SolvePnP(NULL, imgpts.data(), 8, cam, NULL, 0, rvec, tvec) == -1, "object=NULL -> -1");

    check(Image2DAlg_SolvePnP(obj, NULL, 8, cam, NULL, 0, rvec, tvec) == -1, "image=NULL -> -1");

    check(Image2DAlg_SolvePnP(obj, imgpts.data(), 8, NULL, NULL, 0, rvec, tvec) == -1, "camera=NULL -> -1");

    check(Image2DAlg_SolvePnP(obj, imgpts.data(), 8, cam, NULL, 0, NULL, tvec) == -1, "rvec=NULL -> -1");

}




// ---------------- 2.2.0 补充：彩色（BGRA）输入路径冒烟 ----------------
// 上述新算子测试图均为单通道；此处用同内容的 BGRA 版本覆盖 GrayOf 的 4 通道
// 分支（BGRA2GRAY），核验检测/匹配/跟踪/立体链路在彩色输入上行为一致。
// BGRA 图由单通道真值图 GRAY2BGRA 合成（BGRA2GRAY 后逐位还原），预期结果与
// 单通道用例一致。

static void testColorInputPaths()

{

    std::printf("== 彩色输入路径（BGRA -> 灰度） ==\n");

    // DetectAndMatch（BGRA 对，平移 (5,7)）
    cv::setRNGSeed(42);
    cv::Mat tex(96, 96, CV_8UC1);
    cv::randu(tex, cv::Scalar(40), cv::Scalar(220));
    cv::Mat shifted(96, 96, CV_8UC1, cv::Scalar(40));
    tex(cv::Rect(0, 0, 91, 89)).copyTo(shifted(cv::Rect(5, 7, 91, 89)));
    cv::Mat bgra1, bgra2;
    cv::cvtColor(tex, bgra1, cv::COLOR_GRAY2BGRA);
    cv::cvtColor(shifted, bgra2, cv::COLOR_GRAY2BGRA);
    Image2DAlg_Image c1 = MakeImage(bgra1);
    Image2DAlg_Image c2 = MakeImage(bgra2);
    Image2DAlg_OrbMatch* cm = NULL;
    int ncm = 0;
    check(Image2DAlg_DetectAndMatch(&c1, &c2, 0, 500, 0.75, &cm, &ncm) == 0 && ncm > 10,
          "BGRA DetectAndMatch(ORB) 返回 0 且匹配充足");
    int consistent = 0;
    for (int i = 0; i < ncm && cm; ++i)
        if (std::hypot(cm[i].x1 + 5.0 - cm[i].x2, cm[i].y1 + 7.0 - cm[i].y2) < 1.5) ++consistent;
    check(consistent >= 5, "BGRA 匹配含正确平移点对");
    Image2DAlg_FreeOrbMatches(cm);
    cm = NULL;
    ncm = 0;

    // BlobDetect（BGRA 暗斑）
    cv::Mat bg(128, 128, CV_8UC4, cv::Scalar(240, 240, 240, 255));
    cv::circle(bg, cv::Point(30, 30), 6, cv::Scalar(30, 30, 30, 255), -1);
    cv::circle(bg, cv::Point(80, 40), 8, cv::Scalar(30, 30, 30, 255), -1);
    Image2DAlg_Image ibc = MakeImage(bg);
    Image2DAlg_BlobFeature* cbs = NULL;
    int nbc = 0;
    check(Image2DAlg_BlobDetect(&ibc, 50, 2000, 0.5, 0.5, &cbs, &nbc) == 0 && nbc == 2,
          "BGRA BlobDetect 检出 2 个暗斑");
    Image2DAlg_FreeBlobFeatures(cbs);
    cbs = NULL;
    nbc = 0;
    Image2DAlg_FreeImage(&ibc);

    // CalcOpticalFlowPyrLK（BGRA，平移 (3,2)）
    cv::setRNGSeed(7);
    cv::Mat mtex(96, 96, CV_8UC1);
    cv::randu(mtex, cv::Scalar(40), cv::Scalar(220));
    cv::Mat moved(96, 96, CV_8UC1, cv::Scalar(40));
    mtex(cv::Rect(0, 0, 93, 94)).copyTo(moved(cv::Rect(3, 2, 93, 94)));
    cv::Mat bgra_p, bgra_q;
    cv::cvtColor(mtex, bgra_p, cv::COLOR_GRAY2BGRA);
    cv::cvtColor(moved, bgra_q, cv::COLOR_GRAY2BGRA);
    Image2DAlg_Image bp = MakeImage(bgra_p);
    Image2DAlg_Image bq = MakeImage(bgra_q);
    const Image2DAlg_Point2f tin1[1] = { {48.0f, 48.0f} };
    Image2DAlg_Point2f* np = NULL;
    unsigned char st1 = 0;
    check(Image2DAlg_CalcOpticalFlowPyrLK(&bp, &bq, tin1, 1, 21, 3, &np, &st1) == 0 &&
          st1 != 0 && np != NULL && std::fabs(np[0].x - 51.0) < 1.0 &&
          std::fabs(np[0].y - 50.0) < 1.0,
          "BGRA LK 跟踪位移 ≈ (3,2)");
    Image2DAlg_FreePoint2f(np);
    np = NULL;
    Image2DAlg_FreeImage(&bq);
    Image2DAlg_FreeImage(&bp);

    // StereoMatch（BGRA，视差 10）
    cv::setRNGSeed(17);
    cv::Mat stex(120, 210, CV_8UC1);
    cv::randu(stex, cv::Scalar(40), cv::Scalar(220));
    cv::Mat sleft = stex(cv::Rect(0, 0, 200, 120)).clone();
    cv::Mat sright = stex(cv::Rect(10, 0, 200, 120)).clone();
    cv::Mat bl, br;
    cv::cvtColor(sleft, bl, cv::COLOR_GRAY2BGRA);
    cv::cvtColor(sright, br, cv::COLOR_GRAY2BGRA);
    Image2DAlg_Image slc = MakeImage(bl);
    Image2DAlg_Image src_b = MakeImage(br);
    Image2DAlg_Image cdisp;
    cdisp.width = 200;
    cdisp.height = 120;
    cdisp.channels = 1;
    cdisp.data = (unsigned char*)std::malloc((size_t)200 * 120);
    std::memset(cdisp.data, 0, (size_t)200 * 120);
    check(Image2DAlg_StereoMatch(&slc, &src_b, 0, 96, 5, &cdisp) == 0, "BGRA SGBM 返回 0");
    int gok = 0, gtotal = 0;
    for (int y = 20; y < 100; y += 4)
        for (int x = 120; x < 190; x += 4) {
            ++gtotal;
            const int v = cdisp.data[(size_t)y * 200 + x];
            if (v >= 9 && v <= 11) ++gok;
        }
    check(gtotal > 0 && gok * 5 >= gtotal * 4, "BGRA SGBM 深部视差 ≈ 10");
    Image2DAlg_FreeImage(&cdisp);
    Image2DAlg_FreeImage(&src_b);
    Image2DAlg_FreeImage(&slc);

    // FindTransformECC（BGRA 平移模型，模糊结构纹理）
    cv::Mat etex(120, 120, CV_8UC1);
    cv::randu(etex, cv::Scalar(40), cv::Scalar(220));
    cv::Mat etexb;
    cv::GaussianBlur(etex, etexb, cv::Size(9, 9), 2.0);
    cv::Mat tin2(120, 120, CV_8UC1);
    cv::Mat T = (cv::Mat_<double>(2, 3) << 1, 0, 4, 0, 1, 3);
    cv::warpAffine(etexb, tin2, T, tin2.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    cv::Mat bt, bi;
    cv::cvtColor(etexb, bt, cv::COLOR_GRAY2BGRA);
    cv::cvtColor(tin2, bi, cv::COLOR_GRAY2BGRA);
    Image2DAlg_Image et = MakeImage(bt);
    Image2DAlg_Image ein = MakeImage(bi);
    double em[9] = { 0 };
    double corr = 0.0;
    check(Image2DAlg_FindTransformECC(&et, &ein, 0, 200, 1e-5, em, &corr) == 0 &&
          std::fabs(em[2] - 4.0) < 0.5 && std::fabs(em[5] - 3.0) < 0.5 && corr > 0.9,
          "BGRA ECC 恢复平移 (4,3)");
    Image2DAlg_FreeImage(&ein);
    Image2DAlg_FreeImage(&et);

    Image2DAlg_FreeImage(&c2);
    Image2DAlg_FreeImage(&c1);
}



static void testFitLinesFromBinary()

{

    std::printf("== FitLinesFromBinary ==\n");
    // 水平线段 y=20, x=8..32 与竖直线段 x=40, y=8..32（3px 粗）

    cv::Mat g(48, 48, CV_8UC1, cv::Scalar(0));

    cv::line(g, cv::Point(8, 20), cv::Point(32, 20), cv::Scalar(255), 3);

    cv::line(g, cv::Point(40, 8), cv::Point(40, 32), cv::Scalar(255), 3);

    Image2DAlg_Image in = MakeImage(g);

    Image2DAlg_LineFit* lines = NULL;

    int count = 0;

    check(Image2DAlg_FitLinesFromBinary(&in, 20, 0, 0, &lines, &count) == 0 && count == 2,

          "双线段拟合返回 2");

    bool hit_h = false, hit_v = false;

    for (int i = 0; i < count && lines; ++i) {

        if (std::fabs(lines[i].vy) < 0.1 && std::fabs(lines[i].y0 - 20.0) < 1.0) hit_h = true;

        if (std::fabs(lines[i].vx) < 0.1 && std::fabs(lines[i].x0 - 40.0) < 1.0) hit_v = true;

    }

    check(hit_h && hit_v, "水平/竖直线位置正确");

    check(lines != NULL && lines[0].rms < 2.0 && lines[1].rms < 2.0, "直线度 RMS < 2 像素");

    Image2DAlg_FreeLineFits(lines);



    lines = NULL;

    count = 0;

    check(Image2DAlg_FitLinesFromBinary(&in, 20, 0, 100, &lines, &count) == 0 && count == 0,

          "min_length=100 过滤掉短线段");

    Image2DAlg_FreeLineFits(lines);



    lines = (Image2DAlg_LineFit*)1;

    count = -1;

    check(Image2DAlg_FitLinesFromBinary(&in, 20, 0, 0, NULL, &count) == -1, "out_lines=NULL -> -1");

    Image2DAlg_FreeLineFits(NULL);

    check(true, "FreeLineFits NULL 安全");

    Image2DAlg_FreeImage(&in);

}



int main()

{

    setvbuf(stdout, NULL, _IONBF, 0);   // 无缓冲：崩溃时已输出的 PASS/FAIL 不丢失（定位崩溃点）

    testVersionAndLifecycle();

    testReadImage();

    testGrayAndGamma();

    testThreshold();

    testBinarize();

    testBlurAndMorphology();

    testCanny();

    testSobel();

    testGeometry();

    testEnhance();

    testConnectedComponents();

    testContours();

    testMatchTemplate();

    testFitAndColor();

    testShapesDefectsMulti();

    testRoiAndMultiCircle();

    testContourPoints();

    testWarpAndHsvWrap();

    testMatchEnhancedAndFits();

    testHough();

    testFindAllMatchesExAndSubPix();

    testFitEllipsesFromBinary();

    testFitLinesFromBinary();

    testRoiFamily();

    testMatchPyramid();

    testRegionStats();

    testFeaturePointsAndFits();

    testFeaturePipeline();

    testFeatureExtensions();

    testTrackRegisterStereo();

    testColorInputPaths();

    testErrorPaths();

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);

    return g_fail == 0 ? 0 : 1;

}

