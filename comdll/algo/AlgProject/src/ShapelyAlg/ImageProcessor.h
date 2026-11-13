#ifndef SHAPELY_IMAGE_PROCESSOR_H
#define SHAPELY_IMAGE_PROCESSOR_H

// shapely::ImageProcessor —— 二维图像基础算子模块（纯静态工具类）。
// 设计约束（见技术交底书）：
//   * 公共头文件仅包含 <opencv2/core/mat.hpp>（cv::Mat 定义所需），严禁 imgproc；
//   * 枚举参数降级为原生 int（业务层传 OpenCV 枚举发生隐式转换）；
//   * 算法实现全部下沉 .cpp（imgproc 只在 .cpp 内 include）；
//   * 零拷贝：const cv::Mat& 输入，ROI/瓦片共享底层内存（引用计数）；
//   * 无业务语义：只输出矩阵/点集；
//   * 命名规范：分类前缀 + 动作（threshold/dilate/filter/canny/...）。

#include <opencv2/core/mat.hpp>

#include "AlgExport.h"

#include <string>
#include <vector>

namespace shapely {

// 多目标匹配结果项（findAllMatches 输出；loc = 模板左上角位置）
struct Match2d {
    cv::Point2f loc;      // 匹配位置
    double score = 0.0;   // 得分（语义随 method：SQDIFF 系越小越好，其余越大越好）
};

// 旋转模板匹配结果项（findRotatedMatch 输出；loc = 模板中心落点，便于对位输出）
struct RotatedMatch2d {
    cv::Point2f loc;      // 模板中心落在 image 上的位置（亚像素）
    double angle = 0.0;   // 命中角度（度）
    double score = 0.0;   // 得分（语义随 method）
};

// 图像金字塔瓦片（OpenCV 原生结构，零第三方依赖）
//
// 语义契约（务必遵守，否则边缘/跨层场景会出错）：
//   1. image 为浅拷贝，共享源图内存；连续性不保证，裸指针遍历前必须
//      检查 image.isContinuous() 或 clone()。
//   2. roi = 设计网格矩形（当前层坐标系），width/height 恒为 tile_size，
//      位置 = (col*tile_size.width, row*tile_size.height)，仅用于索引。
//   3. covered_roi = roi 与当前层图像边界的交集，是"实际有像素"的区域。
//      命中测试、跨层映射一律用 covered_roi，不要用 roi。
//   4. roi_base = covered_roi 映射到 level 0 坐标系的结果（2 幂 scale）。
//   5. scale = 当前层相对 level 0 的缩放比（典型为 1/(1<<level)）。
//   6. 生命周期：源图由调用方持有；image 的 cv::Mat 引用计数保证像素存活。
struct Tile {
    cv::Mat image;        // 瓦片图像（浅拷贝，零拷贝）
    cv::Rect roi;         // 设计网格矩形（当前层坐标）
    cv::Rect covered_roi; // 实际覆盖区域（roi ∩ 图像边界）
    cv::Rect roi_base;    // covered_roi 映射到 level 0（2 幂 scale 用）
    double scale = 1.0;   // 相对 level 0 的缩放比
    int level = 0;        // 金字塔层级
};

class SHAPELYALG_EXPORT ImageProcessor
{
public:
    ImageProcessor() = delete;   // 纯静态工具类

    // ---------------- 1. 阈值与二值化 ----------------

    /// 全局阈值二值化（cv::threshold 透传）。
    /// [in]  src     输入图（通常 8UC1）
    /// [out] dst     输出图（同 src 尺寸类型）
    /// [in]  thresh  阈值
    /// [in]  maxval  超过阈值时赋予的最大值（THRESH_BINARY 下通常 255）
    /// [in]  type    cv::THRESH_BINARY / BINARY_INV / TRUNC / TOZERO 等枚举（int 降级）
    static void threshold(const cv::Mat& src, cv::Mat& dst,
                          double thresh, double maxval, int type);

    /// 自适应阈值二值化（光照不均场景；cv::adaptiveThreshold 透传）。
    /// [in]  maxval      超阈值赋予值
    /// [in]  method      cv::ADAPTIVE_THRESH_MEAN_C / GAUSSIAN_C
    /// [in]  block_size  邻域窗口边长，必须为奇数（如 3/5/11）
    /// [in]  C           从均值/加权均值中减去的常数（微调阈值偏移）
    static void adaptiveThreshold(const cv::Mat& src, cv::Mat& dst, double maxval,
                                  int method, int block_size, double C);

    // ---------------- 2. 形态学 ----------------

    /// 生成形态学结构元（业务层不必为此 include imgproc）。
    /// [in] shape  cv::MORPH_RECT（矩形）/ MORPH_ELLIPSE（椭圆）/ MORPH_CROSS（十字）
    /// [in] ksize  结构元尺寸（奇数，如 5x5）
    /// 典型：ImageProcessor::makeKernel(cv::MORPH_ELLIPSE, cv::Size(5,5))
    static cv::Mat makeKernel(int shape, cv::Size ksize);

