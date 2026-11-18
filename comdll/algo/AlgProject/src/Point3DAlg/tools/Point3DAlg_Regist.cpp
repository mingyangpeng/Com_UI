// Point3DAlg_Regist.cpp —— 配准算法层实现（纯函数，零 UI）。
// 底层 ICP 家族走 ShapelyAlg::CloudProcessor（已封装 PCL 配准），
// PCA 粗配准在此自行实现（PCL 1.9 无现成的 PCA 配准）。

#include "Point3DAlg_Regist.h"

#include "ShapelyAlg.h"                       // 基础算子库（CloudProcessor）
#include "CommonAlg/include/CommonAlg_FileUtil.h"   // 文件读写

#include <pcl/common/transforms.h>
#include <pcl/registration/gicp.h>
#include <pcl/registration/ndt.h>
#include <pcl/registration/ia_ransac.h>
#include <pcl/features/fpfh.h>
#include <pcl/features/normal_3d.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/filters/voxel_grid.h>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <sstream>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// 单位阵
void SetIdentity(float T[16])
{
    for (int i = 0; i < 16; ++i)
        T[i] = 0.0f;
    T[0] = T[5] = T[10] = T[15] = 1.0f;
}

// 4x4 行主序 → Eigen（PCL 变换用）
Eigen::Matrix4f ToEigen(const float T[16])
{
    Eigen::Matrix4f m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m(r, c) = T[r * 4 + c];
    return m;
}

void FromEigen(const Eigen::Matrix4f& m, float T[16])
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            T[r * 4 + c] = m(r, c);
}

// 轴号归一化：与日志显示保持一致（日志也用这个轴号）
int NormalizeAxis(int axis)
{
    if (axis < 0 || axis > 2)
        return 2;   // 非法值按 Z 处理，与日志一致
    return axis;
}

// 绕单个世界轴的旋转矩阵（右手系，角度为度）
Eigen::Matrix4f RotationAxis(int axis, double angle_deg)
{
    const double a = angle_deg * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    Eigen::Matrix4f m = Eigen::Matrix4f::Identity();
    switch (NormalizeAxis(axis)) {
    case 0:   // X
        m(1, 1) = (float)c; m(1, 2) = (float)-s;
        m(2, 1) = (float)s; m(2, 2) = (float)c;
        break;
    case 1:   // Y
        m(0, 0) = (float)c; m(0, 2) = (float)s;
        m(2, 0) = (float)-s; m(2, 2) = (float)c;
        break;
    default:  // Z
        m(0, 0) = (float)c; m(0, 1) = (float)-s;
        m(1, 0) = (float)s; m(1, 1) = (float)c;
        break;
    }
    return m;
}

// 把新变换写入 next 并更新迭代计数
void Commit(Point3DAlg_RegistStep& step, const Point3DAlg_RegistSession& s,
            const Eigen::Matrix4f& T, const char* what)
{
    step.next = s;
    FromEigen(T, step.next.transform);
    ++step.next.iteration;
    step.message = what;
    step.fitness = -1.0;      // 手动微调不产生 fitness
    step.inlier_rmse = -1.0;
    step.improved = false;
}

} // namespace

// =====================================================================
// 策略名 / 参数默认值 / 会话默认值
// =====================================================================

const char* Point3DAlg_RegistStrategyName(int strategy)
{
    switch (strategy) {
    case POINT3DALG_REGIST_ICP_P2P:     return "ICP-PointToPoint";
    case POINT3DALG_REGIST_ICP_P2PL:    return "ICP-PointToPlane";
    case POINT3DALG_REGIST_GICP:        return "GICP";
    case POINT3DALG_REGIST_FPFH_RANSAC: return "FPFH+RANSAC";
    case POINT3DALG_REGIST_NDT:         return "NDT";
    case POINT3DALG_REGIST_PCA:         return "PCA-主轴粗配准";
    case POINT3DALG_REGIST_MULTISCALE:  return "MultiscaleICP-金字塔";
    default:                            return "Unknown";
    }
}

Point3DAlg_RegistParams::Point3DAlg_RegistParams()
    : max_correspondence_dist(0.0)   // 0 = 按点云间距自适应
    , max_iteration(60)
    , transform_epsilon(1e-8)
    , fitness_epsilon(1e-6)
{
}

