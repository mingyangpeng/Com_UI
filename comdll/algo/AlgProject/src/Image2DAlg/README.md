# Image2DAlg —— 通用 2D 模块

`src/Image2DAlg` 是面向 ComUI 宿主的通用 2D 算法库：图像读取、通用处理、测量/缺陷检测，以及标定接口类（`Image2DAlg_Calib`）。当前版本 **2.3.0**（76 个通用算子：处理 20 + 特征 14 + 测量 37 + 跟踪配准立体 5；另有 Init / ReadImage / GetVersion / Free* 等生命周期与释放接口）。

## 定位与依赖

本模块是 `ShapelyAlg::ImageProcessor` 的**上层**：所有图像算子复用 ImageProcessor 实现，本层只负责参数语义（通道/尺寸/模式）、C 容器与 `cv::Mat` 转换、测量结果结构组装。

```
Image2DAlg.dll
 ├─ ShapelyAlg.dll  (ImageProcessor：滤波/阈值/形态学/边缘/轮廓/匹配)
 ├─ CommonAlg.dll   (标定持久化等)
 └─ opencv_world440.dll
```

交付时按现有 `build/output/Release` 整目录平铺即可（`ShapelyAlg.dll` 已在交付清单内）。

## 文件结构

| 文件 | 说明 |
|------|------|
| `Image2DAlg_api.h` | 对外 C ABI（容器、读写、通用算子、测量结果结构） |
| `Image2DAlg_api.cpp` | 版本/Init/Cleanup/FreeImage/ReadImage |
| `Image2DAlg_Ops.cpp` | 通用处理与测量算子（复用 ImageProcessor） |
| `Image2DAlg_Calib.h/.cpp` | 标定接口类（单目/双目/鱼眼 Mei、立体校正），C++ API |
| `Image2DAlg_test.cpp` | C ABI 测试入口（ctest 第 3 项） |
| `Image2DAlg_CalibTest.cpp` | 标定模拟数据验证（未注册 ctest） |

ComUI 适配层在 `integration/comui/Image2DAlg_comui.cpp`，节点声明在 `integration/comui/image2d_*.node.json`。

## 通道语义

`Image2DAlg_Image { width, height, channels, data }`，`data` 为紧排像素，行步长 `width * channels`。

- 输入支持 **1（灰度）/ 3（BGR）/ 4（BGRA）** 通道，其他通道数返回 `-3`。
- 通用算子**保持输入真实通道数**（1→1、3→3、4→4）；alpha 随通道参与 OpenCV 运算（彩色 CLAHE/双边滤波等需要拆分时保留 alpha）。
- 天然单通道算子输出 1 通道：`ToGray`、`Threshold`、`Canny`、`Enhance(method=0)`。
- 例外：`Image2DAlg_ReadImage` 保持既有宿主契约，输出固定 BGRA 4 通道。
- 输出缓冲一律 `malloc`，用 `Image2DAlg_FreeImage` 释放；测量数组用 `Image2DAlg_FreeBlobs` / `Image2DAlg_FreeContours` 释放（均 NULL 安全）。

## 通用处理算子

