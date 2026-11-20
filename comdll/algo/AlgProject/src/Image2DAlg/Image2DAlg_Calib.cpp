// Image2DAlg_Calib.cpp —— 相机标定实现（手写 Mei 模型 + 张正友 + LM）。
// 数学库 Eigen（thrid/PCL-1.9.1/3rdParty/Eigen），图像容器 OpenCV（仅 Mat/Point）。
// 本文件不使用 cv::calibrateCamera —— 标定求解全部手写。

#include "include/Image2DAlg_Calib.h"
#include "CommonAlg/include/CommonAlg_FileUtil.h"

#include <Eigen/Dense>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace image2d {

namespace {

constexpr int kUnknown = -1;

// ---------------- 小工具 ----------------

cv::Mat IntrinsicsToK(const CameraIntrinsics& K)
{
    cv::Mat m = cv::Mat::zeros(3, 3, CV_64F);
    m.at<double>(0, 0) = K.fx;
    m.at<double>(1, 1) = K.fy;
    m.at<double>(0, 2) = K.cx;
    m.at<double>(1, 2) = K.cy;
    m.at<double>(2, 2) = 1.0;
    return m;
}

// Rodrigues 旋转向量 ↔ 3x3（手写，不依赖 calib3d）
cv::Mat RodriguesToMat(const cv::Mat& rvec)
{
    const double th = cv::norm(rvec);
    cv::Mat R = cv::Mat::eye(3, 3, CV_64F);
    if (th < 1e-12)
        return R;
    cv::Mat k = rvec / th;
    const double kx = k.at<double>(0), ky = k.at<double>(1), kz = k.at<double>(2);
    const double c = std::cos(th), s = std::sin(th), v = 1.0 - c;
    R.at<double>(0, 0) = c + kx * kx * v;
    R.at<double>(0, 1) = kx * ky * v - kz * s;
    R.at<double>(0, 2) = kx * kz * v + ky * s;
    R.at<double>(1, 0) = ky * kx * v + kz * s;
    R.at<double>(1, 1) = c + ky * ky * v;
    R.at<double>(1, 2) = ky * kz * v - kx * s;
    R.at<double>(2, 0) = kz * kx * v - ky * s;
    R.at<double>(2, 1) = kz * ky * v + kx * s;
    R.at<double>(2, 2) = c + kz * kz * v;
    return R;
}

cv::Mat MatToRodrigues(const cv::Mat& R)
{
    const double trace = R.at<double>(0, 0) + R.at<double>(1, 1) + R.at<double>(2, 2);
    double th = std::acos(std::max(-1.0, std::min(1.0, (trace - 1.0) * 0.5)));
    if (th < 1e-12)
        return cv::Mat::zeros(3, 1, CV_64F);
    // 轴 = (R - R^T) 的反对称部分
    cv::Mat axis(3, 1, CV_64F);
    axis.at<double>(0) = R.at<double>(2, 1) - R.at<double>(1, 2);
    axis.at<double>(1) = R.at<double>(0, 2) - R.at<double>(2, 0);
    axis.at<double>(2) = R.at<double>(1, 0) - R.at<double>(0, 1);
    axis /= (2.0 * std::sin(th));
    return axis * th;
}

// ---------------- 针孔投影（含畸变，LM 内联用纯数值版） ----------------

// 旋转向量 → 3x3 旋转矩阵（行主序 9 元素；LM 残差逐点调用，避免 cv::Mat 分配开销）
void RodriguesRaw(const double* rvec, double R[9])
{
    const double th = std::sqrt(rvec[0] * rvec[0] + rvec[1] * rvec[1] + rvec[2] * rvec[2]);
    if (th < 1e-12) {
        R[0] = 1; R[1] = 0; R[2] = 0;
        R[3] = 0; R[4] = 1; R[5] = 0;
        R[6] = 0; R[7] = 0; R[8] = 1;
        return;
    }
    const double kx = rvec[0] / th, ky = rvec[1] / th, kz = rvec[2] / th;
    const double c = std::cos(th), s = std::sin(th), v = 1.0 - c;
    R[0] = c + kx * kx * v; R[1] = kx * ky * v - kz * s; R[2] = kx * kz * v + ky * s;
    R[3] = ky * kx * v + kz * s; R[4] = c + ky * ky * v; R[5] = ky * kz * v - kx * s;
    R[6] = kz * kx * v - ky * s; R[7] = kz * ky * v + kx * s; R[8] = c + kz * kz * v;
}

// 针孔投影（含径向 k1/k2/k3 + 切向 p1/p2 畸变，双精度出口——float 截断会污染数值雅可比）。
// 返回 false = 点落到相机后方（z<=0），out_uv 填 (-1,-1) 罚值（与旧 ProjectPinhole 语义一致）。
bool PinholeProjectRaw(const double R[9], const double t[3],
                       double X, double Y, double Z,
                       double fx, double fy, double cx, double cy,
                       double k1, double k2, double p1, double p2, double k3,
                       double out_uv[2])
{
    const double xc = R[0] * X + R[1] * Y + R[2] * Z + t[0];
    const double yc = R[3] * X + R[4] * Y + R[5] * Z + t[1];
    const double zc = R[6] * X + R[7] * Y + R[8] * Z + t[2];
    if (zc <= 1e-9) {
        out_uv[0] = -1.0;
        out_uv[1] = -1.0;
        return false;
    }
    const double xn = xc / zc, yn = yc / zc;
    const double r2 = xn * xn + yn * yn;
    const double radial = 1.0 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2;
    const double xd = xn * radial + 2.0 * p1 * xn * yn + p2 * (r2 + 2.0 * xn * xn);
    const double yd = yn * radial + p1 * (r2 + 2.0 * yn * yn) + 2.0 * p2 * xn * yn;
    out_uv[0] = fx * xd + cx;
    out_uv[1] = fy * yd + cy;
    return true;
}

// ---------------- Mei 等距模型 ----------------

/// Mei 反投影核心：解 θ 使 sinθ/(ξ+cosθ) = target（target = 归一化像面半径 ρ/f）。
/// 等距半径单调（d/dθ = (ξcosθ+1)/(ξ+cosθ)² > 0，ξ≥0 时），一维牛顿稳定收敛。
/// 返回 false = denom（ξ+cosθ）落入奇异带或迭代后仍不满足方程。
bool MeiUnprojectTheta(double target, double xi, double& theta)
{
    // 退化：ξ≈0 时 Mei 退化为针孔，tanθ = target
    if (std::fabs(xi) < 1e-9) {
        theta = std::atan(target);
        return true;
    }
    theta = std::atan(target);   // 初值（ξ=0 解）
    for (int it = 0; it < 30; ++it) {
        const double st = std::sin(theta), ct = std::cos(theta);
        const double denom = xi + ct;
        if (std::fabs(denom) < 1e-12)
            return false;
        const double f = st / denom - target;
        const double df = (ct * denom + st * st) / (denom * denom);   // d/dθ
        const double dtheta = f / df;
        theta -= dtheta;
        if (std::fabs(dtheta) < 1e-12)
            return true;
    }
    return std::fabs(std::sin(theta) / (xi + std::cos(theta)) - target) < 1e-6;
}

} // namespace

// =====================================================================
// Mei 投影 / 反投影 / 去畸变
// =====================================================================

