#ifndef SHAPELY_GEOM_H
#define SHAPELY_GEOM_H

// shapely::Geom —— 纯二维几何代数与拓扑引擎（基于 GEOS C API，Pimpl 隔离）。
// 设计约束（见技术交底书）：
//   * 公共头文件绝对纯净：仅 C++ 标准库 + shapely::Types.h，零第三方 include；
//   * Pimpl：GEOSGeometry* 完全隐藏在 .cpp；
//   * 静态工厂：构造函数私有，createPoint/createPolygon/... 强制声明几何意图；
//   * 算子原子化：点/线/面能力严格隔离，越界调用返回零值不抛异常；
//   * 不可变性：布尔/形变运算返回新对象，不改原对象；
//   * RAII + 移动语义：禁拷贝（防 Double Free），析构自动释放底层资源。
// 泛化输入（鸭子类型模板，编译期提取 .x/.y 转 double，3D 自动丢 Z）：
//   cv::Point/Point2f/Point2d/Point3f/Point3d、含 .x/.y 的自定义结构体、
//   扁平数组 [x1,y1,...]、二维嵌套数组（内层取 [0][1]）。
//   OpenCV Rect/RotatedRect 输入走适配层 GeomCVAdapter.hpp。

#include "Types.h"
#include "AlgExport.h"

#include <string>
#include <utility>
#include <vector>

namespace shapely {

class SHAPELYALG_EXPORT Geom
{
public:
    // ---------------- 生命周期（RAII，只移动） ----------------
    ~Geom();
    Geom(Geom&& other) noexcept;
    Geom& operator=(Geom&& other) noexcept;
    Geom(const Geom&) = delete;
    Geom& operator=(const Geom&) = delete;

    // ---------------- 通用功能（点/线/面均可调用） ----------------

    /// 是否为有效几何。构建失败（点数不足、WKT 解析失败）或传入空几何时返回 true。
    /// 所有算子对空对象都安全：数值类返回 0/-1，布尔类返回 false，布尔运算返回空对象。
    bool isEmpty() const;

    /// 输出 WKT 字符串（OGC 标准文本），如 "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))"。
    /// 用途：调试打印、结果落盘、与 GIS 工具链交换。空对象返回空串。
    std::string wkt() const;

    /// GeoJSON 几何对象文本（RFC 7946 子集，2D 坐标）：
    /// {"type":"Point","coordinates":[x,y]} / LineString / Polygon（含孔洞环）/
    /// MultiPoint / MultiLineString / MultiPolygon / GeometryCollection。
    /// 用途：上位机/前端交换（JSON 直接可解析）。空对象返回空串。
    std::string toGeoJSON() const;

    /// DXF R12 ASCII ENTITIES 段文本（Point→POINT，线/环/面→LINE 段集，Multi 递归展开）。
    /// 用途：CAD 交换/交付图（AutoCAD/中望/浏览器的最小公共子集）。空对象返回空串。
    std::string toDXF() const;

    /// 几何类型名（GEOS 原名）："Point" / "LineString" / "Polygon" /
    /// "MultiPolygon" / "GeometryCollection" 等。空对象返回 "null"。
    std::string typeName() const;

    /// 二维轴对齐包围盒（AABB）。
    /// 返回：min/max 坐标（与构建输入同单位）；valid=false 表示空对象。
    BoundingBox2d bounds() const;

    /// 提取几何体的全部顶点（按底层存储顺序）：
    ///   Point      → 1 个点
    ///   LineString → 折线顶点序列（不闭合）
    ///   Polygon    → 外壳 ring 顶点（闭合，首尾相同；内部孔洞忽略）
    ///   Multi*/Collection → 逐子几何展开拼接
    /// 用途：结果渲染、导出 DXF/CSV 等。空对象返回空 vector。
    std::vector<Point2d> getExteriorPoints() const;

    /// 拆分多体几何为单体列表。
    ///   单体（Point/LineString/Polygon）→ 返回含自身克隆的 1 元素列表
    ///   Multi*/Collection               → 逐子几何克隆展开
    /// 用途：unionWith 两个不相交区域得到 MultiPolygon 后，逐区域取 area()/bounds()
    ///       （本库的多边形度量只对单体有意义）。空对象返回空列表。
    std::vector<Geom> splitParts() const;

