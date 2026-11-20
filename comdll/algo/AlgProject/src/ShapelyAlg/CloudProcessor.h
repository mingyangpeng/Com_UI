#ifndef SHAPELY_CLOUD_PROCESSOR_H
#define SHAPELY_CLOUD_PROCESSOR_H

// shapely::CloudProcessor —— 三维点云基础算子模块（PCL 封装，显式实例化）。
// 设计约束（见技术交底书）：
//   * 公共头文件仅允许 <pcl/point_types.h> + <pcl/point_cloud.h>（容器定义），
//     严禁算法级重头文件（registration/keypoints/octree/...）；
//   * 复杂算法类（KdTree/Octree）前置声明，输出矩阵统一 float[16]（不暴露 Eigen）；
//   * 库原生枚举 shapely::FitMethod（RANSAC/LMEDS/MSAC），底层映射 pcl::SAC_*
//     （注意：PCL 的 LMEDS/MSAC 在 SACMODEL 平面/球拟合中支持有限，底层做能力回退）；
//   * 显式实例化白名单：pcl::PointXYZ / PointXYZI / PointXYZRGB（.cpp 内），
//     业务层直接链接已编译机器码，无模板传染；
//   * 零拷贝：pcl::PointCloud<PointT>::Ptr 流转；
//   * 命名规范：分类前缀 + 动作（filter/extract/trans/feature/keypoint/fit/
//     segment/regist/tree/surface/measure），camelCase。
//
// 模块划分对齐 Open3D 0.19/0.20 的 PointCloud API（方法名 → 本库接口）：
//   filter   voxel_down_sample / random_down_sample / uniform_down_sample /
//            farthest_point_down_sample / remove_non_finite_points /
//            remove_duplicated_points / voxel_down_sample_and_trace
//   extract  crop（AABB / OBB）/ select_by_index
//   trans    transform / translate / rotate / scale
//   feature  estimate_normals / orient_normals_*（方向/视点/全局一致）/
//            compute_fpfh_feature / estimate_covariances /
//            compute_mahalanobis_distance / compute_nearest_neighbor_distance
//   segment  segment_plane / cluster_dbscan / region growing（平面片检测）
//   regist   registration_icp（point-to-point / point-to-plane）/ GICP / NDT /
//            registration_ransac_based_on_feature_matching（SAC-IA）/
//            evaluate_registration / get_information_matrix_from_point_clouds
//   surface  compute_convex_hull / Alpha Shapes（凹包）/ smooth_mls /
//            create_from_point_cloud_poisson / greedy projection / marching cubes
//            (Hoppe / RBF) / grid projection / organized fast mesh /
//            mesh Laplacian smoothing
//   measure  get_axis_aligned_bounding_box / get_oriented_bounding_box /
//            get_center / compute_mean_and_covariance / compute_point_cloud_distance
//
// 说明：Open3D 的 Ball Pivoting、FGR、Laplacian/Taubin 平滑、DBSCAN 在 PCL 1.9
//   无对应实现或未预编译，本库用等效 PCL 能力替代（DBSCAN 基于 KdTree + 并查集，
//   Taubin/Laplacian 点云平滑用 MLS 投影，网格平滑用 umbrella 算子自实现）。

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>

#include "AlgExport.h"
#include "Types.h"

#include <array>
#include <limits>
#include <string>
#include <vector>

namespace shapely {

// 拟合方法（库原生枚举，底层映射 PCL SAC 花样）
enum class FitMethod {
    RANSAC = 0, //随机采样一致性 需阈值,阈值外一律不算
    LMEDS  = 1, //最小中值平方
    MSAC   = 2, //M-估计采样一致性
};

// 三角网格（表面重建输出，轻量结构：公共头不暴露 PCL PolygonMesh）
struct Mesh3d {
    std::vector<Point3d> vertices;                  // 顶点坐标
    std::vector<std::array<int, 3>> triangles;      // 三角面（顶点索引，逆时针）
    bool valid = false;                             // false = 重建失败/无面片
};

// 位置度偏差项（measurePositionDeviation 输出；2D 口径，Z 忽略）
struct PositionDeviation {
    int theoretical = -1;   // 理论位索引（恒 >= 0）
    int measured = -1;      // 实测索引；-1 = 该理论位在 match_radius 内无实测
    double dx = 0;          // 实测 − 理论（X）
    double dy = 0;          // 实测 − 理论（Y）
    double dist = 0;        // sqrt(dx²+dy²)
};

// 高度图：点云 XY 栅格化产物（3D 点云 → 2D ImageProcessor 全部算子的桥接数据）。
// 行优先存储：index = row*width + col；row 0 对应 y 最小（按图像 imshow 显示时 Y 向下）。
// 含 vector 属非平凡结构，不参与 Types.h 的平凡性 static_assert（同 Mesh3d）。
struct HeightMap {
    int width = 0;                    // 列数（X 方向栅格数）
    int height = 0;                   // 行数（Y 方向栅格数）
    double x0 = 0;                    // 栅格 (0,0) 的 X（= 有效点 x 最小值）
    double y0 = 0;                    // 栅格 (0,0) 的 Y（= 有效点 y 最小值）
    double cell = 0;                  // 栅格边长（与坐标同单位）
    std::vector<float> cells;         // 行优先 z 值；无点栅格 = NaN；size = width*height
    std::vector<unsigned char> mask;  // 1 = 有点 / 0 = 空；size = width*height
    bool valid = false;               // false = 输入非法/为空/超内存上限

    /// 取栅格值（越界或结构不一致返回 NaN；不检查 mask，空栅格本就是 NaN）
    float at(int row, int col) const
    {
        if (row < 0 || col < 0 || row >= height || col >= width ||
            cells.size() != (size_t)width * (size_t)height)
            return std::numeric_limits<float>::quiet_NaN();
        return cells[(size_t)row * (size_t)width + (size_t)col];
    }
};

template <typename PointT>
class SHAPELYALG_EXPORT CloudProcessor
{
public:
    CloudProcessor() = delete;   // 纯静态工具类

    using CloudPtr = typename pcl::PointCloud<PointT>::Ptr;
    using CloudConstPtr = typename pcl::PointCloud<PointT>::ConstPtr;

    // ---------------- filter：滤波与降采样 ----------------

    /// 体素下采样（PCL VoxelGrid）：每个体素内点集以质心替代。
    /// [in] leaf_size  体素边长（三轴同值），与点云坐标同单位（mm 惯例）
    /// [in] min_points_per_voxel  体素内最少点数（0 = 不过滤；Open3D 无此参数，PCL 扩展）
    /// 返回：下采样后的新点云；src 为空或 leaf<=0 返回 nullptr。
    static CloudPtr filterVoxelGrid(const CloudConstPtr& src, double leaf_size,
                                    int min_points_per_voxel = 0);