bool MeiProjectPoint(const double X[3], const CameraIntrinsics& K, double out_uv[2])
{
    // 世界点 → 镜面坐标系（Mei 假定相机系原点即镜面中心，此处直接用）
    const double x = X[0], y = X[1], z = X[2];
    const double norm = std::sqrt(x * x + y * y + z * z);
    if (norm < 1e-12)
        return false;

    // 球面投影
    const double sx = x / norm, sy = y / norm, sz = z / norm;

    // 等距/中心投影到平面：r = f * sinθ / (ξ + cosθ)
    // sinθ = √(sx²+sy²), cosθ = sz（要求 z>0 侧）
    // 各向异性广义焦距：u−cx = fx·sx/(ξ+cosθ)，v−cy = fy·sy/(ξ+cosθ)
    // （fx=fy 时与标准 Mei 单焦距公式逐项一致；旧实现 fy 从未参与，
    //  导致 CalibrateFisheyeMei 优化的 fy 是无约束参数——恒停在初值上）
    const double denom = K.xi + sz;
    if (std::fabs(denom) < 1e-12)
        return false;
    double xd = K.fx * sx / denom;
    double yd = K.fy * sy / denom;

    // Mei 广义畸变（通常很小，默认 0）
    const double r2 = xd * xd + yd * yd;
    const double dist = 1.0 + K.meiC * r2 + K.meiD * r2 * r2 + K.meiE * r2 * r2 * r2;
    xd *= dist;
    yd *= dist;

    out_uv[0] = K.cx + xd;
    out_uv[1] = K.cy + yd;
    return true;
}

bool MeiUnprojectPixel(double u, double v, const CameraIntrinsics& K, double out_dir[3])
{
    if (K.fx < 1e-12 || K.fy < 1e-12)
        return false;
    // 像素 → 归一化像平面（各轴除回各自焦距）：
    //   (u−cx)/fx = sx/(ξ+cosθ)，(v−cy)/fy = sy/(ξ+cosθ)
    const double xd = (u - K.cx) / K.fx;
    const double yd = (v - K.cy) / K.fy;
    const double rho0 = std::sqrt(xd * xd + yd * yd);   // = sinθ/(ξ+cosθ)

    // 解 θ：sinθ/(ξ+cosθ) = rho0
    double theta = 0.0;
    if (!MeiUnprojectTheta(rho0, K.xi, theta))
        return false;
    const double st = std::sin(theta), ct = std::cos(theta);
    const double denom = K.xi + ct;
    if (std::fabs(denom) < 1e-12 || std::fabs(st) < 1e-12)
        return false;

    // 由正向投影关系重建球面方向：sx = xd·denom，sy = yd·denom，sz = cosθ
    // （|sx,sy| = denom·rho0 = sinθ，与 sz=cosθ 构成单位向量）
    double sx = xd * denom;
    double sy = yd * denom;
    const double sz = ct;
    const double n = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (n < 1e-12)
        return false;
    out_dir[0] = sx / n;
    out_dir[1] = sy / n;
    out_dir[2] = sz / n;
    return true;
}

bool MeiUndistortPixel(double u, double v, const CameraIntrinsics& K, double out_uv[2])
{
    // 鱼眼像素 → 方向 → 理想针孔（xi=0 的等价针孔投影）
    double dir[3];
    if (!MeiUnprojectPixel(u, v, K, dir))
        return false;
    if (std::fabs(dir[2]) < 1e-12)
        return false;
    // Mei 的广义 f 直接当针孔焦距（ξ=0 时 Mei=针孔，f 相同）
    out_uv[0] = K.cx + K.fx * dir[0] / dir[2];
    out_uv[1] = K.cy + K.fy * dir[1] / dir[2];
    return true;
}

bool MeiDistortPixel(double u_undist, double v_undist, const CameraIntrinsics& K, double out_uv[2])
{
    // 理想针孔像素 → 方向 → Mei 像素
    if (std::fabs(v_undist - K.cy) < 1e-12 && std::fabs(u_undist - K.cx) < 1e-12) {
        out_uv[0] = K.cx; out_uv[1] = K.cy;
        return true;
    }
    const double xn = (u_undist - K.cx) / K.fx;
    const double yn = (v_undist - K.cy) / K.fy;
    const double norm = std::sqrt(xn * xn + yn * yn);
    if (norm < 1e-12)
        return false;
    // 针孔方向 → 球面 → Mei 投影（各向异性：u/v 轴分别用 fx/fy，与 MeiProjectPoint 一致）
    const double X = xn, Y = yn, Z = 1.0;
    const double nrm = std::sqrt(X * X + Y * Y + Z * Z);
    const double sx = X / nrm, sy = Y / nrm, sz = Z / nrm;
    const double denom = K.xi + sz;
    if (std::fabs(denom) < 1e-12)
        return false;
    double xd = K.fx * sx / denom, yd = K.fy * sy / denom;
    const double r2 = xd * xd + yd * yd;
    const double dist = 1.0 + K.meiC * r2 + K.meiD * r2 * r2 + K.meiE * r2 * r2 * r2;
    xd *= dist; yd *= dist;
    out_uv[0] = K.cx + xd;
    out_uv[1] = K.cy + yd;
    return true;
}

// =====================================================================
// 标定板
// =====================================================================

std::vector<cv::Point3f> CalibBoardWorldPoints(cv::Size corners, double squareSize)
{
    std::vector<cv::Point3f> out;
    out.reserve((size_t)corners.width * corners.height);
    for (int y = 0; y < corners.height; ++y)
        for (int x = 0; x < corners.width; ++x)
            out.push_back(cv::Point3f((float)(x * squareSize), (float)(y * squareSize), 0.0f));
    return out;
}

// =====================================================================
// 内部：LM 优化框架（通用残差最小二乘）
// =====================================================================