    /// 本几何到 other 的最短二维距离。
    /// 单位：与坐标同单位（本项目惯例 mm）。两几何相交时返回 0。
    /// 返回：-1 表示任一方为空对象/计算失败；GEOS 空几何（POINT EMPTY）也归一为 -1。
    double distanceTo(const Geom& other) const;

    /// 修复无效几何（GEOS MakeValid，3.9+）：自交/重复边等修复为有效结构。
    /// 返回：修复后的新几何（结果可能是 Multi*）；空对象返回空；修复失败返回空。
    /// 用途：findContours 自交轮廓先 makeValid 再参与布尔/面积运算
    ///      （isValid()==false 时 area() 语义不可靠）。
    Geom makeValid() const;

    /// 有效性诊断原因（GEOS isValidReason）："Valid Geometry" 或
    /// "Self-intersection[...]" 等带位置的诊断信息；空对象返回 "null geometry"。
    /// 用途：isValid()==false 时定位问题点，比只拿 bool 可诊断。
    std::string isValidReason() const;

    /// 本线程最近一次 GEOS 诊断消息（线程局部；布尔/形变运算入口清空）。
    /// 用途：区分"运算失败（GEOS 拓扑异常，返回空对象）"与"真空结果（不相交等）"——
    ///       运算结果 isEmpty() 时，lastError() 非空 = 失败，空 = 真空。
    /// WKT 解析失败同样写入；成功运算与真空结果不留消息。
    static std::string lastError();

    /// 2D 仿射变换：x' = a·x + b·y + tx；y' = c·x + d·y + ty。
    /// 返回：变换后的新几何（Point/LineString/Polygon/Multi*/孔洞/嵌套结构全保留）。
    /// 用途：缺陷轮廓跨坐标系映射（平移/旋转/缩放一步到位）。
    /// 典型：仅平移 (1,0,tx, 0,1,ty)；绕原点旋转 ang（弧度）
    ///       (cos,-sin,0, sin,cos,0)；缩放 (sx,0,0, 0,sy,0)。
    Geom transformed(double a, double b, double tx, double c, double d, double ty) const;

    /// 两几何是否接触或重叠（DE-9IM 相交判定，边界接触也算相交）。
    /// 任一方为空对象返回 false。
    bool intersects(const Geom& other) const;

    /// 两几何边界接触但不重叠（Touch：相邻区域的邻接判定）。
    /// 例：两个共边矩形 touches=true、intersects=true；间隔 1mm 的两矩形均 false。
    bool touches(const Geom& other) const;

    /// 几何有效性检查（GEOS isValid）：自相交多边形返回 false。
    /// 用途：createPolygon 收到乱序/自交点集后的排查——自交面的 area() 语义不可靠。
    bool isValid() const;

    /// 道格拉斯-普克抽稀：容差内的冗余顶点被删除，大幅减少点数。
    /// [in] tolerance     抽稀容差（与坐标同单位，如 0.1mm）；<=0 时原样返回克隆
    /// [in] keepTopology  true 用拓扑保持版本（结果保证合法、稍慢）
    /// 用途：OpenCV findContours 输出的密集点链转 Geom 前先抽稀，
    ///       否则多边形顶点数巨大、布尔运算显著变慢。
    /// 返回：抽稀后的新对象（本对象不变）。
    Geom simplify(double tolerance, bool keepTopology = false) const;

    // ---------------- 点专属 ----------------

    /// 点的 X 坐标。非 Point 类型返回 0（严格按类型分派，不取包围盒）。
    double getX() const;

    /// 点的 Y 坐标。非 Point 类型返回 0。
    double getY() const;

    /// 本点是否在另一几何（线或多边形）内部。
    /// 语义：GEOS Within——点在线上/面内算 true，在面边界上算 false
    ///      （如需含边界的判定，业务层自行对目标面做微量 buffer）。
    bool within(const Geom& lineOrPolygon) const;