| 函数 | 输出通道 | 说明 |
|------|----------|------|
| `Image2DAlg_ToGray(in, out)` | 1 | BGR(A) → 灰度 |
| `Image2DAlg_Gamma(in, gamma, out)` | 同输入 | LUT `255*(v/255)^(1/gamma)`，gamma 钳制 ≥ 0.05 |
| `Image2DAlg_Threshold(in, mode, thresh, maxval, block, C, out)` | 1 | mode：0 固定二值 / 1 反二值 / 2 OTSU / 3 自适应均值 / 4 自适应高斯 |
| `Image2DAlg_Binarize(in, mode, thresh, thresh2, maxval, mid, block, C, invert, out)` | 1 | 统一二值化：mode 0~4 同 Threshold；5 TRUNC / 6 TOZERO / 7 TOZERO_INV；8 双阈值区间内前景（low≤v≤high）/ 9 双阈值区间外前景 / 10 双阈值三值化（low/high/mid）；`invert` 非 0 对结果取反 |
| `Image2DAlg_Blur(in, method, ksize, sigma1, sigma2, out)` | 同输入 | method：0 高斯 / 1 中值 / 2 双边；ksize 自动补奇 |
| `Image2DAlg_Morphology(in, op, shape, ksize, iterations, out)` | 同输入 | op：0 erode / 1 dilate / 2 open / 3 close / 4 gradient / 5 tophat / 6 blackhat；shape：0 矩形 / 1 十字 / 2 椭圆 |
| `Image2DAlg_Canny(in, t1, t2, out)` | 1 | Canny 边缘（0/255） |
| `Image2DAlg_Sobel(in, dx, dy, ksize, out)` | 1 | Sobel 梯度，输出 \|梯度\| 缩放 0~255（dx+dy≥1，ksize 1/3/5/7） |
| `Image2DAlg_Resize(in, w, h, interpolation, out)` | 同输入 | 目标尺寸缩放 |
| `Image2DAlg_Crop(in, x, y, w, h, out)` | 同输入 | ROI 裁剪，越界自动钳制 |
| `Image2DAlg_Rotate(in, angle, scale, interpolation, out)` | 同输入 | 绕图像中心旋转，输出同尺寸 |
| `Image2DAlg_Enhance(in, method, clip, tile, out)` | 1 或同输入 | method 0 直方图均衡（1 通道）/ 1 CLAHE（彩色走 Lab 亮度通道） |
| `Image2DAlg_HSVThresholdWrap(in, h_lo, h_hi, s_lo, s_hi, v_lo, v_hi, out)` | 1 | wrap 版 HSV 分割：h_lo > h_hi 自动按环绕区间（红色跨 0 一跳搞定） |
| `Image2DAlg_WarpAffine(in, m[6], w, h, interpolation, out)` | 同输入 | 仿射变换（2x3 行主序；画布 w/h；越界填黑）——标定对齐用 |
| `Image2DAlg_WarpPerspective(in, m[9], w, h, interpolation, out)` | 同输入 | 透视变换（3x3 行主序） |
| `Image2DAlg_HSVThreshold(in, h_lo, h_hi, s_lo, s_hi, v_lo, v_hi, out)` | 1 | HSV 阈值分割 0/255 掩膜（h∈[0,180]，红色跨 0 需两次调用；灰度图 -3） |
| `Image2DAlg_AbsDiff(in1, in2, out)` | 同输入 | 批板差分：逐像素差的绝对值（尺寸/通道必须一致） |
| `Image2DAlg_Laplacian(in, ksize, scale, out)` | 同输入 | Laplacian 二阶导（2.3.0）：ksize 1/3/5/7；8U/16U 走 CV_16S 中间取绝对值×scale 饱和回原深度，浮点同深度保符号 |
| `Image2DAlg_Filter2D(in, kernel, ksize, out)` | 同输入 | 自定义 k×k 浮点核卷积（2.3.0）：ksize 奇数 1~51、行主序锚点=核中心；8U/16U 走 CV_16S 中间取绝对值饱和回原深度，浮点同深度保符号。中值/双边/高斯由 Blur method 0/1/2 覆盖，不另设入口 |
| `Image2DAlg_DenoiseNLMeans(in, h, h_color, tw, sw, out)` | 同输入 | 非局部均值降噪（2.3.0）：1ch → fastNlMeansDenoising / 3ch → fastNlMeansDenoisingColored / 4ch 拆 BGR 后回贴 alpha（同 Blur 双边先例）；h 亮度强度（>0，建议 3）、h_color 彩色强度（通常 = h）、tw 模板窗（默认 7，自动补奇）、sw 搜索窗（默认 21，须 > tw 否则 -3，自动补奇）；NLMeans 块匹配语义 Blur 未覆盖故单设入口 |

## 测量 / 缺陷检测

