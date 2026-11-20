# Point3DAlg —— 通用 3D 模块

`src/Point3DAlg` 是面向 ComUI 宿主的通用 3D 算法库：**SICK Ranger3 扫描数据读取（拼接/交织）**、**点云配准（手动粗调 + 6 策略精配准）**、**量测四件套**（bump 高度 / 平面度 / 共面性 / 金标准 NG 对比）、**通用点云算子 C API**（1.1.0 起：文件读写/滤波/裁剪/变换；1.2.0 起：拟合/分割/搜索/度量/法线/特征/关键点/表面重建/HeightMap 桥接；1.3.0 起：FitModel 增空间直线拟合、FilterCloud 增近似体素降采样）。算法与 UI 物理隔离，ComUI 适配层与节点声明全在 `integration/comui/`。

## 定位与依赖

```
Point3DAlg.dll
 ├─ CommonAlg.dll   (JSON 封装：SickReader 读 recipe/common/Scan3DPart 配置)
 ├─ ShapelyAlg.dll  (CloudProcessor：配准 ICP 家族；仅 PCL_FOUND 时链接)
 ├─ opencv_world440.dll (仅 SaveDepthView/DrawFovBoxes 可视函数)
 └─ PCL 运行时（ShapelyAlg 的传递依赖）
```

核心读取链路（`Point3DAlg_SickReader.cpp`）为**纯 C++ 实现，无 PCL/OpenCV 依赖**；配准与量测层在 `tools/` 下，依赖 PCL，按 `PCL_FOUND` 守卫编译（缺失时库内不含配准/量测层）。

## 文件结构

| 文件 | 说明 |
|------|------|
| `Point3DAlg_SickReader.h/.cpp` | **SICK 扫描读取核心**：.dat 平面布局 → FOV 结构（阶段1编码器）→ 多线程读图（阶段2）→ 单机拼接 → 多相机交织 → 深度图/点云；含 json 配置读取与 PNG/PLY 保存套件 |
| `Point3DAlg_SickIO.cpp` | 一步式 C 接口（ReadSickCloud：folder → 点云，错误码 0/-1/-2/-3/-4）+ 版本/生命周期（当前 1.3.0）+ 通用点云容器 + SICK 深层句柄族实现（不依赖 PCL） |
| `include/Point3DAlg_api.h` | C API 主头：错误码全局约定、`Point3DAlg_PointCloud`/`Point3DAlg_Mesh`/`Point3DAlg_HeightMap` 容器、全部点云算子声明（各接口 params/输出布局表以本头注释为权威） |
| `include/Point3DAlg_SickApi.h` | SICK 深层 C 接口声明（句柄族；C++ 宿主头，含输出缓冲布局与错误码口径） |
| `Point3DAlg_CloudOps.cpp` | 点云算子 C 封装（1.1.0 LoadCloud/SaveCloudEx/Filter/Extract/Transform + 1.2.0 FitModel/SegmentCloud/Search/Measure/Normals/Feature/Keypoints/Surface/MeshOp/HeightMap 族；封装 ShapelyAlg::CloudIO/CloudProcessor，点型 PointXYZ，**PCL_FOUND 守卫编译**） |
| `Point3DAlg_test.cpp` | C API 测试：A 部分 = 通用接口（合成数据 A1~A13，恒跑）；B 部分 = SICK 端到端 + 深层接口（依赖 `Z:/ScanImage/3D` 外部数据，**不注册 ctest**，数据缺失打印 SKIP） |
| `tools/Point3DAlg_Regist.h/.cpp` | 配准算法层（纯函数零 UI）：6 策略 ICP-P2P/ICP-P2PL/GICP/FPFH+RANSAC/NDT/PCA + 手动变换操作 + 变换读写 |
| `tools/Point3DAlg_RegistTool.cpp` | 手动配准交互工具（PCLVisualizer + 键盘，独立 exe；键位限制见 tools/README.md） |
| `tools/Point3DAlg_RegistTest.cpp` | 配准算法层单测（26 check，ctest） |
| `tools/Point3DAlg_DomeFit.h/.cpp` | **拱形球顶拟合**：稳健基面 → 聚类 → Kåsa 球拟合 → bump 高度/apex/球心/R + 统计（含共面性两口径） |
| `tools/Point3DAlg_DomeFitTest.cpp` | 球顶拟合测试（合成自检注册 ctest；带 PLY 参数 = 真实数据对照 `scripts/_wafer_dome_baseline.py` 基准断言） |
| `tools/Point3DAlg_RegionPlane.h/.cpp` | **ROI 平面量测**：XY 矩形稳健平面拟合 → 高度/平面度 PV/σ/系数 |
| `tools/Point3DAlg_RegionPlaneTest.cpp` | 平面量测测试（18 check，ctest） |
| `tools/Point3DAlg_CloudDiff.h/.cpp` | **点云高度差对比**：栅格 z 中位 → 逐格差值统计 + 超差判定 + 差值点云；可选 XY 粗对齐（格域 RMS 搜索 + 重叠 70% 门槛） |
| `tools/Point3DAlg_CloudDiffTest.cpp` | 高度差测试（20 check，ctest） |
| `tools/Point3DAlg_SickSynthTest.cpp` | SICK 拼接合成回归（自造 .dat：列映射/交织/偏移对齐/FOV 框/蛇形 2 pass/自然序，40 check，ctest） |
| `tools/Point3DAlg_FitUtil.h` | 量测数值内核（MedianOf/FitPlaneZ，header-only，量测家族共用） |
| `tools/Point3DAlg_CircleFit.h/.cpp` | **圆/孔位量测**：高度切片 → 聚类 → Kåsa 圆拟合（圆心/孔径/RMS），FitCircle2D 支持独立轮廓点 |
| `tools/Point3DAlg_Profile.h/.cpp` | **轮廓截面量测**：线段带内投影 → 弧长分格 z 中位 → 剖面点列 + 台阶落差 max_step |
| `tools/Point3DAlg_BumpReport.h/.cpp` | **bump 全参数报表**：高度+截面直径+共面性偏差+NG 位一体（DomeFit×CircleFit 组合） |
| `SickReader::StitchQuality` | **拼接质量自检**：多相机衔接带格级 z 差统计（σ/超差检出） |
| `tools/README.md` | 配准工具专项文档（键位表/已知键位问题/精度说明） |