    // ---------------- 线专属 ----------------

    /// 折线总长度（逐段累加）。单位：与坐标同单位。
    /// GEOS Length 语义：MultiLineString 返回总长，Polygon 返回周长，点返回 0。
    double length() const;

    /// 本线与 other 是否发生拓扑交叉（Crosses：线穿线、线穿面的内部）。
    /// 仅端点相接/共线重叠不算交叉。任一方为空对象返回 false。
    bool crosses(const Geom& other) const;

    // ---------------- 面专属 ----------------

    /// 面积（GEOS 对合法面恒为正）。单位：坐标单位的平方。
    /// Polygon 返回单面面积，MultiPolygon 返回各面总面积；非面类型（点/线）返回 0。
    double area() const;

    /// 多边形质心（面积加权；凹多边形的质心可能在面外——这是数学事实非 bug）。
    /// 仅面（Polygon/MultiPolygon）有效；空对象/非面类型返回 (0,0)。
    Point2d centroid() const;

    /// 本面是否完全包含 other（点/线/另一面）。
    /// 语义：GEOS Contains——other 无任何点落在 exterior 中，且至少一点在
    ///       interior 中；完全躺在边界上的几何（如沿边的线）不算包含
    ///       （需含边界的判定用 Geom 面的 buffer 微扩，或改用 contains 的语义变体）。
    bool contains(const Geom& other) const;

    /// 交集：两几何重叠部分，返回新对象（本对象不变）。
    /// 不相交时返回空几何（isEmpty()==true），不是 nullptr。
    Geom intersection(const Geom& other) const;

    /// 并集：两几何合并，返回新对象。
    Geom unionWith(const Geom& other) const;

    /// 差集：本几何减去与 other 重叠的部分，返回新对象。
    Geom difference(const Geom& other) const;

    /// 缓冲区：把几何向外（dist>0）膨胀 / 向内（dist<0）收缩。
    /// [in] dist      缓冲距离，单位与坐标同单位；0 等于原样返回
    /// [in] quadSegs  每 1/4 圆弧的逼近段数，越大圆弧越平滑、越慢；默认 8
    /// 返回：缓冲后的新多边形。空对象返回空。
    Geom buffer(double dist, int quadSegs = 8) const;

    /// 凸包：包裹全部顶点的最小凸多边形（面）。点集共线时退化为线/点。
    Geom convexHull() const;

    /// 最小包围盒（与 bounds 等价，语义别名，供"取 ROI 裁图"场景使用）。
    BoundingBox2d toRect2d() const;

    /// 内环（孔洞）数量。仅 Polygon 有效；非面（含 MultiPolygon）返回 0。
    int numInteriorRings() const;

    /// 全部内环（孔洞）顶点：每个内环闭合（首尾相同），顺序同底层存储。
    /// 非面/无孔返回空列表。
    std::vector<std::vector<Point2d>> getInteriorRings() const;

    /// 两几何的最近点对（各返回一点；相交时两点重合于交线）。
    /// [out] out_a  本几何上的最近点
    /// [out] out_b  other 上的最近点
    /// 返回：false = 任一方为空对象/计算失败（输出置 (0,0)）。
    bool nearestPoints(const Geom& other, Point2d& out_a, Point2d& out_b) const;

    /// 本几何与 other 的最短距离是否 ≤ dist（GEOS 原生判定，比 distanceTo 后手比较更快）。
    /// 任一方为空对象返回 false。
    bool isWithinDistance(const Geom& other, double dist) const;

    // =====================================================================
    // 静态工厂（模板，Header-Only；鸭子类型：只需 .x/.y 成员）
    // =====================================================================

    // ---- 点 ----
    /// 从两个数值创建点。
    /// [in] x  X 坐标（单位由调用方约定，本项目惯例 mm）
    /// [in] y  Y 坐标
    static Geom createPoint(double x, double y);