Point3DAlg_RegistSession::Point3DAlg_RegistSession()
    : strategy(POINT3DALG_REGIST_ICP_P2P)
    , iteration(0)
    , best_fitness(-1.0)
    , best_rmse(-1.0)
{
    SetIdentity(transform);
}

// =====================================================================
// 手动微调
// =====================================================================

Point3DAlg_RegistStep Point3DAlg_RegistTranslate(
    const Point3DAlg_RegistSession& s, double dx, double dy, double dz)
{
    Eigen::Matrix4f d = Eigen::Matrix4f::Identity();
    d(0, 3) = (float)dx;
    d(1, 3) = (float)dy;
    d(2, 3) = (float)dz;

    Point3DAlg_RegistStep step;
    // 增量左乘：在目标坐标系下平移
    Commit(step, s, d * ToEigen(s.transform), "");
    char buf[128];
    snprintf(buf, sizeof(buf), "平移 (%+.3f, %+.3f, %+.3f)", dx, dy, dz);
    step.message = buf;
    return step;
}

Point3DAlg_RegistStep Point3DAlg_RegistRotate(
    const Point3DAlg_RegistSession& s, int axis, double angle_deg)
{
    Point3DAlg_RegistStep step;
    Commit(step, s, RotationAxis(axis, angle_deg) * ToEigen(s.transform), "");
    char buf[128];
    snprintf(buf, sizeof(buf), "绕 %c 旋转 %+.2f°", "XYZ"[NormalizeAxis(axis)], angle_deg);
    step.message = buf;
    return step;
}

Point3DAlg_RegistStep Point3DAlg_RegistRotateAround(
    const Point3DAlg_RegistSession& s, const double pivot[3],
    int axis, double angle_deg)
{
    // T = Translate(+pivot) · R · Translate(-pivot) · 当前变换
    Eigen::Matrix4f tp = Eigen::Matrix4f::Identity();
    tp(0, 3) = (float)pivot[0];
    tp(1, 3) = (float)pivot[1];
    tp(2, 3) = (float)pivot[2];
    Eigen::Matrix4f tn = Eigen::Matrix4f::Identity();
    tn(0, 3) = (float)-pivot[0];
    tn(1, 3) = (float)-pivot[1];
    tn(2, 3) = (float)-pivot[2];

    Point3DAlg_RegistStep step;
    Commit(step, s, tp * RotationAxis(axis, angle_deg) * tn * ToEigen(s.transform), "");
    char buf[160];
    snprintf(buf, sizeof(buf), "绕 (%.2f,%.2f,%.2f) 的 %c 轴旋转 %+.2f°",
             pivot[0], pivot[1], pivot[2],
             "XYZ"[NormalizeAxis(axis)], angle_deg);
    step.message = buf;
    return step;
}

Point3DAlg_RegistStep Point3DAlg_RegistReset(const Point3DAlg_RegistSession& s)
{
    Point3DAlg_RegistStep step;
    step.next = s;
    SetIdentity(step.next.transform);
    step.next.iteration = 0;
    step.next.best_fitness = -1.0;
    step.next.best_rmse = -1.0;
    step.fitness = -1.0;
    step.inlier_rmse = -1.0;
    step.improved = false;
    step.message = "已复位为单位阵";
    return step;
}

Point3DAlg_RegistStep Point3DAlg_RegistSetStrategy(
    const Point3DAlg_RegistSession& s, int strategy)
{
    Point3DAlg_RegistStep step;
    step.next = s;
    step.next.strategy = strategy;
    step.fitness = -1.0;
    step.inlier_rmse = -1.0;
    step.improved = false;
    step.message = std::string("策略切换为 ") + Point3DAlg_RegistStrategyName(strategy);
    return step;
}

// =====================================================================
// 配准（策略分派）
// =====================================================================