namespace {

// LM 优化框架：residualFn(params) → 残差向量（未平方），
// cost = ||r||²，H = J^T J（含参数耦合），标准 LM 步长。
using ResidualFn = std::function<Eigen::VectorXd(const Eigen::VectorXd&)>;
using ClampFn = std::function<void(Eigen::VectorXd&)>;

// LM 优化框架。clamp（可选）：每步试验点先过一遍参数盒约束（如 ξ∈[0,1)、f>0），
// 防止无约束参数在简并方向上发散（曾出现 ξ 被优化到 -0.84）。
bool SolveLM(ResidualFn fn, Eigen::VectorXd& params, int maxIter = 200, double lambda = 1e-3,
             ClampFn clamp = nullptr)
{
    if (params.size() == 0)
        return false;
    if (clamp)
        clamp(params);
    Eigen::VectorXd r0 = fn(params);
    double cost = r0.squaredNorm();
    double nu = 2.0;
    for (int it = 0; it < maxIter; ++it) {
        const int n = (int)params.size();
        const int m = (int)r0.size();
        // 数值雅可比（残差向量，中心差分）
        Eigen::MatrixXd J(m, n);
        for (int j = 0; j < n; ++j) {
            const double h = std::max(1e-8, std::fabs(params[j]) * 1e-5);
            Eigen::VectorXd pp = params, pm = params;
            pp[j] += h; pm[j] -= h;
            J.col(j) = (fn(pp) - fn(pm)) / (2.0 * h);
        }
        // LM 步：δ = -(J^T J + λ·diag(J^T J))^-1 · J^T r
        Eigen::MatrixXd JTJ = J.transpose() * J;
        const Eigen::VectorXd g = J.transpose() * r0;
        Eigen::VectorXd D = JTJ.diagonal();
        for (int j = 0; j < n; ++j)
            D[j] = std::max(D[j], 1e-9);
        Eigen::MatrixXd A = JTJ;
        A.diagonal() += lambda * D;
        const Eigen::VectorXd step = A.ldlt().solve(-g);
        if (!step.allFinite())
            break;

        Eigen::VectorXd trial = params + step;
        if (clamp)
            clamp(trial);
        Eigen::VectorXd rt = fn(trial);
        const double trialCost = rt.squaredNorm();
        if (!std::isfinite(trialCost))
            break;
        // Nielsen 增益比（Madsen-Nielsen-Tingleff）：ρ = 实际下降/预测下降。
        // 预测下降 predRed = −½δᵀg（由正规方程 (JᵀJ+λD)δ=−g 两边乘 δᵀ 导出）。
        // 旧实现 λ 固定 ×0.3/×5 步调，早期大步沿规范谷直冲伪吸引盆
        // （fx 曾冲到 1.2 万）；增益比让坏步快速升 λ、好步温和降 λ。
        const double predRed = -0.5 * step.dot(g);
        const double rho = (predRed > 1e-300) ? (cost - trialCost) / predRed : -1.0;
        if (rho > 0.0 && trialCost < cost) {
            const double improve = cost - trialCost;
            params = trial;
            r0 = rt;
            cost = trialCost;
            lambda = std::max(lambda * std::max(1.0 / 3.0, 1.0 - std::pow(2.0 * rho - 1.0, 3.0)), 1e-10);
            nu = 2.0;
            if (improve < 1e-10 * std::max(1.0, cost) || step.norm() < 1e-12)
                break;
        } else {
            lambda *= nu;
            nu *= 2.0;
            if (lambda > 1e14)
                break;
        }
    }
    std::fprintf(stderr, "  [LM end] cost=%.3e\n", cost);
    return true;
}

bool HomographyDLT(const std::vector<cv::Point3f>& world,
                   const std::vector<cv::Point2f>& image, cv::Mat& H)
{
    const size_t n = world.size();
    if (n < 4 || image.size() != n)
        return false;

    // Hartley 归一化（各向同性平移+缩放：质心移原点、平均距离 √2）。
    // 窄贴片/远离原点的点集不做归一化时 A 矩阵病态，SVD 精度损失直接进 H
    // （曾致 Zhang 初值 fx 系统性偏差 −10%，进而把线性畸变初值挤爆到钳制值）。
    auto normalizePts = [](const std::vector<cv::Point2f>& pts, cv::Mat& T,
                           std::vector<cv::Point2f>& out) {
        cv::Point2d c(0.0, 0.0);
        for (const auto& p : pts) {
            c.x += p.x;
            c.y += p.y;
        }
        c.x /= (double)pts.size();
        c.y /= (double)pts.size();
        double d = 0.0;
        for (const auto& p : pts)
            d += std::sqrt((p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y));
        d = std::sqrt(2.0) / std::max(d / (double)pts.size(), 1e-12);
        T = (cv::Mat_<double>(3, 3) << d, 0, -d * c.x, 0, d, -d * c.y, 0, 0, 1);
        out.resize(pts.size());
        for (size_t i = 0; i < pts.size(); ++i)
            out[i] = cv::Point2f((float)(d * (pts[i].x - c.x)), (float)(d * (pts[i].y - c.y)));
    };
    cv::Mat Tw, Ti;
    std::vector<cv::Point2f> worldN, imgN;
    {
        std::vector<cv::Point2f> world2d(n);
        for (size_t i = 0; i < n; ++i)
            world2d[i] = cv::Point2f(world[i].x, world[i].y);   // Z=0 平面
        normalizePts(world2d, Tw, worldN);
        normalizePts(image, Ti, imgN);
    }

    Eigen::MatrixXd A(2 * n, 9);
    for (size_t i = 0; i < n; ++i) {
        const double X = worldN[i].x, Y = worldN[i].y, u = imgN[i].x, v = imgN[i].y;
        A(2 * i, 0) = X; A(2 * i, 1) = Y; A(2 * i, 2) = 1;
        A(2 * i, 3) = 0; A(2 * i, 4) = 0; A(2 * i, 5) = 0;
        A(2 * i, 6) = -u * X; A(2 * i, 7) = -u * Y; A(2 * i, 8) = -u;
        A(2 * i + 1, 0) = 0; A(2 * i + 1, 1) = 0; A(2 * i + 1, 2) = 0;
        A(2 * i + 1, 3) = X; A(2 * i + 1, 4) = Y; A(2 * i + 1, 5) = 1;
        A(2 * i + 1, 6) = -v * X; A(2 * i + 1, 7) = -v * Y; A(2 * i + 1, 8) = -v;
    }
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeFullV);
    const Eigen::VectorXd h = svd.matrixV().col(8);
    if (h.size() != 9)
        return false;
    cv::Mat Hn = cv::Mat::zeros(3, 3, CV_64F);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            Hn.at<double>(r, c) = h[r * 3 + c];
    // 反归一化：H = Ti⁻¹ · Hn · Tw
    H = Ti.inv() * Hn * Tw;
    // 归一化
    const double scale = H.at<double>(2, 2);
    if (std::fabs(scale) < 1e-12)
        return false;
    H /= scale;
    return true;
}