| 函数 | 说明 |
|------|------|
| `Image2DAlg_ConnectedComponents(in, connectivity, min_area, max_area, &blobs, &count)` | 连通域统计，返回 `Image2DAlg_Blob{label, area, left, top, width, height, cx, cy}` 数组；`max_area<=0` 表示不设上限 |
| `Image2DAlg_ConnectedComponentsROI(in, rx, ry, rw, rh, connectivity, min_area, max_area, &blobs, &count)` | ROI 版：仅统计 ROI 内前景，结果坐标已平移回全图系；`rw/rh<=0` = 全图，无交集 -3 |
| `Image2DAlg_AnalyzeContours(in, external_only, min_area, &contours, &count)` | 轮廓测量，返回 `Image2DAlg_ContourInfo`：面积、周长、圆度 `4πA/P²`、包围盒、质心、最小外接矩形、最小外接圆 |
| `Image2DAlg_AnalyzeContoursROI(in, rx, ry, rw, rh, external_only, min_area, &contours, &count)` | ROI 版轮廓测量（坐标同上平移回全图系） |
| `Image2DAlg_MatchTemplate(image, templ, method, &score, &x, &y)` | 模板匹配（`method` 同 `cv::TemplateMatchModes` 0~5），返回最佳左上角与得分（SQDIFF 系越小越好） |
| `Image2DAlg_MatchTemplateROI(image, rx, ry, rw, rh, templ, method, &score, &x, &y)` | ROI 版：仅 ROI 内匹配，命中回全图系（模板大于 ROI -3） |
| `Image2DAlg_FindAllMatchesROI(image, rx, ry, rw, rh, templ, method, thr, max, nms, &matches, &count)` | ROI 版多目标匹配（NMS 在 ROI 内，命中回全图系） |
| `Image2DAlg_MatchTemplateMasked(image, templ, mask, method, &score, &x, &y)` | 带掩膜匹配（遮挡/干扰背景鲁棒；仅 SQDIFF/CCORR_NORMED） |
| `Image2DAlg_MatchTemplateRotated(image, templ, angle_start, angle_end, angle_step, method, &score, &cx, &cy, &angle)` | 旋转模板搜索（Mark 点带角度对位），输出**模板中心**落点 |
| `Image2DAlg_FindAllMatches(image, templ, method, score_threshold, max_matches, nms_radius, &matches, &count)` | 多目标匹配（NMS 去重，按得分从优到劣），配 `FreeMatches` |
| `Image2DAlg_MatchTemplateEx(image, templ, method, scale_lo/hi/step, angle_lo/hi/step, &score, &cx, &cy, &scale, &angle)` | **多尺度 × 多角度**组合搜索，取跨尺度全局最优 |
| `Image2DAlg_MatchTemplateSubPix(image, templ, method, &score, &x, &y)` | 亚像素对位（整像素峰 + 抛物线插值 ±0.5px） |
| `Image2DAlg_FindAllMatchesEx(image, templ, method, score_threshold, max_matches, nms_radius, scale_lo/hi/step, angle_lo/hi/step, &matches, &count)` | **多目标 × 旋转 × 尺度阵列全检**（跨组合全局 NMS），`MatchEx{cx, cy, score, scale, angle}` 中心落点；配 `FreeMatchesEx` |
| `Image2DAlg_MatchTemplatePyramid(image, templ, method, scale_lo/hi/step, angle_lo/hi/step, levels, &score, &cx, &cy, &scale, &angle)` | 金字塔粗搜提速版（顶层全范围粗搜 → 逐级邻域精化，典型提速 4~10 倍；模板最小边 ≥12 约束，弱纹理先用 Ex 验证） |
| `Image2DAlg_MatchTemplateMulti(image, templ, method, lo, hi, step, &score, &x, &y, &scale)` | 多尺度模板匹配：步进缩放模板逐一匹配取跨尺度最佳，输出含 `scale`；某尺度越界自动跳过 |
| `Image2DAlg_FitCircle(xy, count, &fit)` | 点集最小二乘圆拟合（Kåsa+SVD），返回 `Image2DAlg_CircleFit{cx, cy, r, rms}`；退化输入可能得无意义圆，用 rms 判断质量 |
| `Image2DAlg_FitLine(xy, count, &fit)` | 点集直线拟合（DIST_L2，按像素取整），返回 `Image2DAlg_LineFit{vx, vy, x0, y0, rms}` |
| `Image2DAlg_FitCircleEx(xy, count, &fit)` | 圆拟合高精度版（Kåsa 初值 + GN 几何精化，封装 ImageProcessor::fitCircle） |
| `Image2DAlg_FitCircleRobust(xy, count, threshold, max_iters, seed, &fit, &inliers, &n)` | 圆拟合 RANSAC 抗外点（毛刺/粘连场景），配 `FreeInliers` |
| `Image2DAlg_FitEllipse(xy, count, &fit)` | 椭圆拟合（≥5 点），返回 `EllipseFit{cx, cy, width, height, angle}`（全轴长；宽高顺序随 OpenCV 角度约定） |
| `Image2DAlg_FitEllipsesFromBinary(in, min_area, max_area, min_axis, max_axis, &fits, &count)` | 多目标椭圆拟合（椭圆焊盘/异形孔）：二值逐外轮廓 fitEllipse + 全轴长过滤；配 `FreeEllipses` |
| `Image2DAlg_FitLinesFromBinary(in, min_area, max_area, min_length, &lines, &count)` | 多目标直线拟合（板边/边缘线）：二值逐外轮廓 fitLine DIST_L2 + 覆盖跨度过滤；配 `FreeLineFits` |
| `Image2DAlg_FitRect(xy, count, &fit)` | 最小外接旋转矩形，返回 `RectFit{cx, cy, width, height, angle}` |
| `Image2DAlg_FitPoly(xy, count, degree, &fit)` | 多项式拟合 y=Σcoef[i]·x^i（degree 1~5，SVD），返回 `PolyFit{coef[6], degree, rms}`——边缘直线度/翘曲趋势 |
| `Image2DAlg_HoughLinesP(in, rho, theta_deg, threshold, min_len, max_gap, &lines, &count)` | 概率霍夫直线（线段端点 `Line4[]`），配 `FreeLines` |
| `Image2DAlg_HoughLinesPROI(in, rx, ry, rw, rh, ...)` | ROI 版霍夫直线（端点回全图系） |
| `Image2DAlg_HoughCircles(in, dp, min_dist, param1, param2, min_r, max_r, &circles, &count)` | 霍夫圆检测（`HoughCircle{cx,cy,r}[]`，锐利二值图先接 Blur 节点），配 `FreeCirclesHough` |
| `Image2DAlg_HoughCirclesROI(in, rx, ry, rw, rh, ...)` | ROI 版霍夫圆（圆心回全图系） |
| `Image2DAlg_RegionStats(in, rx, ry, rw, rh, &st)` | ROI 内灰度统计 `GrayStats{mean, stddev, min, max}`（批次一致性监控） |
| `Image2DAlg_Histogram(in, out_hist[256])` | 灰度直方图 256 bins 计数 |
| `Image2DAlg_CompareHist(hist1, hist2, method, &score)` | 直方图相似度（method 0 CORREL/1 CHISQR/2 INTERSECT/3 BHATTACHARYYA） |
| `Image2DAlg_FitCirclesFromBinary(in, min_area, max_area, min_r, max_r, subpixel, &circles, &count)` | 多目标圆拟合（孔位/焊球阵列）：二值前景逐外轮廓 Kåsa 拟合；`subpixel!=0` 走灰度梯度亚像素细化（~0.3 像素）；配 `FreeCircles` |
| `Image2DAlg_ContourPoints(in, external_only, min_area, &xy, &points, &offsets, &count)` | 轮廓原始点集导出（扁平 [x0,y0,...] + 每条轮廓起始点索引表）；配 `FreeContourPoints` |
| `Image2DAlg_MatchShapes(xy1, count1, xy2, count2, method, &score)` | 形状相似度（Hu 矩，越小越相似，0=一致；method 1/2/3 = cv::CONTOURS_MATCH_I1~I3） |
| `Image2DAlg_ConvexityDefects(xy, count, &defects, &count)` | 凸性缺陷（引脚变形/缺料检测），返回 `Image2DAlg_Defect{start/end/farthest 索引, depth, fx, fy}`；无凹陷 count=0 |

