// Geom.cpp —— Geom 底层 GEOS 运算实现（纯 double，Pimpl）。
// 线程安全：GEOS reentrant 上下文线程局部。
//
// GEOS 所有权铁律（踩坑记录）：
//   1. createLinearRing/createPolygon/createPoint 的 CoordSeq 所有权转移给几何，
//      仅创建失败才自己 destroy；
//   2. GEOSGetExteriorRing/GEOSGetGeometryN 返回内部指针禁止 destroy，
//      绝不能包进 RAII 包装（双重释放 0xc0000374）；
//   3. LinearRing 必须闭合（首尾点相同），否则 GEOS 堆损坏（不是返回 NULL）；
//   4. GEOSGeomGetPointN 只对 LineString 合法；
//   5. 空几何 distance 返回 inf（成功），判空用 GEOSisEmpty_r。

#include "Geom.h"

#include <geos_c.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace shapely {

// ---------------- GEOS 上下文（线程局部） ----------------

// 本线程最近一次 GEOS 诊断消息（lastError() 的存储；随上下文按线程隔离）
static thread_local std::string g_geomLastError;

static GEOSContextHandle_t ctx()
{
    thread_local GEOSContextHandle_t h = nullptr;
    if (!h) {
        h = GEOS_init_r();
        // 捕获本线程 GEOS 诊断消息（供 lastError() 区分"拓扑异常失败"与"真空结果"）
        GEOSContext_setErrorMessageHandler_r(
            h,
            [](const char* msg, void*) {
                if (msg)
                    g_geomLastError = msg;
            },
            nullptr);
    }
    return h;
}

// 构建失败自动回收的临时指针
struct GeosPtr
{
    GEOSGeometry* g = nullptr;
    explicit GeosPtr(GEOSGeometry* p) : g(p) {}
    ~GeosPtr() { if (g) GEOSGeom_destroy_r(ctx(), g); }
    GeosPtr(const GeosPtr&) = delete;
    GeosPtr& operator=(const GeosPtr&) = delete;
};

// ---------------- 生命周期 ----------------

Geom::Geom() {}
Geom::Geom(void* g) : geom_(g) {}

Geom::~Geom()
{
    if (geom_)
        GEOSGeom_destroy_r(ctx(), (GEOSGeometry*)geom_);
}

Geom::Geom(Geom&& other) noexcept : geom_(other.geom_) { other.geom_ = nullptr; }

Geom& Geom::operator=(Geom&& other) noexcept
{
    if (this != &other) {
        if (geom_)
            GEOSGeom_destroy_r(ctx(), (GEOSGeometry*)geom_);
        geom_ = other.geom_;
        other.geom_ = nullptr;
    }
    return *this;
}

// ---------------- 状态检查 ----------------

bool Geom::isEmpty() const
{
    return !geom_ || GEOSisEmpty_r(ctx(), (const GEOSGeometry*)geom_) == 1;
}

std::string Geom::wkt() const
{
    if (!geom_)
        return {};
    GEOSWKTWriter* w = GEOSWKTWriter_create_r(ctx());
    GEOSWKTWriter_setRoundingPrecision_r(ctx(), w, 15);
    char* s = GEOSWKTWriter_write_r(ctx(), w, (const GEOSGeometry*)geom_);
    GEOSWKTWriter_destroy_r(ctx(), w);
    std::string out = s ? s : "";
    if (s)
        GEOSFree_r(ctx(), s);
    return out;
}

std::string Geom::typeName() const
{
    if (!geom_)
        return "null";
    const char* s = GEOSGeomType_r(ctx(), (const GEOSGeometry*)geom_);
    return s ? s : "unknown";
}

// ---------------- 通用功能 ----------------

BoundingBox2d Geom::bounds() const
{
    BoundingBox2d bb;
    if (!geom_)
        return bb;
    if (GEOSGeom_getExtent_r(ctx(), (const GEOSGeometry*)geom_,
                             &bb.min_x, &bb.min_y, &bb.max_x, &bb.max_y) != 1)
        return BoundingBox2d();
    bb.valid = true;
    return bb;
}

// 读取单条坐标序列的全部顶点（只读内部指针，不构造 RAII 包装）
static void readCoordSeq(const GEOSContextHandle_t c, const GEOSCoordSequence* s,
                         std::vector<Point2d>& out)
{
    if (!s)
        return;
    unsigned n = 0;
    if (GEOSCoordSeq_getSize_r(c, s, &n) != 1)
        return;
    for (unsigned i = 0; i < n; ++i) {
        double x = 0, y = 0;
        if (GEOSCoordSeq_getX_r(c, s, i, &x) == 1 &&
            GEOSCoordSeq_getY_r(c, s, i, &y) == 1)
            out.push_back(Point2d{ x, y });
    }
}

// 按几何类型收集顶点：点/线读自身 CoordSeq，面读外壳 ring，
// Multi*/Collection 逐子几何递归展开（可嵌套）。
// 注意 Polygon 不能走 CoordSeq 分支——GEOSGeom_getCoordSeq 对面返回 NULL
//（回归 2026-11-09：MultiPolygon 曾因此静默返回空）。
static void appendVertices(const GEOSContextHandle_t c, const GEOSGeometry* g,
                           std::vector<Point2d>& out)
{
    const int type = GEOSGeomTypeId_r(c, g);
    if (type == GEOS_POLYGON) {
        const GEOSGeometry* shell = GEOSGetExteriorRing_r(c, g);   // 内部指针，禁止 destroy
        if (shell)
            readCoordSeq(c, GEOSGeom_getCoordSeq_r(c, shell), out);
    } else if (type == GEOS_POINT || type == GEOS_LINESTRING || type == GEOS_LINEARRING) {
        readCoordSeq(c, GEOSGeom_getCoordSeq_r(c, g), out);
    } else {
        // GeometryCollection / Multi*：递归展开
        const int n = GEOSGetNumGeometries_r(c, g);
        for (int i = 0; i < n; ++i) {
            const GEOSGeometry* sub = GEOSGetGeometryN_r(c, g, i);
            if (sub)
                appendVertices(c, sub, out);
        }
    }
}

