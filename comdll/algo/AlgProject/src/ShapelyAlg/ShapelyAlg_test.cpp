// ShapelyAlg_test.cpp —— shapely 库四模块验证（控制台，参数写死）。
// 覆盖：Geom（工厂/布尔/度量/离散化）、Stereo（AABB/OBB/距离/桥接）、
//       ImageProcessor（阈值/轮廓/瓦片/匹配）、CloudProcessor（11 模块 68 算子全
//       方法名覆盖，见 CloudProcessor.h：filter/extract/trans/feature/keypoint/
//       fit/segment/regist/tree/surface/measure，覆盖审计见 dispatch/BOARD.md T24）。

#include "ShapelyAlg.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using shapely::Geom;
using shapely::Stereo;
using shapely::Point2d;
using shapely::Point3d;
using shapely::KdTree3;
using shapely::AABB3d;
using shapely::OBB3d;

// 自定义点结构（验证鸭子类型）
struct MyPt { float x; float y; };
struct MyPt3 { double x; double y; double z; };

static int g_fail = 0;

static void check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_fail;
}

static bool near(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) < eps;
}

// ---------------- Geom ----------------

static void testGeom()
{
    std::printf("== Geom 工厂 ==\n");
    {
        const Geom p = Geom::createPoint(3.0, 4.0);
        check(p.getX() == 3.0 && p.getY() == 4.0, "createPoint(x,y)");
    }
    check(Geom::createPoint(cv::Point(10, 20)).getX() == 10.0, "createPoint(cv::Point)");
    check(Geom::createPoint(cv::Point3f(1.5f, 2.5f, 99.0f)).getY() == 2.5, "createPoint(Point3f 丢Z)");
    check(Geom::createPoint(std::vector<double>{ 7, 8, 999 }).getX() == 7.0, "createPoint(扁平数组)");
    check(Geom::createPoint(MyPt{ 6.0f, 9.0f }).getY() == 9.0, "createPoint(自定义结构)");

    check(near(Geom::createLine(std::vector<Point2d>{
        { 0, 0 }, { 3, 0 }, { 3, 4 } }).length(), 7.0), "createLine 长度 3+4=7");
    check(near(Geom::createLineFromFlat(std::vector<float>{ 0, 0, 0, 5 }).length(), 5.0),
          "createLineFromFlat 长度 5");
    check(near(Geom::createLineFromNested(std::vector<std::vector<double>>{
        { 0, 0 }, { 0, 6 } }).length(), 6.0), "createLineFromNested 长度 6");

    check(near(Geom::createPolygon(std::vector<Point2d>{
        { 0, 0 }, { 4, 0 }, { 4, 4 }, { 0, 4 } }).area(), 16.0), "createPolygon 自动闭合 面积 16");
    check(near(Geom::createPolygonFromFlat(std::vector<double>{ 0, 0, 2, 0, 2, 2, 0, 2 }).area(), 4.0),
          "createPolygonFromFlat 面积 4");
    check(near(Geom::createPolygonFromNested(std::vector<std::vector<int>>{
        { 0, 0 }, { 3, 0 }, { 3, 3 }, { 0, 3 } }).area(), 9.0), "createPolygonFromNested 面积 9");

    check(near(shapely::GeomCVAdapter::createRect(cv::Rect(10, 20, 30, 40)).area(), 1200.0),
          "createRect(cv::Rect) 面积 30x40");
    check(near(Geom::createCircle(0, 0, 10.0).area(), M_PI * 100.0, 1e-2), "createCircle 面积 πr²");
    check(near(Geom::createCircle(0, 0, 10.0, 1e-4, 36).area(), M_PI * 100.0, 1.7),
          "createCircle 36段（理论差 1.59）");
    check(near(Geom::createEllipse(0, 0, 5.0, 2.0, 30.0).area(), M_PI * 10.0, 1e-3),
          "createEllipse(5参数) 面积 πab");
    check(near(shapely::GeomCVAdapter::createEllipse(
        cv::RotatedRect(cv::Point2f(0, 0), cv::Size2f(10, 4), 30.0f)).area(), M_PI * 10.0, 1e-3),
        "createEllipse(RotatedRect) 面积");
    check(near(Geom::fromWKT("POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))").area(), 1.0), "fromWKT 面积 1");

    std::printf("== Geom 运算 ==\n");
    const Geom a = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 4, 4));
    const Geom b = shapely::GeomCVAdapter::createRect(cv::Rect2d(2, 2, 4, 4));
    check(near(a.intersection(b).area(), 4.0), "交集 2x2=4");
    check(near(a.unionWith(b).area(), 28.0), "并集 16+16-4=28");
    check(near(a.difference(b).area(), 12.0), "差集 16-4=12");

    const Geom c1 = Geom::createCircle(0, 0, 1.0);
    const Geom c2 = Geom::createCircle(5, 0, 1.0);
    check(near(c1.distanceTo(c2), 3.0, 1e-2), "距离 5-2=3");
    check(!c1.intersects(c2) && c1.intersects(Geom::createCircle(1.5, 0, 1.0)), "相交判定");

    check(a.contains(Geom::createPoint(1, 1)) && !a.contains(Geom::createPoint(5, 5)), "面包含点");
    check(Geom::createPoint(1, 1).within(a), "点 within 面");
    check(Geom::createLine(std::vector<Point2d>{ { 0, 2 }, { 10, 2 } })
              .crosses(Geom::createLine(std::vector<Point2d>{ { 5, 0 }, { 5, 10 } })), "线交叉");

    check(near(Geom::createPoint(0, 0).buffer(2.0, 16).area(), M_PI * 4.0, 0.05), "点 buffer(2)");
    check(near(Geom::createLine(std::vector<Point2d>{
        { 0, 0 }, { 4, 0 }, { 2, 2 }, { 4, 4 }, { 0, 4 } }).convexHull().area(), 16.0), "折线凸包 16");

    const shapely::BoundingBox2d bb = Geom::createCircle(0, 0, 10.0).bounds();
    check(near(bb.width(), 20.0, 1e-3) && near(bb.height(), 20.0, 1e-3), "圆包围盒 20x20");
    check(Geom::createCircle(0, 0, 10.0).getExteriorPoints().size() >= 8, "圆顶点数 ≥ 8");
    // 回归（2026-11-09）：maxError > 2·半径 时分段数曾反向返回上限 4096
    check(Geom::createCircle(0, 0, 10.0, 100.0).getExteriorPoints().size() == 9,
          "圆 maxError 超直径 → 最少 8 段（8+闭合）");

    const Point2d ct = Geom::createPolygon(
        std::vector<Point2d>{ { 0, 0 }, { 4, 0 }, { 4, 4 }, { 0, 4 } }).centroid();
    check(near(ct.x, 2.0) && near(ct.y, 2.0), "质心 (2,2)");
    // 回归（2026-11-09）：质心曾无类型门——契约是"非面返回 (0,0)"
    const Point2d lineCtr = Geom::createLine(std::vector<Point2d>{ { 0, 0 }, { 4, 0 } }).centroid();
    check(near(lineCtr.x, 0.0) && near(lineCtr.y, 0.0), "质心非面 → (0,0)");
    // GEOS Length/Area 语义（头文件契约已按实际行为修正）
    check(near(Geom::createLine(std::vector<Point2d>{ { 0, 0 }, { 4, 0 } }).length(), 4.0),
          "线长 4");
    check(near(Geom::createPolygon(
        std::vector<Point2d>{ { 0, 0 }, { 4, 0 }, { 4, 4 }, { 0, 4 } }).length(), 16.0),
          "面 length = 周长 16");

    // 抽稀：锯齿折线（20 点）容差 0.5 后点数大幅减少但端点保留
    {
        std::vector<Point2d> jagged;
        for (int i = 0; i < 20; ++i)
            jagged.push_back(Point2d{ (double)i * 0.5, (i % 2) ? 0.3 : 0.0 });
        const Geom dense = Geom::createLine(jagged);
        const Geom sparse = dense.simplify(0.5);
        check(sparse.getExteriorPoints().size() < dense.getExteriorPoints().size() &&
              sparse.getExteriorPoints().size() >= 2, "simplify 抽稀减点");
        // 容差 <=0 原样返回
        check(dense.simplify(0.0).getExteriorPoints().size() == dense.getExteriorPoints().size(),
              "simplify(0) 原样克隆");
    }

    // 有效性：合法面 true，自交蝴蝶结 false
    check(Geom::createPolygon(std::vector<Point2d>{
        { 0, 0 }, { 4, 0 }, { 4, 4 }, { 0, 4 } }).isValid(), "合法面 isValid");
    check(!Geom::createPolygon(std::vector<Point2d>{
        { 0, 0 }, { 4, 4 }, { 4, 0 }, { 0, 4 } }).isValid(), "自交面 isValid=false");

    // makeValid / isValidReason / 2D 仿射（2026-11-10 迭代 5）
    {
        // 自交蝴蝶结：isValid false → makeValid → 有效且面积 = 两三角之和 8
        Geom bowtie = Geom::createPolygon(std::vector<Point2d>{
            { 0, 0 }, { 4, 4 }, { 4, 0 }, { 0, 4 } });
        check(!bowtie.isValid(), "蝴蝶结 isValid=false");
        const std::string reason = bowtie.isValidReason();
        check(reason.find("Self-intersection") != std::string::npos,
              "isValidReason 指出自交");
        const Geom fixed = bowtie.makeValid();
        check(fixed.isValid() && near(fixed.area(), 8.0, 1e-6), "makeValid 修复面积 8");
        check(Geom::createPolygon(
            std::vector<Point2d>{ { 0, 0 }, { 4, 0 }, { 4, 4 }, { 0, 4 } })
                  .isValidReason() == "Valid Geometry", "isValidReason 合法面");
        check(Geom::fromWKT("BOGUS").isValidReason() == "null geometry",
              "isValidReason 空对象");
        // 2D 仿射：旋转 90° (x,y)→(−y,x)
        const Geom rect = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 2, 2));
        const Geom rot = rect.transformed(0, -1, 0, 1, 0, 0);
        check(near(rot.area(), 4.0, 1e-9) && near(rot.bounds().min_x, -2.0, 1e-9) &&
              near(rot.bounds().max_x, 0.0, 1e-9) && near(rot.bounds().max_y, 2.0, 1e-9),
              "transformed 旋转 90° 面积/包围盒");
        // 平移 + 缩放
        const Geom st = rect.transformed(2, 0, 10, 0, 3, 20);
        check(near(st.area(), 24.0, 1e-9) && near(st.bounds().min_x, 10.0, 1e-9),
              "transformed 缩放 (2,3)+平移 (10,20) → 4x6=24");
        // 带孔面 + Multi* 拓扑保留
        const Geom ring = Geom::createPolygonWithHoles(
            std::vector<Point2d>{ { 0, 0 }, { 4, 0 }, { 4, 4 }, { 0, 4 } },
            { std::vector<Point2d>{ { 1, 1 }, { 3, 1 }, { 3, 3 }, { 1, 3 } } });
        const Geom ringT = ring.transformed(1, 0, 100, 0, 1, 200);
        check(ringT.isValid() && ringT.numInteriorRings() == 1 &&
              near(ringT.area(), 12.0, 1e-9), "transformed 带孔面平移保拓扑");
        const Geom p1 = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 1, 1));
        const Geom p2 = shapely::GeomCVAdapter::createRect(cv::Rect2d(5, 5, 1, 1));
        const Geom multi = p1.unionWith(p2);
        const Geom multiT = multi.transformed(1, 0, 0, 0, 1, 0);
        check(multiT.typeName() == "MultiPolygon" && near(multiT.area(), 2.0, 1e-9),
              "transformed MultiPolygon 类型保持");
        check(Geom::fromWKT("BOGUS").transformed(1, 0, 0, 0, 1, 0).isEmpty(),
              "transformed 空对象 → 空");

        // lastError（2026-11-12 迭代 13）：入口清空 + GEOS 诊断捕获（D1 落地）。
        // 契约 = "最近一次 GEOS 诊断"：非空 ⇒ 有诊断；布尔/形变运算入口清空。
        //（不测"初始为空"——前文 fromWKT("BOGUS") 已写入诊断，属契约内行为）
        Geom::fromWKT("BOGUS");
        check(!Geom::lastError().empty(), "lastError 捕获 WKT 解析诊断");
        {
            const Geom r1 = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 1, 1));
            const Geom r2 = shapely::GeomCVAdapter::createRect(cv::Rect2d(2, 0, 1, 1));
            const Geom u = r1.unionWith(r2);
            check(u.isValid() && Geom::lastError().empty(),
                  "布尔入口清空：成功运算后 lastError 为空");
            const Geom farR = shapely::GeomCVAdapter::createRect(cv::Rect2d(50, 50, 1, 1));
            const Geom inter = r1.intersection(farR);   // 真不相交 → 空对象 + 无诊断
            check(inter.isEmpty() && Geom::lastError().empty(),
                  "真空结果不误报（不相交且无诊断）");
        }

        // GeoJSON / DXF 导出（2026-11-11 迭代 7）
        {
            const std::string pj = Geom::createPoint(3, 4).toGeoJSON();
            check(pj == "{\"type\":\"Point\",\"coordinates\":[3,4]}", "GeoJSON Point 逐字");
            const Geom holed = Geom::createPolygonWithHoles(
                std::vector<Point2d>{ { 0, 0 }, { 4, 0 }, { 4, 4 }, { 0, 4 } },
                { std::vector<Point2d>{ { 1, 1 }, { 3, 1 }, { 3, 3 }, { 1, 3 } } });
            const std::string hj = holed.toGeoJSON();
            check(hj.find("\"type\":\"Polygon\"") != std::string::npos &&
                  hj.find("[1,1]") != std::string::npos &&
                  hj.find("[3,3]") != std::string::npos, "GeoJSON Polygon 含孔洞环");
            const Geom m1 = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 1, 1));
            const Geom m2 = shapely::GeomCVAdapter::createRect(cv::Rect2d(5, 5, 1, 1));
            const std::string mj = m1.unionWith(m2).toGeoJSON();
            check(mj.find("\"type\":\"MultiPolygon\"") != std::string::npos,
                  "GeoJSON MultiPolygon 类型");
            check(Geom::fromWKT("BOGUS").toGeoJSON().empty(), "GeoJSON 空对象 → 空串");
            // WKT → GeoJSON 坐标一致（面积口径双通道）
            check(hj.find("0,0") != std::string::npos && hj.find("4,4") != std::string::npos,
                  "GeoJSON 坐标含角点");

            const std::string pd = Geom::createPoint(3, 4).toDXF();
            check(pd.find("POINT") != std::string::npos && pd.find("EOF") != std::string::npos,
                  "DXF POINT 实体 + EOF");
            const std::string rd = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 2, 2))
                                       .toDXF();
            int lineCount = 0;
            for (size_t pos = rd.find("0\nLINE\n"); pos != std::string::npos;
                 pos = rd.find("0\nLINE\n", pos + 1))
                ++lineCount;
            check(lineCount == 4, "DXF 矩形闭合 = 4 条 LINE");
            check(Geom::fromWKT("BOGUS").toDXF().empty(), "DXF 空对象 → 空串");
        }
    }

    Geom moved = Geom::createCircle(0, 0, 1.0);
    const Geom target = std::move(moved);
    check(moved.isEmpty() && near(target.area(), M_PI, 1e-3), "移动语义");

    check(Geom::fromWKT("POINT EMPTY").isEmpty() && Geom::fromWKT("BOGUS").isEmpty(), "空对象安全");

    // touches：共边矩形邻接、分离矩形不邻接
    {
        const Geom L = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 2, 2));
        const Geom R = shapely::GeomCVAdapter::createRect(cv::Rect2d(2, 0, 2, 2));
        const Geom farR = shapely::GeomCVAdapter::createRect(cv::Rect2d(5, 0, 2, 2));
        check(L.touches(R) && !L.intersects(farR) && !L.touches(farR), "touches 邻接判定");
    }

    // splitParts：不相交两面的 union → MultiPolygon → 拆开后逐区域面积
    {
        const Geom p1 = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 2, 2));
        const Geom p2 = shapely::GeomCVAdapter::createRect(cv::Rect2d(10, 10, 3, 3));
        const Geom merged = p1.unionWith(p2);
        std::vector<Geom> parts = merged.splitParts();
        check(parts.size() == 2, "union 不相交 → 2 个 part");
        // 回归（2026-11-09）：MultiPolygon 的 getExteriorPoints 曾返回空
        //（GEOSGeom_getCoordSeq 对 Polygon 子几何返回 NULL，缺递归展开）
        check(merged.getExteriorPoints().size() == 10,
              "MultiPolygon 顶点 = 2 环 × 5（含闭合点）");
        // 面积加权质心：(4·1 + 9·11.5) / 13
        check(near(merged.area(), 13.0) && near(merged.centroid().x, 107.5 / 13.0, 1e-6) &&
              near(merged.centroid().y, 107.5 / 13.0, 1e-6),
              "MultiPolygon 总面积/加权质心");
        if (parts.size() == 2) {
            const double a1 = parts[0].area(), a2 = parts[1].area();
            check(near(a1 + a2, 13.0) && a1 > 0 && a2 > 0, "逐 part 面积 4+9=13");
        }
        // 单体拆分返回自身克隆
        check(p1.splitParts().size() == 1 && near(p1.splitParts()[0].area(), 4.0),
              "单体 splitParts 返回自身");
    }

    // 带孔多边形：面积/包含自动扣孔（2026-11-10 迭代新增）
    {
        const Geom ring = Geom::createPolygonWithHoles(
            std::vector<Point2d>{ { 0, 0 }, { 4, 0 }, { 4, 4 }, { 0, 4 } },          // 外 4x4
            { std::vector<Point2d>{ { 1, 1 }, { 3, 1 }, { 3, 3 }, { 1, 3 } } });    // 孔 2x2
        check(ring.isValid() && near(ring.area(), 12.0), "带孔面面积 16-4=12");
        check(ring.numInteriorRings() == 1, "内环数 1");
        check(ring.getInteriorRings().size() == 1 &&
              ring.getInteriorRings()[0].size() == 5, "内环顶点 5（含闭合）");
        check(!ring.contains(Geom::createPoint(2, 2)) &&
              ring.contains(Geom::createPoint(0.5, 0.5)), "孔内不含/环带含");
        check(near(ring.bounds().width(), 4.0, 1e-9), "带孔面 bounds 取外壳");
        // 无孔/两孔/退化孔
        const Geom two = Geom::createPolygonWithHoles(
            std::vector<Point2d>{ { 0, 0 }, { 6, 0 }, { 6, 4 }, { 0, 4 } },
            { std::vector<Point2d>{ { 1, 1 }, { 2, 1 }, { 2, 2 }, { 1, 2 } },
              std::vector<Point2d>{ { 4, 1 }, { 5, 1 }, { 5, 2 }, { 4, 2 } } });
        check(two.isValid() && near(two.area(), 24.0 - 2.0) && two.numInteriorRings() == 2,
              "双孔面面积 24-2");
        const Geom plain = Geom::createPolygonWithHoles(
            std::vector<Point2d>{ { 0, 0 }, { 2, 0 }, { 2, 2 }, { 0, 2 } }, {});
        check(plain.isValid() && near(plain.area(), 4.0) && plain.numInteriorRings() == 0,
              "无孔面 = 普通多边形");
        check(Geom::createPolygonWithHoles(
                  std::vector<Point2d>{ { 0, 0 } }, {}).isEmpty(),
              "shell 退化 → 空对象");
        check(Geom::createPolygonWithHoles(
                  std::vector<Point2d>{ { 0, 0 }, { 4, 0 }, { 4, 4 }, { 0, 4 } },
                  { std::vector<Point2d>{ { 1, 1 } } }).numInteriorRings() == 0,
              "孔不足 3 点被忽略");
        // 普通多边形内环读取
        check(Geom::createPolygon(
            std::vector<Point2d>{ { 0, 0 }, { 2, 0 }, { 2, 2 }, { 0, 2 } })
                  .getInteriorRings().empty(), "普通面无内环");
    }

    // 批量并集 unaryUnion（2026-11-10 迭代新增；只读指针入参，不消费原始对象）
    {
        const Geom a = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 2, 2));
        const Geom b = shapely::GeomCVAdapter::createRect(cv::Rect2d(1, 0, 2, 2));
        const Geom c = shapely::GeomCVAdapter::createRect(cv::Rect2d(2, 0, 2, 2));
        const Geom merged = Geom::unaryUnion({ &a, &b, &c });
        check(near(merged.area(), 8.0), "unaryUnion 三搭接面 → 8");
        // 与两两 union 等价
        Geom pairWise = a.unionWith(b);
        pairWise = pairWise.unionWith(c);
        check(near(pairWise.area(), merged.area()), "unaryUnion = 两两 union");
        // nullptr/空对象自动忽略；全空 → 空对象
        const Geom emptyG = Geom::fromWKT("BOGUS");
        check(near(Geom::unaryUnion({ &a, nullptr, &emptyG, &b }).area(), 6.0),
              "unaryUnion 忽略空对象");
        check(Geom::unaryUnion({ &emptyG, nullptr }).isEmpty() &&
              Geom::unaryUnion({}).isEmpty(), "unaryUnion 全空 → 空");
    }

    // 最近点对 + 距离谓词（2026-11-10 迭代新增）
    {
        const Geom rect = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 2, 2));
        const Geom pt = Geom::createPoint(5, 1);
        Point2d pa, pb;
        check(rect.nearestPoints(pt, pa, pb), "nearestPoints 成功");
        // GEOS 契约：输出点 0 = g1（本几何）上的最近点，点 1 = g2 上的
        check(near(pa.x, 2.0, 1e-9) && near(pa.y, 1.0, 1e-9) &&
              near(pb.x, 5.0, 1e-9), "最近点对：面边(2,1)↔点(5,1)");
        check(near(std::sqrt((pa.x - pb.x) * (pa.x - pb.x) + (pa.y - pb.y) * (pa.y - pb.y)),
                   rect.distanceTo(pt), 1e-9), "最近点对距离 = distanceTo");
        Point2d za, zb;
        check(!rect.nearestPoints(Geom::fromWKT("BOGUS"), za, zb) && near(za.x, 0.0),
              "nearestPoints 空对象 → false");
        // 点-点（确定性最近对）
        Point2d dda, ddb;
        check(Geom::createPoint(0, 0).nearestPoints(Geom::createPoint(3, 4), dda, ddb) &&
              near(ddb.x, 3.0) && near(ddb.y, 4.0), "点-点最近对");
        // isWithinDistance：距离 1.5 的点
        const Geom near3 = Geom::createPoint(3.5, 1);
        check(rect.isWithinDistance(near3, 1.6) && !rect.isWithinDistance(near3, 1.4),
              "isWithinDistance 公差判定");
        check(!rect.isWithinDistance(Geom::fromWKT("BOGUS"), 100.0),
              "isWithinDistance 空对象 → false");
        // 相交几何最近距离 0 且谓词真
        check(rect.isWithinDistance(
            shapely::GeomCVAdapter::createRect(cv::Rect2d(1, 1, 2, 2)), 0.0),
            "相交 → isWithinDistance(0)");
    }

    // PreparedGeom：预处理 ROI 后批量判定，结果与普通谓词一致
    {
        const Geom roi = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 10, 10));
        const shapely::PreparedGeom roiP = shapely::PreparedGeom::prepare(roi);
        check(!roiP.isEmpty(), "prepare 构建");

        const Geom inside = shapely::GeomCVAdapter::createRect(cv::Rect2d(1, 1, 2, 2));
        const Geom outside = shapely::GeomCVAdapter::createRect(cv::Rect2d(20, 20, 2, 2));
        check(roiP.intersects(inside) && !roiP.intersects(outside), "prepared intersects");
        check(roiP.contains(inside), "prepared contains 内部面");
        // contains 与 covers 的区别只在"完全躺在边界上"的几何（JTS 语义）：
        // 沿 roi 上边缘的线——无任何点在 roi 内部 → contains=false，covers=true
        const Geom topEdge = Geom::createLine(std::vector<Point2d>{ { 0, 10 }, { 10, 10 } });
        check(!roiP.contains(topEdge) && roiP.covers(topEdge),
              "contains 严格内部 / covers 含边界");
        check(roiP.covers(inside) && roiP.covers(roi), "prepared covers");
        check(near(roiP.distanceTo(outside), std::sqrt(200.0), 1e-6), "prepared distance √200");
        // 与普通谓词一致性
        check(roiP.intersects(inside) == roi.intersects(inside) &&
              roiP.contains(inside) == roi.contains(inside), "prepared 与普通谓词一致");

        // 空源 prepare：判定恒 false
        const shapely::PreparedGeom empty = shapely::PreparedGeom::prepare(Geom::fromWKT("POINT EMPTY"));
        check(empty.isEmpty() && !empty.intersects(inside), "prepare 空对象安全");

        // prepared distance 语义与普通 distanceTo 一致（相切接触 → 0）
        const Geom tangent = shapely::GeomCVAdapter::createRect(cv::Rect2d(10, 4, 2, 2));
        check(near(roiP.distanceTo(tangent), 0.0) &&
              near(roi.distanceTo(tangent), 0.0), "prepared distance 相切=0 与普通一致");
    }

    // WKT 往返：POINT/LINESTRING/POLYGON/MULTIPOLYGON 出入一致、类型名正确
    {
        const Geom pt = Geom::createPoint(3.25, -4.5);
        check(pt.wkt() == "POINT (3.25 -4.5)" && pt.typeName() == "Point", "wkt/typeName POINT");
        const Geom line = Geom::createLine(std::vector<Point2d>{ { 0, 0 }, { 1, 1 } });
        check(line.wkt() == "LINESTRING (0 0, 1 1)" && line.typeName() == "LineString",
              "wkt/typeName LINESTRING");
        const Geom poly = Geom::fromWKT("POLYGON((0 0, 2 0, 2 2, 0 2, 0 0))");
        check(Geom::fromWKT(poly.wkt()).area() == 4.0, "WKT 往返 POLYGON 面积不变");
        check(poly.typeName() == "Polygon", "typeName POLYGON");
        const Geom multi = Geom::fromWKT(
            "MULTIPOLYGON(((0 0, 1 0, 1 1, 0 1, 0 0)),((5 5, 6 5, 6 6, 5 6, 5 5)))");
        check(multi.typeName() == "MultiPolygon" && multi.splitParts().size() == 2,
              "typeName MULTIPOLYGON + splitParts 2");
        // 空对象（工厂兜底路径：扁平数组不足 2 元素）：wkt 空串、typeName "null"
        const Geom emptyGeom = Geom::createPoint(std::vector<double>{ 1.0 });
        check(emptyGeom.isEmpty() && emptyGeom.wkt().empty() &&
              emptyGeom.typeName() == "null", "空对象 wkt 空串 typeName null");
    }

    // toRect2d：与 bounds 等价（语义别名，取 ROI 裁图场景）
    {
        const Geom circ = Geom::createCircle(0, 0, 5.0);
        const shapely::BoundingBox2d r1 = circ.toRect2d();
        const shapely::BoundingBox2d r2 = circ.bounds();
        check(r1.valid && r2.valid &&
              r1.min_x == r2.min_x && r1.min_y == r2.min_y &&
              r1.max_x == r2.max_x && r1.max_y == r2.max_y, "toRect2d 与 bounds 等价");
    }

    // buffer 负值收缩：4x4 面收缩 1 → 2x2 面积 4；顶点密度 quadSegs 越大顶点越多
    {
        const Geom sq = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 4, 4));
        check(near(sq.buffer(-1.0, 8).area(), 4.0, 1e-9), "buffer(-1) 收缩 2x2=4");
        check(sq.buffer(-5.0, 8).isEmpty(), "buffer(-5) 超界 → 空对象");
        const Geom bigArc = Geom::createPoint(0, 0).buffer(2.0, 16);
        const Geom smallArc = Geom::createPoint(0, 0).buffer(2.0, 8);
        check(bigArc.getExteriorPoints().size() > smallArc.getExteriorPoints().size(),
              "buffer quadSegs 越大圆顶点越多");
    }

    // contains/covers 边界语义（普通谓词侧，与 prepared 组互补）：
    // 内部点 contains=true；边界点 contains=false 但被 face 覆盖（buffer 0.1 微扩后 true）
    {
        const Geom face = shapely::GeomCVAdapter::createRect(cv::Rect2d(0, 0, 4, 4));
        const Geom edgePt = Geom::createPoint(0, 2);
        check(!face.contains(edgePt), "contains 边界点=false（严格内部）");
        check(face.buffer(0.1, 8).contains(edgePt), "buffer 微扩后含边界点");
        // within 同语义：边界点 within=false
        check(!edgePt.within(face), "within 边界点=false");
        // touches vs intersects：共边邻接 touches+intersects 均真；重叠面 touches=false
        const Geom nb = shapely::GeomCVAdapter::createRect(cv::Rect2d(4, 0, 4, 4));
        const Geom ov = shapely::GeomCVAdapter::createRect(cv::Rect2d(2, 0, 4, 4));
        check(face.touches(nb) && face.intersects(nb), "邻接：touches+intersects 同真");
        check(!face.touches(ov) && face.intersects(ov), "重叠：intersects 真 touches 假");
    }
}

// ---------------- Stereo ----------------

