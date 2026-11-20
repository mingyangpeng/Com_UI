#ifndef IMAGE2DALG_API_H
#define IMAGE2DALG_API_H

// ============================================================================
// Image2DAlg —— 通用 2D 图像算法库 C 接口（本头文件即接口权威文档）
// ============================================================================
// 定位：ShapelyAlg::ImageProcessor 的上层封装——图像算子一律复用基础库实现，
//       本层只负责参数语义（通道/尺寸/模式）、C 容器与 cv::Mat 转换、
//       测量结果结构组装。运行期依赖 ShapelyAlg.dll（及其 PCL/GEOS/OpenCV 运行时）。
//
// 版本历史：
//   1.0.0  基础层（GetVersion / Init / Cleanup / FreeImage / ReadImage）
//   1.1.0  通用处理 10 算子 + 测量 3 算子（ToGray..Enhance / CC / Contours / MatchTemplate）
//   1.2.0  + Sobel
//   1.3.0  + FitCircle / FitLine / HSVThreshold / AbsDiff
//   1.4.0  + MatchShapes / ConvexityDefects / MatchTemplateMulti
//   1.5.0  + ConnectedComponentsROI / AnalyzeContoursROI / FitCirclesFromBinary
//   1.6.0  + ContourPoints（轮廓原始点集导出）
//   1.7.0  + WarpAffine / WarpPerspective / HSVThresholdWrap
//   1.8.0  + 模板匹配增强（Masked/Rotated/FindAllMatches/Ex 尺度×角度）+
//            拟合扩展（FitCircleEx/Robust/Ellipse/Rect/Poly）——封装
//            ShapelyAlg::ImageProcessor 20/21 节新算子，本层零重复实现
//   1.9.0  + HoughLinesP / HoughCircles（封装 23 节霍夫检测）
//   1.10.0 + FindAllMatchesEx（多目标×旋转×尺度阵列全检，全局 NMS）+
//            MatchTemplateSubPix（亚像素对位）
//   1.11.0 + FitEllipsesFromBinary（多目标椭圆拟合）
//   1.12.0 + ROI 族补全（HoughLinesPROI / HoughCirclesROI / MatchTemplateROI /
//            FindAllMatchesROI——结果坐标平移回全图系，同 CCROI 先例）
//   1.13.0 + MatchTemplatePyramid（金字塔粗搜 + 逐级精化的对位提速版）
//   1.14.0 + RegionStats / Histogram / CompareHist（批次灰度一致性监控）
//   1.15.0 + Binarize（统一二值化：单阈值[含 TRUNC/TOZERO]/OTSU/自适应/双阈值 + 反转）
//   2.0.0  + 特征点族（HarrisCorners / GoodFeatures / CornerSubPix）+
//            ORBAlign（ORB 特征匹配 + RANSAC 仿射对位——大形变对位）+
//            ApproxPolyDP（多边形拟合）/ FitRectsFromBinary（多目标旋转矩形）
//   2.1.0  + 特征链路拆分（FastFeatures FAST 检测 / ComputeDescriptors 描述子计算
//            ORB·BRISK·AKAZE·SIFT / MatchFeatures Lowe 比率匹配 /
//            EstimateAffinePartial 部分仿射 / FindHomography 单应）——
//            与 ORBAlign 一站式并存，检测/描述/匹配/几何估计可独立复用
//   2.2.0  + 特征扩展（BriskFeatures / AkazeFeatures / SiftFeatures 检测+描述
//            一次完成 / BlobDetect 斑点检测 + BlobFeature + FreeBlobFeatures /
//            DetectAndMatch 一站式检测+描述+匹配，复用 OrbMatch）+
//            跟踪配准立体族（CalcOpticalFlowPyrLK 光流 / FindTransformECC 对齐 /
//            PhaseCorrelate 平移估计 / StereoMatch 双目视差 / SolvePnP 位姿）
//
//   2.3.0  + 滤波扩展（Laplacian 算子 / Filter2D 自定义卷积核——中值/双边/
//            高斯已由 Blur method 0/1/2 覆盖，不另设入口避免语义漂移）+
//            DenoiseNLMeans 非局部均值降噪
//
// 【通道语义】Image2DAlg_Image.channels 支持 1（灰度）/ 3（BGR）/ 4（BGRA），
//   data 为紧排像素（行步长 = width * channels，无对齐填充）：
//   * 通用算子保持输入真实通道数（1→1、3→3、4→4，alpha 随通道参与运算）；
//   * 自然单通道算子（ToGray / Threshold / Canny / Sobel / Enhance-均衡 /
//     HSVThreshold）输出 1 通道，彩色输入内部先转灰度 / HSV；
//   * 唯一例外 ReadImage 固定输出 BGRA（既有宿主契约，不改）。
//
// 【坐标系】原点在左上角，x 向右、y 向下（图像约定）；位置/尺寸单位均为像素。
//
// 【错误码】（除 Free* / GetVersion 外统一返回 int）
//   0    成功
//   -1   空指针或空图（in/out 为 NULL、data 为空、宽高 <= 0）
//   -2   特征/匹配/估计不足（特征族：无检出关键点、匹配对数过少、几何估计
//        无法求解；跟踪配准族：ECC 不收敛或相关性不足、PnP 求解失败；
//        ReadImage 例外——-2 为打开失败）
//   -3   通道数 / 模式枚举 / 参数取值不支持（如 2 通道、mode 越界、ROI 为空）
//   -4   内存不足（malloc 失败）
//   -99  OpenCV 内部异常（已在 C 边界统一捕获，异常不跨 DLL）
//
// 【内存规则】
//   * out_image->data 与测量结果数组一律由本库 malloc 分配；
//   * 图像缓冲用 Image2DAlg_FreeImage 释放（置空 data）；测量数组用对应
//     FreeBlobs / FreeContours / FreeDefects 释放（free 指针本身，调用方自行置空）；
//   * 输入容器仅作只读视图，算子不修改输入，生命周期归调用方。
//
// 【线程性】无全局状态，各算子可多线程并发调用（Init/Cleanup 为占位）。
//
// 【最小示例】
//   Image2DAlg_Image src, bin;
//   Image2DAlg_ReadImage("board.png", &src);              // BGRA
//   Image2DAlg_Threshold(&src, 0, 128, 255, 0, 0, &bin);  // 二值化 → 1 通道
//   Image2DAlg_Blob* blobs = NULL;  int count = 0;
//   Image2DAlg_ConnectedComponents(&bin, 8, 10, 0, &blobs, &count);
//   // ... 遍历 blobs[i].area / left / top ...
//   Image2DAlg_FreeBlobs(blobs);
//   Image2DAlg_FreeImage(&bin);
//   Image2DAlg_FreeImage(&src);
// ============================================================================

#include "AlgExport.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- 基础层 ----------------

/// 返回库版本号（语义化 "主.次.补丁"，如 "1.4.0"）。
/// 新增接口 → 次版本 +1；行为修正 → 补丁 +1。返回静态字符串，无需释放。
IMAGE2DALG_EXPORT const char* Image2DAlg_GetVersion(void);

/// 初始化（占位实现：本库无全局状态，算子可直接调用）。
/// @return 恒 0；重复调用幂等。
IMAGE2DALG_EXPORT int Image2DAlg_Init(void);

/// 反初始化（占位实现，与 Init 配对无实际副作用）。
IMAGE2DALG_EXPORT void Image2DAlg_Cleanup(void);

/// 图像容器（紧排，行主序）。
///   width/height  像素尺寸（>0）
///   channels      1 = 灰度 / 3 = BGR / 4 = BGRA（本库不使用 2 通道，传 -3）
///   data          像素缓冲（本库 malloc 的输出，或调用方提供的输入）
typedef struct Image2DAlg_Image {
    int width;
    int height;
    int channels;
    unsigned char* data;
} Image2DAlg_Image;

/// 释放图像缓冲（free + data 置 NULL）。
/// image 为 NULL 或 data 已空时为 no-op；可与重复调用、NULL 调用安全混用。
IMAGE2DALG_EXPORT void Image2DAlg_FreeImage(Image2DAlg_Image* image);

/// 图像文件读取。
/// [in]  img_path   文件路径（ANSI 编码；支持 png / tiff / jpg / jpeg）
/// [out] out_image  固定输出 BGRA 4 通道（模块唯一强制 4 通道的接口，宿主契约）
/// @return 0 成功；-1 空指针；-2 打开失败（路径不存在/格式不支持）；-4 内存不足。
IMAGE2DALG_EXPORT int Image2DAlg_ReadImage(const char* img_path, Image2DAlg_Image* out_image);

// ---------------- 通用处理（image → image） ----------------

/// 灰度化。BGR → 灰度（OpenCV 系数 0.114B + 0.587G + 0.299R）；BGRA 丢弃 alpha。
/// 输入 1 通道时原样拷贝输出。输出 1 通道。
IMAGE2DALG_EXPORT int Image2DAlg_ToGray(const Image2DAlg_Image* in, Image2DAlg_Image* out);

/// Gamma 校正（LUT 一次查表，O(1)/像素）。
/// [in]  gamma   曲线指数（输出 = 255*(v/255)^(1/gamma)）；< 0.05 时钳制为 0.05；
///               1.0 = 恒等；>1 提亮、<1 压暗。
/// 输出通道与输入一致（每通道独立查表）。
IMAGE2DALG_EXPORT int Image2DAlg_Gamma(const Image2DAlg_Image* in, double gamma,
                                       Image2DAlg_Image* out);

/// 阈值化（输出 1 通道 0/maxval）。
/// [in]  mode        0 固定二值 / 1 固定反二值 / 2 OTSU 自动阈值 /
///                   3 自适应均值 / 4 自适应高斯
/// [in]  thresh      固定阈值（mode 0/1 有效；2~4 忽略）
/// [in]  maxval      超阈值赋值（常用 255）
/// [in]  block_size  自适应窗口边长（mode 3/4 有效；偶数自动补奇，最小 3）
/// [in]  C           自适应偏移常数（从邻域均值/加权均值中减去）
/// 彩色输入内部先转灰度。光照不均场景用 3/4，双峰直方图用 2。
IMAGE2DALG_EXPORT int Image2DAlg_Threshold(const Image2DAlg_Image* in, int mode,
                                           double thresh, double maxval,
                                           int block_size, double C,
                                           Image2DAlg_Image* out);

