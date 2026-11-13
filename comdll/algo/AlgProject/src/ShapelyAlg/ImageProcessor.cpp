// ImageProcessor.cpp —— 二维图像算子实现（imgproc 下沉在此，头文件零 imgproc）。
// 纯透传原则：直接包装 OpenCV 原生 API，严禁手写底层循环。

#include "ImageProcessor.h"

#include "FitUtil.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>   // cv::cornerSubPix（本库唯一 video 引用，见头注释）

#include <algorithm>
#include <cmath>
#include <limits>

namespace shapely {

// ---------------- 1. 阈值与二值化 ----------------

void ImageProcessor::threshold(const cv::Mat& src, cv::Mat& dst,
                               double thresh, double maxval, int type)
{
    cv::threshold(src, dst, thresh, maxval, type);
}

void ImageProcessor::adaptiveThreshold(const cv::Mat& src, cv::Mat& dst, double maxval,
                                       int method, int block_size, double C)
{
    cv::adaptiveThreshold(src, dst, maxval, method, cv::THRESH_BINARY, block_size, C);
}

// ---------------- 2. 形态学 ----------------

cv::Mat ImageProcessor::makeKernel(int shape, cv::Size ksize)
{
    return cv::getStructuringElement(shape, ksize);
}

void ImageProcessor::dilate(const cv::Mat& src, cv::Mat& dst, const cv::Mat& kernel)
{
    cv::dilate(src, dst, kernel);
}

void ImageProcessor::erode(const cv::Mat& src, cv::Mat& dst, const cv::Mat& kernel)
{
    cv::erode(src, dst, kernel);
}

void ImageProcessor::morphologyEx(const cv::Mat& src, cv::Mat& dst, int op, const cv::Mat& kernel)
{
    cv::morphologyEx(src, dst, op, kernel);
}

// ---------------- 3. 滤波与降噪 ----------------

void ImageProcessor::gaussianBlur(const cv::Mat& src, cv::Mat& dst,
                                  cv::Size ksize, double sigmaX, double sigmaY)
{
    cv::GaussianBlur(src, dst, ksize, sigmaX, sigmaY);
}

void ImageProcessor::medianBlur(const cv::Mat& src, cv::Mat& dst, int ksize)
{
    cv::medianBlur(src, dst, ksize);
}

void ImageProcessor::bilateralFilter(const cv::Mat& src, cv::Mat& dst, int d,
                                     double sigmaColor, double sigmaSpace)
{
    cv::bilateralFilter(src, dst, d, sigmaColor, sigmaSpace);
}

// ---------------- 4. 边缘与特征检测 ----------------

void ImageProcessor::canny(const cv::Mat& src, cv::Mat& edges, double threshold1, double threshold2)
{
    cv::Canny(src, edges, threshold1, threshold2);
}

void ImageProcessor::sobel(const cv::Mat& src, cv::Mat& dst, int ddepth, int dx, int dy, int ksize)
{
    cv::Sobel(src, dst, ddepth, dx, dy, ksize);
}

// ---------------- 5. 几何变换 ----------------

void ImageProcessor::resize(const cv::Mat& src, cv::Mat& dst, cv::Size dsize,
                            double fx, double fy, int interpolation)
{
    cv::resize(src, dst, dsize, fx, fy, interpolation);
}

void ImageProcessor::warpAffine(const cv::Mat& src, cv::Mat& dst, const cv::Mat& M, cv::Size dsize)
{
    cv::warpAffine(src, dst, M, dsize);
}

void ImageProcessor::warpPerspective(const cv::Mat& src, cv::Mat& dst, const cv::Mat& M, cv::Size dsize)
{
    cv::warpPerspective(src, dst, M, dsize);
}

// ---------------- 6. 模板匹配 ----------------

void ImageProcessor::matchTemplate(const cv::Mat& image, const cv::Mat& templ,
                                   cv::Mat& result, int method)
{
    cv::matchTemplate(image, templ, result, method);
}

void ImageProcessor::findBestMatch(const cv::Mat& result, int method,
                                   cv::Point& out_loc, double& out_score)
{
    // SQDIFF / SQDIFF_NORMED 越小越好，其余越大越好
    const bool isSqdiff = (method == cv::TM_SQDIFF || method == cv::TM_SQDIFF_NORMED);
    double minVal = 0, maxVal = 0;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
    if (isSqdiff) {
        out_loc = minLoc;
        out_score = minVal;
    } else {
        out_loc = maxLoc;
        out_score = maxVal;
    }
}

// ---------------- 7. 多尺度金字塔 ----------------

void ImageProcessor::pyrDown(const cv::Mat& src, cv::Mat& dst)
{
    cv::pyrDown(src, dst);
}

void ImageProcessor::pyrUp(const cv::Mat& src, cv::Mat& dst)
{
    cv::pyrUp(src, dst);
}

void ImageProcessor::buildGaussianPyramid(const cv::Mat& src,
                                          std::vector<cv::Mat>& out_pyramid, int maxlevel)
{
    out_pyramid.clear();
    out_pyramid.push_back(src);
    cv::Mat cur = src;
    for (int lv = 0; lv < maxlevel; ++lv) {
        cv::Mat next;
        cv::pyrDown(cur, next);
        out_pyramid.push_back(next);
        cur = next;
    }
}

// ---------------- 8. 空间切片与瓦片 ----------------

void ImageProcessor::splitIntoTiles(const cv::Mat& src, cv::Size tile_size, int overlap,
                                    std::vector<Tile>& out_tiles)
{
    out_tiles.clear();
    if (src.empty() || tile_size.width <= 0 || tile_size.height <= 0)
        return;
    const int step_w = tile_size.width - overlap;
    const int step_h = tile_size.height - overlap;
    if (step_w <= 0 || step_h <= 0)
        return;
    const int cols = (std::max)(1, (int)std::ceil((double)src.cols / step_w));
    const int rows = (std::max)(1, (int)std::ceil((double)src.rows / step_h));
    out_tiles.reserve((size_t)cols * rows);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            Tile t;
            t.roi = cv::Rect(c * step_w, r * step_h, tile_size.width, tile_size.height);
            t.covered_roi = t.roi & cv::Rect(0, 0, src.cols, src.rows);
            if (t.covered_roi.empty())
                continue;
            t.image = src(t.covered_roi);   // 浅拷贝（引用计数，零拷贝）
            t.roi_base = t.covered_roi;
            t.scale = 1.0;
            t.level = 0;
            out_tiles.push_back(std::move(t));
        }
    }
}