std::vector<Point2d> Geom::getExteriorPoints() const
{
    std::vector<Point2d> out;
    if (!geom_)
        return out;
    appendVertices(ctx(), (const GEOSGeometry*)geom_, out);
    return out;
}

double Geom::distanceTo(const Geom& other) const
{
    if (!geom_ || !other.geom_)
        return -1.0;
    // GEOS 空几何（如 fromWKT("POINT EMPTY")）返回 inf——按头文件契约归一为 -1
    if (isEmpty() || other.isEmpty())
        return -1.0;
    double d = 0;
    if (GEOSDistance_r(ctx(), (const GEOSGeometry*)geom_,
                       (const GEOSGeometry*)other.geom_, &d) != 1)
        return -1.0;
    return d;
}

bool Geom::nearestPoints(const Geom& other, Point2d& out_a, Point2d& out_b) const
{
    out_a = Point2d();
    out_b = Point2d();
    if (!geom_ || !other.geom_)
        return false;
    // 返回含 2 个坐标的 CoordSeq（本几何点 + 对方点），所有权归调用方
    GEOSCoordSequence* s = GEOSNearestPoints_r(ctx(), (const GEOSGeometry*)geom_,
                                               (const GEOSGeometry*)other.geom_);
    if (!s)
        return false;
    double x = 0, y = 0;
    const bool okA = GEOSCoordSeq_getX_r(ctx(), s, 0, &x) == 1 &&
                     GEOSCoordSeq_getY_r(ctx(), s, 0, &y) == 1;
    out_a = Point2d{ x, y };
    const bool okB = GEOSCoordSeq_getX_r(ctx(), s, 1, &x) == 1 &&
                     GEOSCoordSeq_getY_r(ctx(), s, 1, &y) == 1;
    out_b = Point2d{ x, y };
    GEOSCoordSeq_destroy_r(ctx(), s);
    return okA && okB;
}

bool Geom::isWithinDistance(const Geom& other, double dist) const
{
    if (!geom_ || !other.geom_)
        return false;
    // 与 distanceTo 同口径：GEOS 空几何视为"无距离"→ false
    if (isEmpty() || other.isEmpty())
        return false;
    return GEOSDistanceWithin_r(ctx(), (const GEOSGeometry*)geom_,
                                (const GEOSGeometry*)other.geom_, dist) == 1;
}

std::string Geom::isValidReason() const
{
    if (!geom_)
        return "null geometry";
    // GEOS 返回堆字符串，须 GEOSFree 释放
    char* r = GEOSisValidReason_r(ctx(), (const GEOSGeometry*)geom_);
    if (!r)
        return std::string();
    std::string out(r);
    GEOSFree_r(ctx(), r);
    return out;
}

std::string Geom::lastError()
{
    return g_geomLastError;
}

Geom Geom::makeValid() const
{
    g_geomLastError.clear();   // 入口清空：运算后 lastError 非空 = 本次失败
    if (!geom_)
        return Geom();
    return Geom(GEOSMakeValid_r(ctx(), (const GEOSGeometry*)geom_));
}

// ---------------- 2D 仿射变换（递归重建，保持 Multi/孔洞/嵌套结构） ----------------