// Zhang（张正友）闭式内参初值：由各视图单应性 H 求解 B = λ·K^{-T}K^{-1}（假设零 skew）。
// cx/cy 钉在图像中心（OpenCV initCameraMatrix2D 同款实践——B 阵反推的 cx/cy
// 在近正对板数据上极不稳定，曾漂到距真值 −90px；中心先验作为初值足够，
// 后续 LM 会自由精化 cx/cy），只解 4 个未知量 [B11 B12 B22 B33]
// （B13=−cx0·B11、B23=−cy0·B22 由 K^{-T}K^{-1} 结构代入）。
// 旧实现没有这一步——LM 从"fx=图宽 + |t|=板对角线×5"的粗起点出发，
// 极易落入 fx↔k1/k2 简并谷（曾收敛到 fx≈5387、k1≈0.77 的伪解）。
// [in]  Hs        每图 DLT 单应（已归一化 H[2,2]=1，≥3 张）
// [in]  imageSize 用于中心先验 cx0/cy0
// [out] K         回填 fx/fy/cx/cy（畸变字段不动）
bool ZhangInitIntrinsics(const std::vector<cv::Mat>& Hs, cv::Size imageSize, CameraIntrinsics& K)
{
    if (Hs.size() < 3)
        return false;
    const double cx0 = imageSize.width * 0.5;
    const double cy0 = imageSize.height * 0.5;
    Eigen::MatrixXd V(2 * Hs.size(), 4);
    int row = 0;
    for (const auto& H : Hs) {
        const double h1[3] = { H.at<double>(0, 0), H.at<double>(1, 0), H.at<double>(2, 0) };
        const double h2[3] = { H.at<double>(0, 1), H.at<double>(1, 1), H.at<double>(2, 1) };
        // v12 行：h1^T B h2 = 0（B13/B23 已用结构关系代入）
        V(row, 0) = h1[0] * h2[0] - cx0 * (h1[2] * h2[0] + h1[0] * h2[2]);
        V(row, 1) = h1[0] * h2[1] + h1[1] * h2[0];
        V(row, 2) = h1[1] * h2[1] - cy0 * (h1[2] * h2[1] + h1[1] * h2[2]);
        V(row, 3) = h1[2] * h2[2];
        ++row;
        // (v11 − v22) 行：h1^T B h1 = h2^T B h2
        // 注意必须用约束"差"——B 正定使 h1^T B h1 严格大于 0，单独堆叠 v11
        // （h1^T B h1 = 0）是无解系统，SVD 只会给出无意义方向（本函数首版踩过的坑）
        V(row, 0) = h1[0] * h1[0] - h2[0] * h2[0] - 2.0 * cx0 * (h1[2] * h1[0] - h2[2] * h2[0]);
        V(row, 1) = 2.0 * (h1[0] * h1[1] - h2[0] * h2[1]);
        V(row, 2) = h1[1] * h1[1] - h2[1] * h2[1] - 2.0 * cy0 * (h1[2] * h1[1] - h2[2] * h2[1]);
        V(row, 3) = h1[2] * h1[2] - h2[2] * h2[2];
        ++row;
    }
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(V, Eigen::ComputeFullV);
    Eigen::VectorXd b = svd.matrixV().col(3);   // 最小奇异值对应的右奇异向量
    if (b.size() != 4 || !b.allFinite())
        return false;
    if (b[0] < 0.0)
        b = -b;                                 // 统一到 B11>0 分支
    // 归一化到 B11=1：b 是任意尺度的方向向量，K 提取公式对 b 整体缩放不变，
    // 守卫必须用尺度无关量。（首版用绝对阈值 w<1e-12——而正常相机
    // w=λ²/(fx²fy²)~1e-13，把完全合法的解拒掉了，Zhang 永远"失败"）
    const double bnorm = b.norm();
    if (!std::isfinite(bnorm) || bnorm < 1e-15 || b[0] < 1e-12 * bnorm)
        return false;
    b /= b[0];
    const double B12 = b[1], B22 = b[2], B33 = b[3];
    const double w = B22 - B12 * B12;           // = (B11·B22 − B12²)/B11²，B11=1
    if (w < 1e-9)
        return false;
    const double lam = B33 - cx0 * cx0 - cy0 * cy0 * B22;   // λ/B11
    if (lam < 1e-9)
        return false;
    const double fx = std::sqrt(lam);           // sqrt(λ/B11)，B11=1
    const double fy = std::sqrt(lam / w);       // sqrt(λ·B11/(B11·B22−B12²)) 的归一化形式
    if (!std::isfinite(fx) || !std::isfinite(fy))
        return false;
    if (fx < 1e-6 || fy < 1e-6)
        return false;
    K.fx = fx; K.fy = fy; K.cx = cx0; K.cy = cy0;
    return true;
}

// 单应 → 该视图外参初值（正确的度量尺度恢复）。
// 关键点：H 归一化到 H[2,2]=1 后只剩方向，量纲信息在 t_z 里——|K⁻¹h1| = 1/|t_z|，
// 因此 t = K⁻¹h3/|K⁻¹h1| 直接恢复毫米量纲（旧实现把 |t| 拍到"板对角线×5"，量纲全错）。
// [in] probe  板面代表点（用于剔除 t_z<0 的镜像分支：板必须落在相机前方）
bool ExtrinsicsFromH(const cv::Mat& H, const CameraIntrinsics& K,
                     const cv::Point3f& probe, cv::Mat& R, cv::Mat& t)
{
    const cv::Mat Km = IntrinsicsToK(K);
    const cv::Mat Kinv = Km.inv();
    const cv::Mat r1v = Kinv * H.col(0);
    const cv::Mat r2v = Kinv * H.col(1);
    const cv::Mat tv = Kinv * H.col(2);
    const double s = cv::norm(r1v);   // = 1/|t_z|
    if (s < 1e-12)
        return false;
    cv::Mat c0 = r1v / s, c1 = r2v / s;
    cv::Mat tvec = tv / s;
    if (c1.dot(c0) < 0.0)
        c1 = -c1;                     // Gram-Schmidt 前对齐方向（避免近垂直时翻转）
    cv::Mat c1o = c1 - (c1.dot(c0)) * c0;
    const double n1 = cv::norm(c1o);
    if (n1 < 1e-12)
        return false;
    c1o /= n1;
    cv::Mat c2 = c0.cross(c1o);       // r3 = r1×r2，det 恒 +1
    R = cv::Mat::zeros(3, 3, CV_64F);
    c0.copyTo(R.col(0));
    c1o.copyTo(R.col(1));
    c2.copyTo(R.col(2));
    t = tvec.clone();
    // 镜像分支剔除：t_z<0 时 r1/t_z、t/t_z 同被翻转成 (−r1,−t)，用"板在前方"检验识别并翻回
    const double zprobe = c2.at<double>(0) * probe.x + c2.at<double>(1) * probe.y +
                          c2.at<double>(2) * probe.z + tvec.at<double>(2);
    if (zprobe < 0.0) {
        R.col(0) = -R.col(0);
        R.col(1) = -R.col(1);
        t = -tvec;
    }
    return true;
}

// 畸变系数线性初值（Zhang 配方第 3 步）已移除：fx 初值有偏差时，该线性最小
// 二乘会把径向尺度误差吸收进 k1/k2（曾钳制到 ±2 的垃圾值），反而把 LM 引入
// 缩放世界伪吸引谷。改为依赖 LM 自身的阻尼下降（OpenCV 同款管线）。

} // namespace

// =====================================================================
// 单目标定
// =====================================================================

