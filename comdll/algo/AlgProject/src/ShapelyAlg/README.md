# ShapelyAlg 使用说明（第三方接入）

纯二维/三维几何与图像/点云**基础算子库**。不含任何业务逻辑——业务流程请在业务层（BusinessAlg）或调用方代码中编排。

---

## 1. 模块定位与分层

```
┌─────────────────────────────┐
│  业务层（BusinessAlg 等）     │  流程编排、业务语义判定
├─────────────────────────────┤
│  Image2DAlg / Point3DAlg    │  通用 2D / 3D 模块（图像读取、SICK 扫描）
├─────────────────────────────┤
│  ShapelyAlg（本库）          │  基础原子算子（几何/图像/点云），无业务语义
└─────────────────────────────┘
```

四个子模块，全部通过唯一的 `ShapelyAlg.h` 引入：

| 子模块 | 底层引擎 | 职责 |
|--------|----------|------|
| `shapely::Geom` | GEOS 3.15 | 二维点/线/面：构造、布尔运算、拓扑判定、度量 |
| `shapely::Stereo` | 纯数学 | 三维点集容器：AABB/OBB、距离、2.5D 降维 |
| `shapely::ImageProcessor` | OpenCV | 二维图像原子算子（阈值/形态学/轮廓/瓦片） |
| `shapely::CloudProcessor` / `CloudIO` | PCL 1.9 | 三维点云算子（滤波/特征/分割/配准/重建/度量）与文件读写 |

## 2. 交付物与接入步骤

### 2.1 拷贝文件

从构建输出目录（`build/output/Release/`）拷贝：

| 类别 | 文件 |
|------|------|
| 库 | `ShapelyAlg.dll`、`ShapelyAlg.lib`（链接用） |
| 头文件 | 整个 `src/ShapelyAlg/` 目录（`ShapelyAlg.h` + 其 include 的全部头） |
| 运行时 DLL | `geos_c.dll`、`geos.dll`（GEOS）、`opencv_world440.dll`、`OpenNI2.dll`、`pcl_*_release.dll`（部署目录全量 20 个，交付目录实况） |

> **pcl 运行时 DLL 的两种口径**：
> - 代码直接依赖的 pcl 子模块为 **10 个**（`CloudProcessor`/`CloudIO` 实际 include 的头文件只涉及 common、io、filters、features、kdtree、keypoints、octree、registration、segmentation、surface 这 10 个子模块）；
> - 部署目录全量为 **20 个**（CMake 的 `collect_third_party_runtime_dlls` 按 PCL 安装包全量收集，交付目录 `build/output/Release/` 实况如此），拷贝时以上表 20 个为准。

> **必须整组拷贝**：`ShapelyAlg.dll` 直接依赖上表全部运行时 DLL，缺任一都会报"找不到模块或它的依赖"（不会指明缺哪个）。

### 2.2 工程配置（MSVC / C++17）

```cpp
// 附加包含目录
<ShapelyAlg 头文件目录>
<OpenCV>/include                    // ImageProcessor.h 引用 cv::Mat
<PCL>/include/pcl-1.9               // CloudProcessor.h 引用 pcl 点型
<PCL 3rdParty>/Boost/include/boost-1_68
<PCL 3rdParty>/Eigen/eigen3

// 附加库目录 + 链接
ShapelyAlg.lib  opencv_world440.lib  pcl_common_release.lib ...（按实际用到的子模块）
```

或者直接用 CMake 目标（本仓库内）：

```cmake
target_link_libraries(你的目标 PUBLIC ShapelyAlg)
```

### 2.3 使用

```cpp
#include "ShapelyAlg.h"   // 唯一入口，包含全部子模块与适配层
```

## 3. 全局约定（务必先读）

| 约定 | 说明 |
|------|------|
| **单位** | 无全局单位强制；本项目惯例 mm。所有几何量（面积 mm²、距离 mm）与输入坐标同单位 |
| **精度** | 内部统一 double，输入 int/float 自动向上转型，无精度损失 |
| **降维** | Geom 是纯二维：3D 输入（Point3f/Point3d）自动丢弃 .z |
| **错误处理** | 全库不抛异常。失败表现为：返回空对象（`isEmpty()==true`）、数值 0/-1、布尔 false、指针 nullptr，详见各头文件注释 |
| **不可变性** | 布尔/形变运算全部返回新对象，原对象不变 |
| **RAII** | 对象析构自动释放底层资源；禁拷贝、只可移动（防 Double Free） |
| **线程安全** | 底层引擎上下文线程局部，多线程可各建各的对象并行计算；单个对象不可跨线程并发调用 |
| **鸭子类型** | 工厂模板只要求元素含 `.x/.y`（3D 加 `.z`）成员——cv::Point 系列、自定义结构体直接可用 |
| **BoostCompat 垫片** | 任何 .cpp 在直接 include `<pcl/io/*.h>` 前，必须**先 include `"BoostCompat.h"`**，否则 Boost 1.68 + 新版 MSVC STL 编译报 C2039（`std::fpos` 无 `seekpos`）。通过 CloudIO.h/CloudProcessor.h 使用点云功能则无需关心（垫片已在库内处理），写自己的 PCL IO 代码时才需要，原理见 [BoostCompat.h](BoostCompat.h) 头注释 |

## 3.1 基础数据结构（Types.h，零第三方依赖）

| 结构 | 字段 | 说明 |
|------|------|------|
| `Point2d` | `x, y` | 二维点（double） |
| `Point3d` | `x, y, z` | 三维点（z = 高度方向惯例） |
| `BoundingBox2d` | `min_x/min_y/max_x/max_y, valid` | 2D AABB；`width()/height()` 取边长 |
| `AABB3d` | `min_x..max_z, valid` | 3D 轴对齐盒；`volume()` = 三边乘积；`intersects()/contains()` 判相交包含 |
| `OBB3d` | `center, extents[3], rotation[9], valid` | PCA 有向盒：extents = 三主轴**半长**，rotation 行主序 3x3（**第 d 行 = 第 d 主轴方向**）；`volume()` = 8×三半长乘积 |

> `OBB3d` 的字段约定被 `Stereo::getOBB`、`CloudProcessor::measureOBB`、`extractCropOBB` 共用；
> 把点变换到盒局部系用 `p_local[d] = rotation[d] · (p - center)`。

## 4. Geom 二维几何

### 4.1 构造（静态工厂，语义显式）

```cpp
using namespace shapely;

// 点：数值 / cv 点（3D 自动丢 Z）/ 扁平数组 / 自定义结构体
Geom p  = Geom::createPoint(3.0, 4.0);
Geom p2 = Geom::createPoint(cv::Point3f(1, 2, 9));          // .z 丢弃 → (1,2)
Geom p3 = Geom::createPoint(std::vector<double>{7, 8, 99}); // 取前两个

// 线：按序连线，不闭合
Geom line = Geom::createLine(std::vector<cv::Point2d>{ {0,0}, {3,0}, {3,4} });
Geom lf   = Geom::createLineFromFlat(std::vector<float>{0,0, 0,5});      // [x1,y1,x2,y2,...]
Geom ln   = Geom::createLineFromNested(std::vector<std::vector<double>>{{0,0},{0,6}});

// 面：自动闭合（首尾不一致内部补点；GEOS ring 闭合是硬性要求，此处已兜底）
Geom poly = Geom::createPolygon(std::vector<cv::Point>{ ... });          // OpenCV 轮廓直接传入
Geom pf   = Geom::createPolygonFromFlat(std::vector<double>{0,0, 2,0, 2,2, 0,2});
Geom pn   = Geom::createPolygonFromNested(std::vector<std::vector<int>>{{0,0},{3,0},{3,3},{0,3}});

// WKT 解析（失败返回空对象，不抛异常）
Geom g = Geom::fromWKT("POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))");

// 规则面：圆/椭圆离散化为普通多边形
Geom c  = Geom::createCircle(0, 0, 10.0);                       // 弦高误差 1e-4 自适应
Geom c2 = Geom::createCircle(0, 0, 10.0, 1e-4, 36);             // 强制 36 段
Geom e  = Geom::createEllipse(0, 0, 5.0, 2.0, 30.0);            // 半轴 5/2，长轴转 30°

// OpenCV 矩形/旋转矩形（适配层）
Geom r = GeomCVAdapter::createRect(cv::Rect(10, 20, 30, 40));
Geom e2 = GeomCVAdapter::createEllipse(rotatedRect);
```

### 4.2 度量与拓扑