namespace {

// 变换单条坐标序列（新建 CoordSeq，所有权归调用方；失败返回 nullptr）
GEOSCoordSequence* transformCoordSeq(const GEOSContextHandle_t h, const GEOSCoordSequence* s,
                                     const double m[6])
{
    unsigned n = 0;
    if (GEOSCoordSeq_getSize_r(h, s, &n) != 1)
        return nullptr;
    GEOSCoordSequence* out = GEOSCoordSeq_create_r(h, n, 2);
    if (!out)
        return nullptr;
    for (unsigned i = 0; i < n; ++i) {
        double x = 0, y = 0;
        if (GEOSCoordSeq_getX_r(h, s, i, &x) != 1 ||
            GEOSCoordSeq_getY_r(h, s, i, &y) != 1) {
            GEOSCoordSeq_destroy_r(h, out);
            return nullptr;
        }
        GEOSCoordSeq_setX_r(h, out, i, m[0] * x + m[1] * y + m[2]);
        GEOSCoordSeq_setY_r(h, out, i, m[3] * x + m[4] * y + m[5]);
    }
    return out;
}

// 单条 ring/线 → 变换后的同类型几何（接管 CoordSeq；失败返回 nullptr）
GEOSGeometry* transformRingLike(const GEOSContextHandle_t h, const GEOSGeometry* g,
                                const double m[6], bool asRing)
{
    const GEOSCoordSequence* s = GEOSGeom_getCoordSeq_r(h, g);
    if (!s)
        return nullptr;
    GEOSCoordSequence* t = transformCoordSeq(h, s, m);
    if (!t)
        return nullptr;
    GEOSGeometry* out = asRing ? GEOSGeom_createLinearRing_r(h, t)
                               : GEOSGeom_createLineString_r(h, t);
    if (!out)
        GEOSCoordSeq_destroy_r(h, t);
    return out;
}

// 递归变换：Point/LineString/LinearRing/Polygon 直接重建，Multi*/Collection 递归
GEOSGeometry* transformRec(const GEOSContextHandle_t h, const GEOSGeometry* g,
                           const double m[6])
{
    const int type = GEOSGeomTypeId_r(h, g);
    switch (type) {
    case GEOS_POINT: {
        const GEOSCoordSequence* s = GEOSGeom_getCoordSeq_r(h, g);
        if (!s)
            return nullptr;
        GEOSCoordSequence* t = transformCoordSeq(h, s, m);
        if (!t)
            return nullptr;
        GEOSGeometry* out = GEOSGeom_createPoint_r(h, t);
        if (!out)
            GEOSCoordSeq_destroy_r(h, t);
        return out;
    }
    case GEOS_LINESTRING:
    case GEOS_LINEARRING:
        return transformRingLike(h, g, m, type == GEOS_LINEARRING);
    case GEOS_POLYGON: {
        // 外壳 + 全部内环重建（createPolygon 成功时接管全部所有权）
        const GEOSGeometry* shell = GEOSGetExteriorRing_r(h, g);
        if (!shell)
            return nullptr;
        GEOSGeometry* newShell = transformRingLike(h, shell, m, true);
        if (!newShell)
            return nullptr;
        const int nHoles = GEOSGetNumInteriorRings_r(h, g);
        std::vector<GEOSGeometry*> holes;
        for (int i = 0; i < nHoles; ++i) {
            const GEOSGeometry* hole = GEOSGetInteriorRingN_r(h, g, i);
            if (!hole)
                continue;
            GEOSGeometry* nh = transformRingLike(h, hole, m, true);
            if (nh)
                holes.push_back(nh);
        }
        GEOSGeometry* out = GEOSGeom_createPolygon_r(
            h, newShell, holes.empty() ? nullptr : holes.data(), (unsigned)holes.size());
        if (!out) {
            GEOSGeom_destroy_r(h, newShell);
            for (GEOSGeometry* ring : holes)
                GEOSGeom_destroy_r(h, ring);
            return nullptr;
        }
        return out;
    }
    default: {
        // Multi*/Collection：递归子几何（保持 Multi 类型不变）
        const int n = GEOSGetNumGeometries_r(h, g);
        std::vector<GEOSGeometry*> subs;
        for (int i = 0; i < n; ++i) {
            const GEOSGeometry* sub = GEOSGetGeometryN_r(h, g, i);
            if (!sub)
                continue;
            GEOSGeometry* ns = transformRec(h, sub, m);
            if (ns)
                subs.push_back(ns);
        }
        if (subs.empty())
            return nullptr;
        const int outType = (type == GEOS_MULTIPOINT || type == GEOS_MULTILINESTRING ||
                             type == GEOS_MULTIPOLYGON || type == GEOS_GEOMETRYCOLLECTION)
                                ? type
                                : GEOS_GEOMETRYCOLLECTION;
        GEOSGeometry* out = GEOSGeom_createCollection_r(h, outType,
                                                        subs.data(), (unsigned)subs.size());
        if (!out) {
            for (GEOSGeometry* p : subs)
                GEOSGeom_destroy_r(h, p);
            return nullptr;
        }
        return out;
    }
    }
}

} // namespace

Geom Geom::transformed(double a, double b, double tx, double c, double d, double ty) const
{
    if (!geom_)
        return Geom();
    const GEOSContextHandle_t h = ctx();
    const double m[6] = { a, b, tx, c, d, ty };
    return Geom(transformRec(h, (const GEOSGeometry*)geom_, m));
}

// ---------------- GeoJSON / DXF 导出（2D 坐标；空对象返回空串） ----------------

