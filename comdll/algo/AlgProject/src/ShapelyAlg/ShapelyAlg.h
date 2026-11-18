#ifndef SHAPELYALG_H
#define SHAPELYALG_H

// ShapelyAlg.h —— shapely 库对外总入口（唯一需要 include 的头）。
// 完整使用说明（接入步骤/运行时 DLL 清单/全局约定/常见坑）见同目录 README.md；
// 全部公开接口的分模块清单见 dispatch/SHAPELY_API_STATUS.md。
//
// 库结构（单文件夹，公共头纯净 + 实现下沉 .cpp；FitUtil.h 为内部共享头，勿直接 include）：
//   Types.h           原生轻量结构（Point2d/3d、BoundingBox2d、AABB3d、OBB3d，
//                     编译期布局契约 static_assert ×10）
//   Geom.h            二维几何代数与拓扑（GEOS，Pimpl，公共头零 GEOS）
//                     含带孔多边形/unaryUnion/nearestPoints/makeValid/2D 仿射/
//                     toGeoJSON·toDXF 导出
//   Stereo.h          三维空间数据管理与度量（纯数学，Pimpl）
//                     含 KdTree3 空间索引（nearest/knn/radiusSearch）、
//                     点-盒/面距离与判定、meanNearestNeighborDistance
//   ImageProcessor.h  二维图像算子 24 组 78 接口（OpenCV，静态工具类，头仅 core/mat）
//                     含绘图标注/带掩膜与多目标匹配/亚像素/fitCircle(RANSAC)/霍夫
//   CloudProcessor.h  三维点云算子 12 模块 90 接口（PCL，显式实例化白名单）
//                     含 fitCircle3D/measureFlatness/多边形 ROI/高度图双向
//   CloudIO.h         文件读写：点云 PCD/PLY/TXT + 网格 OBJ/STL（MeshIO）+
//                     高度图 SHMAP1（HeightMapIO）
//
// 快速上手：
//   #include "ShapelyAlg.h"
//
//   // 二维：鸭子类型输入（cv::Point 系 / 含 .x/.y 的结构 / 扁平数组），3D 自动丢 Z
//   shapely::Geom box = shapely::Geom::createPolygon(pts);
//   shapely::Geom cir = shapely::Geom::createCircle(0, 0, 10);
//   double a = cir.intersection(box).area();
//   for (const shapely::Geom& part : box.unionWith(cir).splitParts())  // 多体拆分
//       double partArea = part.area();
//   shapely::Geom ring = shapely::Geom::createPolygonWithHoles(shell, holes); // 带孔面
//
//   // 预处理几何：ROI 对大量缺陷区域反复判定，先 prepare 走缓存索引
//   shapely::PreparedGeom roi = shapely::PreparedGeom::prepare(roiPoly);
//   if (roi.intersects(defect)) { ... }
//
//   // 交换格式导出（空对象返回空串）
//   std::string gj = box.toGeoJSON();   // RFC 7946（上位机/前端可解析）
//   std::string dx = box.toDXF();       // DXF R12 ENTITIES（CAD 交换/交付图）
//
//   // OpenCV 矩形/旋转矩形（适配层，随总入口一并引入）
//   shapely::Geom r = shapely::GeomCVAdapter::createRect(cv::Rect(0, 0, 30, 40));
//   shapely::Geom e = shapely::GeomCVAdapter::createEllipse(rotatedRect);
//
//   // 三维度量与空间索引
//   shapely::Stereo st = shapely::Stereo::createFromPoints(pts3d);
//   shapely::AABB3d bb = st.getAABB();
//   shapely::OBB3d obb = st.getOBB();               // PCA 主轴盒（extents=半长）
//   shapely::Geom proj = st.projectTo2D();          // 2.5D 桥接
//   shapely::KdTree3 tree;                          // 反复查询建树一次
//   tree.build(pts3d);
//   int idx; double dist;
//   tree.nearest(query, idx, dist);                 // 最近邻 / knn / radiusSearch
//
//   // 图像（枚举直接传 cv:: 常量）
//   shapely::ImageProcessor::threshold(src, dst, 100, 255, cv::THRESH_BINARY);
//   shapely::ImageProcessor::splitIntoTiles(src, {512, 512}, 0, tiles);
//
//   // 点云（模板白名单：pcl::PointXYZ / PointXYZI / PointXYZRGB）
//   auto cloud = shapely::CloudProcessor<pcl::PointXYZ>::filterVoxelGrid(src, 0.01);
//   auto loaded = shapely::CloudIO<pcl::PointXYZ>::ioLoadPCD("a.pcd");
//
//   // 点云表面重建（需先估计并统一法线朝向）+ 网格落盘
//   auto normals = shapely::CloudProcessor<pcl::PointXYZ>::featureNormalEstimation(cloud, 20);
//   normals = shapely::CloudProcessor<pcl::PointXYZ>::featureOrientNormalsConsistentTangentPlane(
//       cloud, normals, 30);
//   shapely::Mesh3d mesh =
//       shapely::CloudProcessor<pcl::PointXYZ>::surfacePoisson(cloud, normals, 9, 8, 1.0);
//   shapely::MeshIO::saveOBJ("part.obj", mesh);     // 或 saveSTL（CAD/打印交换）
//
//   // 3D↔2D 桥接：高度图（接 ImageProcessor 全部 2D 算子）
//   shapely::HeightMap hm =
//       shapely::CloudProcessor<pcl::PointXYZ>::cloudToHeightMap(cloud, 0.05, 0);
//   shapely::HeightMapIO::save("part.hmap", hm);    // 序列化/反序列化
//
//   // 测量：3D 空间圆（孔位/焊球）与平面度
//   float nrm[3]; double radius = 0, rms = 0;
//   shapely::CloudProcessor<pcl::PointXYZ>::fitCircle3D(cloud, shapely::FitMethod::RANSAC,
//       0.05, center3d, nrm, radius, inliers, rms);
//   shapely::CloudProcessor<pcl::PointXYZ>::measureFlatness(cloud, 0.05, flatness, coeffs);

