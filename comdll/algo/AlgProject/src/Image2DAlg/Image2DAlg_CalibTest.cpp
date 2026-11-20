// Image2DAlg_CalibTest.cpp —— 相机标定验证（模拟数据 + 真值对照）。
// 核心验证思路：用已知真值参数正向投影生成"观测角点"，再标定求解，
// 对比求解参数与真值——这能端到端验证整条标定数学链。

#include "include/Image2DAlg_Calib.h"

#include <opencv2/core.hpp>
#include <opencv2/calib3d.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_fail;
}

static cv::Mat MakeRVec(double rx, double ry, double rz)
{
    cv::Mat v(3, 1, CV_64F);
    v.at<double>(0) = rx;
    v.at<double>(1) = ry;
    v.at<double>(2) = rz;
    return v;
}

static cv::Mat MakeTVec(double x, double y, double z)
{
    cv::Mat v(3, 1, CV_64F);
    v.at<double>(0) = x;
    v.at<double>(1) = y;
    v.at<double>(2) = z;
    return v;
}

// ---------------- 单目标定 ----------------
static void testMono()
{
    std::printf("== 单目标定（针孔 + 畸变）==\n");
    const cv::Size imgSize(1280, 960);
    const cv::Size corners(9, 6);
    // 方格边长 25mm：板面 200×125mm，覆盖到像场半径 r≈0.57。
    // 板若只占像场中心窄贴片（r≤0.24），(fx,k1,k2) 径向多项式在该区间上
    // 本质简并——连 OpenCV calibrateCamera 都只能骑在似然谷里（fx 偏差
    // 可达 +6~16px），±5px 级断言统计上不可满足；板面覆盖越宽越可辨识。
    const double square = 25.0;   // mm

    // 真值参数
    image2d::CameraIntrinsics truth;
    truth.imageSize = imgSize;
    truth.fx = 800.0; truth.fy = 805.0;
    truth.cx = 640.0; truth.cy = 480.0;
    truth.k1 = -0.15; truth.k2 = 0.02;
    truth.p1 = 0.001; truth.p2 = -0.002;
    truth.k3 = 0.0;

    const std::vector<cv::Point3f> world = image2d::CalibBoardWorldPoints(corners, square);

    // 生成 10 个模拟视图（不同的位姿），投影 + 加 0.05px 噪声
    std::vector<std::vector<cv::Point2f>> views;
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> noise(-0.05, 0.05);
    for (int v = 0; v < 12; ++v) {
        const double ang = -0.3 + 0.06 * v;
        cv::Mat rvec = MakeRVec(0.02 * (v - 5), 0.03 * v, ang);
        cv::Mat tvec = MakeTVec(-100.0 + 20.0 * v, -80.0 + 15.0 * v, 350.0 + 30.0 * v);
        std::vector<cv::Point2f> obs;
        for (const auto& wp : world) {
            // 手动正向投影（与 CalibrateMono 内部一致的模型）
            cv::Mat R;
            cv::Rodrigues(rvec, R);   // 这里可以用 OpenCV 的 Rodrigues（测试代码可用 calib3d）
            const double X = wp.x, Y = wp.y, Z = wp.z;
            const double xc = R.at<double>(0, 0) * X + R.at<double>(0, 1) * Y + R.at<double>(0, 2) * Z + tvec.at<double>(0);
            const double yc = R.at<double>(1, 0) * X + R.at<double>(1, 1) * Y + R.at<double>(1, 2) * Z + tvec.at<double>(1);
            const double zc = R.at<double>(2, 0) * X + R.at<double>(2, 1) * Y + R.at<double>(2, 2) * Z + tvec.at<double>(2);
            const double xn = xc / zc, yn = yc / zc;
            const double r2 = xn * xn + yn * yn;
            const double rad = 1.0 + truth.k1 * r2 + truth.k2 * r2 * r2;
            const double xd = xn * rad + 2 * truth.p1 * xn * yn + truth.p2 * (r2 + 2 * xn * xn);
            const double yd = yn * rad + truth.p1 * (r2 + 2 * yn * yn) + 2 * truth.p2 * xn * yn;
            obs.push_back(cv::Point2f(
                (float)(truth.fx * xd + truth.cx + noise(rng)),
                (float)(truth.fy * yd + truth.cy + noise(rng))));
        }
        views.push_back(obs);
    }

    // 标定
    image2d::CameraIntrinsics result;
    result.fx = 900; result.fy = 900;           // 故意给偏的初值
    result.cx = 600; result.cy = 500;
    std::vector<cv::Mat> rvecs, tvecs;
    double rms = -1;
    const bool ok = image2d::CalibrateMono(views, world, imgSize, result, rvecs, tvecs, &rms);
    check(ok, "CalibrateMono 求解成功");
    check(rms >= 0 && rms < 0.3, "重投影 RMS < 0.3px（模拟噪声 0.05px）");

    auto near = [](double a, double b, double tol) { return std::fabs(a - b) < tol; };
    check(near(result.fx, truth.fx, 5.0), "fx 收敛（真值 800）");
    check(near(result.fy, truth.fy, 5.0), "fy 收敛（真值 805）");
    check(near(result.cx, truth.cx, 5.0), "cx 收敛（真值 640）");
    check(near(result.cy, truth.cy, 5.0), "cy 收敛（真值 480）");
    check(near(result.k1, truth.k1, 0.05), "k1 收敛（真值 -0.15）");
    check(near(result.k2, truth.k2, 0.05), "k2 收敛（真值 0.02）");

    // 回归（工单003）：外参量纲恢复——t_z 应接近模拟真值（首图 t=(-100,-80,350)）。
    // 修复前 t 被拍到"板对角线×5"的任意尺度，LM 伪收敛后外参完全失真。
    check(!tvecs.empty() && near(tvecs[0].at<double>(2), 350.0, 40.0),
          "外参 t_z 恢复模拟量纲（首图真值 350）");

    std::printf("    求解: fx=%.2f fy=%.2f cx=%.2f cy=%.2f k1=%.4f k2=%.4f rms=%.4f\n",
                result.fx, result.fy, result.cx, result.cy, result.k1, result.k2, rms);
}