namespace {

// CoordSeq → JSON 坐标对数组 [[x,y],...]（17 位有效数字，double 往返无损）
std::string jsonCoordPairs(const GEOSContextHandle_t h, const GEOSCoordSequence* s)
{
    std::ostringstream out;
    out << std::setprecision(17);
    out << "[";
    if (s) {
        unsigned n = 0;
        if (GEOSCoordSeq_getSize_r(h, s, &n) == 1) {
            for (unsigned i = 0; i < n; ++i) {
                double x = 0, y = 0;
                if (GEOSCoordSeq_getX_r(h, s, i, &x) == 1 &&
                    GEOSCoordSeq_getY_r(h, s, i, &y) == 1) {
                    if (i)
                        out << ",";
                    out << "[" << x << "," << y << "]";
                }
            }
        }
    }
    out << "]";
    return out.str();
}

std::string jsonLineCoords(const GEOSContextHandle_t h, const GEOSGeometry* g)
{
    return jsonCoordPairs(h, GEOSGeom_getCoordSeq_r(h, g));
}

// 递归构造 GeoJSON 几何对象
std::string jsonRec(const GEOSContextHandle_t h, const GEOSGeometry* g)
{
    const int type = GEOSGeomTypeId_r(h, g);
    switch (type) {
    case GEOS_POINT: {
        const GEOSCoordSequence* s = GEOSGeom_getCoordSeq_r(h, g);
        double x = 0, y = 0;
        if (s)
            GEOSCoordSeq_getX_r(h, s, 0, &x);
        if (s)
            GEOSCoordSeq_getY_r(h, s, 0, &y);
        std::ostringstream out;
        out << std::setprecision(17);
        out << "{\"type\":\"Point\",\"coordinates\":[" << x << "," << y << "]}";
        return out.str();
    }
    case GEOS_LINESTRING:
    case GEOS_LINEARRING:
        return "{\"type\":\"LineString\",\"coordinates\":" + jsonLineCoords(h, g) + "}";
    case GEOS_POLYGON: {
        std::ostringstream out;
        out << "{\"type\":\"Polygon\",\"coordinates\":[";
        const GEOSGeometry* shell = GEOSGetExteriorRing_r(h, g);
        out << jsonCoordPairs(h, shell ? GEOSGeom_getCoordSeq_r(h, shell) : nullptr);
        const int nHoles = GEOSGetNumInteriorRings_r(h, g);
        for (int i = 0; i < nHoles; ++i) {
            const GEOSGeometry* hole = GEOSGetInteriorRingN_r(h, g, i);
            out << "," << jsonCoordPairs(h, hole ? GEOSGeom_getCoordSeq_r(h, hole) : nullptr);
        }
        out << "]}";
        return out.str();
    }
    case GEOS_MULTIPOINT:
    case GEOS_MULTILINESTRING:
    case GEOS_MULTIPOLYGON:
    case GEOS_GEOMETRYCOLLECTION: {
        static const char* kNames[] = { "MultiPoint", "MultiLineString",
                                        "MultiPolygon", "GeometryCollection" };
        const char* name = (type == GEOS_MULTIPOINT)  ? kNames[0]
                           : (type == GEOS_MULTILINESTRING) ? kNames[1]
                           : (type == GEOS_MULTIPOLYGON)    ? kNames[2]
                                                            : kNames[3];
        const bool collection = (type == GEOS_GEOMETRYCOLLECTION);
        const bool polygonized = (type == GEOS_MULTIPOLYGON);
        const bool lined = (type == GEOS_MULTILINESTRING);
        std::ostringstream out;
        out << "{\"type\":\"" << name << "\",\"";
        out << (collection ? "geometries" : "coordinates") << "\":[";
        const int n = GEOSGetNumGeometries_r(h, g);
        for (int i = 0; i < n; ++i) {
            const GEOSGeometry* sub = GEOSGetGeometryN_r(h, g, i);
            if (!sub)
                continue;
            if (i)
                out << ",";
            if (collection) {
                out << jsonRec(h, sub);
            } else if (type == GEOS_MULTIPOINT) {
                // MultiPoint 坐标平铺为 [ [x,y], ... ]
                const GEOSCoordSequence* s = GEOSGeom_getCoordSeq_r(h, sub);
                double x = 0, y = 0;
                if (s) {
                    GEOSCoordSeq_getX_r(h, s, 0, &x);
                    GEOSCoordSeq_getY_r(h, s, 0, &y);
                }
                out << "[" << x << "," << y << "]";
            } else if (lined) {
                out << jsonLineCoords(h, sub);
            } else if (polygonized) {
                // MultiPolygon 成员 = [ [shell, hole...] ]
                out << "[";
                const GEOSGeometry* shell = GEOSGetExteriorRing_r(h, sub);
                out << jsonCoordPairs(h, shell ? GEOSGeom_getCoordSeq_r(h, shell) : nullptr);
                const int nHoles = GEOSGetNumInteriorRings_r(h, sub);
                for (int k = 0; k < nHoles; ++k) {
                    const GEOSGeometry* hole = GEOSGetInteriorRingN_r(h, sub, k);
                    out << "," << jsonCoordPairs(h, hole ? GEOSGeom_getCoordSeq_r(h, hole)
                                                         : nullptr);
                }
                out << "]";
            }
        }
        out << "]}";
        return out.str();
    }
    default:
        return "{}";
    }
}

// DXF：单条 ring/line 的连续坐标 → LINE 实体段
void dxfLineSegments(std::ostringstream& out, const GEOSContextHandle_t h,
                     const GEOSGeometry* g)
{
    const GEOSCoordSequence* s = GEOSGeom_getCoordSeq_r(h, g);
    if (!s)
        return;
    unsigned n = 0;
    if (GEOSCoordSeq_getSize_r(h, s, &n) != 1)
        return;
    double px = 0, py = 0;
    for (unsigned i = 0; i < n; ++i) {
        double x = 0, y = 0;
        if (GEOSCoordSeq_getX_r(h, s, i, &x) != 1 ||
            GEOSCoordSeq_getY_r(h, s, i, &y) != 1)
            continue;
        if (i > 0)
            out << "0\nLINE\n8\n0\n10\n" << px << "\n20\n" << py << "\n30\n0\n"
                << "11\n" << x << "\n21\n" << y << "\n31\n0\n";
        px = x;
        py = y;
    }
}

void dxfRec(std::ostringstream& out, const GEOSContextHandle_t h, const GEOSGeometry* g)
{
    const int type = GEOSGeomTypeId_r(h, g);
    switch (type) {
    case GEOS_POINT: {
        const GEOSCoordSequence* s = GEOSGeom_getCoordSeq_r(h, g);
        double x = 0, y = 0;
        if (s) {
            GEOSCoordSeq_getX_r(h, s, 0, &x);
            GEOSCoordSeq_getY_r(h, s, 0, &y);
        }
        out << "0\nPOINT\n8\n0\n10\n" << x << "\n20\n" << y << "\n30\n0\n";
        return;
    }
    case GEOS_LINESTRING:
    case GEOS_LINEARRING:
        dxfLineSegments(out, h, g);
        return;
    case GEOS_POLYGON: {
        const GEOSGeometry* shell = GEOSGetExteriorRing_r(h, g);
        if (shell)
            dxfLineSegments(out, h, shell);
        const int nHoles = GEOSGetNumInteriorRings_r(h, g);
        for (int i = 0; i < nHoles; ++i) {
            const GEOSGeometry* hole = GEOSGetInteriorRingN_r(h, g, i);
            if (hole)
                dxfLineSegments(out, h, hole);
        }
        return;
    }
    default: {
        const int n = GEOSGetNumGeometries_r(h, g);
        for (int i = 0; i < n; ++i) {
            const GEOSGeometry* sub = GEOSGetGeometryN_r(h, g, i);
            if (sub)
                dxfRec(out, h, sub);
        }
        return;
    }
    }
}

} // namespace