```cpp
double a = poly.area();                    // 面积（非面返回 0）
double l = line.length();                  // 折线长度（非线返回 0）
BoundingBox2d bb = poly.bounds();          // 轴对齐包围盒（bb.width()/height()）
BoundingBox2d rc = poly.toRect2d();        // bounds 的语义别名（"取 ROI 裁图"场景）
std::vector<Point2d> vs = poly.getExteriorPoints();   // 顶点（渲染/导出用）

double d = c1.distanceTo(c2);              // 最短距离；空对象返回 -1
bool hit = a.intersects(b);                // 接触/重叠（边界接触算）
bool touch = a.touches(b);                 // 邻接（边界接触但不重叠）
bool cross = line1.crosses(line2);         // 拓扑交叉（线穿线/线穿面内部；仅端点相接不算）
bool ok  = poly.isValid();                 // 有效性（自交面 = false，此时 area 语义不可靠）
std::string why = poly.isValidReason();    // 诊断原因："Self-intersection[...]" / "Valid Geometry"
Geom fixed = poly.makeValid();             // 修复无效几何（自交轮廓修复后再布尔/取面积）

// 点专属：取坐标 / 点在几何内判定
double x = pt.getX(), y = pt.getY();       // 非 Point 类型返回 0
bool in  = pt.within(roiPoly);             // GEOS Within：线上/面内算 true，面边界上算 false

// 面专属：质心（面积加权；凹形的质心可能在面外——数学事实非 bug）
Point2d ctr = poly.centroid();

// 类型自省与输出
std::string t = poly.typeName();           // "Point"/"LineString"/"Polygon"/"MultiPolygon"…空对象 "null"
std::string w = poly.wkt();                // WKT 文本（调试打印/落盘/GIS 交换）；空对象返回空串
Geom back = Geom::fromWKT(w);              // 解析失败返回空对象，不抛异常

// 交换格式导出（2D 坐标；空对象返回空串）
std::string gj = poly.toGeoJSON();         // RFC 7946 几何对象（Polygon 含孔洞环，上位机/前端可解析）
std::string dx = poly.toDXF();             // DXF R12 ENTITIES 段（POINT/LINE，CAD 交换/交付图）

// 布尔/形变失败诊断（D1 口径）：结果 isEmpty() 时区分失败与真空
Geom inter = a.intersection(b);
if (inter.isEmpty() && !Geom::lastError().empty())
    ;  // GEOS 拓扑异常导致失败（lastError 含诊断文本）；lastError 空 = 真不相交
// lastError 为线程局部"最近一次 GEOS 诊断"，布尔/形变运算入口清空；WKT 解析失败同样写入

// 多体拆分：union 不相交区域得到 MultiPolygon，拆开后逐区域取面积
Geom merged = p1.unionWith(p2);            // 不相交 → MultiPolygon
for (const Geom& part : merged.splitParts())
    double regionArea = part.area();       // 每个独立区域的面积

// 预处理几何：一个 ROI 面对大量区域反复判定时，先 prepare 提速约一个数量级
PreparedGeom roi = PreparedGeom::prepare(roiPoly);
for (const Geom& defect : defects)
    if (roi.intersects(defect)) ++hitCount;   // 判定走缓存索引
// roi.covers(x) 含边界；roi.contains(x) 严格内部（躺卧在边界上的几何不算包含）
```

### 4.3 布尔与形变（全部返回新对象，原对象不变）

```cpp
Geom inter = a.intersection(b);            // 交集；不相交 → 空几何（isEmpty()==true）
Geom uni   = a.unionWith(b);               // 并集
Geom diff  = a.difference(b);              // 差集
Geom buf   = point.buffer(2.0);            // 膨胀（负值收缩）；quadSegs 控制圆弧平滑度
Geom hull  = line.convexHull();            // 凸包（点集共线时退化为线/点）

bool contained = roiPoly.contains(pt);     // 面包含点/线/面（边界上不算，需含边界先 buffer 微扩）
```

> `makePoint/makeLine/makePolygon/parseWKT/makeCircle/makeEllipse` 是 `.cpp` 内部的
> 底层构建函数（private），对外一律用 4.1 节的 `create*` 静态工厂与 `fromWKT`。

### 4.4 抽稀（密集点链减点）

```cpp
// 抽稀（道格拉斯-普克）：容差内删冗余顶点
Geom sparse = dense.simplify(0.2);          // 0.2mm 容差
Geom safe   = dense.simplify(0.2, true);    // 拓扑保持版（结果合法、稍慢）

// 2D 仿射变换：x'=a·x+b·y+tx, y'=c·x+d·y+ty（Multi/孔洞/嵌套结构全保留）
Geom rot   = poly.transformed(cos, -sin, 0, sin, cos, 0);   // 绕原点旋转 ang
Geom moved = poly.transformed(1, 0, dx, 0, 1, dy);          // 平移
Geom zoom  = poly.transformed(sx, 0, 0, 0, sy, 0);          // 缩放
```

### 4.5 带孔多边形与批量并集

```cpp
// 带孔面：环形/带孔 ROI 必备——面积、包含、相交自动扣除孔洞
//（普通 createPolygon 只能表达外轮廓，孔洞会被当作面内区域算进面积）
Geom ring = Geom::createPolygonWithHoles(
    std::vector<Point2d>{ { 0,0 }, { 40,0 }, { 40,30 }, { 0,30 } },   // 外轮廓
    { std::vector<Point2d>{ { 10,10 }, { 20,10 }, { 20,20 }, { 10,20 } } });  // 孔
double a = ring.area();                    // 1200-100=1100（自动扣孔）
int n = ring.numInteriorRings();           // 1
auto holes = ring.getInteriorRings();      // 全部内环顶点（闭合，首尾相同）
bool inHole = ring.contains(Geom::createPoint(15, 15));   // false（孔内）

// 批量并集：成百缺陷轮廓一次合并（只读指针入参，不消费原对象）
std::vector<const Geom*> defs = { &d1, &d2, &d3 };   // nullptr/空对象自动忽略
Geom all = Geom::unaryUnion(defs);         // 不相交 → MultiPolygon
for (const Geom& part : all.splitParts())
    double regionArea = part.area();

// 最近点对：距离之外再给落点（最小间隙定位）
Point2d pa, pb;
if (a.nearestPoints(b, pa, pb))            // pa 在 a 上、pb 在 b 上；失败=false
    double gap = /* pa 到 pb 距离 */;

// 带公差距离判定（比 distanceTo 后手比较快，GEOS 原生短路）
bool close = a.isWithinDistance(b, 0.05);  // 间隙 ≤ 0.05mm？
```

### 4.6 OpenCV 轮廓 → Geom 的标准姿势

```cpp
// 密集轮廓先抽稀再转多边形，否则布尔运算显著变慢
std::vector<std::vector<cv::Point>> contours;
ImageProcessor::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
for (const auto& c : contours) {
    Geom poly = Geom::createPolygon(c).simplify(0.2);   // 0.2mm 容差抽稀
    if (poly.isValid() && poly.area() > minArea)
        keep.push_back(poly);
}
```

## 5. Stereo 三维点集

```cpp
using namespace shapely;

// 构造：cv::Point3f/Point3d、自定义结构、扁平数组、嵌套数组、包围盒、PCL 点云（适配层）
Stereo s   = Stereo::createFromPoints(pts3d);                     // 元素含 .x/.y/.z 即可
Stereo sf  = Stereo::createFromFlatArray(std::vector<float>{...}); // [x,y,z, x,y,z,...]
Stereo sn  = Stereo::createFromNestedArray(nested);               // 内层取 [0][1][2]
Stereo sb  = Stereo::createFromBox(aabb);                         // AABB/OBB 的 8 个角点
Stereo sc  = StereoPCLAdapter::createFromCloud(pclCloud);         // pcl::PointCloud → Stereo

// 数据管理
bool empty = s.isEmpty();             // 空点集判定（所有度量对空对象安全）
size_t n   = s.size();                // 点数
std::vector<Point3d> pts = s.getPoints();   // 导出全部点（拷贝）

// 度量
AABB3d aabb = s.getAABB();            // 轴对齐盒（三轴独立 min/max；valid=false = 空）
OBB3d  obb  = s.getOBB();             // PCA 有向盒（体积通常远小于 AABB；extents=半长，rotation 行=主轴）
double va  = s.getAABBVolume();       // AABB 体积
double v   = s.getOBBVolume();        // OBB 体积
Point3d c  = s.getCentroid();         // 质心（工件坐标系原点的自然选择）
double d   = s.minDistanceTo(other);  // 最近点对距离（KD 树加速 O(n·log m)）；空返回 -1

// 最近点对（定位工件/夹具干涉检查）
Point3d pa, pb;
s.findClosestPair(other, pa, pb);     // 输出两点集中最近的两个点

// 坐标重合判定（扫描与 CAD 对齐验证；tolerance 传物理精度如 1e-3）
bool coincident = s.hasCoincidentPointWith(other, 1e-3);

// Z 范围与高度过滤（全新对象，原数据不变）
double zmin, zmax;
s.getZBounds(zmin, zmax);
Stereo band = s.filterByZRange(zMin, zMax);

// 坐标系变换
Stereo world = s.transform(T);        // 应用 4x4 行主序矩阵（配准结果 T 的应用端）
Stereo moved = s.translate(dx, dy, dz);

// 2.5D 桥接：剥 Z → 凸包多边形 → 可继续做全部 Geom 面运算
Geom flat = s.projectTo2D();
double coverArea = flat.intersection(roiPolygon).area();

// 点-几何距离与判定（静态工具，2026-11-10 迭代 4）
double dp = Stereo::distanceToPlane(pt, plane4);   // 有符号距离，plane=[a,b,c,d] 除以 |n|
double db = Stereo::distanceToBox(pt, aabb);       // 盒内（含边界）= 0 / 盒外 = 最近面棱角距离
bool inA  = Stereo::pointInAABB(pt, aabb);         // 点在 AABB 内（含边界）
bool inO  = Stereo::pointInOBB(pt, obb);           // 点在 OBB 内（主轴系逐轴 |p_local| ≤ extents）

// KD 树空间索引：对同一目标点集反复查询时建树一次（纯数学无依赖）
shapely::KdTree3 tree;
tree.build(otherPoints);                 // 平均 O(n log n)；空点集返回 false
int idx; double dist;
tree.nearest(query, idx, dist);          // 最近邻（行号 + 欧氏距离）
tree.knn(query, 10, ids, dists);         // K 近邻（有界堆 + 剪枝，升序，k>点数取全部）
std::vector<int> ids; std::vector<double> ds;
tree.radiusSearch(query, 2.0, ids, ds);  // 半径内全部点（含边界），按距离升序
// minDistanceTo / findClosestPair 内部已用 KD 树（百万级扫描点云可用）
double mnn = cloud.meanNearestNeighborDistance();  // 平均最近邻距离（密度自适应选参依据）

// 4x4 变换矩阵工具（配准链常用）
float Tinv[16];
shapely::Stereo::invertTransform(T, Tinv);        // 求逆（工件系 → 扫描系；奇异 false）
shapely::Stereo::composeTransform(T2, T1, T12);   // 串联：out = T2·T1（先 T1 后 T2）
```