    /// 膨胀：亮区域扩张（kernel 结构元，可用 makeKernel 生成）。
    static void dilate(const cv::Mat& src, cv::Mat& dst, const cv::Mat& kernel);

    /// 腐蚀：亮区域收缩。
    static void erode(const cv::Mat& src, cv::Mat& dst, const cv::Mat& kernel);

    /// 复合形态学操作。
    /// [in] op  cv::MORPH_OPEN（开：去噪点）/ CLOSE（闭：填小孔）/
    ///          GRADIENT（梯度：轮廓）/ TOPHAT / BLACKHAT
    static void morphologyEx(const cv::Mat& src, cv::Mat& dst, int op, const cv::Mat& kernel);

    // ---------------- 3. 滤波与降噪 ----------------

    /// 高斯模糊。
    /// [in] ksize   核尺寸（奇数，如 5x5）；(0,0) 时由 sigma 推导
    /// [in] sigmaX  X 方向标准差；0 时由 ksize 推导
    /// [in] sigmaY  Y 方向标准差；默认 0（=同 sigmaX）
    static void gaussianBlur(const cv::Mat& src, cv::Mat& dst,
                             cv::Size ksize, double sigmaX, double sigmaY = 0);

    /// 中值滤波（去椒盐噪声）。[in] ksize 窗口边长，奇数且 >1。
    static void medianBlur(const cv::Mat& src, cv::Mat& dst, int ksize);

    /// 双边滤波（保边去噪）。
    /// [in] d           邻域直径（>0 时由它定邻域，sigma 定权重）
    /// [in] sigmaColor  颜色空间 sigma：越大越容忍颜色差异（越不保边）
    /// [in] sigmaSpace  坐标空间 sigma：越大越远邻域参与
    static void bilateralFilter(const cv::Mat& src, cv::Mat& dst, int d,
                                double sigmaColor, double sigmaSpace);

    // ---------------- 4. 边缘与特征检测 ----------------

    /// Canny 边缘检测。
    /// [in] threshold1  滞后阈值下限（弱边缘）
    /// [in] threshold2  滞后阈值上限（强边缘）；经验取 2~3 倍 threshold1
    static void canny(const cv::Mat& src, cv::Mat& edges, double threshold1, double threshold2);

    /// Sobel 梯度。
    /// [in] ddepth  输出深度（常用 CV_16S 防溢出；CV_8U 会截断负梯度）
    /// [in] dx, dy  X/Y 方向求导阶数（0/1/2，且 dx+dy >= 1）
    /// [in] ksize   核边长 1/3/5/7，默认 3
    static void sobel(const cv::Mat& src, cv::Mat& dst, int ddepth, int dx, int dy, int ksize = 3);

    // ---------------- 5. 几何变换 ----------------

    /// 缩放。dsize 直接指定目标尺寸；dsize 为 (0,0) 时按 fx/fy 比例缩放。
    /// [in] interpolation  cv::INTER_NEAREST/LINEAR(默认)/AREA/CUBIC/LANCZOS4
    static void resize(const cv::Mat& src, cv::Mat& dst, cv::Size dsize,
                       double fx = 0, double fy = 0, int interpolation = 1 /*LINEAR*/);

    /// 仿射变换（M 为 2x3 矩阵，可由 cv::getAffineTransform/getRotationMatrix2D 生成）。
    static void warpAffine(const cv::Mat& src, cv::Mat& dst, const cv::Mat& M, cv::Size dsize);

    /// 透视变换（M 为 3x3 矩阵，可由 cv::getPerspectiveTransform 生成）。
    static void warpPerspective(const cv::Mat& src, cv::Mat& dst, const cv::Mat& M, cv::Size dsize);

    // ---------------- 6. 模板匹配 ----------------

    /// 模板匹配（滑动窗口相似度）。
    /// [in]  method  cv::TM_SQDIFF / TM_CCORR_NORMED / TM_CCOEFF_NORMED 等
    /// [out] result  得分矩阵，尺寸 = (image-templ+1)，每像素一个得分
    /// 注意：TM_CCOEFF_NORMED 即 NCC（归一化互相关）——定位类应用首选。
    static void matchTemplate(const cv::Mat& image, const cv::Mat& templ,
                              cv::Mat& result, int method);

    /// 解析 matchTemplate 得分矩阵，返回最佳匹配位置与得分。
    /// [in]  result    matchTemplate 的输出
    /// [in]  method    与 matchTemplate 相同的 method（决定取最小还是最大值）
    /// [out] out_loc   匹配左上角坐标（模板贴在 image 上的位置）
    /// [out] out_score 对应得分（SQDIFF 系越小越好，其余越大越好）
    static void findBestMatch(const cv::Mat& result, int method,
                              cv::Point& out_loc, double& out_score);

    // ---------------- 7. 多尺度金字塔 ----------------

    /// 高斯金字塔降采样（尺寸减半）。
    static void pyrDown(const cv::Mat& src, cv::Mat& dst);