    /// 体素下采样并回溯（Open3D voxel_down_sample_and_trace）：
    /// 输出下采样点云 + 每个输出点对应的原始点索引列表（同一体素内的点）。
    /// [out] out_trace  out_trace[i] = 第 i 个输出点对应的输入点索引集合
    static CloudPtr filterVoxelGridAndTrace(const CloudConstPtr& src, double leaf_size,
                                            std::vector<std::vector<int>>& out_trace);

    /// 随机采样（Open3D random_down_sample）：随机保留 sample_size 个点。
    /// [in] sample_size  目标点数（≥ 点数时返回副本）
    /// [in] seed         随机种子（固定种子可复现）
    static CloudPtr filterRandomSample(const CloudConstPtr& src, int sample_size,
                                       unsigned int seed = 0);

    /// 等间隔采样（Open3D uniform_down_sample）：从索引 0 起每 every_k 点保留 1 个。
    static CloudPtr filterUniformSample(const CloudConstPtr& src, int every_k);

    /// 最远点采样（Open3D farthest_point_down_sample）：逐步选取距已选集合最远的点，
    /// 结果在空间上比体素更均匀，代价 O(n·k)。
    /// [in] num_samples  目标点数（≥ 点数时返回副本）
    /// [in] start_index  首个采样点索引（越界按 0 处理）
    static CloudPtr filterFarthestPointSample(const CloudConstPtr& src, int num_samples,
                                              int start_index = 0);

    /// 去除非有限点（Open3D remove_non_finite_points）：剔除 NaN/Inf 坐标点。
    static CloudPtr filterRemoveNonFinite(const CloudConstPtr& src);

    /// 去除重复点（Open3D remove_duplicated_points）：坐标距离 ≤ tolerance 视为重复，
    /// 保留首次出现的点；tolerance=0 表示坐标完全相等。
    static CloudPtr filterRemoveDuplicates(const CloudConstPtr& src, double tolerance = 0.0);

    /// 直通滤波：沿指定字段保留 [limit_min, limit_max] 区间内的点。
    /// [in] field_name  字段名（"x"/"y"/"z"/"intensity"）
    /// 返回：过滤后的新点云；src 为空返回 nullptr。
    static CloudPtr filterPassThrough(const CloudConstPtr& src, const std::string& field_name,
                                      double limit_min, double limit_max);

    /// 统计离群点去除：每点统计 k 近邻平均距离，偏离全体均值 std_dev_mul 倍
    /// 标准差以上的点视为噪声剔除。
    /// [in] mean_k       近邻个数（典型 20~50）
    /// [in] std_dev_mul  标准差倍数阈值（典型 1.0，越小越严格）
    static CloudPtr filterStatisticalOutlier(const CloudConstPtr& src, int mean_k, double std_dev_mul);

    /// 半径离群点去除：搜索半径内邻居数不足的点剔除。
    /// [in] radius        搜索半径（含坐标单位）
    /// [in] min_neighbors 至少需要的邻居数；越大越严格
    static CloudPtr filterRadiusOutlier(const CloudConstPtr& src, double radius, int min_neighbors);

    // 条件滤波：XYZ 范围（GT/LT 严格比较，即开区间 (min, max)）
    static CloudPtr filterConditionalXYZ(const CloudConstPtr& src,
                                         double x_min, double x_max,
                                         double y_min, double y_max,
                                         double z_min, double z_max);

    /// 近似体素降采样（PCL ApproximateVoxelGrid）：哈希近似质心，大点云比
    /// filterVoxelGrid 快数倍；格点质心略有偏差，输出格数与精确版可能相差 1~2%。
    /// [in]  leaf  体素边长（>0；<=0 或空云 → nullptr，调用方按空结果处理）
    static CloudPtr filterApproxVoxelGrid(const CloudConstPtr& src, double leaf);

    // ---------------- extract：点云提取 ----------------

    /// 按索引提取子点云（常用于提取拟合/分割后的局内点）。
    /// [in] indices  要提取的点索引；空索引返回 nullptr。
    static CloudPtr extractByIndices(const CloudConstPtr& src, const std::vector<int>& indices);

    /// 反向按索引提取（Open3D select_by_index(..., invert=true)）：剔除给定索引的点。
    static CloudPtr extractByIndicesInverted(const CloudConstPtr& src, const std::vector<int>& indices);

    /// 轴对齐盒裁剪（Open3D crop(AABB)）：保留 [min, max] 闭区间内的点。
    /// [in] min_corner/max_corner  盒最小/最大角点 [x,y,z]
    static CloudPtr extractCropBox(const CloudConstPtr& src, const float min_corner[3],
                                   const float max_corner[3]);

    /// 有向盒裁剪（Open3D crop(OBB)）：把点变换到盒局部系后做 AABB 裁剪。
    /// [in] box               OBB3d（center + extents 半长 + rotation 行=主轴）
    /// [in] invert            true = 保留盒外点（Open3D crop(invert=True)）
    static CloudPtr extractCropOBB(const CloudConstPtr& src, const OBB3d& box, bool invert = false);

    /// 按 2D 多边形 ROI 裁剪点云（XY 投影 + 射线法内外判定，自包含实现）。
    /// [in] polygon  ROI 顶点 XY（首尾闭合与否均可；少于 3 点返回 nullptr）
    /// [in] invert   true = 保留 ROI 外的点（Open3D crop 语义）
    /// 非有限坐标点自动剔除；边界点归属为半开语义（不保证在边界上）——
    /// 需含边界时先把 ROI 外扩微量。
    static CloudPtr extractByPolygonROI(const CloudConstPtr& src,
                                        const std::vector<Point2d>& polygon,
                                        bool invert = false);

    // ---------------- trans：空间变换与投影 ----------------

    /// 应用 4x4 刚体变换矩阵。
    /// [in] matrix_4x4  行主序（Eigen::Matrix4f 兼容布局）：[r00 r01 r02 tx | r10 .. ty | r20 .. tz | 0001]
    static CloudPtr transApplyMatrix(const CloudConstPtr& src, const float matrix_4x4[16]);

    /// 把点云投影到指定平面：p' = p - (n·p+d)/|n|² · n。
    /// [in] plane_coeffs  [a,b,c,d]，平面方程 ax+by+cz+d=0；|n|=0 返回 nullptr
    static CloudPtr transProject(const CloudConstPtr& src, const float plane_coeffs[4]);

    /// 平移点云（dx/dy/dz 为三轴位移量，含坐标单位）。
    static CloudPtr transTranslate(const CloudConstPtr& src, double dx, double dy, double dz);

    /// 绕过原点的轴旋转点云（右手系）。
    /// [in] axis_x/y/z  旋转轴方向向量（不需单位化，内部归一）
    /// [in] angle       旋转角（弧度）
    static CloudPtr transRotate(const CloudConstPtr& src, double axis_x, double axis_y,
                                double axis_z, double angle);