## 6. ImageProcessor 二维图像

全部为静态方法，参数为原生 int（业务层直接传 `cv::THRESH_*` 等枚举，隐式转换）。

### 6.1 常用链路（快速上手）

```cpp
using namespace shapely;

// 阈值 → 形态学 → 轮廓 一条链
ImageProcessor::threshold(gray, binary, 100, 255, cv::THRESH_BINARY);
ImageProcessor::morphologyEx(binary, binary, cv::MORPH_OPEN, kernel);

std::vector<std::vector<cv::Point>> contours;
ImageProcessor::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
// 轮廓点坐标是像素中心：50x100 像素矩形轮廓的 Geom 面积 = 49×99

// 最小面积旋转矩形 / 轴对齐包围矩形
cv::RotatedRect rr = ImageProcessor::getMinAreaRect(contours[0]);
cv::Rect rc = ImageProcessor::getBoundingRect(contours[0]);

// 连通域统计（AOI 缺陷计数/面积过滤，比轮廓法直接）
cv::Mat labels; std::vector<int> areas;
int n = ImageProcessor::connectedComponents(binary, labels, areas, 8);
// n 含背景行：areas[0] = 背景像素数，areas[i] = 第 i 域像素面积
// 结构元不用 include imgproc
cv::Mat kernel = ImageProcessor::makeKernel(cv::MORPH_ELLIPSE, cv::Size(5, 5));

// 超大图瓦片（零拷贝，见 Tile 结构注释里的语义契约）
std::vector<Tile> tiles;
ImageProcessor::splitIntoTiles(bigImage, cv::Size(512, 512), 64, tiles);
// tiles[i].image 共享源图内存；命中/映射用 covered_roi，别用 roi
```

### 6.2 阈值化

```cpp
ImageProcessor::threshold(gray, binary, 100, 255, cv::THRESH_BINARY);
// type: cv::THRESH_BINARY / BINARY_INV / TRUNC / TOZERO

// 光照不均场景：block_size 必须为奇数（3/5/11）；C 为从均值中减去的偏移
ImageProcessor::adaptiveThreshold(gray, binary, 255,
                                  cv::ADAPTIVE_THRESH_GAUSSIAN_C, 11, 2);
```

### 6.3 形态学

```cpp
cv::Mat k = ImageProcessor::makeKernel(cv::MORPH_ELLIPSE, cv::Size(5, 5));
ImageProcessor::dilate(src, dst, k);    // 亮区扩张
ImageProcessor::erode(src, dst, k);     // 亮区收缩
ImageProcessor::morphologyEx(src, dst, cv::MORPH_CLOSE, k);  // 闭：填小孔
// op: MORPH_OPEN（去噪点）/ CLOSE（填小孔）/ GRADIENT（轮廓）/ TOPHAT / BLACKHAT
```

### 6.4 滤波与降噪

```cpp
ImageProcessor::gaussianBlur(src, dst, cv::Size(5, 5), 0);   // ksize 奇数；sigma=0 由 ksize 推导
ImageProcessor::medianBlur(src, dst, 5);                     // 去椒盐噪声（ksize 奇数且 >1）
ImageProcessor::bilateralFilter(src, dst, 9, 75, 75);        // 保边去噪
// d=邻域直径；sigmaColor 越大越不保边；sigmaSpace 越大邻域越远
```

### 6.5 边缘与梯度

```cpp
ImageProcessor::canny(src, edges, 50, 150);   // threshold2 经验取 2~3 倍 threshold1
ImageProcessor::sobel(src, grad, CV_16S, 1, 0, 3);  // ddepth 用 CV_16S 防负梯度截断
// dx/dy: 求导阶数 0/1/2，且 dx+dy >= 1
```

### 6.6 几何变换

```cpp
ImageProcessor::resize(src, dst, cv::Size(640, 480));                    // 直接指定尺寸
ImageProcessor::resize(src, dst, cv::Size(), 0.5, 0.5, cv::INTER_AREA); // 按比例缩放
// interpolation: INTER_NEAREST/LINEAR(默认)/AREA/CUBIC/LANCZOS4

cv::Mat M = cv::getRotationMatrix2D(center, 30.0, 1.0);
ImageProcessor::warpAffine(src, dst, M, dstSize);        // 仿射（M 2x3）

cv::Mat P = cv::getPerspectiveTransform(srcPts, dstPts);
ImageProcessor::warpPerspective(src, dst, P, dstSize);   // 透视（M 3x3）
```

### 6.7 模板匹配

```cpp
cv::Mat result;
ImageProcessor::matchTemplate(image, templ, result, cv::TM_CCOEFF_NORMED);
cv::Point loc; double score;
ImageProcessor::findBestMatch(result, cv::TM_CCOEFF_NORMED, loc, score);
// method 决定取最小还是最大：SQDIFF 系越小越好，其余越大越好
// TM_CCOEFF_NORMED 即 NCC（归一化互相关）——定位类应用首选

// 掩膜匹配：只统计 mask 非零区域（遮挡/背景干扰）；仅 TM_SQDIFF / TM_CCORR_NORMED 支持
cv::Mat mask = ...;   // 与 templ 同尺寸 8UC1，非 0 = 参与
ImageProcessor::matchTemplateMasked(image, templ, mask, result, cv::TM_SQDIFF);
// 零范数区域的 NaN/Inf 自动归一为最差得分，不会带偏后续解析

// 亚像素定位：整数峰 + 抛物线细化（修正量 [-0.5,0.5] 像素）
cv::Point2f subLoc; double subScore;
ImageProcessor::findBestMatchSubPix(result, cv::TM_CCOEFF_NORMED, subLoc, subScore);

// 多目标：重复特征（Mark 点/焊盘阵列）按得分提取 + NMS 抑制
std::vector<Match2d> hits;
ImageProcessor::findAllMatches(result, cv::TM_CCOEFF_NORMED, /*threshold=*/0.8,
                               /*max_matches=*/20, /*nms_radius=*/5, hits);

// 旋转模板对位（Mark 点带角度）：逐角度搜索 + 亚像素细化
shapely::RotatedMatch2d rm = ImageProcessor::findRotatedMatch(
    image, templ, /*angle_start=*/-30.0, /*angle_end=*/30.0, /*step=*/2.0);
// rm.loc = 模板中心落点（亚像素）、rm.angle = 命中角度（度）、rm.score = 得分
// 角分辨率受模板对称性限制（越不对称越精细）；每个角度一次全图匹配——
// 大图先缩小粗搜属业务侧优化

// 角点候选（Mark 点/基准特征；亚像素定位需另行细化）
std::vector<cv::Point2f> corners;
ImageProcessor::goodFeaturesToTrack(gray, corners, /*max=*/20, /*quality=*/0.1, /*min_dist=*/4.0);

// 亚像素角点细化（典型链路：goodFeaturesToTrack → cornerSubPix）
ImageProcessor::cornerSubPix(gray, corners, cv::Size(11, 11), cv::Size(-1, -1),
                             cv::TermCriteria(cv::TermCriteria::EPS | cv::TermCriteria::COUNT, 30, 0.01));

// 非零点坐标列表（HeightMap 有效格提取等）
std::vector<cv::Point> nzPts;
ImageProcessor::findNonZero(mask, nzPts);
```

### 6.8 金字塔

```cpp
ImageProcessor::pyrDown(src, half);       // 尺寸减半
ImageProcessor::pyrUp(src, double_size);  // 尺寸加倍（信息不可恢复，仅插值显示）

std::vector<cv::Mat> pyramid;
ImageProcessor::buildGaussianPyramid(src, pyramid, 4);  // 输出 level0..level4 共 5 层
```

### 6.9 瓦片与金字塔瓦片

```cpp
// 零拷贝瓦片：roi 尺寸恒 = tile_size（含 overlap 补边），covered_roi 是实际覆盖区
std::vector<Tile> tiles;
ImageProcessor::splitIntoTiles(src, cv::Size(512, 512), 64, tiles);

// 多级瓦片：level 0..max_level，每层独立网格；scale = 1/(1<<level)
std::vector<std::vector<Tile>> levels;
ImageProcessor::buildTilePyramid(src, cv::Size(512, 512), 3, levels);
```