static void testStereo()
{
    std::printf("== Stereo ==\n");
    // 立方体角点（8 点）
    const std::vector<Point3d> cube = {
        { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 }, { 0, 4, 0 },
        { 0, 0, 4 }, { 4, 0, 4 }, { 4, 4, 4 }, { 0, 4, 4 },
    };
    const Stereo s = Stereo::createFromPoints(cube);
    check(s.size() == 8 && !s.isEmpty(), "createFromPoints 8 点");

    const Stereo sFlat = Stereo::createFromFlatArray(
        std::vector<double>{ 0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0,
                             0, 0, 4, 4, 0, 4, 4, 4, 4, 0, 4, 4 });
    check(sFlat.size() == 8, "createFromFlatArray 8 点");
    const Stereo sNest = Stereo::createFromNestedArray(
        std::vector<std::vector<double>>{ { 0, 0, 0 }, { 1, 1, 1 }, { 2, 2, 2 } });
    check(sNest.size() == 3, "createFromNestedArray 3 点");
    const Stereo sMy = Stereo::createFromPoints(
        std::vector<MyPt3>{ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } });
    check(sMy.size() == 3, "createFromPoints(自定义结构)");

    // AABB
    const shapely::AABB3d aabb = s.getAABB();
    check(aabb.valid && near(aabb.volume(), 64.0), "AABB 体积 64");
    check(near(s.getAABBVolume(), 64.0), "getAABBVolume");
    // OBB（立方体 PCA：体积 = 64，半长 2）
    const shapely::OBB3d obb = s.getOBB();
    check(obb.valid && near(obb.volume(), 64.0, 1e-6), "OBB 体积 64");
    check(near(obb.extents[0], 2.0, 1e-6) && near(obb.extents[1], 2.0, 1e-6) &&
          near(obb.extents[2], 2.0, 1e-6), "OBB 半长 2");
    check(near(s.getOBBVolume(), 64.0, 1e-6) && near(s.getOBBVolume(), obb.volume(), 1e-12),
          "getOBBVolume 64 且 = getOBB().volume()");
    // 回归（2026-11-09）：雅可比旋转角符号曾写反——旋转盒的主轴/体积全错
    //（轴对齐用例协方差已对角化，抓不到该 bug，必须用旋转盒）
    {
        std::vector<Point3d> rotBox;
        const double ang = 30.0 * 3.14159265358979323846 / 180.0;
        const double ca = std::cos(ang), sa = std::sin(ang);
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
                for (int k = 0; k < 2; ++k) {
                    const double x = (i ? 10.0 : -10.0), y = (j ? 5.0 : -5.0), z = (k ? 2.0 : -2.0);
                    rotBox.push_back(Point3d{ x * ca - y * sa, x * sa + y * ca, z });
                }
        const shapely::OBB3d rob = Stereo::createFromPoints(rotBox).getOBB();
        check(rob.valid && near(rob.volume(), 800.0, 1e-3), "旋转盒 OBB 体积 = 800");
        check(near(rob.extents[0], 10.0, 1e-3) && near(rob.extents[1], 5.0, 1e-3) &&
              near(rob.extents[2], 2.0, 1e-3), "旋转盒 OBB 半长 (10,5,2)");
        check(near(std::fabs(rob.rotation[0] * ca + rob.rotation[1] * sa), 1.0, 1e-6),
              "旋转盒主轴对齐 30° 方向");
    }
    // OBB 退化：点数 < 3 → invalid 默认盒（体积 0）
    const Stereo twoPts = Stereo::createFromPoints(
        std::vector<Point3d>{ { 0, 0, 0 }, { 1, 1, 1 } });
    check(!twoPts.getOBB().valid && near(twoPts.getOBBVolume(), 0.0),
          "OBB 点数<3 → invalid 盒");

    // AABB/OBB 关系
    const shapely::AABB3d inner{ 1, 1, 1, 3, 3, 3, true };
    check(aabb.contains(inner) && inner.intersects(aabb) && !inner.contains(aabb),
          "AABB contains/intersects");
    const shapely::AABB3d far{ 10, 10, 10, 12, 12, 12, true };
    check(!aabb.intersects(far), "AABB 不相交");
    check(obb.intersects(shapely::OBB3d{ Point3d{ 3.9, 0, 0 }, { 2, 2, 2 },
                                         { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, true }), "OBB 相交（平移小盒）");
    check(!obb.intersects(shapely::OBB3d{ Point3d{ 10, 0, 0 }, { 2, 2, 2 },
                                          { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, true }), "OBB 不相交（远盒）");

    // createFromBox
    const Stereo fromBox = Stereo::createFromBox(shapely::AABB3d{ 0, 0, 0, 2, 2, 2, true });
    check(fromBox.size() == 8 && near(fromBox.getAABB().volume(), 8.0), "createFromBox(AABB)");
    const Stereo fromBox2 = Stereo::createFromBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
    check(fromBox2.size() == 8 && near(fromBox2.getAABB().volume(), 1.0), "createFromBox(边界值)");
    const Stereo fromOBB = Stereo::createFromBox(obb);
    check(fromOBB.size() == 8 && near(fromOBB.getAABB().volume(), 64.0, 1e-6), "createFromBox(OBB)");

    // Z 过滤
    double zmin = 0, zmax = 0;
    s.getZBounds(zmin, zmax);
    check(zmin == 0.0 && zmax == 4.0, "getZBounds [0,4]");
    const Stereo low = s.filterByZRange(0.0, 0.0);
    check(low.size() == 4, "filterByZRange z=0 → 4 点");
    // 反向区间（min>max）无点落入 → 空点集；空对象 getZBounds 置 0
    const Stereo emptyRange = s.filterByZRange(5.0, 1.0);
    double ez0 = -1.0, ez1 = -1.0;
    emptyRange.getZBounds(ez0, ez1);
    check(emptyRange.isEmpty() && ez0 == 0.0 && ez1 == 0.0,
          "filter 反向区间→空 getZBounds 置 0");

    // 距离/最近点对/重合
    const Stereo near2 = Stereo::createFromPoints(std::vector<Point3d>{ { 5, 0, 0 }, { 6, 0, 0 } });
    check(near(s.minDistanceTo(near2), 1.0), "minDistanceTo = 1");
    Point3d pa, pb;
    s.findClosestPair(near2, pa, pb);
    check(pa.x == 4.0 && pb.x == 5.0, "findClosestPair (4,0,0)-(5,0,0)");
    check(s.hasCoincidentPointWith(Stereo::createFromPoints(
        std::vector<Point3d>{ { 99, 99, 99 }, { 4, 0, 0 } }), 1e-9), "hasCoincidentPointWith");
    check(!s.hasCoincidentPointWith(Stereo::createFromPoints(
        std::vector<Point3d>{ { 99, 99, 99 } }), 1e-9), "无重合点");

    // 平均最近邻距离（2026-11-11 迭代 7）
    {
        const Stereo square = Stereo::createFromPoints(std::vector<Point3d>{
            { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 } });
        check(near(square.meanNearestNeighborDistance(), 2.0), "meanNND 方格 = 2");
        const Stereo duo = Stereo::createFromPoints(
            std::vector<Point3d>{ { 0, 0, 0 }, { 5, 0, 0 } });
        check(near(duo.meanNearestNeighborDistance(), 5.0), "meanNND 两点 = 5");
        const Stereo lone = Stereo::createFromPoints(std::vector<Point3d>{ { 1, 2, 3 } });
        check(lone.meanNearestNeighborDistance() == -1.0, "meanNND 单点 → -1");
    }

    // 4x4 变换矩阵工具（2026-11-11 迭代 9）：求逆 + 串联
    {
        // T = rotZ90 + t(1,2,3)
        const float T[16] = { 0, -1, 0, 1,  1, 0, 0, 2,  0, 0, 1, 3,  0, 0, 0, 1 };
        float Tinv[16] = { 0 };
        check(Stereo::invertTransform(T, Tinv), "invertTransform 求逆成功");
        float ident[16] = { 0 };
        Stereo::composeTransform(T, Tinv, ident);
        bool isIdent = true;
        for (int r = 0; r < 4 && isIdent; ++r)
            for (int c = 0; c < 4; ++c)
                if (std::fabs(ident[r * 4 + c] - (r == c ? 1.0f : 0.0f)) > 1e-5)
                    isIdent = false;
        check(isIdent, "T·T⁻¹ ≈ I");
        // 逆变换把 T 作用过的点拉回原位：(4,0,0) → T → (1,6,3) → T⁻¹ → (4,0,0)
        const Stereo moved = Stereo::createFromPoints(std::vector<Point3d>{ { 4, 0, 0 } })
                                 .transform(T);
        const float TinvF[16] = { Tinv[0], Tinv[1], Tinv[2], Tinv[3], Tinv[4], Tinv[5],
                                  Tinv[6], Tinv[7], Tinv[8], Tinv[9], Tinv[10], Tinv[11],
                                  Tinv[12], Tinv[13], Tinv[14], Tinv[15] };
        const Stereo back2 = moved.transform(TinvF);
        const Point3d backPt = back2.getPoints().front();
        check(near(backPt.x, 4.0, 1e-4) && near(backPt.y, 0.0, 1e-4) &&
              near(backPt.z, 0.0, 1e-4), "逆变换还原点位");
        // 串联次序：A=平移(1,0,0)，B=rotZ90 → compose(B,A) 作用 (1,0,0) = (0,2,0)
        const float A[16] = { 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const float B[16] = { 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        float BA[16] = { 0 };
        Stereo::composeTransform(B, A, BA);
        const float p[4] = { 1, 0, 0, 1 };
        float q[4] = { 0 };
        for (int r = 0; r < 4; ++r)
            q[r] = BA[r * 4] * p[0] + BA[r * 4 + 1] * p[1] + BA[r * 4 + 2] * p[2] +
                   BA[r * 4 + 3] * p[3];
        check(std::fabs(q[0]) < 1e-6 && std::fabs(q[1] - 2.0f) < 1e-6, "compose 先 A 后 B");
        // 奇异：全零矩阵
        const float zero16[16] = { 0 };
        check(!Stereo::invertTransform(zero16, Tinv), "invertTransform 奇异 → false");
    }

    // KdTree3（2026-11-10 迭代 4）：与暴力最近邻逐查询对拍
    {
        std::vector<Point3d> cloud;
        for (int i = 0; i < 200; ++i)   // x 唯一 → 全部点互异（并列距离对拍按距离断言）
            cloud.push_back(Point3d{ (double)i * 0.113, (double)((i * 37) % 23),
                                     (double)((i * 61) % 19) });
        KdTree3 tree;
        check(tree.build(cloud) && !tree.empty() && tree.size() == 200,
              "KdTree3 build 200 点");
        bool nearestOk = true, radiusOk = true;
        for (int t = 0; t < 50 && nearestOk && radiusOk; ++t) {
            const Point3d query{ (double)((t * 3) % 20) + 0.5, (double)((t * 7) % 18) + 0.25,
                                 (double)((t * 5) % 16) + 0.1 };
            int tIdx = -1;
            double tDist = 0;
            if (!tree.nearest(query, tIdx, tDist)) {
                nearestOk = false;
                break;
            }
            // 暴力最近距离（并列时取其一，按距离对拍）
            double bDist2 = 1e30;
            for (const Point3d& p : cloud) {
                const double dx = p.x - query.x, dy = p.y - query.y, dz = p.z - query.z;
                bDist2 = (std::min)(bDist2, dx * dx + dy * dy + dz * dz);
            }
            if (std::fabs(tDist - std::sqrt(bDist2)) > 1e-9)
                nearestOk = false;
            // 半径搜索对拍：按索引集合比较（树按距离排序，与扫描顺序无关）
            std::vector<int> ri;
            std::vector<double> rd;
            tree.radiusSearch(query, 3.0, ri, rd);
            std::vector<int> brute;
            for (int i = 0; i < (int)cloud.size(); ++i) {
                const double dx = cloud[(size_t)i].x - query.x;
                const double dy = cloud[(size_t)i].y - query.y;
                const double dz = cloud[(size_t)i].z - query.z;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) <= 3.0)
                    brute.push_back(i);
            }
            std::vector<int> riSorted = ri;
            std::sort(riSorted.begin(), riSorted.end());
            if (riSorted != brute || rd.size() != brute.size())
                radiusOk = false;
            for (size_t k = 1; radiusOk && k < rd.size(); ++k)
                if (rd[k] < rd[k - 1] - 1e-12)
                    radiusOk = false;   // 距离升序
        }
        check(nearestOk, "KdTree3 nearest 与暴力对拍（50 查询）");
        check(radiusOk, "KdTree3 radiusSearch 集合一致 + 距离升序");
        // 半径含边界点：恰在半径上的点必含
        std::vector<Point3d> axis = { { 0, 0, 0 }, { 2, 0, 0 } };
        KdTree3 t2;
        check(t2.build(axis), "KdTree3 build 2 点");
        std::vector<int> bi;
        std::vector<double> bd;
        t2.radiusSearch(Point3d{ 0, 0, 0 }, 2.0, bi, bd);
        check(bi.size() == 2 && std::fabs(bd[1] - 2.0) < 1e-12, "KdTree3 半径含边界点");
        // K 近邻（2026-11-11 迭代 7）：对拍暴力排序距离
        {
            const Point3d kq{ 10.5, 8.25, 6.1 };
            std::vector<int> ki;
            std::vector<double> kd;
            tree.knn(kq, 7, ki, kd);
            std::vector<double> all;
            for (const Point3d& p : cloud) {
                const double dx = p.x - kq.x, dy = p.y - kq.y, dz = p.z - kq.z;
                all.push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            std::sort(all.begin(), all.end());
            check((int)kd.size() == 7 &&
                  std::fabs(kd.back() - all[6]) < 1e-9 &&
                  std::fabs(kd.front() - all[0]) < 1e-9, "KdTree3 knn 7 近邻距离对拍");
            // 升序 + k>点数 = 全部 + k<=0 = 空
            bool asc = true;
            for (size_t k = 1; k < kd.size(); ++k)
                if (kd[k] < kd[k - 1] - 1e-12) asc = false;
            std::vector<int> ai;
            std::vector<double> ad;
            tree.knn(kq, 500, ai, ad);
            check(asc && ai.size() == 200, "KdTree3 knn 升序 + k>点数取全部");
            tree.knn(kq, 0, ai, ad);
            check(ai.empty(), "KdTree3 knn k<=0 → 空");
            std::vector<int> oneIdx;
            std::vector<double> oneDist;
            tree.knn(kq, 1, oneIdx, oneDist);
            int rIdx = -1; double rDist = 0;
            tree.nearest(kq, rIdx, rDist);
            check(oneIdx.size() == 1 && oneIdx[0] == rIdx &&
                  std::fabs(oneDist[0] - rDist) < 1e-12, "KdTree3 knn(1) = nearest");
        }
        // 空树/空输入/负半径
        KdTree3 t3;
        int zi = -1;
        double zd = 0;
        check(t3.empty() && t3.size() == 0 &&
              !t3.nearest(Point3d{ 0, 0, 0 }, zi, zd) && zi == -1 && zd == -1.0,
              "KdTree3 空树查询 false");
        std::vector<Point3d> none;
        check(!t3.build(none) && t3.empty(), "KdTree3 build 空点集 → false");
        t2.radiusSearch(Point3d{ 0, 0, 0 }, -1.0, bi, bd);
        check(bi.empty(), "KdTree3 负半径 → 空");
        // 移动语义（源对象保持非空不变量，与 Stereo 同模式）
        KdTree3 movedTree;
        movedTree.build(axis);
        const KdTree3 movedTarget = std::move(movedTree);
        check(!movedTarget.empty() &&
              movedTarget.nearest(Point3d{ 1.9, 0, 0 }, zi, zd) && zi == 1,
              "KdTree3 移动语义");
    }

    // minDistanceTo / findClosestPair KD 树加速正确性（对拍暴力）
    {
        std::vector<Point3d> pax, pbx;
        for (int i = 0; i < 120; ++i)
            pax.push_back(Point3d{ (double)((i * 13) % 30), (double)((i * 29) % 25),
                                   (double)((i * 7) % 21) });
        for (int i = 0; i < 90; ++i)
            pbx.push_back(Point3d{ (double)((i * 17) % 28) + 0.5, (double)((i * 31) % 24) + 0.5,
                                   (double)((i * 5) % 22) + 0.5 });
        const Stereo sa = Stereo::createFromPoints(pax);
        const Stereo sb = Stereo::createFromPoints(pbx);
        double brute = 1e30;
        for (const Point3d& x : pax)
            for (const Point3d& y : pbx) {
                const double dx = x.x - y.x, dy = x.y - y.y, dz = x.z - y.z;
                brute = (std::min)(brute, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        check(std::fabs(sa.minDistanceTo(sb) - brute) < 1e-12, "minDistanceTo KD 加速 = 暴力");
        Point3d ps, po;
        sa.findClosestPair(sb, ps, po);
        const double dd = std::sqrt((ps.x - po.x) * (ps.x - po.x) +
                                    (ps.y - po.y) * (ps.y - po.y) +
                                    (ps.z - po.z) * (ps.z - po.z));
        check(std::fabs(dd - brute) < 1e-12, "findClosestPair KD 加速 = 暴力");
    }

    // 点-面/点-盒判定（2026-11-10 迭代 4）
    {
        const double plane[4] = { 0, 0, 1, 0 };   // z = 0
        check(std::fabs(Stereo::distanceToPlane(Point3d{ 1, 2, 3 }, plane) - 3.0) < 1e-12,
              "点到平面有符号距离 +3");
        check(std::fabs(Stereo::distanceToPlane(Point3d{ 0, 0, -2 }, plane) + 2.0) < 1e-12,
              "点到平面负侧 -2");
        const double plane2[4] = { 0, 0, 2, -4 }; // z = 2（非单位法向）
        check(std::fabs(Stereo::distanceToPlane(Point3d{ 5, 5, 3.5 }, plane2) - 1.5) < 1e-12,
              "点到平面除以 |n|（(7-4)/2=1.5）");
        const double plane0[4] = { 0, 0, 0, 1 };
        check(Stereo::distanceToPlane(Point3d{ 0, 0, 0 }, plane0) == 0.0, "零法向 → 0");
        const AABB3d box{ 0, 0, 0, 2, 2, 2, true };
        check(Stereo::distanceToBox(Point3d{ 1, 1, 1 }, box) == 0.0, "盒内距离 0");
        check(std::fabs(Stereo::distanceToBox(Point3d{ 3, 4, 5 }, box) - std::sqrt(14.0)) < 1e-12,
              "盒外角距离 √14");
        AABB3d invalid;
        check(Stereo::distanceToBox(Point3d{ 0, 0, 0 }, invalid) == -1.0, "invalid 盒 → -1");
        check(Stereo::pointInAABB(Point3d{ 2, 2, 2 }, box) &&
              !Stereo::pointInAABB(Point3d{ 2.1, 1, 1 }, box), "pointInAABB 含边界");
        // OBB：20x10x4 盒绕 Z 转 30°（旋转矩阵行 = 主轴）
        const double ang = 30.0 * 3.14159265358979323846 / 180.0;
        const double ca = std::cos(ang), sa2 = std::sin(ang);
        OBB3d obb;
        obb.center = Point3d{ 0, 0, 0 };
        obb.extents[0] = 10; obb.extents[1] = 5; obb.extents[2] = 2;
        obb.rotation[0] = ca;  obb.rotation[1] = sa2; obb.rotation[2] = 0;
        obb.rotation[3] = -sa2; obb.rotation[4] = ca; obb.rotation[5] = 0;
        obb.rotation[6] = 0;    obb.rotation[7] = 0;  obb.rotation[8] = 1;
        obb.valid = true;
        // 局部系点 (8,3,1)（各轴余量 2/2/1，避开浮点边界）
        const Point3d in8{ 8 * ca - 3 * sa2, 8 * sa2 + 3 * ca, 1 };
        check(Stereo::pointInOBB(in8, obb), "pointInOBB 旋转盒内");
        const Point3d above{ 8 * ca - 3 * sa2, 8 * sa2 + 3 * ca, 3 };
        check(!Stereo::pointInOBB(above, obb), "pointInOBB 超半高在外");
        const Point3d farX{ 11 * ca, 11 * sa2, 0 };
        check(!Stereo::pointInOBB(farX, obb), "pointInOBB 长轴越界");
        OBB3d inv;
        check(!Stereo::pointInOBB(Point3d{ 0, 0, 0 }, inv), "pointInOBB invalid → false");
    }

    // 2.5D 桥接：立方体 XY 投影 = 4x4 正方形
    const Geom proj = s.projectTo2D();
    check(near(proj.area(), 16.0, 1e-6), "projectTo2D 凸包面积 16");
    // 投影退化：共线 3 点凸包无面积；<3 点返回空 Geom
    const Stereo line3 = Stereo::createFromPoints(
        std::vector<Point3d>{ { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } });
    check(near(line3.projectTo2D().area(), 0.0), "projectTo2D 共线点面积 0");
    check(Stereo::createFromPoints(std::vector<Point3d>{ { 0, 0, 0 }, { 1, 1, 1 } })
              .projectTo2D().isEmpty(), "projectTo2D <3 点 → 空 Geom");

    // 移动语义
    Stereo m = Stereo::createFromPoints(cube);
    const Stereo t = std::move(m);
    check(m.isEmpty() && t.size() == 8, "移动语义");
    check(Stereo::createFromPoints(std::vector<Point3d>()).isEmpty() &&
          Stereo::createFromPoints(std::vector<Point3d>()).minDistanceTo(s) == -1.0, "空对象安全");

    // 质心：立方体质心 = (2,2,2)
    {
        const Point3d c = s.getCentroid();
        check(near(c.x, 2.0) && near(c.y, 2.0) && near(c.z, 2.0), "getCentroid (2,2,2)");
    }

    // 坐标系变换：平移后质心随动、点间距离不变（刚体不变量）
    {
        const Stereo shifted = s.translate(10.0, 20.0, 30.0);
        const Point3d c = shifted.getCentroid();
        check(near(c.x, 12.0) && near(c.y, 22.0) && near(c.z, 32.0), "translate 质心 (12,22,32)");
        check(near(shifted.getAABBVolume(), 64.0, 1e-9), "平移不改变 AABB 体积");

        // 4x4 平移矩阵等价 translate
        const float T[16] = { 1,0,0,10,  0,1,0,20,  0,0,1,30,  0,0,0,1 };
        const Stereo viaMat = s.transform(T);
        const Point3d cm = viaMat.getCentroid();
        check(near(cm.x, 12.0) && near(cm.z, 32.0), "transform 4x4 平移");

        // 刚体不变量：任意两点的距离在变换后保持
        const std::vector<Point3d> pa = s.getPoints(), pb = viaMat.getPoints();
        double d0 = std::sqrt(std::pow(pa[0].x-pa[1].x,2)+std::pow(pa[0].y-pa[1].y,2)+std::pow(pa[0].z-pa[1].z,2));
        double d1 = std::sqrt(std::pow(pb[0].x-pb[1].x,2)+std::pow(pb[0].y-pb[1].y,2)+std::pow(pb[0].z-pb[1].z,2));
        check(near(d0, d1, 1e-9), "刚体变换保持点距");

        // 绕 z 转 90° 后 (4,0,0) → (0,4,0)
        const float Rz90[16] = { 0,-1,0,0,  1,0,0,0,  0,0,1,0,  0,0,0,1 };
        const Stereo rotated = s.transform(Rz90);
        bool found = false;
        for (const Point3d& p : rotated.getPoints())
            if (near(p.x, 0) && near(p.y, 4.0) && near(p.z, 0)) { found = true; break; }
        check(found, "transform 绕z 90° (4,0,0)→(0,4,0)");
    }
}

// ---------------- ImageProcessor ----------------

static void testImageProcessor()
{
    std::printf("== ImageProcessor ==\n");
    // 100x100 灰度图，左半 200 右半 50
    cv::Mat img(100, 100, CV_8UC1, cv::Scalar(50));
    img(cv::Rect(0, 0, 50, 100)).setTo(200);

    cv::Mat bin;
    shapely::ImageProcessor::threshold(img, bin, 100, 255, cv::THRESH_BINARY);
    check((int)cv::sum(bin)[0] == 50 * 100 * 255, "threshold 左半二值化");

    cv::Mat blur;
    shapely::ImageProcessor::gaussianBlur(img, blur, cv::Size(5, 5), 1.0);
    check(!blur.empty(), "gaussianBlur");

    cv::Mat edges;
    shapely::ImageProcessor::canny(img, edges, 50, 150);
    check(cv::countNonZero(edges) > 0, "canny 边缘非空");

    // 轮廓 → Geom 联动（左半矩形 → 面积 50x100）
    std::vector<std::vector<cv::Point>> contours;
    shapely::ImageProcessor::findContours(bin, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    check(!contours.empty(), "findContours 非空");
    if (!contours.empty()) {
        // 全局坐标直接进 Geom（鸭子类型，无需转换）
        const Geom poly = Geom::createPolygon(contours[0]);
        check(near(poly.area(), 49.0 * 99.0, 1.0), "轮廓→Geom 面积 49x99（像素中心）");
        const cv::Rect br = shapely::ImageProcessor::getBoundingRect(contours[0]);
        check(br.width == 50 && br.height == 100, "getBoundingRect 50x100");
    }

    // 模板匹配：在右半找 10x10 的 50 值模板
    cv::Mat templ(10, 10, CV_8UC1, cv::Scalar(50));
    cv::Mat result;
    shapely::ImageProcessor::matchTemplate(img, templ, result, cv::TM_SQDIFF);
    cv::Point loc;
    double score = 0;
    shapely::ImageProcessor::findBestMatch(result, cv::TM_SQDIFF, loc, score);
    check(loc.x >= 50 && loc.x <= 90, "findBestMatch 定位右半");

    // 瓦片：100x100 切 40x40 无重叠 → 3x3=9 块（covered_roi 处理边界）
    std::vector<shapely::Tile> tiles;
    shapely::ImageProcessor::splitIntoTiles(img, cv::Size(40, 40), 0, tiles);
    check(tiles.size() == 9, "splitIntoTiles 3x3=9");
    if (tiles.size() == 9) {
        const shapely::Tile& last = tiles.back();
        check(last.covered_roi.width == 20 && last.covered_roi.height == 20,
              "边界瓦片 covered_roi 20x20");
        check(last.image.cols == 20 && last.image.rows == 20, "瓦片图像尺寸 = covered_roi");
    }

    // 金字塔瓦片：2 层
    std::vector<std::vector<shapely::Tile>> pyramid;
    shapely::ImageProcessor::buildTilePyramid(img, cv::Size(40, 40), 1, pyramid);
    check(pyramid.size() == 2, "buildTilePyramid 2 层");
    if (pyramid.size() == 2) {
        check(near(pyramid[1][0].scale, 0.5), "level1 scale 0.5");
        check(pyramid[1][0].roi_base.width == 80, "level1 roi_base 映射回 level0");
    }

    // 金字塔
    std::vector<cv::Mat> pyr;
    shapely::ImageProcessor::buildGaussianPyramid(img, pyr, 2);
    check(pyr.size() == 3 && pyr[2].cols == 25, "buildGaussianPyramid 3 层 25 宽");

    // 零拷贝验证：瓦片改值会影响源图（共享内存）
    if (!tiles.empty()) {
        tiles[0].image.at<uchar>(0, 0) = 255;
        check(img.at<uchar>(0, 0) == 255, "瓦片零拷贝（共享内存）");
    }

    // makeKernel：椭圆结构元尺寸正确、非全零
    {
        const cv::Mat k = shapely::ImageProcessor::makeKernel(cv::MORPH_ELLIPSE, cv::Size(5, 5));
        check(k.size() == cv::Size(5, 5) && k.type() == CV_8U && cv::countNonZero(k) > 0,
              "makeKernel 椭圆 5x5");
    }

    // 连通域：左上 10x10 块 + 右下 20x20 块 + 3 个孤立噪点 → 5 个域
    {
        cv::Mat bw(100, 100, CV_8UC1, cv::Scalar(0));
        bw(cv::Rect(5, 5, 10, 10)).setTo(255);
        bw(cv::Rect(60, 60, 20, 20)).setTo(255);
        bw.at<uchar>(30, 30) = 255;
        bw.at<uchar>(35, 30) = 255;
        bw.at<uchar>(40, 40) = 255;
        cv::Mat labels;
        std::vector<int> areas;
        const int n = shapely::ImageProcessor::connectedComponents(bw, labels, areas, 8);
        check(n == 6, "connectedComponents 返回 6（5 个前景域 + 背景行）");
        if (n >= 3) {
            // 域面积（下标 0 = 背景）：大块 400、小块 100、噪点域 3
            std::vector<int> fg = areas;
            fg.erase(fg.begin());   // 去掉背景
            std::sort(fg.begin(), fg.end());
            check(fg.size() >= 3 && fg.back() == 400 && fg[fg.size() - 2] == 100,
                  "各域像素面积 400/100/3");
        }
        // 面积过滤业务语义：>= 50 的域只有 2 个
        int kept = 0;
        for (size_t i = 1; i < areas.size(); ++i)
            if (areas[i] >= 50) ++kept;
        check(kept == 2, "面积过滤后剩 2 个有效域");
    }

    // findBestMatch 评分分支：SQDIFF 系取最小、其余取最大（非透传逻辑，易错点）
    {
        cv::Mat r(3, 3, CV_32FC1, cv::Scalar(0.5f));
        r.at<float>(0, 2) = 0.1f;   // 最小值在 (x=2,y=0)
        r.at<float>(2, 1) = 0.9f;   // 最大值在 (x=1,y=2)
        shapely::ImageProcessor::findBestMatch(r, cv::TM_SQDIFF, loc, score);
        check(loc == cv::Point(2, 0) && near(score, 0.1), "findBestMatch SQDIFF 取最小 (2,0)");
        shapely::ImageProcessor::findBestMatch(r, cv::TM_SQDIFF_NORMED, loc, score);
        check(loc == cv::Point(2, 0), "findBestMatch SQDIFF_NORMED 同取最小");
        shapely::ImageProcessor::findBestMatch(r, cv::TM_CCORR_NORMED, loc, score);
        check(loc == cv::Point(1, 2) && near(score, 0.9), "findBestMatch CCORR_NORMED 取最大 (1,2)");
    }

    // 带 overlap 瓦片：瓦 40、重叠 10 → 步距 30 → 4x4=16 块；roi 恒为设计网格，covered_roi 裁到边界
    {
        std::vector<shapely::Tile> otiles;
        shapely::ImageProcessor::splitIntoTiles(img, cv::Size(40, 40), 10, otiles);
        check(otiles.size() == 16, "splitIntoTiles overlap 10 → 4x4=16");
        bool roiGrid = !otiles.empty();
        for (const shapely::Tile& t : otiles)
            if (t.roi.width != 40 || t.roi.height != 40) roiGrid = false;
        check(roiGrid, "瓦片 roi 尺寸恒等于设计瓦 40x40");
        check(otiles.size() == 16 && otiles[3].roi == cv::Rect(90, 0, 40, 40) &&
              otiles[3].covered_roi == cv::Rect(90, 0, 10, 40),
              "右缘瓦片 roi 设计网格、covered_roi 裁为 10x40");
        check(!otiles.empty() && otiles.back().covered_roi == cv::Rect(90, 90, 10, 10) &&
              otiles.back().image.cols == 10 && otiles.back().image.rows == 10,
              "右下瓦片 covered_roi/图像 10x10");
    }

    // 金字塔瓦片契约（Tile 语义 4/5/6）：roi_base = covered_roi 按 1/scale 映射回 level0，level/scale 字段正确
    if (pyramid.size() == 2) {
        check(pyramid[1].size() == 4, "level1 瓦片网格 2x2=4");
        if (pyramid[1].size() == 4) {
            check(pyramid[1][3].roi_base == cv::Rect(80, 80, 20, 20),
                  "level1 右下瓦片 roi_base=(80,80,20,20)");
            check(pyramid[1][1].level == 1 && near(pyramid[1][1].scale, 0.5),
                  "level1 瓦片 level/scale 字段");
        }
    }

    // 形态学：erode/dilate 3x3 于 8x8 块，CLOSE 填 2x2 小孔
    {
        cv::Mat m(20, 20, CV_8UC1, cv::Scalar(0));
        m(cv::Rect(5, 5, 8, 8)).setTo(255);
        const cv::Mat k3 = shapely::ImageProcessor::makeKernel(cv::MORPH_RECT, cv::Size(3, 3));
        cv::Mat er, dl;
        shapely::ImageProcessor::erode(m, er, k3);
        check(cv::countNonZero(er) == 36, "erode 3x3：8x8 块 → 36 px");
        shapely::ImageProcessor::dilate(m, dl, k3);
        check(cv::countNonZero(dl) == 100, "dilate 3x3：8x8 块 → 100 px");
        cv::Mat holed = m.clone();
        holed(cv::Rect(8, 8, 2, 2)).setTo(0);   // 中间小孔
        cv::Mat cl;
        shapely::ImageProcessor::morphologyEx(holed, cl, cv::MORPH_CLOSE, k3);
        check(cv::countNonZero(cl) == 64, "morphologyEx CLOSE 填 2x2 小孔 → 64 px");
    }

    // makeKernel 形状参数：CROSS 3x3 五像素、RECT 5x5 全 1
    {
        const cv::Mat kc = shapely::ImageProcessor::makeKernel(cv::MORPH_CROSS, cv::Size(3, 3));
        check(cv::countNonZero(kc) == 5, "makeKernel CROSS 3x3 五像素");
        const cv::Mat kr = shapely::ImageProcessor::makeKernel(cv::MORPH_RECT, cv::Size(5, 5));
        check(cv::countNonZero(kr) == 25, "makeKernel RECT 5x5 全 1");
    }

    // ---------------- 扩展组 10-18：色彩/增强/LUT/积分图/距离/连通/形状/直方图/重映射 ----------------

    using IP = shapely::ImageProcessor;

    // 10. 色彩空间转换
    {
        cv::Mat color(10, 10, CV_8UC3, cv::Scalar(30, 60, 90));
        cv::Mat gray;
        IP::cvtColor(color, gray, cv::COLOR_BGR2GRAY);
        check(gray.type() == CV_8UC1 && !gray.empty(), "cvtColor BGR2GRAY 输出 8UC1");
        // BGR=(30,60,90)：0.299R+0.587G+0.114B = 65.55，OpenCV 定点实现取 66
        check(std::fabs(gray.at<uint8_t>(5, 5) - 66) <= 1, "cvtColor 灰度值 ≈ 0.299R+0.587G+0.114B");
        cv::Mat back;
        IP::cvtColor(gray, back, cv::COLOR_GRAY2BGR);
        check(back.type() == CV_8UC3 && back.channels() == 3, "cvtColor GRAY2BGR 回 3 通道");
        cv::Mat empty;
        IP::cvtColor(empty, back, cv::COLOR_BGR2GRAY);
        check(back.empty(), "cvtColor 空输入 → 空输出");
    }

    // 11. 灰度增强
    {
        cv::Mat low(50, 50, CV_8UC1, cv::Scalar(10));
        low(cv::Rect(0, 0, 25, 50)).setTo(20);          // 动态范围只有 [10,20]
        cv::Mat eq;
        IP::equalizeHist(low, eq);
        double eq_max = 0.0;
        cv::minMaxLoc(eq, nullptr, &eq_max);
        check(!eq.empty() && eq_max > 200, "equalizeHist 拉开动态范围");
        cv::Mat cl;
        IP::clahe(low, cl, 2.0, cv::Size(8, 8));
        check(!cl.empty() && cl.type() == CV_8UC1, "clahe 输出 8UC1");
        cv::Mat empty;
        IP::clahe(empty, cl);
        check(cl.empty(), "clahe 空输入 → 空输出");
    }

    // 12. LUT 与像素级运算
    {
        cv::Mat src(10, 10, CV_8UC1, cv::Scalar(100));
        cv::Mat lutT(1, 256, CV_8UC1);
        for (int i = 0; i < 256; ++i)
            lutT.at<uint8_t>(i) = (uint8_t)(255 - i);       // 反转表
        cv::Mat dst;
        IP::lut(src, lutT, dst);
        check(dst.at<uint8_t>(0, 0) == 155, "lut 查表 255-100=155");

        cv::Mat inv;
        IP::bitwiseNot(src, inv);
        check(inv.at<uint8_t>(0, 0) == 155, "bitwiseNot 255-100");

        cv::Mat a(5, 5, CV_8UC1, cv::Scalar(0xF0));
        cv::Mat b(5, 5, CV_8UC1, cv::Scalar(0x0F));
        cv::Mat dst2;
        IP::bitwiseAnd(a, b, dst2);
        check((int)cv::countNonZero(dst2) == 0, "bitwiseAnd F0&0F=00");
        IP::bitwiseOr(a, b, dst2);
        check((int)cv::countNonZero(dst2) == 25, "bitwiseOr F0|0F=FF");

        cv::Mat m1(5, 5, CV_8UC1, cv::Scalar(50));
        cv::Mat m2(5, 5, CV_8UC1, cv::Scalar(80));
        cv::Mat diff;
        IP::absdiff(m2, m1, diff);
        check(diff.at<uint8_t>(0, 0) == 30, "absdiff |80-50|=30");
        IP::absdiff(m1, m2, diff);
        check(diff.at<uint8_t>(0, 0) == 30, "absdiff 对称 |50-80|=30");
    }

    // 13. 积分图
    {
        cv::Mat src(6, 6, CV_8UC1, cv::Scalar(1));
        cv::Mat sum;
        IP::integral(src, sum, CV_32S);
        check(sum.type() == CV_32SC1 && sum.rows == 7 && sum.cols == 7,
              "integral 尺寸 +1");
        check(sum.at<int>(6, 6) == 36, "integral 全图和 = 36");
        // 区域 (2..4, 2..4) 内 9 个像素 → 9
        const int area = sum.at<int>(5, 5) - sum.at<int>(2, 5) - sum.at<int>(5, 2) + sum.at<int>(2, 2);
        check(area == 9, "integral 4 次查表求 3x3 区域和");
    }

    // 14. 距离变换
    {
        cv::Mat m(11, 11, CV_8UC1, cv::Scalar(0));
        m(cv::Rect(4, 4, 3, 3)).setTo(255);              // 中心 3x3 块
        cv::Mat dist;
        IP::distanceTransform(m, dist, cv::DIST_L2, 5);
        check(dist.type() == CV_32F && !dist.empty(), "distanceTransform 输出 CV_32F");
        double maxv = 0;
        cv::minMaxLoc(dist, nullptr, &maxv);
        check(std::fabs(maxv - 2.0) < 1e-3 || std::fabs(maxv - 1.5) < 1e-3,
              "distanceTransform 3x3 块中心距离 ≈ 1.5~2.0");
        cv::Mat empty;
        IP::distanceTransform(empty, dist, cv::DIST_L2, 5);
        check(dist.empty(), "distanceTransform 空输入 → 空输出");
    }

    // 15. 连通分析增强 + floodFill
    {
        cv::Mat bin(40, 40, CV_8UC1, cv::Scalar(0));
        bin(cv::Rect(2, 2, 10, 10)).setTo(255);          // 域 1：面积 100 @ (2,2)
        bin(cv::Rect(20, 20, 4, 4)).setTo(255);          // 域 2：面积 16 @ (20,20)
        cv::Mat labels, stats, cents;
        int n = IP::connectedComponentsWithStats(bin, labels, stats, cents, 8);
        // 返回值含背景行：2 个前景域 → n = 3（标签 0 为背景）
        check(n == 3, "CCWithStats 含背景行 → 2 前景域");
        bool found = false;
        for (int i = 1; i < n; ++i)
            if (stats.at<int>(i, cv::CC_STAT_AREA) == 100) {
                found = true;
                check(stats.at<int>(i, cv::CC_STAT_LEFT) == 2 &&
                      stats.at<int>(i, cv::CC_STAT_TOP) == 2,
                      "CCWithStats 包围盒 @ (2,2)");
                check(std::fabs(cents.at<double>(i, 0) - 6.5) < 1e-6 &&
                      std::fabs(cents.at<double>(i, 1) - 6.5) < 1e-6,
                      "CCWithStats 质心 (6.5,6.5)");
            }
        check(found, "CCWithStats 找到面积 100 的域");

        cv::Mat img(20, 20, CV_8UC1, cv::Scalar(50));
        cv::Rect filled;
        int px = IP::floodFill(img, nullptr, cv::Point(5, 5), cv::Scalar(200),
                               cv::Scalar(), cv::Scalar(), &filled);
        check(px == 400 && filled == cv::Rect(0, 0, 20, 20),
              "floodFill 全图 400 px 重绘");
        check(img.at<uint8_t>(10, 10) == 200, "floodFill 就地改写像素值");
    }

    // 16. 形状度量
    {
        std::vector<cv::Point> square = { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } };
        double area = IP::contourArea(square);
        check(std::fabs(area - 100.0) < 1e-6, "contourArea 方形 = 100");
        check(IP::isContourConvex(square), "isContourConvex 方形 = true");
        std::vector<cv::Point> lshape = {
            { 0, 0 }, { 10, 0 }, { 10, 4 }, { 4, 4 }, { 4, 10 }, { 0, 10 }
        };
        check(!IP::isContourConvex(lshape), "isContourConvex L 形 = false");
        std::vector<cv::Point> pt1 = { { 0, 0 } };
        check(IP::contourArea(pt1) == 0.0, "contourArea <3 点 → 0");

        std::vector<cv::Point> circle;
        for (int i = 0; i < 20; ++i) {
            const double th = 2.0 * M_PI * i / 20;
            circle.push_back(cv::Point((int)std::lround(20 * std::cos(th)),
                                       (int)std::lround(20 * std::sin(th))));
        }
        cv::RotatedRect ell = IP::fitEllipse(circle);
        check(std::fabs(ell.center.x) < 1.0 && std::fabs(ell.center.y) < 1.0,
              "fitEllipse 圆心 ≈ 原点");
        check(std::fabs(ell.size.width - 40.0) < 4.0 && std::fabs(ell.size.height - 40.0) < 4.0,
              "fitEllipse 直径 ≈ 40");
        std::vector<cv::Point> two = { { 0, 0 }, { 1, 1 } };
        cv::RotatedRect bad_ell = IP::fitEllipse(two);
        check(bad_ell.size.width == 0.0f && bad_ell.size.height == 0.0f,
              "fitEllipse <5 点 → 空");

        std::vector<cv::Point> line5;
        for (int x = 0; x < 5; ++x)
            line5.push_back(cv::Point(x, 2 * x + 1));
        std::vector<float> ln;
        IP::fitLine(line5, ln, cv::DIST_HUBER);
        check(ln.size() == 4 && std::fabs(std::fabs(ln[1] / ln[0]) - 2.0) < 1e-3,
              "fitLine 斜率 = 2");
        std::vector<float> ln_single;
        IP::fitLine(std::vector<cv::Point>{ { 0, 0 } }, ln_single, cv::DIST_L2);
        check(ln_single.empty(), "fitLine 单点 → 空（不崩溃）");

        cv::Point2f cc; float rr;
        IP::minEnclosingCircle(square, cc, rr);
        check(std::fabs(cc.x - 5.0f) < 1e-3 && std::fabs(cc.y - 5.0f) < 1e-3 &&
              std::fabs(rr - 7.07f) < 0.05f, "minEnclosingCircle 对角外接");

        double inside = IP::pointPolygonTest(square, cv::Point2f(5, 5), false);
        double outside = IP::pointPolygonTest(square, cv::Point2f(15, 5), false);
        check(inside > 0 && outside < 0, "pointPolygonTest 内 +1 / 外 -1");
        double distIn = IP::pointPolygonTest(square, cv::Point2f(5, 5), true);
        check(std::fabs(distIn - 5.0) < 1e-3, "pointPolygonTest 带距离 = 5（到边）");

        double self = IP::matchShapes(square, square, cv::CONTOURS_MATCH_I1);
        double other = IP::matchShapes(square, circle, cv::CONTOURS_MATCH_I1);
        check(std::fabs(self) < 1e-6 && other > 1e-6, "matchShapes 自身≈0 / 异形>0");

        std::vector<int> hull;
        cv::convexHull(lshape, hull, false, false);
        std::vector<cv::Vec4i> defects;
        IP::convexityDefects(lshape, hull, defects);
        check(defects.size() == 1, "convexityDefects L 形 1 个凹陷");
    }

    // 17. 直方图
    {
        cv::Mat src(10, 10, CV_8UC1, cv::Scalar(100));
        std::vector<float> h1;
        IP::calcHist8U(src, h1);
        check(h1.size() == 256 && h1[100] == 100.0f, "calcHist8U 256 bins / 100 值 100 次");
        double sim = IP::compareHist(h1, h1, cv::HISTCMP_CORREL);
        check(std::fabs(sim - 1.0) < 1e-6, "compareHist 自身相关 = 1");
        std::vector<float> h2;
        cv::Mat other(10, 10, CV_8UC1, cv::Scalar(200));
        IP::calcHist8U(other, h2);
        double diffh = IP::compareHist(h1, h2, cv::HISTCMP_BHATTACHARYYA);
        check(diffh > 0.5, "compareHist 不同分布 B 氏距离 > 0.5");
    }

    // 18. 重映射
    {
        cv::Mat src(10, 10, CV_8UC1, cv::Scalar(0));
        src.at<uint8_t>(5, 5) = 99;
        cv::Mat map1(10, 10, CV_32FC1), map2(10, 10, CV_32FC1);
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x) {
                map1.at<float>(y, x) = (float)x;
                map2.at<float>(y, x) = (float)y;
            }
        cv::Mat dst;
        IP::remap(src, dst, map1, map2, cv::INTER_LINEAR);
        check(dst.at<uint8_t>(5, 5) == 99, "remap 恒等映射像素不变");
        cv::Mat sh1 = map1 + 1.0f;
        IP::remap(src, dst, sh1, map2, cv::INTER_NEAREST);
        // map 语义是 dst(y,x)=src(map1,map2)：map1=x+1 → 内容左移一列，src(5,5) 落到 dst(5,4)
        check(dst.at<uint8_t>(5, 4) == 99, "remap 平移映射（内容左移）");

        cv::Mat ring(100, 100, CV_8UC1, cv::Scalar(0));
        ring(cv::Rect(45, 45, 10, 10)).setTo(255);
        cv::Mat polar;
        IP::warpPolar(ring, polar, cv::Size(360, 50), cv::Point2f(50, 50), 50.0,
                      cv::WARP_POLAR_LINEAR);
        check(!polar.empty() && polar.rows == 50 && polar.cols == 360,
              "warpPolar 输出 360x50 条带");
        check((int)cv::countNonZero(polar) > 0, "warpPolar 中心亮区被展开");
    }

    // 19. 绘图标注（2026-11-10 迭代新增：业务层标注不再被迫 include imgproc）
    {
        cv::Mat canvas = cv::Mat::zeros(40, 40, CV_8UC1);   // 灰度图（Scalar 单值）
        IP::drawLine(canvas, { 5, 0 }, { 5, 39 }, cv::Scalar(255), 1);
        check((int)cv::countNonZero(canvas) == 40, "drawLine 竖线 40 px");

        canvas.setTo(0);
        IP::drawCircle(canvas, { 20, 20 }, 10, cv::Scalar(255), 1);   // 描边
        check((int)canvas.at<uint8_t>(20, 30) == 255 &&
              (int)canvas.at<uint8_t>(20, 20) == 0, "drawCircle 描边圆心不亮");
        IP::drawCircle(canvas, { 20, 20 }, 3, cv::Scalar(255), -1);   // 实心
        check((int)canvas.at<uint8_t>(20, 20) == 255, "drawCircle 实心圆心亮");

        canvas.setTo(0);
        IP::drawRect(canvas, cv::Rect(2, 2, 10, 8), cv::Scalar(255), -1);   // 实心
        check((int)cv::countNonZero(canvas) == 80, "drawRect 实心 10x8=80 px");
        IP::drawRect(canvas, cv::Rect(2, 2, 10, 8), cv::Scalar(0), 1);      // 描边清回
        check((int)cv::countNonZero(canvas) == 48 &&                           // 80-32 边框
              (int)canvas.at<uint8_t>(5, 5) == 255 &&                          // 内部保持
              (int)canvas.at<uint8_t>(2, 2) == 0,                              // 边框被清
              "drawRect 描边清除边框 32 px（内部 48 保持）");

        canvas.setTo(0);
        const std::vector<cv::Point> tri{ { 5, 5 }, { 25, 5 }, { 15, 25 } };
        IP::drawPolylines(canvas, tri, true, cv::Scalar(255), 1);
        check((int)canvas.at<uint8_t>(5, 15) == 255, "drawPolylines 边上像素");
        IP::fillPolygon(canvas, tri, cv::Scalar(255));
        check((int)cv::countNonZero(canvas) > 100, "fillPolygon 内部填充");

        canvas.setTo(0);
        IP::drawCross(canvas, { 20, 20 }, 5, cv::Scalar(255), 1);
        check((int)cv::countNonZero(canvas) == 21, "drawCross 十字 2*5+1 每臂 11 px");
        IP::drawText(canvas, "12", { 2, 38 }, cv::Scalar(255));
        check((int)cv::countNonZero(canvas) > 21, "drawText 有像素落笔");

        // 空图 no-op（不抛异常、不崩溃）
        cv::Mat nil;
        IP::drawLine(nil, { 0, 0 }, { 9, 9 }, cv::Scalar(255));
        IP::drawText(nil, "x", { 0, 0 }, cv::Scalar(255));
        check(nil.empty(), "绘图空图 no-op");
    }

    // 20. 模板匹配增强（2026-11-10 迭代 3：掩膜/多目标/亚像素；NCC = TM_CCOEFF_NORMED 已有）
    {
        // 掩膜匹配：真块 (5,5)，干扰块 (12,12) 仅底部不同——掩膜应忽略底部差异
        cv::Mat img = cv::Mat::zeros(20, 20, CV_8UC1);
        const uchar tv[2][2] = { { 100, 120 }, { 140, 160 } };   // 模板上半 2x2 特征
        cv::Mat templ = cv::Mat::zeros(4, 4, CV_8UC1);
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 2; ++c) {
                templ.at<uint8_t>(r, c) = tv[r][c];
                img.at<uint8_t>(5 + r, 5 + c) = tv[r][c];       // 真块：上半同、下半 255
                img.at<uint8_t>(12 + r, 12 + c) = tv[r][c];     // 干扰块：同构
            }
        img(cv::Rect(5, 7, 4, 2)).setTo(255);
        img(cv::Rect(12, 14, 4, 2)).setTo(255);
        cv::Mat mask = cv::Mat::zeros(4, 4, CV_8UC1);
        mask(cv::Rect(0, 0, 4, 2)).setTo(255);                  // 只匹配上半

        cv::Mat resMasked, resPlain;
        IP::matchTemplateMasked(img, templ, mask, resMasked, cv::TM_SQDIFF);
        IP::matchTemplate(img, templ, resPlain, cv::TM_SQDIFF);
        check(!resMasked.empty(), "matchTemplateMasked 输出");
        check(std::fabs(resMasked.at<float>(12, 12)) < 1e-6f &&
              resPlain.at<float>(12, 12) > 0.f, "掩膜忽略遮挡区差异（plain 仍有差）");
        cv::Point mloc; double msc;
        IP::findBestMatch(resMasked, cv::TM_SQDIFF, mloc, msc);
        check(mloc.x == 5 && mloc.y == 5 && msc == 0.0, "掩膜 SQDIFF 最优 = 真块 0 差");
        cv::Mat resCC;
        IP::matchTemplateMasked(img, templ, mask, resCC, cv::TM_CCORR_NORMED);
        cv::Point cloc; double csc;
        IP::findBestMatch(resCC, cv::TM_CCORR_NORMED, cloc, csc);
        check(cloc.x == 5 && cloc.y == 5 && std::fabs(csc - 1.0) < 1e-3,
              "掩膜 CCORR_NORMED 最优 = 1.0（NaN/Inf 已归一）");
        // 全图有限性：零范数背景不残留 NaN/Inf 带偏 minMaxLoc
        bool finite = true;
        for (int r = 0; finite && r < resCC.rows; ++r)
            for (int c = 0; c < resCC.cols; ++c)
                finite = std::isfinite(resCC.at<float>(r, c));
        check(finite, "掩膜 CCORR_NORMED 全图有限");
        cv::Mat resBad;
        IP::matchTemplateMasked(img, templ, mask, resBad, cv::TM_CCOEFF_NORMED);
        check(resBad.empty(), "不支持的掩膜 method → 空（OpenCV 4.4 限制）");

        // 亚像素：解析解校验（抛物线顶点）
        cv::Mat score = cv::Mat::zeros(9, 9, CV_32F);
        score.at<float>(5, 5) = 1.0f;
        score.at<float>(5, 4) = 0.9f;
        score.at<float>(5, 6) = 0.7f;
        cv::Point2f sloc; double ssc;
        IP::findBestMatchSubPix(score, cv::TM_CCOEFF_NORMED, sloc, ssc);
        check(std::fabs(sloc.x - 4.75f) < 1e-6 && std::fabs(sloc.y - 5.0f) < 1e-6 &&
              std::fabs(ssc - 1.0125) < 1e-6, "亚像素 max 解析解 (4.75,5) 1.0125");
        cv::Mat diff = cv::Mat::ones(9, 9, CV_32F);   // 背景 1.0（0 会比峰更"小"）
        diff.at<float>(5, 5) = 0.1f;
        diff.at<float>(5, 4) = 0.3f;
        diff.at<float>(5, 6) = 0.5f;
        IP::findBestMatchSubPix(diff, cv::TM_SQDIFF, sloc, ssc);
        check(std::fabs(sloc.x - (5.0 - 1.0 / 6.0)) < 1e-6 && std::fabs(sloc.y - 5.0f) < 1e-6 &&
              std::fabs(ssc - (0.1 - 0.01 / 1.2)) < 1e-6, "亚像素 min 解析解");
        cv::Mat edge = cv::Mat::zeros(5, 5, CV_32F);
        edge.at<float>(2, 0) = 1.0f;
        IP::findBestMatchSubPix(edge, cv::TM_CCOEFF_NORMED, sloc, ssc);
        check(sloc.x == 0.f && sloc.y == 2.f && ssc == 1.0, "亚像素贴边保持整数峰");

        // 多目标：三峰 0.9 + 一峰 0.5
        cv::Mat multi = cv::Mat::zeros(20, 20, CV_32F);
        multi.at<float>(3, 3) = 0.9f;
        multi.at<float>(10, 10) = 0.9f;
        multi.at<float>(17, 17) = 0.9f;
        multi.at<float>(15, 5) = 0.5f;
        std::vector<shapely::Match2d> hits;
        IP::findAllMatches(multi, cv::TM_CCOEFF_NORMED, 0.8, 10, 4, hits);
        check(hits.size() == 3 && std::fabs(hits[0].score - 0.9) < 1e-6,
              "findAllMatches 3 目标按序");
        bool all3 = hits.size() == 3;
        if (all3)
            for (const auto& h : hits)
                all3 = all3 && std::fabs(h.score - 0.9) < 1e-6 &&
                       ((h.loc.x == 3 && h.loc.y == 3) || (h.loc.x == 10 && h.loc.y == 10) ||
                        (h.loc.x == 17 && h.loc.y == 17));
        check(all3, "findAllMatches 位置精确");
        std::vector<shapely::Match2d> none;
        IP::findAllMatches(multi, cv::TM_CCOEFF_NORMED, 0.95, 10, 4, none);
        check(none.empty(), "findAllMatches 阈值以上无目标 → 空");
        std::vector<shapely::Match2d> two;
        IP::findAllMatches(multi, cv::TM_CCOEFF_NORMED, 0.8, 2, 4, two);
        check(two.size() == 2, "findAllMatches max_matches 截断");
        // SQDIFF 系（取最小 + 阈值 ≤ 语义）
        cv::Mat valleys = cv::Mat::ones(20, 20, CV_32F);
        valleys.at<float>(4, 6) = 0.05f;
        valleys.at<float>(14, 16) = 0.05f;
        std::vector<shapely::Match2d> lows;
        IP::findAllMatches(valleys, cv::TM_SQDIFF, 0.1, 10, 3, lows);
        check(lows.size() == 2 && std::fabs(lows[0].score - 0.05) < 1e-6,
              "findAllMatches SQDIFF 取小语义");

        // 旋转模板对位（2026-11-11 迭代 8）：模板 12x12 特征图案 → 图中 15° 贴放
        cv::Mat tpl(12, 12, CV_8UC1, cv::Scalar(0));
        cv::circle(tpl, { 6, 6 }, 4, cv::Scalar(255), -1);
        tpl.at<uint8_t>(2, 2) = 255;   // 破坏旋转对称的角点特征
        cv::Mat scene = cv::Mat::zeros(80, 80, CV_8UC1);
        {
            cv::Mat M = cv::getRotationMatrix2D(cv::Point2f(6.f, 6.f), 15.0, 1.0);
            M.at<double>(0, 2) += 40.0 - 6.0;
            M.at<double>(1, 2) += 35.0 - 6.0;
            cv::warpAffine(tpl, scene, M, scene.size(), cv::INTER_LINEAR,
                           cv::BORDER_CONSTANT, cv::Scalar::all(0));
        }
        const shapely::RotatedMatch2d rm = IP::findRotatedMatch(scene, tpl, -30.0, 30.0, 2.0);
        // 角分辨率受模板对称性限制：12px 模板 3° 差的特征位移 <1px，容差取 2 步距
        check(std::fabs(rm.angle - 15.0) <= 4.0, "旋转对位命中角度 15°±4°");
        check(std::fabs(rm.loc.x - 40.0f) < 2.5f && std::fabs(rm.loc.y - 35.0f) < 2.5f &&
              rm.score > 0.8, "旋转对位中心/得分");
        const shapely::RotatedMatch2d bad = IP::findRotatedMatch(cv::Mat(), tpl, 0, 10, 1.0);
        check(bad.loc.x == -1.f && bad.loc.y == -1.f, "旋转对位空图 → (-1,-1)");
        const shapely::RotatedMatch2d neg =
            IP::findRotatedMatch(scene, tpl, 30.0, -30.0, 2.0);
        check(neg.loc.x == -1.f, "旋转对位 angle_end<start → (-1,-1)");

        // 沿线卡尺（2026-11-12 迭代 12）：x=20 竖直台阶边（暗→亮）
        cv::Mat ramp = cv::Mat(40, 40, CV_8UC1, cv::Scalar(50));
        ramp(cv::Rect(20, 0, 20, 40)).setTo(200);
        std::vector<cv::Point2f> edges;
        const int nE = IP::caliperEdges(ramp, { 5, 20 }, { 35, 20 }, 64, 1, 20.0, edges);
        check(nE == 1 && std::fabs(edges[0].x - 20.0f) < 1.5f &&
              std::fabs(edges[0].y - 20.0f) < 1e-6, "卡尺台阶边亚像素 (20,20)");
        std::vector<cv::Point2f> edgesNeg;
        check(IP::caliperEdges(ramp, { 5, 20 }, { 35, 20 }, 64, -1, 20.0, edgesNeg) == 0,
              "卡尺极性过滤 → 0");
        std::vector<cv::Point2f> edgesNone;
        check(IP::caliperEdges(ramp, { 5, 20 }, { 35, 20 }, 64, 1, 5000.0, edgesNone) == 0,
              "卡尺梯度门限过高 → 0");
        check(IP::caliperEdges(ramp, { 5, 20 }, { 35, 20 }, 3, 1, 20.0, edgesNone) == 0 &&
              IP::caliperEdges(cv::Mat(), { 0, 0 }, { 9, 9 }, 16, 1, 20.0, edgesNone) == 0,
              "卡尺 n<4 / 空图 → 0");
        // 反向走线（亮→暗）：极性 -1 命中同一边
        check(IP::caliperEdges(ramp, { 35, 20 }, { 5, 20 }, 64, -1, 20.0, edges) == 1 &&
              std::fabs(edges[0].x - 20.0f) < 1.5f, "卡尺反向走线极性 -1");

        // 轮廓 Hausdorff（2026-11-12 迭代 12）
        {
            const std::vector<cv::Point2f> sqA = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
            std::vector<cv::Point2f> sqB;
            for (const auto& p : sqA)
                sqB.push_back(p + cv::Point2f(0.5f, 0.0f));
            check(std::fabs(IP::contourHausdorff(sqA, sqB) - 0.5) < 1e-9,
                  "Hausdorff 平移半格 = 0.5");
            check(IP::contourHausdorff(sqA, sqA) == 0.0, "Hausdorff 自身 = 0");
            std::vector<cv::Point2f> emptyC;
            check(IP::contourHausdorff(sqA, emptyC) == -1.0, "Hausdorff 空轮廓 → -1");
        }

        // 角点候选（2026-11-11 迭代 10）：两个亮方块 → 角点落在块内
        cv::Mat feat = cv::Mat::zeros(40, 40, CV_8UC1);
        feat(cv::Rect(5, 5, 8, 8)).setTo(255);
        feat(cv::Rect(25, 25, 8, 8)).setTo(255);
        std::vector<cv::Point2f> corners;
        IP::goodFeaturesToTrack(feat, corners, 20, 0.1, 4.0);
        check(corners.size() >= 2, "goodFeaturesToTrack 两方块检出角点");
        bool inBlocks = corners.size() >= 2;
        for (const auto& cpt : corners)
            inBlocks = inBlocks &&
                       ((cpt.x >= 4 && cpt.x <= 14 && cpt.y >= 4 && cpt.y <= 14) ||
                        (cpt.x >= 24 && cpt.x <= 34 && cpt.y >= 24 && cpt.y <= 34));
        check(inBlocks, "goodFeaturesToTrack 角点位于方块内");
        std::vector<cv::Point2f> noneG;
        IP::goodFeaturesToTrack(cv::Mat(), noneG, 10, 0.1, 4.0);
        check(noneG.empty(), "goodFeaturesToTrack 空图 → 空");

        // 亚像素角点细化（2026-11-12 迭代 14；cv::cornerSubPix 在 video 头，见 .cpp 注）
        {
            std::vector<cv::Point2f> rough;
            for (const auto& cpt : corners)
                rough.push_back(cpt);
            IP::cornerSubPix(feat, rough, cv::Size(5, 5), cv::Size(-1, -1),
                             cv::TermCriteria(cv::TermCriteria::EPS | cv::TermCriteria::COUNT,
                                              30, 0.01));
            check(rough.size() == corners.size(), "cornerSubPix 数量不变");
            // 方块 (5,5)-(12,12) 与 (25,25)-(32,32) 的内角：细化点应落在内角 ±1.5px
            const cv::Point2f realCorners[8] = { { 5, 5 }, { 12, 5 }, { 12, 12 }, { 5, 12 },
                                                 { 25, 25 }, { 32, 25 }, { 32, 32 }, { 25, 32 } };
            bool nearCorner = rough.size() == corners.size();
            for (const auto& cpt : rough) {
                bool hit = false;
                for (const auto& rc : realCorners)
                    if (std::fabs(cpt.x - rc.x) <= 1.5f && std::fabs(cpt.y - rc.y) <= 1.5f)
                        hit = true;
                nearCorner = nearCorner && hit;
            }
            check(nearCorner, "cornerSubPix 精化到方块内角 ±1.5px");
            std::vector<cv::Point2f> noneS;
            IP::cornerSubPix(cv::Mat(), noneS, cv::Size(5, 5), cv::Size(-1, -1),
                             cv::TermCriteria());
            check(noneS.empty(), "cornerSubPix 空图 → 空");
        }

        // 非零点列表（2026-11-12 迭代 14）
        {
            cv::Mat nz = cv::Mat::zeros(10, 10, CV_8UC1);
            nz.at<uint8_t>(2, 3) = 255;
            nz.at<uint8_t>(7, 1) = 128;
            nz.at<uint8_t>(9, 9) = 1;
            std::vector<cv::Point> nzPts;
            IP::findNonZero(nz, nzPts);
            check(nzPts.size() == 3, "findNonZero 3 非零点");
            std::vector<cv::Point> noneN;
            IP::findNonZero(cv::Mat::zeros(5, 5, CV_8UC1), noneN);
            check(noneN.empty(), "findNonZero 全零 → 空");
        }
    }

    // 21. 基础形状拟合（2026-11-10 迭代 3：圆拟合 + 多边形逼近）
    {
        // 精确圆：圆心 (10,20) r=5
        std::vector<cv::Point2f> ring;
        for (int k = 0; k < 36; ++k) {
            const double t = k * 10.0 * 3.14159265358979323846 / 180.0;
            ring.push_back(cv::Point2f((float)(10.0 + 5.0 * std::cos(t)),
                                       (float)(20.0 + 5.0 * std::sin(t))));
        }
        cv::Point2f cc; double cr = 0, crms = 0;
        check(IP::fitCircle(ring, cc, cr, crms) &&
              std::fabs(cc.x - 10.0) < 1e-3 && std::fabs(cc.y - 20.0) < 1e-3 &&
              std::fabs(cr - 5.0) < 1e-3 && crms < 1e-4, "fitCircle 精确圆");
        // 整数轮廓重载（lround 量化噪声 rms≈0.25 → 半径容差放宽到 0.1）
        std::vector<cv::Point> ringI;
        for (const auto& p : ring)
            ringI.push_back(cv::Point((int)std::lround(p.x), (int)std::lround(p.y)));
        check(IP::fitCircle(ringI, cc, cr, crms) &&
              std::fabs(cr - 5.0) < 0.1 && std::fabs(cc.x - 10.0) < 0.1,
              "fitCircle(cv::Point) 重载");
        // 噪声圆：r=8，径向 ±0.05 扰动取 3 整周期（与圆心模式正交 → 半径无偏）
        std::vector<cv::Point2f> noisy;
        for (int k = 0; k < 24; ++k) {
            const double t = k * 15.0 * 3.14159265358979323846 / 180.0;
            const double rr = 8.0 + 0.05 * std::sin(2.0 * 3.14159265358979323846 * 3.0 * k / 24.0);
            noisy.push_back(cv::Point2f((float)(rr * std::cos(t)), (float)(rr * std::sin(t))));
        }
        check(IP::fitCircle(noisy, cc, cr, crms) &&
              std::fabs(cc.x) < 0.01 && std::fabs(cc.y) < 0.01 &&
              std::fabs(cr - 8.0) < 0.01 && crms < 0.05, "fitCircle 噪声圆收敛");
        // 部分弧段 120°（Kasa 有偏、GN 修正后半径仍准）
        std::vector<cv::Point2f> arc;
        for (int k = 0; k <= 12; ++k) {
            const double t = k * 10.0 * 3.14159265358979323846 / 180.0;
            arc.push_back(cv::Point2f((float)(10.0 * std::cos(t)), (float)(10.0 * std::sin(t))));
        }
        check(IP::fitCircle(arc, cc, cr, crms) &&
              std::fabs(cr - 10.0) < 0.1 && std::fabs(cc.x) < 0.1 && std::fabs(cc.y) < 0.1,
              "fitCircle 120° 弧段半径准");
        // 退化：共线 / 点数不足
        std::vector<cv::Point2f> line3 = { { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 3 } };
        check(!IP::fitCircle(line3, cc, cr, crms), "fitCircle 共线 → false");
        std::vector<cv::Point2f> two2 = { { 0, 0 }, { 1, 1 } };
        check(!IP::fitCircle(two2, cc, cr, crms), "fitCircle <3 点 → false");

        // 多边形逼近：带中点的矩形 → 4 角
        std::vector<cv::Point> rectPts = {
            { 0, 0 }, { 5, 0 }, { 10, 0 }, { 10, 10 }, { 5, 10 }, { 0, 10 } };
        std::vector<cv::Point> approx;
        IP::approxPolyDP(rectPts, approx, 2.0, true);
        check(approx.size() == 4, "approxPolyDP 矩形 → 4 顶点");
        bool corners = approx.size() == 4;
        if (corners)
            for (const auto& p : approx)
                corners = corners && (p == cv::Point(0, 0) || p == cv::Point(10, 0) ||
                                      p == cv::Point(10, 10) || p == cv::Point(0, 10));
        check(corners, "approxPolyDP 顶点即四角");
        std::vector<cv::Point> empty2;
        IP::approxPolyDP({}, empty2, 1.0, true);
        check(empty2.empty(), "approxPolyDP 空轮廓 → 空");

        // RANSAC 圆拟合（2026-11-11 迭代 6）：圆环 + 6 个远外点
        std::vector<cv::Point2f> ringOut;
        for (int k = 0; k < 24; ++k) {
            const double t = k * 15.0 * 3.14159265358979323846 / 180.0;
            ringOut.push_back(cv::Point2f((float)(10.0 + 5.0 * std::cos(t)),
                                          (float)(20.0 + 5.0 * std::sin(t))));
        }
        for (int k = 0; k < 6; ++k)
            ringOut.push_back(cv::Point2f(100.0f + 3.f * (float)k, -80.0f + 2.f * (float)k));
        std::vector<int> rIn;
        cv::Point2f rc; double rr = 0, rrms = 0;
        check(IP::fitCircleRobust(ringOut, 0.05, rc, rr, rrms, rIn, 42) &&
              std::fabs(rr - 5.0) < 0.05 && std::fabs(rc.x - 10.0) < 0.05 &&
              std::fabs(rc.y - 20.0) < 0.05, "fitCircleRobust 外点下半径/圆心准");
        bool noOut = true;
        for (int idx : rIn)
            if (idx >= 24) noOut = false;
        check(noOut && rIn.size() == 24, "fitCircleRobust 内点全为圆环点");
        // 种子可复现
        cv::Point2f rc2; double rr2 = 0, rrms2 = 0;
        std::vector<int> rIn2;
        IP::fitCircleRobust(ringOut, 0.05, rc2, rr2, rrms2, rIn2, 42);
        check(std::fabs(rc2.x - rc.x) < 1e-12 && std::fabs(rr2 - rr) < 1e-12 &&
              rIn2 == rIn, "fitCircleRobust 固定种子可复现");
        std::vector<int> tmpIn;
        check(!IP::fitCircleRobust(ringOut, 0.0, rc2, rr2, rrms2, tmpIn),
              "fitCircleRobust threshold<=0 → false");

        // 批量圆拟合（2026-11-11 迭代 8）：3 圆 + 共线退化 + 点数不足
        std::vector<std::vector<cv::Point2f>> batch;
        std::vector<std::pair<cv::Point2f, double>> expect;
        const double rings[3][3] = { { 10, 20, 5 }, { 0, 0, 8 }, { 50, 60, 3 } };
        for (const auto& cfg : rings) {
            std::vector<cv::Point2f> c;
            for (int k = 0; k < 24; ++k) {
                const double t = k * 15.0 * 3.14159265358979323846 / 180.0;
                c.push_back(cv::Point2f((float)(cfg[0] + cfg[2] * std::cos(t)),
                                        (float)(cfg[1] + cfg[2] * std::sin(t))));
            }
            batch.push_back(c);
            expect.push_back({ cv::Point2f((float)cfg[0], (float)cfg[1]), cfg[2] });
        }
        batch.push_back(std::vector<cv::Point2f>{ { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 3 } });
        batch.push_back(std::vector<cv::Point2f>{ { 0, 0 }, { 1, 1 } });
        std::vector<cv::Point2f> bC;
        std::vector<double> bR, bRms;
        std::vector<unsigned char> bV;
        check(IP::fitCircleBatch(batch, 0.0, bC, bR, bRms, bV) == 3, "批量 LS 3 成功");
        bool batchOk = bC.size() == 5 && bV.size() == 5;
        for (int k = 0; batchOk && k < 3; ++k)
            batchOk = bV[(size_t)k] == 1 &&
                      std::fabs(bR[(size_t)k] - expect[(size_t)k].second) < 1e-6 &&
                      std::fabs(bC[(size_t)k].x - expect[(size_t)k].first.x) < 1e-6;
        check(batchOk && bV[3] == 0 && bV[4] == 0, "批量结果对齐 + 退化项 valid=0");
        // RANSAC 模式（threshold>0）
        std::vector<unsigned char> bV2;
        check(IP::fitCircleBatch(batch, 0.05, bC, bR, bRms, bV2, 42) == 3 &&
              bV2[0] == 1 && bV2[3] == 0, "批量 RANSAC 模式");
    }

    // 22. 统计与卷积（2026-11-10 迭代 5）
    {
        cv::Mat img(20, 30, CV_8UC1, cv::Scalar(100));
        cv::Scalar mean, stddev;
        IP::meanStdDev(img, mean, stddev);
        check(std::fabs(mean[0] - 100.0) < 1e-6 && stddev[0] == 0.0, "meanStdDev 常值图");
        // 掩膜：只有左半参与
        cv::Mat m = cv::Mat::zeros(20, 30, CV_8UC1);
        m(cv::Rect(0, 0, 15, 20)).setTo(255);
        img(cv::Rect(15, 0, 15, 20)).setTo(200);
        IP::meanStdDev(img, mean, stddev, m);
        check(std::fabs(mean[0] - 100.0) < 1e-6, "meanStdDev 掩膜只算左半");
        // 20x30 全图非零（100/200 都非零）→ 600
        check(IP::countNonZero(img) == 600, "countNonZero 全非零 600");
        cv::Point mnl, mxl;
        double mnv, mxv;
        IP::minMaxLoc(img, mnv, mxv, mnl, mxl);
        check(mnv == 100.0 && mxv == 200.0 && mnl == cv::Point(0, 0) && mxl == cv::Point(15, 0),
              "minMaxLoc 值与位置");
        cv::Mat nrm;
        IP::normalize(img, nrm, 0, 255, cv::NORM_MINMAX);
        double nmn = 0, nmx = 0;
        cv::Point np1, np2;
        IP::minMaxLoc(nrm, nmn, nmx, np1, np2);
        check(nmn == 0.0 && nmx == 255.0, "normalize MINMAX [0,255]");
        // filter2D：恒等核（仅中心 1）= 原图——注意 Mat::eye 是单位矩阵（对角 3 个 1），
        // 不是卷积恒等核（实测对角 3 tap 会让常值 100 变 300）
        cv::Mat ident = cv::Mat::zeros(3, 3, CV_32F);
        ident.at<float>(1, 1) = 1.0f;
        cv::Mat filt;
        IP::filter2D(img, filt, -1, ident);
        check((int)filt.at<uint8_t>(10, 10) == 100 &&
              (int)filt.at<uint8_t>(0, 0) == 100, "filter2D 恒等核 = 原图");
        // convertScaleAbs：|100*2 − 250| = 50
        cv::Mat csa;
        IP::convertScaleAbs(cv::Mat(5, 5, CV_8UC1, cv::Scalar(100)), csa, 2.0, -250.0);
        check((int)csa.at<uint8_t>(2, 2) == 50 && csa.type() == CV_8UC1, "convertScaleAbs |2x-250|");
        cv::Mat empty;
        IP::meanStdDev(empty, mean, stddev);
        check(mean[0] == 0.0 && IP::countNonZero(empty) == 0, "统计空图安全");
    }

    // 23. 霍夫检测（2026-11-10 迭代 5，合成图）
    {
        // 直线：白底黑图上画一条水平亮线
        cv::Mat lineImg = cv::Mat::zeros(60, 80, CV_8UC1);
        cv::line(lineImg, { 5, 30 }, { 75, 30 }, cv::Scalar(255), 1);
        std::vector<cv::Vec4i> lines;
        IP::houghLinesP(lineImg, lines, 1.0, CV_PI / 180.0, 50, 20.0, 5.0);
        bool foundH = false;
        for (const auto& l : lines)
            if (std::fabs((l[1] + l[3]) * 0.5 - 30.0) <= 2.0 &&
                std::fabs((l[0] + l[2]) * 0.5 - 40.0) <= 15.0)
                foundH = true;
        check(!lines.empty() && foundH, "houghLinesP 检出水平线");
        std::vector<cv::Vec4i> noneL;
        IP::houghLinesP(cv::Mat(), noneL, 1, 1, 50);
        check(noneL.empty(), "houghLinesP 空图 → 空");
        // 圆：黑底白实心圆 r=12 @ (40,40)。HOUGH_GRADIENT 对锐利二值图可能检不出
        //（实测 0 个）——按现实用法先高斯模糊再检测
        cv::Mat circImg = cv::Mat::zeros(80, 80, CV_8UC1);
        cv::circle(circImg, { 40, 40 }, 12, cv::Scalar(255), -1);
        cv::Mat circBlur;
        cv::GaussianBlur(circImg, circBlur, cv::Size(5, 5), 1.5);
        std::vector<cv::Vec3f> circles;
        IP::houghCircles(circBlur, circles, 1.0, 30.0, 60.0, 20.0, 5, 25);
        bool foundC = false;
        for (const auto& c : circles)
            if (std::fabs(c[0] - 40.f) < 3.f && std::fabs(c[1] - 40.f) < 3.f &&
                std::fabs(c[2] - 12.f) < 2.f)
                foundC = true;
        check(!circles.empty() && foundC, "houghCircles 检出圆 (40,40,r12)");
    }
}

// ---------------- CloudProcessor ----------------
// 工单 002：本节起至文件尾的点云用例（CloudProcessor/Open3D 对照/契约/
// 拟合配准变体/多点型/边界）全部依赖 PCL——无 PCL 构建时整段不编译。
// SHAPELYALG_HAS_PCL 由 ShapelyAlg 目标在 PCL_FOUND 时 PUBLIC 定义，随
// 测试 exe 链接传入，无需单独定义。

#ifdef SHAPELYALG_HAS_PCL

static void testCloudProcessor()
{
    std::printf("== CloudProcessor ==\n");
    using CP = shapely::CloudProcessor<pcl::PointXYZ>;
    using Cloud = pcl::PointCloud<pcl::PointXYZ>;
    using CloudPtr = Cloud::Ptr;

    // 平面点云（z=0 的 10x10 网格）+ 离群点
    CloudPtr plane(new Cloud);
    for (int x = 0; x < 10; ++x)
        for (int y = 0; y < 10; ++y)
            plane->push_back(pcl::PointXYZ((float)x, (float)y, 0.0f));
    plane->push_back(pcl::PointXYZ(50.0f, 50.0f, 50.0f));   // 离群点
    plane->width = (uint32_t)plane->size();
    plane->height = 1;
    plane->is_dense = true;

    // 体素滤波（leaf 0.5 → 网格不变但离群点保留；用 1.0 验证降采样）
    CloudPtr voxel = CP::filterVoxelGrid(plane, 1.0);
    check(voxel && voxel->size() == 101, "filterVoxelGrid 1.0 → 101（网格合并+离群）");

    // 近似体素（ApproximateVoxelGrid）：降采样生效、点数不增、与精确版同量级
    CloudPtr avx = CP::filterApproxVoxelGrid(plane, 1.0);
    check(avx && avx->size() >= 80 && avx->size() <= 110, "filterApproxVoxelGrid 降采样生效");
    check(avx && avx->size() <= plane->size(), "filterApproxVoxelGrid 点数不增");

    // 空间直线拟合（fitLine3D）：X 轴向 + 噪声
    {
        CloudPtr line(new pcl::PointCloud<pcl::PointXYZ>);
        for (int i = 0; i < 100; ++i) {
            float t = (float)i;
            line->push_back(pcl::PointXYZ(t, (float)(i % 3) * 0.01f, (float)(i % 5) * 0.01f));
        }
        std::vector<float> lp, ld;
        std::vector<int> li;
        check(CP::fitLine3D(line, shapely::FitMethod::RANSAC, 0.05, lp, ld, li) &&
              li.size() >= 90, "fitLine3D RANSAC 内点 >= 90/100");
        check(ld.size() >= 3 && std::fabs(std::fabs(ld[0]) - 1.0) < 1e-3,
              "fitLine3D 方向 ≈ ±X（|dx| = 1）");
    }

    // 直通滤波：z ∈ [-0.1, 0.1] → 100 点（去离群）
    CloudPtr pt = CP::filterPassThrough(plane, "z", -0.1, 0.1);
    check(pt && pt->size() == 100, "filterPassThrough z → 100");

    // 统计离群去除
    CloudPtr sor = CP::filterStatisticalOutlier(plane, 20, 1.0);
    check(sor && sor->size() == 100, "filterStatisticalOutlier → 100（去离群）");

    // 半径离群
    CloudPtr ror = CP::filterRadiusOutlier(plane, 1.5, 3);
    check(ror && ror->size() == 100, "filterRadiusOutlier → 100");

    // 条件滤波 XYZ
    CloudPtr cond = CP::filterConditionalXYZ(plane, 0, 5, 0, 5, -1, 1);
    check(cond && cond->size() == 16, "filterConditionalXYZ (0,5) 开区间 → 16");

    // 索引提取
    CloudPtr ext = CP::extractByIndices(plane, { 0, 1, 2 });
    check(ext && ext->size() == 3, "extractByIndices 3");

    // 变换：平移
    CloudPtr moved = CP::transTranslate(plane, 1.0, 0, 0);
    check(moved && near((*moved)[0].x, 1.0, 1e-6), "transTranslate x+1");
    // 变换：矩阵（单位阵）
    const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    CloudPtr ident = CP::transApplyMatrix(plane, I);
    check(ident && near((*ident)[0].x, (*plane)[0].x, 1e-6), "transApplyMatrix 单位阵");
    // 变换：投影到 z=0 平面
    const float planeEq[4] = { 0, 0, 1, 0 };
    CloudPtr proj = CP::transProject(plane, planeEq);
    check(proj && near((*proj)[100].z, 0.0, 1e-6), "transProject 离群点投回 z=0");
    // 变换：绕 z 轴 90°
    CloudPtr rot = CP::transRotate(plane, 0, 0, 1, M_PI / 2);
    // plane[1] = (0,1,0)：绕 +z 90° → (-1,0,0)
    check(rot && near((*rot)[1].x, -1.0, 1e-5) && near((*rot)[1].y, 0.0, 1e-5),
          "transRotate 绕z 90°");

    // 变换：绕指定点 (0,0,0) 的 z 轴转 90°——pivot 本身不动
    {
        const float pivot[3] = { 0.0f, 0.0f, 0.0f };
        CloudPtr rotP = CP::transRotateAround(plane, pivot, 0, 0, 1, M_PI / 2);
        check(rotP && near((*rotP)[0].x, 0.0, 1e-5) && near((*rotP)[0].y, 0.0, 1e-5),
              "transRotateAround 中心点不动");
    }
    // 绕非原点 pivot (5,0,0)：plane[60] = (6,0,0)，相对 pivot (1,0) → z 90° → (0,1) → (5,1,0)
    // 而 pivot 自身 plane[50]=(5,0,0) 不动（上一条已验证）
    {
        const float pivot[3] = { 5.0f, 0.0f, 0.0f };
        CloudPtr rotP = CP::transRotateAround(plane, pivot, 0, 0, 1, M_PI / 2);
        check(rotP && near((*rotP)[60].x, 5.0, 1e-5) && near((*rotP)[60].y, 1.0, 1e-5),
              "transRotateAround 非原点中心");
    }

    // 变换：三轴独立缩放（mm→m 的 0.001 倍语义）
    {
        const double sc[3] = { 0.001, 0.001, 0.001 };
        CloudPtr scaled = CP::transScale(plane, sc);
        // plane[99] = (9,9,0) → (0.009, 0.009, 0)
        check(scaled && near((*scaled)[99].x, 0.009, 1e-6) && near((*scaled)[99].y, 0.009, 1e-6),
              "transScale 0.001 单位换算");
        // 镜像（负缩放）：plane[99] = (9,9,0) → (-9,9,0)
        const double mir[3] = { -1.0, 1.0, 1.0 };
        CloudPtr mirror = CP::transScale(plane, mir);
        check(mirror && near((*mirror)[99].x, -9.0, 1e-6) && near((*mirror)[99].y, 9.0, 1e-6),
              "transScale 负值镜像");
    }

    // 法线估计（平面 → 法向 z）
    auto normals = CP::featureNormalEstimation(pt, 10);
    check(normals && !normals->empty() &&
          near((*normals)[0].normal_z, 1.0, 0.1), "featureNormalEstimation 法向 z");

    // 平面拟合
    std::vector<float> coeffs;
    std::vector<int> inliers;
    check(CP::fitPlane(plane, shapely::FitMethod::RANSAC, 0.01, coeffs, inliers) &&
          inliers.size() == 100, "fitPlane 100 局内点");
    check(coeffs.size() >= 4 && near(std::fabs(coeffs[2]), 1.0, 0.01), "fitPlane 法向 z");

    // 平面分割
    CloudPtr inCloud, outCloud;
    check(CP::segmentPlaneRANSAC(plane, 0.01, inCloud, outCloud, coeffs) &&
          inCloud->size() == 100 && outCloud->size() == 1, "segmentPlaneRANSAC 100+1");

    // 欧式聚类：平面网格（间距 1，容差 1.5 → 全连通 1 簇）+ 远点单独 1 簇
    std::vector<CloudPtr> clusters = CP::segmentEuclideanCluster(plane, 1.5, 1, 10000);
    check(clusters.size() == 2 && clusters[0]->size() == 100, "segmentEuclideanCluster 2 簇");

    // KdTree KNN
    std::vector<int> knnIdx;
    std::vector<float> knnDist;
    const float q[3] = { 0.0f, 0.0f, 0.0f };
    CP::treeKdTreeKNN(plane, q, 3, knnIdx, knnDist);
    check(knnIdx.size() == 3 && knnIdx[0] >= 0 && near(knnDist[0], 0.0, 1e-6),
          "treeKdTreeKNN 最近点距离 0");

    // KdTree 半径搜索
    std::vector<int> radIdx;
    std::vector<float> radDist;
    CP::treeKdTreeRadiusSearch(plane, q, 1.5, radIdx, radDist);
    check(radIdx.size() == 4, "treeKdTreeRadiusSearch r=1.5 → 4 点（含自身+对角）");

    // Octree 体素搜索
    std::vector<int> voxIdx;
    CP::treeOctreeVoxelSearch(plane, q, voxIdx, 1.0);
    check(!voxIdx.empty(), "treeOctreeVoxelSearch 非空");

    // Octree 变化检测：新点云多一个点
    CloudPtr plane2(new Cloud(*plane));
    plane2->push_back(pcl::PointXYZ(20.0f, 20.0f, 0.0f));
    plane2->width = (uint32_t)plane2->size();
    std::vector<int> newIdx;
    CP::treeOctreeChangeDetection(plane, plane2, 1.0, newIdx);
    check(!newIdx.empty(), "treeOctreeChangeDetection 检出新增");
    // 挂死回归：resolution=0 会让 octree 内部死循环（实测卡死 >2 分钟），
    // 入口必须直接拒绝
    {
        std::vector<int> hangIdx;
        CP::treeOctreeChangeDetection(plane, plane2, 0.0, hangIdx);
        check(hangIdx.empty(), "treeOctreeChangeDetection(res=0) → 空结果不挂死");
    }

    // 凸包（平面网格 → 2D 凸包 4 角）
    CloudPtr hull = CP::surfaceConvexHull(pt);
    check(hull && hull->size() == 4, "surfaceConvexHull 平面 → 4 角点");

    // ICP：平移后的平面配回原位
    CloudPtr shifted = CP::transTranslate(pt, 0.5, 0, 0);
    float T[16] = { 0 };
    check(CP::registICP_PointToPoint(shifted, pt, 2.0, T) &&
          near(T[3], -0.5, 0.05), "registICP_PointToPoint 平移恢复");
    // PCA 配准
    check(CP::registPCA(shifted, pt, T) && near(T[3], -0.5, 0.1), "registPCA 平移恢复");

    // 多尺度 ICP（Open3D multiscale_icp）：大初值偏差下单级 ICP 失准、金字塔配回。
    // 特征地形 = 全局正弦波 + 局部高斯峰（保证 XY 平移可观测，纯平面不可观测）。
    {
        CloudPtr terrain(new Cloud);
        for (double x = -5.0; x <= 5.0; x += 0.05)
            for (double y = -5.0; y <= 5.0; y += 0.05) {
                const double z = 0.5 * std::sin(0.7 * x) * std::cos(0.6 * y)
                               + 0.6 * std::exp(-((x - 2.0) * (x - 2.0)
                                                + (y - 1.5) * (y - 1.5)) / 2.0);
                terrain->push_back(pcl::PointXYZ((float)x, (float)y, (float)z));
            }
        terrain->width = (uint32_t)terrain->size();
        terrain->height = 1;
        // source = target 经 T_true（平移 [1.5,-1,0.5] + 绕 Z 10°），初值偏差 ~2.4mm
        const float c10 = 0.98480775f, s10 = 0.17364818f;
        const float Ttrue[16] = { c10, -s10, 0, 1.5f,
                                  s10,  c10, 0, -1.0f,
                                  0,    0,   1, 0.5f,
                                  0,    0,   0, 1 };
        CloudPtr moved = CP::transApplyMatrix(terrain, Ttrue);
        // 金字塔：粗级对应距离 2.0×2.5=5.0 覆盖初值偏差，细级 0.3×2.0=0.6 精修
        float Tm[16] = { 0 };
        const bool okM = CP::registMultiscaleICP(moved, terrain,
                                                 { 2.5, 1.0, 0.3 }, { 80, 60, 60 },
                                                 2.0, Tm);
        check(okM, "registMultiscaleICP 金字塔收敛");
        double fitM = 0.0, rmseM = 0.0;
        CP::registEvaluate(moved, terrain, Tm, 0.15, fitM, rmseM);
        // 平移模长旋转不变（行主序平移在 [3]/[7]/[11]）：应恢复 sqrt(3.5) ≈ 1.871
        const double tm = std::sqrt((double)Tm[3] * Tm[3] + Tm[7] * Tm[7] + Tm[11] * Tm[11]);
        check(near(tm, 1.871, 0.15), "multiscale 平移模长恢复");
        check(fitM > 0.9 && rmseM < 0.1, "multiscale fitness/RMSE 达标");
        // 对比：单级 ICP（同末级对应距离 0.3，无初值）无法从大初值恢复——
        // 错配 RMSE 必大于金字塔精配（初值偏差 2.4mm ≫ 0.3 对应半径）
        float Ts[16] = { 0 };
        const bool okS = CP::registICP_PointToPoint(moved, terrain, 0.3, Ts);
        double fitS = 0.0, rmseS = 0.0;
        if (okS) CP::registEvaluate(moved, terrain, Ts, 0.15, fitS, rmseS);
        check(!okS || rmseS > rmseM, "单级 ICP 大初值偏差失准（对比）");
    }

    // Harris / ISS 关键点（平面无角点，仅验证不崩）
    CloudPtr harris = CP::keypointHarris3D(pt, 1.0, 1e-4);
    check(harris != nullptr, "keypointHarris3D 运行");
    CloudPtr iss = CP::keypointISS3D(pt, 1.5, 1.0);
    check(iss != nullptr, "keypointISS3D 运行");

    // CloudIO：TXT 读写回环
    const std::string txtPath = "D:/pmy/alg/output/_shapely_cloud_test.txt";
    check(shapely::CloudIO<pcl::PointXYZ>::ioSaveTXT(txtPath, pt, " "), "ioSaveTXT");
    CloudPtr loaded = shapely::CloudIO<pcl::PointXYZ>::ioLoadTXT(txtPath, " ", 0, 1, 2);
    check(loaded && loaded->size() == pt->size(), "ioLoadTXT 回环 100 点");
    // 越界回归：负列号会让 tok[(size_t)col] 越界读取
    check(shapely::CloudIO<pcl::PointXYZ>::ioLoadTXT(txtPath, " ", -1, 1, 2) == nullptr,
          "ioLoadTXT(负列号) → nullptr");
    // 回归（2026-11-09）：表头/坏行曾静默解析成 (0,0,0) 假点；"nan" 行曾入库
    {
        const std::string badPath = "D:/pmy/alg/output/_shapely_cloud_bad.txt";
        {
            std::ofstream f(badPath);
            f << "x y z\n"        // 表头（列数够但非数字）
              << "a b c\n"        // 非数字列
              << "nan 0 0\n"      // NaN 行
              << "1 2 3\n"        // 有效
              << "4.5 6.25 -1\n"; // 有效
        }
        CloudPtr bad = shapely::CloudIO<pcl::PointXYZ>::ioLoadTXT(badPath, " ", 0, 1, 2);
        check(bad && bad->size() == 2 &&
              std::fabs((*bad)[0].x - 1.0f) < 1e-6f &&
              std::fabs((*bad)[1].z + 1.0f) < 1e-6f,
              "ioLoadTXT 表头/坏行/NaN 行跳过");
        {
            std::ofstream f(badPath);
            f << "x y z\n";
        }
        check(shapely::CloudIO<pcl::PointXYZ>::ioLoadTXT(badPath, " ", 0, 1, 2) == nullptr,
              "ioLoadTXT 无有效数据行 → nullptr");
        std::remove(badPath.c_str());
    }
    // 回归（2026-11-09）：TXT 保存曾用默认 6 位精度，往返丢精度
    {
        CloudPtr prec(new pcl::PointCloud<pcl::PointXYZ>());
        prec->push_back(pcl::PointXYZ());   // 默认构造清零
        prec->back().x = 0.123456789f;
        prec->back().y = 1234.5678f;
        prec->back().z = -9.87654321f;
        const std::string precPath = "D:/pmy/alg/output/_shapely_cloud_prec.txt";
        check(shapely::CloudIO<pcl::PointXYZ>::ioSaveTXT(precPath, prec, " "), "ioSaveTXT 精度保存");
        CloudPtr back = shapely::CloudIO<pcl::PointXYZ>::ioLoadTXT(precPath, " ", 0, 1, 2);
        check(back && back->size() == 1 &&
              std::fabs((*back)[0].x - 0.123456789f) < 1e-6f &&
              std::fabs((*back)[0].y - 1234.5678f) < 1e-4f,
              "ioSaveTXT 9 位精度往返无损");
        std::remove(precPath.c_str());
    }

    // PCD 回环
    const std::string pcdPath = "D:/pmy/alg/output/_shapely_cloud_test.pcd";
    check(shapely::CloudIO<pcl::PointXYZ>::ioSavePCD(pcdPath, pt), "ioSavePCD");
    CloudPtr pcd = shapely::CloudIO<pcl::PointXYZ>::ioLoadPCD(pcdPath);
    check(pcd && pcd->size() == pt->size(), "ioLoadPCD 回环 100 点");

    // PLY 回环
    const std::string plyPath = "D:/pmy/alg/output/_shapely_cloud_test.ply";
    check(shapely::CloudIO<pcl::PointXYZ>::ioSavePLY(plyPath, pt, true), "ioSavePLY");
    CloudPtr ply = shapely::CloudIO<pcl::PointXYZ>::ioLoadPLY(plyPath);
    check(ply && ply->size() == pt->size(), "ioLoadPLY 回环 100 点");

    std::remove(txtPath.c_str());
    std::remove(pcdPath.c_str());
    std::remove(plyPath.c_str());

    // MeshIO：OBJ 网格读写（2026-11-10 迭代 2）
    {
        shapely::Mesh3d tet;
        tet.vertices = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        tet.triangles = { { 0, 2, 1 }, { 0, 1, 3 }, { 1, 2, 3 }, { 2, 0, 3 } };
        tet.valid = true;
        const std::string objPath = "D:/pmy/alg/output/_shapely_mesh_test.obj";
        check(shapely::MeshIO::saveOBJ(objPath, tet), "saveOBJ 四面体");
        const shapely::Mesh3d back = shapely::MeshIO::loadOBJ(objPath);
        check(back.valid && back.vertices.size() == 4 && back.triangles.size() == 4,
              "loadOBJ 回环 4 顶点 4 面");
        bool meshOk = back.vertices.size() == 4;
        for (size_t i = 0; meshOk && i < 4; ++i)
            meshOk = std::fabs(back.vertices[i].x - tet.vertices[i].x) < 1e-12 &&
                     std::fabs(back.vertices[i].y - tet.vertices[i].y) < 1e-12 &&
                     std::fabs(back.vertices[i].z - tet.vertices[i].z) < 1e-12 &&
                     back.triangles[i] == tet.triangles[i];
        check(meshOk, "OBJ 往返坐标/索引逐位一致");
        // 手写 OBJ：v/vt/vn、四边形扇形化、负索引、越界行丢弃、注释/多余 tag
        {
            std::ofstream f(objPath);
            f << "# comment\n"
              << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\n"
              << "f 1/1/1 2/2/2 3/3/3 4/4/4\n"   // 四边形 → 2 三角
              << "f 5//1 2//2 1//3\n"            // v//vn 写法
              << "f -1 -3 -5\n"                  // 负索引 → 5,3,1
              << "f 1 2 99\n"                    // 越界 → 整行丢弃
              << "vn 0 0 1\n";                   // 多余 tag 忽略
        }
        const shapely::Mesh3d weird = shapely::MeshIO::loadOBJ(objPath);
        check(weird.valid && weird.vertices.size() == 5 && weird.triangles.size() == 4,
              "OBJ 四边形扇形化 + v//vn + 负索引 → 4 三角");
        check(!shapely::MeshIO::saveOBJ(objPath, shapely::Mesh3d{}), "saveOBJ 无效网格 → false");
        check(!shapely::MeshIO::loadOBJ("D:/pmy/alg/output/_no_such_mesh.obj").valid,
              "loadOBJ 不存在 → invalid");
        std::remove(objPath.c_str());

        // STL 二进制/ASCII 往返（2026-11-11 迭代 6；二进制坐标 float32 有 ~1e-6 损失）
        shapely::Mesh3d tetra;
        tetra.vertices = { { 0, 0, 0 }, { 1.5, 0, 0 }, { 0, 2.5, 0 }, { 0, 0, 3.5 } };
        tetra.triangles = { { 0, 2, 1 }, { 0, 1, 3 }, { 1, 2, 3 }, { 2, 0, 3 } };
        tetra.valid = true;
        const std::string stlBin = "D:/pmy/alg/output/_shapely_mesh_test.stl";
        check(shapely::MeshIO::saveSTL(stlBin, tetra, true), "saveSTL 二进制");
        shapely::Mesh3d stlB = shapely::MeshIO::loadSTL(stlBin);
        bool stlOk = stlB.valid && stlB.triangles.size() == 4 && stlB.vertices.size() == 12;
        if (stlOk)
            for (size_t f = 0; f < 4 && stlOk; ++f)
                for (int k = 0; k < 3 && stlOk; ++k) {
                    const Point3d& a = tetra.vertices[(size_t)tetra.triangles[f][k]];
                    const Point3d& b = stlB.vertices[f * 3 + (size_t)k];
                    stlOk = std::fabs(a.x - b.x) < 1e-6 && std::fabs(a.y - b.y) < 1e-6 &&
                            std::fabs(a.z - b.z) < 1e-6;
                }
        check(stlOk, "loadSTL 二进制往返 4 面 12 顶点");
        const std::string stlAsc = "D:/pmy/alg/output/_shapely_mesh_test_ascii.stl";
        check(shapely::MeshIO::saveSTL(stlAsc, tetra, false), "saveSTL ASCII");
        shapely::Mesh3d stlA = shapely::MeshIO::loadSTL(stlAsc);
        bool ascOk = stlA.valid && stlA.triangles.size() == 4;
        if (ascOk)
            for (size_t f = 0; f < 4 && ascOk; ++f)
                for (int k = 0; k < 3 && ascOk; ++k) {
                    const Point3d& a = tetra.vertices[(size_t)tetra.triangles[f][k]];
                    const Point3d& b = stlA.vertices[f * 3 + (size_t)k];
                    ascOk = std::fabs(a.x - b.x) < 1e-12 && std::fabs(a.y - b.y) < 1e-12 &&
                            std::fabs(a.z - b.z) < 1e-12;
                }
        check(ascOk, "loadSTL ASCII 全精度往返");
        check(!shapely::MeshIO::saveSTL(stlBin, shapely::Mesh3d{}, true),
              "saveSTL 无效网格 → false");
        check(!shapely::MeshIO::loadSTL("D:/pmy/alg/output/_no_such.stl").valid,
              "loadSTL 不存在 → invalid");
        std::remove(stlBin.c_str());
        std::remove(stlAsc.c_str());

        // HeightMapIO 序列化往返（2026-11-11 迭代 7）
        {
            shapely::HeightMap hmIo;
            hmIo.width = 4;
            hmIo.height = 3;
            hmIo.x0 = 10.5;
            hmIo.y0 = -2.25;
            hmIo.cell = 0.5;
            hmIo.cells.assign(12, std::numeric_limits<float>::quiet_NaN());
            hmIo.mask.assign(12, 0);
            hmIo.cells[1] = 5.5f;  hmIo.mask[1] = 1;
            hmIo.cells[6] = -3.0f; hmIo.mask[6] = 1;
            hmIo.cells[11] = 0.0f; hmIo.mask[11] = 1;
            hmIo.valid = true;
            const std::string hPath = "D:/pmy/alg/output/_shapely_hmap_test.bin";
            check(shapely::HeightMapIO::save(hPath, hmIo), "HeightMapIO save");
            const shapely::HeightMap hb = shapely::HeightMapIO::load(hPath);
            check(hb.valid && hb.width == 4 && hb.height == 3 &&
                  std::fabs(hb.x0 - 10.5) < 1e-12 && std::fabs(hb.cell - 0.5) < 1e-12,
                  "HeightMapIO load 元数据一致");
            check(std::memcmp(hb.cells.data(), hmIo.cells.data(), 12 * sizeof(float)) == 0 &&
                  std::memcmp(hb.mask.data(), hmIo.mask.data(), 12) == 0,
                  "HeightMapIO cells/mask 逐字节一致（含 NaN）");
            shapely::HeightMap badIo = hmIo;
            badIo.valid = false;
            check(!shapely::HeightMapIO::save(hPath, badIo), "HeightMapIO save invalid → false");
            check(!shapely::HeightMapIO::load("D:/pmy/alg/output/_no_such.hmap").valid,
                  "HeightMapIO load 不存在 → invalid");
            {
                std::ofstream g(hPath, std::ios::binary);
                g << "GARBAGE-DATA";
            }
            check(!shapely::HeightMapIO::load(hPath).valid, "HeightMapIO load 魔数不符 → invalid");
            std::remove(hPath.c_str());
        }
    }
}

// ---------------- CloudProcessor 扩展算子（对齐 Open3D 点云 API） ----------------

static void testCloudProcessorOpen3D()
{
    std::printf("== CloudProcessor (Open3D-aligned) ==\n");
    using CP = shapely::CloudProcessor<pcl::PointXYZ>;
    using Cloud = pcl::PointCloud<pcl::PointXYZ>;
    using CloudPtr = Cloud::Ptr;

    // 10x10 平面网格（间距 1，z=0），两个版本：纯净 / 带远点
    CloudPtr grid(new Cloud);
    for (int x = 0; x < 10; ++x)
        for (int y = 0; y < 10; ++y)
            grid->push_back(pcl::PointXYZ((float)x, (float)y, 0.0f));
    grid->width = (uint32_t)grid->size();
    grid->height = 1;
    grid->is_dense = true;

    CloudPtr noisy(new Cloud(*grid));
    noisy->push_back(pcl::PointXYZ(50.0f, 50.0f, 50.0f));
    noisy->width = (uint32_t)noisy->size();
    noisy->height = 1;

    // --- filter：降采样与清洗 ---
    {
        std::vector<std::vector<int>> trace;
        CloudPtr down = CP::filterVoxelGridAndTrace(noisy, 2.0, trace);
        size_t traced = 0;
        bool index_ok = true;
        bool voxel_ok = true;
        for (size_t gi = 0; gi < trace.size(); ++gi) {
            traced += trace[gi].size();
            for (int idx : trace[gi]) {
                if (idx < 0 || idx >= (int)noisy->size())
                    index_ok = false;
                else {
                    // 同组点必须落在同一体素：与输出点（体素质心）的体素坐标一致
                    const pcl::PointXYZ& p = (*noisy)[(size_t)idx];
                    const pcl::PointXYZ& c = (*down)[gi];
                    if (std::floor(p.x / 2.0) != std::floor(c.x / 2.0) ||
                        std::floor(p.y / 2.0) != std::floor(c.y / 2.0) ||
                        std::floor(p.z / 2.0) != std::floor(c.z / 2.0))
                        voxel_ok = false;
                }
            }
        }
        check(down && !trace.empty() && trace.size() == down->size(), "filterVoxelGridAndTrace 输出/回溯等长");
        check(index_ok && traced == noisy->size(), "filterVoxelGridAndTrace 索引全覆盖");
        check(voxel_ok, "filterVoxelGridAndTrace 同组点同体素");
    }
    {
        CloudPtr a = CP::filterRandomSample(grid, 40, 1234);
        CloudPtr b = CP::filterRandomSample(grid, 40, 1234);
        check(a && a->size() == 40 && b && b->size() == 40 &&
              near((*a)[0].x, (*b)[0].x) && near((*a)[0].y, (*b)[0].y),
              "filterRandomSample 固定种子可复现");
    }
    {
        CloudPtr uni = CP::filterUniformSample(grid, 10);
        check(uni && uni->size() == 10 && near((*uni)[1].x, 1.0), "filterUniformSample 每 10 点取 1");
    }
    {
        CloudPtr fps = CP::filterFarthestPointSample(grid, 4, 0);
        check(fps && fps->size() == 4 && near((*fps)[0].x, 0.0) && near((*fps)[0].y, 0.0),
              "filterFarthestPointSample 4 点且首点为起点");
    }
    {
        CloudPtr bad(new Cloud(*grid));
        bad->push_back(pcl::PointXYZ(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f));
        bad->push_back(pcl::PointXYZ(0.0f, std::numeric_limits<float>::infinity(), 0.0f));
        bad->width = (uint32_t)bad->size();
        CloudPtr clean = CP::filterRemoveNonFinite(bad);
        check(clean && clean->size() == 100 && clean->is_dense, "filterRemoveNonFinite 去 NaN/Inf");
    }
    {
        CloudPtr dup(new Cloud(*grid));
        dup->push_back(pcl::PointXYZ(0.0f, 0.0f, 0.0f));      // 完全重复
        dup->push_back(pcl::PointXYZ(3.05f, 3.0f, 0.0f));     // 容差内近似重复
        dup->width = (uint32_t)dup->size();
        CloudPtr exact = CP::filterRemoveDuplicates(dup, 0.0);
        CloudPtr fuzzy = CP::filterRemoveDuplicates(dup, 0.1);
        check(exact && exact->size() == 101, "filterRemoveDuplicates 精确去重");
        check(fuzzy && fuzzy->size() == 100, "filterRemoveDuplicates 容差去重");
    }

    // --- extract：反向索引 / 盒裁剪 ---
    {
        CloudPtr rest = CP::extractByIndicesInverted(grid, { 0, 1, 2 });
        check(rest && rest->size() == 97, "extractByIndicesInverted 剔除 3 点");
    }
    {
        const float lo[3] = { 0.0f, 0.0f, -1.0f };
        const float hi[3] = { 5.0f, 5.0f, 1.0f };
        CloudPtr crop = CP::extractCropBox(grid, lo, hi);
        check(crop && crop->size() == 36, "extractCropBox [0,5] 见方 → 36 点（闭区间）");
    }
    {
        const shapely::OBB3d box = CP::measureOBB(grid);
        CloudPtr inBox = CP::extractCropOBB(grid, box, false);
        CloudPtr outBox = CP::extractCropOBB(grid, box, true);
        check(box.valid && inBox && inBox->size() == 100 && outBox && outBox->empty(),
              "extractCropOBB 全部点落在自身 OBB 内");
    }
    {
        // 2D 多边形 ROI（2026-11-10 迭代 2）：方框 ROI 无边界点 → 精确计数
        const std::vector<Point2d> roi = {
            { 2.5, 2.5 }, { 6.5, 2.5 }, { 6.5, 6.5 }, { 2.5, 6.5 } };
        CloudPtr inRoi = CP::extractByPolygonROI(grid, roi, false);
        CloudPtr outRoi = CP::extractByPolygonROI(grid, roi, true);
        check(inRoi && inRoi->size() == 16 && outRoi && outRoi->size() == 84,
              "extractByPolygonROI [2.5,6.5]² → 16/84");
        check(CP::extractByPolygonROI(grid, { { 0, 0 }, { 1, 1 } }) == nullptr &&
              CP::extractByPolygonROI(nullptr, roi) == nullptr,
              "extractByPolygonROI 退化入参 → nullptr");
        // 三角 ROI：腿 x=0.5/y=0.5、斜边 x+y=10（边界半开），断言避开边界带
        // 严格内部：i,j≥1 且 i+j≤9 → 36 点；明确外部：腿外或 i+j≥11
        const std::vector<Point2d> tri = {
            { 0.5, 0.5 }, { 9.5, 0.5 }, { 0.5, 9.5 } };
        CloudPtr triIn = CP::extractByPolygonROI(grid, tri, false);
        int strictIn = 0, farOut = 0;
        if (triIn)
            for (const auto& p : triIn->points) {
                if (p.x > 0.5f && p.y > 0.5f && p.x + p.y < 10.0f) ++strictIn;
                if (p.x < 0.5f || p.y < 0.5f || p.x + p.y >= 11.0f) ++farOut;
            }
        check(triIn && strictIn == 36 && farOut == 0,
              "extractByPolygonROI 三角形：内部 36 全含、外部 0 含");
    }

    // --- heightmap：3D → 2D 桥接（2026-11-10 迭代 2） ---
    {
        CloudPtr hcloud(new pcl::PointCloud<pcl::PointXYZ>());
        auto addPt = [&hcloud](float x, float y, float z) {
            pcl::PointXYZ p;
            p.x = x; p.y = y; p.z = z;
            hcloud->push_back(p);
        };
        addPt(0, 0, 1); addPt(0, 0, 5);   // 同格叠点 → max 5 / min 1 / mean 3
        addPt(2, 0, 7); addPt(0, 2, -3); addPt(2, 2, 0);
        hcloud->width = (uint32_t)hcloud->size();
        hcloud->height = 1;

        const shapely::HeightMap hm = CP::cloudToHeightMap(hcloud, 1.0, 0);
        check(hm.valid && hm.width == 3 && hm.height == 3 &&
              (int)hm.cells.size() == 9 && (int)hm.mask.size() == 9,
              "heightmap 2x2 点 → 3x3 格");
        check(hm.mask[0] == 1 && std::fabs(hm.at(0, 0) - 5.0f) < 1e-6,
              "heightmap max 同格取最高 5");
        check(hm.at(0, 2) == 7.0f && hm.at(2, 0) == -3.0f,
              "heightmap row=y/col=x 映射");
        check(std::isnan(hm.at(1, 1)) && hm.mask[(size_t)1 * 3 + 1] == 0,
              "heightmap 空格 NaN + mask 0");
        check(std::isnan(hm.at(-1, 0)) && std::isnan(hm.at(3, 0)),
              "heightmap 越界 → NaN");
        const shapely::HeightMap hmin = CP::cloudToHeightMap(hcloud, 1.0, 1);
        check(std::fabs(hmin.at(0, 0) - 1.0f) < 1e-6, "heightmap min 取最低 1");
        const shapely::HeightMap hmean = CP::cloudToHeightMap(hcloud, 1.0, 2);
        check(std::fabs(hmean.at(0, 0) - 3.0f) < 1e-6, "heightmap mean 取平均 3");
        check(CP::cloudToHeightMap(hcloud, 0.5, 0).width == 5, "heightmap 半格 → 5x5");
        check(!CP::cloudToHeightMap(hcloud, 0.0, 0).valid &&
              !CP::cloudToHeightMap(hcloud, 1.0, 7).valid &&
              !CP::cloudToHeightMap(nullptr, 1.0).valid,
              "heightmap cell<=0/坏 mode/空 → invalid");
        check(!CP::cloudToHeightMap(hcloud, 1e-6, 0).valid, "heightmap 超 2000 万格 → invalid");
        // mode=3 点计数密度（2026-11-11 迭代 11）
        {
            CloudPtr dens(new pcl::PointCloud<pcl::PointXYZ>());
            const auto addD = [&dens](float x, float y) {
                pcl::PointXYZ q;
                q.x = x; q.y = y; q.z = 0.f;
                dens->push_back(q);
            };
            addD(0, 0); addD(0.2f, 0.1f);   // 同格 2 点
            addD(2, 0);                      // 另格 1 点
            const shapely::HeightMap hd = CP::cloudToHeightMap(dens, 1.0, 3);
            check(hd.valid && hd.cells[0] == 2.0f && hd.cells[2] == 1.0f &&
                  hd.mask[0] == 1 && hd.mask[1] == 0 && std::isnan(hd.cells[1]),
                  "heightmap mode=3 点计数密度");
            check(!CP::cloudToHeightMap(dens, 1.0, 4).valid, "heightmap mode=4 → invalid");
        }
        addPt(0, 0, std::numeric_limits<float>::quiet_NaN());
        hcloud->width = (uint32_t)hcloud->size();
        hcloud->height = 1;
        const shapely::HeightMap hnan = CP::cloudToHeightMap(hcloud, 1.0, 0);
        check(hnan.valid && std::fabs(hnan.at(0, 0) - 5.0f) < 1e-6, "heightmap NaN z 点剔除");
    }

    // --- 网格顶点聚类抽稀（2026-11-10 迭代 5） ---
    {
        // 手建 5x5 网格面（间距 1，共 25 顶点 32 面）
        shapely::Mesh3d grid;
        for (int j = 0; j < 5; ++j)
            for (int i = 0; i < 5; ++i)
                grid.vertices.push_back(Point3d{ (double)i, (double)j, 0.0 });
        auto vid = [](int i, int j) { return j * 5 + i; };
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) {
                grid.triangles.push_back({ vid(i, j), vid(i + 1, j), vid(i + 1, j + 1) });
                grid.triangles.push_back({ vid(i, j), vid(i + 1, j + 1), vid(i, j + 1) });
            }
        grid.valid = true;
        const shapely::Mesh3d dec = CP::surfaceMeshClusterDecimate(grid, 2.0);
        check(dec.valid && dec.vertices.size() == 9,           // 0..4 → 3 簇/轴 → 3x3
              "抽稀 5x5 网格 cell=2 → 9 质心");
        check(dec.triangles.size() == 8,                       // 2x2 面 × 2 三角
              "抽稀面数 8（无退化/无重复）");
        bool noDegenerate = true;
        for (const auto& t : dec.triangles)
            if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2])
                noDegenerate = false;
        check(noDegenerate, "抽稀无退化三角");
        check(near(dec.vertices[4].x, 2.5, 1e-12) &&
              near(dec.vertices[4].y, 2.5, 1e-12),            // 簇4 = x,y∈{2,3} 质心 (2.5,2.5)
              "抽稀簇质心坐标");
        // 细 cell 不减顶点
        const shapely::Mesh3d keep = CP::surfaceMeshClusterDecimate(grid, 0.5);
        check(keep.valid && keep.vertices.size() == 25 && keep.triangles.size() == 32,
              "抽稀 cell=0.5 → 原样 25 顶点 32 面");
        check(!CP::surfaceMeshClusterDecimate(grid, 0.0).valid &&
              !CP::surfaceMeshClusterDecimate(shapely::Mesh3d{}, 1.0).valid,
              "抽稀 cell<=0/空网格 → invalid");
    }

    // --- 3D 空间圆拟合 / 平面度（2026-11-11 迭代 6） ---
    {
        // 斜面空间圆：法向 normalize(1,2,3)，圆心 (10,20,30)，r=8（手算正交基，不引 Eigen）
        const double nx0 = 1.0, ny0 = 2.0, nz0 = 3.0;
        const double nl = std::sqrt(nx0 * nx0 + ny0 * ny0 + nz0 * nz0);
        const double nA[3] = { nx0 / nl, ny0 / nl, nz0 / nl };
        const double aux[3] = { 1.0, 0.0, 0.0 };
        // u = normalize(n × aux)，v = n × u
        double uA[3] = { nA[1] * aux[2] - nA[2] * aux[1],
                         nA[2] * aux[0] - nA[0] * aux[2],
                         nA[0] * aux[1] - nA[1] * aux[0] };
        const double ul = std::sqrt(uA[0] * uA[0] + uA[1] * uA[1] + uA[2] * uA[2]);
        for (double& x : uA) x /= ul;
        const double vA[3] = { nA[1] * uA[2] - nA[2] * uA[1],
                               nA[2] * uA[0] - nA[0] * uA[2],
                               nA[0] * uA[1] - nA[1] * uA[0] };
        CloudPtr circ(new pcl::PointCloud<pcl::PointXYZ>());
        for (int k = 0; k < 36; ++k) {
            const double t = k * 10.0 * 3.14159265358979323846 / 180.0;
            const double cs = 8.0 * std::cos(t), sn = 8.0 * std::sin(t);
            pcl::PointXYZ q;
            q.x = (float)(10.0 + cs * uA[0] + sn * vA[0]);
            q.y = (float)(20.0 + cs * uA[1] + sn * vA[1]);
            q.z = (float)(30.0 + cs * uA[2] + sn * vA[2]);
            circ->push_back(q);
        }
        Point3d c3;
        float n3[3] = { 0, 0, 0 };
        double r3 = 0, rms3 = 0;
        std::vector<int> in3;
        check(CP::fitCircle3D(circ, shapely::FitMethod::RANSAC, 0.05, c3, n3, r3, in3, rms3) &&
              std::fabs(r3 - 8.0) < 0.05 &&
              std::fabs(c3.x - 10.0) < 0.1 && std::fabs(c3.y - 20.0) < 0.1 &&
              std::fabs(c3.z - 30.0) < 0.1, "fitCircle3D 斜面圆心/半径");
        const double ndot = std::fabs((double)n3[0] * nA[0] + (double)n3[1] * nA[1] +
                                      (double)n3[2] * nA[2]);
        check(std::fabs(ndot - 1.0) < 1e-3 && in3.size() == 36 && rms3 < 0.01,
              "fitCircle3D 法向平行 + 36 内点");
        check(!CP::fitCircle3D(CloudPtr(new pcl::PointCloud<pcl::PointXYZ>()),
                               shapely::FitMethod::RANSAC, 0.05, c3, n3, r3, in3, rms3),
              "fitCircle3D 空云 → false");

        // 圆锥拟合（2026-11-11 迭代 8）：顶点原点、轴 +Z、半开角 30°
        //（PCL 圆锥模型轴 = 三点法线叉积导出，法线质量决定成败——云必须足够稠密）
        CloudPtr cone(new pcl::PointCloud<pcl::PointXYZ>());
        {
            const double tanHalf = std::tan(30.0 * 3.14159265358979323846 / 180.0);
            for (int ring = 0; ring < 38; ++ring) {
                const double h = 5.0 + 0.4 * (double)ring;            // 高度 5~19.8
                const double r = h * tanHalf;
                for (int k = 0; k < 16; ++k) {
                    const double t = k * 22.5 * 3.14159265358979323846 / 180.0;
                    pcl::PointXYZ q;
                    q.x = (float)(r * std::cos(t));
                    q.y = (float)(r * std::sin(t));
                    q.z = (float)h;
                    cone->push_back(q);
                }
            }
        }
        std::vector<float> apex, axisC;
        double angC = 0;
        std::vector<int> inC;
        // 法线角度误差会被切平面交点放大成顶点漂移——阈值需覆盖（实测 0.05 不收敛）
        check(CP::fitCone(cone, shapely::FitMethod::RANSAC, 0.3, apex, axisC, angC, inC),
              "fitCone 收敛");
        std::printf("[probe] cone apex=(%.2f,%.2f,%.2f) axis=(%.3f,%.3f,%.3f) angDeg=%.2f in=%d\n",
                    apex.size() ? apex[0] : -1.f, apex.size() ? apex[1] : -1.f,
                    apex.size() ? apex[2] : -1.f, axisC.size() ? axisC[0] : -1.f,
                    axisC.size() ? axisC[1] : -1.f, axisC.size() ? axisC[2] : -1.f,
                    angC * 180.0 / 3.14159265358979323846, (int)inC.size());
        check(apex.size() == 3 && std::fabs(apex[0]) < 0.05 && std::fabs(apex[1]) < 0.05 &&
              std::fabs(apex[2]) < 0.05, "fitCone 顶点归位原点");
        const double axisZ = axisC.size() == 3 ? std::fabs(axisC[2]) : 0.0;
        const double angDeg = angC * 180.0 / 3.14159265358979323846;
        check(std::fabs(axisZ - 1.0) < 1e-3 && std::fabs(angDeg - 30.0) < 0.5 &&
              inC.size() == 608, "fitCone 轴向/半开角 30°/全内点");
        check(!CP::fitCone(CloudPtr(new pcl::PointCloud<pcl::PointXYZ>()),
                           shapely::FitMethod::RANSAC, 0.05, apex, axisC, angC, inC),
              "fitCone 空云 → false");

        // 圆度 / 圆柱度（2026-11-12 迭代 12）
        {
            CloudPtr circ2(new pcl::PointCloud<pcl::PointXYZ>());
            for (int k = 0; k < 36; ++k) {
                const double t = k * 10.0 * 3.14159265358979323846 / 180.0;
                pcl::PointXYZ q;
                q.x = (float)(10.0 + 8.0 * std::cos(t));
                q.y = (float)(20.0 + 8.0 * std::sin(t));
                q.z = 3.f;
                circ2->push_back(q);
            }
            double rnd = 0, cr3 = 0, rms3 = 0;
            Point3d cc3;
            float cn3[3] = { 0 };
            std::vector<int> rin3;
            check(CP::measureCircleRoundness(circ2, shapely::FitMethod::RANSAC, 0.05, rnd, cc3,
                                             cn3, cr3) &&
                  rnd < 1e-3 && std::fabs(cr3 - 8.0) < 0.05, "圆度：精确圆 ≈ 0");

            CloudPtr cyl2(new pcl::PointCloud<pcl::PointXYZ>());
            for (int layer = 0; layer < 5; ++layer)
                for (int k = 0; k < 16; ++k) {
                    const double t = k * 22.5 * 3.14159265358979323846 / 180.0;
                    pcl::PointXYZ q;
                    q.x = (float)(5.0 * std::cos(t));
                    q.y = (float)(5.0 * std::sin(t));
                    q.z = (float)(layer * 2.5);
                    cyl2->push_back(q);
                }
            double form = 0, cylR = 0;
            std::vector<float> cylAxis, cylPt;
            check(CP::measureCylinderForm(cyl2, shapely::FitMethod::RANSAC, 0.05, form, cylAxis,
                                          cylPt, cylR) &&
                  form < 1e-3 && std::fabs(cylR - 5.0) < 0.05, "圆柱度：精确圆柱 ≈ 0");
        }

        // 位置度 / 平面残差（2026-11-12 迭代 12）
        {
            std::vector<Point2d> theo = { { 0, 0 }, { 10, 0 }, { 20, 0 } };
            std::vector<Point2d> meas = { { 0.1, 0.05 }, { 10.2, 0 }, { 30, 30 } };
            std::vector<shapely::PositionDeviation> pairs;
            const int nMatch =
                CP::measurePositionDeviation(theo, meas, 1.0, pairs);
            check(nMatch == 2 && pairs.size() == 3, "位置度 3 理论位配对 2");
            check(pairs[0].measured == 0 && std::fabs(pairs[0].dx - 0.1) < 1e-12 &&
                  std::fabs(pairs[0].dy - 0.05) < 1e-12 &&
                  std::fabs(pairs[0].dist - std::sqrt(0.1 * 0.1 + 0.05 * 0.05)) < 1e-12,
                  "位置度 t0 偏差逐项");
            check(pairs[1].measured == 1 && std::fabs(pairs[1].dx - 0.2) < 1e-12 &&
                  pairs[2].measured == -1 && pairs[2].dist == 0.0,
                  "位置度 t1 配对 / t2 未配到");
            std::vector<shapely::PositionDeviation> pairs2;
            check(CP::measurePositionDeviation(theo, meas, 50.0, pairs2) == 3,
                  "位置度宽半径全配对");
            check(CP::measurePositionDeviation(theo, meas, -1.0, pairs2) == -1,
                  "位置度 match_radius<0 → -1");
            // 平面残差：z=0 面 + 非归一化系数
            CloudPtr rp(new pcl::PointCloud<pcl::PointXYZ>());
            auto addR = [&rp](float x, float y, float z) {
                pcl::PointXYZ q;
                q.x = x; q.y = y; q.z = z;
                rp->push_back(q);
            };
            addR(0, 0, 1); addR(1, 1, -2);
            addR(2, 2, std::numeric_limits<float>::quiet_NaN());
            std::vector<float> res;
            const float pZ0[4] = { 0, 0, 1, 0 };
            check(CP::planeResiduals(rp, pZ0, res) && res.size() == 3 &&
                  std::fabs(res[0] - 1.0f) < 1e-9 && std::fabs(res[1] + 2.0f) < 1e-9 &&
                  std::isnan(res[2]), "planeResiduals 有符号残差 + NaN 点");
            const float pNZ[4] = { 0, 0, 2, -4 };
            check(CP::planeResiduals(rp, pNZ, res) &&
                  std::fabs(res[0] + 1.0f) < 1e-9, "planeResiduals 除以 |n|（z=1 在 z=2 面上 → -1）");
            const float zeroP[4] = { 0, 0, 0, 0 };
            check(!CP::planeResiduals(rp, zeroP, res), "planeResiduals 零法向 → false");
        }

        // 多平面迭代分割（2026-11-11 迭代 10）：z=0 平面 100 点 + z=3 上半面 60 点
        CloudPtr twoPlanes(new pcl::PointCloud<pcl::PointXYZ>());
        for (int i = 0; i < 100; ++i) {
            pcl::PointXYZ q;
            q.x = (float)(i % 10); q.y = (float)(i / 10); q.z = 0.f;
            twoPlanes->push_back(q);
        }
        for (int i = 0; i < 60; ++i) {
            pcl::PointXYZ q;
            q.x = (float)(i % 10); q.y = (float)(i / 10); q.z = 3.f;
            twoPlanes->push_back(q);
        }
        std::vector<std::vector<float>> mCoeffs;
        std::vector<std::vector<int>> mIn;
        const int nPlanes =
            CP::segmentPlanesMulti(twoPlanes, 0.02, 5, 30, mCoeffs, mIn);
        check(nPlanes == 2 && mCoeffs.size() == 2 && mIn.size() == 2,
              "segmentPlanesMulti 两平面全检出");
        bool planesOk = nPlanes == 2;
        for (int k = 0; planesOk && k < 2; ++k)
            planesOk = (int)mIn[k].size() >= 30 &&
                       std::fabs(mCoeffs[k][2]) > 0.9;   // 两面法向均近 z 轴
        // 全局索引回映射：两平面内点并集 = 160，且互不重叠
        std::vector<int> unionIdx;
        if (planesOk) {
            unionIdx = mIn[0];
            unionIdx.insert(unionIdx.end(), mIn[1].begin(), mIn[1].end());
            std::sort(unionIdx.begin(), unionIdx.end());
            planesOk = unionIdx.size() == 160 &&
                       std::adjacent_find(unionIdx.begin(), unionIdx.end()) == unionIdx.end();
        }
        check(planesOk, "segmentPlanesMulti 全局索引回映射无重叠");
        std::vector<std::vector<float>> noneC;
        std::vector<std::vector<int>> noneI;
        check(CP::segmentPlanesMulti(twoPlanes, 0.02, 1, 200, noneC, noneI) == 0,
              "segmentPlanesMulti min_inliers 过高 → 0");
        // 高度图 → 灰度图（2026-11-11 迭代 10）
        shapely::HeightMap hmImg;
        hmImg.width = 3;
        hmImg.height = 2;
        hmImg.x0 = 0;
        hmImg.y0 = 0;
        hmImg.cell = 1.0;
        hmImg.cells.assign(6, std::numeric_limits<float>::quiet_NaN());
        hmImg.mask.assign(6, 0);
        hmImg.cells[0] = 1.0f; hmImg.mask[0] = 1;
        hmImg.cells[2] = 5.0f; hmImg.mask[2] = 1;
        hmImg.cells[3] = 3.0f; hmImg.mask[3] = 1;
        hmImg.valid = true;
        const cv::Mat img8 = shapely::HeightMapAdapter::toImage(hmImg);
        check(img8.type() == CV_8U && img8.cols == 3 && img8.rows == 2 &&
              img8.at<uint8_t>(0, 0) == 0 && img8.at<uint8_t>(0, 2) == 255 &&
              img8.at<uint8_t>(1, 0) == 128 && img8.at<uint8_t>(0, 1) == 0,
              "toImage 8U min-max 归一 + NaN→0");
        const cv::Mat img16 = shapely::HeightMapAdapter::toImage(hmImg, true);
        check(img16.type() == CV_16U && img16.at<uint16_t>(0, 2) == 5 &&
              img16.at<uint16_t>(1, 0) == 3 && img16.at<uint16_t>(0, 1) == 0,
              "toImage 16U 原值保留");
        check(shapely::HeightMapAdapter::toImage(shapely::HeightMap{}).empty(),
              "toImage invalid → 空 Mat");

        // 平面度：z=2 平面 + ±0.004 正弦噪声（远外点被 RANSAC 剔除）
        CloudPtr flat(new pcl::PointCloud<pcl::PointXYZ>());
        for (int j = 0; j < 15; ++j)
            for (int i = 0; i < 15; ++i) {
                pcl::PointXYZ q;
                q.x = (float)i; q.y = (float)j;
                q.z = 2.0f + 0.004f * std::sin(3.0f * i) * std::cos(2.0f * j);
                flat->push_back(q);
            }
        for (int k = 0; k < 5; ++k) {
            pcl::PointXYZ q;
            q.x = 50.f + (float)k; q.y = 0.f; q.z = 10.f;
            flat->push_back(q);
        }
        double ft = 0;
        std::vector<float> fcoeffs;
        check(CP::measureFlatness(flat, 0.02, ft, fcoeffs) && ft < 0.02 && ft > 1e-6,
              "measureFlatness 噪声平面峰峰值 < 阈值");
        check(std::fabs(fcoeffs[2]) > 0.9, "measureFlatness 法向近 z 轴");
        check(!CP::measureFlatness(CloudPtr(new pcl::PointCloud<pcl::PointXYZ>()), 0.02, ft,
                                   fcoeffs), "measureFlatness 空云 → false");

        // 两平面关系（2026-11-11 迭代 11）
        {
            const float pFlat[4] = { 0, 0, 1, 0 };                       // z = 0
            const float pTilt[4] = { 0, -0.5f, 0.8660254f, 0 };          // 法向绕 x 转 30°
            check(std::fabs(CP::measurePlaneAngle(pFlat, pTilt) - 30.0) < 1e-6,
                  "两平面夹角 30°");
            check(std::fabs(CP::measurePlaneAngle(pFlat, pFlat)) < 1e-9, "同面夹角 0");
            const float pVert[4] = { 1, 0, 0, 0 };
            check(std::fabs(CP::measurePlaneAngle(pFlat, pVert) - 90.0) < 1e-9,
                  "垂直面夹角 90°");
            const float zeroN[4] = { 0, 0, 0, 0 };
            check(CP::measurePlaneAngle(pFlat, zeroN) == -1.0, "零法向 → -1");
            // 交线：z=0 ∩ x=0 → dir = ±y，最近原点点 = (0,0,0)
            float ip[3] = { 0 }, idr[3] = { 0 };
            const float pX0[4] = { 1, 0, 0, 0 };
            check(CP::measurePlaneIntersection(pFlat, pX0, ip, idr) &&
                  std::fabs(ip[0]) < 1e-9 && std::fabs(ip[1]) < 1e-9 &&
                  std::fabs(ip[2]) < 1e-9 &&
                  std::fabs(std::fabs((double)idr[1]) - 1.0) < 1e-9,
                  "交线 z=0∩x=0 → y 轴过原点");
            const float pX3[4] = { 1, 0, 0, -3 };
            check(CP::measurePlaneIntersection(pFlat, pX3, ip, idr) &&
                  std::fabs(ip[0] - 3.f) < 1e-6 && std::fabs(ip[1]) < 1e-6,
                  "交线 z=0∩x=3 → 点 (3,0,0)");
            const float pUp[4] = { 0, 0, 1, -1 };
            check(!CP::measurePlaneIntersection(pFlat, pUp, ip, idr),
                  "平行面无交线 → false");
        }
    }

    // --- 高度图反向点云化（2026-11-11 迭代 6，自建小云） ---
    {
        CloudPtr hc(new pcl::PointCloud<pcl::PointXYZ>());
        auto addH = [&hc](float x, float y, float z) {
            pcl::PointXYZ p;
            p.x = x; p.y = y; p.z = z;
            hc->push_back(p);
        };
        addH(0, 0, 1); addH(2, 0, 7); addH(0, 2, -3); addH(2, 2, 0);
        hc->width = (uint32_t)hc->size();
        hc->height = 1;
        const shapely::HeightMap hm2c = CP::cloudToHeightMap(hc, 1.0, 0);
        CloudPtr back = CP::heightMapToCloud(hm2c);
        int maskSum = 0;
        for (unsigned char mv : hm2c.mask)
            maskSum += mv;
        check(back && (int)back->size() == maskSum, "heightMapToCloud 点数 = 有效格数");
        // 往返：反向点再栅格化 → 掩膜/值一致（格心坐标精确落回原格）
        const shapely::HeightMap hmBack = CP::cloudToHeightMap(back, 1.0, 0);
        bool same = hmBack.valid && hmBack.width == hm2c.width && hmBack.height == hm2c.height;
        for (size_t i = 0; same && i < hm2c.cells.size(); ++i) {
            const bool a = hm2c.mask[i] != 0, b = hmBack.mask[i] != 0;
            if (a != b)
                same = false;
            else if (a && std::fabs(hmBack.cells[i] - hm2c.cells[i]) > 1e-6f)
                same = false;
        }
        check(same, "heightMapToCloud 往返栅格一致");
        shapely::HeightMap bad = hm2c;
        bad.valid = false;
        check(CP::heightMapToCloud(bad) == nullptr, "heightMapToCloud invalid → nullptr");
    }

    // --- 厚度图（2026-11-11 迭代 9）：上/下表面沿 Z 配对求差 ---
    {
        CloudPtr top(new pcl::PointCloud<pcl::PointXYZ>());
        CloudPtr bot(new pcl::PointCloud<pcl::PointXYZ>());
        auto addP = [](const CloudPtr& c, float x, float y, float z) {
            pcl::PointXYZ p;
            p.x = x; p.y = y; p.z = z;
            c->push_back(p);
        };
        addP(top, 0, 0, 5); addP(top, 2, 0, 5);        // 上表面两格
        addP(bot, 0, 0, 2); addP(bot, 3, 0, 1.5f);     // 下表面：col0 配对 / col3 仅下
        const shapely::HeightMap th = CP::thicknessMap(top, bot, 1.0);
        check(th.valid && th.width == 4 && th.height == 1 &&
              std::fabs(th.x0) < 1e-12 && std::fabs(th.cell - 1.0) < 1e-12,
              "厚度图并集栅格 4x1");
        check(th.mask[0] == 1 && std::fabs(th.cells[0] - 3.0f) < 1e-6,
              "厚度 col0 = 5-2 = 3");
        check(th.mask[2] == 0 && std::isnan(th.cells[2]) && th.mask[3] == 0,
              "单侧格缺失 → NaN/mask0");
        check(!CP::thicknessMap(top, CloudPtr(new pcl::PointCloud<pcl::PointXYZ>()), 1.0)
                   .valid &&
              !CP::thicknessMap(top, bot, 0.0).valid, "厚度图空云/cell<=0 → invalid");
    }

    // --- 网格度量：面积 / 有向体积 / 点-网格距离（2026-11-11 迭代 9） ---
    {
        // 立方体 2³：8 顶点 12 面（绕向朝外）
        shapely::Mesh3d cube;
        cube.vertices = { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 },
                          { 0, 0, 2 }, { 2, 0, 2 }, { 2, 2, 2 }, { 0, 2, 2 } };
        const int v0 = 0, v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6, v7 = 7;
        cube.triangles = { { v0, v2, v1 }, { v0, v3, v2 },   // z=0 (−z)
                           { v4, v5, v6 }, { v4, v6, v7 },   // z=2 (+z)
                           { v0, v1, v5 }, { v0, v5, v4 },   // y=0 (−y)
                           { v3, v7, v6 }, { v3, v6, v2 },   // y=2 (+y)
                           { v0, v4, v7 }, { v0, v7, v3 },   // x=0 (−x)
                           { v1, v2, v6 }, { v1, v6, v5 } }; // x=2 (+x)
        cube.valid = true;
        check(near(CP::measureMeshArea(cube), 24.0, 1e-9), "立方体网格面积 24");
        check(near(CP::measureMeshVolume(cube), 8.0, 1e-9), "立方体有向体积 8");
        // 四面体（直角，体积 1/6；斜面 = 边长 √2 正三角形，面积 √3/2）
        shapely::Mesh3d tetra;
        tetra.vertices = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        tetra.triangles = { { 0, 2, 1 }, { 0, 1, 3 }, { 1, 2, 3 }, { 0, 3, 2 } };
        tetra.valid = true;
        check(near(CP::measureMeshArea(tetra), 1.5 + std::sqrt(3.0) / 2.0, 1e-12) &&
              near(CP::measureMeshVolume(tetra), 1.0 / 6.0, 1e-12),
              "四面体面积 1.5+√3/2 / 体积 1/6");
        // 点-网格精确距离
        CloudPtr probes(new pcl::PointCloud<pcl::PointXYZ>());
        const float px[4] = { 1, 3, -1, 1 };   // 内部 / +x 外 / 角外 / 上方
        const float py[4] = { 1, 1, -1, 1 };
        const float pz[4] = { 1, 1, -1, 4 };
        const double expect[4] = { 1.0, 1.0, std::sqrt(3.0), 2.0 };
        for (int k = 0; k < 4; ++k) {
            pcl::PointXYZ q;
            q.x = px[k]; q.y = py[k]; q.z = pz[k];
            probes->push_back(q);
        }
        const std::vector<float> md = CP::measurePointToMeshDistance(probes, cube);
        check(md.size() == 4 &&
              std::fabs(md[0] - expect[0]) < 1e-6 && std::fabs(md[1] - expect[1]) < 1e-6 &&
              std::fabs(md[2] - expect[2]) < 1e-6 && std::fabs(md[3] - expect[3]) < 1e-6,
              "点到网格精确距离（内/外/角/上）");   // float 输出，√3 容差 1e-6
        check(CP::measurePointToMeshDistance(CloudPtr(new pcl::PointCloud<pcl::PointXYZ>()),
                                             cube).empty() &&
              CP::measurePointToMeshDistance(probes, shapely::Mesh3d{}).empty(),
              "点-网格距离无效入参 → 空");
        // 带符号距离（2026-11-12 迭代 12）：内负外正
        {
            const std::vector<float> sd = CP::measurePointToMeshSignedDistance(probes, cube);
            check(sd.size() == 4 && std::fabs(sd[0] + 1.0f) < 1e-6 &&
                  std::fabs(sd[1] - 1.0f) < 1e-6 && std::fabs(sd[2] - std::sqrt(3.0)) < 1e-6 &&
                  std::fabs(sd[3] - 2.0f) < 1e-6, "带符号距离 内- 外+ (含 √3 角点)");
            check(CP::measurePointToMeshSignedDistance(
                      CloudPtr(new pcl::PointCloud<pcl::PointXYZ>()), cube)
                      .empty(), "带符号距离空云 → 空");
        }
        // 每顶点法线（2026-11-12 迭代 12）：立方体角点 = 三相邻面法线之和归一
        {
            const std::vector<float> vn = CP::meshVertexNormals(cube);
            const double inv3 = 1.0 / std::sqrt(3.0);
            bool vnOk = vn.size() == 24;
            if (vnOk) {
                // v0=(0,0,0) 相邻面 −x/−y/−z → (−1,−1,−1)/√3；v6=(2,2,2) → (+,+,+)/√3
                //（float 输出，容差 1e-6）
                vnOk = std::fabs(vn[0] + inv3) < 1e-6 && std::fabs(vn[1] + inv3) < 1e-6 &&
                       std::fabs(vn[2] + inv3) < 1e-6 &&
                       std::fabs(vn[18] - inv3) < 1e-6 && std::fabs(vn[19] - inv3) < 1e-6 &&
                       std::fabs(vn[20] - inv3) < 1e-6;
            }
            check(vnOk, "meshVertexNormals 角点三面合成");
        }
        // OBB 裁剪网格（2026-11-12 迭代 12）：5x5 网格留中段
        {
            shapely::Mesh3d gm;
            for (int j = 0; j < 5; ++j)
                for (int i = 0; i < 5; ++i)
                    gm.vertices.push_back(Point3d{ (double)i, (double)j, 0.0 });
            const auto gid = [](int i, int j) { return j * 5 + i; };
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) {
                    gm.triangles.push_back({ gid(i, j), gid(i + 1, j), gid(i + 1, j + 1) });
                    gm.triangles.push_back({ gid(i, j), gid(i + 1, j + 1), gid(i, j + 1) });
                }
            gm.valid = true;
            shapely::OBB3d box;
            box.center = Point3d{ 2, 2, 0 };
            box.extents[0] = 1.2; box.extents[1] = 1.2; box.extents[2] = 0.5;
            box.valid = true;   // 单位旋转 = 轴对齐
            const shapely::Mesh3d cropped = CP::meshCropByOBB(gm, box);
            check(cropped.valid && cropped.vertices.size() == 9 &&
                  cropped.triangles.size() == 8, "OBB 裁剪 → 9 顶点 8 面");
            bool inBox = cropped.valid;
            for (const Point3d& p : cropped.vertices)
                inBox = inBox && p.x >= 0.8 && p.x <= 3.2 && p.y >= 0.8 && p.y <= 3.2;
            check(inBox, "OBB 裁剪顶点全在盒内");
            shapely::OBB3d invBox;
            check(!CP::meshCropByOBB(gm, invBox).valid, "OBB 裁剪 box 无效 → invalid");
        }
        // 网格面法线（2026-11-11 迭代 11）：立方体 12 面轴对齐
        {
            const std::vector<float> fn = CP::meshFaceNormals(cube);
            check(fn.size() == 36, "meshFaceNormals 12 面 × 3");
            const double expN[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { 0, -1, 0 },
                                        { 0, 1, 0 }, { -1, 0, 0 }, { 1, 0, 0 } };
            bool normalsOk = fn.size() == 36;
            for (size_t f = 0; normalsOk && f < 12; ++f) {
                const double* e = expN[f / 2];
                normalsOk = std::fabs((double)fn[f * 3] - e[0]) < 1e-9 &&
                            std::fabs((double)fn[f * 3 + 1] - e[1]) < 1e-9 &&
                            std::fabs((double)fn[f * 3 + 2] - e[2]) < 1e-9;
            }
            check(normalsOk, "立方体 12 面法线按绕向");
            shapely::Mesh3d degen;
            degen.vertices = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } };   // 共线
            degen.triangles = { { 0, 1, 2 } };
            degen.valid = true;
            const std::vector<float> dn = CP::meshFaceNormals(degen);
            check(dn.size() == 3 && dn[0] == 0.f && dn[1] == 0.f && dn[2] == 0.f,
                  "退化面法线 = (0,0,0)");
            check(CP::meshFaceNormals(shapely::Mesh3d{}).empty(), "meshFaceNormals 无效 → 空");
        }
    }

    // --- trans：绕中心缩放 ---
    {
        const double scale[3] = { 2.0, 2.0, 2.0 };
        const float center[3] = { 5.0f, 5.0f, 0.0f };
        CloudPtr s = CP::transScaleAround(grid, scale, center);
        // grid[0]=(0,0,0) → (-5,-5,0)；grid[50]=(5,0,0) → (5,-5,0)
        check(s && near((*s)[0].x, -5.0) && near((*s)[0].y, -5.0) &&
              near((*s)[50].x, 5.0) && near((*s)[50].y, -5.0), "transScaleAround 绕 (5,5) 放大 2 倍");
    }

    // --- feature：法线 / 朝向 / FPFH / 协方差 / 马氏距离 / 近邻距离 ---
    auto normals = CP::featureNormalEstimation(grid, 10);
    check(normals && normals->size() == 100, "featureNormalEstimation 100 法线");
    {
        // max_nn = 5：半径 2.5 内邻居被截断到 5 个，平面拟合仍应正确
        auto radius_normals = CP::featureNormalEstimationRadius(grid, 2.5, 5);
        bool all_z = radius_normals && radius_normals->size() == 100;
        if (all_z)
            for (const pcl::Normal& n : radius_normals->points)
                if (std::isfinite(n.normal_z) && std::fabs(n.normal_z) < 0.9f)
                    all_z = false;
        check(all_z, "featureNormalEstimationRadius 平面法向 ±z");

        // 半径过小（无邻居）→ 法线置 NaN，is_dense=false
        auto sparse_normals = CP::featureNormalEstimationRadius(grid, 0.1, 10);
        bool all_nan = sparse_normals && sparse_normals->size() == 100;
        if (all_nan)
            for (const pcl::Normal& n : sparse_normals->points)
                if (!std::isnan(n.normal_x))
                    all_nan = false;
        check(all_nan && sparse_normals && !sparse_normals->is_dense,
              "featureNormalEstimationRadius 邻域不足 → NaN");
    }
    {
        // 先把法线全部翻成 -z，再验证朝向修正
        pcl::PointCloud<pcl::Normal>::Ptr flipped(new pcl::PointCloud<pcl::Normal>(*normals));
        for (pcl::Normal& n : flipped->points)
            n.normal_z = -std::fabs(n.normal_z);
        const float dir[3] = { 0.0f, 0.0f, 1.0f };
        auto fixed = CP::featureOrientNormalsToDirection(flipped, dir);
        bool all_up = fixed && fixed->size() == 100;
        if (all_up)
            for (const pcl::Normal& n : fixed->points)
                if (!(n.normal_z > 0.0f))
                    all_up = false;
        check(all_up, "featureOrientNormalsToDirection 法线统一朝 +z");

        const float vp[3] = { 5.0f, 5.0f, 100.0f };
        auto toward = CP::featureOrientNormalsTowardsPoint(grid, flipped, vp);
        bool all_toward = toward && toward->size() == 100;
        if (all_toward)
            for (const pcl::Normal& n : toward->points)
                if (!(n.normal_z > 0.0f))
                    all_toward = false;
        check(all_toward, "featureOrientNormalsTowardsPoint 法线朝向视点");

        // 全局一致化：把法线随机翻一半，MST 传播后应全部同向
        pcl::PointCloud<pcl::Normal>::Ptr scrambled(new pcl::PointCloud<pcl::Normal>(*normals));
        for (size_t i = 0; i < scrambled->size(); ++i)
            if (i % 2 == 0)
                (*scrambled)[i].normal_z = -(*scrambled)[i].normal_z;
        auto consistent = CP::featureOrientNormalsConsistentTangentPlane(grid, scrambled, 10);
        int up = 0, down = 0;
        if (consistent)
            for (const pcl::Normal& n : consistent->points)
                (n.normal_z > 0.0f ? up : down)++;
        check(consistent && consistent->size() == 100 && (up == 100 || down == 100),
              "featureOrientNormalsConsistentTangentPlane 全局同向");
    }
    {
        auto fpfh = CP::featureFPFH(grid, normals, 2.5);
        check(fpfh && fpfh->size() == 100, "featureFPFH 100 个 33 维描述子");
    }
    {
        std::vector<std::array<float, 9>> covs;
        bool ok = CP::featureCovariances(grid, 10, covs);
        // 平面点云：z 方差 ≈ 0，x/y 方差 > 0
        check(ok && covs.size() == 100 && std::fabs(covs[55][8]) < 1e-5f && covs[55][0] > 0.1f,
              "featureCovariances 平面 zz≈0");
    }
    {
        std::vector<double> md;
        bool ok = CP::featureMahalanobisDistance(noisy, md);
        double max_normal = 0.0;
        for (int i = 0; i < 100; ++i)
            max_normal = std::max(max_normal, md[(size_t)i]);
        check(ok && md.size() == 101 && md[100] > max_normal * 3.0,
              "featureMahalanobisDistance 远点显著异常");
    }
    {
        std::vector<float> d = CP::featureNearestNeighborDistance(grid, 1);
        bool ok = d.size() == 100;
        for (float v : d)
            if (!near(v, 1.0, 1e-4))
                ok = false;
        check(ok, "featureNearestNeighborDistance 网格间距 = 1");
    }

    // --- segment：DBSCAN / 区域生长 ---
    {
        std::vector<int> labels;
        int n = CP::segmentDBSCAN(noisy, 1.5, 3, labels);
        check(n == 1 && labels.size() == 101 && labels[100] == -1 && labels[0] == 0,
              "segmentDBSCAN 1 簇 + 远点噪声");
    }
    {
        auto regions = CP::segmentRegionGrowing(grid, normals, 10, 5.0, 0.5, 10, 10000);
        check(regions.size() == 1 && regions[0].size() == 100, "segmentRegionGrowing 平面 → 1 区域 100 点");
    }

    // --- regist：GICP / 特征配准 / 评估 / 信息矩阵 ---
    {
        CloudPtr shifted = CP::transTranslate(grid, 0.5, 0.0, 0.0);
        float T[16] = { 0 };
        bool ok = CP::registGICP(shifted, grid, 2.0, T);
        check(ok && near(T[3], -0.5, 0.15) && near(T[7], 0.0, 0.15),
              "registGICP 平移 0.5 恢复");
    }
    {
        // 非对称盒体表面点云：平移后靠 FPFH + SAC-IA 求初值
        CloudPtr shape(new Cloud);
        for (int x = 0; x <= 8; ++x)
            for (int y = 0; y <= 4; ++y)
                for (int z = 0; z <= 2; ++z)
                    if (x == 0 || x == 8 || y == 0 || y == 4 || z == 0 || z == 2)
                        shape->push_back(pcl::PointXYZ((float)x, (float)y, (float)z));
        shape->width = (uint32_t)shape->size();
        shape->height = 1;

        auto shape_normals = CP::featureNormalEstimation(shape, 20);
        auto shape_fpfh = CP::featureFPFH(shape, shape_normals, 3.0);
        CloudPtr shape_shifted = CP::transTranslate(shape, 1.0, 0.0, 0.0);
        auto shifted_normals = CP::featureNormalEstimation(shape_shifted, 20);
        auto shifted_fpfh = CP::featureFPFH(shape_shifted, shifted_normals, 3.0);

        float T[16] = { 0 };
        bool ok = shape_fpfh && shifted_fpfh &&
                  CP::registFeatureMatching(shape_shifted, shape, shifted_fpfh, shape_fpfh,
                                            1.5, 2.0, T);
        check(ok && near(T[3], -1.0, 0.3), "registFeatureMatching 平移 1.0 恢复");
    }
    {
        const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        double fitness = 0.0, rmse = 1.0;
        bool ok = CP::registEvaluate(grid, grid, I, 0.01, fitness, rmse);
        check(ok && near(fitness, 1.0) && near(rmse, 0.0, 1e-4), "registEvaluate 同云 fitness=1");

        double info[36];
        bool iok = CP::registInformationMatrix(grid, grid, I, 0.01, info);
        // Open3D 公式：M = Σ G_r·G_rᵀ，平移块 = N·I，旋转块 = Σ[p]×ᵀ[p]×
        // 10x10 网格（z=0）：Σx²=Σy²=2850，Σxy=2025，N=100
        //   M(0,0)=Σ(y²+z²)=2850  M(1,1)=Σ(x²+z²)=2850  M(2,2)=Σ(x²+y²)=5700
        //   M(0,1)=-Σxy=-2025     M(3,3)=M(4,4)=M(5,5)=100
        check(iok && near(info[0], 2850.0, 1e-3) && near(info[7], 2850.0, 1e-3) &&
              near(info[14], 5700.0, 1e-3) && near(info[1], -2025.0, 1e-3) &&
              near(info[21], 100.0, 1e-6) && near(info[28], 100.0, 1e-6) &&
              near(info[35], 100.0, 1e-6),
              "registInformationMatrix 旋转块 Σ[p]×ᵀ[p]× / 平移块 N·I");
    }

    // --- surface：凹包 / MLS / Poisson / 贪心投影 / Marching Cubes ---
    {
        // 凹包：L 形点云（10x10 网格挖掉右上 5x5）→ alpha 足够小时顶点数应明显少于凸包
        CloudPtr lshape(new Cloud);
        for (int x = 0; x < 10; ++x)
            for (int y = 0; y < 10; ++y)
                if (!(x >= 5 && y >= 5))
                    lshape->push_back(pcl::PointXYZ((float)x, (float)y, 0.0f));
        lshape->width = (uint32_t)lshape->size();
        lshape->height = 1;
        CloudPtr concave = CP::surfaceConcaveHull(lshape, 2.0);
        CloudPtr convex = CP::surfaceConvexHull(lshape);
        check(concave && !concave->empty() && concave->size() <= lshape->size(),
              "surfaceConcaveHull 非空");
        // 关键语义：L 形（缺右上角）的凹包必须贴合凹角——
        // 凸包只有 4 个角点，凹包需要更多顶点才能描述凹形轮廓
        check(concave && convex && concave->size() > convex->size(),
              "surfaceConcaveHull L 形顶点数 > 凸包（贴合凹角）");
    }
    {
        // 给平面加 ±0.1 噪声，MLS 应压回 z≈0
        CloudPtr wavy(new Cloud);
        unsigned int seed = 7;
        for (const pcl::PointXYZ& p : grid->points) {
            seed = seed * 1103515245u + 12345u;
            const float noise = ((float)((seed >> 16) & 0x7fff) / 32767.0f - 0.5f) * 0.2f;
            wavy->push_back(pcl::PointXYZ(p.x, p.y, noise));
        }
        wavy->width = (uint32_t)wavy->size();
        wavy->height = 1;
        pcl::PointCloud<pcl::Normal>::Ptr mls_normals;
        CloudPtr smooth = CP::surfaceSmoothMLS(wavy, 2.5, 2, 0.0, &mls_normals);
        // MLS 是高斯加权局部拟合：半径接近点间距时权重衰减慢、平滑力度有限。
        // 客观指标取 z 的均方根：输出应明显小于输入的噪声均方根（均匀 ±0.1 噪声 ≈ 0.0577）。
        double rms_in = 0.0, rms_out = 0.0;
        for (const pcl::PointXYZ& p : wavy->points)
            rms_in += (double)p.z * p.z;
        rms_in = std::sqrt(rms_in / (double)wavy->size());
        if (smooth && smooth->size() == wavy->size())
            for (const pcl::PointXYZ& p : smooth->points)
                rms_out += (double)p.z * p.z;
        rms_out = smooth && smooth->size() == wavy->size()
                      ? std::sqrt(rms_out / (double)smooth->size())
                      : rms_in;
        check(smooth && smooth->size() == 100 && rms_out < rms_in * 0.7,
              "surfaceSmoothMLS 平滑（z 均方根下降 > 30%）");
        check(mls_normals && mls_normals->size() == 100, "surfaceSmoothMLS 输出法线");

        // 回归：voxel_size>0 且不索取法线（PCL 1.9 上采样路径会解引用内部 normals_，
        // 曾因 setComputeNormals(false) 导致空指针崩溃）
        CloudPtr upsampled = CP::surfaceSmoothMLS(wavy, 2.5, 2, 0.5, nullptr);
        check(upsampled != nullptr && !upsampled->empty(),
              "surfaceSmoothMLS voxel 上采样且不索取法线（不崩溃）");
    }
    {
        // 球面点云 + 外向法线（Poisson / GP3 / MarchingCubes 的标准输入）
        CloudPtr sphere(new Cloud);
        const int lat = 12, lon = 24;
        for (int i = 1; i < lat; ++i) {
            const double theta = M_PI * i / lat;
            for (int j = 0; j < lon; ++j) {
                const double phi = 2.0 * M_PI * j / lon;
                sphere->push_back(pcl::PointXYZ((float)(10.0 * std::sin(theta) * std::cos(phi)),
                                                (float)(10.0 * std::sin(theta) * std::sin(phi)),
                                                (float)(10.0 * std::cos(theta))));
            }
        }
        sphere->push_back(pcl::PointXYZ(0.0f, 0.0f, 10.0f));
        sphere->push_back(pcl::PointXYZ(0.0f, 0.0f, -10.0f));
        sphere->width = (uint32_t)sphere->size();
        sphere->height = 1;

        pcl::PointCloud<pcl::Normal>::Ptr sn(new pcl::PointCloud<pcl::Normal>);
        sn->resize(sphere->size());
        for (size_t i = 0; i < sphere->size(); ++i) {
            const float len = std::sqrt((*sphere)[i].x * (*sphere)[i].x +
                                        (*sphere)[i].y * (*sphere)[i].y +
                                        (*sphere)[i].z * (*sphere)[i].z);
            (*sn)[i].normal_x = (*sphere)[i].x / len;
            (*sn)[i].normal_y = (*sphere)[i].y / len;
            (*sn)[i].normal_z = (*sphere)[i].z / len;
        }

        shapely::Mesh3d poisson = CP::surfacePoisson(sphere, sn, 6, 5, 1.0);
        check(poisson.valid && !poisson.vertices.empty() && !poisson.triangles.empty(),
              "surfacePoisson 球面 → 非空网格");
        // 几何正确性：重建顶点应落在半径 10 的球面附近
        double max_radial_err = 0.0;
        for (const shapely::Point3d& v : poisson.vertices) {
            const double r = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            max_radial_err = std::max(max_radial_err, std::fabs(r - 10.0));
        }
        check(poisson.valid && max_radial_err < 1.0, "surfacePoisson 球半径 ≈ 10");

        shapely::Mesh3d gp3 = CP::surfaceGreedyProjection(sphere, sn, 3.0, 2.5, 50);
        check(gp3.valid && !gp3.triangles.empty(), "surfaceGreedyProjection 球面 → 三角面");
        // GP3 顶点取自原始点云，半径应严格为 10
        double gp3_max_err = 0.0;
        for (const shapely::Point3d& v : gp3.vertices) {
            const double r = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            gp3_max_err = std::max(gp3_max_err, std::fabs(r - 10.0));
        }
        check(gp3.valid && gp3_max_err < 1e-3, "surfaceGreedyProjection 顶点在球面上");

        shapely::Mesh3d mc = CP::surfaceMarchingCubes(sphere, sn, 24);
        check(mc.valid && !mc.vertices.empty(), "surfaceMarchingCubes 球面 → 顶点");
        double mc_max_err = 0.0;
        for (const shapely::Point3d& v : mc.vertices) {
            const double r = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            mc_max_err = std::max(mc_max_err, std::fabs(r - 10.0));
        }
        check(mc.valid && mc_max_err < 1.5, "surfaceMarchingCubes 球半径 ≈ 10");

        // --- 新增：RBF / GridProjection / 网格后处理 ---
        shapely::Mesh3d rbf = CP::surfaceMarchingCubesRBF(sphere, sn, 24, 0.1);
        double rbf_max_err = 0.0;
        for (const shapely::Point3d& v : rbf.vertices) {
            const double r = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            rbf_max_err = std::max(rbf_max_err, std::fabs(r - 10.0));
        }
        check(rbf.valid && !rbf.triangles.empty() && rbf_max_err < 2.0,
              "surfaceMarchingCubesRBF 球面 → 网格（半径 ≈ 10）");

        // GridProjection 的 PCL 实现要求每个体素邻域内 >= 10 个点（硬编码），
        // 稀疏表面点云会被全部跳过；这里用稠密体点云验证。
        CloudPtr denseBall(new Cloud);
        for (int x = -4; x <= 4; ++x)
            for (int y = -4; y <= 4; ++y)
                for (int z = -4; z <= 4; ++z)
                    if (x * x + y * y + z * z <= 16)
                        denseBall->push_back(pcl::PointXYZ(x * 0.5f, y * 0.5f, z * 0.5f));
        denseBall->width = (uint32_t)denseBall->size();
        denseBall->height = 1;
        pcl::PointCloud<pcl::Normal>::Ptr dn(new pcl::PointCloud<pcl::Normal>);
        dn->resize(denseBall->size());
        for (size_t i = 0; i < denseBall->size(); ++i) {
            const float len = std::sqrt((*denseBall)[i].x * (*denseBall)[i].x +
                                        (*denseBall)[i].y * (*denseBall)[i].y +
                                        (*denseBall)[i].z * (*denseBall)[i].z);
            // 球心点 (0,0,0) 无法定法线方向，用 +z 兜底（避免 0/0 → NaN 污染重建）
            (*dn)[i].normal_x = len > 0.0f ? (*denseBall)[i].x / len : 0.0f;
            (*dn)[i].normal_y = len > 0.0f ? (*denseBall)[i].y / len : 0.0f;
            (*dn)[i].normal_z = len > 0.0f ? (*denseBall)[i].z / len : 1.0f;
        }
        shapely::Mesh3d grid = CP::surfaceGridProjection(denseBall, dn, 0.1, 3);
        check(grid.valid && !grid.vertices.empty() && !grid.triangles.empty(),
              "surfaceGridProjection 稠密体点云 → 非空网格");
        {
            // 几何特性：GridProjection 提取的是体素网格上的等值面，
            // 顶点落在体素边插值处（阶梯状），精度量级 ≈ leaf×padding，不是原始球面。
            // 因此只校验"顶点包围盒尺度合理"，不校验径向精度。
            shapely::AABB3d bb;
            bb.min_x = bb.min_y = bb.min_z = std::numeric_limits<double>::max();
            bb.max_x = bb.max_y = bb.max_z = -std::numeric_limits<double>::max();
            for (const shapely::Point3d& v : grid.vertices) {
                bb.min_x = std::min(bb.min_x, v.x); bb.max_x = std::max(bb.max_x, v.x);
                bb.min_y = std::min(bb.min_y, v.y); bb.max_y = std::max(bb.max_y, v.y);
                bb.min_z = std::min(bb.min_z, v.z); bb.max_z = std::max(bb.max_z, v.z);
            }
            const double span = std::max({ bb.max_x - bb.min_x, bb.max_y - bb.min_y,
                                           bb.max_z - bb.min_z });
            check(span > 2.0 && span < 8.0, "surfaceGridProjection 包围盒尺度合理（R=2 球）");
            // 已知限制回归：输入包围盒 >1 时 PCL 归一化输入但不还原平移，
            // 本库只补偿了尺度（不补偿绝对位置）。固定该行为，避免被误判为回归。
            const double cx = 0.5 * (bb.min_x + bb.max_x);
            const double cy = 0.5 * (bb.min_y + bb.max_y);
            const double cz = 0.5 * (bb.min_z + bb.max_z);
            std::printf("    [info] GridProjection 已知限制：输出中心 (%.2f,%.2f,%.2f) 偏离原点 %.2f\n",
                        cx, cy, cz, std::sqrt(cx * cx + cy * cy + cz * cz));
            check(std::fabs(cx) < 1.0 && std::fabs(cy) < 1.0 && std::fabs(cz) < 1.0,
                  "surfaceGridProjection 位置偏移在已知限制范围内（<1 单位）");
            bool idx_ok = true;
            for (const std::array<int, 3>& t : grid.triangles)
                for (int i : t)
                    if (i < 0 || (size_t)i >= grid.vertices.size())
                        idx_ok = false;
            check(idx_ok, "surfaceGridProjection 面索引合法");
        }
        check(!CP::surfaceGridProjection(sphere, sn, 0.0, 1).valid,
              "surfaceGridProjection(res=0) → 无效");

        // 拉普拉斯平滑：拓扑不变；且必须体现其固有收缩特性（球面半径单调减小）
        shapely::Mesh3d smoothed = CP::surfaceMeshLaplacianSmooth(poisson, 5, 0.3);
        check(smoothed.valid && smoothed.vertices.size() == poisson.vertices.size() &&
              smoothed.triangles.size() == poisson.triangles.size(),
              "surfaceMeshLaplacianSmooth 拓扑不变（顶点/面数一致）");
        {
            // 均值半径：平滑后应严格小于原始（拉普拉斯收缩），但不应塌缩
            auto meanRadius = [](const shapely::Mesh3d& m) {
                double s = 0.0;
                for (const shapely::Point3d& v : m.vertices)
                    s += std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                return s / (double)m.vertices.size();
            };
            const double r0 = meanRadius(poisson);
            const double r5 = meanRadius(smoothed);
            shapely::Mesh3d smoothed20 = CP::surfaceMeshLaplacianSmooth(poisson, 20, 0.3);
            const double r20 = meanRadius(smoothed20);
            check(r5 < r0 && r20 < r5 && r20 > r0 * 0.9,
                  "surfaceMeshLaplacianSmooth 收缩单调（迭代越多半径越小但未塌缩）");
        }
        check(!CP::surfaceMeshLaplacianSmooth(poisson, 0, 0.3).valid,
              "surfaceMeshLaplacianSmooth(iterations=0) → 无效");
        check(!CP::surfaceMeshLaplacianSmooth(poisson, 5, 0.0).valid,
              "surfaceMeshLaplacianSmooth(lambda=0) → 无效");
        {
            shapely::Mesh3d bad;
            bad.vertices.push_back(shapely::Point3d{ 0, 0, 0 });
            bad.triangles.push_back({ 0, 1, 2 });   // 索引越界
            check(!CP::surfaceMeshLaplacianSmooth(bad, 5, 0.5).valid,
                  "surfaceMeshLaplacianSmooth 越界索引 → 无效");
        }
    }

    // --- measure：包围盒 / 质心 / 协方差 / 点云距离 ---
    {
        Point3d mn, mx;
        bool ok = CP::measureMinMax(grid, mn, mx);
        check(ok && near(mn.x, 0.0) && near(mx.x, 9.0) && near(mx.z, 0.0),
              "measureMinMax 网格 [0,9]");
        // 含 NaN 点云被错标 is_dense=true 时，PCL getMinMax3D 不检查有限性。
        // 本库兜底：忽略非有限点，只统计有效点。
        {
            CloudPtr badflag(new Cloud(*grid));
            badflag->push_back(pcl::PointXYZ(std::numeric_limits<float>::quiet_NaN(),
                                             0.0f, 0.0f));
            badflag->push_back(pcl::PointXYZ(999.0f, 0.0f, 0.0f));
            badflag->width = (uint32_t)badflag->size();
            badflag->is_dense = true;   // 故意错标
            Point3d bn, bx;
            bool bok = CP::measureMinMax(badflag, bn, bx);
            // NaN 点被忽略；999 仍在范围内 → [0,999]
            check(bok && near(bn.x, 0.0) && near(bx.x, 999.0),
                  "measureMinMax 忽略 NaN（dense 错标兜底）");
        }
        shapely::AABB3d aabb = CP::measureAABB(grid);
        check(aabb.valid && near(aabb.max_x - aabb.min_x, 9.0) && near(aabb.volume(), 0.0),
              "measureAABB 平面盒体积 0");
        shapely::OBB3d obb = CP::measureOBB(grid);
        check(obb.valid && near(obb.extents[0], 4.5, 1e-3) && near(obb.extents[1], 4.5, 1e-3) &&
              near(obb.center.x, 4.5, 1e-3) && near(obb.center.y, 4.5, 1e-3),
              "measureOBB 半长 4.5 / 中心 (4.5,4.5)");
        Point3d c;
        check(CP::measureCentroid(grid, c) && near(c.x, 4.5) && near(c.y, 4.5),
              "measureCentroid (4.5,4.5)");
        Point3d mean;
        double cov[9];
        check(CP::measureMeanAndCovariance(grid, mean, cov) &&
              near(mean.x, 4.5) && cov[8] < 1e-6 && cov[0] > 1.0,
              "measureMeanAndCovariance 平面 zz≈0");
        CloudPtr shifted = CP::transTranslate(grid, 0.5, 0.0, 0.0);
        std::vector<float> dist = CP::measurePointCloudDistance(shifted, grid);
        bool all_half = dist.size() == 100;
        for (float v : dist)
            if (!near(v, 0.5, 1e-4))
                all_half = false;
        check(all_half, "measurePointCloudDistance 平移 0.5 → 全部 0.5");
    }
}