void ImageProcessor::buildTilePyramid(const cv::Mat& src, cv::Size tile_size, int max_level,
                                      std::vector<std::vector<Tile>>& out_tiles)
{
    out_tiles.clear();
    if (src.empty())
        return;
    // level 0 原图瓦片
    std::vector<Tile> level0;
    splitIntoTiles(src, tile_size, 0, level0);
    out_tiles.push_back(std::move(level0));
    // 逐层下采样（2 幂 scale）
    cv::Mat cur = src;
    for (int lv = 1; lv <= max_level; ++lv) {
        cv::Mat next;
        cv::pyrDown(cur, next);
        if (next.empty())
            break;
        std::vector<Tile> levelTiles;
        splitIntoTiles(next, tile_size, 0, levelTiles);
        const double scale = 1.0 / (double)(1 << lv);
        for (Tile& t : levelTiles) {
            t.level = lv;
            t.scale = scale;
            // covered_roi 映射回 level 0（2 幂 scale，四舍五入对齐）
            t.roi_base = cv::Rect(
                (int)std::lround(t.covered_roi.x / scale),
                (int)std::lround(t.covered_roi.y / scale),
                (int)std::lround(t.covered_roi.width / scale),
                (int)std::lround(t.covered_roi.height / scale));
        }
        out_tiles.push_back(std::move(levelTiles));
        cur = next;
    }
}

// ---------------- 9. 轮廓提取与形状分析 ----------------

void ImageProcessor::findContours(const cv::Mat& binary_src,
                                  std::vector<std::vector<cv::Point>>& out_contours,
                                  int mode, int method)
{
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(binary_src, out_contours, hierarchy, mode, method);
}

cv::RotatedRect ImageProcessor::getMinAreaRect(const std::vector<cv::Point>& points)
{
    return cv::minAreaRect(points);
}

cv::Rect ImageProcessor::getBoundingRect(const std::vector<cv::Point>& points)
{
    return cv::boundingRect(points);
}

int ImageProcessor::connectedComponents(const cv::Mat& binary_src, cv::Mat& out_labels,
                                        std::vector<int>& out_areas, int connectivity)
{
    out_areas.clear();
    if (binary_src.empty()) {
        out_labels.release();
        return 0;
    }
    cv::Mat stats;
    cv::Mat centroids;
    const int n = cv::connectedComponentsWithStats(binary_src, out_labels, stats, centroids,
                                                   connectivity, CV_32S);
    // stats 共 n 行（行 0 = 背景），列 4 = CC_STAT_AREA
    out_areas.reserve((size_t)n);
    for (int i = 0; i < n; ++i)
        out_areas.push_back(stats.at<int>(i, cv::CC_STAT_AREA));
    return n;
}

// ---------------- 10. 颜色空间与增强 ----------------

void ImageProcessor::cvtColor(const cv::Mat& src, cv::Mat& dst, int code)
{
    if (src.empty())
    {
        dst.release();
        return;
    }
    cv::cvtColor(src, dst, code);
}

void ImageProcessor::equalizeHist(const cv::Mat& src, cv::Mat& dst)
{
    if (src.empty())
    {
        dst.release();
        return;
    }
    cv::equalizeHist(src, dst);
}

void ImageProcessor::clahe(const cv::Mat& src, cv::Mat& dst,
                           double clip_limit, cv::Size tile_grid_size)
{
    if (src.empty())
    {
        dst.release();
        return;
    }
    cv::Ptr<cv::CLAHE> c = cv::createCLAHE(clip_limit, tile_grid_size);
    c->apply(src, dst);
}

void ImageProcessor::lut(const cv::Mat& src, const cv::Mat& lut_table, cv::Mat& dst)
{
    if (src.empty() || lut_table.empty())
    {
        dst.release();
        return;
    }
    cv::LUT(src, lut_table, dst);
}

// ---------------- 11. 像素级逻辑与算术运算 ----------------

void ImageProcessor::bitwiseNot(const cv::Mat& src, cv::Mat& dst)
{
    if (src.empty())
    {
        dst.release();
        return;
    }
    cv::bitwise_not(src, dst);
}

void ImageProcessor::bitwiseAnd(const cv::Mat& src1, const cv::Mat& src2, cv::Mat& dst)
{
    if (src1.empty() || src2.empty())
    {
        dst.release();
        return;
    }
    cv::bitwise_and(src1, src2, dst);
}

void ImageProcessor::bitwiseOr(const cv::Mat& src1, const cv::Mat& src2, cv::Mat& dst)
{
    if (src1.empty() || src2.empty())
    {
        dst.release();
        return;
    }
    cv::bitwise_or(src1, src2, dst);
}

void ImageProcessor::absdiff(const cv::Mat& src1, const cv::Mat& src2, cv::Mat& dst)
{
    if (src1.empty() || src2.empty())
    {
        dst.release();
        return;
    }
    cv::absdiff(src1, src2, dst);
}

