// Point3DAlg_RegistTool.cpp —— 手动配准交互工具（PCLVisualizer + 键盘）
//
// 用 PCLVisualizer 的 registerKeyboardCallback 实现：键盘微调位姿 + 多策略 ICP。
// 算法全在 Point3DAlg_Regist.cpp（纯函数），本文件只负责 UI 与按键分派。
//
// 用法：
//   Point3DAlg_RegistTool [源点云 目标点云]
//   不带参数 → 内置演示数据（L 形底板 + 双凸台 + 已知真值变换）
//   支持 .ply / .pcd；.txt/.xyz 取前三列
//
// 键盘：
//   F1..F7    切换策略（标题栏显示；数字键 1-6 被 PCL 内置 style 吞掉，改用功能键）
//   Enter     以当前位姿为初值配准
//   Space     连续迭代开关
//   R         复位单位阵
//   P         保存变换（regist_T.txt + 打印 ComUI 参数）
//   W/S A/D PageUp/PageDown   沿 X/Y/Z 平移（±；Q/E 是 PCL 退出键不可用）
//   ←/→ ↑/↓ I/K   绕 X/Y/Z 旋转（±）
//   +/-       步长 ×2 / ÷2
//   C         目标点云变暗切换
//   H         打印按键说明
//   Esc       退出

#include "Point3DAlg_Regist.h"

#include "CommonAlg/include/CommonAlg_FileUtil.h"

// Boost 1.68 + MSVC 2022 兼容垫片：必须在任何 pcl/io 头之前
#include "BoostCompat.h"

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/io/ply_io.h>
#include <pcl/io/pcd_io.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/console/print.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr double kStepMin = 0.01;
constexpr double kStepMax = 100.0;

using CloudXYZ = pcl::PointCloud<pcl::PointXYZ>;

// ---------------- 点云载入 ----------------

CloudXYZ::Ptr LoadCloud(const std::string& path)
{
    CloudXYZ::Ptr c(new CloudXYZ);
    const std::string ext = CommonAlg_Extension(path);

    bool ok = false;
    if (ext == ".ply")
        ok = pcl::io::loadPLYFile(path, *c) == 0;
    else if (ext == ".pcd")
        ok = pcl::io::loadPCDFile(path, *c) == 0;
    else {
        // 文本：取前三列
        std::ifstream f(path);
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty() || line[0] == '#')
                    continue;
                for (char& ch : line)
                    if (ch == ',')
                        ch = ' ';
                std::istringstream ss(line);
                double x, y, z;
                if (ss >> x >> y >> z)
                    c->push_back(pcl::PointXYZ((float)x, (float)y, (float)z));
            }
            ok = !c->empty();
        }
    }
    if (!ok || c->empty())
        return nullptr;
    return c;
}

// 内置演示：L 形底板 + 两个不同高度凸台（不对称，ICP 可唯一收敛）
CloudXYZ::Ptr MakeDemoSource()
{
    CloudXYZ::Ptr c(new CloudXYZ);
    auto slab = [&c](double x0, double x1, double y0, double y1, double z, double step) {
        for (double x = x0; x <= x1 + 1e-9; x += step)
            for (double y = y0; y <= y1 + 1e-9; y += step)
                c->push_back(pcl::PointXYZ((float)x, (float)y, (float)z));
    };
    slab(0.0, 20.0, 0.0, 6.0, 0.0, 0.4);      // 横条
    slab(0.0, 6.0, 6.0, 20.0, 0.0, 0.4);      // 竖条 → L 形
    slab(8.0, 12.0, 1.0, 5.0, 3.0, 0.3);      // 凸台 A
    slab(14.0, 18.0, 1.5, 4.5, 1.5, 0.3);     // 凸台 B
    return c;
}

// ---------------- 交互工具 ----------------