bool CalibrateMono(
    const std::vector<std::vector<cv::Point2f>>& imagePoints,
    const std::vector<cv::Point3f>& worldPoints,
    cv::Size imageSize,
    CameraIntrinsics& K,
    std::vector<cv::Mat>& rvecs, std::vector<cv::Mat>& tvecs,
    double* outRms)
{
    rvecs.clear(); tvecs.clear();
    if (outRms) *outRms = -1.0;
    if (imagePoints.size() < 3 || worldPoints.empty())
        return false;
    const size_t nPer = worldPoints.size();
    for (const auto& ip : imagePoints)
        if (ip.size() != nPer)
            return false;

    // ---- 1. 逐图 DLT 单应（内参初值与外参初值共用）----
    std::vector<cv::Mat> Hs;
    for (size_t i = 0; i < imagePoints.size(); ++i) {
        cv::Mat H;
        if (!HomographyDLT(worldPoints, imagePoints[i], H))
            return false;
        Hs.push_back(H);
    }

    // ---- 2. 内参初值：Zhang 闭式解（cx/cy 钉图像中心，只解 fx/fy）----
    //      旧实现 fx=图宽 / cx=图宽一半 的粗起点 + 错误量纲的 t，
    //      把 LM 拖进 fx↔k1/k2 简并谷（fx 冲到数千、k1/k2 起飞也能"收敛"）。
    //      Zhang 失败时退回旧启发式；畸变初值一律清零由 LM 学出
    //     （避免与内参耦合规范不确定性）。
    CameraIntrinsics work = K;
    work.imageSize = imageSize;
    work.k1 = work.k2 = work.p1 = work.p2 = 0.0;
    if (!ZhangInitIntrinsics(Hs, imageSize, work)) {
        if (work.fx <= 0) work.fx = imageSize.width;
        if (work.fy <= 0) work.fy = imageSize.width;
        if (work.cx <= 0) work.cx = imageSize.width * 0.5;
        if (work.cy <= 0) work.cy = imageSize.height * 0.5;
    }

    // ---- 3. 多候选 LM ----
    //      Zhang 闭式解的 fx 在近正对板数据上噪声可达 ±25~40%（OpenCV 的
    //      initCameraMatrix2D 同样如此），而全参数 LM 从偏差起点出发会沿
    //      (fx↔t_z) 规范浅脊缓慢爬行，常在 400 次迭代内停在同脊中途的
    //      局部极小（cost 达噪声底 300 倍，且爬行方向随噪声实现可上可下）。
    //      实测起点接近真值时收敛又快又稳——故按 Zhang 值缩放生成多个
    //      fx/fy 候选，各自"阶段A(仅外参)→短程全参数 LM"，按最终重投影
    //      成本择优（噪声底 cost≈1 与山脊停点 cost≥270 判别余量巨大），
    //      最后对最优候选做一次长程全参数精化。
    const int nViews = (int)imagePoints.size();
    const int nIntr = 8;
    const int nParams = nIntr + nViews * 6;

    cv::Point3f probe(0.0f, 0.0f, 0.0f);   // 板面代表点（质心）
    for (const auto& wp : worldPoints) {
        probe.x += wp.x; probe.y += wp.y; probe.z += wp.z;
    }
    {
        const float nW = (float)worldPoints.size();
        probe.x /= nW; probe.y /= nW; probe.z /= nW;
    }

    auto makeResidual = [&](const CameraIntrinsics* fixedIntr) {
        return [&, fixedIntr](const Eigen::VectorXd& p) -> Eigen::VectorXd {
            const CameraIntrinsics& kk = fixedIntr ? *fixedIntr : work;
            double fx = kk.fx, fy = kk.fy, cx = kk.cx, cy = kk.cy;
            double k1 = kk.k1, k2 = kk.k2, p1 = kk.p1, p2 = kk.p2, k3 = kk.k3;
            if (!fixedIntr) {
                fx = p[0]; fy = p[1]; cx = p[2]; cy = p[3];
                k1 = p[4]; k2 = p[5]; p1 = p[6]; p2 = p[7];
            }
            const int extOff = fixedIntr ? 0 : nIntr;
            Eigen::VectorXd r(nViews * (int)nPer * 2);
            int k = 0;
            for (int i = 0; i < nViews; ++i) {
                double R[9];
                RodriguesRaw(&p[extOff + i * 6], R);   // 每视图一次（旧实现逐点重算）
                const double* tv = &p[extOff + i * 6 + 3];
                for (size_t j = 0; j < nPer; ++j) {
                    double uv[2];
                    PinholeProjectRaw(R, tv, worldPoints[j].x, worldPoints[j].y, worldPoints[j].z,
                                      fx, fy, cx, cy, k1, k2, p1, p2, k3, uv);
                    r[k++] = uv[0] - imagePoints[i][j].x;
                    r[k++] = uv[1] - imagePoints[i][j].y;
                }
            }
            return r;
        };
    };

    // 物理盒约束（数值安全网，正常解不会触边）：fx/fy 限在 Zhang 值的
    // [0.4, 2.5] 倍——曾出现 fx 冲到 Zhang 值 15 倍、k2 冲到 2.6 万的
    // 缩放世界伪解；cx/cy 限在图像 ±25%；畸变 |k|≤2、|p|≤0.5。
    auto clampMono = [&work, &imageSize](Eigen::VectorXd& p) {
        p[0] = std::max(0.4 * work.fx, std::min(2.5 * work.fx, p[0]));
        p[1] = std::max(0.4 * work.fy, std::min(2.5 * work.fy, p[1]));
        p[2] = std::max(0.25 * imageSize.width, std::min(0.75 * imageSize.width, p[2]));
        p[3] = std::max(0.25 * imageSize.height, std::min(0.75 * imageSize.height, p[3]));
        p[4] = std::max(-2.0, std::min(2.0, p[4]));
        p[5] = std::max(-2.0, std::min(2.0, p[5]));
        p[6] = std::max(-0.5, std::min(0.5, p[6]));
        p[7] = std::max(-0.5, std::min(0.5, p[7]));
    };

    struct MonoTry {
        Eigen::VectorXd params;
        double cost = 0.0;
    };
    auto runMonoCandidate = [&](const CameraIntrinsics& k0, MonoTry& out) -> bool {
        Eigen::VectorXd p(nParams);
        p[0] = k0.fx; p[1] = k0.fy; p[2] = k0.cx; p[3] = k0.cy;
        p[4] = 0.0; p[5] = 0.0; p[6] = 0.0; p[7] = 0.0;
        for (int i = 0; i < nViews; ++i) {
            cv::Mat R, t;
            // 逐图外参初值：单应分解 + 度量尺度恢复（t = K⁻¹h3/|K⁻¹h1|）
            if (!ExtrinsicsFromH(Hs[i], k0, probe, R, t))
                return false;
            const cv::Mat rv = MatToRodrigues(R);
            for (int j = 0; j < 3; ++j) {
                p[nIntr + i * 6 + j] = rv.at<double>(j);
                p[nIntr + i * 6 + 3 + j] = t.at<double>(j);
            }
        }
        {   // 阶段 A：固定内参只收外参（先归位外参，再放内参，防止早期大步
            // 同时挪动外参与 f/c 冲进缩放伪盆）
            Eigen::VectorXd extOnly(nViews * 6);
            for (int i = 0; i < nViews; ++i)
                for (int j = 0; j < 6; ++j)
                    extOnly[i * 6 + j] = p[nIntr + i * 6 + j];
            SolveLM(makeResidual(&k0), extOnly, 20);
            for (int i = 0; i < nViews; ++i)
                for (int j = 0; j < 6; ++j)
                    p[nIntr + i * 6 + j] = extOnly[i * 6 + j];
        }
        SolveLM(makeResidual(nullptr), p, 250, 1e-3, clampMono);   // 候选短跑
        out.params = p;
        out.cost = makeResidual(nullptr)(p).squaredNorm();
        return true;
    };

    std::vector<MonoTry> tries;
    {
        const double scales[] = { 0.65, 0.75, 0.9, 1.1, 1.3 };
        for (double s : scales) {
            CameraIntrinsics k0 = work;
            k0.fx *= s;
            k0.fy *= s;
            MonoTry tr;
            if (runMonoCandidate(k0, tr))
                tries.push_back(tr);
        }
    }
    if (K.fx > 0) {   // 调用方指定初值候选（fx/fy/cx/cy 取调用方，畸变仍从 0 学起）
        CameraIntrinsics k0 = work;
        k0.fx = K.fx; k0.fy = K.fy;
        k0.cx = K.cx; k0.cy = K.cy;
        k0.k1 = k0.k2 = k0.p1 = k0.p2 = 0.0;
        MonoTry tr;
        if (runMonoCandidate(k0, tr))
            tries.push_back(tr);
    }
    if (tries.empty())
        return false;
    const MonoTry* best = &tries[0];
    for (const MonoTry& tr : tries)
        if (tr.cost < best->cost)
            best = &tr;
    Eigen::VectorXd params = best->params;

    // 最终精化：最优候选继续长程收敛
    SolveLM(makeResidual(nullptr), params, 800, 1e-3, clampMono);

    // ---- 5. 回写 ----
    K = work;
    K.imageSize = imageSize;
    K.fx = params[0]; K.fy = params[1];
    K.cx = params[2]; K.cy = params[3];
    K.k1 = params[4]; K.k2 = params[5];
    K.p1 = params[6]; K.p2 = params[7];

    rvecs.clear(); tvecs.clear();
    double sum2 = 0.0;
    int cnt = 0;
    for (int i = 0; i < nViews; ++i) {
        const double* rv = params.data() + nIntr + i * 6;
        const double* tv = params.data() + nIntr + i * 6 + 3;
        cv::Mat rvec = cv::Mat::zeros(3, 1, CV_64F);
        cv::Mat tvec = cv::Mat::zeros(3, 1, CV_64F);
        for (int j = 0; j < 3; ++j) {
            rvec.at<double>(j) = rv[j];
            tvec.at<double>(j) = tv[j];
        }
        rvecs.push_back(rvec);
        tvecs.push_back(tvec);
        double R[9];
        RodriguesRaw(rv, R);
        for (size_t j = 0; j < nPer; ++j) {
            double uv[2];
            PinholeProjectRaw(R, tv, worldPoints[j].x, worldPoints[j].y, worldPoints[j].z,
                              K.fx, K.fy, K.cx, K.cy, K.k1, K.k2, K.p1, K.p2, K.k3, uv);
            const double dx = uv[0] - imagePoints[i][j].x;
            const double dy = uv[1] - imagePoints[i][j].y;
            sum2 += dx * dx + dy * dy;
            ++cnt;
        }
    }
    if (outRms)
        *outRms = cnt ? std::sqrt(sum2 / cnt) : -1.0;
    return true;
}


