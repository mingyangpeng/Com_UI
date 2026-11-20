// Image2DAlg_Ops.cpp —— 通用 2D 算子（C ABI 封装）。
//
// 定位：本层是 ShapelyAlg::ImageProcessor 的上层，图像算子一律调用 ImageProcessor，
// 这里只负责：参数语义（通道/尺寸/模式）、C 容器 <-> cv::Mat 转换、结果结构组装。
// ImageProcessor 未封装的取值/组合（ROI、旋转矩阵、周长、矩、split/merge）直接用 OpenCV。
//
// 通道语义：输入 1/3/4 通道；自然单通道算子输出 1 通道，其余保持输入通道数。
// 异常不跨 DLL 边界：每个导出函数 try/catch 统一转 -99。

#include "include/Image2DAlg_api.h"
#include "ImageProcessor.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/video.hpp>   // calcOpticalFlowPyrLK / findTransformECC（world440 全含，无新增链接依赖）
#include <opencv2/photo.hpp>   // fastNlMeansDenoising(Colored)（world440 全含，无新增链接依赖）

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using shapely::ImageProcessor;

namespace {

const double kPi = 3.14159265358979323846;

// 输入容器 -> 零拷贝 Mat 视图（返回 0 成功；-1 空图；-3 通道不支持）
int Wrap(const Image2DAlg_Image* in, cv::Mat& mat) {
    if (!in || !in->data || in->width <= 0 || in->height <= 0) return -1;
    if (in->channels != 1 && in->channels != 3 && in->channels != 4) return -3;
    mat = cv::Mat(in->height, in->width, CV_8UC(in->channels), (void*)in->data);
    return 0;
}

// Mat -> 输出容器（malloc 紧排缓冲，与 Image2DAlg_FreeImage 配对）
int CopyOut(const cv::Mat& src, Image2DAlg_Image* out) {
    cv::Mat c = src.isContinuous() ? src : src.clone();
    out->width = c.cols;
    out->height = c.rows;
    out->channels = c.channels();
    size_t size = c.total() * c.elemSize();
    out->data = (unsigned char*)malloc(size);
    if (!out->data) return -4;
    memcpy(out->data, c.data, size);
    return 0;
}

// 转灰度（1 通道原样返回；3/4 通道走 cvtColor）
cv::Mat GrayOf(const cv::Mat& src) {
    if (src.channels() == 1) return src;
    cv::Mat gray;
    ImageProcessor::cvtColor(src, gray,
                             src.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
    return gray;
}

// 二值化：彩色先转灰度，非零像素视为前景
cv::Mat Binarize(const cv::Mat& src) {
    cv::Mat bin;
    ImageProcessor::threshold(GrayOf(src), bin, 0, 255, cv::THRESH_BINARY);
    return bin;
}

// 修正为不小于 min_value 的奇数（OpenCV 核尺寸要求）
int OddAtLeast(int value, int min_value) {
    if (value < min_value) value = min_value;
    if ((value & 1) == 0) ++value;
    return value;
}

// ROI 钳制：w/h <= 0 视为全图；与图像无交集返回空 Rect（调用方转 -3）
cv::Rect ClampRoi(const cv::Mat& src, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return cv::Rect(0, 0, src.cols, src.rows);
    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = (x + w) > src.cols ? src.cols : (x + w);
    const int y1 = (y + h) > src.rows ? src.rows : (y + h);
    if (x1 <= x0 || y1 <= y0) return cv::Rect();
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

// Kåsa 最小二乘圆拟合（SVD 求解）；退化（无解 / r² <= 0）返回 false
bool KasaFitPoints(const std::vector<cv::Point2f>& pts, Image2DAlg_CircleFit* out) {
    const int n = (int)pts.size();
    if (n < 3) return false;
    cv::Mat A(n, 3, CV_64F), b(n, 1, CV_64F);
    for (int i = 0; i < n; ++i) {
        const double x = pts[i].x, y = pts[i].y;
        A.at<double>(i, 0) = x;
        A.at<double>(i, 1) = y;
        A.at<double>(i, 2) = 1.0;
        b.at<double>(i, 0) = -(x * x + y * y);
    }
    cv::Mat sol;
    if (cv::solve(A, b, sol, cv::DECOMP_SVD) == 0) return false;
    const double a = sol.at<double>(0), b2 = sol.at<double>(1), c = sol.at<double>(2);
    const double r2 = a * a / 4.0 + b2 * b2 / 4.0 - c;
    if (r2 <= 0.0) return false;
    out->cx = -a / 2.0;
    out->cy = -b2 / 2.0;
    out->r = std::sqrt(r2);
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double dx = pts[i].x - out->cx, dy = pts[i].y - out->cy;
        const double d = std::sqrt(dx * dx + dy * dy) - out->r;
        sum += d * d;
    }
    out->rms = std::sqrt(sum / n);
    return true;
}

// 亚像素边缘细化：对整型轮廓点，沿 Sobel 梯度方向 3 点采样 |梯度|，
// 抛物线顶点插值取峰值位置（灰度图需已做 3x3 高斯预平滑）。
// 梯度过弱（平坦区）的点保持原位置。
void SubPixelEdgePoints(const cv::Mat& gray, const std::vector<cv::Point>& contour,
                        std::vector<cv::Point2f>& out) {
    cv::Mat gx, gy;
    cv::Sobel(gray, gx, CV_32F, 1, 0, 3);
    cv::Sobel(gray, gy, CV_32F, 0, 1, 3);
    out.reserve(contour.size());
    for (const cv::Point& p : contour) {
        const float gx0 = gx.at<float>(p.y, p.x), gy0 = gy.at<float>(p.y, p.x);
        const float mag0 = std::sqrt(gx0 * gx0 + gy0 * gy0);
        if (mag0 < 1.0f) {   // 平坦区：梯度不可靠，保持整型位置
            out.push_back(cv::Point2f((float)p.x, (float)p.y));
            continue;
        }
        const float dx = gx0 / mag0, dy = gy0 / mag0;
        // 双线性采样 p±d 处的梯度分量
        auto sample = [&](float x, float y, float& vx, float& vy) {
            x = std::min(std::max(x, 0.0f), (float)(gray.cols - 1));
            y = std::min(std::max(y, 0.0f), (float)(gray.rows - 1));
            const int xi = (int)x, yi = (int)y;
            const int xi1 = std::min(xi + 1, gray.cols - 1);
            const int yi1 = std::min(yi + 1, gray.rows - 1);
            const float fx = x - xi, fy = y - yi;
            vx = (1 - fx) * ((1 - fy) * gx.at<float>(yi, xi) + fy * gx.at<float>(yi1, xi)) +
                 fx * ((1 - fy) * gx.at<float>(yi, xi1) + fy * gx.at<float>(yi1, xi1));
            vy = (1 - fx) * ((1 - fy) * gy.at<float>(yi, xi) + fy * gy.at<float>(yi1, xi)) +
                 fx * ((1 - fy) * gy.at<float>(yi, xi1) + fy * gy.at<float>(yi1, xi1));
        };
        float gxp, gyp, gxm, gym;
        sample(p.x + dx, p.y + dy, gxp, gyp);
        sample(p.x - dx, p.y - dy, gxm, gym);
        const float mp = std::sqrt(gxp * gxp + gyp * gyp);
        const float mm = std::sqrt(gxm * gxm + gym * gym);
        const float denom = mm - 2.0f * mag0 + mp;
        float t = 0.0f;
        if (std::fabs(denom) > 1e-6f)
            t = std::min(std::max(0.5f * (mm - mp) / denom, -1.0f), 1.0f);
        out.push_back(cv::Point2f(p.x + t * dx, p.y + t * dy));
    }
}

} // namespace

extern "C" {

int Image2DAlg_ToGray(const Image2DAlg_Image* in, Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    try {
        return CopyOut(GrayOf(src), out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Gamma(const Image2DAlg_Image* in, double gamma, Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    if (gamma < 0.05) gamma = 0.05;
    try {
        cv::Mat lut(1, 256, CV_8UC1);
        for (int i = 0; i < 256; ++i)
            lut.at<uchar>(i) = cv::saturate_cast<uchar>(255.0 * pow(i / 255.0, 1.0 / gamma));
        cv::Mat dst;
        ImageProcessor::lut(src, lut, dst);
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Threshold(const Image2DAlg_Image* in, int mode, double thresh, double maxval,
                         int block_size, double C, Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    if (mode < 0 || mode > 4) return -3;
    try {
        cv::Mat gray = GrayOf(src), dst;
        switch (mode) {
        case 0: ImageProcessor::threshold(gray, dst, thresh, maxval, cv::THRESH_BINARY); break;
        case 1: ImageProcessor::threshold(gray, dst, thresh, maxval, cv::THRESH_BINARY_INV); break;
        case 2: ImageProcessor::threshold(gray, dst, 0, maxval,
                                          cv::THRESH_BINARY | cv::THRESH_OTSU); break;
        case 3: ImageProcessor::adaptiveThreshold(gray, dst, maxval,
                                                  cv::ADAPTIVE_THRESH_MEAN_C,
                                                  OddAtLeast(block_size, 3), C); break;
        default: ImageProcessor::adaptiveThreshold(gray, dst, maxval,
                                                   cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                                                   OddAtLeast(block_size, 3), C); break;
        }
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Binarize(const Image2DAlg_Image* in, int mode,
                        double thresh, double thresh2, double maxval, double mid_val,
                        int block_size, double C, int invert, Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    if (mode < 0 || mode > 10) return -3;

    // 钳制到 8 位灰度范围
    auto clamp8 = [](double v) -> int {
        if (v < 0.0) return 0;
        if (v > 255.0) return 255;
        return (int)(v + 0.5);
    };
    const int hi_maxval = clamp8(maxval);

    // 双阈值合法性：high >= low，否则 -3
    if (mode >= 8 && thresh2 < thresh) return -3;

    try {
        cv::Mat gray = GrayOf(src), dst;

        if (mode == 3 || mode == 4) {
            // 自适应：光照不均场景
            ImageProcessor::adaptiveThreshold(gray, dst, hi_maxval,
                mode == 3 ? cv::ADAPTIVE_THRESH_MEAN_C : cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                OddAtLeast(block_size, 3), C);
        } else if (mode >= 8) {
            // 双阈值：256 项 LUT 分段映射，O(1)/像素
            const int low = clamp8(thresh), high = clamp8(thresh2);
            const int mid = clamp8(mid_val);
            cv::Mat lut(1, 256, CV_8UC1);
            uchar* t = lut.ptr<uchar>(0);
            for (int v = 0; v < 256; ++v) {
                int o;
                if (mode == 8)       o = (v >= low && v <= high) ? hi_maxval : 0;
                else if (mode == 9)  o = (v < low || v > high) ? hi_maxval : 0;
                else /* mode == 10 */ o = (v < low) ? 0 : ((v <= high) ? mid : hi_maxval);
                t[v] = (uchar)o;
            }
            ImageProcessor::lut(gray, lut, dst);
        } else {
            // 单阈值族：cv::threshold 透传（OTSU 时 thresh 忽略）
            int cv_type = cv::THRESH_BINARY;
            double t = thresh;
            switch (mode) {
            case 0: cv_type = cv::THRESH_BINARY;     break;
            case 1: cv_type = cv::THRESH_BINARY_INV; break;
            case 2: cv_type = cv::THRESH_BINARY | cv::THRESH_OTSU; t = 0; break;
            case 5: cv_type = cv::THRESH_TRUNC;      break;
            case 6: cv_type = cv::THRESH_TOZERO;     break;
            default: cv_type = cv::THRESH_TOZERO_INV; break;  // mode 7
            }
            ImageProcessor::threshold(gray, dst, t, hi_maxval, cv_type);
        }

        // 统一反转：v -> 255 - v（对所有 mode 生效；二值图等价于前景/背景互换）
        if (invert) {
            cv::Mat flipped;
            ImageProcessor::bitwiseNot(dst, flipped);
            dst = flipped;
        }
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Blur(const Image2DAlg_Image* in, int method, int ksize, double sigma1,
                    double sigma2, Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    if (method < 0 || method > 2) return -3;
    try {
        cv::Mat dst;
        if (method == 0) {
            int k = OddAtLeast(ksize, 1);
            ImageProcessor::gaussianBlur(src, dst, cv::Size(k, k), sigma1, sigma2);
        } else if (method == 1) {
            ImageProcessor::medianBlur(src, dst, OddAtLeast(ksize, 3));
        } else if (src.channels() == 4) {
            // 双边滤波不支持 4 通道：BGR 处理后保留 alpha
            std::vector<cv::Mat> ch;
            cv::split(src, ch);
            cv::Mat bgr, blurred;
            cv::merge(std::vector<cv::Mat>{ch[0], ch[1], ch[2]}, bgr);
            ImageProcessor::bilateralFilter(bgr, blurred, ksize > 0 ? ksize : 5, sigma1, sigma2);
            std::vector<cv::Mat> out_ch;
            cv::split(blurred, out_ch);
            out_ch.push_back(ch[3]);
            cv::merge(out_ch, dst);
        } else {
            ImageProcessor::bilateralFilter(src, dst, ksize > 0 ? ksize : 5, sigma1, sigma2);
        }
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Morphology(const Image2DAlg_Image* in, int op, int shape, int ksize,
                          int iterations, Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    if (op < 0 || op > 6 || shape < 0 || shape > 2) return -3;
    try {
        const int shape_map[3] = {cv::MORPH_RECT, cv::MORPH_CROSS, cv::MORPH_ELLIPSE};
        cv::Mat kernel = ImageProcessor::makeKernel(shape_map[shape],
                                                    cv::Size(OddAtLeast(ksize, 1),
                                                             OddAtLeast(ksize, 1)));
        cv::Mat work = src.clone();   // 逐次迭代就地处理，避免改动调用方输入
        int count = iterations < 1 ? 1 : iterations;
        for (int i = 0; i < count; ++i) {
            switch (op) {
            case 0: ImageProcessor::erode(work, work, kernel); break;
            case 1: ImageProcessor::dilate(work, work, kernel); break;
            // op 2~6 与 cv::MORPH_OPEN~BLACKHAT 枚举值一致（2~6）
            default: ImageProcessor::morphologyEx(work, work, op, kernel); break;
            }
        }
        return CopyOut(work, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Canny(const Image2DAlg_Image* in, double threshold1, double threshold2,
                     Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    try {
        cv::Mat dst;
        ImageProcessor::canny(GrayOf(src), dst, threshold1, threshold2);
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Sobel(const Image2DAlg_Image* in, int dx, int dy, int ksize,
                     Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    try {
        cv::Mat grad16, grad8;
        ImageProcessor::sobel(GrayOf(src), grad16, CV_16S, dx, dy, ksize);
        cv::convertScaleAbs(grad16, grad8);   // |梯度| 缩放到 0~255
        return CopyOut(grad8, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Resize(const Image2DAlg_Image* in, int out_width, int out_height,
                      int interpolation, Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    if (out_width <= 0 || out_height <= 0) return -3;
    try {
        cv::Mat dst;
        ImageProcessor::resize(src, dst, cv::Size(out_width, out_height), 0, 0, interpolation);
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Crop(const Image2DAlg_Image* in, int x, int y, int width, int height,
                    Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    try {
        int x0 = x < 0 ? 0 : x;
        int y0 = y < 0 ? 0 : y;
        int x1 = (x + width) > src.cols ? src.cols : (x + width);
        int y1 = (y + height) > src.rows ? src.rows : (y + height);
        if (x1 <= x0 || y1 <= y0) return -3;
        return CopyOut(src(cv::Rect(x0, y0, x1 - x0, y1 - y0)).clone(), out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Rotate(const Image2DAlg_Image* in, double angle, double scale,
                      int interpolation, Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    try {
        cv::Point2f center(src.cols / 2.0f, src.rows / 2.0f);
        cv::Mat m = cv::getRotationMatrix2D(center, angle, scale);
        cv::Mat dst;
        cv::warpAffine(src, dst, m, src.size(), interpolation);
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_HSVThresholdWrap(const Image2DAlg_Image* in, double h_lo, double h_hi,
                                double s_lo, double s_hi, double v_lo, double v_hi,
                                Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    if (src.channels() == 1) return -3;
    try {
        cv::Mat hsv;
        if (src.channels() == 4) {   // OpenCV 无 BGRA2HSV：先去 alpha
            cv::Mat bgr;
            ImageProcessor::cvtColor(src, bgr, cv::COLOR_BGRA2BGR);
            ImageProcessor::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
        } else {
            ImageProcessor::cvtColor(src, hsv, cv::COLOR_BGR2HSV);
        }
        cv::Mat mask;
        if (h_lo <= h_hi) {
            cv::inRange(hsv, cv::Scalar(h_lo, s_lo, v_lo), cv::Scalar(h_hi, s_hi, v_hi), mask);
        } else {   // 红色跨 0：[h_lo,180] ∪ [0,h_hi]
            cv::Mat hi_part, lo_part;
            cv::inRange(hsv, cv::Scalar(h_lo, s_lo, v_lo), cv::Scalar(180, s_hi, v_hi), hi_part);
            cv::inRange(hsv, cv::Scalar(0, s_lo, v_lo), cv::Scalar(h_hi, s_hi, v_hi), lo_part);
            cv::bitwise_or(hi_part, lo_part, mask);
        }
        return CopyOut(mask, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_WarpAffine(const Image2DAlg_Image* in, const double* m,
                          int out_width, int out_height, int interpolation,
                          Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!m || !out) return -1;
    if (out_width <= 0 || out_height <= 0) return -3;
    try {
        // ImageProcessor::warpAffine 无 interpolation/border 参数，直接用 cv 并固定黑边
        cv::Mat mt(2, 3, CV_64F, (void*)m);
        cv::Mat dst;
        cv::warpAffine(src, dst, mt, cv::Size(out_width, out_height),
                       interpolation, cv::BORDER_CONSTANT, cv::Scalar());
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_WarpPerspective(const Image2DAlg_Image* in, const double* m,
                               int out_width, int out_height, int interpolation,
                               Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!m || !out) return -1;
    if (out_width <= 0 || out_height <= 0) return -3;
    try {
        cv::Mat mt(3, 3, CV_64F, (void*)m);
        cv::Mat dst;
        cv::warpPerspective(src, dst, mt, cv::Size(out_width, out_height),
                            interpolation, cv::BORDER_CONSTANT, cv::Scalar());
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Enhance(const Image2DAlg_Image* in, int method, double clip_limit,
                       int tile_size, Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    if (method < 0 || method > 1) return -3;
    try {
        cv::Mat dst;
        if (method == 0) {
            ImageProcessor::equalizeHist(GrayOf(src), dst);   // 单通道输出
        } else {
            int tile = tile_size > 0 ? tile_size : 8;
            cv::Size grid(tile, tile);
            if (src.channels() == 1) {
                ImageProcessor::clahe(src, dst, clip_limit, grid);
            } else if (src.channels() == 3) {
                cv::Mat lab;
                ImageProcessor::cvtColor(src, lab, cv::COLOR_BGR2Lab);
                std::vector<cv::Mat> ch;
                cv::split(lab, ch);
                ImageProcessor::clahe(ch[0], ch[0], clip_limit, grid);
                cv::merge(ch, lab);
                ImageProcessor::cvtColor(lab, dst, cv::COLOR_Lab2BGR);
            } else {
                // 4 通道：alpha 不参与增强
                std::vector<cv::Mat> ch;
                cv::split(src, ch);
                cv::Mat bgr;
                cv::merge(std::vector<cv::Mat>{ch[0], ch[1], ch[2]}, bgr);
                cv::Mat lab;
                ImageProcessor::cvtColor(bgr, lab, cv::COLOR_BGR2Lab);
                std::vector<cv::Mat> lab_ch;
                cv::split(lab, lab_ch);
                ImageProcessor::clahe(lab_ch[0], lab_ch[0], clip_limit, grid);
                cv::merge(lab_ch, lab);
                cv::Mat enhanced;
                ImageProcessor::cvtColor(lab, enhanced, cv::COLOR_Lab2BGR);
                std::vector<cv::Mat> out_ch;
                cv::split(enhanced, out_ch);
                out_ch.push_back(ch[3]);
                cv::merge(out_ch, dst);
            }
        }
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

// 连通域核心：src 可为 ROI 视图，ox/oy 为结果坐标补偿（ROI 原点）
int RunConnectedComponents(const cv::Mat& src, int connectivity, int min_area, int max_area,
                           Image2DAlg_Blob** out_blobs, int* out_count, int ox, int oy) {
    *out_blobs = nullptr;
    *out_count = 0;
    cv::Mat labels, stats, centroids;
    int num = ImageProcessor::connectedComponentsWithStats(Binarize(src), labels, stats,
                                                           centroids, connectivity);
    int capacity = num > 1 ? num - 1 : 0;   // 标签 0 是背景
    if (capacity == 0) return 0;

    Image2DAlg_Blob* arr = (Image2DAlg_Blob*)malloc(sizeof(Image2DAlg_Blob) * capacity);
    if (!arr) return -4;
    int k = 0;
    for (int i = 1; i < num; ++i) {
        int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area < min_area) continue;
        if (max_area > 0 && area > max_area) continue;
        Image2DAlg_Blob& b = arr[k++];
        b.label = i;
        b.area = area;
        b.left = stats.at<int>(i, cv::CC_STAT_LEFT) + ox;
        b.top = stats.at<int>(i, cv::CC_STAT_TOP) + oy;
        b.width = stats.at<int>(i, cv::CC_STAT_WIDTH);
        b.height = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        b.cx = centroids.at<double>(i, 0) + ox;
        b.cy = centroids.at<double>(i, 1) + oy;
    }
    if (k == 0) {
        free(arr);
        return 0;
    }
    *out_blobs = arr;
    *out_count = k;
    return 0;
}

int Image2DAlg_ConnectedComponents(const Image2DAlg_Image* in, int connectivity,
                                   int min_area, int max_area,
                                   Image2DAlg_Blob** out_blobs, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_blobs || !out_count) return -1;
    if (connectivity != 4 && connectivity != 8) return -3;
    try {
        return RunConnectedComponents(src, connectivity, min_area, max_area,
                                      out_blobs, out_count, 0, 0);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_ConnectedComponentsROI(const Image2DAlg_Image* in, int roi_x, int roi_y,
                                      int roi_w, int roi_h, int connectivity,
                                      int min_area, int max_area,
                                      Image2DAlg_Blob** out_blobs, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_blobs || !out_count) return -1;
    if (connectivity != 4 && connectivity != 8) return -3;
    try {
        const cv::Rect roi = ClampRoi(src, roi_x, roi_y, roi_w, roi_h);
        if (roi.empty()) return -3;   // ROI 与图像无交集
        return RunConnectedComponents(src(roi), connectivity, min_area, max_area,
                                      out_blobs, out_count, roi.x, roi.y);
    } catch (const cv::Exception&) {
        return -99;
    }
}

// 轮廓测量核心：src 可为 ROI 视图，ox/oy 为结果坐标补偿
int RunAnalyzeContours(const cv::Mat& src, int external_only, int min_area,
                       Image2DAlg_ContourInfo** out_contours, int* out_count, int ox, int oy) {
    *out_contours = nullptr;
    *out_count = 0;
    std::vector<std::vector<cv::Point>> contours;
    ImageProcessor::findContours(Binarize(src), contours,
                                 external_only ? cv::RETR_EXTERNAL : cv::RETR_LIST,
                                 cv::CHAIN_APPROX_SIMPLE);

    std::vector<Image2DAlg_ContourInfo> result;
    result.reserve(contours.size());
    for (const auto& c : contours) {
        double area = ImageProcessor::contourArea(c);
        if (area < min_area || c.empty()) continue;

        Image2DAlg_ContourInfo info = {};
        info.area = area;
        info.perimeter = cv::arcLength(c, true);
        info.circularity = info.perimeter > 0.0
                               ? 4.0 * kPi * area / (info.perimeter * info.perimeter)
                               : 0.0;

        cv::Rect box = ImageProcessor::getBoundingRect(c);
        info.bbox_x = box.x + ox;
        info.bbox_y = box.y + oy;
        info.bbox_w = box.width;
        info.bbox_h = box.height;

        cv::Moments m = cv::moments(c);
        if (m.m00 != 0.0) {
            info.center_x = m.m10 / m.m00 + ox;
            info.center_y = m.m01 / m.m00 + oy;
        } else {
            info.center_x = box.x + box.width * 0.5 + ox;
            info.center_y = box.y + box.height * 0.5 + oy;
        }

        cv::RotatedRect rect = ImageProcessor::getMinAreaRect(c);
        info.min_rect_cx = rect.center.x + ox;
        info.min_rect_cy = rect.center.y + oy;
        info.min_rect_w = rect.size.width;
        info.min_rect_h = rect.size.height;
        info.min_rect_angle = rect.angle;

        cv::Point2f cc;
        float radius = 0;
        ImageProcessor::minEnclosingCircle(c, cc, radius);
        info.circle_cx = cc.x + ox;
        info.circle_cy = cc.y + oy;
        info.circle_r = radius;

        result.push_back(info);
    }

    if (result.empty()) return 0;
    size_t bytes = sizeof(Image2DAlg_ContourInfo) * result.size();
    Image2DAlg_ContourInfo* arr = (Image2DAlg_ContourInfo*)malloc(bytes);
    if (!arr) return -4;
    memcpy(arr, result.data(), bytes);
    *out_contours = arr;
    *out_count = (int)result.size();
    return 0;
}

int Image2DAlg_AnalyzeContours(const Image2DAlg_Image* in, int external_only, int min_area,
                               Image2DAlg_ContourInfo** out_contours, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_contours || !out_count) return -1;
    try {
        return RunAnalyzeContours(src, external_only, min_area, out_contours, out_count, 0, 0);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_AnalyzeContoursROI(const Image2DAlg_Image* in, int roi_x, int roi_y,
                                  int roi_w, int roi_h, int external_only, int min_area,
                                  Image2DAlg_ContourInfo** out_contours, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_contours || !out_count) return -1;
    try {
        const cv::Rect roi = ClampRoi(src, roi_x, roi_y, roi_w, roi_h);
        if (roi.empty()) return -3;   // ROI 与图像无交集
        return RunAnalyzeContours(src(roi), external_only, min_area,
                                  out_contours, out_count, roi.x, roi.y);
    } catch (const cv::Exception&) {
        return -99;
    }
}

// 模板匹配核心：img 可为 ROI 视图，ox/oy 为命中位置补偿
int RunMatchTemplate(const cv::Mat& img, const cv::Mat& tpl, int method,
                     double* out_score, int* out_x, int* out_y, int ox, int oy) {
    cv::Mat result;
    ImageProcessor::matchTemplate(img, tpl, result, method);
    cv::Point loc;
    ImageProcessor::findBestMatch(result, method, loc, *out_score);
    *out_x = loc.x + ox;
    *out_y = loc.y + oy;
    return 0;
}

int Image2DAlg_MatchTemplate(const Image2DAlg_Image* image, const Image2DAlg_Image* templ,
                             int method, double* out_score, int* out_x, int* out_y) {
    cv::Mat img, tpl;
    if (!out_score || !out_x || !out_y) return -1;
    if (int rc = Wrap(image, img)) return rc;
    if (int rc = Wrap(templ, tpl)) return rc;
    if (method < 0 || method > 5) return -3;
    if (img.channels() != tpl.channels() ||
        tpl.cols > img.cols || tpl.rows > img.rows) return -3;
    try {
        return RunMatchTemplate(img, tpl, method, out_score, out_x, out_y, 0, 0);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_MatchTemplateROI(const Image2DAlg_Image* image,
                                int roi_x, int roi_y, int roi_w, int roi_h,
                                const Image2DAlg_Image* templ, int method,
                                double* out_score, int* out_x, int* out_y) {
    cv::Mat img, tpl;
    if (!out_score || !out_x || !out_y) return -1;
    if (int rc = Wrap(image, img)) return rc;
    if (int rc = Wrap(templ, tpl)) return rc;
    if (method < 0 || method > 5) return -3;
    if (img.channels() != tpl.channels()) return -3;
    try {
        const cv::Rect roi = ClampRoi(img, roi_x, roi_y, roi_w, roi_h);
        if (roi.empty()) return -3;              // ROI 与图像无交集
        if (tpl.cols > roi.width || tpl.rows > roi.height) return -3;   // 模板大于 ROI
        return RunMatchTemplate(img(roi), tpl, method, out_score, out_x, out_y, roi.x, roi.y);
    } catch (const cv::Exception&) {
        return -99;
    }
}

void Image2DAlg_FreeBlobs(Image2DAlg_Blob* blobs) {
    free(blobs);
}

int Image2DAlg_FitCirclesFromBinary(const Image2DAlg_Image* in,
                                    int min_area, int max_area,
                                    double min_r, double max_r, int subpixel,
                                    Image2DAlg_CircleFit** out_circles, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_circles || !out_count) return -1;
    *out_circles = nullptr;
    *out_count = 0;
    try {
        cv::Mat bin = Binarize(src);
        std::vector<std::vector<cv::Point>> contours;
        ImageProcessor::findContours(bin, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);

        cv::Mat gray;
        if (subpixel) {   // 预平滑稳定梯度（抗锯齿边缘上抛物线插值才有意义）
            cv::GaussianBlur(GrayOf(src), gray, cv::Size(3, 3), 0);
        }

        std::vector<Image2DAlg_CircleFit> result;
        for (const auto& c : contours) {
            const double area = ImageProcessor::contourArea(c);
            if (area < min_area) continue;
            if (max_area > 0 && area > max_area) continue;
            if ((int)c.size() < 3) continue;

            std::vector<cv::Point2f> pts;
            pts.reserve(c.size());
            for (const cv::Point& p : c)
                pts.push_back(cv::Point2f((float)p.x, (float)p.y));

            Image2DAlg_CircleFit fit;
            if (!KasaFitPoints(pts, &fit)) continue;   // 退化轮廓静默跳过
            if (min_r > 0 && fit.r < min_r) continue;
            if (max_r > 0 && fit.r > max_r) continue;

            if (subpixel) {
                std::vector<cv::Point2f> refined;
                SubPixelEdgePoints(gray, c, refined);
                Image2DAlg_CircleFit fine;
                if (KasaFitPoints(refined, &fine)) fit = fine;
            }
            result.push_back(fit);
        }

        if (result.empty()) return 0;
        size_t bytes = sizeof(Image2DAlg_CircleFit) * result.size();
        Image2DAlg_CircleFit* arr = (Image2DAlg_CircleFit*)malloc(bytes);
        if (!arr) return -4;
        memcpy(arr, result.data(), bytes);
        *out_circles = arr;
        *out_count = (int)result.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_MatchShapes(const float* xy1, int count1, const float* xy2, int count2,
                           int method, double* out_score) {
    if (!xy1 || !xy2 || !out_score) return -1;
    if (count1 < 3 || count2 < 3) return -3;
    if (method < 1 || method > 3) return -3;   // cv::CONTOURS_MATCH_I1~I3
    try {
        std::vector<cv::Point> c1, c2;
        c1.reserve(count1);
        c2.reserve(count2);
        for (int i = 0; i < count1; ++i)
            c1.push_back(cv::Point((int)std::lround(xy1[i * 2]),
                                   (int)std::lround(xy1[i * 2 + 1])));
        for (int i = 0; i < count2; ++i)
            c2.push_back(cv::Point((int)std::lround(xy2[i * 2]),
                                   (int)std::lround(xy2[i * 2 + 1])));
        *out_score = ImageProcessor::matchShapes(c1, c2, method, 0.0);
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_ConvexityDefects(const float* xy, int count,
                                Image2DAlg_Defect** out_defects, int* out_count) {
    if (!xy || !out_defects || !out_count) return -1;
    *out_defects = nullptr;
    *out_count = 0;
    if (count < 3) return -3;
    try {
        std::vector<cv::Point> pts;
        pts.reserve(count);
        for (int i = 0; i < count; ++i)
            pts.push_back(cv::Point((int)std::lround(xy[i * 2]),
                                    (int)std::lround(xy[i * 2 + 1])));
        // convexityDefects 隐含要求凸包为 CCW 方向（与 findContours 正规用法一致），
        // 顺时针轮廓必须先反转点序，否则缺陷静默算错（星形轮廓模糊测试实证）。
        // 旧实现的缺陷：反转后索引落在反转数组上、而"是否反转"未对外暴露，
        // 调用方无法映射回输入——违反 api.h 的 *index = 输入点序 契约。
        // 修复：反转照做，返回索引统一映射回输入点序（count-1-idx）。
        const double signedArea = ImageProcessor::contourArea(pts, true);
        bool reversedOrder = false;
        if (signedArea < 0.0) {
            std::reverse(pts.begin(), pts.end());
            reversedOrder = true;
        }
        std::vector<int> hull;
        cv::convexHull(pts, hull, false);
        // 退化输入（全部共线等）凸包可能少于 3 点：convexityDefects 会断言，
        // 这里按"无凹陷"提前返回 0/NULL/0（与凸形无检出口径一致），不透出 -99。
        if (hull.size() < 3) return 0;
        std::vector<cv::Vec4i> defects;
        ImageProcessor::convexityDefects(pts, hull, defects);
        if (defects.empty()) return 0;

        Image2DAlg_Defect* arr = (Image2DAlg_Defect*)malloc(sizeof(Image2DAlg_Defect) * defects.size());
        if (!arr) return -4;
        for (size_t i = 0; i < defects.size(); ++i) {
            Image2DAlg_Defect& d = arr[i];
            // 索引映射回输入点序：反转数组的 idx 对应原输入的 count-1-idx
            d.start_index = reversedOrder ? count - 1 - defects[i][0] : defects[i][0];
            d.end_index = reversedOrder ? count - 1 - defects[i][1] : defects[i][1];
            d.farthest_index = reversedOrder ? count - 1 - defects[i][2] : defects[i][2];
            d.depth = defects[i][3] / 256.0;   // OpenCV 定点数：depth*256
            d.fx = pts[defects[i][2]].x;
            d.fy = pts[defects[i][2]].y;
        }
        *out_defects = arr;
        *out_count = (int)defects.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_MatchTemplateMulti(const Image2DAlg_Image* image, const Image2DAlg_Image* templ,
                                  int method, double scale_lo, double scale_hi, double scale_step,
                                  double* out_score, int* out_x, int* out_y, double* out_scale) {
    cv::Mat img, tpl;
    if (!out_score || !out_x || !out_y || !out_scale) return -1;
    if (int rc = Wrap(image, img)) return rc;
    if (int rc = Wrap(templ, tpl)) return rc;
    if (method < 0 || method > 5) return -3;
    if (img.channels() != tpl.channels()) return -3;
    if (scale_step <= 0.0 || scale_lo <= 0.0 || scale_lo > scale_hi) return -3;
    try {
        const bool isSqdiff = (method == 0 || method == 1);   // TM_SQDIFF / TM_SQDIFF_NORMED
        const int n = (int)((scale_hi - scale_lo) / scale_step + 1e-9) + 1;
        bool found = false;
        double bestScore = 0.0;
        int bestX = 0, bestY = 0;
        double bestScale = 0.0;
        for (int i = 0; i < n; ++i) {
            const double s = scale_lo + i * scale_step;
            const int tw = (int)std::lround(tpl.cols * s);
            const int th = (int)std::lround(tpl.rows * s);
            if (tw < 1 || th < 1 || tw > img.cols || th > img.rows)
                continue;   // 该尺度下模板无效或越界，跳过
            cv::Mat scaled;
            ImageProcessor::resize(tpl, scaled, cv::Size(tw, th), 0, 0, 1 /*LINEAR*/);
            cv::Mat result;
            ImageProcessor::matchTemplate(img, scaled, result, method);
            cv::Point loc;
            double score = 0.0;
            ImageProcessor::findBestMatch(result, method, loc, score);
            const bool better = !found || (isSqdiff ? score < bestScore : score > bestScore);
            if (better) {
                found = true;
                bestScore = score;
                bestX = loc.x;
                bestY = loc.y;
                bestScale = s;
            }
        }
        if (!found) return -3;   // 任何尺度下模板都放不进图像
        *out_score = bestScore;
        *out_x = bestX;
        *out_y = bestY;
        *out_scale = bestScale;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_MatchTemplateMasked(const Image2DAlg_Image* image, const Image2DAlg_Image* templ,
                                   const Image2DAlg_Image* mask, int method,
                                   double* out_score, int* out_x, int* out_y) {
    cv::Mat img, tpl, mk;
    if (!out_score || !out_x || !out_y) return -1;
    if (int rc = Wrap(image, img)) return rc;
    if (int rc = Wrap(templ, tpl)) return rc;
    if (int rc = Wrap(mask, mk)) return rc;
    if (method != 0 && method != 3) return -3;   // OpenCV 4.4 掩膜仅支持 SQDIFF/CCORR_NORMED
    if (mk.channels() != 1) return -3;
    if (img.channels() != tpl.channels() || tpl.size() != mk.size()) return -3;
    if (tpl.cols > img.cols || tpl.rows > img.rows) return -3;
    try {
        cv::Mat result;
        ImageProcessor::matchTemplateMasked(img, tpl, mk, result, method);
        if (result.empty()) return -3;
        cv::Point loc;
        ImageProcessor::findBestMatch(result, method, loc, *out_score);
        *out_x = loc.x;
        *out_y = loc.y;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_MatchTemplateRotated(const Image2DAlg_Image* image, const Image2DAlg_Image* templ,
                                    double angle_start, double angle_end, double angle_step,
                                    int method, double* out_score, double* out_cx,
                                    double* out_cy, double* out_angle) {
    cv::Mat img, tpl;
    if (!out_score || !out_cx || !out_cy || !out_angle) return -1;
    if (int rc = Wrap(image, img)) return rc;
    if (int rc = Wrap(templ, tpl)) return rc;
    if (method < 0 || method > 5) return -3;
    if (img.channels() != tpl.channels()) return -3;
    if (angle_step <= 0 || angle_end < angle_start) return -3;
    try {
        shapely::RotatedMatch2d m =
            ImageProcessor::findRotatedMatch(img, tpl, angle_start, angle_end, angle_step, method);
        *out_cx = m.loc.x;
        *out_cy = m.loc.y;
        *out_angle = m.angle;
        *out_score = m.score;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

// 多目标匹配核心：img 可为 ROI 视图，ox/oy 为命中位置补偿
int RunFindAllMatches(const cv::Mat& img, const cv::Mat& tpl, int method,
                      double score_threshold, int max_matches, int nms_radius,
                      Image2DAlg_Match** out_matches, int* out_count, int ox, int oy) {
    *out_matches = nullptr;
    *out_count = 0;
    cv::Mat result;
    ImageProcessor::matchTemplate(img, tpl, result, method);
    std::vector<shapely::Match2d> matches;
    ImageProcessor::findAllMatches(result, method, score_threshold, max_matches,
                                   nms_radius, matches);
    if (matches.empty()) return 0;
    Image2DAlg_Match* arr = (Image2DAlg_Match*)malloc(sizeof(Image2DAlg_Match) * matches.size());
    if (!arr) return -4;
    for (size_t i = 0; i < matches.size(); ++i) {
        arr[i].x = matches[i].loc.x + ox;
        arr[i].y = matches[i].loc.y + oy;
        arr[i].score = matches[i].score;
    }
    *out_matches = arr;
    *out_count = (int)matches.size();
    return 0;
}

int Image2DAlg_FindAllMatches(const Image2DAlg_Image* image, const Image2DAlg_Image* templ,
                              int method, double score_threshold, int max_matches, int nms_radius,
                              Image2DAlg_Match** out_matches, int* out_count) {
    cv::Mat img, tpl;
    if (!out_matches || !out_count) return -1;
    if (int rc = Wrap(image, img)) return rc;
    if (int rc = Wrap(templ, tpl)) return rc;
    if (method < 0 || method > 5) return -3;
    if (img.channels() != tpl.channels() || tpl.cols > img.cols || tpl.rows > img.rows) return -3;
    if (max_matches < 1 || nms_radius < 0) return -3;
    try {
        return RunFindAllMatches(img, tpl, method, score_threshold, max_matches, nms_radius,
                                 out_matches, out_count, 0, 0);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FindAllMatchesROI(const Image2DAlg_Image* image,
                                 int roi_x, int roi_y, int roi_w, int roi_h,
                                 const Image2DAlg_Image* templ, int method,
                                 double score_threshold, int max_matches, int nms_radius,
                                 Image2DAlg_Match** out_matches, int* out_count) {
    cv::Mat img, tpl;
    if (!out_matches || !out_count) return -1;
    if (int rc = Wrap(image, img)) return rc;
    if (int rc = Wrap(templ, tpl)) return rc;
    if (method < 0 || method > 5) return -3;
    if (img.channels() != tpl.channels()) return -3;
    if (max_matches < 1 || nms_radius < 0) return -3;
    try {
        const cv::Rect roi = ClampRoi(img, roi_x, roi_y, roi_w, roi_h);
        if (roi.empty()) return -3;              // ROI 与图像无交集
        if (tpl.cols > roi.width || tpl.rows > roi.height) return -3;   // 模板大于 ROI
        return RunFindAllMatches(img(roi), tpl, method, score_threshold, max_matches,
                                 nms_radius, out_matches, out_count, roi.x, roi.y);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_MatchTemplateEx(const Image2DAlg_Image* image, const Image2DAlg_Image* templ,
                               int method, double scale_lo, double scale_hi, double scale_step,
                               double angle_lo, double angle_hi, double angle_step,
                               double* out_score, double* out_cx, double* out_cy,
                               double* out_scale, double* out_angle) {
    cv::Mat img, tpl;
    if (!out_score || !out_cx || !out_cy || !out_scale || !out_angle) return -1;
    if (int rc = Wrap(image, img)) return rc;
    if (int rc = Wrap(templ, tpl)) return rc;
    if (method < 0 || method > 5) return -3;
    if (img.channels() != tpl.channels()) return -3;
    if (scale_step <= 0 || scale_lo <= 0 || scale_lo > scale_hi) return -3;
    if (angle_step <= 0 || angle_hi < angle_lo) return -3;
    try {
        const bool isSqdiff = (method == 0 || method == 1);
        const int ns = (int)((scale_hi - scale_lo) / scale_step + 1e-9) + 1;
        bool found = false;
        double bestScore = 0.0, bestScale = 0.0;
        shapely::RotatedMatch2d best;
        for (int i = 0; i < ns; ++i) {
            const double s = scale_lo + i * scale_step;
            const int tw = (int)std::lround(tpl.cols * s);
            const int th = (int)std::lround(tpl.rows * s);
            if (tw < 1 || th < 1 || tw > img.cols || th > img.rows)
                continue;   // 该尺度下模板无效或越界，跳过
            cv::Mat scaled;
            ImageProcessor::resize(tpl, scaled, cv::Size(tw, th), 0, 0, 1);
            shapely::RotatedMatch2d m =
                ImageProcessor::findRotatedMatch(img, scaled, angle_lo, angle_hi, angle_step, method);
            const bool better = !found || (isSqdiff ? m.score < bestScore : m.score > bestScore);
            if (better) {
                found = true;
                bestScore = m.score;
                best = m;
                bestScale = s;
            }
        }
        if (!found) return -3;
        *out_score = bestScore;
        *out_cx = best.loc.x;
        *out_cy = best.loc.y;
        *out_scale = bestScale;
        *out_angle = best.angle;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_MatchTemplatePyramid(const Image2DAlg_Image* image, const Image2DAlg_Image* templ,
                                    int method, double scale_lo, double scale_hi,
                                    double scale_step, double angle_lo, double angle_hi,
                                    double angle_step, int levels, double* out_score,
                                    double* out_cx, double* out_cy, double* out_scale,
                                    double* out_angle) {
    cv::Mat img, tpl;
    if (!out_score || !out_cx || !out_cy || !out_scale || !out_angle) return -1;
    if (int rc = Wrap(image, img)) return rc;
    if (int rc = Wrap(templ, tpl)) return rc;
    if (method < 0 || method > 5) return -3;
    if (img.channels() != tpl.channels()) return -3;
    if (scale_step <= 0 || scale_lo <= 0 || scale_lo > scale_hi) return -3;
    if (angle_step <= 0 || angle_hi < angle_lo) return -3;
    if (levels < 1) return -3;
    if (levels > 6) levels = 6;   // 防呆上限（文档注明自动截断）
    try {
        const bool isSqdiff = (method == 0 || method == 1);
        // 金字塔构建：层数受"模板最小边 >= 12 / 图像最小边 >= 32"约束自动截断
        //（粗层模板过小会丧失角度判别力，粗搜选错角度后精化窗口将错过真值）
        std::vector<cv::Mat> ip{img}, tp{tpl};
        for (int i = 1; i < levels; ++i) {
            if (std::min(ip.back().cols, ip.back().rows) < 32) break;
            if (std::min(tp.back().cols, tp.back().rows) < 12) break;
            cv::Mat d;
            ImageProcessor::pyrDown(ip.back(), d);
            ip.push_back(d);
            ImageProcessor::pyrDown(tp.back(), d);
            tp.push_back(d);
        }
        const int top = (int)ip.size() - 1;

        bool found = false;
        double bestScore = 0.0, bestScale = 0.0, bestAngle = 0.0;
        cv::Point2f bestLoc(0.0f, 0.0f);
        int bestLevel = 0;
        // 顶层全范围粗搜 → 逐级回原图，仅在粗解 ±1 步进邻域内精化
        for (int l = top; l >= 0; --l) {
            const double s_lo = (l == top) ? scale_lo
                                           : std::max(scale_lo, bestScale - scale_step);
            const double s_hi = (l == top) ? scale_hi
                                           : std::min(scale_hi, bestScale + scale_step);
            const double a_lo = (l == top) ? angle_lo
                                           : std::max(angle_lo, bestAngle - angle_step);
            const double a_hi = (l == top) ? angle_hi
                                           : std::min(angle_hi, bestAngle + angle_step);
            const int ns = (int)((s_hi - s_lo) / scale_step + 1e-9) + 1;
            for (int i = 0; i < ns; ++i) {
                const double s = s_lo + i * scale_step;
                const int tw = (int)std::lround(tp[l].cols * s);
                const int th = (int)std::lround(tp[l].rows * s);
                if (tw < 1 || th < 1 || tw > ip[l].cols || th > ip[l].rows) continue;
                cv::Mat scaled;
                ImageProcessor::resize(tp[l], scaled, cv::Size(tw, th), 0, 0, 1);
                shapely::RotatedMatch2d m =
                    ImageProcessor::findRotatedMatch(ip[l], scaled, a_lo, a_hi, angle_step,
                                                     method);
                const bool better =
                    !found || (isSqdiff ? m.score < bestScore : m.score > bestScore);
                if (better) {
                    found = true;
                    bestScore = m.score;
                    bestLoc = m.loc;
                    bestScale = s;
                    bestAngle = m.angle;
                    bestLevel = l;
                }
            }
        }
        if (!found) return -3;
        // 命中层数的坐标映射回原图（金字塔逐级减半：level0 = level_l * 2^l）
        *out_score = bestScore;
        *out_cx = bestLoc.x * (1 << bestLevel);
        *out_cy = bestLoc.y * (1 << bestLevel);
        *out_scale = bestScale;
        *out_angle = bestAngle;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

void Image2DAlg_FreeContours(Image2DAlg_ContourInfo* contours) {
    free(contours);
}

void Image2DAlg_FreeDefects(Image2DAlg_Defect* defects) {
    free(defects);
}

void Image2DAlg_FreeCircles(Image2DAlg_CircleFit* circles) {
    free(circles);
}

int Image2DAlg_MatchTemplateSubPix(const Image2DAlg_Image* image,
                                   const Image2DAlg_Image* templ, int method,
                                   double* out_score, double* out_x, double* out_y) {
    cv::Mat img, tpl;
    if (!out_score || !out_x || !out_y) return -1;
    if (int rc = Wrap(image, img)) return rc;
    if (int rc = Wrap(templ, tpl)) return rc;
    if (method < 0 || method > 5) return -3;
    if (img.channels() != tpl.channels() || tpl.cols > img.cols || tpl.rows > img.rows) return -3;
    try {
        cv::Mat result;
        ImageProcessor::matchTemplate(img, tpl, result, method);
        cv::Point2f loc;
        ImageProcessor::findBestMatchSubPix(result, method, loc, *out_score);
        *out_x = loc.x;
        *out_y = loc.y;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FindAllMatchesEx(const Image2DAlg_Image* image, const Image2DAlg_Image* templ,
                                int method, double score_threshold, int max_matches,
                                int nms_radius, double scale_lo, double scale_hi,
                                double scale_step, double angle_lo, double angle_hi,
                                double angle_step, Image2DAlg_MatchEx** out_matches,
                                int* out_count) {
    cv::Mat img, tpl;
    if (!out_matches || !out_count) return -1;
    if (int rc = Wrap(image, img)) return rc;
    if (int rc = Wrap(templ, tpl)) return rc;
    if (method < 0 || method > 5) return -3;
    if (img.channels() != tpl.channels()) return -3;
    if (max_matches < 1 || nms_radius < 0) return -3;
    if (scale_step <= 0 || scale_lo <= 0 || scale_lo > scale_hi) return -3;
    if (angle_step <= 0 || angle_hi < angle_lo) return -3;
    *out_matches = nullptr;
    *out_count = 0;
    try {
        const bool isSqdiff = (method == 0 || method == 1);
        const int ns = (int)((scale_hi - scale_lo) / scale_step + 1e-9) + 1;
        const int na = (int)((angle_hi - angle_lo) / angle_step + 1e-9) + 1;

        // 每个 (scale, angle) 组合做多目标提取，候选统一记到模板中心坐标
        struct Cand { double cx, cy, score, scale, angle; };
        std::vector<Cand> cands;
        for (int i = 0; i < ns; ++i) {
            const double s = scale_lo + i * scale_step;
            const int tw = (int)std::lround(tpl.cols * s);
            const int th = (int)std::lround(tpl.rows * s);
            if (tw < 1 || th < 1) continue;
            for (int k = 0; k < na; ++k) {
                const double a = angle_lo + k * angle_step;
                // 旋转画布外扩（覆盖任意角度的模板四角）
                const double rad = a * kPi / 180.0;
                const int cw = (int)std::ceil(std::fabs(tw * std::cos(rad)) +
                                              std::fabs(th * std::sin(rad)));
                const int ch = (int)std::ceil(std::fabs(tw * std::sin(rad)) +
                                              std::fabs(th * std::cos(rad)));
                if (cw < 1 || ch < 1 || cw > img.cols || ch > img.rows)
                    continue;   // 该组合下旋转画布越界，跳过
                cv::Mat scaled;   // 先缩放模板到该尺度（缺失会导致黑画布匹配失效）
                ImageProcessor::resize(tpl, scaled, cv::Size(tw, th), 0, 0, 1);
                cv::Mat m = cv::getRotationMatrix2D(cv::Point2f(tw / 2.0f, th / 2.0f), a, 1.0);
                m.at<double>(0, 2) += cw / 2.0 - tw / 2.0;   // 旋转内容平移到画布中心
                m.at<double>(1, 2) += ch / 2.0 - th / 2.0;
                cv::Mat rot;
                cv::warpAffine(scaled, rot, m, cv::Size(cw, ch), 1, cv::BORDER_CONSTANT,
                               cv::Scalar());
                cv::Mat result;
                ImageProcessor::matchTemplate(img, rot, result, method);
                std::vector<shapely::Match2d> ms;
                ImageProcessor::findAllMatches(result, method, score_threshold, max_matches,
                                               nms_radius, ms);
                for (const auto& it : ms) {
                    cands.push_back({it.loc.x + cw / 2.0, it.loc.y + ch / 2.0,
                                     it.score, s, a});
                }
            }
        }
        if (cands.empty()) return 0;

        // 跨组合全局 NMS（中心距离）：先按得分从优到劣，贪心保留
        std::sort(cands.begin(), cands.end(), [isSqdiff](const Cand& a, const Cand& b) {
            return isSqdiff ? a.score < b.score : a.score > b.score;
        });
        std::vector<Cand> accepted;
        for (const Cand& c : cands) {
            if ((int)accepted.size() >= max_matches) break;
            bool dup = false;
            for (const Cand& a : accepted) {
                if (std::hypot(c.cx - a.cx, c.cy - a.cy) <= nms_radius) { dup = true; break; }
            }
            if (!dup) accepted.push_back(c);
        }
        if (accepted.empty()) return 0;

        Image2DAlg_MatchEx* arr =
            (Image2DAlg_MatchEx*)malloc(sizeof(Image2DAlg_MatchEx) * accepted.size());
        if (!arr) return -4;
        for (size_t i = 0; i < accepted.size(); ++i) {
            arr[i].cx = accepted[i].cx;
            arr[i].cy = accepted[i].cy;
            arr[i].score = accepted[i].score;
            arr[i].scale = accepted[i].scale;
            arr[i].angle = accepted[i].angle;
        }
        *out_matches = arr;
        *out_count = (int)accepted.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

void Image2DAlg_FreeMatchesEx(Image2DAlg_MatchEx* matches) {
    free(matches);
}

void Image2DAlg_FreeMatches(Image2DAlg_Match* matches) {
    free(matches);
}

void Image2DAlg_FreeInliers(int* inliers) {
    free(inliers);
}

// 霍夫直线核心：src 可为 ROI 视图，ox/oy 为结果坐标补偿
int RunHoughLines(const cv::Mat& src, double rho, double theta_deg, int threshold,
                  int min_line_length, int max_line_gap,
                  Image2DAlg_Line4** out_lines, int* out_count, int ox, int oy) {
    *out_lines = nullptr;
    *out_count = 0;
    std::vector<cv::Vec4i> lines;
    ImageProcessor::houghLinesP(GrayOf(src), lines, rho,
                                theta_deg * kPi / 180.0, threshold,
                                min_line_length > 0 ? (double)min_line_length : 0.0,
                                max_line_gap > 0 ? (double)max_line_gap : 0.0);
    if (lines.empty()) return 0;
    Image2DAlg_Line4* arr = (Image2DAlg_Line4*)malloc(sizeof(Image2DAlg_Line4) * lines.size());
    if (!arr) return -4;
    for (size_t i = 0; i < lines.size(); ++i) {
        arr[i].x1 = lines[i][0] + ox;
        arr[i].y1 = lines[i][1] + oy;
        arr[i].x2 = lines[i][2] + ox;
        arr[i].y2 = lines[i][3] + oy;
    }
    *out_lines = arr;
    *out_count = (int)lines.size();
    return 0;
}

int Image2DAlg_HoughLinesP(const Image2DAlg_Image* in,
                           double rho, double theta_deg, int threshold,
                           int min_line_length, int max_line_gap,
                           Image2DAlg_Line4** out_lines, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_lines || !out_count) return -1;
    if (rho <= 0 || theta_deg <= 0) return -3;
    try {
        return RunHoughLines(src, rho, theta_deg, threshold, min_line_length, max_line_gap,
                             out_lines, out_count, 0, 0);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_HoughLinesPROI(const Image2DAlg_Image* in,
                              int roi_x, int roi_y, int roi_w, int roi_h,
                              double rho, double theta_deg, int threshold,
                              int min_line_length, int max_line_gap,
                              Image2DAlg_Line4** out_lines, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_lines || !out_count) return -1;
    if (rho <= 0 || theta_deg <= 0) return -3;
    try {
        const cv::Rect roi = ClampRoi(src, roi_x, roi_y, roi_w, roi_h);
        if (roi.empty()) return -3;   // ROI 与图像无交集
        return RunHoughLines(src(roi), rho, theta_deg, threshold, min_line_length, max_line_gap,
                             out_lines, out_count, roi.x, roi.y);
    } catch (const cv::Exception&) {
        return -99;
    }
}

// 霍夫圆核心：src 可为 ROI 视图，ox/oy 为结果坐标补偿
int RunHoughCircles(const cv::Mat& src, double dp, double min_dist, double param1,
                    double param2, double min_r, double max_r,
                    Image2DAlg_HoughCircle** out_circles, int* out_count, int ox, int oy) {
    *out_circles = nullptr;
    *out_count = 0;
    std::vector<cv::Vec3f> circles;
    ImageProcessor::houghCircles(GrayOf(src), circles, dp, min_dist, param1, param2,
                                 (float)min_r, max_r > 0 ? (float)max_r : 0.0f);
    if (circles.empty()) return 0;
    Image2DAlg_HoughCircle* arr =
        (Image2DAlg_HoughCircle*)malloc(sizeof(Image2DAlg_HoughCircle) * circles.size());
    if (!arr) return -4;
    for (size_t i = 0; i < circles.size(); ++i) {
        arr[i].cx = circles[i][0] + ox;
        arr[i].cy = circles[i][1] + oy;
        arr[i].r = circles[i][2];
    }
    *out_circles = arr;
    *out_count = (int)circles.size();
    return 0;
}

int Image2DAlg_HoughCircles(const Image2DAlg_Image* in,
                            double dp, double min_dist, double param1, double param2,
                            double min_r, double max_r,
                            Image2DAlg_HoughCircle** out_circles, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_circles || !out_count) return -1;
    if (dp <= 0 || min_dist <= 0) return -3;
    try {
        return RunHoughCircles(src, dp, min_dist, param1, param2, min_r, max_r,
                               out_circles, out_count, 0, 0);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_HoughCirclesROI(const Image2DAlg_Image* in,
                               int roi_x, int roi_y, int roi_w, int roi_h,
                               double dp, double min_dist, double param1, double param2,
                               double min_r, double max_r,
                               Image2DAlg_HoughCircle** out_circles, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_circles || !out_count) return -1;
    if (dp <= 0 || min_dist <= 0) return -3;
    try {
        const cv::Rect roi = ClampRoi(src, roi_x, roi_y, roi_w, roi_h);
        if (roi.empty()) return -3;   // ROI 与图像无交集
        return RunHoughCircles(src(roi), dp, min_dist, param1, param2, min_r, max_r,
                               out_circles, out_count, roi.x, roi.y);
    } catch (const cv::Exception&) {
        return -99;
    }
}

void Image2DAlg_FreeLines(Image2DAlg_Line4* lines) {
    free(lines);
}

void Image2DAlg_FreeCirclesHough(Image2DAlg_HoughCircle* circles) {
    free(circles);
}

int Image2DAlg_RegionStats(const Image2DAlg_Image* in,
                           int roi_x, int roi_y, int roi_w, int roi_h,
                           Image2DAlg_GrayStats* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    try {
        const cv::Rect roi = ClampRoi(src, roi_x, roi_y, roi_w, roi_h);
        if (roi.empty()) return -3;   // ROI 与图像无交集
        cv::Mat gray = GrayOf(src)(roi);
        cv::Scalar mean, stddev;
        ImageProcessor::meanStdDev(gray, mean, stddev);
        double lo = 0, hi = 0;
        cv::Point loLoc, hiLoc;
        ImageProcessor::minMaxLoc(gray, lo, hi, loLoc, hiLoc);
        out->mean = mean[0];
        out->stddev = stddev[0];
        out->min = (int)lo;
        out->max = (int)hi;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Histogram(const Image2DAlg_Image* in, int out_hist[256]) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_hist) return -1;
    try {
        std::vector<float> hist;
        ImageProcessor::calcHist8U(GrayOf(src), hist);
        for (int i = 0; i < 256; ++i) out_hist[i] = (int)hist[i];
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_CompareHist(const int* hist1, const int* hist2, int method, double* out_score) {
    if (!hist1 || !hist2 || !out_score) return -1;
    if (method < 0 || method > 3) return -3;   // cv::HISTCMP_* 枚举
    try {
        std::vector<float> h1(hist1, hist1 + 256), h2(hist2, hist2 + 256);
        *out_score = ImageProcessor::compareHist(h1, h2, method);
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_ContourPoints(const Image2DAlg_Image* in,
                             int external_only, int min_area,
                             float** out_xy, int* out_points,
                             int** out_offsets, int* out_contours) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_xy || !out_points || !out_offsets || !out_contours) return -1;
    *out_xy = nullptr;
    *out_points = 0;
    *out_offsets = nullptr;
    *out_contours = 0;
    try {
        std::vector<std::vector<cv::Point>> contours;
        ImageProcessor::findContours(Binarize(src), contours,
                                     external_only ? cv::RETR_EXTERNAL : cv::RETR_LIST,
                                     cv::CHAIN_APPROX_SIMPLE);

        int total = 0;
        std::vector<std::vector<cv::Point>> kept;
        for (const auto& c : contours) {
            if (c.empty()) continue;
            if (ImageProcessor::contourArea(c) < min_area) continue;
            kept.push_back(c);
            total += (int)c.size();
        }
        if (kept.empty()) return 0;

        float* xy = (float*)malloc(sizeof(float) * total * 2);
        int* offs = (int*)malloc(sizeof(int) * kept.size());
        if (!xy || !offs) {
            free(xy);
            free(offs);
            return -4;
        }
        int pt = 0;
        for (size_t k = 0; k < kept.size(); ++k) {
            offs[k] = pt;
            for (const cv::Point& p : kept[k]) {
                xy[pt * 2] = (float)p.x;
                xy[pt * 2 + 1] = (float)p.y;
                ++pt;
            }
        }
        *out_xy = xy;
        *out_points = total;
        *out_offsets = offs;
        *out_contours = (int)kept.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

void Image2DAlg_FreeContourPoints(float* xy, int* offsets) {
    free(xy);
    free(offsets);
}

int Image2DAlg_HSVThreshold(const Image2DAlg_Image* in, double h_lo, double h_hi,
                            double s_lo, double s_hi, double v_lo, double v_hi,
                            Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    if (src.channels() == 1) return -3;   // 灰度图无 HSV 语义
    try {
        cv::Mat hsv;
        if (src.channels() == 4) {   // OpenCV 无 BGRA2HSV：先去 alpha
            cv::Mat bgr;
            ImageProcessor::cvtColor(src, bgr, cv::COLOR_BGRA2BGR);
            ImageProcessor::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
        } else {
            ImageProcessor::cvtColor(src, hsv, cv::COLOR_BGR2HSV);
        }
        cv::Mat mask;
        cv::inRange(hsv, cv::Scalar(h_lo, s_lo, v_lo), cv::Scalar(h_hi, s_hi, v_hi), mask);
        return CopyOut(mask, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_AbsDiff(const Image2DAlg_Image* in1, const Image2DAlg_Image* in2,
                       Image2DAlg_Image* out) {
    cv::Mat a, b;
    if (int rc = Wrap(in1, a)) return rc;
    if (int rc = Wrap(in2, b)) return rc;
    if (!out) return -1;
    if (a.size() != b.size() || a.type() != b.type()) return -3;
    try {
        cv::Mat dst;
        ImageProcessor::absdiff(a, b, dst);
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FitCircle(const float* xy, int count, Image2DAlg_CircleFit* out) {
    if (!xy || !out) return -1;
    if (count < 3) return -3;
    try {
        // Kåsa 代数法：min Σ(x²+y²+a·x+b·y+c)²，圆心=(-a/2,-b/2)，r²=a²/4+b²/4-c
        cv::Mat A(count, 3, CV_64F), b(count, 1, CV_64F);
        for (int i = 0; i < count; ++i) {
            const double x = xy[i * 2], y = xy[i * 2 + 1];
            A.at<double>(i, 0) = x;
            A.at<double>(i, 1) = y;
            A.at<double>(i, 2) = 1.0;
            b.at<double>(i, 0) = -(x * x + y * y);
        }
        cv::Mat sol;
        if (cv::solve(A, b, sol, cv::DECOMP_SVD) == 0) return -3;
        const double a = sol.at<double>(0), b2 = sol.at<double>(1), c = sol.at<double>(2);
        const double r2 = a * a / 4.0 + b2 * b2 / 4.0 - c;
        if (r2 <= 0.0) return -3;   // 重合点等退化输入
        out->cx = -a / 2.0;
        out->cy = -b2 / 2.0;
        out->r = std::sqrt(r2);
        double sum = 0.0;
        for (int i = 0; i < count; ++i) {
            const double dx = xy[i * 2] - out->cx, dy = xy[i * 2 + 1] - out->cy;
            const double d = std::sqrt(dx * dx + dy * dy) - out->r;
            sum += d * d;
        }
        out->rms = std::sqrt(sum / count);
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FitLine(const float* xy, int count, Image2DAlg_LineFit* out) {
    if (!xy || !out) return -1;
    if (count < 2) return -3;
    try {
        std::vector<cv::Point> pts;
        pts.reserve(count);
        for (int i = 0; i < count; ++i)   // ImageProcessor::fitLine 接口为整数点（像素坐标）
            pts.push_back(cv::Point((int)std::lround(xy[i * 2]),
                                    (int)std::lround(xy[i * 2 + 1])));
        std::vector<float> line;
        ImageProcessor::fitLine(pts, line, cv::DIST_L2);
        out->vx = line[0];
        out->vy = line[1];
        out->x0 = line[2];
        out->y0 = line[3];
        double sum = 0.0;   // 点到直线距离（叉积模）RMS
        for (int i = 0; i < count; ++i) {
            const double dx = xy[i * 2] - out->x0, dy = xy[i * 2 + 1] - out->y0;
            const double cross = dx * out->vy - dy * out->vx;
            sum += cross * cross;
        }
        out->rms = std::sqrt(sum / count);
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FitCircleEx(const float* xy, int count, Image2DAlg_CircleFit* out) {
    if (!xy || !out) return -1;
    if (count < 3) return -3;
    try {
        std::vector<cv::Point2f> pts;
        pts.reserve(count);
        for (int i = 0; i < count; ++i)
            pts.push_back(cv::Point2f(xy[i * 2], xy[i * 2 + 1]));
        cv::Point2f c;
        double r = 0, rms = 0;
        if (!ImageProcessor::fitCircle(pts, c, r, rms)) return -3;   // 共线/近共线
        out->cx = c.x;
        out->cy = c.y;
        out->r = r;
        out->rms = rms;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FitCircleRobust(const float* xy, int count, double threshold, int max_iters,
                               unsigned int seed, Image2DAlg_CircleFit* out,
                               int** out_inliers, int* out_inlier_count) {
    if (!xy || !out || !out_inlier_count) return -1;   // out_inliers 可为 NULL 不接收
    if (count < 3) return -3;
    if (threshold <= 0) return -3;
    if (out_inliers) *out_inliers = nullptr;
    *out_inlier_count = 0;
    try {
        std::vector<cv::Point2f> pts;
        pts.reserve(count);
        for (int i = 0; i < count; ++i)
            pts.push_back(cv::Point2f(xy[i * 2], xy[i * 2 + 1]));
        cv::Point2f c;
        double r = 0, rms = 0;
        std::vector<int> inl;
        if (!ImageProcessor::fitCircleRobust(pts, threshold, c, r, rms, inl, seed,
                                             max_iters < 1 ? 1000 : max_iters))
            return -3;   // 无有效圆（内点 < 3）
        out->cx = c.x;
        out->cy = c.y;
        out->r = r;
        out->rms = rms;
        if (out_inliers && !inl.empty()) {
            int* arr = (int*)malloc(sizeof(int) * inl.size());
            if (!arr) return -4;
            memcpy(arr, inl.data(), sizeof(int) * inl.size());
            *out_inliers = arr;
        }
        *out_inlier_count = (int)inl.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FitEllipse(const float* xy, int count, Image2DAlg_EllipseFit* out) {
    if (!xy || !out) return -1;
    if (count < 5) return -3;
    try {
        std::vector<cv::Point> pts;
        pts.reserve(count);
        for (int i = 0; i < count; ++i)   // fitEllipse 接口为整数点（像素坐标）
            pts.push_back(cv::Point((int)std::lround(xy[i * 2]),
                                    (int)std::lround(xy[i * 2 + 1])));
        cv::RotatedRect e = ImageProcessor::fitEllipse(pts);
        if (e.size.width <= 0 || e.size.height <= 0) return -3;
        out->cx = e.center.x;
        out->cy = e.center.y;
        out->width = e.size.width;
        out->height = e.size.height;
        out->angle = e.angle;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FitEllipsesFromBinary(const Image2DAlg_Image* in,
                                     int min_area, int max_area,
                                     double min_axis, double max_axis,
                                     Image2DAlg_EllipseFit** out_ellipses, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_ellipses || !out_count) return -1;
    *out_ellipses = nullptr;
    *out_count = 0;
    try {
        cv::Mat bin = Binarize(src);
        std::vector<std::vector<cv::Point>> contours;
        ImageProcessor::findContours(bin, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);

        std::vector<Image2DAlg_EllipseFit> result;
        for (const auto& c : contours) {
            const double area = ImageProcessor::contourArea(c);
            if (area < min_area) continue;
            if (max_area > 0 && area > max_area) continue;
            if ((int)c.size() < 5) continue;   // fitEllipse 最少 5 点，不足静默跳过

            cv::RotatedRect e = ImageProcessor::fitEllipse(c);
            if (e.size.width <= 0 || e.size.height <= 0) continue;
            const double major = std::max(e.size.width, e.size.height);
            if (min_axis > 0 && major < min_axis) continue;
            if (max_axis > 0 && major > max_axis) continue;

            Image2DAlg_EllipseFit fit;
            fit.cx = e.center.x;
            fit.cy = e.center.y;
            fit.width = e.size.width;
            fit.height = e.size.height;
            fit.angle = e.angle;
            result.push_back(fit);
        }

        if (result.empty()) return 0;
        size_t bytes = sizeof(Image2DAlg_EllipseFit) * result.size();
        Image2DAlg_EllipseFit* arr = (Image2DAlg_EllipseFit*)malloc(bytes);
        if (!arr) return -4;
        memcpy(arr, result.data(), bytes);
        *out_ellipses = arr;
        *out_count = (int)result.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

void Image2DAlg_FreeEllipses(Image2DAlg_EllipseFit* ellipses) {
    free(ellipses);
}

int Image2DAlg_FitRect(const float* xy, int count, Image2DAlg_RectFit* out) {
    if (!xy || !out) return -1;
    if (count < 3) return -3;
    try {
        std::vector<cv::Point> pts;
        pts.reserve(count);
        for (int i = 0; i < count; ++i)
            pts.push_back(cv::Point((int)std::lround(xy[i * 2]),
                                    (int)std::lround(xy[i * 2 + 1])));
        cv::RotatedRect r = ImageProcessor::getMinAreaRect(pts);
        if (r.size.width <= 0 || r.size.height <= 0) return -3;   // 退化输入
        out->cx = r.center.x;
        out->cy = r.center.y;
        out->width = r.size.width;
        out->height = r.size.height;
        out->angle = r.angle;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FitPoly(const float* xy, int count, int degree, Image2DAlg_PolyFit* out) {
    if (!xy || !out) return -1;
    if (degree < 1 || degree > 5) return -3;
    if (count < degree + 1) return -3;
    try {
        cv::Mat A(count, degree + 1, CV_64F), b(count, 1, CV_64F);
        for (int i = 0; i < count; ++i) {
            const double x = xy[i * 2], y = xy[i * 2 + 1];
            double xp = 1.0;
            for (int j = 0; j <= degree; ++j) {
                A.at<double>(i, j) = xp;
                xp *= x;
            }
            b.at<double>(i, 0) = y;
        }
        cv::Mat sol;
        if (cv::solve(A, b, sol, cv::DECOMP_SVD) == 0) return -3;
        std::memset(out, 0, sizeof(*out));
        out->degree = degree;
        double sum = 0.0;
        for (int i = 0; i < count; ++i) {
            const double x = xy[i * 2], y = xy[i * 2 + 1];
            double yfit = 0.0, xp = 1.0;
            for (int j = 0; j <= degree; ++j) {
                out->coef[j] = sol.at<double>(j);
                yfit += sol.at<double>(j) * xp;
                xp *= x;
            }
            const double d = yfit - y;
            sum += d * d;
        }
        out->rms = std::sqrt(sum / count);
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}



// ---------------- 特征点查找（cv 直接调用：ImageProcessor 未封装 cornerHarris/ORB） ----------------

// 亚像素细化公共步骤：cornerSubPix + 失败防护（细化失败保持原位置）
void RefineSubPix(const cv::Mat& gray, std::vector<cv::Point2f>& pts) {
    cv::cornerSubPix(gray, pts, cv::Size(5, 5), cv::Size(-1, -1),
                     cv::TermCriteria(cv::TermCriteria::EPS | cv::TermCriteria::MAX_ITER, 30, 0.03));
    for (size_t i = 0; i < pts.size(); ++i)
        if (std::isnan(pts[i].x) || std::isnan(pts[i].y) || pts[i].x < 0 || pts[i].y < 0 ||
            pts[i].x >= gray.cols || pts[i].y >= gray.rows)
            pts[i] = cv::Point2f(std::lround(pts[i].x) < 0 ? 0 : pts[i].x,
                                 std::lround(pts[i].y) < 0 ? 0 : pts[i].y);
}

int Image2DAlg_HarrisCorners(const Image2DAlg_Image* in, int block_size, int ksize, double k,
                             double thresh_ratio, int subpix,
                             Image2DAlg_Point2f** out_points, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_points || !out_count) return -1;
    if (block_size < 3 || ksize < 3) return -3;
    if (thresh_ratio < 0.0 || thresh_ratio > 1.0) return -3;
    *out_points = nullptr;
    *out_count = 0;
    try {
        cv::Mat gray = GrayOf(src);
        cv::Mat resp;
        cv::cornerHarris(gray, resp, block_size, ksize, k);
        double lo = 0, hi = 0;
        cv::Point loLoc, hiLoc;
        cv::minMaxLoc(resp, &lo, &hi, &loLoc, &hiLoc);
        const double thr = thresh_ratio * hi;
        cv::Mat dil;
        cv::dilate(resp, dil, cv::Mat());   // 3x3 局部极大值 NMS
        std::vector<cv::Point2f> pts;
        for (int y = 0; y < resp.rows; ++y)
            for (int x = 0; x < resp.cols; ++x)
                if (resp.at<float>(y, x) >= thr && resp.at<float>(y, x) >= dil.at<float>(y, x))
                    pts.push_back(cv::Point2f((float)x, (float)y));
        if (pts.empty()) return 0;
        std::sort(pts.begin(), pts.end(), [&resp](const cv::Point2f& a, const cv::Point2f& b) {
            return resp.at<float>((int)a.y, (int)a.x) > resp.at<float>((int)b.y, (int)b.x);
        });
        if (subpix) RefineSubPix(gray, pts);
        size_t bytes = sizeof(Image2DAlg_Point2f) * pts.size();
        Image2DAlg_Point2f* arr = (Image2DAlg_Point2f*)malloc(bytes);
        if (!arr) return -4;
        memcpy(arr, pts.data(), bytes);
        *out_points = arr;
        *out_count = (int)pts.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_GoodFeatures(const Image2DAlg_Image* in, int max_corners,
                            double quality_level, double min_distance, int subpix,
                            Image2DAlg_Point2f** out_points, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_points || !out_count) return -1;
    if (max_corners < 1) return -3;
    if (quality_level <= 0.0 || quality_level > 1.0) return -3;
    if (min_distance < 1) return -3;
    *out_points = nullptr;
    *out_count = 0;
    try {
        cv::Mat gray = GrayOf(src);
        std::vector<cv::Point2f> corners;
        cv::goodFeaturesToTrack(gray, corners, max_corners, quality_level, min_distance);
        if (corners.empty()) return 0;
        if (subpix) RefineSubPix(gray, corners);
        size_t bytes = sizeof(Image2DAlg_Point2f) * corners.size();
        Image2DAlg_Point2f* arr = (Image2DAlg_Point2f*)malloc(bytes);
        if (!arr) return -4;
        memcpy(arr, corners.data(), bytes);
        *out_points = arr;
        *out_count = (int)corners.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_CornerSubPix(const Image2DAlg_Image* in,
                            const Image2DAlg_Point2f* pts, int count,
                            int win_size, Image2DAlg_Point2f** out_points) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!pts || !out_points) return -1;
    if (count < 1) return -3;
    if (win_size < 3) return -3;
    *out_points = nullptr;
    try {
        cv::Mat gray = GrayOf(src);
        std::vector<cv::Point2f> corners;
        corners.reserve(count);
        for (int i = 0; i < count; ++i)
            corners.push_back(cv::Point2f(pts[i].x, pts[i].y));
        RefineSubPix(gray, corners);
        size_t bytes = sizeof(Image2DAlg_Point2f) * corners.size();
        Image2DAlg_Point2f* arr = (Image2DAlg_Point2f*)malloc(bytes);
        if (!arr) return -4;
        memcpy(arr, corners.data(), bytes);
        *out_points = arr;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

// ---------------- 多边形拟合 / 多目标旋转矩形 ----------------

int Image2DAlg_ApproxPolyDP(const float* xy, int count, double epsilon,
                            int closed, float** out_xy, int* out_count) {
    if (!xy || !out_xy || !out_count) return -1;
    if (count < 3) return -3;
    if (epsilon < 0) return -3;
    *out_xy = nullptr;
    *out_count = 0;
    try {
        std::vector<cv::Point> c;
        c.reserve(count);
        for (int i = 0; i < count; ++i)   // approxPolyDP 接口为整数点（像素坐标）
            c.push_back(cv::Point((int)std::lround(xy[i * 2]), (int)std::lround(xy[i * 2 + 1])));
        std::vector<cv::Point> approx;
        ImageProcessor::approxPolyDP(c, approx, epsilon, closed != 0);
        if ((int)approx.size() < 3) return -3;   // 逼近过度退化（epsilon 过大）
        size_t bytes = sizeof(float) * approx.size() * 2;
        float* arr = (float*)malloc(bytes);
        if (!arr) return -4;
        for (size_t i = 0; i < approx.size(); ++i) {
            arr[i * 2] = (float)approx[i].x;
            arr[i * 2 + 1] = (float)approx[i].y;
        }
        *out_xy = arr;
        *out_count = (int)approx.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FitRectsFromBinary(const Image2DAlg_Image* in,
                                  int min_area, int max_area,
                                  Image2DAlg_RectFit** out_rects, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_rects || !out_count) return -1;
    *out_rects = nullptr;
    *out_count = 0;
    try {
        cv::Mat bin = Binarize(src);
        std::vector<std::vector<cv::Point>> contours;
        ImageProcessor::findContours(bin, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
        std::vector<Image2DAlg_RectFit> result;
        for (const auto& c : contours) {
            const double area = ImageProcessor::contourArea(c);
            if (area < min_area) continue;
            if (max_area > 0 && area > max_area) continue;
            if ((int)c.size() < 3) continue;
            cv::RotatedRect r = ImageProcessor::getMinAreaRect(c);
            if (r.size.width <= 0 || r.size.height <= 0) continue;
            Image2DAlg_RectFit fit;
            fit.cx = r.center.x;
            fit.cy = r.center.y;
            fit.width = r.size.width;
            fit.height = r.size.height;
            fit.angle = r.angle;
            result.push_back(fit);
        }
        if (result.empty()) return 0;
        size_t bytes = sizeof(Image2DAlg_RectFit) * result.size();
        Image2DAlg_RectFit* arr = (Image2DAlg_RectFit*)malloc(bytes);
        if (!arr) return -4;
        memcpy(arr, result.data(), bytes);
        *out_rects = arr;
        *out_count = (int)result.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

void Image2DAlg_FreeRectFits(Image2DAlg_RectFit* rects) {
    free(rects);
}

// ---------------- ORB 特征对位（cv features2d/calib3d 直接调用） ----------------

int Image2DAlg_ORBAlign(const Image2DAlg_Image* in1, const Image2DAlg_Image* in2,
                        int n_features, double ratio, double m[6],
                        Image2DAlg_OrbMatch** out_matches, int* out_count, int* out_inliers) {
    cv::Mat a, b;
    if (!m || !out_matches || !out_count || !out_inliers) return -1;
    if (int rc = Wrap(in1, a)) return rc;
    if (int rc = Wrap(in2, b)) return rc;
    if (n_features <= 0) return -3;
    if (ratio <= 0.0 || ratio >= 1.0) return -3;
    *out_matches = nullptr;
    *out_count = 0;
    *out_inliers = 0;
    try {
        cv::Mat g1 = GrayOf(a), g2 = GrayOf(b);
        // edgeThreshold 默认 31 会在小图上把可用区域挤到中心几点（64x64 → 2x2），
        // 按图像尺寸自适应收紧到 8
        cv::Ptr<cv::ORB> orb = cv::ORB::create(n_features, 1.2f, 8, 8);
        std::vector<cv::KeyPoint> kp1, kp2;
        cv::Mat d1, d2;
        orb->detectAndCompute(g1, cv::noArray(), kp1, d1);
        orb->detectAndCompute(g2, cv::noArray(), kp2, d2);
        if ((int)kp1.size() < 4 || (int)kp2.size() < 4 || d1.empty() || d2.empty()) return -2;

        cv::BFMatcher matcher(cv::NORM_HAMMING);
        std::vector<std::vector<cv::DMatch>> knn;
        matcher.knnMatch(d1, d2, knn, 2);
        std::vector<cv::DMatch> good;
        for (const auto& km : knn)
            if (km.size() == 2 && km[0].distance < ratio * km[1].distance)
                good.push_back(km[0]);
        if ((int)good.size() < 3) return -2;

        std::vector<cv::Point2f> p1, p2;
        p1.reserve(good.size());
        p2.reserve(good.size());
        for (const auto& gm : good) {
            p1.push_back(kp1[gm.queryIdx].pt);
            p2.push_back(kp2[gm.trainIdx].pt);
        }
        cv::Mat inlierMask;
        cv::Mat A = cv::estimateAffine2D(p1, p2, inlierMask, cv::RANSAC, 3.0);
        if (A.empty() || A.rows < 2 || A.cols < 3) return -2;

        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                m[r * 3 + c] = A.at<double>(r, c);
        *out_inliers = cv::countNonZero(inlierMask);

        Image2DAlg_OrbMatch* arr =
            (Image2DAlg_OrbMatch*)malloc(sizeof(Image2DAlg_OrbMatch) * good.size());
        if (!arr) return -4;
        for (size_t i = 0; i < good.size(); ++i) {
            arr[i].x1 = kp1[good[i].queryIdx].pt.x;
            arr[i].y1 = kp1[good[i].queryIdx].pt.y;
            arr[i].x2 = kp2[good[i].trainIdx].pt.x;
            arr[i].y2 = kp2[good[i].trainIdx].pt.y;
            arr[i].distance = good[i].distance;
        }
        *out_matches = arr;
        *out_count = (int)good.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

void Image2DAlg_FreeOrbMatches(Image2DAlg_OrbMatch* matches) {
    free(matches);
}

void Image2DAlg_FreePoint2f(Image2DAlg_Point2f* pts) {
    free(pts);
}


// ---------------- 特征链路拆分（2.1.0：检测/描述/匹配/几何估计可独立复用，与 ORBAlign 同风格） ----------------

int Image2DAlg_FastFeatures(const Image2DAlg_Image* in, int threshold, int nonmax_suppression,
                            Image2DAlg_Point2f** out_points, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_points || !out_count) return -1;
    if (threshold <= 0) return -3;
    *out_points = nullptr;
    *out_count = 0;
    try {
        cv::Mat gray = GrayOf(src);
        cv::Ptr<cv::FastFeatureDetector> fast =
            cv::FastFeatureDetector::create(threshold, nonmax_suppression != 0);
        std::vector<cv::KeyPoint> kps;
        fast->detect(gray, kps);
        if (kps.empty()) return 0;   // 无检出属正常（平坦图）
        size_t bytes = sizeof(Image2DAlg_Point2f) * kps.size();
        Image2DAlg_Point2f* arr = (Image2DAlg_Point2f*)malloc(bytes);
        if (!arr) return -4;
        for (size_t i = 0; i < kps.size(); ++i) {
            arr[i].x = kps[i].pt.x;
            arr[i].y = kps[i].pt.y;
        }
        *out_points = arr;
        *out_count = (int)kps.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

// 描述子算子工厂（ComputeDescriptors / 三检测器 / DetectAndMatch 共用，2.2.0 抽取）。
// type: 0=ORB 1=BRISK 2=AKAZE 3=SIFT；contrast_threshold <= 0 -> 各算法默认
//（BRISK thresh=30 / AKAZE threshold=0.001 / SIFT contrastThreshold=0.04，ORB 无此参数）；
// n_features：ORB <= 0 取默认 500（与 ORBAlign 一致）、SIFT <= 0 不限、其余算法无上限参数忽略。
// C++ 链接（返回 cv::Ptr 非法定义为 C 链接可导出，消除 C4190；本文件内部 helper 不导出）
extern "C++" {
cv::Ptr<cv::Feature2D> MakeFeatureAlgorithm(int type, int n_features, double contrast_threshold) {
    if (type == 0) {
        if (n_features <= 0) n_features = 500;
        // edgeThreshold 默认 31 在小图上挤出可用区域，同 ORBAlign 收紧到 8
        return cv::ORB::create(n_features, 1.2f, 8, 8);
    }
    if (type == 1) {
        int thresh = contrast_threshold > 0.0 ? (int)std::lround(contrast_threshold) : 30;
        return cv::BRISK::create(thresh);
    }
    if (type == 2) {
        const float threshold = contrast_threshold > 0.0 ? (float)contrast_threshold : 0.001f;
        return cv::AKAZE::create(cv::AKAZE::DESCRIPTOR_MLDB, 0, 3, threshold);
    }
    return cv::SIFT::create(n_features > 0 ? n_features : 0, 3,
                            contrast_threshold > 0.0 ? contrast_threshold : 0.04);
}
} // extern "C++"

// cv::Mat 描述子 -> Descriptors 结构（malloc data/fdata；内存不足返回 -4）。
// 前置：desc 非空、out 的 data/fdata 尚未分配（ComputeDescriptors / 检测器共用）。
int FillDescriptors(int type, const cv::Mat& desc, Image2DAlg_Descriptors* out) {
    const bool is_float = (desc.depth() == CV_32F);
    const size_t n_elem = (size_t)desc.rows * desc.cols;
    // 整块 memcpy 隐含紧排假设；算子输出的描述子 Mat 总是紧排，此处防御性兜底
    //（上游行为变化时静默复制出步长错位的数据比 -99 更难排查）
    const cv::Mat cdesc = desc.isContinuous() ? desc : desc.clone();
    if (is_float) {
        float* buf = (float*)malloc(n_elem * sizeof(float));
        if (!buf) return -4;
        memcpy(buf, cdesc.data, n_elem * sizeof(float));
        out->fdata = buf;
    } else {
        unsigned char* buf = (unsigned char*)malloc(n_elem);
        if (!buf) return -4;
        memcpy(buf, cdesc.data, n_elem);
        out->data = buf;
    }
    out->type = type;
    out->dim = desc.cols;
    out->is_float = is_float ? 1 : 0;
    out->count = desc.rows;
    return 0;
}

// BFMatcher kNN(k=2) + Lowe 比率测试（MatchFeatures / DetectAndMatch 共用）。
// 二进制用 Hamming、浮点用 L2；比率测试后不足 2 对返回 -2（平坦图属正常）。
int MatchDescriptorsLowe(const cv::Mat& d1, const cv::Mat& d2, bool is_float, double ratio,
                         std::vector<cv::DMatch>& good) {
    cv::BFMatcher matcher(is_float ? cv::NORM_L2 : cv::NORM_HAMMING);
    std::vector<std::vector<cv::DMatch>> knn;
    matcher.knnMatch(d1, d2, knn, 2);
    good.clear();
    for (const auto& km : knn)
        if (km.size() == 2 && km[0].distance < ratio * km[1].distance)
            good.push_back(km[0]);
    if ((int)good.size() < 2) return -2;
    return 0;
}

int Image2DAlg_ComputeDescriptors(const Image2DAlg_Image* in,
                                  const Image2DAlg_Point2f* pts, int count,
                                  int type, int n_features,
                                  Image2DAlg_Descriptors* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!pts || !out) return -1;
    if (count < 1) return -3;
    if (type < 0 || type > 3) return -3;
    try {
        // 关键点须在图像内（越界无法取邻域计算描述子）。
        // 该检查必须在清零 out 之前：复用同一结构体（上次结果尚未 Free）的调用方
        // 在 -2 出口不能丢已有缓冲指针——本库无凭据替其释放，只能靠泄漏
        for (int i = 0; i < count; ++i)
            if (pts[i].x < 0.0f || pts[i].y < 0.0f ||
                pts[i].x >= (float)src.cols || pts[i].y >= (float)src.rows)
                return -2;
        out->type = type;
        out->dim = 0;
        out->is_float = 0;
        out->count = 0;
        out->data = nullptr;
        out->fdata = nullptr;
        cv::Mat gray = GrayOf(src);
        // 描述子采样窗口最大约 ±48px（ORB 模板/方向窗 ±15、BRISK 采样圆 ~21、
        // AKAZE MLDB ~25、SIFT 窗口 ~36，关键点名义尺寸固定 8）。四类算子的
        // compute 对"外部关键点"不做统一的贴边防护（实测 ORB 在 8~15px 贴边区、
        // AKAZE/SIFT 在 ~25px 内的输出与确定性参照不一致——越界读出垃圾描述子，
        // 不抛异常不报错），这里统一复制边框扩图后偏移采样，保证：
        // - 任何贴边点都落在图内，无越界读、结果确定；
        // - 离边 >= 采样窗的内部点结果与直接计算逐位一致（复制边框不进入采样窗）。
        const int margin = 64;
        cv::Mat padded;
        cv::copyMakeBorder(gray, padded, margin, margin, margin, margin, cv::BORDER_REPLICATE);
        std::vector<cv::KeyPoint> kps;
        kps.reserve(count);
        for (int i = 0; i < count; ++i) {
            cv::KeyPoint kp(pts[i].x + margin, pts[i].y + margin, 8.0f);   // 名义邻域尺寸
            // AKAZE 按关键点 class_id 选尺度空间层（须落在 [0, evolution 数)），
            // 外部关键点统一取基尺度（class_id=0，与其 detect 的基尺度点一致）
            if (type == 2) kp.class_id = 0;
            kps.push_back(kp);
        }
        // 描述子算子工厂：ORB 参数与 ORBAlign 保持一致；SIFT 自 4.4.0 进入
        // 主库 features2d（world440 已导出 cv::SIFT::create，无需 contrib）
        cv::Ptr<cv::Feature2D> algo = MakeFeatureAlgorithm(type, n_features, 0.0);
        cv::Mat desc;
        algo->compute(padded, kps, desc);
        // compute 可能丢弃无法计算的点——数量对不上即不足（扩图后贴边点不再被
        // ORB 的边距过滤误杀，该出口仅作为防御保留）
        if ((int)kps.size() != count || desc.empty() || desc.rows != count) return -2;
        return FillDescriptors(type, desc, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

void Image2DAlg_FreeDescriptors(Image2DAlg_Descriptors* d) {
    if (!d) return;
    free(d->data);
    d->data = nullptr;
    free(d->fdata);
    d->fdata = nullptr;
}

int Image2DAlg_MatchFeatures(const Image2DAlg_Descriptors* d1,
                             const Image2DAlg_Descriptors* d2,
                             const Image2DAlg_Point2f* pts1,
                             const Image2DAlg_Point2f* pts2,
                             double ratio,
                             Image2DAlg_OrbMatch** out_matches, int* out_count) {
    if (!d1 || !d2 || !pts1 || !pts2 || !out_matches || !out_count) return -1;
    if (ratio <= 0.0 || ratio >= 1.0) return -3;
    if (d1->type != d2->type || d1->dim != d2->dim || d1->is_float != d2->is_float) return -3;
    if (d1->type < 0 || d1->type > 3) return -3;
    if (d1->count < 1 || d2->count < 1) return -2;   // 空描述子集 = 特征不足
    if (d1->is_float ? (!d1->fdata || !d2->fdata) : (!d1->data || !d2->data)) return -1;
    *out_matches = nullptr;
    *out_count = 0;
    try {
        // 调用方保证 pts1/pts2 与 d1/d2 行序一一对应（ComputeDescriptors 的输入点集）
        cv::Mat m1, m2;
        if (d1->is_float) {
            m1 = cv::Mat(d1->count, d1->dim, CV_32F, (void*)d1->fdata);
            m2 = cv::Mat(d2->count, d2->dim, CV_32F, (void*)d2->fdata);
        } else {
            m1 = cv::Mat(d1->count, d1->dim, CV_8U, (void*)d1->data);
            m2 = cv::Mat(d2->count, d2->dim, CV_8U, (void*)d2->data);
        }
        std::vector<cv::DMatch> good;
        if (int rc = MatchDescriptorsLowe(m1, m2, d1->is_float != 0, ratio, good)) return rc;
        Image2DAlg_OrbMatch* arr =
            (Image2DAlg_OrbMatch*)malloc(sizeof(Image2DAlg_OrbMatch) * good.size());
        if (!arr) return -4;
        for (size_t i = 0; i < good.size(); ++i) {
            arr[i].x1 = pts1[good[i].queryIdx].x;
            arr[i].y1 = pts1[good[i].queryIdx].y;
            arr[i].x2 = pts2[good[i].trainIdx].x;
            arr[i].y2 = pts2[good[i].trainIdx].y;
            arr[i].distance = good[i].distance;
        }
        *out_matches = arr;
        *out_count = (int)good.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

// 匹配点对 -> OpenCV 点组（matches[i] 为 in1 → in2 对应）
void MatchPointsOf(const Image2DAlg_OrbMatch* matches, int count,
                   std::vector<cv::Point2f>& p1, std::vector<cv::Point2f>& p2) {
    p1.reserve(count);
    p2.reserve(count);
    for (int i = 0; i < count; ++i) {
        p1.push_back(cv::Point2f((float)matches[i].x1, (float)matches[i].y1));
        p2.push_back(cv::Point2f((float)matches[i].x2, (float)matches[i].y2));
    }
}

int Image2DAlg_EstimateAffinePartial(const Image2DAlg_OrbMatch* matches,
                                     int count, double ransac_reproj_threshold,
                                     double m[6], int* out_inliers) {
    if (!matches || !m || !out_inliers) return -1;
    if (count < 3) return -2;   // 相似变换最少 2 点 + 1 点校验，< 3 直接判不足
    if (ransac_reproj_threshold <= 0.0) return -3;
    *out_inliers = 0;
    try {
        std::vector<cv::Point2f> p1, p2;
        MatchPointsOf(matches, count, p1, p2);
        cv::Mat inlierMask;
        // in1 → in2 方向（与 ORBAlign 的 estimateAffine2D(p1, p2) 一致）
        cv::Mat A = cv::estimateAffinePartial2D(p1, p2, inlierMask, cv::RANSAC,
                                                ransac_reproj_threshold);
        if (A.empty() || A.rows < 2 || A.cols < 3) return -2;
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                m[r * 3 + c] = A.at<double>(r, c);
        *out_inliers = cv::countNonZero(inlierMask);
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FindHomography(const Image2DAlg_OrbMatch* matches,
                              int count, double ransac_reproj_threshold,
                              double h[9], int* out_inliers) {
    if (!matches || !h || !out_inliers) return -1;
    if (count < 4) return -2;   // 单应最少 4 点（3 点欠定）
    if (ransac_reproj_threshold <= 0.0) return -3;
    *out_inliers = 0;
    try {
        std::vector<cv::Point2f> p1, p2;
        MatchPointsOf(matches, count, p1, p2);
        cv::Mat inlierMask;
        // in1 → in2 方向（与 ORBAlign 的输出约定一致）；共线/退化返回空 Mat
        cv::Mat H = cv::findHomography(p1, p2, inlierMask, cv::RANSAC,
                                       ransac_reproj_threshold);
        if (H.empty() || H.rows < 3 || H.cols < 3) return -2;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                h[r * 3 + c] = H.at<double>(r, c);
        *out_inliers = cv::countNonZero(inlierMask);
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}


// ---------------- 多目标直线拟合（板边 / 边缘线测量） ----------------

int Image2DAlg_FitLinesFromBinary(const Image2DAlg_Image* in,
                                  int min_area, int max_area, double min_length,
                                  Image2DAlg_LineFit** out_lines, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_lines || !out_count) return -1;
    *out_lines = nullptr;
    *out_count = 0;
    try {
        cv::Mat bin = Binarize(src);
        std::vector<std::vector<cv::Point>> contours;
        ImageProcessor::findContours(bin, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
        std::vector<Image2DAlg_LineFit> result;
        for (const auto& c : contours) {
            const double area = ImageProcessor::contourArea(c);
            if (area < min_area) continue;
            if (max_area > 0 && area > max_area) continue;
            if ((int)c.size() < 2) continue;
            std::vector<float> line;
            ImageProcessor::fitLine(c, line, cv::DIST_L2);
            Image2DAlg_LineFit fit;
            fit.vx = line[0];
            fit.vy = line[1];
            fit.x0 = line[2];
            fit.y0 = line[3];
            // 覆盖跨度：轮廓点在方向上的投影极差（直线度过滤用）
            double tmin = 0, tmax = 0;
            for (const cv::Point& p : c) {
                const double t = (p.x - fit.x0) * fit.vx + (p.y - fit.y0) * fit.vy;
                tmin = std::min(tmin, t);
                tmax = std::max(tmax, t);
            }
            const double span = tmax - tmin;
            if (min_length > 0 && span < min_length) continue;
            double sum = 0.0;
            for (const cv::Point& p : c) {
                const double dx = p.x - fit.x0, dy = p.y - fit.y0;
                const double cross = dx * fit.vy - dy * fit.vx;
                sum += cross * cross;
            }
            fit.rms = std::sqrt(sum / c.size());
            result.push_back(fit);
        }
        if (result.empty()) return 0;
        size_t bytes = sizeof(Image2DAlg_LineFit) * result.size();
        Image2DAlg_LineFit* arr = (Image2DAlg_LineFit*)malloc(bytes);
        if (!arr) return -4;
        memcpy(arr, result.data(), bytes);
        *out_lines = arr;
        *out_count = (int)result.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

void Image2DAlg_FreeLineFits(Image2DAlg_LineFit* lines) {
    free(lines);
}


// ---------------- 2.2.0：特征扩展（三检测器 / 斑点 / 一站式匹配，与 2.1.0 同风格） ----------------

// 检测 + 描述一次完成（Brisk/Akaze/SiftFeatures 的公共实现，type 同 ComputeDescriptors）
int DetectAndDescribeFeatures(const Image2DAlg_Image* in, int type, int n_features,
                              double contrast_threshold,
                              Image2DAlg_Point2f** out_points, int* out_count,
                              Image2DAlg_Descriptors* out_desc) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_points || !out_count) return -1;
    *out_points = nullptr;
    *out_count = 0;
    if (out_desc) {
        out_desc->type = type;
        out_desc->dim = 0;
        out_desc->is_float = 0;
        out_desc->count = 0;
        out_desc->data = nullptr;
        out_desc->fdata = nullptr;
    }
    try {
        cv::Mat gray = GrayOf(src);
        std::vector<cv::KeyPoint> kps;
        cv::Mat desc;
        MakeFeatureAlgorithm(type, n_features, contrast_threshold)
            ->detectAndCompute(gray, cv::noArray(), kps, desc);
        if (kps.empty()) return 0;   // 无检出属正常（平坦图），desc 亦为空
        // 先分配点数组再填描述子：若反过来，点数组分配失败（-4）时 out_desc 已
        // 持有 malloc 缓冲，失败出口不再是"输出全空"的干净状态；先分配 arr，
        // 描述子缓冲分配失败 free(arr) 即可整体回滚
        size_t bytes = sizeof(Image2DAlg_Point2f) * kps.size();
        Image2DAlg_Point2f* arr = (Image2DAlg_Point2f*)malloc(bytes);
        if (!arr) return -4;
        if (out_desc && !desc.empty()) {
            // detectAndCompute 自产关键点与描述子按行序一一对应
            if (int rc = FillDescriptors(type, desc, out_desc)) {
                free(arr);
                return rc;
            }
        }
        for (size_t i = 0; i < kps.size(); ++i) {
            arr[i].x = kps[i].pt.x;
            arr[i].y = kps[i].pt.y;
        }
        *out_points = arr;
        *out_count = (int)kps.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_BriskFeatures(const Image2DAlg_Image* in, int n_features,
                             double contrast_threshold,
                             Image2DAlg_Point2f** out_points, int* out_count,
                             Image2DAlg_Descriptors* out_desc) {
    return DetectAndDescribeFeatures(in, 1, n_features, contrast_threshold,
                                     out_points, out_count, out_desc);
}

int Image2DAlg_AkazeFeatures(const Image2DAlg_Image* in, int n_features,
                             double contrast_threshold,
                             Image2DAlg_Point2f** out_points, int* out_count,
                             Image2DAlg_Descriptors* out_desc) {
    return DetectAndDescribeFeatures(in, 2, n_features, contrast_threshold,
                                     out_points, out_count, out_desc);
}

int Image2DAlg_SiftFeatures(const Image2DAlg_Image* in, int n_features,
                            double contrast_threshold,
                            Image2DAlg_Point2f** out_points, int* out_count,
                            Image2DAlg_Descriptors* out_desc) {
    return DetectAndDescribeFeatures(in, 3, n_features, contrast_threshold,
                                     out_points, out_count, out_desc);
}

int Image2DAlg_BlobDetect(const Image2DAlg_Image* in, double min_area, double max_area,
                          double min_circularity, double min_convexity,
                          Image2DAlg_BlobFeature** out_blobs, int* out_count) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out_blobs || !out_count) return -1;
    *out_blobs = nullptr;
    *out_count = 0;
    try {
        // 四个过滤参数按接口约定构建，其余参数用 SimpleBlobDetector 默认
        //（暗斑 blobColor=0 / 灰度阈值 50~220 步进 10 / 惯性比过滤 >= 0.1 / 重复性 2）。
        // 注意：4.4 默认 Params 的凸性过滤是开启的（minConvexity=0.95，会滤掉
        // 一般光栅化圆的锯齿轮廓）——本接口 <= 0 时显式关闭，保证“不过滤”语义可预期
        cv::SimpleBlobDetector::Params params;
        if (min_area > 0.0) {
            params.minArea = (float)min_area;
        } else {
            params.minArea = 0.0f;   // <= 0 = 无下限
        }
        if (max_area > 0.0) {
            params.maxArea = (float)max_area;
        } else {
            params.maxArea = std::numeric_limits<float>::max();   // <= 0 = 不设上限
        }
        if (min_circularity > 0.0) {   // > 0 启用圆度过滤
            params.filterByCircularity = true;
            params.minCircularity = (float)min_circularity;
        } else {
            params.filterByCircularity = false;   // <= 0 = 不过滤（显式关闭）
        }
        if (min_convexity > 0.0) {     // > 0 启用凸性过滤
            params.filterByConvexity = true;
            params.minConvexity = (float)min_convexity;
        } else {
            params.filterByConvexity = false;   // <= 0 = 不过滤（显式关闭，见上注）
        }
        cv::Mat gray = GrayOf(src);
        cv::Ptr<cv::SimpleBlobDetector> det = cv::SimpleBlobDetector::create(params);
        std::vector<cv::KeyPoint> kps;
        det->detect(gray, kps);
        if (kps.empty()) return 0;   // 无检出属正常
        size_t bytes = sizeof(Image2DAlg_BlobFeature) * kps.size();
        Image2DAlg_BlobFeature* arr = (Image2DAlg_BlobFeature*)malloc(bytes);
        if (!arr) return -4;
        for (size_t i = 0; i < kps.size(); ++i) {
            arr[i].x = kps[i].pt.x;
            arr[i].y = kps[i].pt.y;
            // SimpleBlobDetector 的 KeyPoint.size = 斑点直径（含 1~2px 光栅化偏差），
            // 取一半输出为半径
            arr[i].radius = kps[i].size * 0.5f;
        }
        *out_blobs = arr;
        *out_count = (int)kps.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

void Image2DAlg_FreeBlobFeatures(Image2DAlg_BlobFeature* blobs) {
    free(blobs);
}

int Image2DAlg_DetectAndMatch(const Image2DAlg_Image* in1, const Image2DAlg_Image* in2,
                              int type, int n_features, double ratio,
                              Image2DAlg_OrbMatch** out_matches, int* out_count) {
    cv::Mat a, b;
    if (!out_matches || !out_count) return -1;
    if (int rc = Wrap(in1, a)) return rc;
    if (int rc = Wrap(in2, b)) return rc;
    if (type < 0 || type > 3) return -3;
    if (ratio <= 0.0 || ratio >= 1.0) return -3;
    *out_matches = nullptr;
    *out_count = 0;
    try {
        cv::Mat g1 = GrayOf(a), g2 = GrayOf(b);
        // 检测 + 描述（与 ComputeDescriptors 同款算子工厂；两图各做一次）
        cv::Ptr<cv::Feature2D> algo = MakeFeatureAlgorithm(type, n_features, 0.0);
        std::vector<cv::KeyPoint> kp1, kp2;
        cv::Mat d1, d2;
        algo->detectAndCompute(g1, cv::noArray(), kp1, d1);
        algo->detectAndCompute(g2, cv::noArray(), kp2, d2);
        if ((int)kp1.size() < 1 || (int)kp2.size() < 1 || d1.empty() || d2.empty()) return -2;
        std::vector<cv::DMatch> good;
        if (int rc = MatchDescriptorsLowe(d1, d2, type == 3, ratio, good)) return rc;
        Image2DAlg_OrbMatch* arr =
            (Image2DAlg_OrbMatch*)malloc(sizeof(Image2DAlg_OrbMatch) * good.size());
        if (!arr) return -4;
        for (size_t i = 0; i < good.size(); ++i) {
            arr[i].x1 = kp1[good[i].queryIdx].pt.x;
            arr[i].y1 = kp1[good[i].queryIdx].pt.y;
            arr[i].x2 = kp2[good[i].trainIdx].pt.x;
            arr[i].y2 = kp2[good[i].trainIdx].pt.y;
            arr[i].distance = good[i].distance;
        }
        *out_matches = arr;
        *out_count = (int)good.size();
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}


// ---------------- 2.2.0：跟踪、配准与立体（video / calib3d 直接调用） ----------------

int Image2DAlg_CalcOpticalFlowPyrLK(const Image2DAlg_Image* prev, const Image2DAlg_Image* next,
                                    const Image2DAlg_Point2f* pts, int count,
                                    int win_size, int max_level,
                                    Image2DAlg_Point2f** out_pts, unsigned char* out_status) {
    cv::Mat a, b;
    if (!out_pts || !out_status) return -1;
    if (int rc = Wrap(prev, a)) return rc;
    if (int rc = Wrap(next, b)) return rc;
    if (!pts) return -1;
    if (count < 1) return -3;
    if (a.rows != b.rows || a.cols != b.cols) return -3;
    if (win_size <= 0) return -3;
    if (max_level < 0) return -3;
    win_size = OddAtLeast(win_size, 1);   // 偶数自动 +1（OpenCV 要求奇数窗口）
    *out_pts = nullptr;
    try {
        cv::Mat g1 = GrayOf(a), g2 = GrayOf(b);
        std::vector<cv::Point2f> pv;
        pv.reserve(count);
        for (int i = 0; i < count; ++i) pv.push_back(cv::Point2f(pts[i].x, pts[i].y));
        std::vector<cv::Point2f> nv;
        std::vector<unsigned char> st;
        std::vector<float> err;
        cv::calcOpticalFlowPyrLK(g1, g2, pv, nv, st, err,
                                 cv::Size(win_size, win_size), max_level);
        Image2DAlg_Point2f* arr = (Image2DAlg_Point2f*)malloc(sizeof(Image2DAlg_Point2f) * count);
        if (!arr) return -4;
        for (int i = 0; i < count; ++i) {
            arr[i].x = nv[i].x;
            arr[i].y = nv[i].y;
            out_status[i] = st[i];   // 写入调用方预分配的 status 缓冲（一级指针契约）
        }
        *out_pts = arr;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_FindTransformECC(const Image2DAlg_Image* templ, const Image2DAlg_Image* input,
                                int motion_type, int max_iters, double epsilon,
                                double m[9], double* out_correlation) {
    cv::Mat a, b;
    if (!m || !out_correlation) return -1;
    if (int rc = Wrap(templ, a)) return rc;
    if (int rc = Wrap(input, b)) return rc;
    if (motion_type < 0 || motion_type > 2) return -3;
    if (max_iters < 1) return -3;
    if (epsilon <= 0.0) return -3;
    if (a.rows != b.rows || a.cols != b.cols) return -3;
    // 接口枚举 -> cv::MOTION_*（注意 cv 原生 1 是 EUCLIDEAN，本 API 不暴露欧氏模型）
    const int cv_motion = (motion_type == 0) ? cv::MOTION_TRANSLATION
                        : (motion_type == 1) ? cv::MOTION_AFFINE
                                             : cv::MOTION_HOMOGRAPHY;
    try {
        cv::Mat t32, i32;
        GrayOf(a).convertTo(t32, CV_32F);   // ECC 要求 CV_32F
        GrayOf(b).convertTo(i32, CV_32F);
        cv::Mat warp = (motion_type == 2) ? cv::Mat::eye(3, 3, CV_32F)
                                          : cv::Mat::eye(2, 3, CV_32F);
        double correlation = 0.0;
        try {
            correlation = cv::findTransformECC(t32, i32, warp, cv_motion,
                                               cv::TermCriteria(cv::TermCriteria::COUNT +
                                                                cv::TermCriteria::EPS,
                                                                max_iters, epsilon));
        } catch (const cv::Exception&) {
            // OpenCV 对不收敛直接抛异常（4.4 文档："throws an exception if
            // algorithm does not converges"）——按 -2（不收敛/相关性不足）口径
            return -2;
        }
        if (!(correlation >= 0.5)) return -2;   // 迭代耗尽且相关性差/NaN（口径见头文件注释）
        // 2x3（平移/仿射）写前 6 个并把 m[6..8] 清零——防止调用方误按 3x3 使用时
        // 读到栈上残留值；单应路径 9 个全写
        const int rows = (motion_type == 2) ? 3 : 2;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < 3; ++c)
                m[r * 3 + c] = warp.at<float>(r, c);
        if (motion_type != 2) m[6] = m[7] = m[8] = 0.0;
        *out_correlation = correlation;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_PhaseCorrelate(const Image2DAlg_Image* a, const Image2DAlg_Image* b,
                              double* out_dx, double* out_dy, double* out_response) {
    cv::Mat ma, mb;
    if (!out_dx || !out_dy || !out_response) return -1;
    if (int rc = Wrap(a, ma)) return rc;
    if (int rc = Wrap(b, mb)) return rc;
    if (ma.rows != mb.rows || ma.cols != mb.cols) return -3;
    try {
        cv::Mat f1, f2;
        GrayOf(ma).convertTo(f1, CV_32F);   // phaseCorrelate 要求 float
        GrayOf(mb).convertTo(f2, CV_32F);
        double response = 0.0;
        // 窗口函数用默认（noArray）；getOptimalDFTSize 补边由 cv 内部完成
        const cv::Point2d shift = cv::phaseCorrelate(f1, f2, cv::noArray(), &response);
        *out_dx = shift.x;
        *out_dy = shift.y;
        *out_response = response;
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_StereoMatch(const Image2DAlg_Image* left, const Image2DAlg_Image* right,
                           int min_disparity, int num_disparities, int block_size,
                           Image2DAlg_Image* out_disparity) {
    cv::Mat l, r;
    if (int rc = Wrap(left, l)) return rc;
    if (int rc = Wrap(right, r)) return rc;
    if (!out_disparity) return -1;
    if (!out_disparity->data) return -1;   // 调用方预分配（一级指针契约）
    if (l.rows != r.rows || l.cols != r.cols) return -3;
    if (num_disparities <= 0 || num_disparities % 16 != 0) return -3;
    // 容器校验：尺寸与左右图一致、输出固定 1 通道（同 ToGray 容器约定）
    if (out_disparity->width != l.cols || out_disparity->height != l.rows ||
        out_disparity->channels != 1)
        return -3;
    block_size = OddAtLeast(block_size, 3);   // 奇数 >= 3
    try {
        cv::Mat gl = GrayOf(l), gr = GrayOf(r);
        // P1/P2 按 OpenCV 推荐随 block 设定（8/32·block²），其余参数默认
        cv::Ptr<cv::StereoSGBM> sgbm = cv::StereoSGBM::create(
            min_disparity, num_disparities, block_size,
            8 * block_size * block_size, 32 * block_size * block_size);
        cv::Mat disp16;
        sgbm->compute(gl, gr, disp16);   // CV_16S 定点（视差 * 16）
        // 定标口径：/16 四舍五入得整数视差，钳到 [0,255]。
        // 无效视差 -> 0：SGBM 以 (min_disparity-1)*16 填充匹配不可达的左条带，
        // 该标记仅在 min_disparity <= 1 时落在负数区间（可被负值钳位兜住），
        // min_disparity >= 2 时必须显式比对标记值，否则会以 1..n 的合法假象漏出
        const int invalid_raw = (min_disparity - 1) * 16;
        unsigned char* dstp = out_disparity->data;
        for (int y = 0; y < disp16.rows; ++y) {
            const short* row = disp16.ptr<short>(y);
            unsigned char* outrow = dstp + (size_t)y * disp16.cols;
            for (int x = 0; x < disp16.cols; ++x) {
                const int raw = row[x];
                if (raw == invalid_raw) {   // SGBM 无效视差标记 -> 0（契约见头文件）
                    outrow[x] = 0;
                    continue;
                }
                int v = (int)std::lround(raw / 16.0);
                if (v < 0) v = 0;   // 其余越界负值一并钳到 0
                if (v > 255) v = 255;
                outrow[x] = (unsigned char)v;
            }
        }
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_SolvePnP(const double* object_points, const Image2DAlg_Point2f* image_points,
                        int count, const double camera_matrix[9],
                        const double* dist_coeffs, int dist_count,
                        double rvec[3], double tvec[3]) {
    if (!object_points || !image_points || !camera_matrix || !rvec || !tvec) return -1;
    if (count < 4) return -2;   // PnP 最少 4 点（3 点欠定）
    // dist_count 口径：<= 0 无畸变；OpenCV 支持的畸变模型 4/5/8/12/14 项。
    // 其余正值 cv 内部断言必炸（而且会先对调用方短缓冲构造超长零拷贝视图造成
    // 越界读），在此显式拦截，按模块约定返回 -3（参数取值不支持）
    if (dist_count > 0 && dist_count != 4 && dist_count != 5 &&
        dist_count != 8 && dist_count != 12 && dist_count != 14) return -3;
    try {
        // 3N double / Point2f 数组的紧排布局即 Nx1 CV_64FC3 / CV_32FC2（零拷贝视图）
        cv::Mat obj(count, 1, CV_64FC3, (void*)object_points);
        cv::Mat img(count, 1, CV_32FC2, (void*)image_points);
        cv::Mat cam(3, 3, CV_64F, (void*)camera_matrix);
        // dist_coeffs 为 NULL 或 dist_count <= 0 视为无畸变
        cv::Mat dist = (dist_coeffs && dist_count > 0)
                           ? cv::Mat(dist_count, 1, CV_64F, (void*)dist_coeffs)
                           : cv::Mat();
        cv::Mat rv, tv;
        // 默认迭代法（SOLVEPNP_ITERATIVE）：精度优于纯线性解，4 点以上通用
        if (!cv::solvePnP(obj, img, cam, dist, rv, tv, false, cv::SOLVEPNP_ITERATIVE))
            return -2;   // 退化配置求解失败
        for (int i = 0; i < 3; ++i) {
            rvec[i] = rv.at<double>(i, 0);
            tvec[i] = tv.at<double>(i, 0);
        }
        return 0;
    } catch (const cv::Exception&) {
        return -99;
    }
}

// ---------------- 滤波扩展（2.3.0 新增） ----------------

int Image2DAlg_Laplacian(const Image2DAlg_Image* in, int ksize, double scale,
                         Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    if (ksize != 1 && ksize != 3 && ksize != 5 && ksize != 7) return -3;
    if (!(scale > 0.0)) return -3;
    try {
        cv::Mat dst;
        const bool floatIn = (src.depth() == CV_32F || src.depth() == CV_64F);
        if (floatIn) {
            // 浮点输入：同深度直出（保留符号）
            cv::Laplacian(src, dst, src.depth(), ksize, scale);
        } else {
            // 8U/16U：CV_16S 中间 -> |响应|×scale 饱和回原深度（负响应折叠，
            // 工业边缘增强常用口径；需要符号信息时改用浮点输入）
            cv::Mat mid;
            cv::Laplacian(src, mid, CV_16S, ksize, scale);
            cv::convertScaleAbs(mid, dst);
        }
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_Filter2D(const Image2DAlg_Image* in, const float* kernel, int ksize,
                        Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!kernel || !out) return -1;
    if (ksize < 1 || ksize > 51 || ksize % 2 == 0) return -3;
    try {
        cv::Mat kern(ksize, ksize, CV_32F, (void*)kernel);
        cv::Mat dst;
        const bool floatIn = (src.depth() == CV_32F || src.depth() == CV_64F);
        if (floatIn) {
            cv::filter2D(src, dst, src.depth(), kern);
        } else {
            // 8U/16U：CV_16S 中间（保留符号与幅值）-> |响应| 饱和回原深度
            cv::Mat mid;
            cv::filter2D(src, mid, CV_16S, kern);
            cv::convertScaleAbs(mid, dst);
        }
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

int Image2DAlg_DenoiseNLMeans(const Image2DAlg_Image* in, double h, double h_color,
                              int template_window, int search_window,
                              Image2DAlg_Image* out) {
    cv::Mat src;
    if (int rc = Wrap(in, src)) return rc;
    if (!out) return -1;
    // 强度参数须为正（NLMeans 的 h 是噪声水平权重，<=0 无意义）；窗口先按
    // 调用值比较（search 须 > template），再自动补奇（下限 3 / 补奇后仍严格大于）
    if (!(h > 0.0) || !(h_color > 0.0)) return -3;
    if (search_window <= template_window) return -3;
    const int tw = OddAtLeast(template_window, 3);
    const int sw = OddAtLeast(search_window, tw + 2);
    try {
        cv::Mat dst;
        if (src.channels() == 1) {
            cv::fastNlMeansDenoising(src, dst, (float)h, tw, sw);
        } else if (src.channels() == 3) {
            cv::fastNlMeansDenoisingColored(src, dst, (float)h, (float)h_color, tw, sw);
        } else {
            // 4 通道 BGRA：NLMeans 不支持 4 通道——拆 BGR 走 Colored 后回贴 alpha
            std::vector<cv::Mat> ch;
            cv::split(src, ch);
            cv::Mat bgr, denoised;
            cv::merge(std::vector<cv::Mat>{ch[0], ch[1], ch[2]}, bgr);
            cv::fastNlMeansDenoisingColored(bgr, denoised, (float)h, (float)h_color, tw, sw);
            std::vector<cv::Mat> out_ch;
            cv::split(denoised, out_ch);
            out_ch.push_back(ch[3]);
            cv::merge(out_ch, dst);
        }
        return CopyOut(dst, out);
    } catch (const cv::Exception&) {
        return -99;
    }
}

} // extern "C"