### 6.10 轮廓与形状分析（与 Geom 联动）

```cpp
ImageProcessor::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
// mode: RETR_EXTERNAL（最外层）/ RETR_LIST / RETR_TREE

cv::Mat labels; std::vector<int> areas;
int n = ImageProcessor::connectedComponents(binary, labels, areas, 8);
// labels: 0=背景，1..N=各域；areas[0] 恒为背景面积

cv::RotatedRect rr = ImageProcessor::getMinAreaRect(contour);   // 最小面积旋转矩形
cv::Rect rc       = ImageProcessor::getBoundingRect(contour);   // 轴对齐包围矩形
```

### 6.11 色彩空间转换与灰度增强

```cpp
ImageProcessor::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);     // 检测前处理标配
ImageProcessor::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);       // HSV 通道分离

ImageProcessor::equalizeHist(gray, eq);                      // 全局均衡化
ImageProcessor::clahe(gray, cl, 2.0, cv::Size(8, 8));        // CLAHE：工作照首选（局部增强不过曝）
```

### 6.12 像素级运算与 LUT（模板差分核心）

```cpp
// 查找表：伽马校正/灰度分段映射，O(1)/像素
cv::Mat lutTable(1, 256, CV_8UC1);
for (int i = 0; i < 256; ++i) lutTable.at<uint8_t>(i) = cv::saturate_cast<uint8_t>(255.0 * std::pow(i / 255.0, 1.5));
ImageProcessor::lut(gray, lutTable, dst);

ImageProcessor::bitwiseNot(gray, inv);        // 灰度反转
ImageProcessor::bitwiseAnd(img, mask, out);   // 掩膜合成（ROI 外清零）
ImageProcessor::bitwiseOr(a, b, out);

// 批板 vs 模板差分——AOI 检出的核心手段
ImageProcessor::absdiff(batch, golden, diff);
ImageProcessor::threshold(diff, diffBin, 30, 255, cv::THRESH_BINARY);  // 差异超 30 灰阶 → 疑似缺陷
```

### 6.13 积分图与距离变换

```cpp
// 积分图：任意矩形区域像素和 = 4 次查表 → 大窗口均值/方差 O(1)
cv::Mat sum;
ImageProcessor::integral(gray, sum, CV_32S);   // 输出比源图大 1
// 区域 (x,y,w,h) 的和 = sum(y+h,x+w) - sum(y,x+w) - sum(y+h,x) + sum(y,x)

// 距离变换：前景点到最近背景的距离；局部极大值 = 圆形焊球中心
cv::Mat dist;
ImageProcessor::distanceTransform(bin, dist, cv::DIST_L2, 5);
```

### 6.14 连通分析增强与泛洪填充

```cpp
// 全统计版：面积 + 包围盒 + 质心一次到位（缺陷定位一步完成）
cv::Mat labels, stats, cents;
int n = ImageProcessor::connectedComponentsWithStats(bin, labels, stats, cents, 8);
// stats 第 i 行 5 列：[CC_STAT_LEFT, TOP, WIDTH, HEIGHT, AREA]；cents 第 i 行 = (x,y) 质心

// 泛洪填充：孔洞封闭（就地修改 image）
int px = ImageProcessor::floodFill(img, nullptr, cv::Point(5, 5), cv::Scalar(255),
                                   cv::Scalar(), cv::Scalar(), &filledRect);
```

### 6.15 形状度量（焊球/焊盘/引脚测量）

```cpp
double area = ImageProcessor::contourArea(contour);          // 轮廓面积
bool convex = ImageProcessor::isContourConvex(contour);      // 凸形判定

// 圆拟合（几何最小二乘：Kasa 初值 + Gauss-Newton 精化）——焊球直径/孔位测量
cv::Point2f cc; double radius, rms;
if (ImageProcessor::fitCircle(contour, cc, radius, rms))     // 共线/<3 点 → false
    ;  // rms = 点到圆周残差均方根（拟合质量）；弧段 < 90° 时结果可能偏移

// 折线/轮廓顶点逼近：密集轮廓 → 少量顶点（形状判定 / Geom::createPolygon 前减点）
std::vector<cv::Point> poly;
ImageProcessor::approxPolyDP(contour, poly, 2.0, true);

// RANSAC 圆拟合（抗外点）：焊盘边缘毛刺/粘连场景（fitCircle 纯最小二乘会被拉偏）
std::vector<int> inliers;
cv::Point2f rc; double rr, rrms;
if (ImageProcessor::fitCircleRobust(contour, /*threshold=*/0.05, rc, rr, rrms, inliers, /*seed=*/42))
    ;  // 内点索引在 inliers；固定 seed 结果可复现

// 沿线卡尺（尺寸测量经典）：方向采样 + 梯度极值亚像素边缘
std::vector<cv::Point2f> edges;
int nE = ImageProcessor::caliperEdges(gray, /*p1=*/{5, 20}, /*p2=*/{35, 20},
                                      /*n_samples=*/64, /*polarity=*/1, /*min_grad=*/20.0, edges);
// polarity: 0=任意 / 1=暗→亮 / -1=亮→暗；边缘点位于线段上（t 亚像素）

// 两轮廓 Hausdorff 距离（形状偏差量化，双向最大-最小）
double hd = ImageProcessor::contourHausdorff(measured, golden);

// 批量圆拟合（焊盘阵列/孔阵列）：threshold>0 逐项 RANSAC，<=0 逐项最小二乘
std::vector<std::vector<cv::Point2f>> pads = ...;
std::vector<cv::Point2f> centers; std::vector<double> radii, rmss;
std::vector<unsigned char> valid;   // 与 pads 对齐（退化项 = 0）
int okCount = ImageProcessor::fitCircleBatch(pads, 0.05, centers, radii, rmss, valid, 42);
cv::RotatedRect ell = ImageProcessor::fitEllipse(contour);   // 椭圆拟合（≥5 点）
// ell.size.width/height = 长短轴全长 → 圆焊球直径测量

std::vector<float> line4;   // [vx, vy, x0, y0]
ImageProcessor::fitLine(points, line4, cv::DIST_HUBER);      // DIST_HUBER 抗外点

cv::Point2f cc; float radius;
ImageProcessor::minEnclosingCircle(contour, cc, radius);     // 最小外接圆

double d = ImageProcessor::pointPolygonTest(contour, pt, true); // 内正外负的带符号距离
double sim = ImageProcessor::matchShapes(c1, c2, cv::CONTOURS_MATCH_I1); // 越小越相似

// 凸性缺陷：引脚变形/缺料（凹陷深度超阈值即缺陷）
std::vector<int> hull; cv::convexHull(contour, hull, false, false);
std::vector<cv::Vec4i> defects;
ImageProcessor::convexityDefects(contour, hull, defects);
```

### 6.16 直方图（批次一致性）

```cpp
std::vector<float> h1, h2;
ImageProcessor::calcHist8U(gray1, h1);                        // 256 bins
ImageProcessor::calcHist8U(gray2, h2);
double sim = ImageProcessor::compareHist(h1, h2, cv::HISTCMP_CORREL);  // 1 = 完全一致
// method: CORREL（相关）/ CHISQR / INTERSECT / BHATTACHARYYA（0 = 一致）
```

### 6.17 重映射与极坐标展开

```cpp
// 畸变校正：initUndistortRectifyMap 的输出直接喂入
cv::Mat map1, map2;   // 由标定流程生成
ImageProcessor::remap(distorted, undistorted, map1, map2, cv::INTER_LINEAR);

// 环形工件展开：圆上等角度缺陷 → 展开图中等间距（点阵 COB/圆形基板）
ImageProcessor::warpPolar(ring, polar, cv::Size(360, 50),
                          cv::Point2f(50, 50), 50.0, cv::WARP_POLAR_LINEAR);
```

### 6.18 绘图标注（结果可视化）

```cpp
// 标注原语全部在库内（业务层不必为此 include imgproc）；img 为空一律 no-op
ImageProcessor::drawRect(annotated, defectRect, cv::Scalar(0, 0, 255), 2);  // 缺陷框
ImageProcessor::drawPolylines(annotated, contour, true, cv::Scalar(0, 255, 0), 1); // 轮廓描边
ImageProcessor::fillPolygon(annotated, contour, cv::Scalar(0, 0, 255));     // 填充
ImageProcessor::drawCircle(annotated, center, 5, cv::Scalar(255), -1);      // 实心圆
ImageProcessor::drawCross(annotated, center, 6, cv::Scalar(0, 255, 255));   // 十字定位标
ImageProcessor::drawText(annotated, "01", org, cv::Scalar(0, 255, 255));    // 编号（Hershey 字体，不支持中文）
```

### 6.19 统计与卷积

```cpp
// 区域亮度/对比度量化（可带掩膜；空图安全）
cv::Scalar mean, stddev;
ImageProcessor::meanStdDev(region, mean, stddev);            // 或 (region, mean, stddev, mask)

int nz = ImageProcessor::countNonZero(binary);               // 非零像素数
ImageProcessor::minMaxLoc(result, mn, mx, mnLoc, mxLoc);     // 极值 + 位置

ImageProcessor::normalize(src, dst, 0, 255, cv::NORM_MINMAX); // 拉满 [0,255]
ImageProcessor::filter2D(src, dst, -1, kernel);              // 自定义卷积（锐化/平滑/梯度）
ImageProcessor::convertScaleAbs(grad, disp, 2.0, 0.0);       // |2x| → 8U 梯度可视化
```