// ---------------- CloudProcessor 契约与边界（空输入 / 非法参数 / 退化数据） ----------------

// 每个新接口在空输入、非法参数下的返回约定必须稳定：不崩溃、返回 nullptr/false/空。
static void testCloudProcessorContracts()
{
    std::printf("== CloudProcessor 契约（空输入/非法参数） ==\n");
    using CP = shapely::CloudProcessor<pcl::PointXYZ>;
    using Cloud = pcl::PointCloud<pcl::PointXYZ>;
    using CloudPtr = Cloud::Ptr;

    const float kUp[3] = { 0.0f, 0.0f, 1.0f };
    const float kOrigin[3] = { 0.0f, 0.0f, 0.0f };
    const float kZero16[16] = { 0 };

    CloudPtr nullCloud;                                   // 空指针
    CloudPtr emptyCloud(new Cloud);                       // 空点云
    emptyCloud->width = 0;
    emptyCloud->height = 1;

    CloudPtr tiny(new Cloud);                             // 1 点（不足拟合/邻域）
    tiny->push_back(pcl::PointXYZ(0.0f, 0.0f, 0.0f));
    tiny->width = 1;
    tiny->height = 1;

    // --- filter ---
    check(CP::filterVoxelGrid(nullCloud, 1.0) == nullptr, "filterVoxelGrid(nullptr) → nullptr");
    check(CP::filterVoxelGrid(emptyCloud, 1.0) != nullptr &&
          CP::filterVoxelGrid(emptyCloud, 1.0)->empty(), "filterVoxelGrid(空云) → 空云");
    check(CP::filterVoxelGrid(tiny, 0.0) == nullptr, "filterVoxelGrid(leaf=0) → nullptr");
    check(CP::filterVoxelGrid(tiny, -1.0) == nullptr, "filterVoxelGrid(leaf<0) → nullptr");
    check(CP::filterRandomSample(tiny, 0) == nullptr, "filterRandomSample(0) → nullptr");
    check(CP::filterUniformSample(tiny, 0) == nullptr, "filterUniformSample(0) → nullptr");
    check(CP::filterFarthestPointSample(tiny, 0) == nullptr, "filterFarthestPointSample(0) → nullptr");
    check(CP::filterFarthestPointSample(nullCloud, 5) == nullptr, "filterFarthestPointSample(nullptr) → nullptr");
    check(CP::filterRemoveNonFinite(nullCloud) == nullptr, "filterRemoveNonFinite(nullptr) → nullptr");
    check(CP::filterRemoveDuplicates(nullCloud) == nullptr, "filterRemoveDuplicates(nullptr) → nullptr");
    {
        std::vector<std::vector<int>> trace;
        check(CP::filterVoxelGridAndTrace(nullCloud, 1.0, trace) == nullptr && trace.empty(),
              "filterVoxelGridAndTrace(nullptr) → nullptr + 空 trace");
        check(CP::filterVoxelGridAndTrace(tiny, 0.0, trace) == nullptr, "filterVoxelGridAndTrace(leaf=0) → nullptr");
    }

    // --- extract / trans ---
    check(CP::extractByIndices(nullCloud, { 0 }) == nullptr, "extractByIndices(nullptr) → nullptr");
    check(CP::extractByIndices(tiny, {}) == nullptr, "extractByIndices(空索引) → nullptr");
    check(CP::extractByIndicesInverted(nullCloud, { 0 }) == nullptr, "extractByIndicesInverted(nullptr) → nullptr");
    {
        // 反向提取空索引 = 全量拷贝（不是 nullptr）
        CloudPtr all = CP::extractByIndicesInverted(tiny, {});
        check(all && all->size() == 1, "extractByIndicesInverted(空索引) → 全量");
    }
    {
        const float lo[3] = { 0, 0, 0 }, hi[3] = { 1, 1, 1 };
        check(CP::extractCropBox(nullCloud, lo, hi) == nullptr, "extractCropBox(nullptr) → nullptr");
        // min/max 反序应自动纠正
        const float hi2[3] = { 0, 0, 0 }, lo2[3] = { 1, 1, 1 };
        CloudPtr c = CP::extractCropBox(tiny, hi2, lo2);
        check(c && c->size() == 1, "extractCropBox 反序角点自动纠正");
        shapely::OBB3d badBox;   // valid=false
        check(CP::extractCropOBB(tiny, badBox) == nullptr, "extractCropOBB(无效盒) → nullptr");
    }
    check(CP::transApplyMatrix(nullCloud, kZero16) == nullptr, "transApplyMatrix(nullptr) → nullptr");
    {
        const float bad[4] = { 0, 0, 0, 0 };
        check(CP::transProject(tiny, bad) == nullptr, "transProject(零法向) → nullptr");
        check(CP::transRotate(tiny, 0, 0, 0, 1.0) == nullptr, "transRotate(零轴) → nullptr");
        const float pivot[3] = { 0, 0, 0 };
        check(CP::transRotateAround(tiny, pivot, 0, 0, 0, 1.0) == nullptr,
              "transRotateAround(零轴) → nullptr");
    }

    // --- feature ---
    check(CP::featureNormalEstimation(nullCloud, 10) == nullptr, "featureNormalEstimation(nullptr) → nullptr");
    check(CP::featureNormalEstimation(tiny, 2) == nullptr, "featureNormalEstimation(k<3) → nullptr");
    check(CP::featureNormalEstimationRadius(tiny, 0.0, 10) == nullptr,
          "featureNormalEstimationRadius(r=0) → nullptr");
    check(CP::featureOrientNormalsToDirection(nullptr, kUp) == nullptr,
          "featureOrientNormalsToDirection(nullptr) → nullptr");
    {
        auto n = CP::featureNormalEstimation(tiny, 3);
        const float zero[3] = { 0, 0, 0 };
        check(CP::featureOrientNormalsToDirection(n, zero) == nullptr,
              "featureOrientNormalsToDirection(零方向) → nullptr");
        check(CP::featureOrientNormalsTowardsPoint(tiny, nullptr, kUp) == nullptr,
              "featureOrientNormalsTowardsPoint(空法线) → nullptr");
        // 法线数与点数不匹配 → nullptr（避免越界）
        CloudPtr two(new Cloud(*tiny));
        two->push_back(pcl::PointXYZ(1, 0, 0));
        two->width = 2;
        check(CP::featureOrientNormalsTowardsPoint(two, n, kUp) == nullptr,
              "featureOrientNormalsTowardsPoint(法线数不匹配) → nullptr");
        check(CP::featureOrientNormalsConsistentTangentPlane(two, n, 10) == nullptr,
              "featureOrientNormalsConsistentTangentPlane(法线数不匹配) → nullptr");
        check(CP::featureOrientNormalsConsistentTangentPlane(tiny, n, 1) == nullptr,
              "featureOrientNormalsConsistentTangentPlane(k<2) → nullptr");
        check(CP::featureFPFH(tiny, n, 0.0) == nullptr, "featureFPFH(r=0) → nullptr");
        // 点数 < 3：PCL 会返回全零直方图且 is_dense=true（看似合法），接口层必须拦截
        check(CP::featureFPFH(tiny, n, 1.0) == nullptr, "featureFPFH(点数<3) → nullptr");
    }
    {
        std::vector<std::array<float, 9>> covs;
        check(!CP::featureCovariances(nullCloud, 10, covs) && covs.empty(),
              "featureCovariances(nullptr) → false");
        check(!CP::featureCovariances(tiny, 2, covs), "featureCovariances(k<3) → false");
        std::vector<double> md;
        check(!CP::featureMahalanobisDistance(tiny, md) && md.empty(),
              "featureMahalanobisDistance(<4 点) → false");
        check(CP::featureNearestNeighborDistance(nullCloud).empty(),
              "featureNearestNeighborDistance(nullptr) → 空");
    }

    // --- segment ---
    {
        std::vector<int> labels;
        check(CP::segmentDBSCAN(nullCloud, 1.0, 3, labels) == 0 && labels.empty(),
              "segmentDBSCAN(nullptr) → 0 簇");
        check(CP::segmentDBSCAN(tiny, 0.0, 3, labels) == 0, "segmentDBSCAN(eps=0) → 0 簇");
        check(CP::segmentDBSCAN(tiny, 1.0, 0, labels) == 0, "segmentDBSCAN(min_pts=0) → 0 簇");
        // 单点、min_points=1：自身即核心 → 1 簇
        check(CP::segmentDBSCAN(tiny, 1.0, 1, labels) == 1 && labels.size() == 1 && labels[0] == 0,
              "segmentDBSCAN 单点 min_pts=1 → 1 簇");
    }
    {
        auto regions = CP::segmentRegionGrowing(nullCloud, nullptr, 30, 5.0, 0.5, 1, 100);
        check(regions.empty(), "segmentRegionGrowing(nullptr) → 空");
        auto n = CP::featureNormalEstimation(tiny, 3);
        check(CP::segmentRegionGrowing(tiny, n, 2, 5.0, 0.5, 1, 100).empty(),
              "segmentRegionGrowing(k<3) → 空");
    }

    // --- regist ---
    {
        float T[16];
        check(!CP::registGICP(nullCloud, tiny, 1.0, T), "registGICP(nullptr) → false");
        check(!CP::registGICP(emptyCloud, tiny, 1.0, T), "registGICP(空源) → false");
        check(!CP::registFeatureMatching(tiny, tiny, nullptr, nullptr, 1.0, 1.0, T),
              "registFeatureMatching(空特征) → false");
        double fitness = 0, rmse = 0;
        check(!CP::registEvaluate(nullCloud, tiny, kZero16, 1.0, fitness, rmse),
              "registEvaluate(nullptr) → false");
        double info[36];
        check(!CP::registInformationMatrix(tiny, tiny, kZero16, 1.0, info),
              "registInformationMatrix(点数<6) → false");
    }

    // --- surface ---
    check(CP::surfaceConcaveHull(nullCloud, 1.0) == nullptr, "surfaceConcaveHull(nullptr) → nullptr");
    check(CP::surfaceConcaveHull(tiny, 0.0) == nullptr, "surfaceConcaveHull(alpha=0) → nullptr");
    check(CP::surfaceSmoothMLS(nullCloud, 1.0) == nullptr, "surfaceSmoothMLS(nullptr) → nullptr");
    check(CP::surfaceSmoothMLS(tiny, 0.0) == nullptr, "surfaceSmoothMLS(r=0) → nullptr");
    {
        auto n = CP::featureNormalEstimation(tiny, 3);
        check(!CP::surfacePoisson(tiny, nullptr, 6, 5, 1.0).valid, "surfacePoisson(空法线) → 无效");
        check(!CP::surfacePoisson(tiny, n, 6, 5, 1.0).valid, "surfacePoisson(点数不足) → 无效");
        check(!CP::surfaceGreedyProjection(tiny, n, 0.0, 2.5, 50).valid,
              "surfaceGreedyProjection(r=0) → 无效");
        check(!CP::surfaceMarchingCubes(tiny, n, 4).valid, "surfaceMarchingCubes(res<8) → 无效");
    }

    // 法线含 NaN：PCL 的 Poisson/GP3/MarchingCubes 不会报错（仍返回"成功"），
    // 但结果不可用。本库在入口拦截（有效法线占比必须过半）。
    {
        using CloudPtr = pcl::PointCloud<pcl::PointXYZ>::Ptr;
        CloudPtr ball(new pcl::PointCloud<pcl::PointXYZ>);
        for (int i = 1; i < 12; ++i) {
            const double th = M_PI * i / 12;
            for (int j = 0; j < 20; ++j) {
                const double ph = 2.0 * M_PI * j / 20;
                ball->push_back(pcl::PointXYZ((float)(10.0 * std::sin(th) * std::cos(ph)),
                                              (float)(10.0 * std::sin(th) * std::sin(ph)),
                                              (float)(10.0 * std::cos(th))));
            }
        }
        ball->width = (uint32_t)ball->size();
        ball->height = 1;

        pcl::PointCloud<pcl::Normal>::Ptr bad(new pcl::PointCloud<pcl::Normal>);
        bad->resize(ball->size());
        for (size_t i = 0; i < ball->size(); ++i) {
            const float len = std::sqrt((*ball)[i].x * (*ball)[i].x +
                                        (*ball)[i].y * (*ball)[i].y +
                                        (*ball)[i].z * (*ball)[i].z);
            if (i % 3 == 0) {   // 1/3 的点法线为 NaN
                (*bad)[i].normal_x = (*bad)[i].normal_y = (*bad)[i].normal_z =
                    std::numeric_limits<float>::quiet_NaN();
            } else {
                (*bad)[i].normal_x = (*ball)[i].x / len;
                (*bad)[i].normal_y = (*ball)[i].y / len;
                (*bad)[i].normal_z = (*ball)[i].z / len;
            }
        }
        // 有效法线占 2/3（过半）→ 应剔除 NaN 点后正常重建
        shapely::Mesh3d p1 = CP::surfacePoisson(ball, bad, 6, 5, 1.0);
        check(p1.valid && !p1.triangles.empty(), "surfacePoisson 剔除 NaN 法线点后成功");

        // 2/3 为 NaN（有效占比 < 50%）→ 整体拒绝，避免产出垃圾网格
        pcl::PointCloud<pcl::Normal>::Ptr mostly_bad(new pcl::PointCloud<pcl::Normal>);
        mostly_bad->resize(ball->size());
        for (size_t i = 0; i < ball->size(); ++i) {
            const float len = std::sqrt((*ball)[i].x * (*ball)[i].x +
                                        (*ball)[i].y * (*ball)[i].y +
                                        (*ball)[i].z * (*ball)[i].z);
            if (i % 3 != 0) {   // 2/3 的点法线为 NaN
                (*mostly_bad)[i].normal_x = (*mostly_bad)[i].normal_y = (*mostly_bad)[i].normal_z =
                    std::numeric_limits<float>::quiet_NaN();
            } else {
                (*mostly_bad)[i].normal_x = (*ball)[i].x / len;
                (*mostly_bad)[i].normal_y = (*ball)[i].y / len;
                (*mostly_bad)[i].normal_z = (*ball)[i].z / len;
            }
        }
        check(!CP::surfacePoisson(ball, mostly_bad, 6, 5, 1.0).valid,
              "surfacePoisson(有效法线 <50%) → 拒绝");
        check(!CP::surfaceMarchingCubes(ball, mostly_bad, 24).valid,
              "surfaceMarchingCubes(有效法线 <50%) → 拒绝");
        check(!CP::surfaceGreedyProjection(ball, mostly_bad, 3.0, 2.5, 50).valid,
              "surfaceGreedyProjection(有效法线 <50%) → 拒绝");
        check(!CP::surfaceMarchingCubesRBF(ball, mostly_bad, 24, 0.1).valid,
              "surfaceMarchingCubesRBF(有效法线 <50%) → 拒绝");
        check(!CP::surfaceGridProjection(ball, mostly_bad, 0.5, 1).valid,
              "surfaceGridProjection(有效法线 <50%) → 拒绝");
    }

    // --- measure ---
    {
        Point3d mn, mx;
        check(!CP::measureMinMax(nullCloud, mn, mx), "measureMinMax(nullptr) → false");
        check(!CP::measureAABB(emptyCloud).valid, "measureAABB(空云) → invalid");
        check(!CP::measureOBB(tiny).valid, "measureOBB(<3 点) → invalid");
        Point3d c;
        check(!CP::measureCentroid(nullCloud, c), "measureCentroid(nullptr) → false");
        double cov[9];
        check(!CP::measureMeanAndCovariance(tiny, c, cov), "measureMeanAndCovariance(1 点) → false");
        check(CP::measurePointCloudDistance(nullCloud, tiny).empty(),
              "measurePointCloudDistance(nullptr) → 空");
        check(CP::measurePointCloudDistance(tiny, nullCloud).empty(),
              "measurePointCloudDistance(target=null) → 空");
    }
}

