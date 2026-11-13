# 手动配准交互工具（Point3DAlg）

两片点云的交互式配准：**键盘手动粗调 + 6 种策略精配准**，导出 4×4 变换矩阵供 ComUI 节点使用。

**C++ / PCL 实现**，无 Python 依赖。图形界面基于 `pcl::visualization::PCLVisualizer`
（PCL 1.9.1 的 visualization 模块已静态链接 VTK，无需额外部署 VTK 运行时 DLL）。

## 为什么是独立工具而不是 ComUI 算子

ComUI 插件的硬约束（`docs/THIRD_PARTY_OPS.md` §9）决定键盘交互不可能塞进算子 DLL：

| 约束 | 原文 | 后果 |
|---|---|---|
| 纯计算 | 「算子 DLL 禁止引用/创建任何 UI 控件」 | 不能在算法 DLL 里开窗口 |
| 无生命周期钩子 | 「算子没有 Initialize/Shutdown；每次执行新建实例」 | 交互会话状态无处保存 |
| 无键盘通道 | ABI 只有 params/images/clouds | 键盘事件无法传入 |

**分工**：
- **本工具（离线）**：人眼+键盘把两片点云调到大致重合 → 跑 ICP 精修 → 导出矩阵
- **ComUI 节点（在线）**：接收两片点云 + 该矩阵作初值，自动跑 ICP，输出配准结果

## 文件构成

| 文件 | 职责 |
|---|---|
| `Point3DAlg_Regist.h/.cpp` | **配准算法层**（纯函数、零 UI），编进 `Point3DAlg.dll`，ComUI 节点复用 |
| `Point3DAlg_RegistTool.cpp` | **配准交互工具**（PCLVisualizer + 键盘），独立 exe |
| `Point3DAlg_RegistTest.cpp` | 配准算法层单元测试（无 UI，26 项 check） |
| `Point3DAlg_DomeFit.h/.cpp` | **拱形球顶 bump 拟合**（纯函数）：基面 → 聚类 → 球拟合 → 高度，编进 `Point3DAlg.dll` |
| `Point3DAlg_DomeFitTest.cpp` | 球顶拟合测试（合成自检注册 ctest；带 PLY 参数 = 真实数据对照基准） |
| `Point3DAlg_SickSynthTest.cpp` | SICK 拼接合成回归（自造平面布局 .dat：单机列映射 / 多相机交织 / 偏移对齐 / FOV 框坐标，注册 ctest） |
| `Point3DAlg_FitUtil.h` | 量测数值内核（MedianOf / FitPlaneZ，header-only，DomeFit 与 RegionPlane 共用） |
| `Point3DAlg_RegionPlane.h/.cpp` | **ROI 平面量测**（纯函数）：稳健平面拟合 → 高度/平面度 PV/σ，台阶差=两 ROI 高度相减 |
| `Point3DAlg_RegionPlaneTest.cpp` | 平面量测测试（水平/倾斜/台阶/混台阶 20% minority/边界，18 项 check，注册 ctest） |

## 构建与运行

```bash
cmake --build build --config Release --target Point3DAlg_RegistTool
./build/output/Release/Point3DAlg_RegistTool.exe                      # 内置演示
./build/output/Release/Point3DAlg_RegistTool.exe src.ply dst.ply      # 真实数据
./build/output/Release/Point3DAlg_RegistTest.exe                      # 算法层自测
```

支持 `.ply` / `.pcd`；`.txt` / `.xyz` 取前三列。

输出文件：
- `regist_T.txt` —— 4×4 矩阵（文本，可编辑）
- `regist_T_flat.txt` —— 一行 16 个逗号分隔值，**直接粘进 ComUI 节点参数**
- `regist_truth.txt` —— 仅演示模式，已知真值供自检

## 键盘操作

| 键 | 功能 |
|---|---|
| **F1** | ICP 点到点（通用，最稳） |
| **F2** | ICP 点到面（平面多时精度高，需法线） |
| **F3** | GICP（平面/曲面混合，精度最高） |
| **F4** | FPFH + RANSAC（SAC-IA，无需初值，慢） |
| **F5** | NDT（大初始偏差稳健） |
| **F6** | PCA 主轴粗配准（极快，给 ICP 找初值） |
| **Enter** | 以当前位姿为初值配准 |
| **Space** | 连续迭代开关（每 0.4s 一次，收敛自动停） |
| **R** | 复位到单位阵（会附带重置相机视角） |
| **P** | 保存变换（同时打印 ComUI 参数值；会附带把显示切换为点表示） |
| **W/S** | 沿 X −/+ 平移（W 朝 −X，S 朝 +X；附带切换线框/表面表示） |
| **A/D** | 沿 Y −/+ 平移（A 朝 −Y，D 朝 +Y） |
| **PageUp/PageDown** | 沿 Z +/− 平移（Q/E 是 PCL 退出键，不可用，见键位说明） |
| **↑/↓** | 绕 X ± 旋转 |
| **←/→** | 绕 Y ± 旋转 |
| **I/K** | 绕 Z −/+ 旋转（I 朝 −Z 方向，K 朝 +Z 方向，右手系） |
| **+/-** | 步长 ×2 / ÷2（起始 1mm / 1°；`=` 同效，小键盘 ± 需开 NumLock；会附带改变点大小/视图缩放） |
| **C** | 目标点云变暗切换（看穿对齐情况；会附带打印相机参数） |
| **O** | 旋转中心切换：目标质心（默认）/ 世界原点（会附带切换透视/平行投影） |
| **H** | 打印按键说明（会附带打印 PCL 内置帮助） |
| **Esc** | 退出 |