/// 统一二值化（输出 1 通道）。相比 Threshold：补全 TRUNC/TOZERO、双阈值，且统一支持反转。
/// 与 Threshold 的关系：本接口独立新增，不改变旧接口行为；mode 0~4 与 Threshold 语义一致。
/// [in] mode
///   0 固定二值      v >= thresh  -> maxval，否则 0
///   1 固定反二值    v >= thresh  -> 0，否则 maxval
///   2 OTSU 自动阈值（双峰直方图；thresh 忽略）
///   3 自适应均值    光照不均场景（block_size/C 有效）
///   4 自适应高斯    光照不均场景（block_size/C 有效）
///   5 TRUNC 截断    v > thresh -> thresh，否则 v（保留灰度层次）
///   6 TOZERO        v > thresh -> 0，否则 v
///   7 TOZERO_INV    v > thresh -> v，否则 0
///   8 双阈值·区间内前景  low <= v <= high -> maxval，否则 0（高亮带提取 / 背景抑制）
///   9 双阈值·区间外前景  v < low 或 v > high -> maxval，否则 0（暗/亮异常同时标记）
///  10 双阈值·三值化      v < low -> 0；low <= v <= high -> mid_val；v > high -> maxval
/// [in] thresh     固定阈值（mode 0/1/5/6/7 有效；mode 8/9/10 作为下限 low）
/// [in] thresh2    双阈值上限 high（mode 8/9/10 有效；须 >= thresh，否则 -3）
/// [in] maxval     前景赋值（常用 255；mode 10 为高值端；钳制到 [0,255]）
/// [in] mid_val    三值化中间值（仅 mode 10 有效；钳制到 [0,255]）
/// [in] block_size 自适应窗口边长（mode 3/4 有效；偶数自动补奇，最小 3）
/// [in] C          自适应偏移常数（mode 3/4 有效）
/// [in] invert     0 不反转；非 0 对最终结果逐像素取反（v -> 255 - v），对所有 mode 生效
/// 彩色输入内部先转灰度（保持 ToGray 现有方法不变）。输出 1 通道。
IMAGE2DALG_EXPORT int Image2DAlg_Binarize(const Image2DAlg_Image* in, int mode,
                                          double thresh, double thresh2,
                                          double maxval, double mid_val,
                                          int block_size, double C, int invert,
                                          Image2DAlg_Image* out);

/// 模糊 / 降噪（输出通道与输入一致）。
/// [in]  method   0 高斯 / 1 中值 / 2 双边
/// [in]  ksize    核尺寸（高斯/中值自动补奇；双边=邻域直径，<=0 取 5）
/// [in]  sigma1   高斯：X 方向 sigma（0 = 由 ksize 推导）；双边：sigmaColor
/// [in]  sigma2   高斯：Y 方向 sigma（0 = 同 sigma1）；双边：sigmaSpace
/// 4 通道双边内部拆 BGR 处理并保留 alpha（OpenCV 双边不支持 4 通道）。
IMAGE2DALG_EXPORT int Image2DAlg_Blur(const Image2DAlg_Image* in, int method, int ksize,
                                      double sigma1, double sigma2, Image2DAlg_Image* out);

/// 形态学（输出通道与输入一致）。
/// [in]  op          0 腐蚀 / 1 膨胀 / 2 开 / 3 闭 / 4 梯度 / 5 顶帽 / 6 黑帽
///                   （与 cv::MORPH_* 枚举同值：2~6 直接透传）
/// [in]  shape       0 矩形 / 1 十字 / 2 椭圆
/// [in]  ksize       结构元边长（自动补奇，>= 1）
/// [in]  iterations  重复次数（< 1 按 1 处理）
/// 开运算去噪点、闭运算填小孔、梯度取轮廓、顶帽/黑帽提亮/暗细节。
IMAGE2DALG_EXPORT int Image2DAlg_Morphology(const Image2DAlg_Image* in, int op, int shape,
                                            int ksize, int iterations, Image2DAlg_Image* out);

/// Canny 边缘检测（输出 1 通道 0/255）。
/// [in]  threshold1  滞后阈值下限（弱边缘）
/// [in]  threshold2  滞后阈值上限（强边缘）；经验取 2~3 倍 threshold1
/// 彩色输入先转灰度。
IMAGE2DALG_EXPORT int Image2DAlg_Canny(const Image2DAlg_Image* in,
                                       double threshold1, double threshold2,
                                       Image2DAlg_Image* out);

/// Sobel 梯度（输出 1 通道灰度 = |梯度| 缩放到 0~255）。
/// [in]  dx/dy   求导阶数（dx + dy >= 1，否则 OpenCV 异常 → -99）
/// [in]  ksize   核边长 1/3/5/7，其他取值 → -99
/// 内部 CV_16S 求导后取绝对值缩放，避免负梯度截断。彩色输入先转灰度。
IMAGE2DALG_EXPORT int Image2DAlg_Sobel(const Image2DAlg_Image* in,
                                       int dx, int dy, int ksize,
                                       Image2DAlg_Image* out);

/// 缩放。
/// [in]  out_width/out_height  目标尺寸（<= 0 → -3）
/// [in]  interpolation         cv::InterpolationFlags：0 近邻 / 1 线性（默认）/
///                             2 立方 / 3 面积 / 4 Lanczos4
IMAGE2DALG_EXPORT int Image2DAlg_Resize(const Image2DAlg_Image* in,
                                        int out_width, int out_height, int interpolation,
                                        Image2DAlg_Image* out);

/// 裁剪 ROI（输出通道与输入一致）。
/// [in]  x/y/width/height  目标区域；越界部分自动钳制到图内；
///                         与图像无交集（空结果）→ -3。
IMAGE2DALG_EXPORT int Image2DAlg_Crop(const Image2DAlg_Image* in,
                                      int x, int y, int width, int height,
                                      Image2DAlg_Image* out);

/// 旋转：绕图像中心旋转 angle 度并缩放 scale，输出与原图同尺寸。
/// [in]  angle           逆时针角度（度，OpenCV 约定）
/// [in]  scale           缩放系数（> 0）
/// [in]  interpolation  同 Resize
/// 旋转后超出原图范围的部分填 0（黑）。
IMAGE2DALG_EXPORT int Image2DAlg_Rotate(const Image2DAlg_Image* in, double angle, double scale,
                                        int interpolation, Image2DAlg_Image* out);

/// 灰度增强。
/// [in]  method     0 全局直方图均衡 / 1 CLAHE（限制对比度自适应均衡）
/// [in]  clip_limit CLAHE 对比度上限（典型 2.0~4.0，越大越接近全局均衡）
/// [in]  tile_size  CLAHE 分块边长（<= 0 取 8）
/// 通道行为：method 0 彩色先转灰度、输出 1 通道；method 1 输出与输入同通道
///（彩色走 Lab 亮度通道，4 通道保留 alpha 不参与增强）。
IMAGE2DALG_EXPORT int Image2DAlg_Enhance(const Image2DAlg_Image* in, int method,
                                         double clip_limit, int tile_size,
                                         Image2DAlg_Image* out);

// ---------------- 测量 / 缺陷检测 ----------------

/// 连通域（AOI 缺陷计数 / 面积过滤）。
/// [in]  in            二值语义：非零像素为前景（彩色先转灰度）
/// [in]  connectivity  邻域定义，只能 4 或 8（否则 -3）
/// [in]  min_area      面积过滤下限（像素，闭区间）
/// [in]  max_area      面积过滤上限（<= 0 表示不设上限）
/// [out] out_blobs     malloc 数组（调用方用 Image2DAlg_FreeBlobs 释放）
/// [out] out_count     数量；无结果时 *out_blobs = NULL、*out_count = 0、仍返回 0
typedef struct Image2DAlg_Blob {
    int label;                       // 连通域编号（1 起，与标记图对应）
    int area;                        // 像素面积
    int left, top, width, height;    // 包围盒（左上角 + 宽高）
    double cx, cy;                   // 质心（亚像素，像素单位）
} Image2DAlg_Blob;

IMAGE2DALG_EXPORT int Image2DAlg_ConnectedComponents(const Image2DAlg_Image* in,
                                                     int connectivity,
                                                     int min_area, int max_area,
                                                     Image2DAlg_Blob** out_blobs,
                                                     int* out_count);

/// 连通域（ROI 版）：仅在 ROI 内统计，结果坐标平移回全图坐标系（bbox 左上角与
/// 质心均已加 ROI 原点，调用方无需二次换算）。
/// [in]  roi_x/y/w/h  目标区域；w 或 h <= 0 表示全图（等价 ConnectedComponents）；
///                    ROI 与图像无交集 → -3
IMAGE2DALG_EXPORT int Image2DAlg_ConnectedComponentsROI(const Image2DAlg_Image* in,
                                                        int roi_x, int roi_y,
                                                        int roi_w, int roi_h,
                                                        int connectivity,
                                                        int min_area, int max_area,
                                                        Image2DAlg_Blob** out_blobs,
                                                        int* out_count);

/// 轮廓测量（一次得到每个轮廓的全部常用几何量）。
/// [in]  in            二值语义：非零像素为前景
/// [in]  external_only != 0 只取最外层轮廓（RETR_EXTERNAL）；0 = 全部（RETR_LIST）
/// [in]  min_area      面积过滤（轮廓面积 < min_area 的跳过）
/// [out] out_contours  malloc 数组（用 Image2DAlg_FreeContours 释放）
/// [out] out_count     数量；无结果时 *out_contours = NULL、*out_count = 0、仍返回 0
typedef struct Image2DAlg_ContourInfo {
    double area;                     // 轮廓面积（像素²；顶点在像素中心，10x10 方块约 81）
    double perimeter;                // 周长（闭合 arcLength）
    double circularity;              // 圆度 4πA/P²：1 ≈ 圆，0.785 ≈ 方；P=0 时为 0
    int bbox_x, bbox_y, bbox_w, bbox_h;
    double center_x, center_y;       // 质心（矩；m00=0 时退化为包围盒中心）
    double min_rect_cx, min_rect_cy; // 最小外接矩形中心
    double min_rect_w, min_rect_h, min_rect_angle;  // 宽/高/角度（OpenCV RotatedRect 约定）
    double circle_cx, circle_cy, circle_r;          // 最小外接圆（注意：非最小二乘拟合）
} Image2DAlg_ContourInfo;

IMAGE2DALG_EXPORT int Image2DAlg_AnalyzeContours(const Image2DAlg_Image* in,
                                                 int external_only, int min_area,
                                                 Image2DAlg_ContourInfo** out_contours,
                                                 int* out_count);

/// 轮廓测量（ROI 版）：仅在 ROI 内提取轮廓，结果坐标（包围盒/质心/最小外接矩形/
/// 最小外接圆）平移回全图坐标系。
/// [in]  roi_x/y/w/h  同 ConnectedComponentsROI（w 或 h <= 0 = 全图；无交集 → -3）
IMAGE2DALG_EXPORT int Image2DAlg_AnalyzeContoursROI(const Image2DAlg_Image* in,
                                                    int roi_x, int roi_y,
                                                    int roi_w, int roi_h,
                                                    int external_only, int min_area,
                                                    Image2DAlg_ContourInfo** out_contours,
                                                    int* out_count);

/// 轮廓原始点集导出（供宿主绘制轮廓 / 上层二次处理）。
/// [in]  in             二值语义：非零像素为前景
/// [in]  external_only != 0 只取最外层轮廓；0 = 全部
/// [in]  min_area      面积过滤（同 AnalyzeContours）
/// [out] out_xy        全部轮廓点扁平拼接 [x0,y0,x1,y1,...]（float，malloc；
///                     缓冲共 2*out_points 个 float）
/// [out] out_points    点个数（每个点占 2 个 float）
/// [out] out_offsets   每条轮廓的起始"点索引"（int 数组，长度 = 轮廓数；malloc）
///                     第 k 条轮廓的点 = out_xy[out_offsets[k]*2 .. out_offsets[k+1]*2)
/// [out] out_contours  轮廓条数
/// 点序与 cv::findContours 一致（CHAIN_APPROX_SIMPLE 压缩拐点）；
/// 无结果时各出参置 NULL/0、返回 0。释放用 Image2DAlg_FreeContourPoints。
IMAGE2DALG_EXPORT int Image2DAlg_ContourPoints(const Image2DAlg_Image* in,
                                               int external_only, int min_area,
                                               float** out_xy, int* out_points,
                                               int** out_offsets, int* out_contours);