    /// 绕过指定点的轴旋转（业务常用：绕工件中心/夹具轴旋转）。
    /// [in] pivot       旋转中心 [x,y,z]（轴过此点）
    /// 等价于 translate(-pivot) → transRotate → translate(+pivot) 的复合。
    static CloudPtr transRotateAround(const CloudConstPtr& src, const float pivot[3],
                                      double axis_x, double axis_y, double axis_z, double angle);

    /// 三轴独立缩放（单位换算：mm→m 传 0.001；镜像传负值）。
    /// [in] scale_xyz  [sx, sy, sz] 各轴缩放系数
    static CloudPtr transScale(const CloudConstPtr& src, const double scale_xyz[3]);

    /// 绕指定中心缩放（Open3D scale(scale, center)）：p' = c + s·(p - c)。
    /// [in] center  缩放中心 [x,y,z]（如点云质心）
    static CloudPtr transScaleAround(const CloudConstPtr& src, const double scale[3],
                                     const float center[3]);

    // ---------------- feature：特征提取 ----------------

    /// 估计点云法线（K 近邻局部平面拟合）。
    /// [in] k_search  近邻个数（典型 10~30，越大越平滑）
    /// 返回：法线点云（xyz = 单位法向，curvature = 曲率），与输入点一一对应；
    ///       src 为空返回 nullptr。
    static pcl::PointCloud<pcl::Normal>::Ptr featureNormalEstimation(const CloudConstPtr& src, int k_search);

    /// 半径邻域法线估计（Open3D estimate_normals(KDTreeSearchParamHybrid)）：
    /// 在 radius 内取邻居（最多 max_neighbors 个）拟合局部平面。
    /// [in] radius         搜索半径（与坐标同单位）
    /// [in] max_neighbors  邻居数上限（Open3D 的 max_nn 语义；半径内不足 3 点置 NaN）
    static pcl::PointCloud<pcl::Normal>::Ptr featureNormalEstimationRadius(const CloudConstPtr& src,
                                                                          double radius,
                                                                          int max_neighbors);

    /// 法线朝向一致化（Open3D orient_normals_to_align_with_direction）：
    /// 逐个翻转法线使其与参考方向夹角 ≤ 90°（如朝 +Z、朝相机）。
    /// [in] reference_direction 参考方向 [dx,dy,dz]（不需单位化）
    /// 返回：翻转后的新法线云；输入为空返回 nullptr。
    static pcl::PointCloud<pcl::Normal>::Ptr featureOrientNormalsToDirection(
        const pcl::PointCloud<pcl::Normal>::ConstPtr& normals, const float reference_direction[3]);

    /// 法线朝向一致化（Open3D orient_normals_towards_camera_location）：
    /// 使法线指向视点（与 point→viewpoint 向量同向）。
    static pcl::PointCloud<pcl::Normal>::Ptr featureOrientNormalsTowardsPoint(
        const CloudConstPtr& src, const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
        const float viewpoint[3]);

    /// 法线朝向全局一致化（Open3D orient_normals_consistent_tangent_plane）：
    /// 在 KNN 图上取最小生成树，沿树边逐点翻转，使相邻法线夹角 ≤ 90°。
    /// Poisson 重建前必须先做这一步（法线朝向不一致会导致表面翻面/空洞）。
    /// [in] k_neighbors  邻接图近邻数（典型 20~50；越大越平滑，代价 O(n·k)）
    /// 返回：朝向一致的新法线云；输入为空返回 nullptr。
    /// 注意：PCL 1.9 无该实现，内部用 KdTree + Prim MST 实现（对应 Open3D 的图传播简化版）。
    static pcl::PointCloud<pcl::Normal>::Ptr featureOrientNormalsConsistentTangentPlane(
        const CloudConstPtr& src, const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
        int k_neighbors = 30);

    /// FPFH 特征（Open3D compute_fpfh_feature）：快速点特征直方图，33 维/点，
    /// 用于粗配准的局部描述子。
    /// [in] normals       与 src 一一对应的法线（必填）
    /// [in] radius_search 特征计算半径（典型取点云平均间距的 5~10 倍）
    /// 返回：nullptr = 输入非法/点数 < 3/法线全无效（PCL 对退化输入会返回
    ///       全零直方图且 is_dense=true，此处提前拦截，避免配准静默错配）。
    static pcl::PointCloud<pcl::FPFHSignature33>::Ptr featureFPFH(
        const CloudConstPtr& src, const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
        double radius_search);

    /// 逐点协方差估计（Open3D estimate_covariances）：K 近邻局部协方差矩阵。
    /// [out] out_covariances  每个点 3x3 协方差，行主序展开为 9 个 float（[i*9 + r*3 + c]）
    /// 返回：false = src/normals 为空或 k<3
    static bool featureCovariances(const CloudConstPtr& src, int k_search,
                                   std::vector<std::array<float, 9>>& out_covariances);

    /// 马氏距离（Open3D compute_mahalanobis_distance）：点相对整体分布的异常程度，
    /// 用于粗筛离群点。要求点数 ≥ 4（协方差可逆）。
    /// [out] out_distances  与 src 等长的距离数组
    /// 返回：false = 点数不足
    static bool featureMahalanobisDistance(const CloudConstPtr& src,
                                           std::vector<double>& out_distances);

    /// 最近邻距离（Open3D compute_nearest_neighbor_distance）：每点到最近邻的距离，
    /// 可用均值/中位数估计点云平均间距（选 radius/k 参数的常用依据）。
    /// [in] k  取第 k 近邻（默认 1 = 最近邻；k≥size 时取最远点）
    static std::vector<float> featureNearestNeighborDistance(const CloudConstPtr& src, int k = 1);

    // ---------------- keypoint：关键点检测 ----------------

    /// Harris 3D 角点检测（内部转 PointXYZ 处理，非极大值抑制开启）。
    /// [in] radius     邻域搜索半径
    /// [in] threshold  响应阈值，越小保留的关键点越多
    static CloudPtr keypointHarris3D(const CloudConstPtr& src, double radius, double threshold);

    /// ISS 关键点检测（内在形状特征，对旋转/缩放稳定）。
    /// [in] salient_radius  显著性半径（典型取点云平均间距的数倍）
    /// [in] non_max_radius  非极大值抑制半径
    static CloudPtr keypointISS3D(const CloudConstPtr& src, double salient_radius, double non_max_radius);

    // ---------------- fit：稳健模型拟合 ----------------

    /// 拟合平面（采样一致性框架）。
    /// [in]  method             RANSAC/LMEDS/MSAC（平面模型三者都支持）
    /// [in]  distance_threshold 局内点判定距离阈值（含坐标单位，如 0.01）
    /// [out] out_coeffs         平面方程 [a,b,c,d]（ax+by+cz+d=0，|n|=1）
    /// [out] out_inliers        局内点索引
    /// 返回：false = 拟合失败（无局内点）；src 为空返回 false。
    static bool fitPlane(const CloudConstPtr& src, FitMethod method, double distance_threshold,
                         std::vector<float>& out_coeffs, std::vector<int>& out_inliers);