窗口标题实时显示：当前策略、步长、旋转中心、历史最优 fitness。

## 键位说明（T27 修复后的选键依据）

分派已改为 `getKeySym()`（VTK 键名）。以下选键约束来自对 PCL 1.9.1
`interactor_style.cpp` 与 VTK 8.1 的源码级审计（T21），键位按约束重新分配，待真机逐键复核：

1. **策略键 1-6 → F1-F6**：数字键被 PCL 内置 style 用于点云处理器切换，
   处理后直接 return，`keyboard_signal` 回调收不到；F1-F12 PCL 不拦截、可穿透。
2. **Z 向平移 Q/E → PageUp/PageDown**：PCL/VTK 把 q/e 定义为退出（按下直接关窗口）；
   `x` 被 PCL 框选模式占用，故选 PgUp/PgDn。
3. **分派改用键名**：`getKeyCode()` 是 ToAscii 字符码，Esc/方向键/功能键等
   非打印键恒为 0，按字符码分派会漏掉（旧实现的三处死绑定）；键名分派同时
   解决 Shift/CapsLock 大写不匹配问题。
4. **内置副作用保留**（功能可用，属 PCL/VTK 行为）：w/s 切换线框/表面表示、
   r 重置相机、o 切换透视/平行投影、c 打印相机参数、h 打印 PCL 帮助、
   +/- 改点大小、= 触发视图缩放。
5. **Enter** 键码 CR=13 与键名 Return 双通道识别（含小键盘 Enter 的 KP_Enter）。

## 正确用法（关键）

**不要指望从单位阵一键跑通 ICP**——ICP 是局部优化，大偏差时必然陷入局部极小。

```
1. 视觉上先对齐：WASD + PgUp/PgDn + IK 把源点云（红）大致叠到目标（蓝）上，方向键做绕 X/Y 微调
   ↓
2. 按 F1 或 F3 选策略，Enter 精配准（重复 2~3 次直到 fitness 不再提升）
   ↓
3. 若仍不理想：先按 F6（PCA）拿初值，或按 F4（FPFH+RANSAC）无初值粗配准
   ↓
4. P 保存，把打印的 init_transform 值粘进 ComUI 节点
```

## 算法验证结果（19 项 check 全过）

| 验证项 | 结果 |
|---|---|
| 会话基础操作（平移/旋转/绕点旋转/复位/策略切换） | 全部正确（含绕指定点旋转中心不动的验证） |
| 变换文件往返（4×4 / 扁平格式） | 误差 < 1e-6 |
| 6 种策略均可运行 | fitness > 0，无崩溃 |
| **PCA 绝对精度** | **err < 1e-4（精确命中真值）** |
| 近初值 ICP | 相对单位阵初值误差下降 37%（0.69 → 0.43） |

## 精度限制（数学必然，非 bug）

**规则格点云的 ICP 量化误差**：演示数据是间隔 0.4mm 的规则网格，ICP 会把每个点吸附到
最近格点，因此即使真值正确也只能收敛到约半个格距（0.2mm）的偏移。

- 真实扫描数据点分布连续，无此问题
- 这也是为什么用 **PCA 验证绝对精度**（PCA 不依赖点对应关系，err < 1e-4）

**其他已知局限**：

| 局限 | 表现 | 应对 |
|---|---|---|
| ICP 局部极小 | fitness=1.0 但视觉错位 | 必须先手动粗调 |
| 纯平面不可观测 | 面内平移/绕法向旋转滑移 | 工件需有凸台/缺口等特征 |
| 对称件多解 | 可能翻 180° | 用 PCA 后人工判断 |
| 点到面在平坦区奇异 | fitness 骤降 | `Point3DAlg_RegistRun` 已自动回退点到点 |

## 与 ComUI 配合

工具导出的矩阵对应 ComUI 节点 `alg.point3d.manual_regist` 的 `init_transform` 参数：

```json
{
  "name": "init_transform",
  "label": "初始变换(16值逗号分隔)",
  "type": "text",
  "default": "1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1"
}
```

节点侧调用同一套算法层（`Point3DAlg_RegistRun`，参数 `strategy` 选 1~6），
输出配准后的点云 + 最终变换，接 `pub.cloud` 在 3D 工作台查看。

## 已知未做（按需再加）

- **多尺度 ICP**：粗→细两轮（先大体素后小体素），能扩大收敛域
- **特征约束**：SICK 扫描数据的 marker/靶点，用于无重叠区域的配准
- **批量配准**：多帧点云链式配准，累积误差需闭环优化