// ---------------- 12. 积分图与距离变换 ----------------

void ImageProcessor::integral(const cv::Mat& src, cv::Mat& out_sum, int sdepth)
{
    if (src.empty())
    {
        out_sum.release();
        return;
    }
    cv::integral(src, out_sum, sdepth);
}

void ImageProcessor::distanceTransform(const cv::Mat& src, cv::Mat& dst,
                                       int distance_type, int mask_size, int dst_type)
{
    if (src.empty())
    {
        dst.release();
        return;
    }
    cv::distanceTransform(src, dst, distance_type, mask_size, dst_type);
}

// ---------------- 13. 连通域与泛洪填充 ----------------

int ImageProcessor::connectedComponentsWithStats(const cv::Mat& binary_src,
                                                 cv::Mat& out_labels, cv::Mat& out_stats,
                                                 cv::Mat& out_centroids, int connectivity)
{
    if (binary_src.empty())
    {
        out_labels.release();
        out_stats.release();
        out_centroids.release();
        return 0;
    }
    return cv::connectedComponentsWithStats(binary_src, out_labels, out_stats, out_centroids,
                                            connectivity);
}

int ImageProcessor::floodFill(cv::Mat& image, cv::Mat* mask, cv::Point seed,
                              cv::Scalar new_val, cv::Scalar lo_diff,
                              cv::Scalar up_diff, cv::Rect* out_rect)
{
    if (image.empty())
        return -1;
    if (mask)
        return cv::floodFill(image, *mask, seed, new_val, out_rect, lo_diff, up_diff);
    // 无 mask 版重载：cv::floodFill(image, seed, new_val, rect, loDiff, upDiff, flags)
    return cv::floodFill(image, seed, new_val, out_rect, lo_diff, up_diff);
}

// ---------------- 14. 轮廓几何与形状度量 ----------------

double ImageProcessor::contourArea(const std::vector<cv::Point>& contour, bool oriented)
{
    if (contour.size() < 3)
        return 0.0;
    return cv::contourArea(contour, oriented);
}

bool ImageProcessor::isContourConvex(const std::vector<cv::Point>& contour)
{
    if (contour.size() < 3)
        return false;
    return cv::isContourConvex(contour);
}

cv::RotatedRect ImageProcessor::fitEllipse(const std::vector<cv::Point>& contour)
{
    if (contour.size() < 5)
        return cv::RotatedRect();
    return cv::fitEllipse(contour);
}

void ImageProcessor::fitLine(const std::vector<cv::Point>& points, std::vector<float>& out_line,
                             int dist_type, double param, double reps, double aeps)
{
    out_line.clear();
    if (points.size() < 2)
        return;
    cv::Vec4f l;
    cv::fitLine(points, l, dist_type, param, reps, aeps);
    out_line = { l[0], l[1], l[2], l[3] };
}

void ImageProcessor::minEnclosingCircle(const std::vector<cv::Point>& points,
                                        cv::Point2f& out_center, float& out_radius)
{
    out_center = cv::Point2f(0.f, 0.f);
    out_radius = 0.f;
    if (points.empty())
        return;
    cv::minEnclosingCircle(points, out_center, out_radius);
}

double ImageProcessor::pointPolygonTest(const std::vector<cv::Point>& contour, cv::Point2f pt,
                                        bool measure_dist)
{
    // 空轮廓：任何点都不在轮廓内，返回 0（边界语义）
    if (contour.empty())
        return 0.0;
    return cv::pointPolygonTest(contour, pt, measure_dist);
}

double ImageProcessor::matchShapes(const std::vector<cv::Point>& contour1,
                                   const std::vector<cv::Point>& contour2,
                                   int method, double parameter)
{
    // 空轮廓无法计算 Hu 矩：视为"完全不相似"，返回 double 最大值由业务判断
    if (contour1.empty() || contour2.empty())
        return (std::numeric_limits<double>::max)();
    return cv::matchShapes(contour1, contour2, method, 0.0);
}

void ImageProcessor::convexityDefects(const std::vector<cv::Point>& contour,
                                      const std::vector<int>& convex_hull,
                                      std::vector<cv::Vec4i>& out_defects)
{
    out_defects.clear();
    // 凸包至少 3 点，轮廓至少 4 点才能存在缺陷点
    if (contour.size() < 4 || convex_hull.size() < 3)
        return;
    // std::vector<int> 包点索引 → CV_32S 1xN Mat
    cv::Mat hullMat(convex_hull);
    cv::Mat defectsMat;
    cv::convexityDefects(contour, hullMat, defectsMat);
    if (defectsMat.empty())
        return;
    // CV_32SC4 (Nx1 Vec4i) → std::vector<cv::Vec4i>
    if (defectsMat.type() == CV_32SC4)
    {
        const size_t n = (size_t)defectsMat.total();
        out_defects.reserve(n);
        for (size_t i = 0; i < n; ++i)
            out_defects.push_back(defectsMat.at<cv::Vec4i>((int)i));
    }
}

// ---------------- 15. 直方图 ----------------

void ImageProcessor::calcHist8U(const cv::Mat& src, std::vector<float>& out_hist)
{
    out_hist.clear();
    if (src.empty() || src.type() != CV_8UC1)
        return;
    const int histSize = 256;
    float range[2] = { 0.f, 256.f };
    const float* ranges = { range };
    int channels = { 0 };
    cv::Mat histMat;
    cv::calcHist(&src, 1, &channels, cv::Mat(), histMat, 1, &histSize, &ranges);
    out_hist.resize((size_t)histSize);
    for (int i = 0; i < histSize; ++i)
        out_hist[(size_t)i] = histMat.at<float>(i);
}