    /// 拟合球体。
    /// [out] out_center  球心 [x,y,z]
    /// [out] out_radius  球半径
    /// 注意：PCL 1.9 球模型仅支持 RANSAC/LMEDS，传 MSAC 会自动回退 RANSAC。
    static bool fitSphere(const CloudConstPtr& src, FitMethod method, double distance_threshold,
                          std::vector<float>& out_center, double& out_radius,
                          std::vector<int>& out_inliers);

    /// 拟合空间直线（PCL SACMODEL_LINE：RANSAC/LMEDS/MSAC 三种方法均支持）。
    /// [out] out_point  直线上一点 [x,y,z]
    /// [out] out_dir    单位方向 [dx,dy,dz]
    static bool fitLine3D(const CloudConstPtr& src, FitMethod method, double distance_threshold,
                          std::vector<float>& out_point, std::vector<float>& out_dir,
                          std::vector<int>& out_inliers);

    /// 拟合圆柱。
    /// [out] out_axis   轴方向 [dx,dy,dz]（单位向量）
    /// [out] out_point  轴上一点 [x,y,z]
    /// [out] out_radius 圆柱半径
    /// 注意：PCL 1.9 圆柱模型仅支持 RANSAC，method 参数被忽略。
    static bool fitCylinder(const CloudConstPtr& src, FitMethod method, double distance_threshold,
                            std::vector<float>& out_axis, std::vector<float>& out_point,
                            double& out_radius, std::vector<int>& out_inliers);

    /// 3D 空间圆拟合（孔位/焊球测量）：RANSAC 主平面 → 局内点投影 → 2D 几何最小
    /// 二乘圆拟合（与 ImageProcessor::fitCircle 同核）→ 回投 3D。
    /// [in]  method/distance_threshold  平面拟合口径（复用 fitPlane；同一阈值也用于圆周内点）
    /// [out] out_center  圆心（3D）
    /// [out] out_normal  圆平面单位法向 [x,y,z]
    /// [out] out_radius  半径
    /// [out] out_inliers 圆局内点索引（平面局内点中距圆周 ≤ 阈值者）
    /// [out] out_rms     圆周残差均方根（局内点口径）
    /// 返回：false = 平面拟合失败 / 点数 < 3 / 圆退化（投影点共线）。
    static bool fitCircle3D(const CloudConstPtr& src, FitMethod method, double distance_threshold,
                            Point3d& out_center, float out_normal[3], double& out_radius,
                            std::vector<int>& out_inliers, double& out_rms);

    /// 拟合圆锥（沉头孔/倒角口）。
    /// [out] out_apex    圆锥顶点 [x,y,z]
    /// [out] out_axis    轴方向 [dx,dy,dz]（单位向量）
    /// [out] out_angle   半开角（弧度，0~π/2）
    /// [out] out_inliers 局内点索引
    /// 注意：① PCL 1.9 圆锥模型仅支持 RANSAC，method 参数被忽略；轴向无约束。
    ///       ② 本接口直接构造 SampleConsensusModelCone + RANSAC——绕开
    ///          SACSegmentationFromNormals 的 PCL 1.9 缺陷（其 CONE 分支用默认值
    ///          0/0 覆盖开角约束，任何真实圆锥都 0 局内点）。
    ///       ③ 顶点由三切平面交解析求得，法线角度误差会被放大成顶点漂移——
    ///          distance_threshold 需覆盖该漂移（KNN 法线在曲面上的典型误差下，
    ///          过小的阈值会不收敛；法线无定向不影响，切平面与符号无关）。
    static bool fitCone(const CloudConstPtr& src, FitMethod method, double distance_threshold,
                        std::vector<float>& out_apex, std::vector<float>& out_axis,
                        double& out_angle, std::vector<int>& out_inliers);

    /// 圆度（GD&T 简化口径）：3D 空间圆拟合后，圆周局内点到拟合圆的径向残差峰峰值。
    /// [out] out_roundness          峰峰值（与坐标同单位）
    /// [out] out_center/out_normal/out_radius  拟合圆参数（同 fitCircle3D）
    /// 返回：false = fitCircle3D 失败 / 圆周局内点 < 2。
    static bool measureCircleRoundness(const CloudConstPtr& src, FitMethod method,
                                       double distance_threshold, double& out_roundness,
                                       Point3d& out_center, float out_normal[3],
                                       double& out_radius);

    /// 圆柱度（GD&T 简化口径）：fitCylinder 局内点到轴距离与半径之差的峰峰值。
    /// [out] out_form            峰峰值（与坐标同单位）
    /// [out] out_axis/out_point/out_radius  拟合圆柱参数（同 fitCylinder）
    /// 注意：PCL 1.9 圆柱模型仅支持 RANSAC，method 参数被忽略。
    /// 返回：false = fitCylinder 失败 / 局内点 < 2。
    static bool measureCylinderForm(const CloudConstPtr& src, FitMethod method,
                                    double distance_threshold, double& out_form,
                                    std::vector<float>& out_axis, std::vector<float>& out_point,
                                    double& out_radius);

    /// 位置度（孔位偏差表）：理论孔位列表 vs 实测圆心数组，逐理论位就近配对
    ///（贪心，每实测至多配一位，配对距离 ≤ match_radius）。
    /// [in] theoretical  理论孔位（2D，Z 忽略）
    /// [in] measured     实测圆心（2D，Z 忽略；典型来源 = 各孔 fitCircle3D 的圆心）
    /// [in] match_radius 配对半径（与坐标同单位）
    /// [out] out_pairs   与 theoretical 等长（theoretical 恒有效；measured=-1 = 未配到）
    /// 返回：配对成功数；参数非法返回 -1。
    static int measurePositionDeviation(const std::vector<Point2d>& theoretical,
                                        const std::vector<Point2d>& measured,
                                        double match_radius,
                                        std::vector<PositionDeviation>& out_pairs);

    /// 点云对给定平面方程的有符号残差数组（residual = n·p + d；
    /// n 不归一时内部除 |n|，保证残差为真实距离口径）。
    /// 非有限点输出 NaN。返回：false = src 空 / 法向零向量。
    static bool planeResiduals(const CloudConstPtr& src, const float plane[4],
                               std::vector<float>& out_residuals);

    // ---------------- segment：分割与聚类 ----------------

    /// 平面分割：RANSAC 拟合主平面，输出局内点云与局外点云。
    /// [out] out_inliers   主平面局内点（拟合成功保证非空）
    /// [out] out_outliers  其余点（可能为空点云，不为 nullptr）
    /// [out] out_coeffs    平面方程 [a,b,c,d]
    static bool segmentPlaneRANSAC(const CloudConstPtr& src, double distance_threshold,
                                   CloudPtr& out_inliers, CloudPtr& out_outliers,
                                   std::vector<float>& out_coeffs);