ComUI 适配层：`integration/comui/Point3DAlg_comui.cpp`；节点声明：`integration/comui/point3d_*.node.json`（9 个）。

## 接口速查

### C ABI（1.2.0 起共导出 42 个：25（1.1.0）+ 17（1.2.0 P1/P2）；1.3.0 无新导出，FitModel/FilterCloud 扩展取值域）

| 函数 | 用途 |
|---|---|
| `Point3DAlg_ReadSickCloud(folder, frame, recipe, common, &xyz, &count)` | 一步式：扫描文件夹 → SoA 点云（mm）。返回 0 / -1 参数 / **-2 无 .dat / -3 帧越界** / -4 配置数据错误 |
| `Point3DAlg_FreeBuffer` | 释放 ReadSickCloud / SICK 深层接口 / FitModel 内点 / Normals·Feature·MeshOp 数组输出 |
| `Point3DAlg_LoadSickConfig(recipe, common, &cfg, err, len)` | 读配方/通用参数 json → SickConfig（错误码 1~6 + err_msg） |
| `Point3DAlg_SaveCloud / SaveDepth / SaveDepthView / DrawFovBoxes` | PLY 点云 / 16 位深度 PNG / 8 位可视 PNG / FOV 框标注 |
| `Point3DAlg_GetVersion / Init / Cleanup` | 版本（"1.3.0"）/ 初始化（占位）/ 清理（占位） |
| `Point3DAlg_FreeCloud(&cloud)` / `FreeClouds(segments, count)` | 释放 `Point3DAlg_PointCloud` / `SegmentCloud` 段数组（均 NULL 安全） |
| `Point3DAlg_LoadCloud(path, type, delimiter, &out)` | 读点云文件（0=PCD 1=PLY 2=TXT；TXT 分隔符 NULL=空白切分） |
| `Point3DAlg_SaveCloudEx(path, type, &cloud, binary)` | 写点云文件（binary 仅 PLY 生效；PCD 恒二进制、TXT 恒文本） |
| `Point3DAlg_FilterCloud(&in, method, params, n, &out)` | 滤波/降采样 11 method（0=voxel … 9=conditionalXYZ、10=approxVoxel 近似体素（1.3.0），params 布局见 api.h 表） |
| `Point3DAlg_ExtractCloud(&in, method, indices, n, params, m, &out)` | 提取/裁剪 5 method（0=byIndices … 4=cropPolygonROI） |
| `Point3DAlg_TransformCloud(&in, method, params, n, &out)` | 空间变换 7 method（0=applyMatrix(4x4 行主序) … 6=project） |
| `Point3DAlg_FitModel(&in, method, fit, threshold, coeffs, cap, &inliers, &n)` | 通用拟合 6 model（0=plane[4] 1=sphere[4] 2=cylinder[7] 3=circle3D[7] 4=cone[7] 5=line3D[6] 空间直线（1.3.0））× 3 fit（0=RANSAC 1=MSAC 2=LMS）；capacity 不足 → -3 回填所需长度 |
| `Point3DAlg_SegmentCloud(&in, method, params, n, &segs, &count)` | 分割 5 method（0=planeRANSAC 1=euclideanCluster 2=dbscan 3=regionGrowing 4=planesMulti）→ 段数组（FreeClouds 释放） |
| `Point3DAlg_KnnSearch(&in, query[3], k, &idx, &dist, &n)` | KdTree KNN（dist = 欧氏距离升序；k<=0 → -3） |
| `Point3DAlg_RadiusSearch(&in, query[3], r, &idx, &dist, &n)` | KdTree 半径搜索（dist = 欧氏距离**升序**，封装层排序——FLANN 半径搜索原生无序；r<=0 → -3；0 命中属成功） |
| `Point3DAlg_FreeSearchResult(idx, dist)` | 释放 KNN/半径搜索两缓冲（NULL 安全，不复用 FreeBuffer） |
| `Point3DAlg_MeasureCloud(&in, &other, method, params, n, vals, cap, &n)` | 度量 8 method（0=AABB[6] 1=OBB[15] 2=质心[3] 3=均值协方差[12] 4=点云距离[N] 5=平面度[3] 6=平面夹角[1] 7=平面交线[6]；6/7 由 params 传两面方程） |
| `Point3DAlg_EstimateNormals(&in, method, param, &normals, &n)` | 法线估计（0=KNN k 1=radius；输出 N×4 [nx,ny,nz,curvature]，FreeBuffer 释放） |
| `Point3DAlg_ComputeFeature(&in, type, param, &feat, &dim, &n)` | 特征描述（0=FPFH dim33 1=协方差 dim9 2=马氏 dim1 3=最近邻距离 dim1；FreeBuffer 释放） |
| `Point3DAlg_DetectKeypoints(&in, type, radius, threshold, &out)` | 关键点（0=Harris3D 1=ISS3D；输出点云 FreeCloud 释放；0 检出 → -2） |
| `Point3DAlg_ReconstructSurface(&in, type, params, n, &mesh)` | 表面重建 9 type（0=convexHull … 8=organizedFastMesh；type3~7 内部自动估法线+朝向一致化；Mesh 容器 FreeMesh 释放） |
| `Point3DAlg_MeshOp(&mesh, op, params, n, &out, &scalar, &arr, &n)` | 网格处理 8 op（0=平滑 1=抽稀 2=OBB裁剪 → out_mesh；3=面积 4=体积 → scalar；5=点距 6=面法线 7=顶点法线 → arr，FreeBuffer 释放） |
| `Point3DAlg_FreeMesh(&mesh)` | 释放 `Point3DAlg_Mesh`（NULL 安全） |
| `Point3DAlg_CloudToHeightMap(&in, cell, mode, &hm)` | 3D→2D 桥：XY 栅格化（mode 0=最高 z 1=最低 2=均值 3=点密度） |
| `Point3DAlg_HeightMapToCloud(&hm, &out)` | 高度图 → 点云（每有效格一个格心点） |
| `Point3DAlg_ThicknessMap(&top, &bot, cell, &hm)` | 厚度图（上下表面沿 Z 配对求差） |
| `Point3DAlg_FreeHeightMap(&hm)` | 释放 `Point3DAlg_HeightMap`（NULL 安全） |
| `Point3DAlg_CreateSickReader / LoadRawInto / FreeSickReader` | SICK 读取句柄生命周期（LoadRawInto 支持 `;` 分隔多相机；recipe/common NULL=沿用句柄配置） |
| `Point3DAlg_GetSickInfo / GetSickRowY / GetSickRaw` | 尺寸状态 / 行中心 Y / 原始 uint16 高度图+编码器 int32（库 malloc 副本，FreeBuffer 释放） |
| `Point3DAlg_GetSickIntensity / StitchQuality / GetFovBoxes` | 强度图+逐点强度 / 拼接质量自检（填充 StitchQualityResult）/ FOV 框 {x,y,w,h}+id |