std::string Geom::toGeoJSON() const
{
    if (!geom_)
        return std::string();
    return jsonRec(ctx(), (const GEOSGeometry*)geom_);
}

std::string Geom::toDXF() const
{
    if (!geom_)
        return std::string();
    std::ostringstream out;
    out << std::setprecision(17);
    out << "0\nSECTION\n2\nENTITIES\n";
    dxfRec(out, ctx(), (const GEOSGeometry*)geom_);
    out << "0\nENDSEC\n0\nEOF\n";
    return out.str();
}

bool Geom::intersects(const Geom& other) const
{
    if (!geom_ || !other.geom_)
        return false;
    return GEOSIntersects_r(ctx(), (const GEOSGeometry*)geom_,
                            (const GEOSGeometry*)other.geom_) == 1;
}

bool Geom::touches(const Geom& other) const
{
    if (!geom_ || !other.geom_)
        return false;
    return GEOSTouches_r(ctx(), (const GEOSGeometry*)geom_,
                         (const GEOSGeometry*)other.geom_) == 1;
}

std::vector<Geom> Geom::splitParts() const
{
    std::vector<Geom> out;
    if (!geom_)
        return out;
    const GEOSContextHandle_t c = ctx();
    const GEOSGeometry* g = (const GEOSGeometry*)geom_;
    const int type = GEOSGeomTypeId_r(c, g);

    // 单体（Point 0 / LineString 1 / LinearRing 2 / Polygon 3）→ 自身克隆
    if (type <= GEOS_POLYGON) {
        out.push_back(Geom(GEOSGeom_clone_r(c, g)));
        return out;
    }
    // Multi*/Collection → 逐子几何克隆展开（子指针归父所有，必须克隆）
    const int n = GEOSGetNumGeometries_r(c, g);
    for (int i = 0; i < n; ++i) {
        const GEOSGeometry* sub = GEOSGetGeometryN_r(c, g, i);
        if (sub)
            out.push_back(Geom(GEOSGeom_clone_r(c, sub)));
    }
    return out;
}

bool Geom::isValid() const
{
    if (!geom_)
        return false;
    return GEOSisValid_r(ctx(), (const GEOSGeometry*)geom_) == 1;
}

Geom Geom::simplify(double tolerance, bool keepTopology) const
{
    g_geomLastError.clear();   // 入口清空：运算后 lastError 非空 = 本次失败
    if (!geom_)
        return Geom();
    // 容差非正：原样返回克隆（本类型禁拷贝，走 GEOS 克隆）
    if (tolerance <= 0)
        return Geom(GEOSGeom_clone_r(ctx(), (const GEOSGeometry*)geom_));
    GEOSGeometry* g = keepTopology
        ? GEOSTopologyPreserveSimplify_r(ctx(), (const GEOSGeometry*)geom_, tolerance)
        : GEOSSimplify_r(ctx(), (const GEOSGeometry*)geom_, tolerance);
    return Geom(g);
}

// ---------------- 点专属 ----------------

double Geom::getX() const
{
    if (!geom_ || GEOSGeomTypeId_r(ctx(), (const GEOSGeometry*)geom_) != GEOS_POINT)
        return 0.0;   // 非点返回 0（头文件契约）
    double x = 0;
    if (GEOSGeom_getXMin_r(ctx(), (const GEOSGeometry*)geom_, &x) != 1)
        return 0.0;
    return x;
}

double Geom::getY() const
{
    if (!geom_ || GEOSGeomTypeId_r(ctx(), (const GEOSGeometry*)geom_) != GEOS_POINT)
        return 0.0;   // 非点返回 0（头文件契约）
    double y = 0;
    if (GEOSGeom_getYMin_r(ctx(), (const GEOSGeometry*)geom_, &y) != 1)
        return 0.0;
    return y;
}

bool Geom::within(const Geom& lineOrPolygon) const
{
    if (!geom_ || !lineOrPolygon.geom_)
        return false;
    return GEOSWithin_r(ctx(), (const GEOSGeometry*)geom_,
                        (const GEOSGeometry*)lineOrPolygon.geom_) == 1;
}

// ---------------- 线专属 ----------------

double Geom::length() const
{
    if (!geom_)
        return 0.0;
    double len = 0;
    if (GEOSLength_r(ctx(), (const GEOSGeometry*)geom_, &len) != 1)
        return 0.0;
    return len;
}

bool Geom::crosses(const Geom& other) const
{
    if (!geom_ || !other.geom_)
        return false;
    return GEOSCrosses_r(ctx(), (const GEOSGeometry*)geom_,
                         (const GEOSGeometry*)other.geom_) == 1;
}

// ---------------- 面专属 ----------------

double Geom::area() const
{
    if (!geom_)
        return 0.0;
    double a = 0;
    if (GEOSArea_r(ctx(), (const GEOSGeometry*)geom_, &a) != 1)
        return 0.0;
    return a;
}