/// 释放轮廓点集（两个数组各自 free，均 NULL 安全；调用方自行置空）。
IMAGE2DALG_EXPORT void Image2DAlg_FreeContourPoints(float* xy, int* offsets);

/// 模板匹配（滑动窗口相似度）。
/// [in]  image/templ  两图通道数必须一致（否则 -3），模板不得大于图像（否则 -3）
/// [in]  method       cv::TemplateMatchModes 0~5（0/1 = SQDIFF 系）
/// [out] out_score    得分：SQDIFF 系越小越好（0=完全一致），其余越大越好（1=归一化满值）
/// [out] out_x/out_y  最佳匹配的模板左上角（图像坐标系）
IMAGE2DALG_EXPORT int Image2DAlg_MatchTemplate(const Image2DAlg_Image* image,
                                               const Image2DAlg_Image* templ, int method,
                                               double* out_score, int* out_x, int* out_y);

/// 多尺度模板匹配：在 [scale_lo, scale_hi] 内以 scale_step 步进缩放模板逐一匹配，
/// 取跨尺度最佳的位置 / 得分 / 尺度。
/// [in]  scale_lo/hi/step  尺度范围与步进（step <= 0 或 lo > hi → -3；
///                         遍历次数 = (hi-lo)/step + 1，注意范围别开太大）
/// [out] out_scale         最佳尺度（实际使用的缩放系数）
/// 某尺度下模板缩放后越出图像则跳过该尺度；所有尺度均越界 → -3。
/// 结果坐标在图像坐标系（模板按该尺度缩放后贴上的左上角）。
IMAGE2DALG_EXPORT int Image2DAlg_MatchTemplateMulti(const Image2DAlg_Image* image,
                                                    const Image2DAlg_Image* templ, int method,
                                                    double scale_lo, double scale_hi,
                                                    double scale_step,
                                                    double* out_score, int* out_x, int* out_y,
                                                    double* out_scale);

// ---------------- 模板匹配增强（封装 ShapelyAlg::ImageProcessor 20 节） ----------------

/// 带掩膜的模板匹配：只统计 mask 非零区域（遮挡/干扰背景下的鲁棒匹配）。
/// [in]  mask    与 templ 同尺寸 1 通道图（非 0 = 参与匹配；2/3/4 通道 → -3）
/// [in]  method  仅 0（TM_SQDIFF）/ 3（TM_CCORR_NORMED）支持掩膜（OpenCV 4.4
///               限制），其他 method → -3
/// 输出语义同 MatchTemplate（左上角坐标；SQDIFF 越小越好）。
IMAGE2DALG_EXPORT int Image2DAlg_MatchTemplateMasked(const Image2DAlg_Image* image,
                                                     const Image2DAlg_Image* templ,
                                                     const Image2DAlg_Image* mask, int method,
                                                     double* out_score, int* out_x, int* out_y);

/// 旋转模板搜索（Mark 点带角度对位）：逐角度旋转模板匹配，取全局最优。
/// [in]  angle_start/end/step  角度范围（度，end >= start；step > 0，否则 -3）
/// [out] out_cx/out_cy  模板**中心**落在 image 上的位置（亚像素——注意与
///                      MatchTemplate 的左上角语义不同）
/// [out] out_angle      命中角度（度，逆时针）；[out] out_score 得分
/// 两图通道必须一致。代价：每角度一次全图匹配，大图建议先粗搜。
IMAGE2DALG_EXPORT int Image2DAlg_MatchTemplateRotated(const Image2DAlg_Image* image,
                                                      const Image2DAlg_Image* templ,
                                                      double angle_start, double angle_end,
                                                      double angle_step, int method,
                                                      double* out_score, double* out_cx,
                                                      double* out_cy, double* out_angle);

/// 多目标模板匹配：按得分从优到劣提取至多 max_matches 个目标（NMS 去重）。
/// [in]  score_threshold  SQDIFF 系 = "得分 <= 阈值"命中，其余 = "得分 >= 阈值"
/// [in]  max_matches      最多输出数（>= 1，否则 -3）
/// [in]  nms_radius       非极大值抑制半径（像素，>= 0）
/// [out] out_matches      malloc 数组（用 FreeMatches 释放）；loc = 模板左上角
/// [out] out_count        数量；无命中时 *out_matches = NULL、*out_count = 0、返回 0
typedef struct Image2DAlg_Match {
    double x, y;      // 模板左上角（图像坐标系）
    double score;     // 得分（语义随 method）
} Image2DAlg_Match;

IMAGE2DALG_EXPORT int Image2DAlg_FindAllMatches(const Image2DAlg_Image* image,
                                                const Image2DAlg_Image* templ, int method,
                                                double score_threshold, int max_matches,
                                                int nms_radius,
                                                Image2DAlg_Match** out_matches, int* out_count);

/// 释放匹配结果数组（直接 free 指针本身；NULL 安全；调用方自行置空）。
IMAGE2DALG_EXPORT void Image2DAlg_FreeMatches(Image2DAlg_Match* matches);

/// 模板匹配（ROI 版）：仅在 ROI 内匹配，命中左上角平移回全图坐标系。
/// [in]  roi_x/y/w/h  同 ConnectedComponentsROI（w/h <= 0 = 全图；无交集 → -3）；
///                    模板大于 ROI → -3
IMAGE2DALG_EXPORT int Image2DAlg_MatchTemplateROI(const Image2DAlg_Image* image,
                                                  int roi_x, int roi_y, int roi_w, int roi_h,
                                                  const Image2DAlg_Image* templ, int method,
                                                  double* out_score, int* out_x, int* out_y);

/// 多目标模板匹配（ROI 版）：仅在 ROI 内匹配，命中左上角平移回全图坐标系，
/// NMS 在 ROI 内进行。模板大于 ROI → -3。
IMAGE2DALG_EXPORT int Image2DAlg_FindAllMatchesROI(const Image2DAlg_Image* image,
                                                   int roi_x, int roi_y, int roi_w, int roi_h,
                                                   const Image2DAlg_Image* templ, int method,
                                                   double score_threshold, int max_matches,
                                                   int nms_radius,
                                                   Image2DAlg_Match** out_matches, int* out_count);

/// 多尺度 × 多角度组合模板匹配（Mark 点缩放+旋转对位）：
/// 在 [scale_lo,scale_hi] 步进 scale_slot 的每个尺度上做旋转搜索
///（[angle_lo,angle_hi] 步进 angle_step），取跨尺度全局最优。
/// [out] out_cx/out_cy  模板**中心**落点（亚像素）；[out] out_scale/out_angle
/// 参数校验同 MatchTemplateMulti / MatchTemplateRotated；某尺度下模板越界跳过。
/// 代价 = 尺度数 × 角度数 次全图匹配——先粗后精属调用方优化。
IMAGE2DALG_EXPORT int Image2DAlg_MatchTemplateEx(const Image2DAlg_Image* image,
                                                 const Image2DAlg_Image* templ, int method,
                                                 double scale_lo, double scale_hi,
                                                 double scale_step,
                                                 double angle_lo, double angle_hi,
                                                 double angle_step,
                                                 double* out_score, double* out_cx,
                                                 double* out_cy, double* out_scale,
                                                 double* out_angle);

/// 金字塔粗搜加速的旋转+尺度模板匹配（Ex 的提速版，单目标）。
/// 两段式：在顶层粗化图上做全范围粗搜 → 逐级回原图，仅在粗解 ±1 步进邻域内精化。
/// [in]  levels  金字塔层数（>= 1，否则 -3；> 6 自动截断；1 = 等价 MatchTemplateEx）。
///               层数同时受"模板最小边 >= 12 / 图像最小边 >= 32"约束自动截断
///               （粗层模板过小会丧失角度判别力——小模板请直接用 MatchTemplateEx）
/// [out] 语义同 MatchTemplateEx（cx/cy = 模板中心落点，已映射回原图坐标系）
/// 注意：粗搜在低分辨率进行——弱纹理/低对比模板可能粗搜失准（先用 Ex 验证再切本接口）；
///       SQDIFF 系得分跨层不可比，建议归一化类 method（CCORR/CCOEFF_NORMED）。
/// 耗时 ≈ 顶层全范围搜索 + 每层 9 次精化匹配——层数 2~3 时典型提速 4~10 倍。
IMAGE2DALG_EXPORT int Image2DAlg_MatchTemplatePyramid(const Image2DAlg_Image* image,
                                                      const Image2DAlg_Image* templ, int method,
                                                      double scale_lo, double scale_hi,
                                                      double scale_step,
                                                      double angle_lo, double angle_hi,
                                                      double angle_step, int levels,
                                                      double* out_score, double* out_cx,
                                                      double* out_cy, double* out_scale,
                                                      double* out_angle);

/// 亚像素模板匹配：整像素峰 + 三分点抛物线插值（±0.5 像素修正）。
/// [out] out_x/out_y  亚像素左上角坐标；[out] out_score 插值后得分
/// 其余语义同 MatchTemplate（封装 ImageProcessor::findBestMatchSubPix）。
IMAGE2DALG_EXPORT int Image2DAlg_MatchTemplateSubPix(const Image2DAlg_Image* image,
                                                     const Image2DAlg_Image* templ, int method,
                                                     double* out_score, double* out_x,
                                                     double* out_y);

/// 多目标 × 旋转 × 尺度组合搜索（Mark 阵列全检）：
/// 在每个 (scale, angle) 组合上做多目标匹配，跨组合合并做全局 NMS
///（中心距离 > nms_radius 去重），按得分从优到劣输出至多 max_matches 个目标。
/// [in]  score_threshold  SQDIFF 系 = "得分 <= 阈值"命中，其余 = "得分 >= 阈值"
/// [in]  max_matches      最多输出数（>= 1，否则 -3）
/// [in]  nms_radius       全局 NMS 半径（像素，>= 0；以模板中心为准）
/// [in]  scale_lo/hi/step、angle_lo/hi/step  校验同 MatchTemplateEx
/// [out] out_matches      malloc 数组（用 FreeMatchesEx 释放）；cx/cy = 模板
///                        **中心**落点（整数精度，亚像素精化接 MatchTemplateSubPix）
/// [out] out_count        数量；无命中时 *out_matches = NULL、*out_count = 0、返回 0
/// 代价 = 尺度数 × 角度数 次全图匹配 + 每次多目标提取——范围勿开过大。
typedef struct Image2DAlg_MatchEx {
    double cx, cy;    // 模板中心落点（图像坐标系，整数精度）
    double score;     // 得分（语义随 method）
    double scale;     // 命中尺度
    double angle;     // 命中角度（度，逆时针）
} Image2DAlg_MatchEx;

IMAGE2DALG_EXPORT int Image2DAlg_FindAllMatchesEx(const Image2DAlg_Image* image,
                                                  const Image2DAlg_Image* templ, int method,
                                                  double score_threshold, int max_matches,
                                                  int nms_radius,
                                                  double scale_lo, double scale_hi,
                                                  double scale_step,
                                                  double angle_lo, double angle_hi,
                                                  double angle_step,
                                                  Image2DAlg_MatchEx** out_matches,
                                                  int* out_count);