### 6.20 霍夫检测（直线 / 圆）

```cpp
// 直线定位（输入 = 边缘图，先 canny）
std::vector<cv::Vec4i> lines;
ImageProcessor::houghLinesP(edges, lines, 1.0, CV_PI / 180.0, 50, 20.0, 5.0);

// 圆定位（输入 = 灰度图；内部自带 Canny）
std::vector<cv::Vec3f> circles;   // [x, y, r]
ImageProcessor::houghCircles(gray, circles, 1.0, 30.0, 100.0, 30.0, 5, 25);
// 注意：HOUGH_GRADIENT 对锐利二值图可能一无所获（实测）——先 gaussianBlur 平滑
```

## 7. CloudProcessor / CloudIO 三维点云

模块划分对齐 Open3D 0.19/0.20 的 `PointCloud` API（下表左侧为 Open3D 方法，右侧为本库接口）。

### 7.1 滤波与降采样（filter）

```cpp
using CP = shapely::CloudProcessor<pcl::PointXYZ>;   // 白名单：PointXYZ/XYZI/XYZRGB
using shapely::FitMethod;

// Open3D: voxel_down_sample
auto down  = CP::filterVoxelGrid(cloud, 0.5);                     // 体素 0.5mm
auto down2 = CP::filterVoxelGrid(cloud, 0.5, 10);                // 体素内 ≥10 点才保留
// Open3D: voxel_down_sample_and_trace —— 回溯每个输出点对应的原始点索引
std::vector<std::vector<int>> trace;
auto traced = CP::filterVoxelGridAndTrace(cloud, 0.5, trace);
// Open3D: random_down_sample / uniform_down_sample / farthest_point_down_sample
auto rnd = CP::filterRandomSample(cloud, 5000, 42);              // 固定种子可复现
auto uni = CP::filterUniformSample(cloud, 10);                   // 每 10 点取 1
auto fps = CP::filterFarthestPointSample(cloud, 1000);           // 空间均匀采样
// Open3D: remove_non_finite_points / remove_duplicated_points
auto clean = CP::filterRemoveNonFinite(cloud);                   // 去 NaN/Inf
auto uniq  = CP::filterRemoveDuplicates(cloud, 1e-3);            // 容差去重

// PCL 原生（Open3D 无对应）
auto band  = CP::filterPassThrough(cloud, "z", zMin, zMax);      // 直通（高度带）
auto sor   = CP::filterStatisticalOutlier(cloud, 20, 1.0);       // 统计去噪
auto ror   = CP::filterRadiusOutlier(cloud, 1.5, 3);             // 半径去噪
auto cond  = CP::filterConditionalXYZ(cloud, x0, x1, y0, y1, z0, z1);
```

### 7.2 提取与裁剪（extract）

```cpp
auto sub   = CP::extractByIndices(cloud, idx);                   // select_by_index
auto rest  = CP::extractByIndicesInverted(cloud, idx);           // select_by_index(invert)
const float lo[3] = { x0, y0, z0 }, hi[3] = { x1, y1, z1 };
auto box   = CP::extractCropBox(cloud, lo, hi);                  // crop(AABB)
auto inObb = CP::extractCropOBB(cloud, obb, false);              // crop(OBB)
auto outObb= CP::extractCropOBB(cloud, obb, true);               // crop(OBB, invert=True)
auto inPoly= CP::extractByPolygonROI(cloud, polygon, false);     // 2D 多边形 ROI（XY 射线法，invert=保留外部）
// polygon 为 XY 顶点表（首尾闭合与否均可，<3 点返回 nullptr）；
// 边界点为半开归属语义——需含边界先把 ROI 外扩微量
```

### 7.3 空间变换（trans）

```cpp
auto world = CP::transApplyMatrix(cloud, T);                       // transform（扫描系 → 工件系）
auto rotO  = CP::transRotate(cloud, 0, 0, 1, 3.14159);            // rotate(R)：绕原点转
auto rotP  = CP::transRotateAround(cloud, pivot, 0, 0, 1, 3.14159);// rotate(R, center)：绕指定点转
auto moved = CP::transTranslate(cloud, 1.0, 0, 0);                 // translate
const double sc[3] = { 0.001, 0.001, 0.001 };
auto meter = CP::transScale(cloud, sc);                            // scale（mm → m）
auto zoom  = CP::transScaleAround(cloud, sc, center);              // scale(scale, center)
auto proj  = CP::transProject(cloud, plane_coeffs);                // 投影到平面（PCL 原生）
```

> `transRotate` 绕原点（点云坐标系原点）；`transRotateAround` 轴过指定 `pivot` 点，等价于
> `translate(-pivot) → rotate → translate(+pivot)`。业务上绕工件中心转通常用后者。

### 7.4 特征（feature）

```cpp
auto normals = CP::featureNormalEstimation(cloud, 20);                    // estimate_normals(KNN)
auto radN    = CP::featureNormalEstimationRadius(cloud, 2.5, 10);         // estimate_normals(radius+max_nn)
auto up      = CP::featureOrientNormalsToDirection(normals, dir);         // orient_normals_to_align_with_direction
auto toCam   = CP::featureOrientNormalsTowardsPoint(cloud, normals, eye); // orient_normals_towards_camera_location
auto global  = CP::featureOrientNormalsConsistentTangentPlane(cloud, normals, 30); // orient_normals_consistent_tangent_plane
auto fpfh    = CP::featureFPFH(cloud, normals, 5.0);                      // compute_fpfh_feature（33 维）
std::vector<std::array<float, 9>> covs;
CP::featureCovariances(cloud, 20, covs);                                  // estimate_covariances
std::vector<double> md;
CP::featureMahalanobisDistance(cloud, md);                                // compute_mahalanobis_distance
std::vector<float> nnDist = CP::featureNearestNeighborDistance(cloud);    // compute_nearest_neighbor_distance
```

### 7.5 分割与聚类（segment / fit）

```cpp
// Open3D: segment_plane
std::vector<float> coeffs; std::vector<int> inliers;
bool ok = CP::fitPlane(cloud, FitMethod::RANSAC, 0.02, coeffs, inliers);
CP::segmentPlaneRANSAC(cloud, 0.02, inCloud, outCloud, coeffs);

// Open3D: cluster_dbscan（PCL 1.9 无 DBSCAN，内部 KdTree + 并查集）
std::vector<int> labels;
int nClusters = CP::segmentDBSCAN(cloud, 1.0, 5, labels);   // labels[i] = -1 为噪声

// Open3D: detect_planar_patches 的平滑区域语义
auto regions = CP::segmentRegionGrowing(cloud, normals, 30, 5.0, 0.5, 50, 1000000);

// PCL 原生
auto clusters = CP::segmentEuclideanCluster(cloud, 1.5, 50, 100000);   // 按点数降序
CP::fitSphere(cloud, FitMethod::RANSAC, 0.02, center, radius, inliers);   // 球仅 RANSAC/LMEDS
CP::fitCylinder(cloud, FitMethod::RANSAC, 0.02, axis, pt, radius, inliers); // 圆柱仅 RANSAC

// ⚠️ 拟合/分割家族（fit/segment/measureFlatness/fitCircle3D/厚度图）入口统一剔除
// NaN/Inf 点（Debug 回归实测：NaN 进 RANSAC 会触发 PCL KdTree 断言中断；
// Release 静默 no solution）。返回的 inlier 索引仍指向**原始**输入点云行号。

// 3D 空间圆拟合（孔位/焊球测量）：主平面 → 投影 → 2D 圆拟合 → 回投
shapely::Point3d c3; float n3[3]; double r3, rms3; std::vector<int> in3;
if (CP::fitCircle3D(cloud, FitMethod::RANSAC, /*threshold=*/0.05, c3, n3, r3, in3, rms3))
    ;  // c3=圆心，n3=平面单位法向，r3=半径；同一阈值也用于圆周内点判定

// 圆锥拟合（沉头孔/倒角口）：直接构造 PCL 圆锥模型 + RANSAC
//（绕开 SACSegmentationFromNormals 的 PCL 1.9 缺陷：其 CONE 分支用默认值 0/0
//  覆盖开角约束，任何真实圆锥都 0 局内点）
std::vector<float> apex, coneAxis; double coneAng = 0; std::vector<int> coneIn;
if (CP::fitCone(cloud, FitMethod::RANSAC, /*threshold=*/0.3, apex, coneAxis, coneAng, coneIn))
    ;  // apex=顶点，coneAng=半开角（弧度）。顶点由三切平面交解析求得，
       // 法线角度误差会放大成顶点漂移——阈值需覆盖（法线无定向不影响）

// 多平面迭代分割（阶梯/挡墙类多平面工件）：拟合 → 剔内点 → 重复
std::vector<std::vector<float>> planes; std::vector<std::vector<int>> planeIn;
int n = CP::segmentPlanesMulti(cloud, /*threshold=*/0.05, /*max_planes=*/5,
                               /*min_inliers=*/30, planes, planeIn);
// 内点索引相对原始 cloud；平面按检出顺序（点数降序趋势）
```

### 7.6 关键点（keypoint）