class RegistTool
{
public:
    RegistTool(CloudXYZ::Ptr src, CloudXYZ::Ptr dst)
        : src_(std::move(src)), dst_(std::move(dst))
    {
        params_.max_correspondence_dist = 0.0;   // 自适应
        spacing_ = Point3DAlg_RegistEstimateSpacing(dst_);

        // 目标点云质心作为默认旋转中心
        double sx = 0, sy = 0, sz = 0;
        for (const auto& p : dst_->points) { sx += p.x; sy += p.y; sz += p.z; }
        const double n = dst_->empty() ? 1.0 : (double)dst_->size();
        pivot_[0] = sx / n; pivot_[1] = sy / n; pivot_[2] = sz / n;

        // 显示用点云（带颜色）
        src_disp_ = ToRgb(src_, 255, 90, 90);      // 红 = 源
        dst_disp_ = ToRgb(dst_, 90, 150, 255);     // 蓝 = 目标

        InitVis();
        PrintHelp();
        PrintStatus();
    }

private:
    // ---- 工具 ----

    // 显示用点云带颜色（算法层用 PointXYZ，显示层单独转 PointXYZRGB）
    using CloudRGB = pcl::PointCloud<pcl::PointXYZRGB>;

    static CloudRGB::Ptr ToRgb(const CloudXYZ::ConstPtr& src, uint8_t r, uint8_t g, uint8_t b)
    {
        CloudRGB::Ptr c(new CloudRGB);
        c->reserve(src->size());
        for (const auto& p : src->points) {
            pcl::PointXYZRGB q;
            q.x = p.x; q.y = p.y; q.z = p.z;
            q.r = r; q.g = g; q.b = b;
            c->push_back(q);
        }
        c->width = (uint32_t)c->size();
        c->height = 1;
        c->is_dense = true;
        return c;
    }

    void PrintHelp() const
    {
        std::printf(
            "\n===== Point3DAlg 手动配准工具 =====\n"
            "  F1..F7   切换策略（ICP-P2P / ICP-P2PL / GICP / FPFH+RANSAC / NDT / PCA / 多尺度金字塔）\n"
            "  Enter    以当前位姿为初值配准\n"
            "  Space    连续迭代开关\n"
            "  R        复位单位阵      P  保存变换\n"
            "  W/S A/D PgUp/PgDn  平移 X/Y/Z   ←→ ↑↓ I/K  旋转 X/Y/Z\n"
            "  +/-      步长 ×2 ÷2      C  目标变暗      H  帮助\n"
            "  O        旋转中心切换（目标质心 / 世界原点）\n"
            "  Esc      退出\n"
            "  注：R/P/C/O/W/S 等键伴随 PCL 内置行为（相机复位/点表示/投影切换等）\n"
            "===================================\n\n");
    }

    void PrintStatus() const
    {
        std::printf("[状态] 策略 %d=%s | 步长 %.2fmm/%.2f° | 间距 %.3fmm（阈值由算法层按间距自适应）\n",
                    session_.strategy, Point3DAlg_RegistStrategyName(session_.strategy),
                    step_mm_, step_deg_, spacing_);
        if (session_.best_fitness >= 0.0)
            std::printf("       最优 fitness=%.4f  rmse=%.4f  迭代=%d\n",
                        session_.best_fitness, session_.best_rmse, session_.iteration);
    }