/// 释放扩展匹配结果数组（直接 free 指针本身；NULL 安全；调用方自行置空）。
IMAGE2DALG_EXPORT void Image2DAlg_FreeMatchesEx(Image2DAlg_MatchEx* matches);

/// HSV 色彩阈值分割（输出 1 通道 0/255 掩膜）。
/// [in]  h_lo/h_hi  色相范围 [0,180]（OpenCV HSV；红色跨 0 需两次调用取并集）
/// [in]  s_lo/hi    饱和度范围 [0,255]
/// [in]  v_lo/hi    明度范围 [0,255]
/// 输入须为彩色（3/4 通道，灰度图 → -3）；4 通道先去 alpha。
IMAGE2DALG_EXPORT int Image2DAlg_HSVThreshold(const Image2DAlg_Image* in,
                                              double h_lo, double h_hi,
                                              double s_lo, double s_hi,
                                              double v_lo, double v_hi,
                                              Image2DAlg_Image* out);

/// HSV 色彩阈值分割（wrap 版）：h_lo > h_hi 时自动按环绕区间处理
///（即 [h_lo,180] ∪ [0,h_hi]，专治红色跨 0；h_lo <= h_hi 时与 HSVThreshold 等价）。
/// s/v 区间语义不变。其余同 HSVThreshold。
IMAGE2DALG_EXPORT int Image2DAlg_HSVThresholdWrap(const Image2DAlg_Image* in,
                                                  double h_lo, double h_hi,
                                                  double s_lo, double s_hi,
                                                  double v_lo, double v_hi,
                                                  Image2DAlg_Image* out);

/// 批板差分：两图逐像素差的绝对值（AOI 检出核心手段，差值图再阈值即缺陷区）。
/// [in]  in1/in2  尺寸与通道必须一致（否则 -3）
IMAGE2DALG_EXPORT int Image2DAlg_AbsDiff(const Image2DAlg_Image* in1,
                                         const Image2DAlg_Image* in2,
                                         Image2DAlg_Image* out);

// ---------------- 点集拟合（xy 为交错数组 [x0,y0,x1,y1,...]，count 为点数） ----------------

/// 最小二乘圆拟合（Kåsa 代数法，SVD 求解）。
/// [in]  xy/count  点集（count >= 3，否则 -3）
/// [out] out       圆心 / 半径 / 残差
/// 共线、重合等退化输入可能得到无意义圆（不返回错误），调用方应结合 rms 判断质量；
/// 典型合格拟合 rms 与输入噪声同量级。
typedef struct Image2DAlg_CircleFit {
    double cx, cy;    // 圆心（像素）
    double r;         // 半径（像素）
    double rms;       // 残差均方根（像素）：sqrt(Σ(|p-c|-r)²/N)
} Image2DAlg_CircleFit;

IMAGE2DALG_EXPORT int Image2DAlg_FitCircle(const float* xy, int count,
                                           Image2DAlg_CircleFit* out);

/// 直线拟合（DIST_L2 最小二乘；内部按像素取整——输入来自图像坐标）。
/// [in]  xy/count  点集（count >= 2，否则 -3）
/// [out] out       方向 / 线上一点 / 残差
typedef struct Image2DAlg_LineFit {
    double vx, vy;    // 单位方向向量
    double x0, y0;    // 线上一点（像素）
    double rms;       // 点到直线垂直距离 RMS（像素）
} Image2DAlg_LineFit;

IMAGE2DALG_EXPORT int Image2DAlg_FitLine(const float* xy, int count,
                                         Image2DAlg_LineFit* out);

// ---------------- 拟合扩展（封装 ImageProcessor 21 节 + 多项式） ----------------

/// 圆拟合（高精度版）：Kåsa 初值 + Gauss-Newton 几何精化（比 FitCircle 的
/// 纯代数解残差更小）。退化（共线/点数 < 3）→ -3。
IMAGE2DALG_EXPORT int Image2DAlg_FitCircleEx(const float* xy, int count,
                                             Image2DAlg_CircleFit* out);

/// 圆拟合（RANSAC 抗外点）：毛刺/粘连外点场景（纯最小二乘会被外点拉偏）。
/// [in]  threshold    内点判定：点到圆周距离 <= threshold（像素；<= 0 → -3）
/// [in]  max_iters    RANSAC 迭代上限（< 1 按 1000）
/// [in]  seed         随机种子（固定可复现）
/// [out] out_inliers 内点索引数组（malloc，用 FreeInliers 释放；可为 NULL 不接收）
IMAGE2DALG_EXPORT int Image2DAlg_FitCircleRobust(const float* xy, int count, double threshold,
                                                 int max_iters, unsigned int seed,
                                                 Image2DAlg_CircleFit* out,
                                                 int** out_inliers, int* out_inlier_count);

IMAGE2DALG_EXPORT void Image2DAlg_FreeInliers(int* inliers);

/// 椭圆拟合（cv::fitEllipse 代数拟合）。
/// [in]  count >= 5（否则 -3）
/// [out] out   中心 / 全轴长（非半轴）/ 旋转角（OpenCV RotatedRect 约定）
typedef struct Image2DAlg_EllipseFit {
    double cx, cy;        // 中心（像素）
    double width, height; // 长短轴全长（像素）
    double angle;         // 旋转角（度，OpenCV 约定）
} Image2DAlg_EllipseFit;

IMAGE2DALG_EXPORT int Image2DAlg_FitEllipse(const float* xy, int count,
                                            Image2DAlg_EllipseFit* out);

/// 多目标椭圆拟合（椭圆焊盘 / 异形孔测量）：二值前景逐外轮廓做
/// cv::fitEllipse 代数拟合（少 5 点的轮廓静默跳过）。
/// [in]  in            二值语义：非零像素为前景
/// [in]  min/max_area  面积过滤（像素²，同 ConnectedComponents）
/// [in]  min_axis/max_axis  全轴长过滤（像素；对 width/height 的较大者判定；
///                         max_axis <= 0 表示不设上限）
/// [out] out_ellipses  malloc 数组（用 FreeEllipses 释放）
/// [out] out_count     数量；无结果时 *out_ellipses = NULL、*out_count = 0、返回 0
IMAGE2DALG_EXPORT int Image2DAlg_FitEllipsesFromBinary(const Image2DAlg_Image* in,
                                                       int min_area, int max_area,
                                                       double min_axis, double max_axis,
                                                       Image2DAlg_EllipseFit** out_ellipses,
                                                       int* out_count);

/// 释放椭圆拟合结果数组（直接 free 指针本身；NULL 安全；调用方自行置空）。
IMAGE2DALG_EXPORT void Image2DAlg_FreeEllipses(Image2DAlg_EllipseFit* ellipses);

/// 最小外接矩形拟合（旋转矩形）。
/// [in]  count >= 3（否则 -3）
typedef struct Image2DAlg_RectFit {
    double cx, cy;        // 中心（像素）
    double width, height; // 边长（像素）
    double angle;         // 旋转角（度，OpenCV RotatedRect 约定）
} Image2DAlg_RectFit;

IMAGE2DALG_EXPORT int Image2DAlg_FitRect(const float* xy, int count, Image2DAlg_RectFit* out);

/// 多项式拟合 y = Σ coef[i]·x^i（最小二乘，SVD 求解）。
/// [in]  degree  1~5（越界 → -3）；count >= degree + 1（否则 -3）
/// [out] out     coef 升幂（coef[0] + coef[1]x + ... ），未用项为 0；rms 残差
/// 用途：边缘直线度 / 翘曲趋势线。
typedef struct Image2DAlg_PolyFit {
    double coef[6];   // 升幂系数 coef[i]·x^i
    int degree;       // 实际拟合阶数
    double rms;       // 残差均方根（y 方向，与坐标同单位）
} Image2DAlg_PolyFit;

IMAGE2DALG_EXPORT int Image2DAlg_FitPoly(const float* xy, int count, int degree,
                                         Image2DAlg_PolyFit* out);

// ---------------- 霍夫检测（封装 ImageProcessor 23 节） ----------------

/// 概率霍夫直线检测（线段端点输出）。
/// [in]  in               二值/边缘图（先 Canny）
/// [in]  rho              距离分辨率（像素，典型 1）
/// [in]  theta            角度分辨率（度，典型 1 → 内部转弧度）
/// [in]  threshold        投票阈值（越大越严）
/// [in]  min_line_length  最短线长（<= 0 不限）
/// [in]  max_line_gap     线最大断距（<= 0 不限）
/// [out] out_lines        malloc 数组（用 FreeLines 释放）
/// [out] out_count        数量；无检出时 *out_lines = NULL、*out_count = 0、返回 0
typedef struct Image2DAlg_Line4 {
    int x1, y1, x2, y2;   // 线段端点
} Image2DAlg_Line4;

IMAGE2DALG_EXPORT int Image2DAlg_HoughLinesP(const Image2DAlg_Image* in,
                                             double rho, double theta_deg, int threshold,
                                             int min_line_length, int max_line_gap,
                                             Image2DAlg_Line4** out_lines, int* out_count);

/// 概率霍夫直线（ROI 版）：仅在 ROI 内检测，线段端点平移回全图坐标系。
/// [in]  roi_x/y/w/h  同 ConnectedComponentsROI（w/h <= 0 = 全图；无交集 → -3）
IMAGE2DALG_EXPORT int Image2DAlg_HoughLinesPROI(const Image2DAlg_Image* in,
                                                int roi_x, int roi_y, int roi_w, int roi_h,
                                                double rho, double theta_deg, int threshold,
                                                int min_line_length, int max_line_gap,
                                                Image2DAlg_Line4** out_lines, int* out_count);

/// 霍夫圆检测（HOUGH_GRADIENT）。
/// [in]  in          灰度语义图（彩色先转灰度；检测前内部已建议先模糊——
///                   本接口不做预模糊，锐利二值图可能一无所获，调用方先接 Blur 节点）
/// [in]  dp          分辨率倒数（1 = 与源图同分辨率）
/// [in]  min_dist    圆心最小间距（防重复检出）
/// [in]  param1      内部 Canny 高阈值
/// [in]  param2      圆心投票阈值（越小圆越多）
/// [in]  min_r/max_r 半径范围（max_r <= 0 = 不限）
/// [out] out_circles malloc 数组（用 FreeCirclesHough 释放）
typedef struct Image2DAlg_HoughCircle {
    double cx, cy, r;
} Image2DAlg_HoughCircle;

IMAGE2DALG_EXPORT int Image2DAlg_HoughCircles(const Image2DAlg_Image* in,
                                              double dp, double min_dist,
                                              double param1, double param2,
                                              double min_r, double max_r,
                                              Image2DAlg_HoughCircle** out_circles,
                                              int* out_count);

/// 霍夫圆（ROI 版）：仅在 ROI 内检测，圆心平移回全图坐标系。
/// [in]  roi_x/y/w/h  同 ConnectedComponentsROI（w/h <= 0 = 全图；无交集 → -3）
IMAGE2DALG_EXPORT int Image2DAlg_HoughCirclesROI(const Image2DAlg_Image* in,
                                                 int roi_x, int roi_y, int roi_w, int roi_h,
                                                 double dp, double min_dist,
                                                 double param1, double param2,
                                                 double min_r, double max_r,
                                                 Image2DAlg_HoughCircle** out_circles,
                                                 int* out_count);