// ---------------- CloudProcessor 拟合与配准变体（球/圆柱/点到面/NDT） ----------------

static void testCloudProcessorFitRegistVariants()
{
    std::printf("== CloudProcessor 拟合/配准变体 ==\n");
    using CP = shapely::CloudProcessor<pcl::PointXYZ>;
    using Cloud = pcl::PointCloud<pcl::PointXYZ>;
    using CloudPtr = Cloud::Ptr;

    // 球面点云：球心 (5,5,5)、半径 3
    CloudPtr sphere(new Cloud);
    const int lat = 16, lon = 32;
    for (int i = 1; i < lat; ++i) {
        const double theta = M_PI * i / lat;
        for (int j = 0; j < lon; ++j) {
            const double phi = 2.0 * M_PI * j / lon;
            sphere->push_back(pcl::PointXYZ(
                (float)(5.0 + 3.0 * std::sin(theta) * std::cos(phi)),
                (float)(5.0 + 3.0 * std::sin(theta) * std::sin(phi)),
                (float)(5.0 + 3.0 * std::cos(theta))));
        }
    }
    sphere->width = (uint32_t)sphere->size();
    sphere->height = 1;

    {
        std::vector<float> center;
        double radius = 0.0;
        std::vector<int> inliers;
        const bool ok = CP::fitSphere(sphere, shapely::FitMethod::RANSAC, 0.05, center, radius, inliers);
        check(ok && center.size() == 3 && near(center[0], 5.0, 0.2) &&
              near(center[1], 5.0, 0.2) && near(center[2], 5.0, 0.2) && near(radius, 3.0, 0.2),
              "fitSphere 球心 (5,5,5) / 半径 3");
        // MSAC 在 PCL 1.9 球模型不支持 → 内部回退 RANSAC，仍应成功
        std::vector<float> c2;
        double r2 = 0.0;
        std::vector<int> in2;
        check(CP::fitSphere(sphere, shapely::FitMethod::MSAC, 0.05, c2, r2, in2) &&
              near(r2, 3.0, 0.2), "fitSphere MSAC 回退 RANSAC 仍成功");
        // 平面点云上拟合球：无局内点 → false
        CloudPtr flat(new Cloud);
        for (int x = 0; x < 10; ++x)
            for (int y = 0; y < 10; ++y)
                flat->push_back(pcl::PointXYZ((float)x, (float)y, 0.0f));
        flat->width = (uint32_t)flat->size();
        flat->height = 1;
        std::vector<float> c3;
        double r3 = 0.0;
        std::vector<int> in3;
        const bool flatOk = CP::fitSphere(flat, shapely::FitMethod::RANSAC, 0.01, c3, r3, in3);
        check(!flatOk || in3.empty() || r3 > 100.0, "fitSphere 平面点云不产生小半径球");
    }

    // 圆柱面点云：轴沿 z、过 (2,3)、半径 4
    {
        CloudPtr cyl(new Cloud);
        for (int k = 0; k < 20; ++k)
            for (int j = 0; j < 40; ++j) {
                const double a = 2.0 * M_PI * j / 40.0;
                cyl->push_back(pcl::PointXYZ((float)(2.0 + 4.0 * std::cos(a)),
                                             (float)(3.0 + 4.0 * std::sin(a)),
                                             (float)(k * 0.5)));
            }
        cyl->width = (uint32_t)cyl->size();
        cyl->height = 1;
        std::vector<float> axis, pt;
        double radius = 0.0;
        std::vector<int> inliers;
        const bool ok = CP::fitCylinder(cyl, shapely::FitMethod::RANSAC, 0.05, axis, pt, radius, inliers);
        const double axis_z = (axis.size() == 3) ? std::fabs((double)axis[2]) : 0.0;
        check(ok && axis.size() == 3 && pt.size() == 3 && axis_z > 0.95 && near(radius, 4.0, 0.2),
              "fitCylinder 轴向 z / 半径 4");
    }

    // 点到面 ICP：需要 target 法线
    {
        CloudPtr planeCloud(new Cloud);
        for (int x = 0; x < 10; ++x)
            for (int y = 0; y < 10; ++y)
                planeCloud->push_back(pcl::PointXYZ((float)x, (float)y, 0.0f));
        planeCloud->width = (uint32_t)planeCloud->size();
        planeCloud->height = 1;
        auto normals = CP::featureNormalEstimation(planeCloud, 10);
        CloudPtr shifted = CP::transTranslate(planeCloud, 0.3, 0.0, 0.0);
        float T[16] = { 0 };
        const bool pp_ok = CP::registICP_PointToPlane(shifted, planeCloud, normals, 2.0, T);
        check(pp_ok && near(T[3], -0.3, 0.05), "registICP_PointToPlane 平移 0.3 恢复");
        // 法线为空 → false
        float T2[16] = { 0 };
        check(!CP::registICP_PointToPlane(shifted, planeCloud, nullptr, 2.0, T2),
              "registICP_PointToPlane(空法线) → false");
    }

    // NDT：需要每个体素内有足够点支撑正态分布，用带噪声的稠密点云（更接近真实扫描）
    {
        CloudPtr dense(new Cloud);
        unsigned int seed = 12345u;
        auto rnd = [&seed]() {
            seed = seed * 1103515245u + 12345u;
            return (float)((seed >> 16) & 0x7fff) / 32767.0f - 0.5f;
        };
        for (int x = 0; x < 20; ++x)
            for (int y = 0; y < 20; ++y)
                for (int z = 0; z < 8; ++z)
                    dense->push_back(pcl::PointXYZ(x * 0.5f + 0.05f * rnd(),
                                                   y * 0.5f + 0.05f * rnd(),
                                                   z * 0.5f + 0.05f * rnd()));
        dense->width = (uint32_t)dense->size();
        dense->height = 1;
        CloudPtr shifted = CP::transTranslate(dense, 0.4, 0.0, 0.0);
        float T[16] = { 0 };
        const bool ok = CP::registNDT(shifted, dense, 2.0, T);
        // NDT 对初值/体素分辨率敏感，可能收敛到局部极小：
        // 契约要求"收敛时变换有限且平移量在合理范围"，不强制精确恢复
        bool finite = true;
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(T[i]))
                finite = false;
        check(ok && finite && std::fabs(T[3]) < 1.0, "registNDT 收敛且变换有限/平移合理");
        float T2[16] = { 0 };
        check(!CP::registNDT(nullptr, dense, 1.0, T2), "registNDT(nullptr) → false");
    }

    // 特征配准：特征数与点数不匹配 → false
    {
        CloudPtr a(new Cloud(*sphere));
        auto n = CP::featureNormalEstimation(a, 20);
        auto f = CP::featureFPFH(a, n, 2.0);
        float T[16] = { 0 };
        check(!CP::registFeatureMatching(a, a, f, nullptr, 1.0, 1.0, T),
              "registFeatureMatching(空特征) → false");
        auto truncated = pcl::PointCloud<pcl::FPFHSignature33>::Ptr(
            new pcl::PointCloud<pcl::FPFHSignature33>(*f));
        truncated->points.resize(f->size() / 2);
        truncated->width = (uint32_t)truncated->points.size();
        truncated->height = 1;
        check(!CP::registFeatureMatching(a, a, truncated, f, 1.0, 1.0, T),
              "registFeatureMatching(特征数不匹配) → false");

        // 崩溃回归：FPFH 对找不到邻居的点输出全 NaN 直方图（PCL fpfh.hpp:250），
        // NaN 特征会让 SAC-IA 段错误（实测）。入口必须拒绝。
        auto nanf = pcl::PointCloud<pcl::FPFHSignature33>::Ptr(
            new pcl::PointCloud<pcl::FPFHSignature33>(*f));
        for (size_t i = 0; i < nanf->size(); i += 3)
            for (int d = 0; d < 33; ++d)
                nanf->points[i].histogram[d] = std::numeric_limits<float>::quiet_NaN();
        check(!CP::registFeatureMatching(a, a, nanf, f, 1.0, 1.0, T),
              "registFeatureMatching(特征含 NaN) → false 不崩溃");
        check(!CP::registFeatureMatching(a, a, f, nanf, 1.0, 1.0, T),
              "registFeatureMatching(target 特征含 NaN) → false");
    }

    // Poisson：法线数不匹配 → 无效
    {
        CloudPtr a(new Cloud(*sphere));
        auto n = CP::featureNormalEstimation(a, 20);
        auto truncated = pcl::PointCloud<pcl::Normal>::Ptr(new pcl::PointCloud<pcl::Normal>(*n));
        truncated->points.resize(n->size() / 2);
        truncated->width = (uint32_t)truncated->points.size();
        truncated->height = 1;
        check(!CP::surfacePoisson(a, truncated, 6, 5, 1.0).valid,
              "surfacePoisson(法线数不匹配) → 无效");
        check(!CP::surfaceGreedyProjection(a, truncated, 2.0, 2.5, 50).valid,
              "surfaceGreedyProjection(法线数不匹配) → 无效");
        check(!CP::surfaceMarchingCubes(a, truncated, 32).valid,
              "surfaceMarchingCubes(法线数不匹配) → 无效");
    }

    // T24 补强①：fitPlane 方法变体——头文件注明平面模型 RANSAC/LMEDS/MSAC 全支持，
    // 此前只测过 RANSAC；另补空云契约（头文件"src 为空返回 false"）
    {
        CloudPtr plane(new Cloud);
        for (int x = 0; x < 10; ++x)
            for (int y = 0; y < 10; ++y)
                plane->push_back(pcl::PointXYZ((float)x, (float)y, 0.0f));
        plane->width = (uint32_t)plane->size();
        plane->height = 1;
        std::vector<float> coeffs;
        std::vector<int> inliers;
        check(CP::fitPlane(plane, shapely::FitMethod::LMEDS, 0.01, coeffs, inliers) &&
              inliers.size() == 100, "fitPlane LMEDS 平面 → 100 局内点");
        check(CP::fitPlane(plane, shapely::FitMethod::MSAC, 0.01, coeffs, inliers) &&
              inliers.size() == 100, "fitPlane MSAC 平面 → 100 局内点");
        std::printf("[probe] msac in=%d coeff=(%.3f,%.3f,%.3f,%.3f)\n", (int)inliers.size(),
                    coeffs.size() ? coeffs[0] : -1.f, coeffs.size() ? coeffs[1] : -1.f,
                    coeffs.size() ? coeffs[2] : -1.f, coeffs.size() ? coeffs[3] : -1.f);
        std::printf("[probe] msac ret in=%d coeffs=(%.3f,%.3f,%.3f,%.3f)\n",
                    (int)inliers.size(), coeffs.size() ? coeffs[0] : -1.f,
                    coeffs.size() ? coeffs[1] : -1.f, coeffs.size() ? coeffs[2] : -1.f,
                    coeffs.size() ? coeffs[3] : -1.f);
        std::vector<float> c0;
        std::vector<int> i0;
        check(!CP::fitPlane(CloudPtr(), shapely::FitMethod::RANSAC, 0.01, c0, i0),
              "fitPlane(空云) → false");
    }

    // T24 补强②：registICP_PointToPoint 的 init_transform 初值路径（第 5 参此前从未传过）：
    // 头文件注明"配准是局部优化，初值越接近真值越可靠"
    {
        CloudPtr grid(new Cloud);
        for (int x = 0; x < 10; ++x)
            for (int y = 0; y < 10; ++y)
                grid->push_back(pcl::PointXYZ((float)x, (float)y, 0.0f));
        grid->width = (uint32_t)grid->size();
        grid->height = 1;
        float T[16] = { 0 };
        // a) 初值 = 精确真值：源云由 grid 平移 (+2,+1,+3) 生成，
        //    p_target = T·p_source ⇒ 真值 T = 逆变换 = 平移 (-2,-1,-3)
        CloudPtr shifted = CP::transTranslate(grid, 2.0, 1.0, 3.0);
        const float T_true[16] = { 1, 0, 0, -2.0f, 0, 1, 0, -1.0f, 0, 0, 1, -3.0f, 0, 0, 0, 1 };
        check(CP::registICP_PointToPoint(shifted, grid, 5.0, T, T_true) &&
              near(T[3], -2.0, 1e-3) && near(T[7], -1.0, 1e-3) && near(T[11], -3.0, 1e-3),
              "registICP_PointToPoint(init=精确真值) 平移恢复");
        // b) 大初始偏差（源云 = grid 绕 z 转 +90° 后平移 (15,8,4)）：
        //    真值 T = 逆变换：旋转块 R⁻¹（绕 z -90°）+ 平移 -R⁻¹·(15,8,4) = (-8,15,-4)
        CloudPtr rot = CP::transRotate(grid, 0.0, 0.0, 1.0, M_PI / 2.0);
        rot = CP::transTranslate(rot, 15.0, 8.0, 4.0);
        const float T_truth[16] = { 0, 1, 0, -8.0f, -1, 0, 0, 15.0f, 0, 0, 1, -4.0f, 0, 0, 0, 1 };
        const bool ok = CP::registICP_PointToPoint(rot, grid, 3.0, T, T_truth);
        bool finite = ok;
        if (finite)
            for (int i = 0; i < 16; ++i)
                if (!std::isfinite(T[i]))
                    finite = false;
        check(finite && near(T[3], -8.0, 0.1) && near(T[7], 15.0, 0.1) && near(T[11], -4.0, 0.1),
              "registICP_PointToPoint(init=真值) 90° 旋转大偏差收敛恢复");
        // c) 同数据传单位初值：旋转 + z 向错位 4 > max_dist=3 无对应点，
        //    不可能恢复真值 T[3]=-8（初值价值）
        float T_ident[16] = { 0 };
        T_ident[0] = T_ident[5] = T_ident[10] = T_ident[15] = 1.0f;
        float T_out2[16] = { 0 };
        const bool ok2 = CP::registICP_PointToPoint(rot, grid, 3.0, T_out2, T_ident);
        check(!ok2 || std::fabs(T_out2[3] + 8.0) > 1.0,
              "registICP_PointToPoint(init=单位阵) 不恢复真值（初值价值）");
    }

    // T24 补强③：registInformationMatrix 正路径——现有测试只覆盖"无对应点 → false"
    {
        CloudPtr grid(new Cloud);
        for (int x = 0; x < 5; ++x)
            for (int y = 0; y < 5; ++y)
                grid->push_back(pcl::PointXYZ((float)x, (float)y, 0.0f));
        grid->width = (uint32_t)grid->size();
        grid->height = 1;
        const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        double info[36];
        check(CP::registInformationMatrix(grid, grid, I, 2.0, info) &&
              std::fabs(info[21]) >= 1.0 && std::fabs(info[28]) >= 1.0 && std::fabs(info[35]) >= 1.0,
              "registInformationMatrix 恒等对齐 → 平移块主对角非零");
    }

    // T24 补强④：registPCA 旋转恢复——现有测试只测过纯平移。
    // 主轴符号有 180° 翻转自由度（头文件"对称物体的主轴可能翻转 180°"），
    // 故只断言轴映射结构（旋转块零元位置/行范数）与 ẑ 轴不动，不锁定符号
    {
        CloudPtr blob(new Cloud);
        for (int x = 0; x < 9; ++x)
            for (int y = 0; y < 5; ++y)
                for (int z = 0; z < 3; ++z)
                    blob->push_back(pcl::PointXYZ((float)x, (float)y * 2.5f, (float)z * 3.0f));
        blob->width = (uint32_t)blob->size();
        blob->height = 1;
        // 绕 z 轴 90° 刚体旋转：(x,y,z) → (-y,x,z)，再平移使两云质心不同
        CloudPtr rot90(new Cloud);
        for (const auto& p : blob->points)
            rot90->push_back(pcl::PointXYZ(-p.y + 7.5f, p.x + 2.0f, p.z));
        rot90->width = (uint32_t)rot90->size();
        rot90->height = 1;
        float T[16] = { 0 };
        check(CP::registPCA(rot90, blob, T) &&
              near(T[2], 0.0, 1e-3) && near(T[6], 0.0, 1e-3) &&
              near(T[8], 0.0, 1e-3) && near(T[9], 0.0, 1e-3) &&
              near(std::fabs(T[10]), 1.0, 1e-3) &&
              near(std::fabs(T[0]) + std::fabs(T[1]), 1.0, 1e-3) &&
              near(std::fabs(T[4]) + std::fabs(T[5]), 1.0, 1e-3),
              "registPCA 主轴 90° 旋转轴映射正确（符号无关）");
    }
}