    /// 金字塔上采样（尺寸加倍，信息不可恢复，仅用于插值显示）。
    static void pyrUp(const cv::Mat& src, cv::Mat& dst);

    /// 构建高斯金字塔：输出 [level0 原图, level1, ..., level_maxlevel]，
    /// 共 maxlevel+1 层。
    static void buildGaussianPyramid(const cv::Mat& src,
                                     std::vector<cv::Mat>& out_pyramid, int maxlevel);

    // ---------------- 8. 空间切片与瓦片 ----------------

    /// 大图 → 带 overlap 的零拷贝瓦片（见 Tile 结构语义契约）。
    /// [in]  src        源图
    /// [in]  tile_size  瓦片设计尺寸（roi.width/height 恒等于它）
    /// [in]  overlap    相邻瓦片重叠像素数（0 = 不重叠）；步距 = tile_size - overlap
    /// [out] out_tiles  瓦片列表（行优先顺序）；image 为浅拷贝共享源图内存
    static void splitIntoTiles(const cv::Mat& src, cv::Size tile_size, int overlap,
                               std::vector<Tile>& out_tiles);

    /// 金字塔瓦片集合：level 0..max_level，每层独立瓦片网格（层间不重叠）。
    /// [in]  max_level  最高层数；输出 out_tiles.size() = max_level+1（下层为空时提前截止）
    /// 每个瓦片的 scale = 1/(1<<level)，roi_base 是 covered_roi 映射回 level 0 的矩形。
    static void buildTilePyramid(const cv::Mat& src, cv::Size tile_size, int max_level,
                                 std::vector<std::vector<Tile>>& out_tiles);

    // ---------------- 9. 轮廓提取与形状分析（与 Geom 联动） ----------------

    /// 从二值图提取轮廓（cv::findContours 透传）。
    /// [in]  binary_src   二值图（非 0 视为前景；建议先过 threshold）
    /// [out] out_contours 轮廓点集列表，坐标为像素中心（注意：面积是 (w-1)(h-1) 语义）
    /// [in]  mode         cv::RETR_EXTERNAL（只取最外层）/ RETR_LIST / RETR_TREE
    /// [in]  method       cv::CHAIN_APPROX_SIMPLE（压缩拐点）/ CHAIN_APPROX_NONE
    /// 轮廓点可直接传入 Geom::createPolygon 做面积/拓扑运算。
    static void findContours(const cv::Mat& binary_src,
                             std::vector<std::vector<cv::Point>>& out_contours,
                             int mode, int method);

    /// 连通域标记与统计（比 findContours 更直接：一次得到每域的包围盒与像素面积）。
    /// [in]  binary_src    二值图（非 0 视为前景）
    /// [in]  connectivity  4 或 8（邻域定义）
    /// [out] out_labels    标记图（与 src 同尺寸 CV_32S；0 = 背景，1..N = 各域编号）
    /// [out] out_areas     各域像素面积，下标 i 对应标记 i（out_areas[0] 恒为背景面积）
    /// 返回：连通域个数 N（不含背景）；输入为空返回 0。
    /// 用途：AOI 缺陷计数/面积过滤——配合 out_areas 遍历即可按面积筛除噪声域。
    static int connectedComponents(const cv::Mat& binary_src, cv::Mat& out_labels,
                                   std::vector<int>& out_areas, int connectivity = 8);

    /// 点集的最小面积旋转矩形（cv::minAreaRect 透传）。
    /// 返回：center/size/angle；点集为空返回空 RotatedRect。
    static cv::RotatedRect getMinAreaRect(const std::vector<cv::Point>& points);

    /// 点集的轴对齐包围矩形（cv::boundingRect 透传，像素级 int）。
    static cv::Rect getBoundingRect(const std::vector<cv::Point>& points);

    // ---------------- 10. 色彩空间转换 ----------------

    /// 色彩空间转换（cv::cvtColor 透传）。
    /// [in] code  cv::COLOR_BGR2GRAY / COLOR_BGR2HSV / COLOR_GRAY2BGR 等枚举（int 降级）
    /// 用途：检测前处理标配（彩色相机图 → 灰度/HSV 通道分离）。
    static void cvtColor(const cv::Mat& src, cv::Mat& dst, int code);

    // ---------------- 11. 灰度增强 ----------------

    /// 全局直方图均衡化（8UC1 输入）。
    static void equalizeHist(const cv::Mat& src, cv::Mat& dst);

    /// CLAHE 限制对比度自适应均衡（比全局均衡化更适工作照：局部增强不过曝）。
    /// [in] clip_limit      对比度限制上限（典型 2.0~4.0；越大越接近全局均衡化）
    /// [in] tile_grid_size  分块网格（典型 8x8；块内独立均衡）
    static void clahe(const cv::Mat& src, cv::Mat& dst,
                      double clip_limit = 2.0, cv::Size tile_grid_size = cv::Size(8, 8));