bool CalibrateFisheyeMei(
    const std::vector<std::vector<cv::Point2f>>& imagePoints,
    const std::vector<cv::Point3f>& worldPoints,
    cv::Size imageSize,
    CameraIntrinsics& K,
    double* outRms)
{
    if (imagePoints.size() < 3 || worldPoints.empty())
        return false;
    const size_t nPer = worldPoints.size();

    // 1. 逐图 DLT 单应（鱼眼图点的近似针孔化：H 是对实际投影的最小二乘拟合，作初值足够）
    std::vector<cv::Mat> Hs;
    for (size_t i = 0; i < imagePoints.size(); ++i) {
        cv::Mat H;
        if (!HomographyDLT(worldPoints, imagePoints[i], H))
            return false;
        Hs.push_back(H);
    }

    // 2. 候选内参初值（多起点）：Mei 的 (f,ξ) 存在近似简并谷，单起点 LM 可能
    //    滑向 ξ→1 边界的伪驻点——多起点分别求解，取最终重投影成本更低者。
    //    候选 1：外部指定（fx>0 时，沿用调用方初值语义）；
    //    候选 2：Zhang 闭式解 + 度量修正——针孔拟合关系 f_p = f·cosθ̄/(ξ+cosθ̄)
    //    （鱼眼中心区近似针孔，f_p 系统性偏小 ~(1+ξ) 倍），反推 f = f_p·(ξ0+cosθ̄)/cosθ̄。
    //    旧实现 fx=图宽、|t|=板对角线×5 的粗起点曾让 LM 收敛到 fx≈1037 的伪解。
    const int nViews = (int)imagePoints.size();
    const int nIntr = 5;   // fx fy cx cy xi
    const int nParams = nIntr + nViews * 6;

    cv::Point3f probe(0.0f, 0.0f, 0.0f);   // 板面质心（同 CalibrateMono）
    for (const auto& wp : worldPoints) {
        probe.x += wp.x; probe.y += wp.y; probe.z += wp.z;
    }
    {
        const float nW = (float)worldPoints.size();
        probe.x /= nW; probe.y /= nW; probe.z /= nW;
    }

    // 残差：fx fy cx cy xi 来自参数向量；mei 广义畸变沿用调用方 K（通常 0）
    CameraIntrinsics base = K;
    base.imageSize = imageSize;
    auto residualFn = [&](const Eigen::VectorXd& p) -> Eigen::VectorXd {
        CameraIntrinsics kk = base;
        kk.fx = p[0]; kk.fy = p[1];
        kk.cx = p[2]; kk.cy = p[3];
        kk.xi = p[4];
        Eigen::VectorXd r(nViews * (int)nPer * 2);
        int k = 0;
        for (int i = 0; i < nViews; ++i) {
            double R[9];
            RodriguesRaw(&p[nIntr + i * 6], R);   // 每视图一次（旧实现逐点重算）
            const double* tv = &p[nIntr + i * 6 + 3];
            for (size_t j = 0; j < nPer; ++j) {
                const double X = worldPoints[j].x, Y = worldPoints[j].y, Z = 0.0;
                const double xc = R[0] * X + R[1] * Y + R[2] * Z + tv[0];
                const double yc = R[3] * X + R[4] * Y + R[5] * Z + tv[1];
                const double zc = R[6] * X + R[7] * Y + R[8] * Z + tv[2];
                const double w[3] = { xc, yc, zc };
                double uv[2];
                if (MeiProjectPoint(w, kk, uv)) {
                    r[k++] = uv[0] - imagePoints[i][j].x;
                    r[k++] = uv[1] - imagePoints[i][j].y;
                } else {
                    r[k++] = 1e4;   // 视场外罚项（大残差而非 1e6 平方爆炸）
                    r[k++] = 1e4;
                }
            }
        }
        return r;
    };

    // ξ 盒约束 [0, 0.999]、f>0（无约束时曾优化到 ξ=-0.84 的非物理区）
    auto clampMei = [](Eigen::VectorXd& p) {
        p[4] = std::max(0.0, std::min(0.999, p[4]));
        p[0] = std::max(p[0], 1e-3);
        p[1] = std::max(p[1], 1e-3);
    };

    struct MeiTry {
        Eigen::VectorXd params;
        double cost = 0.0;
    };
    auto runCandidate = [&](double fx0, double fy0, double cx0, double cy0, double xi0,
                            MeiTry& out) -> bool {
        CameraIntrinsics k0;
        k0.imageSize = imageSize;
        k0.fx = std::max(fx0, 1e-3); k0.fy = std::max(fy0, 1e-3);
        k0.cx = cx0; k0.cy = cy0; k0.xi = xi0;
        Eigen::VectorXd p(nParams);
        p[0] = k0.fx; p[1] = k0.fy;
        p[2] = cx0; p[3] = cy0;
        p[4] = std::max(0.0, std::min(0.999, xi0));
        for (int i = 0; i < nViews; ++i) {
            cv::Mat R, t;
            if (!ExtrinsicsFromH(Hs[i], k0, probe, R, t))
                return false;
            const cv::Mat rv = MatToRodrigues(R);
            for (int j = 0; j < 3; ++j) {
                p[nIntr + i * 6 + j] = rv.at<double>(j);
                p[nIntr + i * 6 + 3 + j] = t.at<double>(j);
            }
        }
        SolveLM(residualFn, p, 400, 1e-3, clampMei);
        out.params = p;
        out.cost = residualFn(p).squaredNorm();
        return true;
    };

    std::vector<MeiTry> tries;
    MeiTry t1;
    if (K.fx > 0 && runCandidate(K.fx, K.fy, K.cx, K.cy,
                                 (K.xi > 0 && K.xi < 1.0) ? K.xi : 0.5, t1))
        tries.push_back(t1);

    CameraIntrinsics kz;
    kz.imageSize = imageSize;
    if (ZhangInitIntrinsics(Hs, imageSize, kz)) {
        // 平均入射角余弦（kz 外参统计）→ f 度量修正
        std::vector<cv::Mat> Rz, tz;
        bool extOk = true;
        for (size_t i = 0; i < Hs.size() && extOk; ++i) {
            cv::Mat R, t;
            if (!ExtrinsicsFromH(Hs[i], kz, probe, R, t))
                extOk = false;
            else {
                Rz.push_back(R);
                tz.push_back(t);
            }
        }
        if (extOk) {
            double cosSum = 0.0;
            int nTh = 0;
            for (size_t i = 0; i < Rz.size(); ++i) {
                const cv::Mat& R = Rz[i];
                const cv::Mat& tv = tz[i];
                for (size_t j = 0; j < nPer; ++j) {
                    const double X = worldPoints[j].x, Y = worldPoints[j].y, Z = 0.0;
                    const double xc = R.at<double>(0, 0) * X + R.at<double>(0, 1) * Y + tv.at<double>(0);
                    const double yc = R.at<double>(1, 0) * X + R.at<double>(1, 1) * Y + tv.at<double>(1);
                    const double zc = R.at<double>(2, 0) * X + R.at<double>(2, 1) * Y + tv.at<double>(2);
                    const double d = std::sqrt(xc * xc + yc * yc + zc * zc);
                    if (d > 1e-9 && zc > 0.0) {
                        cosSum += zc / d;
                        ++nTh;
                    }
                }
            }
            if (nTh > 0) {
                const double cosT = cosSum / nTh;
                const double corr = (0.5 + cosT) / cosT;   // f = f_p·(ξ0+cosθ̄)/cosθ̄
                MeiTry t2;
                if (runCandidate(kz.fx * corr, kz.fy * corr, kz.cx, kz.cy, 0.5, t2))
                    tries.push_back(t2);
            }
        }
    }
    if (tries.empty()) {
        // 无外部初值且 Zhang 退化：退回旧启发式（图宽量级）
        MeiTry t3;
        runCandidate((double)imageSize.width, (double)imageSize.width,
                     imageSize.width * 0.5, imageSize.height * 0.5, 0.5, t3);
        tries.push_back(t3);
    }
    const MeiTry* best = &tries[0];
    for (const auto& tr : tries)
        if (tr.cost < best->cost)
            best = &tr;
    Eigen::VectorXd params = best->params;

    K.imageSize = imageSize;
    K.fx = params[0]; K.fy = params[1];
    K.cx = params[2]; K.cy = params[3];
    K.xi = params[4];

    // 重投影 RMS
    if (outRms) {
        double sum2 = 0.0;
        int cnt = 0;
        bool ok = true;
        for (int i = 0; i < nViews && ok; ++i) {
            cv::Mat rvec = cv::Mat::zeros(3, 1, CV_64F);
            cv::Mat tvec = cv::Mat::zeros(3, 1, CV_64F);
            for (int j = 0; j < 3; ++j) {
                rvec.at<double>(j) = params[nIntr + i * 6 + j];
                tvec.at<double>(j) = params[nIntr + i * 6 + 3 + j];
            }
            cv::Mat R = RodriguesToMat(rvec);
            for (size_t j = 0; j < nPer; ++j) {
                const double X = worldPoints[j].x, Y = worldPoints[j].y;
                const double xc = R.at<double>(0, 0) * X + R.at<double>(0, 1) * Y + tvec.at<double>(0);
                const double yc = R.at<double>(1, 0) * X + R.at<double>(1, 1) * Y + tvec.at<double>(1);
                const double zc = R.at<double>(2, 0) * X + R.at<double>(2, 1) * Y + tvec.at<double>(2);
                const double w[3] = { xc, yc, zc };
                double uv[2];
                if (!MeiProjectPoint(w, K, uv)) { ok = false; break; }
                const double dx = uv[0] - imagePoints[i][j].x;
                const double dy = uv[1] - imagePoints[i][j].y;
                sum2 += dx * dx + dy * dy;
                ++cnt;
            }
        }
        *outRms = (ok && cnt) ? std::sqrt(sum2 / cnt) : -1.0;
    }
    return true;
}