// ---------------- CloudProcessor 三白名单点型（模板实例化 + 泛型路径） ----------------

// 白名单 PointXYZ / PointXYZI / PointXYZRGB 必须都能编译链接并得到一致的几何结果。
template <typename PointT>
static void testPointTypeInstance(const char* type_name)
{
    using CP = shapely::CloudProcessor<PointT>;
    using Cloud = pcl::PointCloud<PointT>;
    using CloudPtr = typename Cloud::Ptr;

    const float kOrigin[3] = { 0.0f, 0.0f, 0.0f };
    const float kCorner[3] = { 2.0f, 2.0f, 1.0f };
    const float kUp[3] = { 0.0f, 0.0f, 1.0f };

    CloudPtr cloud(new Cloud);
    for (int x = 0; x < 6; ++x)
        for (int y = 0; y < 6; ++y)
            for (int z = 0; z < 3; ++z) {
                PointT p;
                p.x = (float)x;
                p.y = (float)y;
                p.z = (float)z;
                cloud->push_back(p);
            }
    cloud->width = (uint32_t)cloud->size();
    cloud->height = 1;
    cloud->is_dense = true;

    char buf[160];

    // filter
    CloudPtr voxel = CP::filterVoxelGrid(cloud, 2.0);
    std::snprintf(buf, sizeof(buf), "[%s] filterVoxelGrid", type_name);
    check(voxel && !voxel->empty() && voxel->size() < cloud->size(), buf);

    CloudPtr crop = CP::extractCropBox(cloud, kOrigin, kCorner);
    std::snprintf(buf, sizeof(buf), "[%s] extractCropBox", type_name);
    check(crop && crop->size() == 18, buf);

    CloudPtr moved = CP::transTranslate(cloud, 1.0, 2.0, 3.0);
    std::snprintf(buf, sizeof(buf), "[%s] transTranslate", type_name);
    check(moved && near((*moved)[0].x, 1.0) && near((*moved)[0].y, 2.0) && near((*moved)[0].z, 3.0), buf);

    // feature：法线 / 朝向 / 协方差
    auto normals = CP::featureNormalEstimation(cloud, 10);
    std::snprintf(buf, sizeof(buf), "[%s] featureNormalEstimation", type_name);
    check(normals && normals->size() == cloud->size(), buf);

    auto oriented = CP::featureOrientNormalsToDirection(normals, kUp);
    std::snprintf(buf, sizeof(buf), "[%s] featureOrientNormalsToDirection", type_name);
    check(oriented && oriented->size() == cloud->size(), buf);

    std::vector<std::array<float, 9>> covs;
    std::snprintf(buf, sizeof(buf), "[%s] featureCovariances", type_name);
    check(CP::featureCovariances(cloud, 10, covs) && covs.size() == cloud->size(), buf);

    std::vector<double> md;
    std::snprintf(buf, sizeof(buf), "[%s] featureMahalanobisDistance", type_name);
    check(CP::featureMahalanobisDistance(cloud, md) && md.size() == cloud->size(), buf);

    // segment
    std::vector<int> labels;
    const int nclusters = CP::segmentDBSCAN(cloud, 1.5, 5, labels);
    std::snprintf(buf, sizeof(buf), "[%s] segmentDBSCAN", type_name);
    check(nclusters >= 1 && labels.size() == cloud->size(), buf);

    // regist
    CloudPtr shifted = CP::transTranslate(cloud, 0.5, 0.0, 0.0);
    float T[16] = { 0 };
    std::snprintf(buf, sizeof(buf), "[%s] registICP_PointToPoint", type_name);
    check(CP::registICP_PointToPoint(shifted, cloud, 2.0, T) && near(T[3], -0.5, 0.05), buf);

    // surface
    CloudPtr hull = CP::surfaceConvexHull(cloud);
    std::snprintf(buf, sizeof(buf), "[%s] surfaceConvexHull", type_name);
    check(hull && !hull->empty(), buf);

    auto smooth = CP::surfaceSmoothMLS(cloud, 2.0, 2, 0.0, nullptr);
    std::snprintf(buf, sizeof(buf), "[%s] surfaceSmoothMLS", type_name);
    check(smooth && smooth->size() == cloud->size(), buf);

    // measure
    const shapely::AABB3d aabb = CP::measureAABB(cloud);
    std::snprintf(buf, sizeof(buf), "[%s] measureAABB", type_name);
    check(aabb.valid && near(aabb.min_x, 0.0) && near(aabb.max_x, 5.0), buf);

    const shapely::OBB3d obb = CP::measureOBB(cloud);
    std::snprintf(buf, sizeof(buf), "[%s] measureOBB", type_name);
    check(obb.valid && near(obb.extents[0], 2.5, 1e-3), buf);

    std::vector<float> d = CP::measurePointCloudDistance(shifted, cloud);
    std::snprintf(buf, sizeof(buf), "[%s] measurePointCloudDistance", type_name);
    check(d.size() == cloud->size(), buf);

    // keypoint（原白名单路径）
    CloudPtr harris = CP::keypointHarris3D(cloud, 1.0, 1e-4);
    std::snprintf(buf, sizeof(buf), "[%s] keypointHarris3D", type_name);
    check(harris != nullptr, buf);
}