    // ---------------- 12. 像素级运算与 LUT ----------------

    /// 查找表变换（cv::LUT 透传）：src 逐像素查 lut 表得输出。
    /// [in] lut  256 元素查找表（CV_8UC1；多通道源时每通道分别查）
    /// 用途：伽马校正、灰度分段映射、色表渲染——一次查表 O(1)/像素。
    static void lut(const cv::Mat& src, const cv::Mat& lut, cv::Mat& dst);

    /// 逐像素取反（8UC1/8UC3）。灰度反转 = 255 - p。
    static void bitwiseNot(const cv::Mat& src, cv::Mat& dst);

    /// 两图按位与（掩膜合成：把 ROI 外区域清零的常用手段）。
    static void bitwiseAnd(const cv::Mat& src1, const cv::Mat& src2, cv::Mat& dst);

    /// 两图按位或。
    static void bitwiseOr(const cv::Mat& src1, const cv::Mat& src2, cv::Mat& dst);

    /// 两图逐像素差的绝对值（批板 vs 模板差分——AOI 检出核心手段）。
    static void absdiff(const cv::Mat& src1, const cv::Mat& src2, cv::Mat& dst);

    // ---------------- 13. 积分图（大窗口快速统计） ----------------

    /// 积分图（cv::integral 透传）：sum(x,y) = 左上角矩形内像素和。
    /// [in] sdepth  输出深度（常用 CV_32S；CV_64F 防大图溢出）
    /// 任意矩形区域像素和 = 4 次查表，窗口均值/方差检测 O(1)——暗斑快速筛查。
    static void integral(const cv::Mat& src, cv::Mat& sum, int sdepth = -1);

    // ---------------- 14. 距离变换 ----------------

    /// 距离变换：每个前景点到最近背景点的距离（cv::distanceTransform 透传）。
    /// [in] distance_type  cv::DIST_L1（快）/ DIST_L2（精确）/ DIST_C
    /// [in] mask_size      3 / 5（DIST_L2 精度）或 DIST_MASK_PRECISE
    /// [in] dst_type       输出深度 CV_32F（默认）
    /// 用途：骨架提取、自适应腐蚀半径、圆焊球中心定位（距离局部极大值 = 圆心）。
    static void distanceTransform(const cv::Mat& src, cv::Mat& dst,
                                  int distance_type, int mask_size, int dst_type = CV_32F);

    // ---------------- 15. 连通分析增强 ----------------

    /// 连通域全统计（cv::connectedComponentsWithStats 透传）：
    /// 一次得到每个域的像素面积、包围盒、质心——AOI 缺陷计数/定位一步到位。
    /// [out] out_labels     标记图（0 = 背景，1..N = 各域）
    /// [out] out_stats      每行 5 列：[CC_STAT_LEFT, TOP, WIDTH, HEIGHT, AREA]（CV_32S）
    /// [out] out_centroids  每行 (x, y) 质心（CV_64F）
    /// 返回：连通域个数 N（不含背景）；输入为空返回 0。
    static int connectedComponentsWithStats(const cv::Mat& binary_src, cv::Mat& out_labels,
                                            cv::Mat& out_stats, cv::Mat& out_centroids,
                                            int connectivity = 8);

    /// 泛洪填充（cv::floodFill 透传）：从种子点填充连通同色区域。
    /// [in,out] image  输入输出图（会被就地修改：种子连通域被重绘为 new_val）
    /// [in]     mask   可选工作掩膜（比 image 大 2 像素的圈；不需要传 nullptr）
    /// [in]  lo_diff/up_diff  邻域像素与种子（或当前）像素的允许差（决定填充范围）
    /// [out] out_rect        被填充区域包围盒（可为 nullptr）
    /// 返回：重绘像素个数；填充失败返回 -1。
    /// 用途：区域填充/孔洞封闭。
    static int floodFill(cv::Mat& image, cv::Mat* mask, cv::Point seed,
                         cv::Scalar new_val, cv::Scalar lo_diff = cv::Scalar(),
                         cv::Scalar up_diff = cv::Scalar(), cv::Rect* out_rect = nullptr);

    // ---------------- 16. 形状度量（与 Geom 联动） ----------------

    /// 轮廓面积（cv::contourArea 透传）。
    /// [in] oriented  true 时按顶点顺序返回带符号面积（顺时针为负）
    /// 返回：像素面积；点数 < 3 返回 0。
    static double contourArea(const std::vector<cv::Point>& contour, bool oriented = false);

    /// 轮廓是否为凸形（cv::isContourConvex 透传）；点数 < 3 返回 false。
    static bool isContourConvex(const std::vector<cv::Point>& contour);

    /// 椭圆拟合（cv::fitEllipse 透传）：最少 5 点，返回 center/size/angle。
    /// 用途：圆焊球/椭圆焊盘的尺寸测量（size.width/height = 长短轴全长）。
    static cv::RotatedRect fitEllipse(const std::vector<cv::Point>& points);