/// 释放霍夫线段数组（直接 free 指针本身；NULL 安全；调用方自行置空）。
IMAGE2DALG_EXPORT void Image2DAlg_FreeLines(Image2DAlg_Line4* lines);

/// 释放霍夫圆数组（直接 free 指针本身；NULL 安全；调用方自行置空）。
IMAGE2DALG_EXPORT void Image2DAlg_FreeCirclesHough(Image2DAlg_HoughCircle* circles);

// ---------------- 区域统计（封装 ImageProcessor 17/22 节，批次一致性监控） ----------------

/// 区域灰度统计（均值/标准差/最值；ROI 内建，w/h <= 0 = 全图）。
/// [in]  in            灰度语义：彩色先转灰度
/// [in]  roi_x/y/w/h   目标区域（越界钳制；与图像无交集 → -3）
/// [out] out           mean/stddev（OpenCV 定义）/ min / max（灰度 0~255）
typedef struct Image2DAlg_GrayStats {
    double mean;
    double stddev;
    int min;
    int max;
} Image2DAlg_GrayStats;

IMAGE2DALG_EXPORT int Image2DAlg_RegionStats(const Image2DAlg_Image* in,
                                             int roi_x, int roi_y, int roi_w, int roi_h,
                                             Image2DAlg_GrayStats* out);

/// 灰度直方图（256 bins，无掩膜）。
/// [out] out_hist  调用方分配的 int[256]，各灰度级像素计数
/// 彩色输入先转灰度。
IMAGE2DALG_EXPORT int Image2DAlg_Histogram(const Image2DAlg_Image* in, int out_hist[256]);

/// 直方图相似度（cv::compareHist）。
/// [in]  hist1/hist2  各自由 Histogram 输出的 256 bins 计数
/// [in]  method       0 相关 CORREL（1=同）/ 1 卡方 CHISQR（0=同）/
///                    2 相交 INTERSECT（越大越像）/ 3 巴氏 BHATTACHARYYA（0=同）
///                    越界 → -3
IMAGE2DALG_EXPORT int Image2DAlg_CompareHist(const int* hist1, const int* hist2, int method,
                                             double* out_score);

/// 形状相似度（Hu 矩距离，越小越相似，0 = 完全一致）。
/// [in]  xy1/count1, xy2/count2  两组轮廓点（各 >= 3，内部按像素取整）
/// [in]  method   1/2/3 对应 cv::CONTOURS_MATCH_I1~I3（常用 1；越界 → -3）
/// [out] out_score
IMAGE2DALG_EXPORT int Image2DAlg_MatchShapes(const float* xy1, int count1,
                                             const float* xy2, int count2,
                                             int method, double* out_score);

/// 凸性缺陷（凸包与轮廓间的凹陷）：引脚变形 / 缺料检测——深度超阈值即缺陷。
/// [in]  xy/count      轮廓点集（count >= 3，否则 -3）
/// [out] out_defects   malloc 数组（用 Image2DAlg_FreeDefects 释放）
/// [out] out_count     数量；无凹陷（凸形）时 *out_defects = NULL、*out_count = 0、仍返回 0
/// 点序自适应：内部按有向面积归一化遍历方向（顺时针轮廓先反转再计算），
/// 返回的 *index 已统一映射回【输入点序】（start/end/farthest 皆然，可安全用于回切输入点集）。
/// 输入须为简单多边形遍历序（findContours 输出天然满足）；自交/乱序点集会触发
/// OpenCV 断言、以 -99 返回（见错误码）。共线等退化输入（凸包 <3 点）按无凹陷
/// 返回 0/NULL/0。整数轮廓上深度 <~0.5px 的微缺陷属取整噪声，可按 depth 过滤。
typedef struct Image2DAlg_Defect {
    int start_index;       // 凹陷段起始顶点（输入点序）
    int end_index;         // 凹陷段结束顶点
    int farthest_index;    // 最深点顶点
    double depth;          // 凹陷深度（像素，已由 OpenCV 定点数 /256 还原）
    double fx, fy;         // 最深点坐标
} Image2DAlg_Defect;

IMAGE2DALG_EXPORT int Image2DAlg_ConvexityDefects(const float* xy, int count,
                                                  Image2DAlg_Defect** out_defects,
                                                  int* out_count);

/// 多目标圆拟合（AOI 孔位 / 焊球阵列测量）：二值前景逐外轮廓做 Kåsa 拟合。
/// [in]  in           二值语义：非零像素为前景；subpixel != 0 时另用其灰度梯度细化边缘
/// [in]  min/max_area 面积过滤（像素，闭区间；同 ConnectedComponents）
/// [in]  min_r/max_r  半径过滤（像素；max_r <= 0 表示不设上限）
/// [in]  subpixel     != 0 启用灰度梯度亚像素边缘细化：内部 3x3 高斯预平滑 →
///                    Sobel 梯度 → 沿梯度方向 3 点抛物线插值取 |梯度| 峰值 →
///                    亚像素边缘点重新拟合（抗锯齿边缘典型精度 ~0.3 像素）
/// [out] out_circles  malloc 数组（用 Image2DAlg_FreeCircles 释放）
/// [out] out_count    数量；无结果时 *out_circles = NULL、*out_count = 0、仍返回 0
/// 退化轮廓（共线/重合、点数 < 3）静默跳过，不判错误。
IMAGE2DALG_EXPORT int Image2DAlg_FitCirclesFromBinary(const Image2DAlg_Image* in,
                                                      int min_area, int max_area,
                                                      double min_r, double max_r,
                                                      int subpixel,
                                                      Image2DAlg_CircleFit** out_circles,
                                                      int* out_count);

/// 释放圆拟合结果数组（直接 free 指针本身；NULL 安全；调用方自行置空）。
IMAGE2DALG_EXPORT void Image2DAlg_FreeCircles(Image2DAlg_CircleFit* circles);

/// 释放测量结果数组（直接 free 指针本身；NULL 安全；调用方自行置空）。
IMAGE2DALG_EXPORT void Image2DAlg_FreeBlobs(Image2DAlg_Blob* blobs);
IMAGE2DALG_EXPORT void Image2DAlg_FreeContours(Image2DAlg_ContourInfo* contours);
IMAGE2DALG_EXPORT void Image2DAlg_FreeDefects(Image2DAlg_Defect* defects);

// ---------------- 几何变换（标定对齐 / 坐标校正用） ----------------

/// 仿射变换：M 为 src→dst 的前向映射（cv::warpAffine 默认语义，非 WARP_INVERSE_MAP）——
///                      dst(x,y) 采样自 src(M⁻¹·[x,y,1]ᵀ)，越界填 0。可直接使用
/// [in]  m              2x3 矩阵行主序 [m00,m01,m02,m10,m11,m12]（6 元素，可为
///                      cv::getRotationMatrix2D / getAffineTransform 的输出）
/// [in]  out_w/out_h   输出画布尺寸（<= 0 → -3）
/// [in]  interpolation  同 Resize
/// 越出源图的部分填 0（黑）。输出通道与输入一致。
IMAGE2DALG_EXPORT int Image2DAlg_WarpAffine(const Image2DAlg_Image* in,
                                            const double* m,   /* 6 元素 */
                                            int out_width, int out_height,
                                            int interpolation,
                                            Image2DAlg_Image* out);

/// 透视变换：dst(x,y) = 齐次投影 src(M · [x,y,1]ᵀ)。
/// [in]  m              3x3 矩阵行主序（9 元素，可为 cv::getPerspectiveTransform 输出）
/// [in]  out_w/out_h   输出画布尺寸（<= 0 → -3）
/// [in]  interpolation  同 Resize
/// 越出源图的部分填 0（黑）。输出通道与输入一致。
IMAGE2DALG_EXPORT int Image2DAlg_WarpPerspective(const Image2DAlg_Image* in,
                                                 const double* m,   /* 9 元素 */
                                                 int out_width, int out_height,
                                                 int interpolation,
                                                 Image2DAlg_Image* out);

// ---------------- 特征点查找（Mark 点自动定位 / 大形变对位前置） ----------------

/// 二维浮点点集容器（角点/特征点/多边形顶点）。
typedef struct Image2DAlg_Point2f {
    float x, y;
} Image2DAlg_Point2f;

/// 释放点集数组（直接 free 指针本身；NULL 安全；调用方自行置空）。
IMAGE2DALG_EXPORT void Image2DAlg_FreePoint2f(Image2DAlg_Point2f* pts);

/// Harris 角点检测（响应阈值取相对最大响应的比例，3x3 邻域 NMS 去重）。
/// [in]  block_size   邻域窗口边长（奇数，>= 3，典型 5）
/// [in]  ksize        Sobel 求导核边长（奇数，>= 3，典型 3）
/// [in]  k            Harris 常数（典型 0.04~0.06）
/// [in]  thresh_ratio 响应阈值比例 [0,1]：仅保留响应 >= ratio*最大响应 的角点
///                    （0 = 保留全部局部极大值；典型 0.01~0.1）
/// [in]  subpix       != 0 做角点亚像素细化（±0.5px，需图内有梯度结构）
/// [out] out_points   malloc 数组（用 FreePoint2f 释放）；
///                    无检出时 *out_points = NULL、*out_count = 0、仍返回 0
///                    （纯平坦图无梯度结构无检出属正常）。彩色输入先转灰度。
IMAGE2DALG_EXPORT int Image2DAlg_HarrisCorners(const Image2DAlg_Image* in,
                                               int block_size, int ksize, double k,
                                               double thresh_ratio, int subpix,
                                               Image2DAlg_Point2f** out_points, int* out_count);

/// Shi-Tomasi 角点（goodFeaturesToTrack）：按质量降序、内置最小间距 NMS。
/// [in]  max_corners    最大角点数（>= 1，否则 -3）
/// [in]  quality_level  质量系数 (0,1]：相对最强角点响应的比例（典型 0.01~0.1）
/// [in]  min_distance   角点最小间距（像素，>= 1）
/// [in]  subpix         同 HarrisCorners
IMAGE2DALG_EXPORT int Image2DAlg_GoodFeatures(const Image2DAlg_Image* in, int max_corners,
                                              double quality_level, double min_distance,
                                              int subpix,
                                              Image2DAlg_Point2f** out_points, int* out_count);

/// 角点亚像素细化（梯度法，需初始角点附近有梯度结构）。
/// [in]  pts/count   初始角点（如 HarrisCorners/GoodFeatures 的输出）
/// [in]  win_size    搜索窗口边长（奇数，>= 3，典型 5~11）
/// [out] out_points  malloc 数组（用 FreePoint2f 释放），细化失败的位置保持原值
IMAGE2DALG_EXPORT int Image2DAlg_CornerSubPix(const Image2DAlg_Image* in,
                                              const Image2DAlg_Point2f* pts, int count,
                                              int win_size, Image2DAlg_Point2f** out_points);

// ---------------- 多边形拟合（形状判定基础） ----------------