static void testCloudProcessorPointTypes()
{
    std::printf("== CloudProcessor 三白名单点型 ==\n");
    testPointTypeInstance<pcl::PointXYZ>("PointXYZ");
    testPointTypeInstance<pcl::PointXYZI>("PointXYZI");
    testPointTypeInstance<pcl::PointXYZRGB>("PointXYZRGB");
}

// ---------------- CloudProcessor 边界条件（小样本 / 退化 / 极值） ----------------

static void testCloudProcessorEdgeCases()
{
    std::printf("== CloudProcessor 边界条件 ==\n");
    using CP = shapely::CloudProcessor<pcl::PointXYZ>;
    using Cloud = pcl::PointCloud<pcl::PointXYZ>;
    using CloudPtr = Cloud::Ptr;

    // 1) 单点云：所有"需要邻域"的算子都不能崩
    CloudPtr one(new Cloud);
    one->push_back(pcl::PointXYZ(1.0f, 2.0f, 3.0f));
    one->width = 1;
    one->height = 1;
    check(CP::measureAABB(one).valid && near(CP::measureAABB(one).min_x, 1.0),
          "单点 measureAABB 有效");
    check(!CP::measureOBB(one).valid, "单点 measureOBB 无效（PCA 需 ≥3 点）");
    {
        auto n = CP::featureNormalEstimation(one, 10);
        check(n == nullptr || n->size() == 1, "单点法线估计不崩溃");
    }
    {
        std::vector<int> labels;
        CP::segmentDBSCAN(one, 1.0, 2, labels);   // 不崩溃即可
        check(labels.size() == 1, "单点 DBSCAN 标签长度 1");
    }

    // 2) 共线点云：OBB 退化但不应崩；平面拟合仍可给出方向
    CloudPtr line(new Cloud);
    for (int i = 0; i < 20; ++i)
        line->push_back(pcl::PointXYZ((float)i, 0.0f, 0.0f));
    line->width = 20;
    line->height = 1;
    {
        shapely::OBB3d obb = CP::measureOBB(line);
        check(obb.valid && near(obb.extents[0], 9.5, 1e-3), "共线点 OBB 长半轴 9.5");
        check(CP::surfaceConvexHull(line) != nullptr, "共线点凸包不崩溃");
    }

    // 3) 全同点（20 个完全相同）：去重 / 聚类 / 法线退化
    CloudPtr same(new Cloud);
    for (int i = 0; i < 20; ++i)
        same->push_back(pcl::PointXYZ(5.0f, 5.0f, 5.0f));
    same->width = 20;
    same->height = 1;
    {
        CloudPtr uniq = CP::filterRemoveDuplicates(same, 0.0);
        check(uniq && uniq->size() == 1, "全同点精确去重 → 1");
        CloudPtr uniqTol = CP::filterRemoveDuplicates(same, 1e-3);
        check(uniqTol && uniqTol->size() == 1, "全同点容差去重 → 1");
        std::vector<int> labels;
        const int nc = CP::segmentDBSCAN(same, 1.0, 3, labels);
        check(nc == 1 && labels.size() == 20, "全同点 DBSCAN → 1 簇");
        // 全同点方差为 0：马氏距离协方差奇异，加正则后应仍返回有限值
        std::vector<double> md;
        const bool ok = CP::featureMahalanobisDistance(same, md);
        bool finite = ok && md.size() == 20;
        if (finite)
            for (double v : md)
                if (!std::isfinite(v))
                    finite = false;
        check(finite, "全同点马氏距离有限（协方差正则）");
    }

    // 4) 含 NaN/Inf 输入：清洗后参与后续算子
    {
        CloudPtr dirty(new Cloud);
        for (int x = 0; x < 5; ++x)
            for (int y = 0; y < 5; ++y)
                dirty->push_back(pcl::PointXYZ((float)x, (float)y, 0.0f));
        dirty->push_back(pcl::PointXYZ(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f));
        dirty->push_back(pcl::PointXYZ(0.0f, std::numeric_limits<float>::infinity(), 0.0f));
        dirty->width = (uint32_t)dirty->size();
        dirty->height = 1;
        dirty->is_dense = false;
        CloudPtr clean = CP::filterRemoveNonFinite(dirty);
        check(clean && clean->size() == 25, "脏云清洗 → 25");
        CloudPtr down = CP::filterVoxelGrid(clean, 1.0);
        check(down && !down->empty(), "清洗后体素下采样可运行");
    }

    // 5) 采样请求超过点数：返回全量而非崩溃
    {
        CloudPtr g(new Cloud);
        for (int i = 0; i < 10; ++i)
            g->push_back(pcl::PointXYZ((float)i, 0.0f, 0.0f));
        g->width = 10;
        g->height = 1;
        CloudPtr r = CP::filterRandomSample(g, 100);
        check(r && r->size() == 10, "filterRandomSample(超额) → 全量 10");
        CloudPtr f = CP::filterFarthestPointSample(g, 100);
        check(f && f->size() == 10, "filterFarthestPointSample(超额) → 全量 10");
        CloudPtr u = CP::filterUniformSample(g, 100);
        check(u && u->size() == 1, "filterUniformSample(k>n) → 1 点");
    }

    // 6) 空点云参与度量/重建：返回空而非崩溃
    {
        CloudPtr empty(new Cloud);
        empty->width = 0;
        empty->height = 1;
        check(CP::surfaceConvexHull(empty) == nullptr, "空云凸包 → nullptr");
        check(CP::surfaceSmoothMLS(empty, 1.0) == nullptr, "空云 MLS → nullptr");
        check(CP::measurePointCloudDistance(empty, empty).empty(), "空云点云距离 → 空");
    }

    // 6b) 有序点云三角化（OrganizedFastMesh）：height>1 才有效
    {
        // 构造 8x8 有序栅格（模拟深度相机输出）
        // 注意：OrganizedFastMesh 的 isShadowed 用视点(默认原点)做遮挡剔除，
        // 因此点云放在 +z 方向、视点在原点前方才符合真实相机几何。
        CloudPtr organized(new Cloud);
        organized->width = 8;
        organized->height = 8;
        organized->resize(64);
        for (int v = 0; v < 8; ++v)
            for (int u = 0; u < 8; ++u) {
                pcl::PointXYZ& p = (*organized)[(size_t)(v * 8 + u)];
                p.x = (float)(u - 4) * 0.1f;
                p.y = (float)(v - 4) * 0.1f;
                p.z = 1.0f + 0.01f * (float)u;   // 视点前方 ~1m，轻微深度变化
            }
        organized->is_dense = true;

        shapely::Mesh3d ofm = CP::surfaceOrganizedFastMesh(organized, 1, true);
        check(ofm.valid && ofm.vertices.size() == 64 && !ofm.triangles.empty(),
              "surfaceOrganizedFastMesh 8x8 栅格 → 64 顶点 + 三角面");
        // 步长 2：采样四边形变稀疏。PCL 的自适应切割会按 z 差异剔除退化/阴影三角形，
        // 面数不一定线性减少，只要求有效且不增加。
        shapely::Mesh3d ofm2 = CP::surfaceOrganizedFastMesh(organized, 2, true);
        check(ofm2.valid && !ofm2.triangles.empty() &&
              ofm2.triangles.size() <= ofm.triangles.size(),
              "surfaceOrganizedFastMesh 步长 2 → 有效且面数不增加");
        // 固定切割模式（非自适应）不剔除退化三角形 → 每个有效四边形 2 个三角形
        shapely::Mesh3d ofmFixed = CP::surfaceOrganizedFastMesh(organized, 1, false);
        check(ofmFixed.valid && ofmFixed.triangles.size() == 2 * 7 * 7,
              "surfaceOrganizedFastMesh 固定切割 → 98 三角形");

        // 无序点云（height=1）→ 无效
        CloudPtr unordered(new Cloud);
        for (int i = 0; i < 20; ++i)
            unordered->push_back(pcl::PointXYZ((float)i, 0.0f, 0.0f));
        unordered->width = 20;
        unordered->height = 1;
        check(!CP::surfaceOrganizedFastMesh(unordered, 1, true).valid,
              "surfaceOrganizedFastMesh 无序点云 → 无效");
        check(!CP::surfaceOrganizedFastMesh(organized, 0, true).valid,
              "surfaceOrganizedFastMesh(step=0) → 无效");
    }

    // 7) OBB 裁剪：盒外点全部剔除、盒内点全部保留（与 AABB 交叉验证）
    {
        CloudPtr box(new Cloud);
        for (int x = 0; x < 5; ++x)
            for (int y = 0; y < 5; ++y)
                for (int z = 0; z < 5; ++z)
                    box->push_back(pcl::PointXYZ((float)x, (float)y, (float)z));
        box->width = (uint32_t)box->size();
        box->height = 1;
        shapely::OBB3d obb = CP::measureOBB(box);
        CloudPtr inside = CP::extractCropOBB(box, obb, false);
        CloudPtr outside = CP::extractCropOBB(box, obb, true);
        check(inside && inside->size() == box->size() && outside && outside->empty(),
              "OBB 裁剪：全部点在自身盒内");
    }

    // 8) 信息矩阵：对应点不足（间距 > max_dist）→ false
    {
        CloudPtr a(new Cloud), b(new Cloud);
        for (int i = 0; i < 10; ++i) {
            a->push_back(pcl::PointXYZ((float)i, 0.0f, 0.0f));
            b->push_back(pcl::PointXYZ((float)i + 100.0f, 0.0f, 0.0f));   // 相距 100
        }
        a->width = b->width = 10;
        a->height = b->height = 1;
        const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        double info[36];
        check(!CP::registInformationMatrix(a, b, I, 1.0, info),
              "registInformationMatrix 无对应点 → false");
        double fitness = 1.0, rmse = 1.0;
        check(CP::registEvaluate(a, b, I, 1.0, fitness, rmse) &&
              near(fitness, 0.0) && near(rmse, 0.0),
              "registEvaluate 无对应点 → fitness=0/rmse=0");
    }

    // 9) 缩放到中心：中心点不动、对称点等距
    {
        CloudPtr p(new Cloud);
        p->push_back(pcl::PointXYZ(-2.0f, 0.0f, 0.0f));
        p->push_back(pcl::PointXYZ(0.0f, 0.0f, 0.0f));
        p->push_back(pcl::PointXYZ(2.0f, 0.0f, 0.0f));
        p->width = 3;
        p->height = 1;
        const double s[3] = { 3.0, 3.0, 3.0 };
        const float c[3] = { 0.0f, 0.0f, 0.0f };
        CloudPtr out = CP::transScaleAround(p, s, c);
        check(out && near((*out)[1].x, 0.0) && near((*out)[0].x, -6.0) && near((*out)[2].x, 6.0),
              "transScaleAround 中心不动 / 端点 ×3");
    }

    // 10) 法线全局一致化：两个不连通分量各自内部一致（不要求跨分量同向）
    {
        CloudPtr two(new Cloud);
        for (int i = 0; i < 6; ++i) {
            two->push_back(pcl::PointXYZ((float)i, 0.0f, 0.0f));          // 分量 A
            two->push_back(pcl::PointXYZ((float)i + 100.0f, 0.0f, 0.0f)); // 分量 B
        }
        two->width = (uint32_t)two->size();
        two->height = 1;
        auto n = CP::featureNormalEstimation(two, 5);
        pcl::PointCloud<pcl::Normal>::Ptr scrambled(new pcl::PointCloud<pcl::Normal>(*n));
        for (size_t i = 0; i < scrambled->size(); ++i)
            if (i % 2 == 0)
                (*scrambled)[i].normal_z = -(*scrambled)[i].normal_z;
        auto fixed = CP::featureOrientNormalsConsistentTangentPlane(two, scrambled, 5);
        // 分量内一致：分量 A 前 6 个法线同号，分量 B 后 6 个同号
        bool a_consistent = fixed && fixed->size() == 12;
        bool b_consistent = a_consistent;
        if (a_consistent) {
            const float ref_a = (*fixed)[0].normal_z;
            const float ref_b = (*fixed)[6].normal_z;
            for (int i = 0; i < 6; ++i) {
                if ((*fixed)[(size_t)i].normal_z * ref_a < 0.0f)
                    a_consistent = false;
                if ((*fixed)[(size_t)(i + 6)].normal_z * ref_b < 0.0f)
                    b_consistent = false;
            }
        }
        check(a_consistent && b_consistent, "法线一致化：不连通分量各自内部一致");
    }

    // T24 补强⑤：离群滤波"真删"正路径——既有用例只在干净云上验证"不误删"，
    // 此处验证噪声点确实被剔除（SOR 按近邻平均距离、半径滤波按邻居数）
    {
        CloudPtr base(new Cloud);
        for (int x = 0; x < 10; ++x)
            for (int y = 0; y < 10; ++y)
                base->push_back(pcl::PointXYZ((float)x, (float)y, 0.0f));
        base->width = (uint32_t)base->size();
        base->height = 1;
        CloudPtr dirty(new Cloud(*base));
        dirty->push_back(pcl::PointXYZ(55.0f, 55.0f, 0.0f));   // 远离网格的孤立噪声点
        dirty->width = (uint32_t)dirty->size();
        dirty->height = 1;
        CloudPtr sor = CP::filterStatisticalOutlier(dirty, 10, 1.0);
        check(sor && sor->size() == 100, "filterStatisticalOutlier 剔除孤立噪声点 → 100");
        CloudPtr ror = CP::filterRadiusOutlier(dirty, 1.5, 3);
        check(ror && ror->size() == 100, "filterRadiusOutlier 剔除孤立噪声点 → 100");
    }

    // T24 补强⑥：表面重建对非平面几何——圆柱面（头文件注明重建入口自动
    // 剔除无效法线；有效法线占比 <50% 整体拒绝），互补既有的平面/球面用例
    {
        CloudPtr cyl(new Cloud);
        for (int k = 0; k < 8; ++k)
            for (int j = 0; j < 24; ++j) {
                const double a = 2.0 * M_PI * j / 24.0;
                cyl->push_back(pcl::PointXYZ((float)(5.0 + 2.0 * std::cos(a)),
                                             (float)(5.0 + 2.0 * std::sin(a)),
                                             (float)(k * 0.5)));
            }
        cyl->width = (uint32_t)cyl->size();
        cyl->height = 1;
        auto n = CP::featureNormalEstimation(cyl, 15);
        check(n && n->size() == cyl->size(), "圆柱面法线估计数量一致");
        shapely::Mesh3d mc = CP::surfaceMarchingCubes(cyl, n, 48);
        check(mc.valid && !mc.triangles.empty(),
              "surfaceMarchingCubes 圆柱面 → 有效网格");
        shapely::Mesh3d gpt = CP::surfaceGreedyProjection(cyl, n, 1.5, 2.5, 80);
        check(gpt.valid && !gpt.triangles.empty(),
              "surfaceGreedyProjection 圆柱面 → 有效网格");
    }
}

#endif // SHAPELYALG_HAS_PCL（点云用例段）

int main()
{
    testGeom();
    testStereo();
    testImageProcessor();
#ifdef SHAPELYALG_HAS_PCL
    testCloudProcessor();
    testCloudProcessorOpen3D();
    testCloudProcessorContracts();
    testCloudProcessorFitRegistVariants();
    testCloudProcessorPointTypes();
    testCloudProcessorEdgeCases();
#endif
    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