    /// 直线拟合（cv::fitLine 透传）。
    /// [in] dist_type  cv::DIST_L2（最小二乘）/ DIST_L1 / DIST_HUBER（抗外点，推荐）
    /// [out] out_line  4 元素：[vx, vy, x0, y0]（方向向量 + 线上一点）
    static void fitLine(const std::vector<cv::Point>& points, std::vector<float>& out_line,
                        int dist_type, double param = 0, double reps = 0.01, double aeps = 0.01);

    /// 最小外接圆（cv::minEnclosingCircle 透传）。
    static void minEnclosingCircle(const std::vector<cv::Point>& points,
                                   cv::Point2f& out_center, float& out_radius);

    /// 点到轮廓的距离（cv::pointPolygonTest 透传）。
    /// [in] measure_dist  true 返回带符号像素距离（内正外负）；false 只判内外（返回 +1/-1/0）
    /// 用途：点是否落入 ROI/缺陷区（配合 Geom::createPolygon 亦可）。
    static double pointPolygonTest(const std::vector<cv::Point>& contour, cv::Point2f pt,
                                   bool measure_dist);

    /// 形状相似度（cv::matchShapes 透传，Hu 矩）：越小越相似，0 = 完全一致。
    /// [in] method  cv::CONTOURS_MATCH_I1 / I2 / I3（常用 I1）
    static double matchShapes(const std::vector<cv::Point>& contour1,
                              const std::vector<cv::Point>& contour2,
                              int method, double parameter = 0.0);

    /// 轮廓凸性缺陷（cv::convexityDefects 透传）：凸包与轮廓间的凹陷深度。
    /// [in]  convex_hull  凸包顶点索引（cv::convexHull(contour, hull) 的输出，int 索引版）
    /// [out] out_defects  每行 4 元素 [start_index, end_index, farthest_index, 深度×256]
    /// 用途：引脚变形/缺料检测（凹陷深度超阈值即缺陷）。
    static void convexityDefects(const std::vector<cv::Point>& contour,
                                 const std::vector<int>& convex_hull,
                                 std::vector<cv::Vec4i>& out_defects);

    // ---------------- 17. 直方图 ----------------

    /// 灰度直方图（8UC1 简化封装：256 bins，无掩膜）。
    /// [out] out_hist  256 个计数值
    /// 用途：批次灰度分布一致性监控。
    static void calcHist8U(const cv::Mat& src, std::vector<float>& out_hist);

    /// 直方图相似度（cv::compareHist 透传，需先各自 calcHist8U）。
    /// [in] method  cv::HISTCMP_CORREL（相关，1=同）/ CHISQR / INTERSECT / BHATTACHARYYA（0=同）
    static double compareHist(const std::vector<float>& hist1, const std::vector<float>& hist2,
                              int method);

    // ---------------- 18. 重映射（畸变校正/极坐标展开） ----------------

    /// 任意重映射（cv::remap 透传）：dst(x,y) = src(map1(x,y), map2(x,y))。
    /// [in] map1/map2  各像素的源坐标（CV_32FC1 或 CV_16SC2）
    /// [in] interpolation  cv::INTER_LINEAR（默认）/ INTER_NEAREST / INTER_CUBIC
    /// 用途：相机标定后的畸变校正（initUndistortRectifyMap 的输出直接喂入）。
    static void remap(const cv::Mat& src, cv::Mat& dst, const cv::Mat& map1, const cv::Mat& map2,
                      int interpolation, cv::Scalar border_value = cv::Scalar());

    /// 极坐标展开（cv::warpPolar 透传）：圆形区域 → 矩形图。
    /// [in] center/max_radius  展开的圆心与最大半径
    /// [in] flags  cv::WARP_POLAR_LINEAR（环形工件展开成条带）/ WARP_POLAR_LOG
    /// 用途：环形工件（点阵 COB/圆形基板）的检测展开——圆上等角度缺陷在展开图中变成等间距。
    static void warpPolar(const cv::Mat& src, cv::Mat& dst, cv::Size dsize, cv::Point2f center,
                          double max_radius, int flags);

    /// 角点候选检测（cv::goodFeaturesToTrack 透传）：按质量降序输出角点。
    /// [out] out_corners  角点列表
    /// [in] max_corners   最大数量；quality_level 质量系数（典型 0.01~0.1）；
    ///                    min_distance 角点最小间距（像素）
    /// [in] mask          可选 ROI 掩膜（8UC1 非 0 = 参与）；空 = 全图
    /// 用途：Mark 点角点候选、基准特征提取。亚像素定位需另行细化。
    static void goodFeaturesToTrack(const cv::Mat& src, std::vector<cv::Point2f>& out_corners,
                                    int max_corners, double quality_level, double min_distance,
                                    const cv::Mat& mask = cv::Mat());