double ImageProcessor::compareHist(const std::vector<float>& hist1, const std::vector<float>& hist2,
                                   int method)
{
    // 尺寸不等于 256 视为无效直方图
    if (hist1.size() != 256 || hist2.size() != 256)
        return 0.0;
    cv::Mat1f h1(1, 256, const_cast<float*>(hist1.data()));
    cv::Mat1f h2(1, 256, const_cast<float*>(hist2.data()));
    return cv::compareHist(h1, h2, method);
}

// ---------------- 16. 重映射与极坐标变换 ----------------

void ImageProcessor::remap(const cv::Mat& src, cv::Mat& dst, const cv::Mat& map1, const cv::Mat& map2,
                           int interpolation, cv::Scalar border_value)
{
    if (src.empty() || map1.empty() || map2.empty())
    {
        dst.release();
        return;
    }
    cv::remap(src, dst, map1, map2, interpolation, cv::BORDER_CONSTANT, border_value);
}

void ImageProcessor::warpPolar(const cv::Mat& src, cv::Mat& dst, cv::Size dsize,
                               cv::Point2f center, double max_radius, int flags)
{
    if (src.empty())
    {
        dst.release();
        return;
    }
    cv::warpPolar(src, dst, dsize, center, max_radius, flags);
}

void ImageProcessor::goodFeaturesToTrack(const cv::Mat& src,
                                         std::vector<cv::Point2f>& out_corners,
                                         int max_corners, double quality_level,
                                         double min_distance, const cv::Mat& mask)
{
    out_corners.clear();
    if (src.empty() || max_corners <= 0)
        return;
    if (mask.empty())
        cv::goodFeaturesToTrack(src, out_corners, max_corners, quality_level, min_distance);
    else
        cv::goodFeaturesToTrack(src, out_corners, max_corners, quality_level, min_distance,
                                mask);
}

void ImageProcessor::cornerSubPix(const cv::Mat& src, std::vector<cv::Point2f>& corners,
                                  cv::Size win_size, cv::Size zero_zone,
                                  cv::TermCriteria criteria)
{
    if (src.empty() || corners.empty())
        return;
    cv::cornerSubPix(src, corners, win_size, zero_zone, criteria);
}

void ImageProcessor::findNonZero(const cv::Mat& src, std::vector<cv::Point>& out_points)
{
    out_points.clear();
    if (src.empty())
        return;
    cv::findNonZero(src, out_points);
}

// ---------------- 卡尺与轮廓偏差 ----------------

namespace {

// 双线性采样（8U/16U/32F/64F 单通道；越界坐标 clamp；其他深度返回 0）
double pixelSampleBilinear(const cv::Mat& img, double x, double y)
{
    x = (std::min)((std::max)(x, 0.0), (double)img.cols - 1);
    y = (std::min)((std::max)(y, 0.0), (double)img.rows - 1);
    const int x0 = (int)x, y0 = (int)y;
    const int x1 = (std::min)(x0 + 1, img.cols - 1);
    const int y1 = (std::min)(y0 + 1, img.rows - 1);
    const double fx = x - x0, fy = y - y0;
    const auto at = [&img](int r, int c) -> double {
        switch (img.depth()) {
        case CV_8U: return img.at<uint8_t>(r, c);
        case CV_16U: return img.at<uint16_t>(r, c);
        case CV_32F: return img.at<float>(r, c);
        case CV_64F: return img.at<double>(r, c);
        default: return 0.0;
        }
    };
    const double v00 = at(y0, x0), v10 = at(y0, x1);
    const double v01 = at(y1, x0), v11 = at(y1, x1);
    return v00 * (1 - fx) * (1 - fy) + v10 * fx * (1 - fy) + v01 * (1 - fx) * fy +
           v11 * fx * fy;
}

} // namespace

int ImageProcessor::caliperEdges(const cv::Mat& image, cv::Point2f p1, cv::Point2f p2,
                                 int n_samples, int polarity, double min_gradient,
                                 std::vector<cv::Point2f>& out_edges)
{
    out_edges.clear();
    if (image.empty() || n_samples < 4)
        return 0;
    // 沿线段均匀采样
    std::vector<double> g((size_t)n_samples);
    for (int k = 0; k < n_samples; ++k) {
        const double t = (double)k / (double)(n_samples - 1);
        g[(size_t)k] = pixelSampleBilinear(image, p1.x + t * (p2.x - p1.x),
                                           p1.y + t * (p2.y - p1.y));
    }
    // 中心差分梯度 → 局部 |梯度| 极大（天然 NMS）→ 抛物线亚像素
    std::vector<double> grad((size_t)n_samples, 0.0);
    for (int k = 1; k + 1 < n_samples; ++k)
        grad[(size_t)k] = 0.5 * (g[(size_t)k + 1] - g[(size_t)k - 1]);
    int count = 0;
    for (int k = 1; k + 1 < n_samples; ++k) {
        const double a = std::fabs(grad[(size_t)k]);
        if (a < min_gradient)
            continue;
        if (polarity == 1 && grad[(size_t)k] < 0)
            continue;
        if (polarity == -1 && grad[(size_t)k] > 0)
            continue;
        if (std::fabs(grad[(size_t)k - 1]) > a || std::fabs(grad[(size_t)k + 1]) >= a)
            continue;   // 非局部极大（平台取首个）
        const double b1 = 0.5 * (grad[(size_t)k + 1] - grad[(size_t)k - 1]);
        const double b2 = 0.5 * (grad[(size_t)k - 1] + grad[(size_t)k + 1] - 2.0 * grad[(size_t)k]);
        double dt = 0.0;
        if (std::fabs(b2) > 1e-12)
            dt = std::clamp(-b1 / (2.0 * b2), -0.5, 0.5);
        const double t = ((double)k + dt) / (double)(n_samples - 1);
        out_edges.push_back(cv::Point2f((float)(p1.x + t * (p2.x - p1.x)),
                                        (float)(p1.y + t * (p2.y - p1.y))));
        ++count;
    }
    return count;
}