Point2d Geom::centroid() const
{
    if (!geom_)
        return Point2d();
    // 契约：仅面（Polygon/MultiPolygon）给质心；点/线等非面返回 (0,0)
    //（回归 2026-11-09：此前无类型门，line.centroid() 返回了顶点平均）
    const int type = GEOSGeomTypeId_r(ctx(), (const GEOSGeometry*)geom_);
    if (type != GEOS_POLYGON && type != GEOS_MULTIPOLYGON)
        return Point2d();
    // 真质心（面积加权；凹多边形质心可能在面外）
    GeosPtr c(GEOSGetCentroid_r(ctx(), (const GEOSGeometry*)geom_));
    if (!c.g)
        return Point2d();
    double x = 0, y = 0;
    GEOSGeom_getXMin_r(ctx(), c.g, &x);
    GEOSGeom_getYMin_r(ctx(), c.g, &y);
    return Point2d{ x, y };
}

int Geom::numInteriorRings() const
{
    if (!geom_)
        return 0;
    const GEOSContextHandle_t c = ctx();
    if (GEOSGeomTypeId_r(c, (const GEOSGeometry*)geom_) != GEOS_POLYGON)
        return 0;
    return GEOSGetNumInteriorRings_r(c, (const GEOSGeometry*)geom_);
}

std::vector<std::vector<Point2d>> Geom::getInteriorRings() const
{
    std::vector<std::vector<Point2d>> out;
    if (!geom_)
        return out;
    const GEOSContextHandle_t c = ctx();
    const GEOSGeometry* g = (const GEOSGeometry*)geom_;
    if (GEOSGeomTypeId_r(c, g) != GEOS_POLYGON)
        return out;
    const int n = GEOSGetNumInteriorRings_r(c, g);
    out.reserve((size_t)(n > 0 ? n : 0));
    for (int i = 0; i < n; ++i) {
        const GEOSGeometry* ring = GEOSGetInteriorRingN_r(c, g, i);   // 内部指针，禁止 destroy
        if (!ring)
            continue;
        std::vector<Point2d> pts;
        readCoordSeq(c, GEOSGeom_getCoordSeq_r(c, ring), pts);
        out.push_back(std::move(pts));
    }
    return out;
}

bool Geom::contains(const Geom& other) const
{
    if (!geom_ || !other.geom_)
        return false;
    return GEOSContains_r(ctx(), (const GEOSGeometry*)geom_,
                          (const GEOSGeometry*)other.geom_) == 1;
}

Geom Geom::intersection(const Geom& other) const
{
    g_geomLastError.clear();   // 入口清空：运算后 lastError 非空 = 本次失败
    if (!geom_ || !other.geom_)
        return Geom();
    return Geom(GEOSIntersection_r(ctx(), (const GEOSGeometry*)geom_,
                                   (const GEOSGeometry*)other.geom_));
}

Geom Geom::unionWith(const Geom& other) const
{
    g_geomLastError.clear();   // 入口清空：运算后 lastError 非空 = 本次失败
    if (!geom_ || !other.geom_)
        return Geom();
    return Geom(GEOSUnion_r(ctx(), (const GEOSGeometry*)geom_,
                            (const GEOSGeometry*)other.geom_));
}

Geom Geom::difference(const Geom& other) const
{
    g_geomLastError.clear();   // 入口清空：运算后 lastError 非空 = 本次失败
    if (!geom_ || !other.geom_)
        return Geom();
    return Geom(GEOSDifference_r(ctx(), (const GEOSGeometry*)geom_,
                                 (const GEOSGeometry*)other.geom_));
}

Geom Geom::buffer(double dist, int quadSegs) const
{
    g_geomLastError.clear();   // 入口清空：运算后 lastError 非空 = 本次失败
    if (!geom_)
        return Geom();
    return Geom(GEOSBuffer_r(ctx(), (const GEOSGeometry*)geom_, dist, quadSegs));
}

Geom Geom::convexHull() const
{
    g_geomLastError.clear();   // 入口清空：运算后 lastError 非空 = 本次失败
    if (!geom_)
        return Geom();
    return Geom(GEOSConvexHull_r(ctx(), (const GEOSGeometry*)geom_));
}

BoundingBox2d Geom::toRect2d() const
{
    return bounds();
}

// ---------------- 底层构建 ----------------

Geom Geom::makePoint(double x, double y)
{
    const GEOSContextHandle_t c = ctx();
    GEOSCoordSequence* s = GEOSCoordSeq_create_r(c, 1, 2);
    if (!s)
        return Geom();
    GEOSCoordSeq_setX_r(c, s, 0, x);
    GEOSCoordSeq_setY_r(c, s, 0, y);
    GEOSGeometry* g = GEOSGeom_createPoint_r(c, s);
    if (!g)
        GEOSCoordSeq_destroy_r(c, s);
    return Geom(g);
}

Geom Geom::makeLine(const std::vector<Point2d>& pts)
{
    if (pts.size() < 2)
        return Geom();
    const GEOSContextHandle_t c = ctx();
    GEOSCoordSequence* s = GEOSCoordSeq_create_r(c, (unsigned)pts.size(), 2);
    if (!s)
        return Geom();
    for (size_t i = 0; i < pts.size(); ++i) {
        GEOSCoordSeq_setX_r(c, s, (unsigned)i, pts[i].x);
        GEOSCoordSeq_setY_r(c, s, (unsigned)i, pts[i].y);
    }
    GEOSGeometry* g = GEOSGeom_createLineString_r(c, s);
    if (!g)
        GEOSCoordSeq_destroy_r(c, s);
    return Geom(g);
}