    /// 亚像素角点细化（cv::cornerSubPix 透传）：把整数角点就地精化到亚像素。
    /// 典型链路：goodFeaturesToTrack → cornerSubPix。
    /// ⚠️ cv::cornerSubPix 声明在 OpenCV video 头——本库唯一引用 video 模块处
    ///（仅 .cpp，公共头仍零 video；opencv_world 单 DLL，部署清单不变）。
    /// [in] win_size   搜索窗口（典型 cv::Size(11, 11)）
    /// [in] zero_zone  死区（典型 cv::Size(-1, -1) = 自动）
    /// [in] criteria   迭代终止（典型 cv::TermCriteria(cv::TermCriteria::EPS |
    ///                 cv::TermCriteria::COUNT, 30, 0.01)）
    /// [in,out] corners 输入粗角点，就地精化为亚像素坐标
    static void cornerSubPix(const cv::Mat& src, std::vector<cv::Point2f>& corners,
                             cv::Size win_size, cv::Size zero_zone, cv::TermCriteria criteria);

    /// 非零像素坐标列表（cv::findNonZero 透传）。
    /// 用途：HeightMap 有效格提取（经 HeightMapAdapter::toImage 后）等。
    /// 返回：点数（空图/全零返回 0）。
    static void findNonZero(const cv::Mat& src, std::vector<cv::Point>& out_points);

    /// 沿线段卡尺（尺寸测量经典算子）：p1→p2 均匀采样灰度（双线性），中心差分
    /// 求梯度，梯度局部极值处输出亚像素边缘点（抛物线插值）。
    /// [in]  p1/p2       线段端点（像素坐标）
    /// [in]  n_samples   采样数（>= 4；越大边缘定位越稳）
    /// [in]  polarity    极性：0 = 任意；1 = 仅暗→亮（正梯度）；-1 = 仅亮→暗（负梯度）
    /// [in]  min_gradient 最小梯度幅值（灰阶/像素）
    /// [out] out_edges   亚像素边缘点（位于线段上：p1 + t·(p2−p1)）
    /// 返回：边缘个数；空图 / n_samples < 4 返回 0。
    static int caliperEdges(const cv::Mat& image, cv::Point2f p1, cv::Point2f p2,
                            int n_samples, int polarity, double min_gradient,
                            std::vector<cv::Point2f>& out_edges);

    /// 两轮廓（点集）Hausdorff 距离（2D，双向最大-最小，暴力 O(n·m)）。
    /// 用途：实测轮廓 vs 理论轮廓的形状偏差量化（matchShapes 的 Hu 矩之外的直接口径）。
    /// 返回：-1 = 任一为空。
    static double contourHausdorff(const std::vector<cv::Point2f>& a,
                                   const std::vector<cv::Point2f>& b);

    // ---------------- 19. 绘图标注（结果可视化；业务层不必为此 include imgproc） ----------------
    // 约定：img 为空时全部为 no-op（不抛异常）；颜色用 cv::Scalar（灰度图传 Scalar(s)）。

    /// 画线段。
    static void drawLine(cv::Mat& img, cv::Point p1, cv::Point p2,
                         cv::Scalar color, int thickness = 1);

    /// 画圆。thickness = 1+ 描边宽度；负数 = 实心。
    static void drawCircle(cv::Mat& img, cv::Point center, int radius,
                           cv::Scalar color, int thickness = 1);

    /// 画矩形（cv::Rect）。thickness = 1+ 描边宽度；负数 = 实心。
    static void drawRect(cv::Mat& img, cv::Rect rect, cv::Scalar color, int thickness = 1);

    /// 画折线/轮廓（多点连线）。[in] is_closed  true = 首尾相连闭合。
    /// 用途：findContours 输出的轮廓直接传入标注。
    static void drawPolylines(cv::Mat& img, const std::vector<cv::Point>& points,
                              bool is_closed, cv::Scalar color, int thickness = 1);

    /// 填充多边形（实心）。
    static void fillPolygon(cv::Mat& img, const std::vector<cv::Point>& points, cv::Scalar color);

    /// 画十字标记（缺陷定位标注常用）。[in] length  十字臂长（像素）。
    static void drawCross(cv::Mat& img, cv::Point center, int length,
                          cv::Scalar color, int thickness = 1);

    /// 画文本（Hershey 简洁字体，中文不支持——只用于编号/数值标注）。
    /// [in] scale  字体缩放（典型 0.4~0.8）；[in] thickness 描边粗细
    static void drawText(cv::Mat& img, const std::string& text, cv::Point org,
                         cv::Scalar color, double scale = 0.5, int thickness = 1);

    // ---------------- 20. 模板匹配增强（掩膜 / 多目标 / 亚像素） ----------------

    /// 带掩膜的模板匹配：只统计 mask 非零区域（遮挡/干扰背景下的鲁棒匹配）。
    /// [in] mask    与 templ 同尺寸（8UC1，非 0 = 参与匹配）
    /// [in] method  仅 cv::TM_SQDIFF / TM_CCORR_NORMED 支持 mask（OpenCV 4.4 限制），
    ///              其他 method 清空 result 返回（见下）
    /// 零范数区域（全黑背景等）产生的 NaN/Inf 自动归一为该方法最差得分
    /// （SQDIFF = FLT_MAX / CCORR_NORMED = 0），不会带偏 findBestMatch/findAllMatches。
    /// 失败表现：result 为空。
    static void matchTemplateMasked(const cv::Mat& image, const cv::Mat& templ,
                                    const cv::Mat& mask, cv::Mat& result, int method);