```cpp
auto harris = CP::keypointHarris3D(cloud, 1.0, 1e-4);   // Harris 3D 角点（radius, threshold）
auto iss    = CP::keypointISS3D(cloud, 1.5, 1.0);       // ISS 内在形状特征（salient, non_max）
```

| 接口 | 算法 | 特点 | 典型用途 |
|------|------|------|----------|
| `keypointHarris3D` | Harris 3D | 对棱角/角点敏感，响应阈值越小保留越多 | 刚性件角点定位、配准控制点 |
| `keypointISS3D` | ISS（Intrinsic Shape Signature） | 对旋转/缩放稳定，基于特征值比 | 自由曲面、无序点云配准 |

> 两者内部都转 `PointXYZ` 处理（PCL 只支持 XYZ 输入），返回点云坐标为关键点位置，不保留原索引。Open3D 无对应方法（其关键点检测在 `pipelines` 之外未提供）。

### 7.7 配准（regist）

```cpp
float T[16];
CP::registICP_PointToPoint(source, target, 2.0, T);              // registration_icp(point-to-point)
CP::registICP_PointToPlane(source, target, target_normals, 2.0, T); // point-to-plane
CP::registGICP(source, target, 2.0, T);                          // registration_generalized_icp
CP::registNDT(source, target, 1.0, T);                           // registration_ndt
CP::registPCA(source, target, T);                                // PCA 粗配准（无 Open3D 对应）
CP::registFeatureMatching(src, tgt, srcFpfh, tgtFpfh, 1.0, 2.0, T); // ransac_based_on_feature_matching

// evaluate_registration / get_information_matrix_from_point_clouds
double fitness = 0, rmse = 0;
CP::registEvaluate(source, target, T, 1.0, fitness, rmse);       // 内点占比 + 内点 RMSE
double info[36];
CP::registInformationMatrix(source, target, T, 1.0, info);       // 6x6，顺序 [r, t]
```

### 7.8 表面重建（surface）

#### 7.8.1 包络面（点集 → 轮廓/包络点云）

```cpp
auto hull    = CP::surfaceConvexHull(cloud);                     // compute_convex_hull
auto concave = CP::surfaceConcaveHull(cloud, 2.0);               // create_from_point_cloud_alpha_shape
```

| 接口 | PCL 实现 | 适用场景 | 输出 |
|------|----------|----------|------|
| `surfaceConvexHull` | `ConvexHull` | 求最小凸包络、平面点集的 2D 轮廓 | 包络顶点点云 |
| `surfaceConcaveHull` | `ConcaveHull`（Alpha Shapes） | 贴合凹形轮廓（L 形、带缺口工件） | 包络顶点点云 |

> `alpha` 与 Open3D 同为长度量纲，但数值刻度不同：越小越贴合细节，越大越接近凸包。

#### 7.8.2 平滑与重采样（点云 → 点云）

```cpp
pcl::PointCloud<pcl::Normal>::Ptr nrm;
auto smooth = CP::surfaceSmoothMLS(cloud, 2.5, 2, 0.0, &nrm);   // smooth_mls（voxel>0 时上采样）
```

| 参数 | 含义 | 典型值 |
|------|------|--------|
| `radius` | 局部拟合搜索半径 | 点云平均间距的 2~3 倍 |
| `polynomial_order` | 拟合阶数（1=平面，2=二次曲面） | 2 |
| `voxel_size` | 上采样体素边长（≤0 关闭） | 0 或 0.5×点间距 |

> 内部固定 `setComputeNormals(true)`：PCL 1.9 的上采样路径会无条件写入内部法线缓冲，关闭会导致空指针崩溃。调用方不索取 `out_normals` 时结果被丢弃。

#### 7.8.3 三角网格重建（点云 → Mesh3d）

所有重建接口统一返回 `shapely::Mesh3d`（`vertices` + `triangles` + `valid`），不暴露 `pcl::PolygonMesh`。

```cpp
shapely::Mesh3d mesh;
mesh = CP::surfacePoisson(cloud, normals, 9, 8, 1.0);              // 水密闭合曲面
mesh = CP::surfaceGreedyProjection(cloud, normals, 3.0, 2.5, 100); // 保持原始点作顶点
mesh = CP::surfaceMarchingCubes(cloud, normals, 64);               // Hoppe 符号距离场
mesh = CP::surfaceMarchingCubesRBF(cloud, normals, 64, 0.1);       // RBF 符号距离场
mesh = CP::surfaceGridProjection(cloud, normals, 0.1, 3);          // 体素极值面（流形保证）
mesh = CP::surfaceOrganizedFastMesh(organizedCloud, 1, true);      // 有序点云像素邻接
```

| 接口 | PCL 实现 | 输入要求 | 特点 | 适用场景 |
|------|----------|----------|------|----------|
| `surfacePoisson` | `Poisson` | 法线**朝向一致** | 水密、抗噪、平滑 | 闭合工件、逆向建模 |
| `surfaceGreedyProjection` | `GreedyProjectionTriangulation` | 法线 + 密度较均匀 | 顶点取自原始点、保留细节 | 扫描片、CAD 比对 |
| `surfaceMarchingCubes` | `MarchingCubesHoppe` | 法线 | 隐式场、速度快 | 扫描片快速预览 |
| `surfaceMarchingCubesRBF` | `MarchingCubesRBF` | 法线 | RBF 场更平滑、更慢 | 有噪声/稀疏扫描片 |
| `surfaceGridProjection` | `GridProjection` | 法线 + **每体素邻域 ≥10 点** | 流形保证、可补小孔；输出阶梯状等值面 | 稠密体点云 |
| `surfaceOrganizedFastMesh` | `OrganizedFastMesh` | **有序点云**（height>1），无需法线 | 最快、无插值 | 深度相机/结构光栅格输出 |

**参数速查：**

```cpp
// Poisson：depth 越大细节越多；samples_per_node 1.0 适合低噪，1.5~2 抗噪
CP::surfacePoisson(cloud, normals, /*depth=*/9, /*iso_divide=*/8, /*samples_per_node=*/1.0);

// GP3：search_radius ≈ 点间距×2~5；mu 控制自适应半径；max_neighbors 50~100
CP::surfaceGreedyProjection(cloud, normals, /*search_radius=*/3.0, /*mu=*/2.5, /*max_neighbors=*/100);

// MarchingCubes / RBF：resolution 50~150；RBF 的 off_surface_epsilon 0.1~0.3
CP::surfaceMarchingCubes(cloud, normals, /*resolution=*/64);
CP::surfaceMarchingCubesRBF(cloud, normals, /*resolution=*/64, /*off_surface_epsilon=*/0.1);

// GridProjection：resolution 取点间距 1~2 倍；padding_size 0~3（越大越能补孔）
CP::surfaceGridProjection(cloud, normals, /*resolution=*/0.1, /*padding_size=*/3);

// OrganizedFastMesh：triangle_pixel_size 步长；adaptive_cut 按 z 差异选对角线
CP::surfaceOrganizedFastMesh(organized, /*triangle_pixel_size=*/1, /*adaptive_cut=*/true);
```

#### 7.8.4 网格后处理（Mesh3d → Mesh3d）

```cpp
shapely::Mesh3d calm = CP::surfaceMeshLaplacianSmooth(mesh, 10, 0.5); // 拉普拉斯平滑
```

| 接口 | 作用 | 参数 | 说明 |
|------|------|------|------|
| `surfaceMeshLaplacianSmooth` | Umbrella 算子平滑 | `iterations` 5~20；`lambda` 0.3~0.6 | 顶点数/面数不变；**会使曲面整体收缩**，见下 |
| `surfaceMeshClusterDecimate` | 顶点聚类抽稀 | `cell` 聚类体素边长 | 快速粗糙：cell 内顶点合并为质心，退化/重复面剔除；不保流形——保形减面用 PCL VTK 系 |
| `meshFaceNormals` | 每面单位法线 | — | 按顶点序叉积；退化/越界面 = (0,0,0)；渲染/偏差方向判定 |
| `meshVertexNormals` | 每顶点法线 | — | 相邻面叉积累加 = 面积加权后单位化；孤立顶点 = (0,0,0) |
| `meshCropByOBB` | OBB 裁剪网格 | `box` 有向盒 | 面级过滤（三顶点全在盒内保留），顶点重编号 |
| `measurePointToMeshSignedDistance` | 带符号点-网格距离 | — | 内负外正：精确最近距离 × 射线奇偶符号（固定斜向射线避开棱线）；**仅闭合网格符号有物理意义** |

**拉普拉斯平滑的收缩特性（实测）**：球 R=10、λ=0.5 时，均值半径随迭代单调减小：

| 迭代次数 | 0（原始） | 5 | 10 | 20 | 50 |
|---|---|---|---|---|---|
| 均值半径 | 10.000 | 9.958 | 9.912 | 9.822 | 9.558 |

需要保形时减小 `iterations`/`lambda`。无收缩的 Taubin 平滑（λ/μ 交替）PCL 1.9 未提供，
可按同样邻接结构自行扩展（`p' = p + λ·Δ` 后再做一次 `p'' = p' + μ·Δ`，μ ≈ -λ 略大）。