namespace {

// 用 kd-tree 统计对应点，算 fitness（局内点占比，0~1 越大越好）与 rmse。
// 所有策略共用这一处——避免各分支各写一份导致口径不一致。
void EvaluateFitness(const pcl::PointCloud<pcl::PointXYZ>& src,
                     const pcl::PointCloud<pcl::PointXYZ>& dst,
                     const Eigen::Matrix4f& T, double max_dist,
                     double& fitness, double& rmse)
{
    pcl::PointCloud<pcl::PointXYZ> aligned;
    pcl::transformPointCloud(src, aligned, T);

    pcl::KdTreeFLANN<pcl::PointXYZ> kdt;
    pcl::PointCloud<pcl::PointXYZ>::Ptr dstPtr(new pcl::PointCloud<pcl::PointXYZ>(dst));
    kdt.setInputCloud(dstPtr);

    std::vector<int> idx(1);
    std::vector<float> d2(1);
    int inliers = 0;
    double sum2 = 0.0;
    for (const auto& p : aligned.points) {
        if (kdt.nearestKSearch(p, 1, idx, d2) == 1 && std::sqrt(d2[0]) <= max_dist) {
            ++inliers;
            sum2 += d2[0];
        }
    }
    fitness = aligned.empty() ? 0.0 : (double)inliers / (double)aligned.size();
    rmse = inliers ? std::sqrt(sum2 / inliers) : -1.0;
}

// PCA 主轴粗配准：质心对齐 + 主轴对齐。
// 特征向量符号歧义（v 与 -v 等价）用三阶矩（偏斜度）符号消解——该量旋转不变。
bool RunPca(const pcl::PointCloud<pcl::PointXYZ>& src,
            const pcl::PointCloud<pcl::PointXYZ>& dst,
            Eigen::Matrix4f& T_out, std::string& msg)
{
    if (src.size() < 10 || dst.size() < 10) {
        msg = "PCA 需要至少 10 个点";
        return false;
    }

    auto axes_of = [](const pcl::PointCloud<pcl::PointXYZ>& c, Eigen::Vector3f& centroid,
                      Eigen::Matrix3f& axes) {
        centroid = Eigen::Vector3f::Zero();
        for (const auto& p : c.points)
            centroid += Eigen::Vector3f(p.x, p.y, p.z);
        centroid /= (float)c.size();

        Eigen::Matrix3f cov = Eigen::Matrix3f::Zero();
        for (const auto& p : c.points) {
            const Eigen::Vector3f d = Eigen::Vector3f(p.x, p.y, p.z) - centroid;
            cov += d * d.transpose();
        }
        cov /= (float)c.size();

        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> es(cov);
        // eigenvalues 升序 → 取降序（主轴 1 = 最大特征值）
        axes.col(0) = es.eigenvectors().col(2);
        axes.col(1) = es.eigenvectors().col(1);
        axes.col(2) = es.eigenvectors().col(0);

        // 用三阶矩符号定轴正向（旋转不变，消除 180° 歧义）
        Eigen::Vector3f skew = Eigen::Vector3f::Zero();
        for (const auto& p : c.points) {
            const Eigen::Vector3f proj = axes.transpose() *
                (Eigen::Vector3f(p.x, p.y, p.z) - centroid);
            skew += proj.cwiseProduct(proj).cwiseProduct(proj);
        }
        for (int i = 0; i < 3; ++i)
            if (skew[i] < 0.0f)
                axes.col(i) = -axes.col(i);
    };

    Eigen::Vector3f cs, cd;
    Eigen::Matrix3f as, ad;
    axes_of(src, cs, as);
    axes_of(dst, cd, ad);

    const Eigen::Matrix3f R = ad * as.transpose();
    const Eigen::Vector3f t = cd - R * cs;

    T_out = Eigen::Matrix4f::Identity();
    T_out.block<3, 3>(0, 0) = R;
    T_out.block<3, 1>(0, 3) = t;
    msg = "PCA 主轴粗配准完成（无 fitness，请接 ICP 精修）";
    return true;
}

// FPFH + RANSAC（SAC-IA）
bool RunFpfhRansac(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& src,
                   const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& dst,
                   double max_dist,
                   Eigen::Matrix4f& T_out, double& fitness, double& rmse, std::string& msg)
{
    const float voxel = (float)std::max(max_dist * 0.5, 0.1);

    pcl::PointCloud<pcl::PointXYZ>::Ptr s_down(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr d_down(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::VoxelGrid<pcl::PointXYZ> vg;
    vg.setLeafSize(voxel, voxel, voxel);
    vg.setInputCloud(src); vg.filter(*s_down);
    vg.setInputCloud(dst); vg.filter(*d_down);

    if (s_down->size() < 20 || d_down->size() < 20) {
        msg = "FPFH 下采样后点数不足";
        return false;
    }

    auto normals_of = [voxel](const pcl::PointCloud<pcl::PointXYZ>::Ptr& c) {
        pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> ne;
        ne.setInputCloud(c);
        pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>);
        ne.setSearchMethod(tree);
        ne.setRadiusSearch(voxel * 5.0);
        pcl::PointCloud<pcl::Normal>::Ptr n(new pcl::PointCloud<pcl::Normal>);
        ne.compute(*n);
        return n;
    };
    auto fpfh_of = [voxel](const pcl::PointCloud<pcl::PointXYZ>::Ptr& c,
                           const pcl::PointCloud<pcl::Normal>::Ptr& n) {
        pcl::FPFHEstimation<pcl::PointXYZ, pcl::Normal, pcl::FPFHSignature33> f;
        f.setInputCloud(c);
        f.setInputNormals(n);
        pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>);
        f.setSearchMethod(tree);
        f.setRadiusSearch(voxel * 5.0);
        pcl::PointCloud<pcl::FPFHSignature33>::Ptr out(new pcl::PointCloud<pcl::FPFHSignature33>);
        f.compute(*out);
        return out;
    };

    auto ns = normals_of(s_down);
    auto nd = normals_of(d_down);
    auto fs = fpfh_of(s_down, ns);
    auto fd = fpfh_of(d_down, nd);

    pcl::SampleConsensusInitialAlignment<pcl::PointXYZ, pcl::PointXYZ, pcl::FPFHSignature33> sac;
    sac.setInputSource(s_down);
    sac.setSourceFeatures(fs);
    sac.setInputTarget(d_down);
    sac.setTargetFeatures(fd);
    sac.setMinSampleDistance((float)(max_dist * 0.5));
    sac.setMaxCorrespondenceDistance((float)(max_dist * 2.0));
    sac.setMaximumIterations(500);

    pcl::PointCloud<pcl::PointXYZ> aligned;
    sac.align(aligned);
    if (!sac.hasConverged()) {
        msg = "FPFH+RANSAC 未收敛（点云特征不足？）";
        return false;
    }
    T_out = sac.getFinalTransformation();
    fitness = sac.getFitnessScore(max_dist * 2.0);
    rmse = -1.0;   // PCL 的 SAC-IA 不直接给 RMSE
    msg = "FPFH+RANSAC 完成（建议接 ICP 精修）";
    return true;
}

} // namespace