    /// 从任意含 .x/.y 成员的结构创建点（鸭子类型）。
    ///   cv::Point（int）/ Point2f / Point2d / Point3f / Point3d / 自定义结构体
    /// 3D 点只取 .x/.y，.z 无情丢弃（本模块是纯二维）。
    /// 整型/浮点成员统一 static_cast 为 double。
    template <typename P> static Geom createPoint(const P& p)
        { return createPoint((double)p.x, (double)p.y); }

    /// 从扁平数组创建点：取前两个元素作为 x、y，其余忽略。
    /// [in] flat  元素数 >= 2 才有效；不足返回空对象
    template <typename T> static Geom createPoint(const std::vector<T>& flat)
        { return flat.size() < 2 ? Geom() : createPoint((double)flat[0], (double)flat[1]); }

    // ---- 线（不强制闭合） ----
    /// 从点集创建折线：按数组顺序连线，不闭合（首尾不补点）。
    /// [in] pts  点序列（元素含 .x/.y，如 cv::Point2d / Point3f / 自定义结构）
    ///           少于 2 个点返回空对象
    template <typename P> static Geom createLine(const std::vector<P>& pts);

    /// 从一维扁平数组创建折线：[x1,y1, x2,y2, ...] 两两配对。
    /// 奇数个元素时最后一个被忽略。少于 2 个完整点返回空对象。
    template <typename T> static Geom createLineFromFlat(const std::vector<T>& flat);

    /// 从二维嵌套数组创建折线：外层每项是一个点，内层取 [0]=X、[1]=Y（忽略后续）。
    /// 内层不足 2 个元素的行被跳过；有效点少于 2 个返回空对象。
    template <typename T> static Geom createLineFromNested(const std::vector<std::vector<T>>& nested);

    // ---- 多边形（自动闭合） ----
    /// 从点集创建多边形：按顺序连线，首尾不一致时内部自动补闭合点
    /// （GEOS LinearRing 硬性要求首尾相同，不闭合会堆损坏——由本接口兜底）。
    /// [in] pts  顶点序列；有效顶点（含闭合点）少于 4 个返回空对象
    template <typename P> static Geom createPolygon(const std::vector<P>& pts);

    /// 扁平数组版多边形：[x1,y1, x2,y2, ...] 两两配对，其余同 createPolygon。
    template <typename T> static Geom createPolygonFromFlat(const std::vector<T>& flat);

    /// 嵌套数组版多边形：内层取 [0]=X、[1]=Y，其余同 createPolygon。
    template <typename T> static Geom createPolygonFromNested(const std::vector<std::vector<T>>& nested);

    // ---- WKT ----
    /// 从 WKT 字符串解析任意几何（POINT / LINESTRING / POLYGON / MULTI* / GEOMETRYCOLLECTION）。
    /// [in] wkt  OGC 标准文本，如 "POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))"
    /// 解析失败返回空对象（isEmpty()==true），不抛异常。
    static Geom fromWKT(const std::string& wkt);

    // ---- 曲线（离散化为普通多边形，语义丢失） ----
    /// 创建圆：离散为 N 边形（构建后即普通 Polygon，圆属性不保留）。
    /// [in] cx, cy    圆心坐标
    /// [in] radius    半径，>0；<=0 返回空对象
    /// [in] max_error 最大弦高误差（弦到圆弧的最大距离），默认 1e-4。
    ///                分段数自适应：θ = 2·acos(1-e/r)，N = ceil(2π/θ)。
    ///                半径越大/误差越小 → 分段越多。
    /// [in] segments  >0 时强制指定分段数（覆盖 max_error 策略），
    ///                无论哪种策略都强制截断在 [8, 4096]
    ///                （下限：低于八边形失去圆的意义；上限：防极端误差撑爆内存）
    static Geom createCircle(double cx, double cy, double radius,
                             double max_error = 1e-4, int segments = 0);

    /// 创建椭圆：离散为 N 边形。
    /// [in] xc, yc     中心坐标
    /// [in] a, b       半轴长（a 沿 theta_deg 方向，b 垂直方向），均须 >0
    /// [in] theta_deg  长轴方向角（度，逆时针）
    /// [in] max_error  弦高误差上限；按长半轴 max(a,b) 计算——保证最坏曲率处
    ///                 误差不超过该值
    /// [in] segments   >0 时强制指定分段数，截断 [8, 4096]
    /// 半轴 <=0 返回空对象。
    static Geom createEllipse(double xc, double yc, double a, double b, double theta_deg,
                              double max_error = 1e-4, int segments = 0);