// ---------------- 鱼眼 Mei 标定 ----------------
static void testFisheye()
{
    std::printf("== 鱼眼 Mei 标定 ==\n");
    const cv::Size imgSize(1280, 960);
    const cv::Size corners(9, 6);
    const double square = 10.0;

    // 真值（鱼眼：xi=0.6，无针孔畸变，Mei 广义畸变 0）
    image2d::CameraIntrinsics truth;
    truth.imageSize = imgSize;
    truth.fx = 350.0; truth.fy = 352.0;
    truth.cx = 640.0; truth.cy = 480.0;
    truth.xi = 0.6;
    truth.meiC = 0.0; truth.meiD = 0.0; truth.meiE = 0.0;

    const std::vector<cv::Point3f> world = image2d::CalibBoardWorldPoints(corners, square);

    // 生成模拟视图：鱼眼投影（视角较大，靠近边缘）
    std::vector<std::vector<cv::Point2f>> views;
    std::mt19937 rng(54321);
    std::uniform_real_distribution<double> noise(-0.05, 0.05);
    for (int v = 0; v < 8; ++v) {
        cv::Mat rvec = MakeRVec(0.05 * v, -0.04 * v, 0.1 * v);
        cv::Mat tvec = MakeTVec(-60.0 + 10.0 * v, -50.0 + 8.0 * v, 200.0 + 20.0 * v);
        cv::Mat R;
        cv::Rodrigues(rvec, R);
        std::vector<cv::Point2f> obs;
        for (const auto& wp : world) {
            const double X = wp.x, Y = wp.y, Z = wp.z;
            const double xc = R.at<double>(0, 0) * X + R.at<double>(0, 1) * Y + R.at<double>(0, 2) * Z + tvec.at<double>(0);
            const double yc = R.at<double>(1, 0) * X + R.at<double>(1, 1) * Y + R.at<double>(1, 2) * Z + tvec.at<double>(1);
            const double zc = R.at<double>(2, 0) * X + R.at<double>(2, 1) * Y + R.at<double>(2, 2) * Z + tvec.at<double>(2);
            const double w[3] = { xc, yc, zc };
            double uv[2];
            if (!image2d::MeiProjectPoint(w, truth, uv)) {
                std::printf("    [警告] 视图 %d 点 %zu 在视场外\n", v, obs.size());
                continue;
            }
            obs.push_back(cv::Point2f(
                (float)(uv[0] + noise(rng)),
                (float)(uv[1] + noise(rng))));
        }
        if ((int)obs.size() == corners.width * corners.height)
            views.push_back(obs);
    }
    check(views.size() >= 3, "模拟鱼眼视图生成（有效视场内）");

    // 标定（给偏的初值）
    image2d::CameraIntrinsics result;
    result.fx = 500; result.fy = 500;
    result.cx = 600; result.cy = 500;
    result.xi = 0.2;
    double rms = -1;
    const bool ok = image2d::CalibrateFisheyeMei(views, world, imgSize, result, &rms);
    check(ok, "CalibrateFisheyeMei 求解成功");
    if (!ok)
        return;
    check(rms >= 0 && rms < 0.5, "重投影 RMS < 0.5px");

    auto near = [](double a, double b, double tol) { return std::fabs(a - b) < tol; };
    check(near(result.fx, truth.fx, 15.0), "fx 收敛（真值 350）");
    check(near(result.fy, truth.fy, 15.0), "fy 收敛（真值 352）");
    check(near(result.cx, truth.cx, 10.0), "cx 收敛（真值 640）");
    check(near(result.cy, truth.cy, 10.0), "cy 收敛（真值 480）");
    check(near(result.xi, truth.xi, 0.15), "xi 收敛（真值 0.6）");

    // 投影/反投影往返一致性
    {
        const double w[3] = { 3.0, 4.0, 10.0 };
        double uv[2], dir[3], back[2];
        check(image2d::MeiProjectPoint(w, result, uv), "MeiProjectPoint");
        check(image2d::MeiUnprojectPixel(uv[0], uv[1], result, dir), "MeiUnprojectPixel");
        // 方向应与原始归一化方向一致
        const double n0 = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        check(std::fabs(dir[0] - w[0] / n0) < 1e-6 &&
              std::fabs(dir[1] - w[1] / n0) < 1e-6 &&
              std::fabs(dir[2] - w[2] / n0) < 1e-6, "反投影方向一致");
        // 去畸变 → 重畸变 往返
        check(image2d::MeiUndistortPixel(uv[0], uv[1], result, back), "MeiUndistortPixel");
        double re[2];
        check(image2d::MeiDistortPixel(back[0], back[1], result, re), "MeiDistortPixel");
        check(std::fabs(re[0] - uv[0]) < 1e-6 && std::fabs(re[1] - uv[1]) < 1e-6,
              "鱼眼去畸变往返一致");
    }

    // 回归（工单003）：Mei 投影 fy 各向异性参与——fy≠fx 时 v 轴必须用 fy。
    // 修复前 MeiProjectPoint 只用 fx，CalibrateFisheyeMei 优化的 fy 是无约束
    // 参数（雅可比恒零），永远停在初值上。
    {
        image2d::CameraIntrinsics Ka;
        Ka.imageSize = imgSize;
        Ka.fx = 350.0; Ka.fy = 500.0;   // 故意 fy≠fx
        Ka.cx = 640.0; Ka.cy = 480.0;
        Ka.xi = 0.6;
        const double w2[3] = { 3.0, 4.0, 10.0 };
        const double nw = std::sqrt(w2[0] * w2[0] + w2[1] * w2[1] + w2[2] * w2[2]);
        const double denom = Ka.xi + w2[2] / nw;          // ξ + cosθ
        const double eu = Ka.fx * (w2[0] / nw) / denom;   // u−cx 期望（fx·sx/denom）
        const double ev = Ka.fy * (w2[1] / nw) / denom;   // v−cy 期望（fy·sy/denom）
        double uv2[2];
        check(image2d::MeiProjectPoint(w2, Ka, uv2), "MeiProjectPoint fy≠fx");
        check(std::fabs(uv2[0] - (Ka.cx + eu)) < 1e-9 &&
              std::fabs(uv2[1] - (Ka.cy + ev)) < 1e-9, "各向异性投影：u/v 轴分别用 fx/fy");
        double dir2[3];
        check(image2d::MeiUnprojectPixel(uv2[0], uv2[1], Ka, dir2), "MeiUnprojectPixel fy≠fx");
        check(std::fabs(dir2[0] - w2[0] / nw) < 1e-9 &&
              std::fabs(dir2[1] - w2[1] / nw) < 1e-9 &&
              std::fabs(dir2[2] - w2[2] / nw) < 1e-9, "各向异性反投影方向一致");
    }

    std::printf("    求解: fx=%.2f fy=%.2f cx=%.2f cy=%.2f xi=%.4f rms=%.4f\n",
                result.fx, result.fy, result.cx, result.cy, result.xi, rms);
}