double ImageProcessor::contourHausdorff(const std::vector<cv::Point2f>& a,
                                        const std::vector<cv::Point2f>& b)
{
    if (a.empty() || b.empty())
        return -1.0;
    const auto directed = [](const std::vector<cv::Point2f>& from,
                             const std::vector<cv::Point2f>& to) {
        double worst = 0.0;
        for (const cv::Point2f& p : from) {
            double best = 1e300;
            for (const cv::Point2f& q : to) {
                const double dx = p.x - q.x, dy = p.y - q.y;
                const double d = dx * dx + dy * dy;
                if (d < best)
                    best = d;
            }
            worst = (std::max)(worst, std::sqrt(best));
        }
        return worst;
    };
    return (std::max)(directed(a, b), directed(b, a));
}

// ---------------- 17. 绘图标注（img 为空一律 no-op） ----------------

void ImageProcessor::drawLine(cv::Mat& img, cv::Point p1, cv::Point p2,
                              cv::Scalar color, int thickness)
{
    if (img.empty())
        return;
    cv::line(img, p1, p2, color, thickness);
}

void ImageProcessor::drawCircle(cv::Mat& img, cv::Point center, int radius,
                                cv::Scalar color, int thickness)
{
    if (img.empty())
        return;
    cv::circle(img, center, radius, color, thickness);
}

void ImageProcessor::drawRect(cv::Mat& img, cv::Rect rect, cv::Scalar color, int thickness)
{
    if (img.empty())
        return;
    cv::rectangle(img, rect, color, thickness);
}

void ImageProcessor::drawPolylines(cv::Mat& img, const std::vector<cv::Point>& points,
                                   bool is_closed, cv::Scalar color, int thickness)
{
    if (img.empty() || points.empty())
        return;
    cv::polylines(img, points, is_closed, color, thickness);
}

void ImageProcessor::fillPolygon(cv::Mat& img, const std::vector<cv::Point>& points,
                                 cv::Scalar color)
{
    if (img.empty() || points.size() < 3)
        return;
    cv::fillPoly(img, points, color);
}

void ImageProcessor::drawCross(cv::Mat& img, cv::Point center, int length,
                               cv::Scalar color, int thickness)
{
    if (img.empty())
        return;
    cv::line(img, center - cv::Point(length, 0), center + cv::Point(length, 0), color, thickness);
    cv::line(img, center - cv::Point(0, length), center + cv::Point(0, length), color, thickness);
}

void ImageProcessor::drawText(cv::Mat& img, const std::string& text, cv::Point org,
                              cv::Scalar color, double scale, int thickness)
{
    if (img.empty())
        return;
    cv::putText(img, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, color, thickness);
}

// ---------------- 18. 模板匹配增强 ----------------

void ImageProcessor::matchTemplateMasked(const cv::Mat& image, const cv::Mat& templ,
                                         const cv::Mat& mask, cv::Mat& result, int method)
{
    // OpenCV 4.4 仅 TM_SQDIFF / TM_CCORR_NORMED 支持掩膜（其余 method 直接抛错）——先拦截
    if (method != cv::TM_SQDIFF && method != cv::TM_CCORR_NORMED ||
        image.empty() || templ.empty() || mask.empty()) {
        result.release();
        return;
    }
    cv::matchTemplate(image, templ, result, method, mask);
    // 背景/遮挡区域分母为零会产生 NaN/Inf（实测 minMaxLoc 会被 inf 带偏）——
    // 归一为该方法的最差得分：SQDIFF = FLT_MAX，CCORR_NORMED = 0
    if (result.type() == CV_32F) {
        const float worst = (method == cv::TM_SQDIFF)
                                ? (std::numeric_limits<float>::max)()
                                : 0.0f;
        for (int r = 0; r < result.rows; ++r) {
            float* row = result.ptr<float>(r);
            for (int c = 0; c < result.cols; ++c)
                if (!std::isfinite(row[c]))
                    row[c] = worst;
        }
    }
}

void ImageProcessor::findBestMatchSubPix(const cv::Mat& result, int method,
                                         cv::Point2f& out_loc, double& out_score)
{
    out_loc = cv::Point2f(0.f, 0.f);
    out_score = 0.0;
    if (result.empty())
        return;
    // 整数峰（复用 findBestMatch 的取大/取小语义）
    cv::Point locI;
    double scoreI = 0.0;
    findBestMatch(result, method, locI, scoreI);
    out_loc = cv::Point2f((float)locI.x, (float)locI.y);
    out_score = scoreI;
    if (result.depth() != CV_32F && result.depth() != CV_64F)
        return;   // 非 matchTemplate 输出深度：保持整数结果
    const int x = locI.x, y = locI.y;
    if (x <= 0 || x >= result.cols - 1 || y <= 0 || y >= result.rows - 1)
        return;   // 贴边无三分点
    auto at = [&result](int r, int c) -> double {
        return result.depth() == CV_32F ? (double)result.at<float>(r, c)
                                        : result.at<double>(r, c);
    };
    const double c = at(y, x), l = at(y, x - 1), r = at(y, x + 1);
    const double t = at(y - 1, x), b = at(y + 1, x);
    // 水平/垂直抛物线：f(u) = c + b1·u + b2·u²，顶点 u* = -b1/(2b2)，f* = c - b1²/(4b2)
    const double b1x = 0.5 * (r - l), b2x = 0.5 * (l + r - 2.0 * c);
    const double b1y = 0.5 * (b - t), b2y = 0.5 * (t + b - 2.0 * c);
    double dx = 0.0, dy = 0.0, fs = c;
    if (std::fabs(b2x) > 1e-12) {
        dx = std::clamp(-b1x / (2.0 * b2x), -0.5, 0.5);
        fs -= b1x * b1x / (4.0 * b2x);
    }
    if (std::fabs(b2y) > 1e-12) {
        dy = std::clamp(-b1y / (2.0 * b2y), -0.5, 0.5);
        fs -= b1y * b1y / (4.0 * b2y);
    }
    out_loc.x += (float)dx;
    out_loc.y += (float)dy;
    out_score = fs;
}