### 错误码口径（同 DLL 内两族并存，按接口查表，不得跨族套用）

| 族 | 0 | -1 | -2 | -3 | -4 | -99 |
|---|---|---|---|---|---|---|
| SICK 读取族（ReadSickCloud + 深层句柄族） | 成功 | 参数/空指针 | **无 .dat / 未加载 / 无强度 / 无衔接带** | **帧号越界**（StitchQuality 的 threshold<=0 也用 -3） | 配置/数据错误 | 未预期异常 |
| 点云计算族（Load/Save/Filter/Extract/Transform + 1.2.0 Fit/Segment/Search/Measure/Normals/Feature/Keypoints/Surface/MeshOp/HeightMap） | 成功（搜索 0 命中亦成功） | 参数/空指针/空点云 | 空结果/退化/文件打不开/拟合失败/重建失败 | 参数越界（method/params/索引、k<=0、capacity 不足） | 内存不足 | PCL 异常 |

无 PCL 构建时点云计算族整组符号不导出（CMake `PCL_FOUND` 守卫，与 tools/ 配准层同策略，1.2.0 新接口同守卫）；SICK 族与版本/生命周期接口任何构建下都存在。FilterCloud(voxel) 与 ComUI 适配层内联的 `ComuiVoxel` 语义一致（体素质心），可择一使用——一致性已由 `Point3DAlg_test` A6 段（PCL 版 ↔ 测试内参考实现 floor(p/leaf) 分格质心）回归守卫。