    /// 解析得分矩阵极值点并做亚像素细化（三分点抛物线插值，修正量 [-0.5, 0.5]）。
    /// [out] out_loc    亚像素坐标（整数峰 + 修正）
    /// [out] out_score  极值点处抛物线插值得分
    /// method 语义同 findBestMatch（SQDIFF 系取最小，其余取最大）；
    /// result 需为 matchTemplate 输出（CV_32F/CV_64F），峰值贴边时保持整数结果。
    static void findBestMatchSubPix(const cv::Mat& result, int method,
                                    cv::Point2f& out_loc, double& out_score);

    /// 多目标匹配：按得分从优到劣提取至多 max_matches 个目标。
    /// [in] score_threshold  SQDIFF 系 = "得分 ≤ 阈值"，其余 = "得分 ≥ 阈值"
    /// [in] nms_radius       非极大值抑制半径（像素；命中目标邻域不再重复出目标）
    /// [out] out_matches     最优在前；loc 为整数峰位置（亚像素需接 findBestMatchSubPix）
    static void findAllMatches(const cv::Mat& result, int method, double score_threshold,
                               int max_matches, int nms_radius,
                               std::vector<Match2d>& out_matches);

    /// 旋转模板搜索（Mark 点带角度对位）：逐角度旋转模板做匹配，取全局最优，
    /// 命中角度的得分图再做亚像素细化。
    /// [in] angle_start/end/step  角度范围（度，angle_end >= angle_start；step>0，
    ///                            步数上限 3600 防呆）
    /// 返回：loc = 模板中心落在 image 上的位置（亚像素，旋转画布外接中心）；
    ///       angle = 命中角度（度）；score = 该角度得分图的最优分。
    ///       输入非法/无角度时 loc=(-1,-1)、score 取 method 的最差值。
    /// 代价：每个角度一次全图 matchTemplate——大图先缩小粗搜属业务侧优化。
    static RotatedMatch2d findRotatedMatch(const cv::Mat& image, const cv::Mat& templ,
                                           double angle_start, double angle_end,
                                           double angle_step, int method = 5 /* cv::TM_CCOEFF_NORMED（数值常量：本头按约定
                                     不 include imgproc，2026-11-11 解除全仓构建阻塞） */);

    // ---------------- 21. 基础形状拟合（2D 点集） ----------------

    /// 圆拟合（几何最小二乘：Kasa 代数拟合初值 + Gauss-Newton 精化）。
    /// [out] out_center  圆心
    /// [out] out_radius  半径（与坐标同单位）
    /// [out] out_rms     残差均方根（点到圆周距离的 RMS）
    /// 返回：false = 点数 < 3 / 共线或近共线（无有限解）。
    /// 注意：弧段覆盖 < 90° 时圆心-半径仍可能偏移——测量用途尽量覆盖足够弧段。
    static bool fitCircle(const std::vector<cv::Point2f>& points,
                          cv::Point2f& out_center, double& out_radius, double& out_rms);

    /// 圆拟合（cv::Point 整数轮廓重载，内部转 float）。
    static bool fitCircle(const std::vector<cv::Point>& points,
                          cv::Point2f& out_center, double& out_radius, double& out_rms);

    /// 圆拟合（RANSAC 抗外点）：随机三点定圆 → 统计内点 → 最优内点集 GN 精化。
    /// [in] threshold   内点判定：点到圆周距离 ≤ threshold（与坐标同单位）
    /// [out] out_inliers 内点索引（精化后圆上重算）
    /// [in] seed        随机种子（固定可复现）；max_iters 迭代上限
    /// 返回：false = 点数 < 3 / threshold<=0 / 无有效圆（内点 < 3）。
    /// 用途：焊盘边缘毛刺/粘连外点场景（fitCircle 纯最小二乘会被外点拉偏）。
    static bool fitCircleRobust(const std::vector<cv::Point2f>& points, double threshold,
                                cv::Point2f& out_center, double& out_radius, double& out_rms,
                                std::vector<int>& out_inliers,
                                unsigned int seed = 0, int max_iters = 1000);

    /// 批量圆拟合：N 个独立点集一次完成（焊盘阵列/孔阵列）。
    /// [in] contours  每个元素为独立点集（<3 点/退化项输出 valid=0）
    /// [in] threshold RANSAC 内点阈值：>0 逐项走 fitCircleRobust（抗外点），
    ///                <=0 逐项走 fitCircle（纯最小二乘）
    /// [out] out_centers/out_radii/out_rmss/out_valid  与 contours 对齐（失败项置 0）
    /// 返回：成功个数。
    static int fitCircleBatch(const std::vector<std::vector<cv::Point2f>>& contours,
                              double threshold,
                              std::vector<cv::Point2f>& out_centers,
                              std::vector<double>& out_radii, std::vector<double>& out_rmss,
                              std::vector<unsigned char>& out_valid,
                              unsigned int seed = 0, int max_iters = 1000);