/// 轮廓顶点逼近（Douglas-Peucker，epsilon 为像素容差）：矩形/多边形形状判定——
/// epsilon 内删冗余顶点（矩形轮廓 → 4 角点）。
/// [in]  xy/count   轮廓点（count >= 3，否则 -3）
/// [in]  epsilon    逼近容差（像素；0 = 仅去除共线冗余）
/// [in]  closed     != 0 = 闭合轮廓（findContours 输出）
/// [out] out_xy     malloc 扁平点集 [x0,y0,...]（用 FreeContourPoints 释放）
/// [out] out_count  逼近后顶点个数
IMAGE2DALG_EXPORT int Image2DAlg_ApproxPolyDP(const float* xy, int count, double epsilon,
                                              int closed, float** out_xy, int* out_count);

/// 多目标旋转矩形拟合（异形件 / 料口测量）：二值前景逐外轮廓 minAreaRect。
/// [in]  in          二值语义：非零像素为前景
/// [in]  min/max_area 面积过滤（同 ConnectedComponents）
/// [out] out_rects   malloc 数组（用 FreeRectFits 释放）；无结果时 NULL/0/返回 0
IMAGE2DALG_EXPORT int Image2DAlg_FitRectsFromBinary(const Image2DAlg_Image* in,
                                                    int min_area, int max_area,
                                                    Image2DAlg_RectFit** out_rects,
                                                    int* out_count);

IMAGE2DALG_EXPORT void Image2DAlg_FreeRectFits(Image2DAlg_RectFit* rects);

/// 多目标直线拟合（板边 / 边缘线测量）：二值前景逐外轮廓做 DIST_L2 直线拟合。
/// [in]  in          二值语义：非零像素为前景（彩色先转灰度）
/// [in]  min/max_area 面积过滤（同 ConnectedComponents）
/// [in]  min_length   直线覆盖跨度过滤（像素：轮廓点在方向上的投影跨度；
///                    <= 0 表示不设下限）
/// [out] out_lines    malloc 数组（用 FreeLineFits 释放）；无结果时 NULL/0/返回 0
/// [out] LineFit.rms  点到直线垂直距离的 RMS（直线度指标）
IMAGE2DALG_EXPORT int Image2DAlg_FitLinesFromBinary(const Image2DAlg_Image* in,
                                                    int min_area, int max_area,
                                                    double min_length,
                                                    Image2DAlg_LineFit** out_lines,
                                                    int* out_count);

IMAGE2DALG_EXPORT void Image2DAlg_FreeLineFits(Image2DAlg_LineFit* lines);


// ---------------- 特征匹配对位（ORB，大形变场景） ----------------

/// ORB 特征对位：特征点查找 + 描述子匹配（Hamming + Lowe 比率测试）+
/// RANSAC 仿射估计——弥补模板匹配（只适合小形变），旋转/尺度/亮度变化鲁棒。
/// [in]  in1/in2       两张灰度语义图（彩色先转灰度）
/// [in]  n_features    ORB 特征点上限（典型 500，<= 0 → -3）
/// [in]  ratio         Lowe 比率测试系数（典型 0.7~0.8；(0,1) 越界 → -3）
/// [out] m             仿射 2x3 行主序（in1 → in2，RANSAC 内点最小二乘）
/// [out] out_matches   malloc 匹配点对数组（用 FreeOrbMatches 释放）：
///                     x1/y1 = 图1 特征点，x2/y2 = 图2 对应点，distance = Hamming
/// [out] out_count     比率测试后匹配对数
/// [out] out_inliers   RANSAC 内点数（仿射质量指标：>= 10 视为可靠）
/// 返回：0 成功；-2 特征/匹配不足（< 3 对或无法求解仿射——平坦图属正常）；
///       其余错误码同全局约定。
typedef struct Image2DAlg_OrbMatch {
    double x1, y1;    // 图1 特征点
    double x2, y2;    // 图2 对应特征点
    double distance;  // 描述子 Hamming 距离（越小越相似）
} Image2DAlg_OrbMatch;

IMAGE2DALG_EXPORT int Image2DAlg_ORBAlign(const Image2DAlg_Image* in1,
                                          const Image2DAlg_Image* in2,
                                          int n_features, double ratio,
                                          double m[6],
                                          Image2DAlg_OrbMatch** out_matches, int* out_count,
                                          int* out_inliers);

IMAGE2DALG_EXPORT void Image2DAlg_FreeOrbMatches(Image2DAlg_OrbMatch* matches);


// ---------------- 特征链路拆分（检测 / 描述 / 匹配 / 几何估计，2.1.0 新增） ----------------

/// FAST 角点检测（圆周连续像素比较，比 Harris 快一个数量级，适合实时/视频前处理）。
/// 彩色输入先转灰度。
/// [in]  threshold           灰度差阈值（典型 10~40；<= 0 → -3）
/// [in]  nonmax_suppression  非极大值抑制开关（0 关闭；非 0 开启去重）
/// [out] out_points          malloc 数组（用 Image2DAlg_FreePoint2f 释放）；
///                           无检出时 *out_points = NULL、*out_count = 0、仍返回 0
///                           （纯平坦图无检出属正常，不是错误）
/// [out] out_count           检出点数
/// 返回：0 成功；-1 空指针/空图；-3 参数越界；-4 内存不足；-99 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_FastFeatures(const Image2DAlg_Image* in,
                                              int threshold, int nonmax_suppression,
                                              Image2DAlg_Point2f** out_points, int* out_count);

/// 描述子容器（Image2DAlg_ComputeDescriptors 的输出；data/fdata 由本库 malloc）。
///   type      0=ORB 1=BRISK 2=AKAZE 3=SIFT（与 ComputeDescriptors 的 type 参数一致）
///   dim       单描述子字节数（二进制）或 float 维数（SIFT 浮点）
///   is_float  1 = 浮点描述子（SIFT，读 fdata）；0 = 二进制描述子（读 data）
///   count     描述子个数（= 输入关键点数）
///   data      is_float=0 时有效：count*dim 字节
///   fdata     is_float=1 时有效：count*dim 个 float
typedef struct Image2DAlg_Descriptors {
    int type;             // 0=ORB 1=BRISK 2=AKAZE 3=SIFT
    int dim;              // 单描述子字节数（二进制）或 float 维数（SIFT）
    int is_float;         // 1=SIFT 浮点描述子
    int count;            // 描述子个数（= 输入关键点数）
    unsigned char* data;  // is_float=0：count*dim 字节
    float* fdata;         // is_float=1：count*dim 个 float（与 data 二选一）
} Image2DAlg_Descriptors;

/// 关键点描述子计算（检测与描述解耦：关键点可来自 FAST/Harris/GoodFeatures
/// 或外部跟踪器；一次检测可多次匹配）。彩色输入先转灰度。
/// [in]  in          灰度语义图
/// [in]  pts/count   关键点（count >= 1，否则 -3；坐标须在图像内，越界 → -2；
///                   贴边点由本库复制边框扩图计算，可正常出描述子——-2 仅在
///                   算子仍丢弃关键点时返回）
/// [in]  type        0=ORB(二进制 32 字节) / 1=BRISK(二进制 64 字节) /
///                   2=AKAZE(二进制 61 字节) / 3=SIFT(浮点 128 维)；
///                   越界 → -3。注意：匹配时两套描述子 type 必须严格一致
/// [in]  n_features  仅 ORB 有效（<= 0 取默认 500，同 ORBAlign）；其他类型忽略
/// [out] out         调用方分配的结构体，本库填充字段；data/fdata 用
///                   Image2DAlg_FreeDescriptors 释放（结构体本身归调用方）。
///                   内存归属注记：重复调用前须先 FreeDescriptors（本库不释放
///                   旧缓冲，否则泄漏）；校验失败（-1/-2/-3）不动 out 原有内容
/// 返回：0 成功；-1 空指针/空图；-2 关键点越界/算子丢弃导致无法计算描述子；
///       -3 参数/枚举越界；-4 内存不足；-99 OpenCV 异常。
/// 注：SIFT 自 OpenCV 4.4.0 起进入主库 features2d（专利到期），本库
///     opencv_world440 已导出 cv::SIFT::create，可直接使用（无需 contrib/nonfree）；
///     AKAZE 对输入关键点统一按基尺度（原始分辨率尺度空间层）计算描述子。
IMAGE2DALG_EXPORT int Image2DAlg_ComputeDescriptors(const Image2DAlg_Image* in,
                                                    const Image2DAlg_Point2f* pts, int count,
                                                    int type, int n_features,
                                                    Image2DAlg_Descriptors* out);

/// 释放描述子缓冲（free data/fdata 并各自置空；结构体本身由调用方管理；
/// NULL 安全，可重复调用）。
IMAGE2DALG_EXPORT void Image2DAlg_FreeDescriptors(Image2DAlg_Descriptors* d);

/// 描述子匹配：BFMatcher kNN(k=2) + Lowe 比率测试（保留 d1 < ratio*d2 的配对）。
/// 二进制描述子（ORB/BRISK/AKAZE）用 Hamming 距离，浮点（SIFT）用 L2 距离。
/// [in]  d1/d2        两套描述子（type/dim/is_float 必须一致，否则 -3）
/// [in]  pts1/pts2    与 d1/d2 行序一一对应的关键点（pts1[i] 即 d1 第 i 行的
///                   关键点，pts2[i] 即 d2 第 i 行——即 ComputeDescriptors 的输入
///                   点集；数组长度须 >= d1/d2 的 count，本库无法校验，不足属
///                   调用方错误）
/// [in]  ratio        Lowe 比率系数（典型 0.7~0.8；不在 (0,1) → -3）
/// [out] out_matches  malloc 匹配点对数组（复用 Image2DAlg_OrbMatch 结构，
///                   用 Image2DAlg_FreeOrbMatches 释放）：
///                   x1/y1 = 图1 关键点，x2/y2 = 图2 对应点，
///                   distance = 描述子距离（Hamming 或 L2，随 is_float）
/// [out] out_count    比率测试后匹配对数
/// 返回：0 成功；-1 空指针；-2 匹配对数 < 2（平坦图属正常）；
///       -3 参数越界或两描述子 type/dim/is_float 不一致；-4 内存不足；-99 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_MatchFeatures(const Image2DAlg_Descriptors* d1,
                                               const Image2DAlg_Descriptors* d2,
                                               const Image2DAlg_Point2f* pts1,
                                               const Image2DAlg_Point2f* pts2,
                                               double ratio,
                                               Image2DAlg_OrbMatch** out_matches,
                                               int* out_count);

/// 部分仿射估计（4 自由度：旋转 + 各向同性尺度 + 平移）——比 ORBAlign 的
/// 全仿射（6 DOF）约束更强、抗误匹配更稳，工业视觉对位首选。
/// [in]  matches/count            匹配点对（如 MatchFeatures / ORBAlign 输出；
///                                  count >= 3，否则 -2）
/// [in]  ransac_reproj_threshold  RANSAC 重投影阈值（像素，典型 3.0；<= 0 → -3）
/// [out] m                        2x3 行主序，方向 in1 → in2（与 ORBAlign 一致：
///                                  图1 点 → 图2 点，可直接接 WarpAffine）；
///                                  返回非 0 时不写入（保持调用方原值）
/// [out] out_inliers              RANSAC 内点数（>= 10 视为可靠）；校验通过后
///                                  清零，求解失败/异常时为 0
/// 返回：0 成功；-1 空指针；-2 点数不足或无法求解；-3 参数越界；
///       -99 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_EstimateAffinePartial(const Image2DAlg_OrbMatch* matches,
                                                       int count,
                                                       double ransac_reproj_threshold,
                                                       double m[6], int* out_inliers);