#include "Types.h"
#include "Geom.h"
#include "Stereo.h"
#include "ImageProcessor.h"
#include "CloudProcessor.h"
#include "CloudIO.h"

// ---------------- OpenCV 适配层（cv::Rect/RotatedRect → Geom） ----------------

#include <opencv2/core/types.hpp>

namespace shapely {

// cv::Rect / cv::RotatedRect（OpenCV 整数/浮点矩形）→ Geom 面对象的便捷转换。
// 全静态工具类，用于把 OpenCV 检测结果（ROI 框/旋转焊盘）接入几何布尔运算。
class GeomCVAdapter
{
public:
    // cv::Rect 系列 → 矩形面（四角点自动闭合）
    template <typename R>
    static Geom createRect(const R& r)
    {
        const double x = (double)r.x, y = (double)r.y;
        const double w = (double)r.width, h = (double)r.height;
        const std::vector<Point2d> corners = {
            { x,     y },
            { x + w, y },
            { x + w, y + h },
            { x,     y + h },
        };
        return Geom::createPolygon(corners);
    }

    // cv::Rect2d 显式重载（坐标本身已是 double，与模板版行为一致）
    static Geom createRect(const cv::Rect2d& r)
    {
        return createRect<cv::Rect2d>(r);
    }

    // cv::RotatedRect → 椭圆面（center/size/angle 离散化）
    static Geom createEllipse(const cv::RotatedRect& rr,
                               double max_error = 1e-4, int segments = 0)
    {
        return Geom::createEllipse((double)rr.center.x, (double)rr.center.y,
                                   (double)rr.size.width * 0.5,
                                   (double)rr.size.height * 0.5,
                                   (double)rr.angle, max_error, segments);
    }
};

} // namespace shapely

// ---------------- PCL 适配层（pcl::PointCloud → Stereo） ----------------
// （PCL 头较重，按需单独 include "ShapelyAlg.h" 亦可用；
//   本总入口默认引入，与 CloudProcessor 一致。）

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>