void ImageProcessor::findAllMatches(const cv::Mat& result, int method, double score_threshold,
                                    int max_matches, int nms_radius,
                                    std::vector<Match2d>& out_matches)
{
    out_matches.clear();
    if (result.empty() || max_matches <= 0 || nms_radius < 0)
        return;
    const bool lowerBetter = (method == cv::TM_SQDIFF || method == cv::TM_SQDIFF_NORMED);
    cv::Mat work = result.clone();   // 抑制在副本上就地置最差值
    if (work.type() != CV_32F)
        work.convertTo(work, CV_32F);
    const float worst = lowerBetter ? (std::numeric_limits<float>::max)()
                                    : -(std::numeric_limits<float>::max)();
    while ((int)out_matches.size() < max_matches) {
        double minV = 0, maxV = 0;
        cv::Point minL, maxL;
        cv::minMaxLoc(work, &minV, &maxV, &minL, &maxL);
        const cv::Point best = lowerBetter ? minL : maxL;
        const double score = lowerBetter ? minV : maxV;
        if (lowerBetter ? (score > score_threshold) : (score < score_threshold))
            break;   // 其余目标低于阈值
        Match2d m;
        m.loc = cv::Point2f((float)best.x, (float)best.y);
        m.score = score;
        out_matches.push_back(m);
        const int x0 = (std::max)(0, best.x - nms_radius);
        const int y0 = (std::max)(0, best.y - nms_radius);
        const int x1 = (std::min)(work.cols - 1, best.x + nms_radius);
        const int y1 = (std::min)(work.rows - 1, best.y + nms_radius);
        if (x1 >= x0 && y1 >= y0)
            work(cv::Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1)).setTo(cv::Scalar(worst));
    }
}

// 旋转模板搜索：逐角度旋转模板匹配，取全局最优并亚像素细化
RotatedMatch2d ImageProcessor::findRotatedMatch(const cv::Mat& image, const cv::Mat& templ,
                                                double angle_start, double angle_end,
                                                double angle_step, int method)
{
    const bool lowerBetter = (method == cv::TM_SQDIFF || method == cv::TM_SQDIFF_NORMED);
    RotatedMatch2d best;
    best.loc = cv::Point2f(-1.f, -1.f);
    best.angle = 0.0;
    best.score = lowerBetter ? (std::numeric_limits<double>::max)()
                             : -(std::numeric_limits<double>::max)();
    if (image.empty() || templ.empty() || angle_step <= 0 || angle_end < angle_start)
        return best;
    const int steps =
        (std::min)(3600, (int)std::floor((angle_end - angle_start) / angle_step) + 1);
    if (steps <= 0)
        return best;
    const int w = templ.cols, h = templ.rows;
    for (int i = 0; i < steps; ++i) {
        const double ang = angle_start + angle_step * (double)i;
        // 模板绕自身中心旋转，画布扩到旋转后外接矩形（中心 = 原模板中心）
        const double rad = ang * CV_PI / 180.0;
        const double ca = std::cos(rad), sa = std::sin(rad);
        const int bw =
            (std::max)(1, (int)std::ceil(std::fabs((double)w * ca) + std::fabs((double)h * sa)));
        const int bh =
            (std::max)(1, (int)std::ceil(std::fabs((double)w * sa) + std::fabs((double)h * ca)));
        cv::Mat M = cv::getRotationMatrix2D(cv::Point2f(w * 0.5f, h * 0.5f), ang, 1.0);
        M.at<double>(0, 2) += (double)bw * 0.5 - (double)w * 0.5;
        M.at<double>(1, 2) += (double)bh * 0.5 - (double)h * 0.5;
        cv::Mat rotated;
        cv::warpAffine(templ, rotated, M, cv::Size(bw, bh), cv::INTER_LINEAR,
                       cv::BORDER_CONSTANT, cv::Scalar::all(0));
        cv::Mat result;
        matchTemplate(image, rotated, result, method);
        if (result.empty())
            continue;
        cv::Point2f sub;
        double subScore = 0;
        findBestMatchSubPix(result, method, sub, subScore);
        if ((lowerBetter && subScore < best.score) || (!lowerBetter && subScore > best.score)) {
            // 命中中心 = 旋转画布左上角（亚像素）+ 画布半宽高（模板中心）
            best.loc = cv::Point2f(sub.x + (float)bw * 0.5f, sub.y + (float)bh * 0.5f);
            best.angle = ang;
            best.score = subScore;
        }
    }
    return best;
}

// ---------------- 19. 基础形状拟合 ----------------

namespace {

// 圆拟合核心：委托 FitUtil.h 共享实现（与 CloudProcessor::fitCircle3D 同源）
bool fitCircleCore(const std::vector<cv::Point2f>& pts,
                   cv::Point2f& out_center, double& out_radius, double& out_rms)
{
    out_rms = 0.0;
    if (pts.size() < 3)
        return false;
    std::vector<double> xy;
    xy.reserve(pts.size() * 2);
    for (const cv::Point2f& p : pts) {
        xy.push_back((double)p.x);
        xy.push_back((double)p.y);
    }
    double cx = 0, cy = 0, r = 0, rms = 0;
    if (!detail::fitCircle2D(xy, cx, cy, r, rms))
        return false;
    out_center = cv::Point2f((float)cx, (float)cy);
    out_radius = r;
    out_rms = rms;
    return true;
}

// LCG 伪随机（fitCircleRobust 用；固定种子可复现，不引 <random> 重头）
inline unsigned int lcgNext(unsigned int& s)
{
    s = s * 1664525u + 1013904223u;
    return s;
}

} // namespace