测量输入为非零即前景的二值语义；彩色输入内部先转灰度。无结果时返回 `0` 且 `count=0`。

## 特征点与匹配对位（2.0.0 / 2.1.0）

2.0.0 特征点族（大形变对位前置）：

| 函数 | 说明 |
|------|------|
| `Image2DAlg_HarrisCorners(in, block, ksize, k, thresh_ratio, subpix, &pts, &n)` | Harris 角点（相对最大响应阈值 + 3x3 NMS；`subpix!=0` 亚像素细化） |
| `Image2DAlg_GoodFeatures(in, max_corners, quality, min_dist, subpix, &pts, &n)` | Shi-Tomasi 角点（质量降序 + 最小间距 NMS） |
| `Image2DAlg_CornerSubPix(in, pts, count, win, &out)` | 角点亚像素细化（细化失败保持原位） |
| `Image2DAlg_ORBAlign(in1, in2, n_features, ratio, m[6], &matches, &count, &inliers)` | 一站式：ORB 检测 + Hamming 匹配（Lowe 比率）+ RANSAC 全仿射，`m` 为 in1→in2 的 2x3 行主序；配 `FreeOrbMatches` |

2.1.0 特征链路拆分（检测 → 描述 → 匹配 → 几何估计可独立复用，与 ORBAlign 并存）：