    /// 带孔多边形工厂：shell 外轮廓 + holes 内环列表（环形/带孔 ROI 必备——
    /// 面积、包含、相交自动扣除孔洞，普通 createPolygon 只能表达外轮廓）。
    /// [in] shell  外轮廓顶点（自动闭合）；少于 3 点返回空对象
    /// [in] holes  每个内环的顶点序列（各自自动闭合）；少于 3 点的环被忽略
    static Geom createPolygonWithHoles(const std::vector<Point2d>& shell,
                                       const std::vector<std::vector<Point2d>>& holes);

    /// 批量并集（一元并集）：一次合并多个几何。
    /// 等价于逐个 unionWith，但 GEOS 内部一次性处理，更快且避免中间结果碎片化。
    /// [in] geoms  输入几何只读指针列表（本类型禁拷贝，传指针不消费入参）；
    ///             nullptr / 空对象自动忽略
    /// 返回：合并结果（可能为 Multi*）；输入全空返回空对象。
    static Geom unaryUnion(const std::vector<const Geom*>& geoms);

private:
    friend class GeomCVAdapter;    // 适配层访问底层构建
    friend class Stereo;           // 2.5D 桥接（projectTo2D 构建空对象）
    friend class PreparedGeom;     // 预处理几何（读取底层句柄）
    Geom();                        // 空对象（构建失败兜底）
    explicit Geom(void* g);        // 接管底层句柄（内部/适配层）

    // ---------------- 底层构建（private：业务层一律用上方 create*/fromWKT 工厂） ----------------
    // 纯 double 路径；点集闭合语义由调用方保证（makePolygon 自动闭合，makeRingPolygon 要求已闭合）。

    /// 内部：单点构建（createPoint 的转发目标）。
    static Geom makePoint(double x, double y);

    /// 内部：折线构建（不闭合）。
    static Geom makeLine(const std::vector<Point2d>& pts);

    /// 内部：面构建，自动闭合（首尾不一致补点；少于 3 点返回空对象）。
    static Geom makePolygon(const std::vector<Point2d>& pts);

    /// 内部：面构建，要求输入已闭合（首尾相同，size >= 4）。
    static Geom makeRingPolygon(const std::vector<Point2d>& pts);

    /// 内部：WKT 解析（fromWKT 的转发目标；失败返回空对象）。
    static Geom parseWKT(const std::string& wkt);

    /// 内部：圆离散化（curveSegments 策略：segments>0 强制；否则按弦高误差自适应 8~4096 段）。
    static Geom makeCircle(double cx, double cy, double r, double max_error, int segments);

    /// 内部：椭圆离散化（精度按长半轴计算，最坏曲率口径）。
    static Geom makeEllipse(double xc, double yc, double a, double b, double theta_deg,
                            double max_error, int segments);