bool ImageProcessor::fitCircle(const std::vector<cv::Point2f>& points,
                               cv::Point2f& out_center, double& out_radius, double& out_rms)
{
    return fitCircleCore(points, out_center, out_radius, out_rms);
}

bool ImageProcessor::fitCircle(const std::vector<cv::Point>& points,
                               cv::Point2f& out_center, double& out_radius, double& out_rms)
{
    std::vector<cv::Point2f> f;
    f.reserve(points.size());
    for (const cv::Point& p : points)
        f.push_back(cv::Point2f((float)p.x, (float)p.y));
    return fitCircleCore(f, out_center, out_radius, out_rms);
}

bool ImageProcessor::fitCircleRobust(const std::vector<cv::Point2f>& points, double threshold,
                                     cv::Point2f& out_center, double& out_radius, double& out_rms,
                                     std::vector<int>& out_inliers,
                                     unsigned int seed, int max_iters)
{
    out_inliers.clear();
    out_center = cv::Point2f(0.f, 0.f);
    out_radius = 0.0;
    out_rms = 0.0;
    if (points.size() < 3 || threshold <= 0 || max_iters <= 0)
        return false;

    // ---- RANSAC：随机三点定圆 → 内点计数（LCG 固定种子可复现） ----
    unsigned int s = seed ^ 0x9E3779B9u;   // 避免 seed=0 时前三点固定
    const int n = (int)points.size();
    int bestA = -1, bestB = -1, bestC = -1;
    int bestCount = -1;
    for (int it = 0; it < max_iters; ++it) {
        const int i1 = (int)(lcgNext(s) % (unsigned)n);
        int i2 = (int)(lcgNext(s) % (unsigned)n);
        int i3 = (int)(lcgNext(s) % (unsigned)n);
        if (i2 == i1) i2 = (i2 + 1) % n;
        if (i3 == i1 || i3 == i2) i3 = (i3 + 2) % n;
        const cv::Point2f& p1 = points[(size_t)i1];
        const cv::Point2f& p2 = points[(size_t)i2];
        const cv::Point2f& p3 = points[(size_t)i3];
        // 三点外接圆（行列式解）；共线跳过
        const double d = 2.0 * ((double)p1.x * ((double)p2.y - (double)p3.y) +
                                (double)p2.x * ((double)p3.y - (double)p1.y) +
                                (double)p3.x * ((double)p1.y - (double)p2.y));
        if (std::fabs(d) < 1e-9)
            continue;
        const double q1 = (double)p1.x * p1.x + (double)p1.y * p1.y;
        const double q2 = (double)p2.x * p2.x + (double)p2.y * p2.y;
        const double q3 = (double)p3.x * p3.x + (double)p3.y * p3.y;
        const double ux = (q1 * ((double)p2.y - (double)p3.y) +
                           q2 * ((double)p3.y - (double)p1.y) +
                           q3 * ((double)p1.y - (double)p2.y)) / d;
        const double uy = (q1 * ((double)p3.x - (double)p2.x) +
                           q2 * ((double)p1.x - (double)p3.x) +
                           q3 * ((double)p2.x - (double)p1.x)) / d;
        const double r = std::hypot((double)p1.x - ux, (double)p1.y - uy);
        if (!std::isfinite(r) || r > 1e9)
            continue;
        int count = 0;
        for (const cv::Point2f& p : points)
            if (std::fabs(std::hypot((double)p.x - ux, (double)p.y - uy) - r) <= threshold)
                ++count;
        if (count > bestCount) {
            bestCount = count;
            bestA = i1; bestB = i2; bestC = i3;
        }
    }
    if (bestCount < 3)
        return false;

    // ---- 最优三点圆收集内点 → GN 精化（共享核） ----
    std::vector<int> inliers;
    inliers.reserve((size_t)bestCount);
    {
        const cv::Point2f& p1 = points[(size_t)bestA];
        const cv::Point2f& p2 = points[(size_t)bestB];
        const cv::Point2f& p3 = points[(size_t)bestC];
        const double d = 2.0 * ((double)p1.x * ((double)p2.y - (double)p3.y) +
                                (double)p2.x * ((double)p3.y - (double)p1.y) +
                                (double)p3.x * ((double)p1.y - (double)p2.y));
        const double q1 = (double)p1.x * p1.x + (double)p1.y * p1.y;
        const double q2 = (double)p2.x * p2.x + (double)p2.y * p2.y;
        const double q3 = (double)p3.x * p3.x + (double)p3.y * p3.y;
        const double ux = (q1 * ((double)p2.y - (double)p3.y) +
                           q2 * ((double)p3.y - (double)p1.y) +
                           q3 * ((double)p1.y - (double)p2.y)) / d;
        const double uy = (q1 * ((double)p3.x - (double)p2.x) +
                           q2 * ((double)p1.x - (double)p3.x) +
                           q3 * ((double)p2.x - (double)p1.x)) / d;
        const double r = std::hypot((double)p1.x - ux, (double)p1.y - uy);
        for (int i = 0; i < n; ++i) {
            const cv::Point2f& p = points[(size_t)i];
            if (std::fabs(std::hypot((double)p.x - ux, (double)p.y - uy) - r) <= threshold)
                inliers.push_back(i);
        }
    }
    if (inliers.size() < 3)
        return false;
    std::vector<double> xy;
    xy.reserve(inliers.size() * 2);
    for (int i : inliers) {
        xy.push_back((double)points[(size_t)i].x);
        xy.push_back((double)points[(size_t)i].y);
    }
    double cx = 0, cy = 0, r = 0, rms = 0;
    if (!detail::fitCircle2D(xy, cx, cy, r, rms))
        return false;
    // 精化后圆上重算内点与 RMS（内点不足 3 时退回精化前口径）
    out_inliers.clear();
    double ss = 0;
    for (int i = 0; i < n; ++i) {
        const double dist = std::hypot((double)points[(size_t)i].x - cx,
                                       (double)points[(size_t)i].y - cy);
        const double res = std::fabs(dist - r);
        if (res <= threshold) {
            out_inliers.push_back(i);
            ss += res * res;
        }
    }
    if (out_inliers.size() < 3) {
        out_inliers = inliers;
        out_center = cv::Point2f((float)cx, (float)cy);
        out_radius = r;
        out_rms = rms;
        return true;
    }
    out_center = cv::Point2f((float)cx, (float)cy);
    out_radius = r;
    out_rms = std::sqrt(ss / (double)out_inliers.size());
    return true;
}