    void SetWindowTitle()
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Point3DAlg 手动配准 | [%d] %s | %.2fmm/%.2f° | 转:%s | fit=%.4f",
                 session_.strategy, Point3DAlg_RegistStrategyName(session_.strategy),
                 step_mm_, step_deg_, use_pivot_ ? "质心" : "原点",
                 session_.best_fitness < 0 ? 0.0 : session_.best_fitness);
        vis_->setWindowName(buf);
    }

    // 用当前变换刷新源点云显示
    void RefreshSource()
    {
        CloudXYZ::Ptr t = Point3DAlg_RegistApplyTransform(src_, session_.transform);
        src_disp_ = ToRgb(t, 255, 90, 90);
        vis_->updatePointCloud<pcl::PointXYZRGB>(src_disp_, "source");
        SetWindowTitle();
    }

    void ApplyStep(const Point3DAlg_RegistStep& step, bool print_status = false)
    {
        session_ = step.next;
        if (!step.message.empty())
            std::printf("  %s\n", step.message.c_str());
        if (step.improved)
            std::printf("  ★ 新最优 fitness=%.5f\n", session_.best_fitness);
        RefreshSource();
        if (print_status)
            PrintStatus();   // 只在配准后打印完整状态；微调类操作靠标题栏实时展示
    }

    // ---- 初始化 ----

    void InitVis()
    {
        vis_.reset(new pcl::visualization::PCLVisualizer("Point3DAlg 手动配准"));
        vis_->setBackgroundColor(0.08, 0.08, 0.10);

        vis_->addPointCloud<pcl::PointXYZRGB>(src_disp_, "source");
        vis_->addPointCloud<pcl::PointXYZRGB>(dst_disp_, "target");
        vis_->setPointCloudRenderingProperties(
            pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2.0, "source");
        vis_->setPointCloudRenderingProperties(
            pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2.0, "target");
        vis_->addCoordinateSystem(1.0, "axis");

        // PCLVisualizer 只支持注册一个键盘回调，内部按键码分派
        vis_->registerKeyboardCallback(
            [this](const pcl::visualization::KeyboardEvent& e) { OnKey(e); });
    }

    // 统一按键分派：keyDown + 无修饰键才响应
    //
    // 分派一律用 getKeySym()（VTK 键名）：getKeyCode() 是 ToAscii 字符码，
    // Esc/方向键/功能键等非打印键恒为 0，按字符码分派会漏掉（旧实现的三处死绑定）。
    // 策略键改用 F1-F6：数字键 1-6 会被 PCL 内置 style 在 keyboard_signal 之前吞掉
    // （切换点云 handler 后直接 return，用户回调收不到）；F1-F12 PCL 不拦截、可穿透。
    // Z 向平移改用 PageUp/PageDown：Q/E 是 PCL/VTK 的退出键（按下直接关窗口）。
    void OnKey(const pcl::visualization::KeyboardEvent& e)
    {
        if (!e.keyDown() || e.isCtrlPressed() || e.isAltPressed())
            return;
        const unsigned char k = (unsigned char)e.getKeyCode();
        std::string sym = e.getKeySym();
        std::transform(sym.begin(), sym.end(), sym.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });

        // 策略 1..7 → F1..F7
        if (sym.size() == 2 && sym[0] == 'f' && sym[1] >= '1' && sym[1] <= '7') {
            OnStrategy(sym[1] - '0');
            return;
        }
        // 功能键（VTK 键名，不区分大小写）
        if (sym == "return" || sym == "kp_enter" || k == 13) { OnRunRegist(); return; }
        if (sym == "escape") { vis_->close(); return; }
        if (sym == "space" || k == ' ') { OnToggleContinuous(); return; }
        if (sym == "prior") { OnTranslate(2, +1); return; }   // PageUp：+Z
        if (sym == "next")  { OnTranslate(2, -1); return; }   // PageDown：−Z
        // 旋转（方向键 + IK）
        if (sym == "up")    { OnRotate(0, +1); return; }
        if (sym == "down")  { OnRotate(0, -1); return; }
        if (sym == "left")  { OnRotate(1, +1); return; }
        if (sym == "right") { OnRotate(1, -1); return; }
        if (sym == "i") { OnRotate(2, +1); return; }
        if (sym == "k") { OnRotate(2, -1); return; }
        // 平移（w/s/a/d 与 PCL 内置线框/表面切换共存，属既有副作用）
        if (sym == "w") { OnTranslate(0, +1); return; }
        if (sym == "s") { OnTranslate(0, -1); return; }
        if (sym == "a") { OnTranslate(1, -1); return; }
        if (sym == "d") { OnTranslate(1, +1); return; }
        // 功能
        if (sym == "r") { OnReset(); return; }
        if (sym == "p") { OnSave(); return; }
        if (sym == "c") { OnToggleDim(); return; }
        if (sym == "o") { OnTogglePivot(); return; }
        if (sym == "h") { PrintHelp(); return; }
        // 步长（'+'=Shift+=、'='、NumLock 小键盘 +；PCL 会附带改点大小）
        if (k == '+' || k == '=') { OnStep(2.0); return; }
        if (k == '-') { OnStep(0.5); return; }
    }

    // ---- 按键处理 ----

    void OnStrategy(int k)
    {
        ApplyStep(Point3DAlg_RegistSetStrategy(session_, k));
    }

    void OnTranslate(int axis, int sign)
    {
        double d[3] = { 0.0, 0.0, 0.0 };
        d[axis] = sign * step_mm_;
        ApplyStep(Point3DAlg_RegistTranslate(session_, d[0], d[1], d[2]));
    }

    void OnRotate(int axis, int sign)
    {
        // 默认绕目标点云质心转（手工配准最直观；绕世界原点会让远处点飞走）
        // 按 O 键可在「绕质心」与「绕世界原点」间切换
        if (use_pivot_) {
            const double piv[3] = { pivot_[0], pivot_[1], pivot_[2] };
            ApplyStep(Point3DAlg_RegistRotateAround(session_, piv, axis, sign * step_deg_));
        } else {
            ApplyStep(Point3DAlg_RegistRotate(session_, axis, sign * step_deg_));
        }
    }

    void OnTogglePivot()
    {
        use_pivot_ = !use_pivot_;
        std::printf("[旋转中心] %s (%.2f, %.2f, %.2f)\n",
                    use_pivot_ ? "目标质心" : "世界原点",
                    pivot_[0], pivot_[1], pivot_[2]);
        SetWindowTitle();
    }

    void OnStep(double factor)
    {
        step_mm_ = std::min(std::max(step_mm_ * factor, kStepMin), kStepMax);
        step_deg_ = std::min(std::max(step_deg_ * factor, 0.05), 45.0);
        std::printf("[步长] %.3f mm / %.2f°\n", step_mm_, step_deg_);
        SetWindowTitle();
    }

    void OnRunRegist()
    {
        std::printf("\n[配准] 策略 %d = %s ...\n", session_.strategy,
                    Point3DAlg_RegistStrategyName(session_.strategy));
        Point3DAlg_RegistStep step = Point3DAlg_RegistRun(session_, src_, dst_, params_);
        ApplyStep(step, true);
        if (step.fitness < 0.0)
            std::printf("  ! 本次未产出有效结果\n");
    }

    void OnToggleContinuous()
    {
        continuous_ = !continuous_;
        std::printf("[连续迭代] %s\n", continuous_ ? "开（每 0.4s 一次，收敛自动停）" : "关");
    }

    void OnReset()
    {
        ApplyStep(Point3DAlg_RegistReset(session_));
    }

    void OnSave()
    {
        // 4x4 矩阵 + ComUI 扁平参数
        if (Point3DAlg_RegistSaveTransform("regist_T.txt", session_.transform))
            std::printf("[保存] 4x4 矩阵 → regist_T.txt\n");
        if (Point3DAlg_RegistSaveTransformFlat("regist_T_flat.txt", session_.transform))
            std::printf("[保存] ComUI 参数 → regist_T_flat.txt\n");

        std::printf("\n[ComUI] 把下面这行粘进 manual_regist 节点的 init_transform 参数：\n%s\n\n",
                    Point3DAlg_RegistFormatFlat(session_.transform).c_str());
        if (session_.best_fitness >= 0.0)
            std::printf("[提示] 历史最优 fitness=%.5f rmse=%.4f\n",
                        session_.best_fitness, session_.best_rmse);
    }

    void OnToggleDim()
    {
        dim_ = !dim_;
        dst_disp_ = dim_ ? ToRgb(dst_, 45, 75, 128) : ToRgb(dst_, 90, 150, 255);
        vis_->updatePointCloud<pcl::PointXYZRGB>(dst_disp_, "target");
        std::printf("[显示] 目标点云 %s\n", dim_ ? "变暗" : "正常");
    }

    // 连续迭代：靠 PCLVisualizer 的 spinOnce 循环驱动
    void Tick()
    {
        if (!continuous_)
            return;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_tick_).count();
        if (dt < 0.4)
            return;
        last_tick_ = now;

        Point3DAlg_RegistStep step = Point3DAlg_RegistRun(session_, src_, dst_, params_);
        if (step.fitness < 0.0) {
            continuous_ = false;
            std::printf("[连续迭代] 配准失败，停止\n");
            return;
        }

        // 收敛判据：连续 5 次 fitness 无改善才停（局部极小时 fitness 也可能很高，
        // 用「是否还在提升」比「是否达 0.999」更可靠）
        if (step.improved) {
            no_improve_count_ = 0;
            ApplyStep(step);          // 无打印，避免连续迭代刷屏
        } else {
            session_ = step.next;     // 状态照常更新（变换仍在小幅调整）
            RefreshSource();
            if (++no_improve_count_ >= 5) {
                continuous_ = false;
                std::printf("[连续迭代] 连续 5 次无改善，停止（best fitness=%.5f）\n",
                            session_.best_fitness);
            }
        }
    }