/// 单应矩阵估计（3x3 透视变换，配合 WarpPerspective 做平面拼接/大视角对位）。
/// [in]  matches/count            匹配点对（count >= 4，否则 -2）
/// [in]  ransac_reproj_threshold  同 EstimateAffinePartial（<= 0 → -3）
/// [out] h                        3x3 行主序，方向 in1 → in2（h[8] 归一为 1）；
///                                  返回非 0 时不写入（保持调用方原值）
/// [out] out_inliers              RANSAC 内点数；校验通过后清零，
///                                  求解失败/异常时为 0
/// 返回：0 成功；-1 空指针；-2 点数不足或退化（如所有点共线）；-3 参数越界；
///       -99 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_FindHomography(const Image2DAlg_OrbMatch* matches,
                                                int count,
                                                double ransac_reproj_threshold,
                                                double h[9], int* out_inliers);


// ---------------- 特征扩展（BRISK / AKAZE / SIFT 检测+描述、斑点、一站式匹配，2.2.0 新增） ----------------

// 【contrast_threshold 映射表】三个检测+描述一次完成接口的同一参数：
//   BriskFeatures -> BRISK::create 的 thresh（int，典型 30；<= 0 取默认 30）
//   AkazeFeatures -> AKAZE::create 的 threshold（float，典型 0.001；<= 0 取默认
//                    0.001，值越小检出越多）
//   SiftFeatures  -> SIFT::create 的 contrastThreshold（典型 0.04；<= 0 取默认
//                    0.04，越大检出越少）

/// BRISK 特征（检测 + 描述一次完成：尺度空间极值 + 二进制 64 字节描述子，
/// 旋转/尺度鲁棒、速度快）。彩色输入先转灰度。
/// [in]  n_features         特征点上限——本接口忽略（BRISK 无上限参数）
/// [in]  contrast_threshold 见上方映射表（BRISK thresh，int 取整）
/// [out] out_points         malloc 数组（用 Image2DAlg_FreePoint2f 释放）；
///                          无检出时 *out_points = NULL、*out_count = 0、仍返回 0
///                          （平坦图无检出属正常，不是错误）
/// [out] out_count          检出点数
/// [out] out_desc           调用方分配的结构体，本库填充字段（data/fdata 缓冲用
///                          Image2DAlg_FreeDescriptors 释放，结构体本身归调用方）；
///                          传 NULL = 只检测不取描述子
/// 内存契约：out_points 为二级指针（本库 malloc 配 FreePoint2f）；out_desc 为
/// 一级结构体指针（调用方分配，其 data/fdata 缓冲由本库 malloc 配 FreeDescriptors）。
/// 失败语义：-1 校验失败不写任何输出；-4/-99 时 *out_points/*out_count 已置
/// NULL/0、out_desc 已清零（本库保证失败出口无残留缓冲）。
/// 返回：0 成功；-1 空指针/空图；-4 内存不足；-99 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_BriskFeatures(const Image2DAlg_Image* in, int n_features,
                                               double contrast_threshold,
                                               Image2DAlg_Point2f** out_points, int* out_count,
                                               Image2DAlg_Descriptors* out_desc);

/// AKAZE 特征（检测 + 描述一次完成：非线性扩散尺度空间 + 二进制 61 字节描述子，
/// 弱纹理/模糊场景优于 ORB）。彩色输入先转灰度。
/// [in]  n_features         特征点上限——本接口忽略（AKAZE 无上限参数）
/// [in]  contrast_threshold 见上方映射表（AKAZE threshold，float）
/// 其余同 BriskFeatures（out_desc 为 AKAZE 描述子；关键点由 detectAndCompute
/// 自产，无 ComputeDescriptors 对外部关键点的 class_id 约束）。
IMAGE2DALG_EXPORT int Image2DAlg_AkazeFeatures(const Image2DAlg_Image* in, int n_features,
                                               double contrast_threshold,
                                               Image2DAlg_Point2f** out_points, int* out_count,
                                               Image2DAlg_Descriptors* out_desc);

/// SIFT 特征（检测 + 描述一次完成：DoG 尺度空间 + 浮点 128 维描述子，
/// 匹配质量最高、视角/尺度变化最稳）。彩色输入先转灰度。
/// [in]  n_features         特征点上限 -> SIFT::create 的 nfeatures（<= 0 不限）
/// [in]  contrast_threshold 见上方映射表（SIFT contrastThreshold）
/// 其余同 BriskFeatures（out_desc 为浮点描述子：is_float=1、fdata 有效）。
/// 注：SIFT 自 OpenCV 4.4.0 起进入主库 features2d（同 ComputeDescriptors 的说明）。
IMAGE2DALG_EXPORT int Image2DAlg_SiftFeatures(const Image2DAlg_Image* in, int n_features,
                                              double contrast_threshold,
                                              Image2DAlg_Point2f** out_points, int* out_count,
                                              Image2DAlg_Descriptors* out_desc);

/// 斑点检测（SimpleBlobDetector：灰度多阈值二值化 -> 连通域 -> 形状过滤聚合）。
/// 与 FitCirclesFromBinary 的语义区分：本接口 = 连通域形状过滤（多阈值聚合，
/// 按面积/圆度/凸性/惯性比筛选，快速定位圆点/焊盘）；FitCirclesFromBinary =
/// 二值前景逐轮廓最小二乘圆拟合（精度更高、可选亚像素细化）。快速定位用本
/// 接口，精密测量用拟合接口。
/// [in]  min_area          面积过滤下限（像素²；<= 0 = 无下限）
/// [in]  max_area          面积过滤上限（<= 0 = 不设上限，同 ConnectedComponents 约定）
/// [in]  min_circularity   最小圆度 [0,1]（> 0 启用圆度过滤；<= 0 显式关闭圆度过滤）
/// [in]  min_convexity     最小凸性 [0,1]（> 0 启用凸性过滤；<= 0 显式关闭凸性过滤）
///       其余参数用 SimpleBlobDetector 默认：暗斑（blobColor=0，亮斑请先反转）、
///       灰度阈值 50~220 步进 10、惯性比过滤 >= 0.1、重复性 >= 2。
///       注：4.4 默认 Params 的凸性过滤开启（minConvexity=0.95，会滤掉一般
///       光栅化圆的锯齿轮廓），故圆度/凸性过滤状态由本接口的参数显式决定
/// [out] out_blobs         malloc 数组（用 Image2DAlg_FreeBlobFeatures 释放）；
///                         无检出时 *out_blobs = NULL、*out_count = 0、仍返回 0；
///                         -4/-99 时同样置空（-1 校验失败不写输出）
/// [out] out_count         斑点数
/// 返回：0 成功；-1 空指针/空图；-3 通道不支持；-4 内存不足；-99 OpenCV 异常。
typedef struct Image2DAlg_BlobFeature {
    float x, y;      // 斑点中心（像素）
    float radius;    // 斑点半径（像素，直径/2——SimpleBlobDetector 的尺寸为斑点
                     // 直径，含 1~2px 光栅化偏差）
} Image2DAlg_BlobFeature;

IMAGE2DALG_EXPORT int Image2DAlg_BlobDetect(const Image2DAlg_Image* in,
                                            double min_area, double max_area,
                                            double min_circularity, double min_convexity,
                                            Image2DAlg_BlobFeature** out_blobs, int* out_count);

/// 释放斑点检测结果数组（直接 free 指针本身；NULL 安全；调用方自行置空）。
IMAGE2DALG_EXPORT void Image2DAlg_FreeBlobFeatures(Image2DAlg_BlobFeature* blobs);

/// 一站式特征检测 + 描述 + 匹配（ORBAlign 的泛化版：ORB/BRISK/AKAZE/SIFT 四选一）。
/// 内部 = 检测（detectAndCompute）+ 描述（ComputeDescriptors 同款算子工厂）+
/// 匹配（MatchFeatures 同款 BFMatcher kNN + Lowe 比率）；给不想管理描述子
/// 生命周期的调用方——需要复用描述子/关键点时用拆分版接口。
/// [in]  type        0=ORB / 1=BRISK / 2=AKAZE / 3=SIFT（同 ComputeDescriptors
///                   枚举；越界 -> -3）
/// [in]  n_features  特征点上限：ORB 有效（<= 0 取默认 500，与 ORBAlign 内部
///                   参数一致——ORBAlign 本身要求 > 0）；SIFT 有效（<= 0 不限）；
///                   BRISK/AKAZE 无上限参数，忽略
/// [in]  ratio       Lowe 比率系数（典型 0.7~0.8；不在 (0,1) -> -3）
/// [out] out_matches  malloc 匹配点对数组（复用 Image2DAlg_OrbMatch 结构，
///                   用 Image2DAlg_FreeOrbMatches 释放）：
///                   x1/y1 = 图1 关键点，x2/y2 = 图2 对应点，distance = 描述子距离；
///                   -4/-99 时 *out_matches/*out_count 已置 NULL/0
/// [out] out_count   比率测试后匹配对数
/// 返回：0 成功；-1 空指针/空图；-2 特征/匹配不足（任一图无检出关键点或
///       比率测试后 < 2 对——平坦图属正常）；-3 参数越界；-4 内存不足；
///       -99 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_DetectAndMatch(const Image2DAlg_Image* in1,
                                                const Image2DAlg_Image* in2,
                                                int type, int n_features, double ratio,
                                                Image2DAlg_OrbMatch** out_matches,
                                                int* out_count);


// ---------------- 跟踪、配准与立体（2.2.0 新增） ----------------

/// 金字塔 LK 光流跟踪（视频帧间点跟踪、运动分析；cv::calcOpticalFlowPyrLK）。
/// [in]  prev/next   前后两帧（彩色先转灰度；尺寸须一致，否则 -3）
/// [in]  pts/count   待跟踪点（count >= 1，否则 -3）
/// [in]  win_size    搜索窗边长（典型 21；<= 0 -> -3；偶数自动 +1）
/// [in]  max_level   金字塔层数（典型 3；< 0 -> -3）
/// [out] out_pts     跟踪后的新位置（malloc 数组，用 Image2DAlg_FreePoint2f 释放）；
///                   跟踪失败的点位置无意义，以 out_status 为准；
///                   -1/-3 校验失败不写任何输出，-4/-99 时 *out_pts 已置 NULL
/// [out] out_status  一级指针契约——调用方预分配 count 字节（本库不分配该缓冲）；
///                   status[i] 非 0 = 第 i 点跟踪成功、0 = 丢失；NULL -> -1；
///                   仅返回 0 时写入（非 0 返回保持调用方原值，不应采信）
/// 返回：0 成功；-1 空指针/空图（含 out_status 为 NULL）；-2 无（本接口不判
///       特征不足）；-3 参数越界（win_size <= 0 / max_level < 0 / count < 1 /
///       两图尺寸不一致）；-4 内存不足；-99 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_CalcOpticalFlowPyrLK(const Image2DAlg_Image* prev,
                                                      const Image2DAlg_Image* next,
                                                      const Image2DAlg_Point2f* pts, int count,
                                                      int win_size, int max_level,
                                                      Image2DAlg_Point2f** out_pts,
                                                      unsigned char* out_status);