    /// 欧式聚类：按邻域连通性拆分为多个独立点云簇。
    /// [in] cluster_tolerance  连通判定距离（两点距离小于它视为同簇）
    /// [in] min_size/max_size  簇点数上下限（范围外的簇丢弃）
    /// 返回：按点数降序排列的簇列表。
    static std::vector<CloudPtr> segmentEuclideanCluster(const CloudConstPtr& src,
                                                         double cluster_tolerance,
                                                         int min_size, int max_size);

    /// DBSCAN 密度聚类（Open3D cluster_dbscan）：核心点 = 半径 eps 内邻居数 ≥ min_points，
    /// 噪声点单独标记。
    /// [in] eps         邻域半径
    /// [in] min_points  核心点所需最小邻居数（含自身）
    /// [out] out_labels 按"剔除非有限点后的 clean 云"对齐的簇标签（**与 src 不等长**：
    ///             labels[j] = clean 点 j 的标签，clean 点 j 对应 src 行 global[j]，
    ///             global 由 fitInputClean 产生；含 NaN/Inf 输入时调用方须自行映射回原行号）；
    ///             -1 = 噪声，≥0 = 簇号（按簇大小降序编号）
    /// 返回：簇数量（不含噪声）
    /// 注意：PCL 1.9 无 DBSCAN，内部用 KdTree 半径搜索 + 并查集实现。
    static int segmentDBSCAN(const CloudConstPtr& src, double eps, int min_points,
                             std::vector<int>& out_labels);

    /// 区域生长分割（PCL RegionGrowing，对应 Open3D detect_planar_patches 的平滑区域）：
    /// 以法线夹角 + 曲率双阈值把平滑连通的点聚成区域。
    /// [in] normals          与 src 一一对应的法线
    /// [in] k_neighbors      邻域点数（典型 30~50）
    /// [in] smoothness_deg   法线夹角阈值（度；越小越严格，典型 3~10）
    /// [in] curvature_thresh 曲率阈值（典型 0.05~1.0）
    /// [in] min_size/max_size 区域点数上下限
    /// 返回：区域索引列表（按点数降序）。
    static std::vector<std::vector<int>> segmentRegionGrowing(
        const CloudConstPtr& src, const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
        int k_neighbors, double smoothness_deg, double curvature_thresh,
        int min_size, int max_size);

    /// 多平面迭代分割（阶梯/挡墙类多平面工件）：RANSAC 拟合 → 剔除局内点 → 重复，
    /// 直到达 max_planes 或剩余点不足以构成 min_inliers 的平面。
    /// [in] distance_threshold  平面局内点阈值（与坐标同单位）
    /// [in] max_planes          最多平面数（>=1）
    /// [in] min_inliers         单平面最少局内点数（不足即停止；>=3）
    /// [out] out_coeffs   各平面方程 [a,b,c,d]（|n|=1，按检出顺序=点数降序趋势）
    /// [out] out_inliers  各平面局内点索引（相对原始 src）
    /// 返回：检出平面数（0 = 无有效平面）。
    static int segmentPlanesMulti(const CloudConstPtr& src, double distance_threshold,
                                  int max_planes, int min_inliers,
                                  std::vector<std::vector<float>>& out_coeffs,
                                  std::vector<std::vector<int>>& out_inliers);

    // ---------------- regist：配准与位姿估计 ----------------

    /// ICP 点到点配准：求 source → target 的刚体变换。
    /// [in]  max_dist      对应点最大搜索距离（太大易错配，太小不收敛）
    /// [out] out_transform 行主序 4x4：p_target = T · p_source
    /// 返回：false = 未收敛。失败时 out_transform 内容未定义。
    /// [in] init_transform  初始变换（可为 nullptr = 单位阵）。
    ///       配准是局部优化，初值越接近真值越可靠——手工粗调后精配准必须传它。
    /// [in] max_iteration  最大迭代数（0 = PCL 默认 60；多尺度金字塔按层级传）
    static bool registICP_PointToPoint(const CloudConstPtr& source, const CloudConstPtr& target,
                                       double max_dist, float out_transform[16],
                                       const float* init_transform = nullptr,
                                       int max_iteration = 0);

    /// ICP 点到面配准（收敛更快更稳，需要 target 法线）。
    /// [in] target_normals  target 的法线（与 target 点一一对应，数量必须一致）
    /// 注意：PCL 1.9 的点到面 LLS 求解在纯平面等退化几何上会产出 NaN 变换，
    ///       此时内部自动回退到点到点 ICP；两者都失败才返回 false。
    static bool registICP_PointToPlane(const CloudConstPtr& source, const CloudConstPtr& target,
                                       const pcl::PointCloud<pcl::Normal>::Ptr& target_normals,
                                       double max_dist, float out_transform[16],
                                       const float* init_transform = nullptr);

    /// NDT 配准（正态分布变换，大初始偏差下比 ICP 稳健）。
    /// [in] voxel_size  NDT 格子边长（典型 1.0，单位与点云一致）
    static bool registNDT(const CloudConstPtr& source, const CloudConstPtr& target,
                          double voxel_size, float out_transform[16]);

    /// PCA 粗配准：两片点云质心对齐 + 主轴对齐（无迭代，作 ICP 初值）。
    /// 对称物体的主轴可能翻转 180°，仅适合明显不对称的形状。
    static bool registPCA(const CloudConstPtr& source, const CloudConstPtr& target,
                          float out_transform[16]);

    /// 广义 ICP（Open3D registration_generalized_icp）：以局部协方差做各向异性代价，
    /// 对平面/圆柱类工件比点到点 ICP 更稳。
    /// [in]  max_dist      对应点最大搜索距离
    /// [out] out_transform p_target = T · p_source
    static bool registGICP(const CloudConstPtr& source, const CloudConstPtr& target,
                           double max_dist, float out_transform[16]);

    /// 基于特征的全局配准（Open3D registration_ransac_based_on_feature_matching）：
    /// FPFH 对应关系 + SAC-IA 采样一致性求初值，无需初始位姿。
    /// [in] source_features/target_features  FPFH 描述子（与点云一一对应）
    /// [in] min_sample_distance  采样点对最小距离（通常取点云尺度 0.1~0.3 倍）
    /// [in] max_correspondence_distance  对应点距离阈值
    /// 返回：false = 未收敛。
    static bool registFeatureMatching(const CloudConstPtr& source, const CloudConstPtr& target,
                                      const pcl::PointCloud<pcl::FPFHSignature33>::ConstPtr& source_features,
                                      const pcl::PointCloud<pcl::FPFHSignature33>::ConstPtr& target_features,
                                      double min_sample_distance,
                                      double max_correspondence_distance,
                                      float out_transform[16]);

    /// 配准质量评估（Open3D evaluate_registration）：统计变换后 source 到 target
    /// 对应点距离 ≤ max_dist 的占比与均方误差。
    /// [out] out_fitness     内点占比 [0,1]（越接近 1 越好）
    /// [out] out_inlier_rmse 内点均方根误差（与坐标同单位；无内点时 0）
    /// 返回：false = 输入为空
    static bool registEvaluate(const CloudConstPtr& source, const CloudConstPtr& target,
                               const float transform[16], double max_dist,
                               double& out_fitness, double& out_inlier_rmse);

