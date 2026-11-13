#ifndef NOMINMAX
#define NOMINMAX
#endif
// ShapelyAlg_bench.cpp —— 热点算子基准（方向 D 第 1 步：OpenMP 决策依据）。
// 确定性数据（LCG），单次预热 + 多轮计时取最优；输出 ms 与吞吐。
// 注意：不注册 ctest（计时类测试不宜 CI）；数据规模见各 case 注释。
#include "ShapelyAlg.h"

#include <opencv2/imgproc.hpp>   // getRotationMatrix2D/warpAffine（bench 自用，非公共头）

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

unsigned int g_lcg = 12345u;
double lcg01()
{
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return (double)(g_lcg >> 8) / 16777216.0;   // [0,1)
}

double benchOnce(const char* name, const std::function<void()>& fn)
{
    fn();   // 预热（首次分配/缓存）
    const auto t0 = Clock::now();
    fn();
    const double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::printf("  %-42s %10.3f ms\n", name, ms);
    return ms;
}

using CloudPtr = shapely::CloudProcessor<pcl::PointXYZ>::CloudPtr;

CloudPtr makeCloud(size_t n, double span)
{
    CloudPtr c(new pcl::PointCloud<pcl::PointXYZ>());
    c->reserve(n);
    for (size_t i = 0; i < n; ++i) {
        pcl::PointXYZ p;
        p.x = (float)(lcg01() * span);
        p.y = (float)(lcg01() * span);
        p.z = (float)(lcg01() * span * 0.1);
        c->push_back(p);
    }
    return c;
}

} // namespace

int main()
{
    using CP = shapely::CloudProcessor<pcl::PointXYZ>;
    using shapely::CloudProcessor;

    std::printf("== ShapelyAlg bench (Release, 确定性 LCG 数据) ==\n");

    // ---- 点云 500k（span 50mm，z ∈ [0,5]） ----
    const CloudPtr cloud = makeCloud(500000, 50.0);

    benchOnce("filterVoxelGrid(0.1) 500k pts", [&] {
        auto out = CP::filterVoxelGrid(cloud, 0.1);
        volatile size_t n = out ? out->size() : 0; (void)n;
    });

    benchOnce("cloudToHeightMap(0.5, max) 500k", [&] {
        auto hm = CP::cloudToHeightMap(cloud, 0.5, 0);
        volatile size_t n = hm.cells.size(); (void)n;
    });

    benchOnce("cloudToHeightMap(0.1, density) 500k", [&] {
        auto hm = CP::cloudToHeightMap(cloud, 0.1, 3);
        volatile size_t n = hm.cells.size(); (void)n;
    });

    // ---- KdTree3：建树 + 1 万次最近邻 ----
    {
        std::vector<shapely::Point3d> pts;
        pts.reserve(200000);
        for (size_t i = 0; i < 200000; ++i)
            pts.push_back(shapely::Point3d{ lcg01() * 50, lcg01() * 50, lcg01() * 5 });
        shapely::KdTree3 tree;
        const double buildMs = benchOnce("KdTree3 build 200k", [&] {
            tree.build(pts);
        });
        (void)buildMs;
        std::vector<shapely::Point3d> queries;
        for (int i = 0; i < 10000; ++i)
            queries.push_back(shapely::Point3d{ lcg01() * 50, lcg01() * 50, lcg01() * 5 });
        double ms = 0;
        int hits = 0;
        const auto t0 = Clock::now();
        for (const auto& q : queries) {
            int idx = -1; double dist = 0;
            tree.nearest(q, idx, dist);
            if (idx >= 0) ++hits;
        }
        ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::printf("  %-42s %10.3f ms   (10k nearest, %.1f us/query)\n",
                    "KdTree3 nearest x10k", ms, ms * 1000.0 / 10000.0);
        (void)hits;
    }

    // ---- 平面拟合（100k 平面 + 噪声） ----
    {
        CloudPtr plane(new pcl::PointCloud<pcl::PointXYZ>());
        plane->reserve(100000);
        for (size_t i = 0; i < 100000; ++i) {
            pcl::PointXYZ p;
            p.x = (float)(lcg01() * 50);
            p.y = (float)(lcg01() * 50);
            p.z = 2.0f + (float)(lcg01() - 0.5) * 0.04f;
            plane->push_back(p);
        }
        benchOnce("fitPlane RANSAC 100k", [&] {
            std::vector<float> coeffs; std::vector<int> in;
            CP::fitPlane(plane, shapely::FitMethod::RANSAC, 0.05, coeffs, in);
        });
    }

    // ---- 点-网格距离：2 万点 × 球面网格（Poisson 输出规模） ----
    {
        // 合成球面点 → 法线 → Poisson 网格（典型重建规模）
        CloudPtr sphere(new pcl::PointCloud<pcl::PointXYZ>());
        sphere->reserve(20000);
        for (size_t i = 0; i < 20000; ++i) {
            const double u = lcg01() * 2.0 - 1.0, v = lcg01() * 2.0 * 3.14159265358979323846;
            const double r = std::sqrt((std::max)(0.0, 1.0 - u * u));
            pcl::PointXYZ p;
            p.x = (float)(10.0 * r * std::cos(v));
            p.y = (float)(10.0 * r * std::sin(v));
            p.z = (float)(10.0 * u);
            sphere->push_back(p);
        }
        auto normals = CP::featureNormalEstimation(sphere, 20);
        normals = CP::featureOrientNormalsConsistentTangentPlane(sphere, normals, 30);
        shapely::Mesh3d mesh = CP::surfacePoisson(sphere, normals, 7, 8, 1.0);
        std::printf("  [sphere mesh: %d verts / %d tris]\n", (int)mesh.vertices.size(),
                    (int)mesh.triangles.size());
        if (mesh.valid) {
            CloudPtr q = makeCloud(20000, 25.0);
            benchOnce("pointToMeshDistance 20k x mesh", [&] {
                auto d = CP::measurePointToMeshDistance(q, mesh);
                volatile size_t n = d.size(); (void)n;
            });
            benchOnce("pointToMeshSignedDistance 20k", [&] {
                auto d = CP::measurePointToMeshSignedDistance(q, mesh);
                volatile size_t n = d.size(); (void)n;
            });
        }
    }

    // ---- 2D：旋转模板对位（256 图 24 角度） ----
    {
        cv::Mat img = cv::Mat::zeros(256, 256, CV_8UC1);
        cv::Mat tpl(24, 24, CV_8UC1, cv::Scalar(0));
        cv::circle(tpl, { 12, 12 }, 9, cv::Scalar(255), -1);
        tpl.at<uint8_t>(4, 3) = 255;
        cv::Mat M = cv::getRotationMatrix2D(cv::Point2f(12.f, 12.f), 20.0, 1.0);
        M.at<double>(0, 2) += 128.0 - 12.0;
        M.at<double>(1, 2) += 128.0 - 12.0;
        cv::warpAffine(tpl, img, M, img.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT,
                       cv::Scalar::all(0));
        benchOnce("findRotatedMatch 256px x 24 angles", [&] {
            auto rm = shapely::ImageProcessor::findRotatedMatch(img, tpl, -30.0, 30.0, 2.5);
            volatile double s = rm.score; (void)s;
        });
    }

    std::printf("== done ==\n");
    return 0;
}