// ---------------- 双目标定 ----------------
static void testStereo()
{
    std::printf("== 双目标定 + 立体校正 ==\n");
    const cv::Size imgSize(1280, 960);
    const cv::Size corners(9, 6);
    // 方格边长 25mm：与单目测试同理，板面须覆盖足够像场半径，
    // (fx,k1) 径向简并才被透视线索打破（窄中央贴片连 OpenCV 都无法辨识 fx）
    const double square = 25.0;

    // 左右相机有固定的相对位姿（平行光轴 + 已知基线）
    const double baseline = 120.0;   // mm
    image2d::CameraIntrinsics truthL, truthR;
    truthL.imageSize = imgSize;
    truthL.fx = 800; truthL.fy = 800; truthL.cx = 640; truthL.cy = 480;
    truthR = truthL;
    truthR.cx = 620;

    const std::vector<cv::Point3f> world = image2d::CalibBoardWorldPoints(corners, square);

    // 模拟：同一批位姿（板→世界），左右相机分别投影
    // 右相机 = 左相机沿相机系 X 平移 −baseline（刚体基线，平行光轴）
    // 位姿/视角数与单目测试对齐（12 视图、倾斜 ±15° 级、距离 350~680）：
    // 板面近乎正对时 (fx,t_z,k1) 存在近似连续规范谷——曾实测 8 视图弱倾斜数据
    // 上真值 rms(0.0421) 反而高于谷内伪解(0.0410)，连 OpenCV calibrateCamera
    // 都只能收敛到 fx 偏 +6~16px，±5px 断言在统计上不可满足；倾斜提供的
    // 透视线索是数据可辨识的前提。
    std::vector<std::vector<cv::Point2f>> leftViews, rightViews;
    std::mt19937 rng(999);
    std::uniform_real_distribution<double> noise(-0.05, 0.05);
    for (int v = 0; v < 12; ++v) {
        cv::Mat rvec = MakeRVec(0.02 * (v - 5), 0.03 * v, -0.3 + 0.06 * v);
        cv::Mat tvec = MakeTVec(-100.0 + 20.0 * v, -80.0 + 15.0 * v, 350.0 + 30.0 * v);
        cv::Mat R;
        cv::Rodrigues(rvec, R);

        std::vector<cv::Point2f> ol, orr;
        for (const auto& wp : world) {
            // 左相机（在世界系原点，姿态 = rvec/tvec）
            {
                const double X = wp.x, Y = wp.y, Z = wp.z;
                const double xc = R.at<double>(0, 0) * X + R.at<double>(0, 1) * Y + R.at<double>(0, 2) * Z + tvec.at<double>(0);
                const double yc = R.at<double>(1, 0) * X + R.at<double>(1, 1) * Y + R.at<double>(1, 2) * Z + tvec.at<double>(1);
                const double zc = R.at<double>(2, 0) * X + R.at<double>(2, 1) * Y + R.at<double>(2, 2) * Z + tvec.at<double>(2);
                const double xn = xc / zc, yn = yc / zc;
                const double r2 = xn * xn + yn * yn;
                const double rad = 1.0 - 0.1 * r2;
                ol.push_back(cv::Point2f(
                    (float)(truthL.fx * (xn * rad) + truthL.cx + noise(rng)),
                    (float)(truthL.fy * (yn * rad) + truthL.cy + noise(rng))));
            }
            // 右相机 = 左相机沿相机系 X 平移 −baseline（刚体基线，平行光轴）：
            //   x_r = R·X + t − (b,0,0)，相对位姿恒为 R_rel=I、t_rel=(b,0,0)。
            //   （旧写法把 wp.x−baseline 代入 R 后 xc 又减 baseline——基线被计两次，
            //   附加位姿相关项 R·(b,0,0)，等效基线≈2b 且随位姿漂移，与
            //   "固定相对位姿 + 基线 120" 的断言自相矛盾，任何正确实现都无法满足）
            {
                const double X = wp.x, Y = wp.y, Z = wp.z;
                const double xc = R.at<double>(0, 0) * X + R.at<double>(0, 1) * Y + R.at<double>(0, 2) * Z + tvec.at<double>(0) - baseline;
                const double yc = R.at<double>(1, 0) * X + R.at<double>(1, 1) * Y + R.at<double>(1, 2) * Z + tvec.at<double>(1);
                const double zc = R.at<double>(2, 0) * X + R.at<double>(2, 1) * Y + R.at<double>(2, 2) * Z + tvec.at<double>(2);
                const double xn = xc / zc, yn = yc / zc;
                const double r2 = xn * xn + yn * yn;
                const double rad = 1.0 - 0.1 * r2;
                orr.push_back(cv::Point2f(
                    (float)(truthR.fx * (xn * rad) + truthR.cx + noise(rng)),
                    (float)(truthR.fy * (yn * rad) + truthR.cy + noise(rng))));
            }
        }
        leftViews.push_back(ol);
        rightViews.push_back(orr);
    }

    image2d::CameraIntrinsics KL, KR;
    image2d::StereoExtrinsics ext;
    image2d::StereoRectify rect;
    double rms = -1;
    const bool ok = image2d::CalibrateStereo(leftViews, rightViews, world, imgSize,
                                             KL, KR, ext, rect, &rms);
    check(ok, "CalibrateStereo 求解成功");
    if (!ok)
        return;
    check(rms >= 0 && rms < 0.3, "重投影 RMS < 0.3px");

    auto near = [](double a, double b, double tol) { return std::fabs(a - b) < tol; };
    check(near(KL.fx, truthL.fx, 5.0), "左 fx 收敛");
    check(near(KR.fx, truthR.fx, 5.0), "右 fx 收敛");
    // 基线：t 的 X 分量应接近 -baseline（右相机在左相机系下的位置）
    std::printf("    外参 t = (%.2f, %.2f, %.2f)  基线真值 %.0f\n",
                ext.t.at<double>(0), ext.t.at<double>(1), ext.t.at<double>(2), baseline);
    check(std::fabs(ext.t.at<double>(0)) > baseline * 0.9 &&
          std::fabs(ext.t.at<double>(0)) < baseline * 1.1, "基线恢复");
    check(near(ext.t.at<double>(1), 0.0, 3.0) &&
          near(ext.t.at<double>(2), 0.0, 3.0), "基线沿 X 轴（Y/Z≈0）");
    check(!rect.R1.empty() && !rect.R2.empty() && !rect.P1.empty() &&
          !rect.P2.empty() && !rect.Q.empty(), "立体校正输出齐全");
}