    /// 配准信息矩阵（Open3D get_information_matrix_from_point_clouds）：
    /// 由对应点协方差近似，用于位姿图优化中各边的权重。
    /// [out] out_information 行主序 6x6（顺序 [rx,ry,rz,tx,ty,tz]）
    static bool registInformationMatrix(const CloudConstPtr& source, const CloudConstPtr& target,
                                        const float transform[16], double max_dist,
                                        double out_information[36]);

    /// 多尺度 ICP（Open3D multiscale_icp）：voxel 金字塔由粗到细逐级点到点 ICP，
    /// 每级在 filterVoxelGrid 下采样云上进行，上一级变换作下一级初值（PCL align(init)
    /// 的 final 已含 init，直接链式传递）；每级对应距离 = max_dist_factor × 该级 voxel
    /// ——粗级容差大锁全局位置，细级容差小做精修。
    /// 适用：大点云 / 初值偏差大于末级对应距离的场景（如 moi_wafer 48 万点，
    /// 典型 voxel {2.5, 1.0, 0.3}、iters {80, 60, 60}、factor 2.0）。
    /// voxel_sizes 为空时退化为原始云上的单级 ICP（对应距离 = max_dist_factor，
    /// 此时 factor 是距离本身而非系数；<=0 无法确定距离，返回 false）。
    /// [in] voxel_sizes  各级体素边长（粗→细，同点云单位；每级从原始点云独立下采样）
    /// [in] iters        各级最大迭代数（与 voxel_sizes 对应；数量不足补最后一值；0 = PCL 默认）
    /// [in] max_dist_factor  每级对应距离 = factor × voxel（<=0 时取 1.5）
    /// [in] init_transform   初始变换（可为 nullptr = 单位阵）
    /// 返回：false = 输入无效或某级未收敛。失败时 out_transform 内容未定义。
    static bool registMultiscaleICP(const CloudConstPtr& source, const CloudConstPtr& target,
                                    const std::vector<double>& voxel_sizes,
                                    const std::vector<int>& iters,
                                    double max_dist_factor, float out_transform[16],
                                    const float* init_transform = nullptr);

    // ---------------- tree：空间索引与近邻搜索 ----------------

    /// KdTree K 近邻搜索。
    /// [in]  query_point   查询点 [x,y,z]
    /// [in]  k             邻居个数（超过点数时按实际点数返回）
    /// [out] out_indices  近邻点索引（按距离升序）
    /// [out] out_distances 对应欧氏距离（平方距离语义见 PCL 文档；升序对应）
    static void treeKdTreeKNN(const CloudConstPtr& src, const float query_point[3], int k,
                              std::vector<int>& out_indices, std::vector<float>& out_distances);

    /// KdTree 半径搜索：返回半径内全部点。
    /// [in] radius  搜索半径；src 为空/点云为空时输出空列表
    static void treeKdTreeRadiusSearch(const CloudConstPtr& src, const float query_point[3],
                                       double radius, std::vector<int>& out_indices,
                                       std::vector<float>& out_distances);

    // Octree 体素搜索：返回查询点所在体素内的全部点索引。
    // resolution：体素边长（与点云同单位；默认 1.0 适合米制，mm/像素单位请显式传）
    static void treeOctreeVoxelSearch(const CloudConstPtr& src, const float query_point[3],
                                      std::vector<int>& out_indices,
                                      double resolution = 1.0);

    /// Octree 变化检测：src2 中相对 src1 新出现（体素变化）的点索引。
    /// [in] min_resolution  体素分辨率（决定"变化"的空间粒度）
    static void treeOctreeChangeDetection(const CloudConstPtr& src1, const CloudConstPtr& src2,
                                          double min_resolution, std::vector<int>& out_new_indices);

    // ---------------- surface：表面重建 ----------------

    // 统一约定（适用于下方所有需要法线的重建接口）：
    //   * 法线必须与 src 一一对应（size 相等），否则返回 valid=false；
    //   * 入口自动剔除 NaN / Inf / 零长度法线的点（PCL 遇 NaN 法线不报错但结果不可用）；
    //   * 有效法线占比 < 50% 时整体拒绝（valid=false），用于拦截上游法线估计失效；
    //   * 重建前应先用 featureOrientNormals* 统一法线朝向，否则表面翻面/空洞。

    /// 3D 凸包：包裹全部点的最小凸面。平面点集返回其 2D 凸包轮廓点。
    /// 返回：凸包顶点点云；src 为空/共线退化时返回 nullptr。
    static CloudPtr surfaceConvexHull(const CloudConstPtr& src);

    /// 凹包/Alpha Shapes（Open3D create_from_point_cloud_alpha_shape 的等价物，
    /// PCL ConcaveHull）：alpha 越小越贴合点云细节，越大越接近凸包。
    /// 返回：凹包顶点点云；src 为空/退化返回 nullptr。
    static CloudPtr surfaceConcaveHull(const CloudConstPtr& src, double alpha);

    /// MLS 平滑与重采样（Open3D smooth_mls）：局部多项式拟合投影，
    /// 同时正平噪声并可按体素上采样补点。
    /// [in] radius           搜索半径（须 > 点云平均间距）
    /// [in] polynomial_order 拟合阶数（1=平面，2=二次曲面；典型 2）
    /// [in] voxel_size       上采样体素边长（≤0 关闭上采样，仅投影平滑）
    /// [out] out_normals     可选输出：每个输出点的法线（传 nullptr 不输出）
    /// 返回：平滑后的点云；src 为空/点数不足返回 nullptr。
    static CloudPtr surfaceSmoothMLS(const CloudConstPtr& src, double radius,
                                     int polynomial_order = 2, double voxel_size = 0.0,
                                     pcl::PointCloud<pcl::Normal>::Ptr* out_normals = nullptr);

    /// Poisson 表面重建（Open3D create_from_point_cloud_poisson）：
    /// 需要一致朝向的法线，输出闭合网格（水密表面）。
    /// [in] normals       与 src 一一对应的法线（必填）
    /// [in] depth         八叉树最大深度（8~12；越大细节越多、越慢，典型 9）
    /// [in] iso_divide    等值面提取的八叉树划分深度（典型 8，需 ≤ depth）
    /// [in] samples_per_node 每节点采样数（1.0 适合噪声小的数据，1.5~2 抗噪）
    /// 返回：三角网格；失败时 Mesh3d::valid == false。
    static Mesh3d surfacePoisson(const CloudConstPtr& src,
                                 const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                 int depth = 9, int iso_divide = 8,
                                 double samples_per_node = 1.0);

