// ShapelyAlg_demo.cpp —— 第三方调用实例（完整可运行，覆盖典型 AOI 流程）。
//
// 场景一（2D）：缺陷区域检出与统计
//   合成图像 → 阈值二值化 → 轮廓提取 → 抽稀 → 转多边形 → 面积过滤
//   → PreparedGeom ROI 相交判定 → 标注图保存
//
// 场景二（3D）：工件凸台测量
//   合成点云（基面 + 凸台）→ AABB/OBB 体积对比 → 高度带过滤
//   → 2.5D 投影取凸台占地面积 → 质心 → PLY 落盘
//
// 场景三（3D）：点云表面重建
//   合成球面扫描点云 → 法线估计 → 朝向一致化（方向/全局）
//   → Poisson 重建 → 精度校验 → 网格平滑 → 其他重建方法对比
//
// 第三方接入只需三步：
//   1. #include "ShapelyAlg.h"
//   2. 链接 ShapelyAlg.lib，同目录放置 ShapelyAlg.dll 与其依赖 DLL（见 README §2）
//   3. 按本文件的调用方式组合各模块

#include "ShapelyAlg.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>

#include <cmath>
#include <cstdio>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// =====================================================================
// 场景一（2D）：缺陷区域检出与统计
// =====================================================================
static void demo2D()
{
    std::printf("========== 场景一：2D 缺陷区域检出 ==========\n");

    // ---- 1. 合成待检图像（300x300：两个"缺陷"块 + 三个噪声小点）----
    cv::Mat img(300, 300, CV_8UC1, cv::Scalar(0));
    cv::rectangle(img, cv::Rect(30, 30, 40, 30), 255, -1);        // 缺陷 A：矩形
    cv::circle(img, cv::Point(200, 80), 25, 255, -1);             // 缺陷 B：圆 r=25
    cv::rectangle(img, cv::Rect(50, 200, 5, 5), 255, -1);         // 噪声
    cv::rectangle(img, cv::Rect(60, 205, 5, 5), 255, -1);         // 噪声
    cv::rectangle(img, cv::Rect(70, 210, 5, 5), 255, -1);         // 噪声

    // ---- 2. 二值化（业务层直接传 cv:: 枚举）----
    cv::Mat binary;
    shapely::ImageProcessor::threshold(img, binary, 127, 255, cv::THRESH_BINARY);

    // ---- 3. 轮廓提取（像素中心坐标）----
    std::vector<std::vector<cv::Point>> contours;
    shapely::ImageProcessor::findContours(binary, contours,
                                          cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    // ---- 4. 逐轮廓：抽稀 → 转多边形 → 有效性/面积过滤（业务规则：>= 200px^2）----
    struct Defect { shapely::Geom poly; double area; };
    std::vector<Defect> defects;
    for (const auto& c : contours) {
        // OpenCV 轮廓动辄上百点，先抽稀再做几何运算
        shapely::Geom poly = shapely::Geom::createPolygon(c).simplify(0.3);
        if (poly.isEmpty() || !poly.isValid())
            continue;                                   // 退化/自交面剔除
        const double area = poly.area();
        if (area < 200.0)
            continue;                                   // 业务规则：小面积是噪声
        // Geom 只可移动不可拷贝（RAII 防 Double Free），Move 进去而非拷贝
        defects.push_back(Defect{ std::move(poly), area });
    }
    std::printf("轮廓 %zu 个 → 有效缺陷 %zu 个（面积阈值 200px^2）\n",
                contours.size(), defects.size());

    // ---- 5. PreparedGeom ROI 判定：只统计右半区（150..300 列）内的缺陷 ----
    // 重复判定前先 prepare，批量场景比普通谓词快约一个数量级
    const shapely::Geom roiPoly = shapely::GeomCVAdapter::createRect(cv::Rect2d(150, 0, 150, 300));
    const shapely::PreparedGeom roi = shapely::PreparedGeom::prepare(roiPoly);

    // ---- 6. 标注输出 ----
    cv::Mat annotated;
    cv::cvtColor(img, annotated, cv::COLOR_GRAY2BGR);
    cv::rectangle(annotated, cv::Rect(150, 0, 150, 300), cv::Scalar(0, 0, 255), 2);  // ROI 红框

    double inRoiTotal = 0.0;
    int id = 0;
    for (const Defect& d : defects) {
        const bool inRoi = roi.intersects(d.poly);
        const shapely::BoundingBox2d bb = d.poly.bounds();
        const shapely::Point2d ct = d.poly.centroid();

        std::printf("  缺陷#%d: 面积 %8.1f px^2  质心 (%6.1f, %6.1f)  包围盒 %4.0fx%4.0f  %s\n",
                    id, d.area, ct.x, ct.y, bb.width(), bb.height(),
                    inRoi ? "[ROI 内]" : "[ROI 外]");

        if (inRoi) {
            // 求交用原 Geom（PreparedGeom 只提供快速谓词，不做布尔运算）
            inRoiTotal += roiPoly.intersection(d.poly).area();
            cv::rectangle(annotated,
                          cv::Rect((int)bb.min_x, (int)bb.min_y,
                                   (int)bb.width(), (int)bb.height()),
                          cv::Scalar(0, 255, 0), 2);
        }
        ++id;
    }
    std::printf("ROI 面积 %.0f px^2，ROI 内缺陷覆盖 %.1f px^2（%.2f%%）\n",
                roiPoly.area(), inRoiTotal, 100.0 * inRoiTotal / roiPoly.area());

    cv::imwrite("shapely_demo_annotated.png", annotated);
    std::printf("标注图已保存: shapely_demo_annotated.png\n\n");
}

// =====================================================================
// 场景二（3D）：工件凸台测量
// =====================================================================
static void demo3D()
{
    std::printf("========== 场景二：3D 工件凸台测量 ==========\n");

    // ---- 1. 合成点云：20x20mm 基面（z=0，间距 1mm）+ 6x6mm 凸台（z=2，间距 0.5mm）----
    std::vector<shapely::Point3d> pts;
    for (double x = 0; x <= 20.0; x += 1.0)
        for (double y = 0; y <= 20.0; y += 1.0)
            pts.push_back(shapely::Point3d{ x, y, 0.0 });
    const size_t baseCount = pts.size();
    for (double x = 7.0; x <= 13.0; x += 0.5)
        for (double y = 7.0; y <= 13.0; y += 0.5)
            pts.push_back(shapely::Point3d{ x, y, 2.0 });

    // ---- 2. 装入 Stereo（鸭子类型：std::vector<shapely::Point3d> 直接可用）----
    const shapely::Stereo cloud = shapely::Stereo::createFromPoints(pts);
    std::printf("点云 %zu 点（基面 %zu + 凸台 %zu）\n",
                cloud.size(), baseCount, cloud.size() - baseCount);

    // ---- 3. 包围盒对比：OBB 显著小于 AABB（更贴合真实物体）----
    const shapely::AABB3d aabb = cloud.getAABB();
    const shapely::OBB3d  obb  = cloud.getOBB();
    std::printf("AABB %.1f x %.1f x %.1f，体积 %.0f mm^3\n",
                aabb.max_x - aabb.min_x, aabb.max_y - aabb.min_y,
                aabb.max_z - aabb.min_z, aabb.volume());
    std::printf("OBB  体积 %.0f mm^3（含凸台全高）\n", obb.volume());

    // ---- 4. 质心（凸台抬高整体质心 Z）----
    const shapely::Point3d c = cloud.getCentroid();
    std::printf("质心 (%.2f, %.2f, %.3f)\n", c.x, c.y, c.z);

    // ---- 5. 高度带过滤：只看凸台层（z ∈ [1, 3]）----
    const shapely::Stereo boss = cloud.filterByZRange(1.0, 3.0);
    std::printf("凸台层（z∈[1,3]）: %zu 点\n", boss.size());

    // ---- 6. 2.5D 投影：凸包面积 = 凸台占地面积（期望 6x6=36mm^2）----
    const shapely::Geom footprint = boss.projectTo2D();
    std::printf("凸台占地（2.5D 投影面积）: %.2f mm^2\n", footprint.area());

    // ---- 7. 凸台顶面到基面的距离（验证高度 = 2mm）----
    const shapely::Stereo base = cloud.filterByZRange(-0.1, 0.1);
    std::printf("凸台到基面最短距离: %.3f mm\n", boss.minDistanceTo(base));

    // ---- 8. PLY 落盘（第三方可用任何 PLY 工具查看）----
    pcl::PointCloud<pcl::PointXYZ>::Ptr pc(new pcl::PointCloud<pcl::PointXYZ>);
    pc->resize(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        (*pc)[i].x = (float)pts[i].x;
        (*pc)[i].y = (float)pts[i].y;
        (*pc)[i].z = (float)pts[i].z;
    }
    pc->width = (uint32_t)pc->size();
    pc->height = 1;
    shapely::CloudIO<pcl::PointXYZ>::ioSavePLY("shapely_demo_cloud.ply", pc, true);
    std::printf("点云已保存: shapely_demo_cloud.ply\n");
}

// =====================================================================
// 场景三（3D）：点云表面重建（法线一致化 → Poisson → 网格平滑）
// =====================================================================
static void demoSurfaceReconstruction()
{
    std::printf("========== 场景三：点云表面重建 ==========\n");
    using CP = shapely::CloudProcessor<pcl::PointXYZ>;

    // ---- 1. 合成球面扫描点云（半径 10mm，模拟结构光扫描片）----
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
    const int lat = 20, lon = 40;
    for (int i = 1; i < lat; ++i) {
        const double theta = M_PI * i / lat;
        for (int j = 0; j < lon; ++j) {
            const double phi = 2.0 * M_PI * j / lon;
            cloud->push_back(pcl::PointXYZ(
                (float)(10.0 * std::sin(theta) * std::cos(phi)),
                (float)(10.0 * std::sin(theta) * std::sin(phi)),
                (float)(10.0 * std::cos(theta))));
        }
    }
    cloud->width = (uint32_t)cloud->size();
    cloud->height = 1;
    std::printf("输入点云: %zu 点\n", cloud->size());

    // ---- 2. 估计法线 ----
    auto normals = CP::featureNormalEstimation(cloud, 20);
    if (!normals) {
        std::printf("法线估计失败\n");
        return;
    }

    // ---- 3. 法线朝向一致化（重建前必做，否则表面翻面/空洞）----
    // 场景 A：已知扫描朝向（朝外）→ 直接对齐参考方向
    const float outward[3] = { 0.0f, 0.0f, 1.0f };
    auto oriented = CP::featureOrientNormalsToDirection(normals, outward);
    // 场景 B：无先验朝向 → KNN 图 + MST 全局一致化
    auto consistent = CP::featureOrientNormalsConsistentTangentPlane(cloud, normals, 30);
    if (!oriented || !consistent) {
        std::printf("法线一致化失败\n");
        return;
    }
    std::printf("法线: %zu 条（已做方向对齐 + 全局一致化）\n", oriented->size());

    // ---- 4. Poisson 重建（水密闭合曲面）----
    shapely::Mesh3d mesh = CP::surfacePoisson(cloud, consistent, 8, 7, 1.0);
    if (!mesh.valid) {
        std::printf("Poisson 重建失败\n");
        return;
    }
    std::printf("Poisson 网格: %zu 顶点 / %zu 三角面\n",
                mesh.vertices.size(), mesh.triangles.size());

    // ---- 5. 重建质量校验：顶点到球心距离应接近半径 10 ----
    double max_err = 0.0, sum_err = 0.0;
    for (const shapely::Point3d& v : mesh.vertices) {
        const double r = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        const double e = std::fabs(r - 10.0);
        max_err = std::max(max_err, e);
        sum_err += e;
    }
    std::printf("重建精度: 半径误差 最大 %.4fmm / 平均 %.4fmm\n",
                max_err, sum_err / (double)mesh.vertices.size());

    // ---- 6. 网格拉普拉斯平滑（去锯齿；注意会整体收缩）----
    auto meanRadius = [](const shapely::Mesh3d& m) {
        double s = 0.0;
        for (const shapely::Point3d& v : m.vertices)
            s += std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return s / (double)m.vertices.size();
    };
    shapely::Mesh3d smooth = CP::surfaceMeshLaplacianSmooth(mesh, 5, 0.3);
    shapely::Mesh3d smooth20 = CP::surfaceMeshLaplacianSmooth(mesh, 20, 0.3);
    std::printf("平滑(5 次) : %zu 顶点 / %zu 三角面，均值半径 %.4fmm（原始 %.4fmm）\n",
                smooth.vertices.size(), smooth.triangles.size(),
                meanRadius(smooth), meanRadius(mesh));
    std::printf("平滑(20次) : 均值半径 %.4fmm（拉普拉斯固有收缩，迭代越多越小）\n",
                meanRadius(smooth20));

    // ---- 7. 其他重建方法对比（同一输入）----
    shapely::Mesh3d gp3 = CP::surfaceGreedyProjection(cloud, consistent, 3.0, 2.5, 100);
    std::printf("GP3     : %zu 顶点 / %zu 三角面（顶点取自原始点）\n",
                gp3.vertices.size(), gp3.triangles.size());

    shapely::Mesh3d mc = CP::surfaceMarchingCubes(cloud, consistent, 48);
    std::printf("MC-Hoppe: %zu 顶点 / %zu 三角面\n", mc.vertices.size(), mc.triangles.size());

    shapely::Mesh3d rbf = CP::surfaceMarchingCubesRBF(cloud, consistent, 48, 0.1);
    std::printf("MC-RBF  : %zu 顶点 / %zu 三角面\n", rbf.vertices.size(), rbf.triangles.size());
}

int main()
{
    demo2D();
    demo3D();
    demoSurfaceReconstruction();
    std::printf("\n演示完成\n");
    return 0;
}