    // Pimpl：底层句柄（实际类型在 .cpp，头文件零 GEOS 痕迹）
    void* geom_ = nullptr;
};

// =====================================================================
// 模板工厂实现（Header-Only，编译期特化）
// =====================================================================

namespace detail {

// 点集归一化：任意 .x/.y 结构 → vector<Point2d>（3D 丢 Z，统一 double）
template <typename P>
inline std::vector<Point2d> normalizePoints(const std::vector<P>& pts)
{
    std::vector<Point2d> out;
    out.reserve(pts.size());
    for (const P& p : pts)
        out.push_back(Point2d{ (double)p.x, (double)p.y });
    return out;
}

// 扁平数组 [x1,y1,x2,y2,...] → 点集
template <typename T>
inline std::vector<Point2d> flatToPoints(const std::vector<T>& flat)
{
    std::vector<Point2d> pts;
    const size_t n = flat.size() / 2;
    pts.reserve(n);
    for (size_t i = 0; i < n; ++i)
        pts.push_back(Point2d{ (double)flat[i * 2], (double)flat[i * 2 + 1] });
    return pts;
}

// 二维嵌套数组（内层取索引 0,1 作为 X,Y，忽略后续）→ 点集
template <typename T>
inline std::vector<Point2d> nestedToPoints(const std::vector<std::vector<T>>& nested)
{
    std::vector<Point2d> pts;
    pts.reserve(nested.size());
    for (const std::vector<T>& p : nested)
        if (p.size() >= 2)
            pts.push_back(Point2d{ (double)p[0], (double)p[1] });
    return pts;
}

} // namespace detail

template <typename P>
inline Geom Geom::createLine(const std::vector<P>& pts)
{
    return makeLine(detail::normalizePoints(pts));
}

template <typename T>
inline Geom Geom::createLineFromFlat(const std::vector<T>& flat)
{
    return makeLine(detail::flatToPoints(flat));
}

template <typename T>
inline Geom Geom::createLineFromNested(const std::vector<std::vector<T>>& nested)
{
    return makeLine(detail::nestedToPoints(nested));
}

template <typename P>
inline Geom Geom::createPolygon(const std::vector<P>& pts)
{
    return makePolygon(detail::normalizePoints(pts));   // 内部自动闭合
}

template <typename T>
inline Geom Geom::createPolygonFromFlat(const std::vector<T>& flat)
{
    return makePolygon(detail::flatToPoints(flat));
}

template <typename T>
inline Geom Geom::createPolygonFromNested(const std::vector<std::vector<T>>& nested)
{
    return makePolygon(detail::nestedToPoints(nested));
}

// =====================================================================
// PreparedGeom —— 预处理几何（空间索引缓存）
// =====================================================================
// 场景：一个几何（如 ROI 面）面对成百上千个其他几何反复做相交/包含判定。
// 普通谓词每次都重建被测几何的索引；Prepared 把索引构建一次缓存，
// 重复判定快约一个数量级。
//
// 生命周期：内部持有源几何的移动入副本（GEOS prepared 结构内部引用基础
// 几何指针，要求基础几何保活——由成员 base_ 自动保证）。
// 用法：
//   PreparedGeom roi = PreparedGeom::prepare(roiPoly);
//   for (const Geom& defect : defects)
//       if (roi.intersects(defect)) { ... }        // 判定走缓存索引
//
// 线程安全：与 Geom 相同——单个对象不可跨线程并发调用。

class SHAPELYALG_EXPORT PreparedGeom
{
public:
    ~PreparedGeom();
    PreparedGeom(PreparedGeom&& other) noexcept;
    PreparedGeom& operator=(PreparedGeom&& other) noexcept;
    PreparedGeom(const PreparedGeom&) = delete;
    PreparedGeom& operator=(const PreparedGeom&) = delete;

    /// 从 Geom 构建预处理对象（内部克隆源几何并建索引）。
    /// 源几何为空对象时返回空 PreparedGeom（各判定恒 false）。
    static PreparedGeom prepare(const Geom& g);

    /// 是否构建成功。空对象的全部判定恒返回 false / -1。
    bool isEmpty() const;

    /// 快速相交判定（语义与 Geom::intersects 完全一致）。
    bool intersects(const Geom& other) const;

    /// 快速包含判定（语义与 Geom::contains 一致：躺卧在边界上的几何不算）。
    bool contains(const Geom& other) const;

    /// 快速覆盖判定（含边界：躺卧在边界上的几何算 covers=true，与 contains 互补）。
    bool covers(const Geom& other) const;

    /// 快速距离（语义与 Geom::distanceTo 一致；空对象/失败返回 -1）。
    double distanceTo(const Geom& other) const;

private:
    PreparedGeom();                       // 空对象
    explicit PreparedGeom(void* prep);    // 接管底层句柄（内部）

    Geom base_;                           // 持有源几何（prepared 内部引用它，必须保活）
    const void* prep_ = nullptr;          // GEOSPreparedGeometry*（隐藏在 .cpp）
};

} // namespace shapely

#endif // SHAPELY_GEOM_H
