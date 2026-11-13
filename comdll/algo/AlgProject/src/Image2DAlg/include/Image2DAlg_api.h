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

IMAGE2DALG_EXPORT void Image2DAlg_FreeLines(Image2DAlg_Line4* lines);
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
/// 点序自适应：内部保证逆时针后再算凸包，若输入为顺时针会先反转，
/// 返回的 *index 均以调整后的点序为准。
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

/// 仿射变换：dst(x,y) = src(M · [x,y,1]ᵀ)。
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

#ifdef __cplusplus
}
#endif

#endif // IMAGE2DALG_API_H