// =====================================================================
// 双目标定 + 立体校正
// =====================================================================

bool CalibrateStereo(
    const std::vector<std::vector<cv::Point2f>>& leftPoints,
    const std::vector<std::vector<cv::Point2f>>& rightPoints,
    const std::vector<cv::Point3f>& worldPoints,
    cv::Size imageSize,
    CameraIntrinsics& KL, CameraIntrinsics& KR,
    StereoExtrinsics& ext, StereoRectify& rect,
    double* outRms)
{
    if (leftPoints.size() < 3 || leftPoints.size() != rightPoints.size())
        return false;

    // 1. 左右各自单目标定
    std::vector<cv::Mat> rl, tl, rr, tr;
    double rmsL = -1, rmsR = -1;
    CameraIntrinsics kl0 = KL, kr0 = KR;
    if (!CalibrateMono(leftPoints, worldPoints, imageSize, kl0, rl, tl, &rmsL))
        return false;
    if (!CalibrateMono(rightPoints, worldPoints, imageSize, kr0, rr, tr, &rmsR))
        return false;
    KL = kl0; KR = kr0;

    // 2. 立体对齐：逐图 T_l→板、T_r→板 → 右系在左系下的位姿 = T_r^-1 * T_l（板→左 = 板→右 · 右→左）
    //    R = Rr * Rl^T, t = tr - R * tl（板在左右各自相机系下的位姿组合）
    //    多帧平均（用第一帧也够，这里取平均更稳）
    cv::Mat Racc = cv::Mat::zeros(3, 3, CV_64F);
    cv::Mat tacc = cv::Mat::zeros(3, 1, CV_64F);
    for (size_t i = 0; i < leftPoints.size(); ++i) {
        const cv::Mat Rl = RodriguesToMat(rl[i]);
        const cv::Mat Rr = RodriguesToMat(rr[i]);
        // 右→左：R_rel = Rl * Rr^T ; t_rel = tl - R_rel * tr
        const cv::Mat Rrel = Rl * Rr.t();
        const cv::Mat trel = tl[i] - Rrel * tr[i];
        Racc += Rrel;
        tacc += trel;
    }
    Racc /= (double)leftPoints.size();
    tacc /= (double)leftPoints.size();

    ext.R = Racc.clone();
    ext.t = tacc.clone();

    if (outRms)
        *outRms = (rmsL + rmsR) * 0.5;

    // 3. Bouguet 立体校正
    const cv::Mat r1 = MatToRodrigues(rl[0]);
    (void)r1;
    // 分解 R 为两个半旋转 R1, R2（Bouguet）：r = R1·R2，各转一半
    cv::Mat R = ext.R;
    cv::Mat rvec = MatToRodrigues(R);
    const double theta = cv::norm(rvec);
    cv::Mat raxis = rvec / theta;
    const cv::Mat R1 = MatToRodrigues(raxis * (theta * 0.5));
    const cv::Mat R2 = MatToRodrigues(raxis * (-theta * 0.5));
    rect.R1 = R1;
    rect.R2 = R2;
    rect.rectSize = imageSize;

    // P1/P2：校正后投影矩阵（基线放在 P2[0][3] = -fx*tx）
    const cv::Mat P1 = IntrinsicsToK(KL) * cv::Mat::eye(3, 4, CV_64F) * 1.0;
    cv::Mat P1m = cv::Mat::zeros(3, 4, CV_64F);
    IntrinsicsToK(KL).copyTo(P1m.colRange(0, 3));
    cv::Mat P2m = cv::Mat::zeros(3, 4, CV_64F);
    IntrinsicsToK(KR).copyTo(P2m.colRange(0, 3));
    P2m.at<double>(0, 3) = -KR.fx * ext.t.at<double>(0);   // 基线项
    rect.P1 = P1m;
    rect.P2 = P2m;

    // Q：视差 → 深度
    cv::Mat Q = cv::Mat::zeros(4, 4, CV_64F);
    Q.at<double>(0, 0) = 1.0;
    Q.at<double>(1, 1) = 1.0;
    Q.at<double>(3, 2) = -1.0 / KL.fx;                  // 1/fx
    Q.at<double>(2, 3) = KL.cx;                          // 占位
    Q.at<double>(3, 3) = KL.cx;                          // 标准形式含 cx/cx' 差
    Q.at<double>(2, 3) = 0.0;
    Q.at<double>(3, 3) = 0.0;
    // 标准 Q：[1 0 0 -cx; 0 1 0 -cy; 0 0 0 f; 0 0 -1/Tx (cx-cx')/Tx]
    Q = cv::Mat::zeros(4, 4, CV_64F);
    Q.at<double>(0, 0) = 1.0;
    Q.at<double>(1, 1) = 1.0;
    Q.at<double>(0, 3) = -KL.cx;
    Q.at<double>(1, 3) = -KL.cy;
    Q.at<double>(2, 3) = KL.fx;
    Q.at<double>(3, 2) = -1.0 / (KR.fx * std::fabs(ext.t.at<double>(0)) / KR.fx);   // = -1/Tx
    Q.at<double>(3, 2) = -1.0 / std::max(1e-9, std::fabs(ext.t.at<double>(0)));
    Q.at<double>(3, 3) = 0.0;
    rect.Q = Q;
    return true;
}