Point3DAlg_RegistStep Point3DAlg_RegistRun(
    const Point3DAlg_RegistSession& s,
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& source,
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& target,
    const Point3DAlg_RegistParams& params)
{
    Point3DAlg_RegistStep step;
    step.next = s;
    step.fitness = -1.0;
    step.inlier_rmse = -1.0;
    step.improved = false;

    if (!source || !target || source->empty() || target->empty()) {
        step.message = "点云为空，无法配准";
        return step;
    }

    // 自适应阈值：max_corr=0 时按目标点云间距 ×4
    double max_dist = params.max_correspondence_dist;
    if (max_dist <= 0.0) {
        const double spacing = Point3DAlg_RegistEstimateSpacing(target);
        max_dist = std::min(std::max(spacing * 4.0, 0.05), 20.0);
    }

    const Eigen::Matrix4f init = ToEigen(s.transform);
    Eigen::Matrix4f T = init;
    double fitness = -1.0, rmse = -1.0;
    std::string msg;

    // 真正跑配准的策略（PCA/FPFH 自己处理初值语义）
    switch (s.strategy) {
    case POINT3DALG_REGIST_ICP_P2P: {
        float T16[16];
        std::memcpy(T16, s.transform, sizeof(T16));
        if (!shapely::CloudProcessor<pcl::PointXYZ>::registICP_PointToPoint(
                source, target, max_dist, T16, s.transform)) {
            msg = "ICP 点到点未收敛";
            break;
        }
        T = ToEigen(T16);
        msg = "ICP-PointToPoint";
        break;
    }
    case POINT3DALG_REGIST_ICP_P2PL: {
        // 目标法线（K 近邻 20 足够）
        pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> ne;
        ne.setInputCloud(target);
        pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>);
        ne.setSearchMethod(tree);
        ne.setKSearch(20);
        pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
        ne.compute(*normals);

        float T16[16];
        std::memcpy(T16, s.transform, sizeof(T16));
        if (!shapely::CloudProcessor<pcl::PointXYZ>::registICP_PointToPlane(
                source, target, normals, max_dist, T16, s.transform)) {
            msg = "ICP 点到面未收敛";
            break;
        }
        T = ToEigen(T16);
        msg = "ICP-PointToPlane";

        // 退化检测：点到面在平坦区域奇异（法线全平行），fitness 会很低 → 回退点到点
        double f = 0.0, r = 0.0;
        EvaluateFitness(*source, *target, T, max_dist, f, r);
        if (f < 0.05) {
            float Tfall[16];
            std::memcpy(Tfall, s.transform, sizeof(Tfall));
            if (shapely::CloudProcessor<pcl::PointXYZ>::registICP_PointToPoint(
                    source, target, max_dist, Tfall, s.transform)) {
                T = ToEigen(Tfall);
                msg = "ICP-PointToPlane 退化(平坦区奇异)，已回退点到点";
            }
        }
        break;
    }
    case POINT3DALG_REGIST_GICP: {
        pcl::GeneralizedIterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> gicp;
        gicp.setInputSource(source);
        gicp.setInputTarget(target);
        gicp.setMaxCorrespondenceDistance(max_dist);
        gicp.setMaximumIterations(params.max_iteration);
        gicp.setTransformationEpsilon(params.transform_epsilon);
        gicp.setEuclideanFitnessEpsilon(params.fitness_epsilon);
        pcl::PointCloud<pcl::PointXYZ> aligned;
        gicp.align(aligned, init);
        if (!gicp.hasConverged()) {
            msg = "GICP 未收敛";
            break;
        }
        T = gicp.getFinalTransformation();
        msg = "GICP";
        break;
    }
    case POINT3DALG_REGIST_FPFH_RANSAC: {
        if (!RunFpfhRansac(source, target, max_dist, T, fitness, rmse, msg))
            return step;   // 失败：fitness 保持 -1
        break;
    }
    case POINT3DALG_REGIST_NDT: {
        pcl::NormalDistributionsTransform<pcl::PointXYZ, pcl::PointXYZ> ndt;
        ndt.setInputSource(source);
        ndt.setInputTarget(target);
        ndt.setResolution((float)std::max(max_dist, 1.0));
        ndt.setMaximumIterations(params.max_iteration);
        ndt.setTransformationEpsilon(params.transform_epsilon);
        pcl::PointCloud<pcl::PointXYZ> aligned;
        ndt.align(aligned, init);
        if (!ndt.hasConverged()) {
            msg = "NDT 未收敛";
            break;
        }
        T = ndt.getFinalTransformation();
        msg = "NDT";
        break;
    }
    case POINT3DALG_REGIST_PCA: {
        if (!RunPca(*source, *target, T, msg))
            return step;
        break;
    }
    case POINT3DALG_REGIST_MULTISCALE: {
        // 多尺度 ICP（Open3D multiscale_icp）：voxel 金字塔由 max_dist 自适应——
        // 粗级 voxel=2×max_dist（对应距离 3×max_dist）覆盖较大初值偏差，
        // 细级 voxel=0.3×max_dist 逼近单级 ICP 精度
        const std::vector<double> voxels = { max_dist * 2.0, max_dist * 0.8, max_dist * 0.3 };
        const std::vector<int> iters(3, std::max(params.max_iteration, 1));
        float T16[16];
        std::memcpy(T16, s.transform, sizeof(T16));
        if (!shapely::CloudProcessor<pcl::PointXYZ>::registMultiscaleICP(
                source, target, voxels, iters, 1.5, T16, s.transform)) {
            msg = "MultiscaleICP 未收敛";
            break;
        }
        T = ToEigen(T16);
        msg = "MultiscaleICP";
        break;
    }
    default:
        step.message = "未知策略";
        return step;
    }

    // ---- 变换有限性守卫（项目铁律：NaN 统一拦截）----
    // GICP/NDT/SAC-IA 在退化输入下可产出 NaN 变换而 hasConverged() 仍为 true
    // （ICP 家族在 CloudProcessor::matToArray 内部已拦，此处统一兜底所有策略）。
    if (!T.allFinite()) {
        step.message = msg.empty() ? "配准失败" : msg;
        step.message += "：变换含非有限值，判失败";
        step.fitness = -1.0;
        step.inlier_rmse = -1.0;
        step.improved = false;
        return step;   // next 保持初值（s），不写入 NaN
    }

    // 统一评估 fitness（FPFH 分支已在内部算过，跳过重复计算）
    if (fitness < 0.0)
        EvaluateFitness(*source, *target, T, max_dist, fitness, rmse);

    step.fitness = fitness;
    step.inlier_rmse = rmse;

    char buf[256];
    if (rmse >= 0.0)
        snprintf(buf, sizeof(buf), "%s: fitness=%.4f rmse=%.4f", msg.c_str(), fitness, rmse);
    else
        snprintf(buf, sizeof(buf), "%s: fitness=%.4f", msg.c_str(), fitness);
    step.message = buf;

    FromEigen(T, step.next.transform);
    ++step.next.iteration;
    if (fitness > step.next.best_fitness) {
        step.next.best_fitness = fitness;
        step.next.best_rmse = rmse;
        step.improved = true;
    }
    return step;
}