// 构造闭合 LinearRing（输入必须已闭合：首尾相同，size >= 4）。
// 成功时所有权随返回转移；失败返回 nullptr（内部已清理 CoordSeq）。
static GEOSGeometry* buildLinearRing(const GEOSContextHandle_t c,
                                     const std::vector<Point2d>& closed)
{
    if (closed.size() < 4)
        return nullptr;
    GEOSCoordSequence* s = GEOSCoordSeq_create_r(c, (unsigned)closed.size(), 2);
    if (!s)
        return nullptr;
    for (size_t i = 0; i < closed.size(); ++i) {
        GEOSCoordSeq_setX_r(c, s, (unsigned)i, closed[i].x);
        GEOSCoordSeq_setY_r(c, s, (unsigned)i, closed[i].y);
    }
    GEOSGeometry* ring = GEOSGeom_createLinearRing_r(c, s);
    if (!ring)
        GEOSCoordSeq_destroy_r(c, s);
    return ring;
}

// 闭合工具：首尾不一致补点
static std::vector<Point2d> closeRing(const std::vector<Point2d>& pts)
{
    std::vector<Point2d> closed = pts;
    const Point2d& a = closed.front();
    const Point2d& b = closed.back();
    if (a.x != b.x || a.y != b.y)
        closed.push_back(a);
    return closed;
}

// 多边形：自动闭合（LinearRing 要求首尾相同，不闭合会堆损坏）
Geom Geom::makePolygon(const std::vector<Point2d>& pts)
{
    if (pts.size() < 3)
        return Geom();
    return makeRingPolygon(closeRing(pts));
}

Geom Geom::makeRingPolygon(const std::vector<Point2d>& pts)
{
    const GEOSContextHandle_t c = ctx();
    GEOSGeometry* ring = buildLinearRing(c, pts);
    if (!ring)
        return Geom();
    GEOSGeometry* g = GEOSGeom_createPolygon_r(c, ring, nullptr, 0);
    if (!g) {
        GEOSGeom_destroy_r(c, ring);
        return Geom();
    }
    return Geom(g);
}

// 带孔多边形：shell + holes 全部闭合成 ring 后交给 GEOS（成功时 GEOS 接管全部所有权）
Geom Geom::createPolygonWithHoles(const std::vector<Point2d>& shell,
                                  const std::vector<std::vector<Point2d>>& holes)
{
    if (shell.size() < 3)
        return Geom();
    const GEOSContextHandle_t c = ctx();
    GEOSGeometry* shellRing = buildLinearRing(c, closeRing(shell));
    if (!shellRing)
        return Geom();
    std::vector<GEOSGeometry*> holeRings;
    for (const std::vector<Point2d>& h : holes) {
        if (h.size() < 3)
            continue;   // 少于 3 点的孔环忽略（头文件契约）
        GEOSGeometry* r = buildLinearRing(c, closeRing(h));
        if (r)
            holeRings.push_back(r);
    }
    GEOSGeometry* g = GEOSGeom_createPolygon_r(c, shellRing,
                                               holeRings.empty() ? nullptr : holeRings.data(),
                                               (unsigned)holeRings.size());
    if (!g) {
        // 失败时 GEOS 不接管参数所有权：全部手动销毁
        GEOSGeom_destroy_r(c, shellRing);
        for (GEOSGeometry* r : holeRings)
            GEOSGeom_destroy_r(c, r);
        return Geom();
    }
    return Geom(g);
}

// 批量并集（一元并集）：克隆入参装进 GeometryCollection 后一次 GEOSUnaryUnion
Geom Geom::unaryUnion(const std::vector<const Geom*>& geoms)
{
    const GEOSContextHandle_t c = ctx();
    std::vector<GEOSGeometry*> clones;
    clones.reserve(geoms.size());
    for (const Geom* g : geoms)
        if (g && g->geom_)
            clones.push_back(GEOSGeom_clone_r(c, (const GEOSGeometry*)g->geom_));
    if (clones.empty())
        return Geom();
    // createCollection 成功后接管 clones 元素所有权；失败时元素仍归调用方，需手动销毁
    GeosPtr col(GEOSGeom_createCollection_r(c, GEOS_GEOMETRYCOLLECTION,
                                            clones.data(), (int)clones.size()));
    if (!col.g) {
        for (GEOSGeometry* p : clones)
            GEOSGeom_destroy_r(c, p);
        return Geom();
    }
    return Geom(GEOSUnaryUnion_r(c, col.g));
}

Geom Geom::parseWKT(const std::string& wkt)
{
    const GEOSContextHandle_t c = ctx();
    GEOSWKTReader* r = GEOSWKTReader_create_r(c);
    GEOSGeometry* g = GEOSWKTReader_read_r(c, r, wkt.c_str());
    GEOSWKTReader_destroy_r(c, r);
    return Geom(g);
}

// ---------------- 曲线离散化 ----------------
// 分段数策略：segments > 0 强制指定；否则按弦高误差自适应
//   θ = 2·acos(1 - e/r)，N = ceil(2π/θ)；下限 8（低于八边形失去圆的意义），
//   上限 4096（防极端误差要求撑爆 GEOS 内存）。

static int curveSegments(double featureRadius, double maxError, int segments)
{
    constexpr int kMinSeg = 8;
    constexpr int kMaxSeg = 4096;
    if (segments > 0)
        return std::clamp(segments, kMinSeg, kMaxSeg);
    if (featureRadius <= 0 || maxError <= 0)
        return kMaxSeg;
    const double ratio = 1.0 - maxError / featureRadius;
    if (ratio <= -1.0)
        return kMinSeg;   // 误差要求比直径还松 → 最少段数即可满足（回归 2026-11-09：原误返回 kMaxSeg）
    const double theta = 2.0 * std::acos(ratio);
    if (theta <= 0)
        return kMaxSeg;
    const int n = (int)std::ceil(2.0 * M_PI / theta);
    return std::clamp(n, kMinSeg, kMaxSeg);
}

