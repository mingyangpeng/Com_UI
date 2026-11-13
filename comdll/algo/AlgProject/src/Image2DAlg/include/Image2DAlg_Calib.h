#ifndef IMAGE2DALG_CALIB_H
#define IMAGE2DALG_CALIB_H

// Image2DAlg_Calib —— 相机标定（单目 / 双目立体校正 / 鱼眼 Mei 等距模型）。
// 纯 C++ 手写实现（基于 Eigen 线性代数，OpenCV 仅用图像容器与角点检测数据结构）。
//
// Mei 模型（Mei et al. 2007, "Single View Point Omnidirectional Camera Calibration
// from Planar Grids"）—— 等距投影统一球面模型：
//   世界点 X → 单位球面投影 x_s = X/||X||（以镜面中心为原点）
//   → 极坐标投影 r = ξ + f·θ(ρ)，θ = atan2(√(x²+y²), z) 为球面入射角
//   → 畸变 s(ρ) 多项式 → 像素坐标 u = c + G·m + s
// 线性部分：u = K·m + 畸变；K = [[f,0,cx],[0,f,cy],[0,0,1]]（f 为广义焦距）
// ξ ∈ [0,1]：0=理想针孔，1=纯双曲面镜。
//
// 单目：张正友法求初值 → 非线性最小二乘（Levenberg-Marquardt）精化（含 Mei 鱼眼）。
// 双目：左右各自标定 → 立体对齐（R,t 精化）→ 立体校正（ Bouguet 方法）。
//
// 标定结果持久化：纯文本 key=value（易读易解析），save/load 对称。

#include "AlgExport.h"

#include <opencv2/core.hpp>   // cv::Mat / cv::Point2f / cv::Size

#include <string>
#include <vector>

namespace image2d {

// ---------------- 相机内参（针孔 + 畸变） ----------------
struct IMAGE2DALG_EXPORT CameraIntrinsics {
    cv::Size  imageSize;                 // 图像尺寸
    double    fx = 0, fy = 0;            // 焦距（像素）
    double    cx = 0, cy = 0;            // 光心
    double    k1 = 0, k2 = 0, p1 = 0, p2 = 0, k3 = 0;   // 针孔畸变系数
    double    xi = 0;                    // Mei 鱼眼参数（0=针孔，1=双曲面）
    double    meiC = 0, meiD = 0, meiE = 0;   // Mei 广义相机畸变参数