### 1.2.0/1.3.0 输出布局速查（权威口径 = api.h 注释）

| 接口 / method | 输出 |
|---|---|
| `FitModel(plane)` | `[a,b,c,d]`（ax+by+cz+d=0，\|n\|=1） |
| `FitModel(sphere)` | `[cx,cy,cz,r]` |
| `FitModel(cylinder)` | `[px,py,pz 轴上点, dx,dy,dz 单位轴, r]`（PCL 圆柱系数序） |
| `FitModel(circle3D)` | `[cx,cy,cz, nx,ny,nz 单位法向, r]`（底层圆周 RMS 未暴露） |
| `FitModel(cone)` | `[ax,ay,az 顶点, dx,dy,dz 单位轴, 半开角弧度]` |
| `FitModel(line3D)` | `[px,py,pz 线上一点, dx,dy,dz 单位方向]`（1.3.0 新增，6 值；三种 fit 均支持） |
| `SegmentCloud(0 planeRANSAC)` | 恒 2 段：[局内点（恒非空）, 其余点（可为空段 count=0，如全部点在平面上）]（平面方程用 FitModel 另取） |
| `MeasureCloud(AABB)` | `[minx,miny,minz,maxx,maxy,maxz]` |
| `MeasureCloud(OBB)` | `[cx,cy,cz, ex,ey,ez 半长, R9 行主序(行=主轴)]` |
| `MeasureCloud(meanCovariance)` | `[mx,my,mz, c00..c22 行主序]`（12 值） |
| `MeasureCloud(pointCloudDistance)` | N 值 = in 每点到 other 最近欧氏距离 |
| `MeasureCloud(flatness)` | `[pv 峰峰值, sigma 局内点残差标准差(复算口径), inlier_count]` |
| `MeasureCloud(planeAngle)` | `[angle_deg]`（锐角口径 [0,90]） |
| `MeasureCloud(planeIntersection)` | `[px,py,pz, dx,dy,dz]`（交线最近原点点 + 单位方向） |
| `EstimateNormals` | N×4：每点 `[nx,ny,nz,curvature]`（半径模式邻域<3 点 → NaN 保留） |
| `Mesh` 容器 | `vertices` 3V float 交错 + `faces` 3F int（每面 3 顶点索引，四边形已扇形三角化） |
| `HeightMap` 容器 | `width/height/x0/y0/cell` + `cells`（行优先 float z，空格=NaN）+ `mask`（1=有点）+ `valid`；宿主按单通道浮点图直用（row 0 = y 最小，与图像坐标系一致） |

释放函数对照：`FreeCloud`（点云）、`FreeClouds`（段数组，需传段数）、`FreeSearchResult`（KNN/半径搜索 int+float 两缓冲）、`FreeMesh`（网格）、`FreeHeightMap`（高度图）、`FreeBuffer`（FitModel 内点 / EstimateNormals / ComputeFeature / MeshOp 的 float 数组输出）——全部 NULL 安全。

### C++ 类 `Point3DAlg_SickReader`