    /// 贪婪投影三角化（PCL GreedyProjectionTriangulation）：
    /// 适用于有序/密度较均匀的扫描点云，保持原始点作顶点。
    /// [in] normals       与 src 一一对应的法线（必填）
    /// [in] search_radius 邻域搜索半径（典型取点云平均间距的 2~5 倍）
    /// [in] mu            最近邻距离倍数 → 自适应搜索半径（典型 2.5~3.0）
    /// [in] max_neighbors 最大近邻数（典型 50~100）
    /// 返回：三角网格；失败时 Mesh3d::valid == false。
    static Mesh3d surfaceGreedyProjection(const CloudConstPtr& src,
                                          const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                          double search_radius, double mu = 2.5,
                                          int max_neighbors = 100);

    /// Marching Cubes 重建（PCL MarchingCubesHoppe，Hoppe 符号距离场）：
    /// 无需闭合点云，按法线方向构建隐式场；适合扫描片重建。
    /// [in] normals  与 src 一一对应的法线（必填）
    /// [in] resolution  三轴网格分辨率（典型 50~150；越大越精细）
    static Mesh3d surfaceMarchingCubes(const CloudConstPtr& src,
                                       const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                       int resolution = 64);

    /// Marching Cubes RBF 重建（PCL MarchingCubesRBF）：用径向基函数拟合符号距离场，
    /// 比 Hoppe 版更平滑，适合有噪声/稀疏的扫描片；计算量更大（需解 2N×2N 线性方程组）。
    /// [in] normals     与 src 一一对应的法线（必填）
    /// [in] resolution  三轴网格分辨率（典型 50~120）
    /// [in] off_surface_epsilon  离面约束点的**绝对位移量**（与坐标同单位，不是比例），
    ///       典型取点云平均间距的 0.1~0.3 倍。过小则约束不足、过大则表面膨胀。
    static Mesh3d surfaceMarchingCubesRBF(const CloudConstPtr& src,
                                          const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                          int resolution = 64,
                                          double off_surface_epsilon = 0.1);

    /// 网格投影重建（PCL GridProjection）：把点云投影到体素网格上做极值面提取，
    /// 输出带流形保证的闭合网格。
    /// [in] normals     与 src 一一对应的法线（必填）
    /// [in] resolution  体素边长（与坐标同单位，典型取点云平均间距的 1~2 倍）
    /// [in] padding_size 邻域填充格数（0~3；越大越能补小孔洞）
    /// 注意（PCL 1.9 实现限制）：
    ///   ① **要求每体素邻域 ≥10 个点**（硬编码），稀疏表面点云会返回空网格；
    ///   ② 输出顶点是体素边上的插值点（阶梯状表面），精度约 leaf×padding，
    ///      不能当作精确拟合曲面用于高精度测量；
    ///   ③ 输入包围盒最大边长 > 1 时 PCL 会把点云归一化，但**输出只做了尺度还原、
    ///      绝对位置仍带偏移**——需要绝对坐标时，请先把点云平移到单位盒内再调用；
    ///   ④ 输出四边形面，本库已统一扇形三角化。
    static Mesh3d surfaceGridProjection(const CloudConstPtr& src,
                                        const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                        double resolution, int padding_size = 1);

    /// 有序点云快速三角化（PCL OrganizedFastMesh）：把 organized 点云的像素邻接
    /// 直接连成三角面，速度最快、无需法线；仅适用于 height > 1 的有序点云
    /// （深度相机/结构光相机的规则栅格输出）。
    /// [in] triangle_pixel_size  三角化步长（像素；1 = 相邻像素成面，2 = 隔 2 像素采样）
    /// [in] adaptive_cut         true = 按 z 差异自适应选择对角线（否则固定左上→右下）
    /// 注意：① 内部按视点（默认原点）做遮挡剔除，点云应位于视点前方；
    ///      ② 步长 > 1 时边缘 (step-1) 行/列不参与三角化；
    ///      ③ adapt 模式会剔除退化/阴影三角形，面数不保证等于 2×(w-step)×(h-step)。
    /// 返回：三角网格；输入非有序或为空时 valid == false。
    static Mesh3d surfaceOrganizedFastMesh(const CloudConstPtr& src,
                                           int triangle_pixel_size = 1,
                                           bool adaptive_cut = true);

    /// 网格拉普拉斯平滑（umbrella 算子，PCL 1.9 无 VTK 网格平滑的可用封装）：
    /// 每个顶点向邻居质心移动，抑制表面锯齿。
    /// [in] iterations  迭代次数（典型 5~20）
    /// [in] lambda      步长 [0,1]（典型 0.3~0.6）
    /// 注意：**拉普拉斯平滑会使曲面整体收缩**——球面半径随迭代单调减小
    ///      （实测球 R=10、λ=0.5：5 次→9.958，10 次→9.912，50 次→9.558）。
    ///      需要保形时减小 iterations/lambda；需要无收缩平滑请用 Taubin（λ/μ 交替），
    ///      PCL 1.9 未提供，可按本函数同样的邻接结构自行扩展。
    /// 返回：平滑后的网格（顶点数/面数不变）；输入非法返回 valid=false。
    static Mesh3d surfaceMeshLaplacianSmooth(const Mesh3d& mesh,
                                             int iterations = 10, double lambda = 0.5);

    /// 网格顶点聚类抽稀：把 cell 边长体素内的顶点合并为质心，面索引重映射。
    /// [in] cell  聚类体素边长（与坐标同单位；<=0 返回 valid=false）
    /// 顶点合并后退化三角（顶点重合）与重复面自动剔除。
    /// 注意：顶点聚类法快速粗糙，不保证流形/拓扑——保形减面请用 PCL 原生
    ///       VTK 系（MeshQuadricsDecimation，需 VTK 支持）。
    /// 返回：抽稀后网格；输入非法/无有效面返回 valid=false。
    static Mesh3d surfaceMeshClusterDecimate(const Mesh3d& mesh, double cell);

    /// 网格总表面积（Σ 面面积；无效网格返回 0）。
    static double measureMeshArea(const Mesh3d& mesh);

    /// 网格有向体积（散度定理：Σ v0·(v1×v2)/6，单位³）。
    /// 仅当网格闭合（水密）且面绕向一致朝外时为真实体积；开放网格/绕向混乱
    /// 的返回值无物理意义（仅作口径参考）。
    /// 返回：无效网格返回 0。
    static double measureMeshVolume(const Mesh3d& mesh);

    /// 点云到网格的最近距离（点到三角面精确最近点，暴力 O(n·f)——
    /// 网格大时先用 surfaceMeshClusterDecimate 减面）。
    /// [in] mesh  目标网格（索引越界的面跳过）
    /// 返回：与 src 等长的距离数组；src 为空/mesh 无效返回空数组。
    static std::vector<float> measurePointToMeshDistance(const CloudConstPtr& src,
                                                         const Mesh3d& mesh);