namespace shapely {

// pcl::PointCloud（任意点型，含 XYZ/XYZRGBA/XYZI 等）→ Stereo 点集的便捷转换。
// 仅提取 x/y/z 三坐标；点型字段差异不影响转换。
class StereoPCLAdapter
{
public:
    // PCL 点云 → Stereo（遍历提取三维坐标）
    template <typename PointT>
    static Stereo createFromCloud(const pcl::PointCloud<PointT>& cloud)
    {
        std::vector<Point3d> pts;
        pts.reserve(cloud.size());
        for (const PointT& p : cloud.points)
            pts.push_back(Point3d{ (double)p.x, (double)p.y, (double)p.z });
        return Stereo::makeFromPoints(std::move(pts));
    }

    // PCL 点云指针重载
    template <typename PointT>
    static Stereo createFromCloud(const typename pcl::PointCloud<PointT>::Ptr& cloud)
    {
        return cloud ? createFromCloud(*cloud) : Stereo();
    }
};

} // namespace shapely

// ---------------- HeightMap 适配层（HeightMap → cv::Mat，接 ImageProcessor 全链） ----------------

#include <cmath>
#include <algorithm>

namespace shapely {

// HeightMap（不规则格网高度图）→ cv::Mat 灰度图，衔接 ImageProcessor 全链
//（先 toImage 再走 2D 算子）。
class HeightMapAdapter
{
public:
    /// 高度图 → 灰度图（行序与 HeightMap 一致：row 0 = y 最小，即图像 top）。
    /// 空格（mask=0）与 NaN 一律输出 0。
    /// [in] preserve16  false（默认）= CV_8U，有效格 min-max 归一化到 [0,255]
    ///                  （zmax==zmin 时全 0）；true = CV_16U，z 原值截断到 [0,65535]
    /// 返回：invalid/空图返回空 Mat。
    static cv::Mat toImage(const HeightMap& hm, bool preserve16 = false)
    {
        cv::Mat out;
        if (!hm.valid || hm.width <= 0 || hm.height <= 0 ||
            hm.cells.size() != (size_t)hm.width * (size_t)hm.height ||
            hm.mask.size() != hm.cells.size())
            return out;
        if (preserve16) {
            out = cv::Mat(hm.height, hm.width, CV_16U, cv::Scalar(0));
            for (int r = 0; r < hm.height; ++r)
                for (int c = 0; c < hm.width; ++c) {
                    const size_t i = (size_t)r * (size_t)hm.width + (size_t)c;
                    if (!hm.mask[i] || std::isnan((double)hm.cells[i]))
                        continue;
                    double z = (double)hm.cells[i];
                    z = z < 0 ? 0 : (z > 65535.0 ? 65535.0 : z);
                    out.at<uint16_t>(r, c) = (uint16_t)(z + 0.5);
                }
            return out;
        }
        double zmin = 1e300, zmax = -1e300;
        for (size_t i = 0; i < hm.cells.size(); ++i) {
            if (!hm.mask[i] || std::isnan((double)hm.cells[i]))
                continue;
            zmin = (std::min)(zmin, (double)hm.cells[i]);
            zmax = (std::max)(zmax, (double)hm.cells[i]);
        }
        out = cv::Mat(hm.height, hm.width, CV_8U, cv::Scalar(0));
        const double range = zmax - zmin;
        for (int r = 0; r < hm.height; ++r)
            for (int c = 0; c < hm.width; ++c) {
                const size_t i = (size_t)r * (size_t)hm.width + (size_t)c;
                if (!hm.mask[i] || std::isnan((double)hm.cells[i]) || range <= 0)
                    continue;
                out.at<uint8_t>(r, c) =
                    (uint8_t)(((double)hm.cells[i] - zmin) / range * 255.0 + 0.5);
            }
        return out;
    }
};

} // namespace shapely

#endif // SHAPELYALG_H