int ImageProcessor::fitCircleBatch(const std::vector<std::vector<cv::Point2f>>& contours,
                                   double threshold, std::vector<cv::Point2f>& out_centers,
                                   std::vector<double>& out_radii, std::vector<double>& out_rmss,
                                   std::vector<unsigned char>& out_valid,
                                   unsigned int seed, int max_iters)
{
    out_centers.clear();
    out_radii.clear();
    out_rmss.clear();
    out_valid.clear();
    const size_t n = contours.size();
    out_centers.reserve(n);
    out_radii.reserve(n);
    out_rmss.reserve(n);
    out_valid.reserve(n);
    int okCount = 0;
    for (const std::vector<cv::Point2f>& pts : contours) {
        cv::Point2f c;
        double r = 0, rms = 0;
        bool valid = false;
        if (threshold > 0) {
            std::vector<int> in;
            valid = fitCircleRobust(pts, threshold, c, r, rms, in, seed, max_iters);
        } else {
            valid = fitCircle(pts, c, r, rms);
        }
        out_centers.push_back(valid ? c : cv::Point2f());
        out_radii.push_back(valid ? r : 0.0);
        out_rmss.push_back(valid ? rms : 0.0);
        out_valid.push_back(valid ? 1 : 0);
        if (valid)
            ++okCount;
    }
    return okCount;
}

void ImageProcessor::approxPolyDP(const std::vector<cv::Point>& contour,
                                  std::vector<cv::Point>& out_approxCurve,
                                  double epsilon, bool closed)
{
    out_approxCurve.clear();
    if (contour.size() < 3)
        return;
    cv::approxPolyDP(contour, out_approxCurve, epsilon, closed);
}

// ---------------- 22. 统计与卷积 ----------------

void ImageProcessor::meanStdDev(const cv::Mat& src, cv::Scalar& out_mean,
                                cv::Scalar& out_stddev, const cv::Mat& mask)
{
    out_mean = cv::Scalar::all(0);
    out_stddev = cv::Scalar::all(0);
    if (src.empty())
        return;
    if (mask.empty())
        cv::meanStdDev(src, out_mean, out_stddev);
    else
        cv::meanStdDev(src, out_mean, out_stddev, mask);
}

int ImageProcessor::countNonZero(const cv::Mat& src)
{
    if (src.empty())
        return 0;
    return cv::countNonZero(src);
}

void ImageProcessor::minMaxLoc(const cv::Mat& src, double& out_min, double& out_max,
                               cv::Point& out_minLoc, cv::Point& out_maxLoc)
{
    out_min = 0;
    out_max = 0;
    out_minLoc = cv::Point();
    out_maxLoc = cv::Point();
    if (src.empty())
        return;
    cv::minMaxLoc(src, &out_min, &out_max, &out_minLoc, &out_maxLoc);
}

void ImageProcessor::normalize(const cv::Mat& src, cv::Mat& dst,
                               double alpha, double beta, int norm_type)
{
    if (src.empty()) {
        dst.release();
        return;
    }
    cv::normalize(src, dst, alpha, beta, norm_type);
}

void ImageProcessor::filter2D(const cv::Mat& src, cv::Mat& dst, int ddepth, const cv::Mat& kernel)
{
    if (src.empty() || kernel.empty()) {
        dst.release();
        return;
    }
    cv::filter2D(src, dst, ddepth, kernel);
}

void ImageProcessor::convertScaleAbs(const cv::Mat& src, cv::Mat& dst,
                                     double alpha, double beta)
{
    if (src.empty()) {
        dst.release();
        return;
    }
    cv::convertScaleAbs(src, dst, alpha, beta);
}

// ---------------- 23. 霍夫检测 ----------------

void ImageProcessor::houghLinesP(const cv::Mat& src, std::vector<cv::Vec4i>& out_lines,
                                 double rho, double theta, int threshold,
                                 double min_line_length, double max_line_gap)
{
    out_lines.clear();
    if (src.empty())
        return;
    cv::HoughLinesP(src, out_lines, rho, theta, threshold, min_line_length, max_line_gap);
}

void ImageProcessor::houghCircles(const cv::Mat& src, std::vector<cv::Vec3f>& out_circles,
                                  double dp, double min_dist, double param1,
                                  double param2, int min_radius, int max_radius)
{
    out_circles.clear();
    if (src.empty())
        return;
    cv::HoughCircles(src, out_circles, cv::HOUGH_GRADIENT, dp, min_dist,
                     param1, param2, min_radius, max_radius);
}

} // namespace shapely