pcl::PointCloud<pcl::PointXYZ>::Ptr Point3DAlg_RegistApplyTransform(
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud, const float transform[16])
{
    pcl::PointCloud<pcl::PointXYZ>::Ptr out(new pcl::PointCloud<pcl::PointXYZ>);
    if (!cloud)
        return out;
    pcl::transformPointCloud(*cloud, *out, ToEigen(transform));
    return out;
}

bool Point3DAlg_RegistIsIdentity(const float transform[16], double eps)
{
    float I[16];
    SetIdentity(I);
    for (int i = 0; i < 16; ++i)
        if (std::fabs((double)transform[i] - (double)I[i]) > eps)
            return false;
    return true;
}

double Point3DAlg_RegistEstimateSpacing(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud)
{
    if (!cloud || cloud->size() < 2)
        return 1.0;
    pcl::KdTreeFLANN<pcl::PointXYZ> kdt;
    kdt.setInputCloud(cloud);

    // 隔点采样，避免上百万点全算
    const size_t step = std::max<size_t>(1, cloud->size() / 2000);
    std::vector<float> dists;
    dists.reserve(cloud->size() / step + 1);
    std::vector<int> idx(2);
    std::vector<float> d2(2);
    for (size_t i = 0; i < cloud->size(); i += step) {
        if (kdt.nearestKSearch(cloud->points[i], 2, idx, d2) == 2 && d2[1] > 0.0f)
            dists.push_back(std::sqrt(d2[1]));
    }
    if (dists.empty())
        return 1.0;
    std::nth_element(dists.begin(), dists.begin() + dists.size() / 2, dists.end());
    return (double)dists[dists.size() / 2];
}