// ---------------- 持久化 ----------------
static void testPersist()
{
    std::printf("== 标定结果持久化 ==\n");
    image2d::CameraIntrinsics K;
    K.imageSize = cv::Size(1920, 1080);
    K.fx = 1234.5; K.fy = 1230.2;
    K.cx = 960.1; K.cy = 540.8;
    K.k1 = -0.2; K.k2 = 0.03; K.p1 = 0.001; K.p2 = -0.002; K.k3 = 0.004;
    K.xi = 0.75; K.meiC = 0.01; K.meiD = -0.002; K.meiE = 0.0003;

    const char* path = "calib_test_result.txt";
    check(image2d::CalibSave(path, K), "CalibSave");

    image2d::CameraIntrinsics loaded;
    check(image2d::CalibLoad(path, loaded), "CalibLoad");
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
    check(loaded.imageSize == K.imageSize, "imageSize 往返");
    check(near(loaded.fx, K.fx) && near(loaded.fy, K.fy), "fx/fy 往返");
    check(near(loaded.cx, K.cx) && near(loaded.cy, K.cy), "cx/cy 往返");
    check(near(loaded.k1, K.k1) && near(loaded.k3, K.k3), "畸变系数往返");
    check(near(loaded.xi, K.xi) && near(loaded.meiC, K.meiC) &&
          near(loaded.meiD, K.meiD) && near(loaded.meiE, K.meiE), "Mei 参数往返");

    // 带双目段
    image2d::StereoExtrinsics ext;
    ext.R = cv::Mat::eye(3, 3, CV_64F);
    ext.t = cv::Mat::zeros(3, 1, CV_64F);
    ext.t.at<double>(0) = -120.0;
    image2d::StereoRectify rect;
    rect.P1 = cv::Mat::zeros(3, 4, CV_64F);
    rect.P2 = cv::Mat::zeros(3, 4, CV_64F);
    rect.Q = cv::Mat::eye(4, 4, CV_64F);
    rect.rectSize = cv::Size(1280, 960);
    check(image2d::CalibSave(path, K, &ext, &rect), "CalibSave 带双目");
    image2d::CameraIntrinsics K2;
    image2d::StereoExtrinsics ext2;
    image2d::StereoRectify rect2;
    check(image2d::CalibLoad(path, K2, &ext2, &rect2), "CalibLoad 带双目");
    check(std::fabs(ext2.t.at<double>(0) - (-120.0)) < 1e-6, "外参 t 往返");
    check(std::fabs(rect2.Q.at<double>(0, 0) - 1.0) < 1e-9, "Q 往返");
    std::remove(path);
}

int main()
{
    testMono();
    testFisheye();
    testStereo();
    testPersist();
    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