```cpp
Point3DAlg_SickConfig cfg;                       // LoadSickConfig 填充或手工填
Point3DAlg_SickReader reader(cfg);
reader.LoadRaw({"Z:/ScanImage/3D", "Z:/ScanImage/3D-2"});   // 多相机
const float* depth = reader.GetDepth();          // 拼接+交织深度图（mm，0=无效）
int n = 0;
const float* cloud = reader.GetCloud(&n);        // SoA 点云（mm）
std::vector<int> rects, ids;
reader.FovBoxes(rects, ids);                     // FOV 框（可视标注用）
// Raw()/Encoder() 懒构建（首次调用才拼合相机 0 原始数据）
```

坐标约定见 `Point3DAlg_SickReader.h` 头注释（.dat 平面布局、拼接旋转 90°、交织列序、x 步距 = 实测 scanStep×mmPulse/相机数）。

### 量测层（`tools/`，PCL 类型接口）

```cpp
// 拱形球顶拟合：点云 → 每个 bump 的高度/球顶/球心/R + 共面性统计
Point3DAlg_DomeFit(cloud, params, bumps, &base);
Point3DAlg_DomeStats st = Point3DAlg_DomeStatsOf(bumps);
// st.coplanarity（极差法）/ st.fit_sigma（剥离倾斜的最佳拟合面残差）/ st.fit_p·q（翘曲向）

// ROI 平面量测：XY 矩形 → 高度/平面度 PV/σ（台阶差 = 两 ROI 高度相减）
Point3DAlg_RegionPlaneFit(cloud, x, y, w, h, params, r);

// 点云高度差对比：基准/被测 → 格级 z 差统计 + 超差判定 + 差值点云（z=diff）
Point3DAlg_CloudDiff(cloudA, cloudB, params, result, &diffCloud);

// 圆/孔位量测：高度切片 → 聚类 → 每簇圆心/孔径
Point3DAlg_DetectCircles(cloud, z_low, z_high, params, circles);
Point3DAlg_FitCircle2D(xy, count, ...);   // 已有轮廓点时直接拟合

// 轮廓截面：沿 XY 线段提取 z 剖面（台阶落差 max_step 定位）
Point3DAlg_Profile(cloud, x0, y0, x1, y1, params, result, &profile);

// bump 全参数报表：高度+截面直径+共面性偏差+NG 位（一次性输出）
Point3DAlg_BumpReport(cloud, params, entries, &stats);

// 拼接质量自检：多相机衔接带 z 差（σ 大 = 相机漂移/丢帧）
reader.StitchQuality(0.02, quality);

// 配准（手动粗调 + 精配准，变换 4×4 行主序 float[16]）
Point3DAlg_RegistRun(session, src, dst, params, &step);   // 策略 1~6
```

全部纯函数、确定性（无 RANSAC 随机性，量测可复现）；拟合结果统一有限性校验（NaN 判失败不静默输出）。

## ComUI 节点（9 个）

| 节点 id | 功能 | 输出 |
|---|---|---|
| `alg.point3d.read_sick` | 扫描文件夹 → 点云 | cloud |
| `alg.point3d.voxel_downsample` | 体素下采样 | cloud |
| `alg.point3d.manual_regist` | 两片点云 ICP 精配准（初值 16 值参数） | cloud + 消息 |
| `alg.point3d.dome_fit` | bump 高度量测 | cloud（每 bump 一点 z=高度）+ 消息（统计+共面性） |
| `alg.point3d.region_plane` | ROI 高度/平面度 | 消息（台阶差=两节点高度相减） |
| `alg.point3d.cloud_diff` | 金标准 NG 对比（可选 XY 粗对齐，精度 ≈±cell/2） | cloud（差值点云）+ 消息（超差占比/对齐量） |
| `alg.point3d.circle_fit` | 圆/孔位量测（高度切片） | cloud（每孔一点 z=半径）+ 消息 |
| `alg.point3d.profile` | 轮廓截面（台阶落差/剖面） | cloud（剖面点列）+ 消息（max_step 定位） |
| `alg.point3d.bump_report` | bump 全参数报表（高度/直径/NG） | cloud（每 bump 一点 z=高度）+ 消息（统计/NG） |

## 测试与验证