    cv::Mat K() const;                   // 3x3 内参矩阵（CV_64F）
};

// ---------------- 外参（双目） ----------------
struct IMAGE2DALG_EXPORT StereoExtrinsics {
    cv::Mat R;    // 3x3：target(右) 相机坐标系 → 参考(左)相机坐标系 的旋转
    cv::Mat t;    // 3x1：平移（mm 或与实际同单位）
};

// ---------------- 立体校正输出 ----------------
struct IMAGE2DALG_EXPORT StereoRectify {
    cv::Mat R1, R2;                      // 左右各自的校正旋转
    cv::Mat P1, P2;                      // 校正后的投影矩阵 3x4
    cv::Mat Q;                           // 视差→深度重投影矩阵
    cv::Size rectSize;                   // 校正后图像尺寸
};

// ---------------- Mei 等距模型：投影 / 反投影 ----------------

/// 世界点（相机系）→ 像素坐标。返回 false 表示点在有效视场外。
IMAGE2DALG_EXPORT bool MeiProjectPoint(
    const double X[3], const CameraIntrinsics& K, double out_uv[2]);

/// 像素坐标 → 归一化方向（相机系单位向量，Mei 反投影）。
/// 迭代求解（Mei 模型反投影无闭式解，牛顿法 3~5 次收敛）。
IMAGE2DALG_EXPORT bool MeiUnprojectPixel(
    double u, double v, const CameraIntrinsics& K, double out_dir[3]);

/// 鱼眼去畸变：像素 → 矫正后像素（映射到理想针孔图像）。
IMAGE2DALG_EXPORT bool MeiUndistortPixel(
    double u, double v, const CameraIntrinsics& K, double out_uv[2]);

/// 针孔 → 鱼眼重投影（去畸变的逆映射，用于生成去畸变查表）。
IMAGE2DALG_EXPORT bool MeiDistortPixel(
    double u_undist, double v_undist, const CameraIntrinsics& K, double out_uv[2]);

// ---------------- 标定板：内角点世界坐标生成 ----------------

/// 生成标定板单个视图的内角点世界坐标（Z=0 平面，棋盘格方格边长 squareSize）。
/// [in] corners  内角点行列数（如 9x6 的棋盘 → 9,6）
IMAGE2DALG_EXPORT std::vector<cv::Point3f> CalibBoardWorldPoints(
    cv::Size corners, double squareSize);

// ---------------- 单目标定（张正友 + LM 精化） ----------------

/// 单目标定（针孔模型 + 畸变）。
/// [in] imagePoints   每张图的内角点像素坐标（>=3 张，每张 corners 数量一致）
/// [in] worldPoints   对应的世界坐标（每张一致，通常由 CalibBoardWorldPoints 生成）
/// [in] imageSize     图像尺寸
/// [out] K            内参 + 畸变
/// [out] rvecs/tvecs  每张图的外参（旋转向量 / 平移）
/// [out] outRms       重投影 RMS 误差（像素）
/// 返回 false = 角点数量不足或求解失败。
IMAGE2DALG_EXPORT bool CalibrateMono(
    const std::vector<std::vector<cv::Point2f>>& imagePoints,
    const std::vector<cv::Point3f>& worldPoints,
    cv::Size imageSize,
    CameraIntrinsics& K,
    std::vector<cv::Mat>& rvecs, std::vector<cv::Mat>& tvecs,
    double* outRms = nullptr);

// ---------------- 单目鱼眼标定（Mei 模型） ----------------

/// 鱼眼单目标定（Mei 等距模型）：先用针孔初值，再 LM 精化 xi + 畸变。
/// 输入要求同 CalibrateMono（鱼眼图上检测到的角点）。
IMAGE2DALG_EXPORT bool CalibrateFisheyeMei(
    const std::vector<std::vector<cv::Point2f>>& imagePoints,
    const std::vector<cv::Point3f>& worldPoints,
    cv::Size imageSize,
    CameraIntrinsics& K,
    double* outRms = nullptr);

// ---------------- 双目标定 + 立体校正 ----------------

/// 双目标定：左右各自单目标定 → 立体对齐 → Bouguet 立体校正。
/// [in] leftPoints/rightPoints   左右图像角点（张数一致、每张点数一致）
/// [out] KL/KR                   左右内参 + 畸变
/// [out] ext                     左→右外参
/// [out] rect                    立体校正输出（R1/R2/P1/P2/Q）
IMAGE2DALG_EXPORT bool CalibrateStereo(
    const std::vector<std::vector<cv::Point2f>>& leftPoints,
    const std::vector<std::vector<cv::Point2f>>& rightPoints,
    const std::vector<cv::Point3f>& worldPoints,
    cv::Size imageSize,
    CameraIntrinsics& KL, CameraIntrinsics& KR,
    StereoExtrinsics& ext, StereoRectify& rect,
    double* outRms = nullptr);

// ---------------- 标定结果持久化 ----------------

/// 保存为 key=value 文本（所有字段逐行写出，易读易解析）。
IMAGE2DALG_EXPORT bool CalibSave(const char* path, const CameraIntrinsics& K,
                                 const StereoExtrinsics* ext = nullptr,
                                 const StereoRectify* rect = nullptr);

/// 加载（ext/rect 为 nullptr 时只读内参；文件里缺对应段则保持默认）。
IMAGE2DALG_EXPORT bool CalibLoad(const char* path, CameraIntrinsics& K,
                                 StereoExtrinsics* ext = nullptr,
                                 StereoRectify* rect = nullptr);

} // namespace image2d

#endif // IMAGE2DALG_CALIB_H