| 函数 | 说明 |
|------|------|
| `Image2DAlg_FastFeatures(in, threshold, nonmax_suppression, &pts, &n)` | FAST 角点（比 Harris 快一个数量级；`threshold<=0` → -3；平坦图无检出返回 0/0）；配 `FreePoint2f` |
| `Image2DAlg_ComputeDescriptors(in, pts, count, type, n_features, &out)` | 关键点描述子计算，type：0=ORB(32B) / 1=BRISK(64B) / 2=AKAZE(61B) / 3=SIFT(浮点 128 维)；`n_features` 仅 ORB 有效；关键点越界/贴边 → -2；输出 `Image2DAlg_Descriptors{type, dim, is_float, count, data, fdata}` |
| `Image2DAlg_FreeDescriptors(d)` | 释放描述子 data/fdata 并置空（结构体归调用方；NULL 安全） |
| `Image2DAlg_MatchFeatures(d1, d2, pts1, pts2, ratio, &matches, &count)` | BFMatcher kNN(k=2) + Lowe 比率：二进制用 Hamming、SIFT 用 L2；ratio ∉ (0,1) → -3，type/dim/is_float 不一致 → -3，匹配 < 2 对 → -2；**复用** `Image2DAlg_OrbMatch` 结构与 `FreeOrbMatches` |
| `Image2DAlg_EstimateAffinePartial(matches, count, thr, m[6], &inliers)` | 部分仿射估计（4 DOF：旋转+等比缩放+平移，工业对位首选）：RANSAC；count < 3 → -2，thr <= 0 → -3；`m` 为 in1→in2 的 2x3 行主序（同 ORBAlign 方向，可接 WarpAffine） |
| `Image2DAlg_FindHomography(matches, count, thr, h[9], &inliers)` | 单应估计（3x3 透视，可接 WarpPerspective）：RANSAC；count < 4 → -2，共线/退化 → -2，thr <= 0 → -3；`h` 为 in1→in2 行主序、h[8]=1 |

典型链路：`FastFeatures → ComputeDescriptors → MatchFeatures → EstimateAffinePartial/FindHomography → WarpAffine/WarpPerspective`。

## 特征扩展与跟踪配准立体（2.2.0）

2.2.0 特征扩展（检测+描述一次完成、斑点、一站式匹配）：