| 入口 | 内容 | ctest |
|---|---|---|
| `Point3DAlg_RegistTest` | 配准 26 check | ✅ |
| `Point3DAlg_DomeFitTest` | 球顶拟合合成 22 check；带 PLY 参数 = moi_wafer 基准断言 | ✅（无参模式） |
| `Point3DAlg_RegionPlaneTest` | 平面量测 18 check | ✅ |
| `Point3DAlg_CloudDiffTest` | 高度差 20 check | ✅ |
| `Point3DAlg_SickSynthTest` | SICK 拼接合成 40 check | ✅ |
| `Point3DAlg_test` | 通用 C API（合成数据 A1~A13，恒跑：滤波/裁剪/变换/IO + 体素一致性 + 拟合/分割/搜索/度量 + 法线/特征/关键点 + 表面重建/MeshOp + HeightMap）+ SICK 真实数据端到端 + 深层句柄族 | ❌（B 部分依赖 Z: 数据，手动跑；数据缺失打印 SKIP） |
| `scripts/_test_comui_*.py` ×3 | ctypes 模拟宿主端到端 | 手动 |
| `scripts/_wafer_dome_baseline.py` | DomeFit Python 基准（数值对照） | 手动 |

## 已知局限与遗留

- **`.xml` 换算兜底预留未实现**：头注释已如实标注，换算完全依赖 recipe/common json
- 16 位深度 PNG 无压缩（zlib 存储块），大图体积大；`zmin` 有效点映射 0 与"无效=0"语义接近（zmin 处会被当无效点，读回时需注意）
- `Point3DAlg_RegistTool` 部分键位受 PCL/VTK 内置行为限制（详见 tools/README.md 已知键位问题）
- `Image2DAlg_Calib` 同模块但属 2D 标定方向，见 Image2DAlg 文档
- **1.2.0 表面重建的两处登记偏差**：① type 0/1（凸包/凹包）在 C 封装层直接调 PCL ConvexHull/ConcaveHull 取回面片——CloudProcessor 同名算子只回顶点云；② type 3~7（法线重建）在封装层自动组合"KNN k=10 估法线 + MST 朝向一致化 k=30"（§9.12 强制约束；SoA 容器无法传入法线），不暴露法线参数
- `ReconstructSurface` type 8（organizedFastMesh）**恒返回 -2**：SoA 点云容器表达不了有序点云（height 恒 1），保留枚举供宿主显式感知
- `FitModel(circle3D)` 未暴露底层圆周残差 RMS（布局保持草案的 7 值），需要时可用 out_inliers 复算
- **FilterCloud(voxel) 与 ComuiVoxel（voxel_downsample 节点）的数值口径**：语义一致（分格/每格质心），逐格质心差 ≤~1.7e-5——PCL 为 float32 顺序累加、ComuiVoxel 为 double 累加，mm 级 leaf 下 ≤20nm 工业无感；非 dyadic leaf 且坐标恰为 leaf 整数倍时分格可能不同（一致性脚本 scripts/_test_comui_voxel_consistency.py 可复查），建议优先 2 的幂 leaf
- `DetectKeypoints(ISS3D)` 数据口径：ISS 显著性要求邻域散布矩阵最小特征值严格 > 0——**无噪声的精确解析曲面**（邻域严格共面，如理想圆柱单层邻域）e3 恒为 0 → 恒 0 关键点（-2）；真实扫描/带噪数据正常（测试用带 z 微噪声圆柱验证，合成无噪数据上定位到的口径坑已在 api.h 注明）
- MeshOp 的法线/距离输出走独立 float 数组（`out_array`，FreeBuffer 释放）——Mesh 容器无顶点属性位，写回不可行
- `ComuiVoxel` ↔ FilterCloud(voxel) 数值一致性已由测试 A6 段守卫（PCL 版 ↔ 参考实现）；ComUI 适配层侧的对账属 integration 侧遗留
- **NaN/Inf 输入口径（1.2.0 review 定稿）**：拟合/分割族（FitModel/SegmentCloud 0/1/2/4、MeasureCloud 0/3/5）底层入口自动剔除非有限点（索引/段映射回原始坐标，dbscan 同样剔除后在封装层映射）；TransformCloud 的旋转轴/平面法向**零向量或含非有限分量 → -3**（NaN 轴原会静默产出全 NaN 点云）；EstimateNormals 逐点保留输出（非有限点对应法线 NaN，契约 N 与输入一一对应）；ReconstructSurface type3~7 入口剔除 NaN/Inf 坐标与 NaN/零长法线点（有效占比 <50% → -2），MeshOp op5 非有限查询点 → NaN 距离；其余算子（Filter/Extract 等）NaN 点透传底层、建议先 method=4 剔除
- **RadiusSearch 升序**：FLANN 半径搜索原生无序（树遍历序），封装层按距离升序稳定排序（indices/dists 同步重排）——api.h"按距离升序"契约对两搜索接口一致成立（回归 A9 段守卫）