Geom Geom::makeCircle(double cx, double cy, double r, double maxError, int segments)
{
    if (r <= 0)
        return Geom();
    const int n = curveSegments(r, maxError, segments);
    std::vector<Point2d> pts;
    pts.reserve((size_t)n + 1);
    for (int i = 0; i < n; ++i) {
        const double t = 2.0 * M_PI * (double)i / (double)n;
        pts.push_back(Point2d{ cx + r * std::cos(t), cy + r * std::sin(t) });
    }
    pts.push_back(pts.front());   // 闭合
    return makeRingPolygon(pts);
}

// 椭圆：中心 (xc,yc)，半轴 a/b，长轴方向 thetaDeg（度）。离散精度按长半轴算（最坏曲率）。
Geom Geom::makeEllipse(double xc, double yc, double a, double b, double thetaDeg,
                       double maxError, int segments)
{
    if (a <= 0 || b <= 0)
        return Geom();
    const int n = curveSegments(std::max(a, b), maxError, segments);
    const double th = thetaDeg * M_PI / 180.0;
    const double ct = std::cos(th), st = std::sin(th);
    std::vector<Point2d> pts;
    pts.reserve((size_t)n + 1);
    for (int i = 0; i < n; ++i) {
        const double t = 2.0 * M_PI * (double)i / (double)n;
        const double ex = a * std::cos(t);
        const double ey = b * std::sin(t);
        pts.push_back(Point2d{ xc + ex * ct - ey * st,
                               yc + ex * st + ey * ct });
    }
    pts.push_back(pts.front());   // 闭合
    return makeRingPolygon(pts);
}

// ---------------- 公共工厂转发（非模板部分） ----------------

Geom Geom::createPoint(double x, double y) { return makePoint(x, y); }
Geom Geom::fromWKT(const std::string& wkt) { return parseWKT(wkt); }
Geom Geom::createCircle(double cx, double cy, double r, double max_error, int segments)
{
    return makeCircle(cx, cy, r, max_error, segments);
}
Geom Geom::createEllipse(double xc, double yc, double a, double b, double theta_deg,
                         double max_error, int segments)
{
    return makeEllipse(xc, yc, a, b, theta_deg, max_error, segments);
}

// ---------------- PreparedGeom 预处理几何 ----------------

PreparedGeom::PreparedGeom() {}
PreparedGeom::PreparedGeom(void* prep) : prep_(prep) {}

PreparedGeom::~PreparedGeom()
{
    // 先销毁 prepared（内部引用 base_ 的底层指针），再析构 base_
    if (prep_)
        GEOSPreparedGeom_destroy_r(ctx(), (const GEOSPreparedGeometry*)prep_);
}

PreparedGeom::PreparedGeom(PreparedGeom&& other) noexcept
    : base_(std::move(other.base_)), prep_(other.prep_)
{
    other.prep_ = nullptr;
}

PreparedGeom& PreparedGeom::operator=(PreparedGeom&& other) noexcept
{
    if (this != &other) {
        if (prep_)
            GEOSPreparedGeom_destroy_r(ctx(), (const GEOSPreparedGeometry*)prep_);
        base_ = std::move(other.base_);
        prep_ = other.prep_;
        other.prep_ = nullptr;
    }
    return *this;
}

PreparedGeom PreparedGeom::prepare(const Geom& g)
{
    if (g.isEmpty())
        return PreparedGeom();
    // 内部克隆源几何并持有（GEOS prepared 结构引用基础几何指针，要求保活）
    PreparedGeom out;
    out.base_ = Geom(GEOSGeom_clone_r(ctx(), (const GEOSGeometry*)g.geom_));
    out.prep_ = GEOSPrepare_r(ctx(), (const GEOSGeometry*)out.base_.geom_);
    if (!out.prep_) {
        out.base_ = Geom();
        out.prep_ = nullptr;
    }
    return out;
}

bool PreparedGeom::isEmpty() const
{
    return !prep_ || base_.isEmpty();
}

bool PreparedGeom::intersects(const Geom& other) const
{
    if (!prep_ || !other.geom_ || other.isEmpty())
        return false;
    return GEOSPreparedIntersects_r(ctx(), (const GEOSPreparedGeometry*)prep_,
                                    (const GEOSGeometry*)other.geom_) == 1;
}

bool PreparedGeom::contains(const Geom& other) const
{
    if (!prep_ || !other.geom_ || other.isEmpty())
        return false;
    return GEOSPreparedContains_r(ctx(), (const GEOSPreparedGeometry*)prep_,
                                  (const GEOSGeometry*)other.geom_) == 1;
}

bool PreparedGeom::covers(const Geom& other) const
{
    if (!prep_ || !other.geom_ || other.isEmpty())
        return false;
    return GEOSPreparedCovers_r(ctx(), (const GEOSPreparedGeometry*)prep_,
                                (const GEOSGeometry*)other.geom_) == 1;
}

double PreparedGeom::distanceTo(const Geom& other) const
{
    if (!prep_ || !other.geom_ || other.isEmpty())
        return -1.0;
    double d = 0;
    if (GEOSPreparedDistance_r(ctx(), (const GEOSPreparedGeometry*)prep_,
                               (const GEOSGeometry*)other.geom_, &d) != 1)
        return -1.0;
    return d;
}

} // namespace shapely