| 函数 | 说明 |
|------|------|
| `Image2DAlg_BriskFeatures(in, n_features, contrast_threshold, &pts, &n, &desc)` | BRISK 检测+描述一次完成：`contrast_threshold` → BRISK `thresh`（<=0 默认 30）；`n_features` 忽略（无上限参数）；描述子 64 字节 |
| `Image2DAlg_AkazeFeatures(...)` | AKAZE 检测+描述：`contrast_threshold` → AKAZE `threshold`（<=0 默认 0.001）；描述子 61 字节；`out_desc` 传 NULL = 只检测 |
| `Image2DAlg_SiftFeatures(...)` | SIFT 检测+描述：`n_features` → `nfeatures`（<=0 不限）、`contrast_threshold` → `contrastThreshold`（<=0 默认 0.04）；浮点 128 维 |
| `Image2DAlg_BlobDetect(in, min_area, max_area, min_circularity, min_convexity, &blobs, &n)` | 斑点检测（SimpleBlobDetector 连通域形状过滤，与 FitCirclesFromBinary 的轮廓圆拟合互补）；面积/圆度/凸性过滤，<=0 = 不过滤该项；暗斑默认（亮斑先反转）；配 `BlobFeature{x,y,radius}` + `FreeBlobFeatures` |
| `Image2DAlg_DetectAndMatch(in1, in2, type, n_features, ratio, &matches, &n)` | 一站式检测+描述+匹配（ORBAlign 泛化版，type 0=ORB/1=BRISK/2=AKAZE/3=SIFT）；复用 `OrbMatch` + `FreeOrbMatches` |

2.2.0 跟踪、配准与立体：

| 函数 | 说明 |
|------|------|
| `Image2DAlg_CalcOpticalFlowPyrLK(prev, next, pts, count, win_size, max_level, &out_pts, out_status)` | 金字塔 LK 光流：`win_size` 偶数自动补奇、`max_level >= 0`；**`out_status` 一级指针契约——调用方预分配 count 字节**；`out_pts` 本库 malloc 配 `FreePoint2f` |
| `Image2DAlg_FindTransformECC(templ, input, motion_type, max_iters, epsilon, m[9], &corr)` | ECC 对齐：motion_type 0=平移(2x3)/1=仿射(2x3)/2=单应(3x3)（与 cv 原生枚举取值不同）；`m` 为模板坐标取 input(m·x) 的采样映射；2x3 仅前 6 个有效；-2 = 不收敛/相关性 < 0.5 |
| `Image2DAlg_PhaseCorrelate(a, b, &dx, &dy, &resp)` | 相位相关平移估计（亚像素）：输出 b 相对 a 的位移；尺寸不等 → -3 |
| `Image2DAlg_StereoMatch(left, right, min_disparity, num_disparities, block_size, out_disparity)` | SGBM 双目视差：num_disparities 须为 16 倍数（否则 -3）；**`out_disparity` 一级指针契约——调用方预分配**（width/height/channels 与左图一致、1 通道）；输出 8U = CV_16S 定点值/16 四舍五入，无效视差 → 0；左图 x < min_disparity+num_disparities 条带视差不可靠（SGBM 特性） |
| `Image2DAlg_SolvePnP(object_points, image_points, count, camera_matrix, dist_coeffs, dist_count, rvec, tvec)` | 2D-3D 位姿（默认迭代法 SOLVEPNP_ITERATIVE）：count >= 4（否则 -2）；`dist_coeffs` 可 NULL（dist_count<=0 无畸变）；object_points 3N 扁平 double |

## 错误码

| 码 | 含义 |
|----|------|
| 0 | 成功 |
| -1 | 空指针 / 空图 |
| -2 | 特征/匹配/估计不足（特征族与 ORBAlign：无关键点、匹配对数过少、几何估计退化；跟踪配准族：ECC 不收敛或相关性不足、PnP 求解失败；ReadImage 例外——-2 为打开失败） |
| -3 | 通道数、模式或参数不支持 |
| -4 | 内存不足 |
| -99 | OpenCV 异常（在 C 边界统一捕获，异常不跨 DLL） |

## 构建与测试

```bat
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release --target Image2DAlg Image2DAlg_test
ctest --test-dir build -C Release -R Image2DAlg_test --output-on-failure
```

测试工作目录为 `build/output/Release`（与运行时 DLL 同目录），临时 png 跑完自清理。