    /// 网格每面单位法线（按顶点序叉积；退化面/索引越界面 = (0,0,0)）。
    /// 用途：重建质量评估、偏差方向判定、渲染。
    /// 返回：与 triangles 等长的法线数组（每面 [nx,ny,nz] 3 个 float，行主序）；
    ///       无效网格返回空数组。
    static std::vector<float> meshFaceNormals(const Mesh3d& mesh);

    /// 网格每顶点法线（相邻面法线按面积加权平均——累加未归一叉积后单位化；
    /// 孤立/退化顶点 = (0,0,0)）。
    /// 返回：与 vertices 等长的法线数组（每顶点 [nx,ny,nz]）；无效网格返回空数组。
    static std::vector<float> meshVertexNormals(const Mesh3d& mesh);

    /// 按 OBB 裁剪网格（面级过滤：三顶点全部落在盒内的面保留）。
    /// 输出顶点 = 保留面的顶点并集（重新编号），面索引同步重映射。
    /// 返回：Mesh3d（valid=false = 无保留面 / 输入无效 / box 无效）。
    static Mesh3d meshCropByOBB(const Mesh3d& mesh, const OBB3d& box);

    /// 点云到闭合网格的带符号距离（内负外正）。
    /// 口径：距离 = 点-三角面精确最近距离（同 measurePointToMeshDistance）；
    /// 符号 = 从点沿 +X 发射射线，穿越三角面次数奇偶（奇 = 内）。
    /// 注意：仅闭合（水密）网格符号有物理意义；非水密网格符号无意义（距离仍准）。
    /// 非有限点输出 NaN。返回：src 为空/mesh 无效返回空数组。
    static std::vector<float> measurePointToMeshSignedDistance(const CloudConstPtr& src,
                                                               const Mesh3d& mesh);

    // ---------------- measure：包围盒与统计度量 ----------------

    /// 三轴独立范围（Open3D get_min_bound/get_max_bound）：输出最小/最大角点。
    /// 返回：false = src 为空
    static bool measureMinMax(const CloudConstPtr& src, Point3d& out_min, Point3d& out_max);

    /// 轴对齐包围盒（Open3D get_axis_aligned_bounding_box）。
    /// AABB3d::valid == false 表示 src 为空。
    static AABB3d measureAABB(const CloudConstPtr& src);

    /// 有向包围盒（Open3D get_oriented_bounding_box）：PCA 主轴对齐，
    /// 语义与 Stereo::getOBB 一致（extents = 半长，rotation 行 = 主轴）。
    static OBB3d measureOBB(const CloudConstPtr& src);

    /// 质心（Open3D get_center）：点数加权平均，与 Stereo::getCentroid 同语义。
    /// 返回：false = src 为空
    static bool measureCentroid(const CloudConstPtr& src, Point3d& out_centroid);

    /// 均值与协方差（Open3D compute_mean_and_covariance）：
    /// [out] out_mean    质心 [x,y,z]
    /// [out] out_cov     3x3 协方差，行主序 9 个 double
    /// 返回：false = 点数 < 2
    static bool measureMeanAndCovariance(const CloudConstPtr& src, Point3d& out_mean,
                                         double out_cov[9]);

    /// 点云间逐点最近距离（Open3D compute_point_cloud_distance）：
    /// 对 src 每个点求到 target 的最近距离，常用于配准前后偏差/形变比对。
    /// 返回：与 src 等长的距离数组；src/target 为空返回空数组。
    static std::vector<float> measurePointCloudDistance(const CloudConstPtr& src,
                                                        const CloudConstPtr& target);

    /// 平面度（GD&T 简化口径）：RANSAC 主平面局内点到平面的有符号残差峰峰值。
    /// [out] out_flatness  峰峰值 max-min（与坐标同单位）
    /// [out] out_coeffs    平面方程（同 fitPlane，|n|=1）
    /// 返回：false = 拟合失败 / 局内点 < 2。
    static bool measureFlatness(const CloudConstPtr& src, double distance_threshold,
                                double& out_flatness, std::vector<float>& out_coeffs);

    /// 两平面夹角（度）：acos(|n1·n2|)，取锐角口径 [0, 90]。
    /// 返回：-1 = 任一法向非有限/零向量。
    static double measurePlaneAngle(const float plane1[4], const float plane2[4]);

    /// 两平面交线。
    /// [out] out_point  交线上一点（三方程 n1·x=-d1, n2·x=-d2, dir·x=0 的解 = 最靠近原点的交点）
    /// [out] out_dir    交线方向（n1×n2 单位化）
    /// 返回：false = 非法输入 / 近平行（|n1×n2| < 1e-9，无交线或交线在无穷远）。
    /// 用途：折弯件两面的交棱定位、两面夹角判定（配合 measurePlaneAngle）。
    static bool measurePlaneIntersection(const float plane1[4], const float plane2[4],
                                          float out_point[3], float out_dir[3]);

    // ---------------- heightmap：3D → 2D 桥接 ----------------

    /// 点云 XY 栅格化为高度图：3D AOI 结果接入 2D ImageProcessor 全部算子的桥
    ///（阈值/滤波/轮廓/连通域/模板匹配直接可用）。
    /// [in] cell  栅格边长（与坐标同单位；<=0 返回 invalid）
    /// [in] mode  聚合方式：0=取最高 z，1=取最低 z，2=取平均 z，3=点计数密度
    ///            （其他值返回 invalid）
    /// 坐标映射：col=(x-x_min)/cell、row=(y-y_min)/cell（row 0 = y 最小）；
    /// 空栅格 z=NaN、mask=0（mode=3 时空格计数值=NaN）；非有限坐标/高度的点自动剔除。
    /// 总栅格数上限 2000 万（超出返回 invalid，防 cell 过小撑爆内存）。
    static HeightMap cloudToHeightMap(const CloudConstPtr& src, double cell, int mode = 0);

    /// 高度图反向点云化：每个有效格（mask=1）生成一个点，坐标取格心
    /// x = x0 + (col+0.5)·cell、y = y0 + (row+0.5)·cell、z = cells（保留 rgb/intensity 为 0）。
    /// 返回：invalid/空图返回 nullptr。
    static CloudPtr heightMapToCloud(const HeightMap& hm);

    /// 厚度图：同一工件上/下表面两点云沿 Z 配对求差（板厚/锡膏厚度测量）。
    /// 实现：网格范围取两云有效点 XY 包围盒的并集；上云每格取最高 z、下云每格取
    /// 最低 z；同格双有效 → 厚度 = zTop − zBottom（cells），mask=1；任一缺失 = NaN/0。
    /// [in] cell  栅格边长（与坐标同单位；<=0 返回 invalid；上限同 cloudToHeightMap）
    /// 返回：HeightMap（valid=false = 任一云为空/非法/超内存上限）。
    static HeightMap thicknessMap(const CloudConstPtr& top, const CloudConstPtr& bottom,
                                  double cell);
};

} // namespace shapely

#endif // SHAPELY_CLOUD_PROCESSOR_H