public:
    // 主循环：spinOnce 等 33ms 再返回，约 30FPS（有事件会提前醒，键入手感不受影响）
    void Loop()
    {
        last_tick_ = std::chrono::steady_clock::now();
        while (!vis_->wasStopped()) {
            vis_->spinOnce(33, true);
            Tick();
        }
    }

private:
    CloudXYZ::Ptr src_, dst_;
    CloudRGB::Ptr src_disp_, dst_disp_;
    pcl::visualization::PCLVisualizer::Ptr vis_;

    Point3DAlg_RegistSession session_;
    Point3DAlg_RegistParams  params_;

    double spacing_ = 1.0;
    double step_mm_ = 1.0;
    double step_deg_ = 1.0;

    bool continuous_ = false;
    bool dim_ = false;
    bool use_pivot_ = true;          // 旋转是否绕工件质心
    double pivot_[3] = { 0, 0, 0 };   // 目标点云质心
    int no_improve_count_ = 0;       // 连续迭代中「无改善」次数
    std::chrono::steady_clock::time_point last_tick_;
};

} // namespace

int main(int argc, char** argv)
{
    CloudXYZ::Ptr src, dst;

    if (argc >= 3) {
        src = LoadCloud(argv[1]);
        dst = LoadCloud(argv[2]);
        if (!src || !dst) {
            std::printf("点云载入失败：%s / %s\n", argv[1], argv[2]);
            return 1;
        }
        std::printf("源 %zu 点，目标 %zu 点\n", src->size(), dst->size());
    } else {
        src = MakeDemoSource();
        // 演示：目标 = 源绕 Z 转 8° + 平移 (1.2, -0.7, 0.35)
        float T[16];
        const double a = 8.0 * 3.14159265358979323846 / 180.0;
        const float c = (float)std::cos(a), s = (float)std::sin(a);
        T[0] = c; T[1] = -s; T[2] = 0; T[3] = 1.2f;
        T[4] = s; T[5] = c;  T[6] = 0; T[7] = -0.7f;
        T[8] = 0; T[9] = 0;  T[10] = 1; T[11] = 0.35f;
        T[12] = 0; T[13] = 0; T[14] = 0; T[15] = 1.0f;
        dst = Point3DAlg_RegistApplyTransform(src, T);
        Point3DAlg_RegistSaveTransform("regist_truth.txt", T);
        std::printf("演示数据：源 %zu 点，目标已施加真值变换（绕Z 8°，平移 1.2/-0.7/0.35）\n",
                    src->size());
        std::printf("真值已写入 regist_truth.txt，可用来自检收敛精度\n");
    }

    RegistTool tool(src, dst);
    tool.Loop();
    return 0;
}