// =====================================================================
// 变换文件读写
// =====================================================================

bool Point3DAlg_RegistSaveTransform(const char* path, const float transform[16])
{
    if (!path || !path[0])
        return false;
    char buf[1024];
    int n = 0;
    for (int r = 0; r < 4; ++r) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%.8f %.8f %.8f %.8f\n",
                      transform[r * 4], transform[r * 4 + 1],
                      transform[r * 4 + 2], transform[r * 4 + 3]);
    }
    return CommonAlg_WriteFile(path, std::string(buf, (size_t)n));
}

bool Point3DAlg_RegistSaveTransformFlat(const char* path, const float transform[16])
{
    if (!path || !path[0])
        return false;
    return CommonAlg_WriteFile(path, Point3DAlg_RegistFormatFlat(transform) + "\n");
}

std::string Point3DAlg_RegistFormatFlat(const float transform[16])
{
    std::string out;
    char buf[64];
    for (int i = 0; i < 16; ++i) {
        snprintf(buf, sizeof(buf), "%.6f", transform[i]);
        if (i)
            out += ",";
        out += buf;
    }
    return out;
}

bool Point3DAlg_RegistParseTransformText(const char* text, float transform[16])
{
    if (!text || !text[0])
        return false;

    // 把所有非数字字符视为分隔符（兼容 4x4 矩阵、逗号/空格扁平、带键名的整行）
    std::vector<double> vals;
    const char* p = text;
    while (*p && vals.size() < 16) {
        if (std::isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.') {
            char* end = nullptr;
            const double v = std::strtod(p, &end);
            if (end != p) {
                vals.push_back(v);
                p = end;
                continue;
            }
        }
        ++p;
    }
    if (vals.size() != 16)
        return false;
    for (int i = 0; i < 16; ++i)
        transform[i] = (float)vals[(size_t)i];
    return true;
}

bool Point3DAlg_RegistLoadTransform(const char* path, float transform[16])
{
    if (!path || !path[0])
        return false;
    std::string text;
    if (!CommonAlg_ReadFile(path, text))
        return false;
    return Point3DAlg_RegistParseTransformText(text.c_str(), transform);
}