/// ECC 图像对齐（迭代最大化增强相关系数，亚像素精度、对光照变化较稳；
/// cv::findTransformECC）。
/// [in]  templ/input   模板图与输入图（彩色先转灰度、内部转 CV_32F；
///                     尺寸须一致，否则 -3）
/// [in]  motion_type   0=平移(2x3) / 1=仿射(2x3) / 2=单应(3x3)；越界 -> -3
///                     （注意与 cv::MOTION_* 原生枚举取值不同：cv 的 1 是
///                     MOTION_EUCLIDEAN，本 API 未暴露欧氏模型）
/// [in]  max_iters     迭代上限（< 1 -> -3）
/// [in]  epsilon       收敛阈值（相邻迭代相关系数增量，典型 1e-5；<= 0 -> -3）
/// [out] m             变换矩阵行主序，OpenCV ECC 方向：模板坐标 x 处取
///                     input(m·x)——即 warpAffine(input, m, WARP_INVERSE_MAP)
///                     可将 input 对齐到 templ（单应接 WarpPerspective 同理）；
///                     motion_type 0/1 为 2x3——仅前 6 个有效，m[6..8] 清零
///                     （防误按 3x3 使用时读到残留值）；返回非 0 时 m 不写入
/// [out] out_correlation  对齐后相关系数（越接近 1 越好）；返回非 0 时不写入
/// 返回：0 成功；-1 空指针/空图；-2 不收敛/相关性不足（判定口径：OpenCV 抛出
///       ECC 不收敛异常，或返回相关系数 < 0.5——两图无结构/形变过大属正常，
///       调用方可重试）；-3 参数越界；-99 其余 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_FindTransformECC(const Image2DAlg_Image* templ,
                                                  const Image2DAlg_Image* input,
                                                  int motion_type, int max_iters,
                                                  double epsilon, double m[9],
                                                  double* out_correlation);

/// 相位相关平移估计（FFT 频域互功率谱相位斜率，对光照/亮度变化极稳、亚像素
/// 精度；cv::phaseCorrelate）。
/// [in]  a/b          两张图（彩色先转灰度、内部转 CV_32F；尺寸须一致否则 -3）
/// [out] out_dx/out_dy  平移量（b 内容相对 a 移动的像素数：b = a 平移
///                     (dx,dy) 后的图 -> 输出 (dx,dy)）；返回非 0 时不写入
/// [out] out_response  峰值响应 [0,1]（5x5 质心区域能量占比，越接近 1 越
///                     单峰可信；典型 > 0.3 可信）
/// 返回：0 成功；-1 空指针/空图；-3 尺寸不一致；-99 OpenCV 异常。
/// 注：不启用窗口函数（cv 默认 noArray），强边缘效应场景由调用方先平滑。
IMAGE2DALG_EXPORT int Image2DAlg_PhaseCorrelate(const Image2DAlg_Image* a,
                                                const Image2DAlg_Image* b,
                                                double* out_dx, double* out_dy,
                                                double* out_response);

/// 双目立体匹配（SGBM 半全局块匹配输出视差图；配合 Image2DAlg_Calib.h 的立体
/// 校正结果可转深度；cv::StereoSGBM）。
/// [in]  left/right      已校正的左右灰度图（彩色先转灰度；尺寸须一致否则 -3；
///                       视差约定 = xL - xR >= 0，右图内容相对左图向左偏移）
/// [in]  min_disparity   视差起点（可为负，通常 0）
/// [in]  num_disparities 视差搜索数（> 0 且须为 16 的倍数，否则 -3）
/// [in]  block_size      匹配块边长（奇数 >= 3；偶数自动补奇、< 3 按 3）；
///                       P1/P2 平滑惩罚按 OpenCV 推荐 8/32·block² 自动设定
/// [out] out_disparity   一级指针契约——调用方预分配并初始化的容器（沿用 ToGray
///                       的容器约定，不新增 Free 函数）：
///                       width/height 须与左右图一致、channels 须为 1、data 容量
///                       足够（width*height 字节）；不匹配 -> -3，data 为 NULL -> -1
///                       定标口径：输出 8U 灰度，像素值 = 整数视差（SGBM 的
///                       CV_16S 定点值 / 16 四舍五入，钳到 [0,255]）；无效视差
///                       （SGBM 以 (min_disparity-1)*16 填充）-> 0——按标记值
///                       显式比对，任意 min_disparity 下无效像素恒为 0；
///                       返回非 0 时 out_disparity->data 保持调用方内容未写
/// 返回：0 成功；-1 空指针/空图（含 out_disparity->data 为 NULL）；-3 参数越界
///       （num_disparities <= 0 或非 16 倍数、两图尺寸不一致、out 容器
///       尺寸/通道不匹配）；-4 内存不足；-99 OpenCV 异常。
/// 注：SGBM 搜索窗随视差数向左越出右图边界——左图 x < min_disparity +
///     num_disparities 的条带内视差不可靠（OpenCV SGBM 特性），可靠视差区
///     大致从 x >= min_disparity + num_disparities 开始。
IMAGE2DALG_EXPORT int Image2DAlg_StereoMatch(const Image2DAlg_Image* left,
                                             const Image2DAlg_Image* right,
                                             int min_disparity, int num_disparities,
                                             int block_size,
                                             Image2DAlg_Image* out_disparity);

/// 2D-3D 位姿估计（已知 3D 物点与 2D 投影 + 相机内参求相机位姿；cv::solvePnP，
/// 采用默认迭代法 SOLVEPNP_ITERATIVE）。
/// [in]  object_points  3N 扁平 double [x0,y0,z0,x1,y1,z1,...]（物点，世界系）
/// [in]  image_points   对应 2D 投影（与 object_points 按序一一对应）
/// [in]  count          点数（>= 4，否则 -2；3 点欠定）
/// [in]  camera_matrix  内参 3x3 行主序 [fx,0,cx, 0,fy,cy, 0,0,1]
/// [in]  dist_coeffs    畸变系数（可为 NULL；dist_count <= 0 视为无畸变）
/// [in]  dist_count     畸变系数个数（0 = 无畸变；OpenCV 4/5/8/12/14 项模型，
///                      其余正值 → -3）
/// [out] rvec/tvec      旋转向量（Rodrigues，可用 calib3d::Rodrigues 转矩阵）
///                      与平移（世界系 -> 相机系）；返回非 0 时不写入
/// 返回：0 成功；-1 空指针；-2 点数不足（< 4）或求解失败（退化配置）；
///       -3 dist_count 非 0/4/5/8/12/14；-99 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_SolvePnP(const double* object_points,  /* 3N */
                                          const Image2DAlg_Point2f* image_points,
                                          int count, const double camera_matrix[9],
                                          const double* dist_coeffs, int dist_count,
                                          double rvec[3], double tvec[3]);

// ---------------- 滤波扩展（2.3.0 新增） ----------------
// 注：中值/双边/高斯已由 Image2DAlg_Blur 的 method 0/1/2 覆盖（含 4ch 双边
// 内部拆 BGR 保 alpha），本节只补 Blur 未覆盖的导数类、自定义核滤波与非局部
// 均值降噪（NLMeans 块匹配加权去噪，语义不同于局部滤波），避免同一语义
// 多入口漂移。

/// Laplacian 算子（二阶导：边缘/纹理增强）。
/// [in]  ksize   核边长（1/3/5/7，奇数；其余 -> -3）
/// [in]  scale   结果缩放系数（>0；1 = 原始值）
/// [out] out     同 MedianBlur 容器约定。深度规则：浮点输入 -> 同深度（保留
///               符号）；8U/16U 输入 -> CV_16S 中间结果取绝对值 × scale 后
///               饱和回原深度（负响应折叠为正，工业边缘增强常用口径）
/// 返回：0 成功；-1 空指针；-3 ksize 非法/scale <= 0/容器不匹配；-99 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_Laplacian(const Image2DAlg_Image* in, int ksize,
                                           double scale, Image2DAlg_Image* out);

/// 自定义卷积核（filter2D：任意线性滤波，锐化/模板匹配核/边缘核）。
/// [in]  kernel   float 数组，ksize*ksize 个元素行主序（锚点 = 核中心）
/// [in]  ksize    核边长（奇数，1~51；核元素全 0 也合法——输出全 0 图）
/// [out] out      同 MedianBlur 容器约定。深度规则：8U/16U 输入 -> CV_16S
///                中间结果取绝对值后饱和回原深度；浮点输入 -> 同深度（保留符号）
/// 返回：0 成功；-1 空指针；-3 ksize 非法（<1、>51、偶数）/kernel 为 NULL/
///       容器不匹配；-99 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_Filter2D(const Image2DAlg_Image* in,
                                          const float* kernel, int ksize,
                                          Image2DAlg_Image* out);

/// 非局部均值降噪（NLMeans：cv::fastNlMeansDenoising / fastNlMeansDenoisingColored，
/// 块匹配加权平均去高斯白噪声，保细节优于局部滤波——与 Blur 双边互补的另一条
/// 降噪路径，Blur 未覆盖此语义故单设入口）。
/// [in]  in              输入图（1 灰度 / 3 BGR / 4 BGRA；其余 -> -3）
/// [in]  h               亮度分量滤波强度（>0；常用 3~10：大 = 去噪强但细节损失
///                       多，小 = 保细节但有残留噪声；建议默认 3）
/// [in]  h_color         彩色分量强度（>0；仅 3/4 通道生效——内部转 CIELAB 后
///                       L 分量用 h、AB 分量用 h_color，通常取 = h）
/// [in]  template_window 模板窗边长（块尺寸；奇数，偶数/过小自动补奇下限 3，
///                       默认建议 7）
/// [in]  search_window   搜索窗边长（取相似块的搜索范围；奇数，偶数自动补奇；
///                       调用值须 > template_window，补奇后仍保证严格大于；
///                       默认建议 21，性能随其线性下降）
/// [out] out             同 MedianBlur 容器约定：malloc 新紧排缓冲（width/height/
///                       channels 同输入），用 Image2DAlg_FreeImage 释放；返回
///                       非 0 时 out->data 保持调用方内容未写。路由规则：
///                       1 通道 -> fastNlMeansDenoising；3 通道 ->
///                       fastNlMeansDenoisingColored；4 通道 BGRA 内部拆 BGR 走
///                       Colored 后回贴原 alpha（同 Blur 4ch 双边先例）。
///                       输入容器恒 8U 紧排（data 为 unsigned char*，全模块统一
///                       口径；cv photo 模块 NLMeans 支持 CV_8U/CV_16U，32F 不适用）
/// 返回：0 成功；-1 空指针/空图（in/out 为 NULL、data 为空、宽高 <= 0）；
///       -3 h <= 0、h_color <= 0、search_window <= template_window 或容器不匹配
///       （通道非 1/3/4）；-99 OpenCV 异常。
IMAGE2DALG_EXPORT int Image2DAlg_DenoiseNLMeans(const Image2DAlg_Image* in,
                                                double h, double h_color,
                                                int template_window, int search_window,
                                                Image2DAlg_Image* out);

#ifdef __cplusplus
}
#endif

#endif // IMAGE2DALG_API_H