// =====================================================================
// 持久化
// =====================================================================

bool CalibSave(const char* path, const CameraIntrinsics& K,
               const StereoExtrinsics* ext, const StereoRectify* rect)
{
    if (!path || !path[0])
        return false;
    char buf[2048];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
        "# CameraCalib v1 (key=value)\n"
        "model=pinhole+dist\n"
        "image_width=%d\nimage_height=%d\n"
        "fx=%.10f\nfy=%.10f\ncx=%.10f\ncy=%.10f\n"
        "k1=%.12f\nk2=%.12f\np1=%.12f\np2=%.12f\nk3=%.12f\n"
        "xi=%.10f\nmei_c=%.10f\nmei_d=%.10f\nmei_e=%.10f\n",
        K.imageSize.width, K.imageSize.height,
        K.fx, K.fy, K.cx, K.cy,
        K.k1, K.k2, K.p1, K.p2, K.k3,
        K.xi, K.meiC, K.meiD, K.meiE);
    if (ext) {
        for (int r = 0; r < 3; ++r)
            n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                          "R%d=%.10f %.10f %.10f\n", r,
                          ext->R.at<double>(r, 0), ext->R.at<double>(r, 1), ext->R.at<double>(r, 2));
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "t=%.10f %.10f %.10f\n",
                      ext->t.at<double>(0), ext->t.at<double>(1), ext->t.at<double>(2));
    }
    if (rect) {
        for (int r = 0; r < 3; ++r)
            n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                          "P1_%d=%.10f %.10f %.10f %.10f\n", r,
                          rect->P1.at<double>(r, 0), rect->P1.at<double>(r, 1),
                          rect->P1.at<double>(r, 2), rect->P1.at<double>(r, 3));
        for (int r = 0; r < 3; ++r)
            n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                          "P2_%d=%.10f %.10f %.10f %.10f\n", r,
                          rect->P2.at<double>(r, 0), rect->P2.at<double>(r, 1),
                          rect->P2.at<double>(r, 2), rect->P2.at<double>(r, 3));
        for (int r = 0; r < 4; ++r)
            n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                          "Q_%d=%.10f %.10f %.10f %.10f\n", r,
                          rect->Q.at<double>(r, 0), rect->Q.at<double>(r, 1),
                          rect->Q.at<double>(r, 2), rect->Q.at<double>(r, 3));
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "rect_width=%d\nrect_height=%d\n",
                      rect->rectSize.width, rect->rectSize.height);
    }
    return CommonAlg_WriteFile(path, std::string(buf, (size_t)n));
}

bool CalibLoad(const char* path, CameraIntrinsics& K,
               StereoExtrinsics* ext, StereoRectify* rect)
{
    if (!path || !path[0])
        return false;
    std::string text;
    if (!CommonAlg_ReadFile(path, text))
        return false;

    auto parseVec = [](const std::string& v, cv::Mat& m, int r) {
        double a[4] = { 0, 0, 0, 0 };
        if (sscanf(v.c_str(), "%lf %lf %lf %lf", &a[0], &a[1], &a[2], &a[3]) >= 1)
            for (int c = 0; c < 4 && c < m.cols; ++c)
                m.at<double>(r, c) = a[c];
    };

    K = CameraIntrinsics();
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, (eol == std::string::npos ? text.size() : eol) - pos);
        pos = (eol == std::string::npos) ? text.size() : eol + 1;
        if (line.empty() || line[0] == '#')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);

        if      (key == "image_width")  K.imageSize.width = atoi(val.c_str());
        else if (key == "image_height") K.imageSize.height = atoi(val.c_str());
        else if (key == "fx") K.fx = atof(val.c_str());
        else if (key == "fy") K.fy = atof(val.c_str());
        else if (key == "cx") K.cx = atof(val.c_str());
        else if (key == "cy") K.cy = atof(val.c_str());
        else if (key == "k1") K.k1 = atof(val.c_str());
        else if (key == "k2") K.k2 = atof(val.c_str());
        else if (key == "p1") K.p1 = atof(val.c_str());
        else if (key == "p2") K.p2 = atof(val.c_str());
        else if (key == "k3") K.k3 = atof(val.c_str());
        else if (key == "xi") K.xi = atof(val.c_str());
        else if (key == "mei_c") K.meiC = atof(val.c_str());
        else if (key == "mei_d") K.meiD = atof(val.c_str());
        else if (key == "mei_e") K.meiE = atof(val.c_str());
        else if (ext) {
            if (key == "R0" || key == "R1" || key == "R2") {
                if (ext->R.empty()) ext->R = cv::Mat::zeros(3, 3, CV_64F);
                parseVec(val, ext->R, key[1] - '0');
            } else if (key == "t") {
                if (ext->t.empty()) ext->t = cv::Mat::zeros(3, 1, CV_64F);
                double a[3] = { 0, 0, 0 };
                if (sscanf(val.c_str(), "%lf %lf %lf", &a[0], &a[1], &a[2]) == 3)
                    for (int r = 0; r < 3; ++r)
                        ext->t.at<double>(r) = a[r];
            }
        }
        if (rect) {
            if (key.rfind("P1_", 0) == 0) {
                if (rect->P1.empty()) rect->P1 = cv::Mat::zeros(3, 4, CV_64F);
                parseVec(val, rect->P1, atoi(key.c_str() + 3));
            } else if (key.rfind("P2_", 0) == 0) {
                if (rect->P2.empty()) rect->P2 = cv::Mat::zeros(3, 4, CV_64F);
                parseVec(val, rect->P2, atoi(key.c_str() + 3));
            } else if (key.rfind("Q_", 0) == 0) {
                if (rect->Q.empty()) rect->Q = cv::Mat::zeros(4, 4, CV_64F);
                parseVec(val, rect->Q, atoi(key.c_str() + 2));
            } else if (key == "rect_width") rect->rectSize.width = atoi(val.c_str());
            else if (key == "rect_height") rect->rectSize.height = atoi(val.c_str());
        }
    }
    return K.fx > 0 && K.fy > 0;
}

} // namespace image2d