> **为何不提供 EarClipping**：PCL 的 `EarClipping` 输入是 `PolygonMesh`（可含 >3 顶点面），
> 而本库 `Mesh3d` 只存三角面，无法承载待切分的多边形。若需要处理含四边面的 CAD 网格，
> 请直接用 PCL 原生 `pcl::EarClipping`（`#include <pcl/surface/ear_clipping.h>`）。
> 本库所有重建接口的内部转换（`meshFromPolygonMesh`）已对多边形面做扇形三角化。

#### 7.8.5 重建前处理（关键）

Poisson / GP3 / MarchingCubes / GridProjection 都**要求法线朝向一致**，否则表面会翻面或出现空洞：

```cpp
auto normals = CP::featureNormalEstimation(cloud, 20);
// 方法一：已知朝向（如扫描朝 +Z）
normals = CP::featureOrientNormalsToDirection(normals, up);
// 方法二：已知视点
normals = CP::featureOrientNormalsTowardsPoint(cloud, normals, viewpoint);
// 方法三：无先验，全局一致化（KNN 图 + 最小生成树传播）
normals = CP::featureOrientNormalsConsistentTangentPlane(cloud, normals, 30);
```

> **法线质量由接口自动把关**（无需手工过滤）：
> - 5 个重建接口（Poisson / GP3 / MarchingCubes / RBF / GridProjection）入口统一校验，
>   自动剔除 **NaN / Inf / 零长度** 法线的点；
> - 若有效法线占比 **< 50%**，直接返回 `Mesh3d::valid == false`，避免产出垃圾网格。
>   这拦住的是"上游法线估计整体失效"（如 `featureNormalEstimationRadius` 半径设得过小）。
>
> 因此推荐流程：`featureNormalEstimation`（KNN，不产 NaN）→ 朝向一致化 → 重建。
> 若用 `featureNormalEstimationRadius`，半径务必覆盖点云平均间距的 3 倍以上。

#### 7.8.6 选型建议

```
需要水密闭合曲面？
├─ 是 → surfacePoisson（法线一致 + 点数足够）
└─ 否 → 点云是有序栅格（深度相机）？
        ├─ 是 → surfaceOrganizedFastMesh（最快）
        └─ 否 → 需要保持原始点作顶点？
                ├─ 是 → surfaceGreedyProjection
                └─ 否 → 噪声大 / 稀疏？
                        ├─ 是 → surfaceMarchingCubesRBF 或 surfaceGridProjection
                        └─ 否 → surfaceMarchingCubes（最快）
```

### 7.9 度量（measure）

```cpp
shapely::Point3d mn, mx;
CP::measureMinMax(cloud, mn, mx);                    // get_min_bound / get_max_bound
shapely::AABB3d aabb = CP::measureAABB(cloud);       // get_axis_aligned_bounding_box
shapely::OBB3d  obb  = CP::measureOBB(cloud);        // get_oriented_bounding_box
shapely::Point3d c;
CP::measureCentroid(cloud, c);                       // get_center
double cov[9];
CP::measureMeanAndCovariance(cloud, c, cov);         // compute_mean_and_covariance
std::vector<float> d = CP::measurePointCloudDistance(a, b);  // compute_point_cloud_distance

// 平面度（GD&T 简化口径）：主平面局内点残差峰峰值，外点自动被 RANSAC 剔除
double flatness = 0; std::vector<float> fcoeffs;
if (CP::measureFlatness(cloud, /*threshold=*/0.02, flatness, fcoeffs))
    ;  // fcoeffs = 平面方程（|n|=1）

// 两平面关系（折弯件交棱/直角面判定）
double ang = CP::measurePlaneAngle(fcoeffs.data(), fcoeffs.data());     // 锐角口径 [0,90]
float iPt[3], iDir[3];
if (CP::measurePlaneIntersection(fcoeffs.data(), fcoeffs.data(), iPt, iDir))
    ;  // 交线上一点（最靠近原点）+ 单位方向；近平行 → false

// GD&T 家族（2026-11-12 迭代 12）
double roundness = 0, form = 0, radius = 0;
Point3d cc; float cn[3];
CP::measureCircleRoundness(cloud, FitMethod::RANSAC, 0.05, roundness, cc, cn, radius);
    // 圆度 = 圆周局内点径向残差峰峰值（复用 fitCircle3D）
std::vector<float> cylAxis, cylPt;
CP::measureCylinderForm(cloud, FitMethod::RANSAC, 0.05, form, cylAxis, cylPt, radius);
    // 圆柱度 = 局内点到轴距离与半径之差的峰峰值（复用 fitCylinder）

// 位置度：理论孔位 vs 实测圆心（各孔 fitCircle3D 圆心），逐理论位就近配对
std::vector<Point2d> theory = ..., measured = ...;
std::vector<shapely::PositionDeviation> dev;
int nMatch = CP::measurePositionDeviation(theory, measured, /*match_radius=*/0.5, dev);
// dev[i]: theoretical/measured 索引（-1=未配到）+ dx/dy/dist

// 任意点云对平面方程的有符号残差数组（业务直方图自由度）
std::vector<float> res;
CP::planeResiduals(cloud, /*plane=*/fcoeffs.data(), res);   // 非有限点 = NaN
```

### 7.10 近邻与文件读写

```cpp
// KdTree / Octree（PCL 原生）
std::vector<int> idx; std::vector<float> dist;
CP::treeKdTreeKNN(cloud, query, 10, idx, dist);
CP::treeKdTreeRadiusSearch(cloud, query, 1.5, idx, dist);
CP::treeOctreeVoxelSearch(cloud, query, idx, 1.0);           // res<=0 自动按 1.0
CP::treeOctreeChangeDetection(cloud1, cloud2, 1.0, idx);     // res<=0 拒绝（octree 会挂死）
```

### 7.11 文件读写（CloudIO）

```cpp
// 若在自己的 .cpp 里直接 include <pcl/io/*.h>（如 pcl/io/pcd_io.h）：
// 必须先 include 本库的 BoostCompat.h 垫片（见 §3 全局约定与 BoostCompat.h 头注释），
// 否则 Boost 1.68 + 新版 MSVC STL 编译报 C2039 "seekpos 不是 std::fpos 的成员"
#include "BoostCompat.h"          // ← 必须在 pcl/io 头之前（写法同库内 CloudIO.cpp）
#include <pcl/io/pcd_io.h>

using shapely::CloudIO;                      // 引入类模板（CloudIO 是模板，非别名）
using PointXYZCloudIO = shapely::CloudIO<pcl::PointXYZ>;   // 惯用别名：具名引用具体实例
// 也可直接写 shapely::CloudIO<pcl::PointXYZ>::ioLoadPCD(...)

// PCD / PLY：二进制格式，加载失败返回 nullptr / 保存失败返回 false
auto pcd = PointXYZCloudIO::ioLoadPCD("part.pcd");
PointXYZCloudIO::ioSavePCD("out.pcd", cloud);
auto ply  = PointXYZCloudIO::ioLoadPLY("part.ply");
PointXYZCloudIO::ioSavePLY("out.ply", cloud, /*is_binary=*/true);   // false = ASCII

// TXT：自由列序（默认前 3 列 = X/Y/Z），'#' 开头行跳过
auto txt = PointXYZCloudIO::ioLoadTXT("scan.txt", " ", 0, 1, 2);
PointXYZCloudIO::ioSaveTXT("out.txt", cloud, " ");   // delimiter 空则用空格
// 负列号直接返回 nullptr（防越界）；空格分隔时连续空格视为一个

// 网格（Mesh3d）落盘/读回：OBJ（通用）与 STL（CAD/3D 打印，二进制默认）
shapely::MeshIO::saveOBJ("part.obj", mesh);
shapely::MeshIO::saveSTL("part.stl", mesh, /*is_binary=*/true);
shapely::Mesh3d back = shapely::MeshIO::loadSTL("part.stl");   // 自动识别二进制/ASCII
// STL 二进制坐标为 float32（double 往返 ~1e-6 相对损失，格式固有）

// 高度图序列化（SHMAP1 二进制：元数据 + cells + mask 全量恢复）
shapely::HeightMapIO::save("part.hmap", hm);
shapely::HeightMap hm2 = shapely::HeightMapIO::load("part.hmap");

// 厚度图：上/下表面点云沿 Z 配对求差（板厚/锡膏厚度）
shapely::HeightMap th =
    shapely::CloudProcessor<pcl::PointXYZ>::thicknessMap(topCloud, bottomCloud, /*cell=*/0.1);
// 网格范围取两云包围盒并集；同格双有效 → 厚度 = 上最高 z − 下最低 z，单侧 = NaN

// 高度图 → 灰度图（HeightMapAdapter 适配层，接 ImageProcessor 全部 2D 算子）
cv::Mat gray8 = shapely::HeightMapAdapter::toImage(th);           // 8U min-max 归一化，NaN/空格→0
cv::Mat gray16 = shapely::HeightMapAdapter::toImage(th, true);    // 16U 原值截断 [0,65535]

// 网格度量（Mesh3d）
double area = shapely::CloudProcessor<pcl::PointXYZ>::measureMeshArea(mesh);    // Σ 面面积
double vol = shapely::CloudProcessor<pcl::PointXYZ>::measureMeshVolume(mesh);   // 有向体积（闭合+绕向朝外=真实体积）
std::vector<float> dev =
    shapely::CloudProcessor<pcl::PointXYZ>::measurePointToMeshDistance(cloud, mesh);
// 点到三角面精确最近距离（暴力 O(n·f)，网格大先抽稀）——CAD 比对/重建偏差评估
```