    /// 折线/轮廓顶点逼近（cv::approxPolyDP 透传）：epsilon 内删冗余顶点。
    /// [in] epsilon  逼近精度（与坐标同单位，即道格拉斯-普克容差）
    /// [in] closed   true = 轮廓闭合（findContours 输出）
    /// 用途：密集轮廓 → 少量顶点（形状判定 / Geom::createPolygon 前减点）。
    static void approxPolyDP(const std::vector<cv::Point>& contour,
                             std::vector<cv::Point>& out_approxCurve,
                             double epsilon, bool closed);

    // ---------------- 22. 统计与卷积 ----------------

    /// 均值与标准差（cv::meanStdDev 透传）。
    /// [in] mask  8UC1，非 0 像素参与统计；空 = 全图。空图输出全 0。
    /// 用途：批次灰度一致性监控、区域亮度/对比度量化。
    static void meanStdDev(const cv::Mat& src, cv::Scalar& out_mean, cv::Scalar& out_stddev,
                           const cv::Mat& mask = cv::Mat());

    /// 非零像素计数（cv::countNonZero 透传）。空图返回 0。
    static int countNonZero(const cv::Mat& src);

    /// 全图最小/最大值及其位置（cv::minMaxLoc 透传，无掩膜版）。空图输出全 0。
    /// 配合 matchTemplate 的 result 矩阵做快速峰值检查。
    static void minMaxLoc(const cv::Mat& src, double& out_min, double& out_max,
                          cv::Point& out_minLoc, cv::Point& out_maxLoc);

    /// 数组归一化（cv::normalize 透传）。
    /// [in] norm_type  cv::NORM_MINMAX（缩放到 [alpha,beta]，默认）或 NORM_L1/L2/INF
    static void normalize(const cv::Mat& src, cv::Mat& dst, double alpha = 1.0,
                          double beta = 0.0, int norm_type = cv::NORM_MINMAX);

    /// 自定义卷积（cv::filter2D 透传）：任意核滤波/锐化/梯度。
    /// [in] kernel  浮点单通道核（如锐化/平滑/自定义梯度算子）
    /// [in] ddepth  输出深度（-1 = 同 src；负梯度场景建议 CV_32F/CV_16S）
    static void filter2D(const cv::Mat& src, cv::Mat& dst, int ddepth, const cv::Mat& kernel);

    /// 线性变换取绝对值（cv::convertScaleAbs 透传）：dst = |src·alpha + beta|（8U）。
    /// 用途：Sobel/Canny 后的梯度可视化、亮度增益。
    static void convertScaleAbs(const cv::Mat& src, cv::Mat& dst,
                                double alpha = 1.0, double beta = 0.0);

    // ---------------- 23. 霍夫检测（直线 / 圆） ----------------

    /// 概率霍夫直线检测（cv::HoughLinesP 透传）。
    /// [out] out_lines  每行 [x1, y1, x2, y2]
    /// [in] rho         距离分辨率（像素，典型 1）
    /// [in] theta       角度分辨率（弧度，典型 CV_PI/180）
    /// [in] threshold   投票阈值（越大越严）
    /// [in] min_line_length  最短线长（0 = 不限）；max_line_gap 同线最大断距（0 = 不限）
    /// 输入应为二值/边缘图（先 canny）。
    static void houghLinesP(const cv::Mat& src, std::vector<cv::Vec4i>& out_lines,
                            double rho, double theta, int threshold,
                            double min_line_length = 0.0, double max_line_gap = 0.0);

    /// 霍夫圆检测（cv::HoughCircles 透传，HOUGH_GRADIENT）。
    /// [out] out_circles  每行 [x, y, radius]
    /// [in] dp        分辨率倒数（1 = 与源图同分辨率）
    /// [in] min_dist  圆心最小间距（防重复检出）
    /// [in] param1    内部 Canny 高阈值；param2 圆心投票阈值（越小圆越多）
    /// [in] min_radius / max_radius  半径范围（max_radius <= 0 = 用图像对角线）
    /// 输入应为 8UC1 灰度图（内部自带 Canny，无需先做边缘）。
    /// 注意：HOUGH_GRADIENT 对锐利二值图可能一无所获（实测）——检测前先
    ///       gaussianBlur 平滑（现实相机图天然满足）。
    static void houghCircles(const cv::Mat& src, std::vector<cv::Vec3f>& out_circles,
                             double dp, double min_dist, double param1 = 100,
                             double param2 = 30, int min_radius = 0, int max_radius = 0);
};

} // namespace shapely

#endif // SHAPELY_IMAGE_PROCESSOR_H