| 接口 | 格式 | 失败表现 |
|------|------|----------|
| `ioLoadPCD / ioLoadPLY` | 二进制 | 返回 nullptr（文件不存在/格式不符） |
| `ioSavePCD` | 二进制 | 返回 false |
| `ioSavePLY` | 二进制/ASCII | 返回 false |
| `ioLoadTXT` | 文本 | 返回 nullptr（文件不存在/负列号/无有效数据行）；列数不足、非数字列（表头）、NaN/Inf 行整行跳过，不再静默当 0 |
| `ioSaveTXT` | 文本 | 返回 false（文件打不开/写入失败）；9 位有效数字（float 往返无损） |

> **模板白名单**：为避免模板传染，PCL 算子只对 `pcl::PointXYZ / PointXYZI / PointXYZRGB` 显式实例化。其他点型（如 PointNormal）需自行扩展白名单并重编本库。

### 7.12 高度图（3D → 2D 桥接）与网格文件（MeshIO）

```cpp
// 点云 XY 栅格化为高度图：接上 ImageProcessor 全部 2D 算子（阈值/滤波/轮廓/连通域/模板匹配）
shapely::HeightMap hm = CP::cloudToHeightMap(cloud, /*cell=*/0.05, /*mode=*/0);
// mode: 0=取最高 z（表面）/ 1=取最低 z（谷底）/ 2=取平均 z / 3=点计数密度（其他值 invalid）
// hm.cells 行优先 index = row*width+col；row 0 = y 最小（imshow 显示时 Y 向下）
// 空栅格 cells=NaN + mask=0；总栅格数上限 2000 万（cell 过小 → valid=false）
// 取值/越界安全：hm.at(row, col)，越界返回 NaN

// 表面重建结果（Mesh3d）落盘/读回：OBJ 通用交换格式（CloudCompare/MeshLab 可读）
shapely::MeshIO::saveOBJ("part.obj", mesh);              // false = 网格无效/索引越界/写失败
shapely::Mesh3d loaded = shapely::MeshIO::loadOBJ("part.obj");
// loadOBJ 支持 f 的 v、v/vt、v/vt/vn、v//vn 四种写法与负索引；
// >3 顶点的面扇形三角化；越界 f 行整行丢弃
```

| 接口 | 格式 | 失败表现 |
|------|------|----------|
| `MeshIO::saveOBJ` | OBJ ASCII | 返回 false（mesh 无效/面索引越界/写入失败） |
| `MeshIO::loadOBJ` | OBJ ASCII | `valid=false`（文件不存在/无有效 v+f 行） |
| `MeshIO::saveSTL` | STL 二进制/ASCII | 返回 false（mesh 无效/索引越界/写入失败） |
| `MeshIO::loadSTL` | STL 自动识别 | `valid=false`（文件不存在/无有效面） |
| `HeightMapIO::save` | SHMAP1 二进制 | 返回 false（hm 无效/写入失败） |
| `HeightMapIO::load` | SHMAP1 二进制 | `valid=false`（不存在/魔数不符/长度不符） |
>
> **Open3D 无直接对应或 PCL 1.9 未提供的算子**：
> - **Ball Pivoting**：PCL 1.9 未编译该模块 → 用 `surfacePoisson` 或 `surfaceGridProjection` 替代；
> - **FGR**（Fast Global Registration）：PCL 1.9 无实现 → 用 `registFeatureMatching`（SAC-IA）+ `registGICP` 两段式替代；
> - **Laplacian / Taubin 点云平滑**：PCL 1.9 无对应 → 点云用 `surfaceSmoothMLS` 投影平滑；三角网格用 `surfaceMeshLaplacianSmooth`；
> - **`detect_planar_patches`**：用 `segmentRegionGrowing` 得到平滑区域后再逐个 `fitPlane`；
> - **`segment_plane` 的 `ransac_n`/`probability` 细粒度控制**：本库固定为 PCL 默认值，需要时可直接改用 PCL 原生 `SACSegmentation`。


## 8. 线程安全

- 底层 GEOS 上下文为线程局部（thread_local）：**多线程各建各的对象并行计算是安全的**
- 单个 `Geom`/`Stereo` 对象不可跨线程并发调用（无内部锁）
- `ImageProcessor`/`CloudProcessor` 为无状态静态方法，多线程安全（`cv::Mat` 输入输出各自独立）

## 9. 完整调用实例

见 [ShapelyAlg_demo.cpp](ShapelyAlg_demo.cpp)（可独立编译运行的三个真实场景）：

| 场景 | 覆盖的 API 链 |
|------|--------------|
| **2D 缺陷区域检出与统计** | 合成图 → `threshold` → `findContours` → `createPolygon(...).simplify()` → `isValid`/`area` 过滤 → `PreparedGeom::prepare` + `intersects` → `bounds`/`centroid`/`intersection` → 标注图输出 |
| **3D 工件凸台测量** | 合成点云 → `Stereo::createFromPoints` → `getAABB`/`getOBB`/`getCentroid` → `filterByZRange` → `projectTo2D().area()` → `minDistanceTo` → `CloudIO::ioSavePLY` |
| **3D 点云表面重建** | 合成球面扫描点云 → `featureNormalEstimation` → `featureOrientNormalsToDirection` + `featureOrientNormalsConsistentTangentPlane` → `surfacePoisson` → 半径精度校验 → `surfaceMeshLaplacianSmooth` → `surfaceGreedyProjection`/`surfaceMarchingCubes`/`surfaceMarchingCubesRBF` 对比 |

构建与运行（本仓库内）：

```bash
cmake --build build --config Release --target ShapelyAlg_demo
./build/output/Release/ShapelyAlg_demo.exe
```

预期输出要点：圆缺陷面积 1890px²、ROI 覆盖率 4.20%、凸台占地 36mm²、凸台高度 2.000mm；
球面重建 4326 顶点 / 8648 三角面，半径最大误差 0.1535mm、平均 0.0095mm；
平滑（5 次）均值半径 9.9760mm（原始 10.0048mm）、平滑（20 次）9.8942mm（拉普拉斯固有收缩）。

运行后在工作目录生成两个输出文件（以当前工作目录为相对路径）：
- `shapely_demo_annotated.png` —— 场景一的缺陷标注图（OpenCV imwrite PNG）
- `shapely_demo_cloud.ply` —— 场景二保存的合成点云（二进制 PLY，`ioSavePLY`）

## 10. 常见坑

| 坑 | 说明 |
|----|------|
| 多边形必须能闭合 | 传乱序/自交点集会得到 `isValid()==false` 的面，此时 `area()` 语义不可靠——先 `isValid()` 检查 |
| 轮廓坐标是像素中心 | findContours 的 50×100 像素矩形，Geom 面积 = 49×99；换算物理面积需乘像素当量² |
| `contains` 不含边界 | 点在多边形边界上返回 false；需要含边界时对目标面 `buffer(1e-6)` |
| 密集轮廓先抽稀 | OpenCV 轮廓动辄数百点，先 `simplify(0.1~0.5)` 再做布尔运算 |
| 点云单位 | 滤波/拟合/树的半径参数都与坐标同单位；mm 点云传 1.0 就是 1mm |
| Debug 运行时 | Debug 版需要 `pcl_*_debug.dll`、`opencv_world440d.dll` 等另一套，别混用 |
| 重建前必须统一法线朝向 | Poisson/GP3/MarchingCubes/GridProjection 都要求法线一致，否则翻面/空洞；用 `featureOrientNormals*` 系列 |
| 法线含 NaN 会让重建静默失败 | `featureNormalEstimationRadius` 对邻域不足的点输出 NaN。PCL 的 Poisson/GP3/MarchingCubes 遇到 NaN 法线**不报错**（仍返回"成功"）但结果不可用。**本库已在 5 个重建接口统一拦截**：自动剔除非法法线点；若有效法线占比 <50% 则直接返回 `valid==false` |
| `GridProjection` 要求稠密点云 | PCL 1.9 硬编码"每体素邻域 ≥10 点"，稀疏表面点云会被全部跳过 → 返回空网格；应改用 Poisson/GP3 |
| `GridProjection` 输出四边形 | 本库 `Mesh3d` 统一扇形三角化，无需调用方处理 |
| `GridProjection` 尺度/位置 | PCL 1.9 在输入包围盒 >1 时归一化点云却不还原输出：本库补偿了**尺度**，但**绝对位置仍带偏移**（实测 R=2 球输出中心偏离原点约 1.0）。需要绝对坐标时先把点云平移到单位盒内 |
| `OrganizedFastMesh` 需要有序点云 | `height` 必须 > 1（深度相机/结构光栅格）；且内部按视点做遮挡剔除，点云应位于视点前方 |
| `fitCylinder` 必须内部估计法线 | PCL 1.9 的圆柱模型强制要求法线，`SACSegmentation` 无法设置 → 本库内部自动走 `SACSegmentationFromNormals` |
| 点到面 ICP 退化几何 | PCL 1.9 的 LLS 求解在纯平面等退化几何上产出 NaN 变换但 `hasConverged()==true`；本库已加有限性校验并自动回退点到点 ICP |
