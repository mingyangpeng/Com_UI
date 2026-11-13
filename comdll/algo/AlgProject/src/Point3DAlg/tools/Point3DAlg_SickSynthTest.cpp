// Point3DAlg_SickSynthTest.cpp —— SICK 拼接链路合成数据回归（无外部数据依赖，注册 ctest）。
// 自造 .dat（图像块 + 编码器记录）跑 LoadRaw → GetDepth 全链路，验证：
//   1. 单相机拼接列映射（列 = (A−aMin)/scanStep，行反转不影响同值行）；
//   2. 双相机交织：合并列 k = 排序后第 k%n 台相机的第 k/n 列
//      （回归点：2026-11-10 修复前拷贝循环缺设备归属过滤，整图被末位相机覆盖）；
//   3. 起始脉冲偏移对齐：offset = (aMin−minPulse)/scanStep 左补 0 + 左侧全零列裁剪；
//   4. FovBoxes 多相机坐标（k = (colFirst+offset)·n + rank − left）
//      （回归点：修复前漏加 offset，非首相机框整体左移）。
// 退出码：0=通过 1=失败。

#include "include/Point3DAlg_SickReader.h"

#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const int kSickWidth = 2560;   // 与 SickReader.cpp 内部常量一致
static int g_fail = 0;

static void check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_fail;
}

// .dat 平面布局（与真实数据一致，见 include/Point3DAlg_SickReader.h 头注释）：
//   [N × 5120B 高度图][N × 20B 编码器记录（5×int32：A, 0x40000000, B, C, 0）]
// 图像全行同值 = baseRaw+i（编码扫描行号）；A_i = aMin + i×step。
static void WriteDat(const std::string& path, int32_t aMin, int lines, int step, int baseRaw)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::printf("写 .dat 失败: %s\n", path.c_str());
        std::exit(1);
    }
    for (int i = 0; i < lines; ++i)                       // ---- 图像块（连续）----
        for (int c = 0; c < 2560; ++c) {
            const uint16_t v = (uint16_t)(baseRaw + i);
            std::fwrite(&v, sizeof(v), 1, f);
        }
    for (int i = 0; i < lines; ++i) {                     // ---- 编码器块（连续）----
        const int32_t rec[5] = { aMin + (int32_t)i * step, 0x40000000, 0, 0, 0 };
        std::fwrite(rec, sizeof(int32_t), 5, f);
    }
    std::fclose(f);
}

static void RmDir(const char* dir)
{
    // 枚举删除各 cam 目录下全部文件（用例会写不同名字的 .dat，不能只删固定名）
    for (int cam = 0; cam < 2; ++cam) {
        const std::string sub = std::string(dir) + "/cam" + std::to_string(cam);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((sub + "/*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    std::remove((sub + "/" + fd.cFileName).c_str());
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        _rmdir(sub.c_str());
    }
    _rmdir(dir);
}

static Point3DAlg_SickConfig MakeCfg()
{
    Point3DAlg_SickConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.mm_pulse = 0.0002;
    cfg.x_scale = 0.006;       // = scanStep×mm_pulse（仅文档语义，GetCloud 用实测 scanStep）
    cfg.y_scale = 0.0035;      // 行向 mm/px（任意正值）
    cfg.z_scale = 0.001;       // z = raw×0.001
    cfg.z_offset = 0.0;
    // 补偿全零（comp_fit0=0 关一阶项，comp_count=0 关多项式）
    return cfg;
}

// 断言拼接图整列：期望非零 → 整列同值 = raw×z_scale；期望 0 → 整列全零（左补零列）
static void CheckCol(const float* depth, long long rows, long long cols, long long col,
                     double expectZ, const char* tag)
{
    double sum = 0.0;
    long long nz = 0;
    for (long long r = 0; r < rows; ++r) {
        const float v = depth[(size_t)r * (size_t)cols + (size_t)col];
        if (v != 0.0f) { ++nz; sum += v; }
    }
    const double avg = nz ? sum / (double)nz : 0.0;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s（列 %lld：期望 %.4f，实测 %.4f，非零 %lld/%lld）",
                  tag, col, expectZ, avg, nz, rows);
    const bool ok = (expectZ == 0.0)
        ? (nz == 0)                                            // 左补零列：全零
        : (nz == rows && std::fabs(avg - expectZ) < 1e-4);     // 数据列：整列同值
    check(ok, buf);
}

// 断言某行段整段同值（蛇形多 pass：同一列两个 pass 各占一段行）
static void CheckBand(const float* depth, long long rows, long long cols, long long col,
                      long long rowLo, long long rowHi, double expectZ, const char* tag)
{
    double sum = 0.0;
    long long nz = 0;
    for (long long r = rowLo; r < rowHi; ++r) {
        const float v = depth[(size_t)r * (size_t)cols + (size_t)col];
        if (v != 0.0f) { ++nz; sum += v; }
    }
    const double avg = nz ? sum / (double)nz : 0.0;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s（列 %lld 行 %lld..%lld：期望 %.4f，实测 %.4f，非零 %lld/%lld）",
                  tag, col, rowLo, rowHi, expectZ, avg, nz, rowHi - rowLo);
    check(nz == rowHi - rowLo && std::fabs(avg - expectZ) < 1e-4, buf);
}

int main()
{
    std::printf("== SICK 拼接合成回归 ==\n");
    std::fflush(stdout);
    const int kLines = 30;
    const int kStep = 30;
    const std::string root = "_sick_synth";

    // ---- 数据目录：cam0/cam1 ----
    _mkdir(root.c_str());
    _mkdir((root + "/cam0").c_str());
    _mkdir((root + "/cam1").c_str());
    WriteDat(root + "/cam0/scan.dat", 3000, kLines, kStep, 1000);
    WriteDat(root + "/cam1/scan.dat", 3000 + kStep, kLines, kStep, 2000);
    std::printf("[阶段] 合成数据已写入\n");
    std::fflush(stdout);
    // cam1 起始脉冲偏移 1 列（offset=1）：验证左补 0 + 全零列裁剪 + 框坐标

    const Point3DAlg_SickConfig cfg = MakeCfg();

    // ---- 用例 1：双相机交织（cam1 offset=1）----
    {
        // 注意：reader 必须活在断言全程——GetDepth/FovBoxes 返回的缓冲由 reader 持有
        Point3DAlg_SickReader reader(cfg);
        const float* depth = nullptr;
        long long rows = 0, cols = 0;
        std::vector<int> rects, ids;
        int nBox = 0;
        try {
            reader.LoadRaw({ root + "/cam0", root + "/cam1" }, -1);
            check(reader.DeviceCount() == 2, "双相机加载");
            std::printf("[阶段] 用例1 GetDepth 开始\n");
            std::fflush(stdout);
            depth = reader.GetDepth();
            rows = reader.StitchRows();
            cols = reader.StitchCols();
            nBox = reader.FovBoxes(rects, ids);
        } catch (const std::exception& e) {
            std::printf("  [EXC] %s\n", e.what());
            std::fflush(stdout);
            check(false, "用例1 抛异常（消息见上）");
            RmDir(root.c_str());
            std::printf("\n存在失败（失败 %d 项）\n", g_fail);
            return 1;
        }
        std::printf("  拼接图：%lld 行 × %lld 列\n", rows, cols);
        std::fflush(stdout);
        // maxCols = max(offset+cols) = max(0+30, 1+30) = 31 → totalCols = 62；
        // cam1 k=61 → sc=29 有效 → 右裁到 61；k=1（cam1 左补零列）保留 → cols=62
        check(cols == 62, "列数 = 62（maxCols 31 × 2 相机，无全零边缘列）");

        // 交织列序（修复核心断言）：k 偶 → cam0 sc=k/2；k 奇 → cam1 sc=k/2−offset
        CheckCol(depth, rows, cols, 0, 1.000, "交织列 0 = cam0 col0");
        CheckCol(depth, rows, cols, 1, 0.0, "交织列 1 = cam1 左补 0（sc=−1）");
        CheckCol(depth, rows, cols, 2, 1.001, "交织列 2 = cam0 col1");
        CheckCol(depth, rows, cols, 3, 2.000, "交织列 3 = cam1 col0（offset=1）");
        CheckCol(depth, rows, cols, 4, 1.002, "交织列 4 = cam0 col2");
        CheckCol(depth, rows, cols, 61, 2.029, "末列 61 = cam1 col29");

        // ---- FovBoxes（多相机坐标：k = (colFirst+offset)·n + rank − left）----
        check(nBox == 2 && (int)rects.size() == 2 * 4, "FOV 框数量 = 2");
        if (nBox == 2) {
            // 排序：cam0 aMin=3000 < cam1 3030 → cam0 rank0、cam1 rank1
            // cam0: k0=(0+0)*2+0−0=0；cam1: k0=(0+1)*2+1−0=3
            const int x0 = rects[0], x1 = rects[4];
            check(x0 == 0, "cam0 框 x=0");
            check(x1 == 3, "cam1 框 x=3（(0+1)*2+1）");
        }
    }

    // ---- 用例 2：深偏移（cam1 aMin=3060，offset=2）----
    {
        std::printf("[阶段] 用例2 开始\n");
        std::fflush(stdout);
        WriteDat(root + "/cam1/scan.dat", 3000 + 2 * kStep, kLines, kStep, 2000);
        const float* depth = nullptr;
        long long rows = 0, cols = 0;
        Point3DAlg_SickReader reader(cfg);
        try {
            reader.LoadRaw({ root + "/cam0", root + "/cam1" }, -1);
            depth = reader.GetDepth();
            rows = reader.StitchRows();
            cols = reader.StitchCols();
        } catch (const std::exception& e) {
            std::printf("  [EXC] %s\n", e.what());
            std::fflush(stdout);
            check(false, "用例2 抛异常（消息见上）");
            RmDir(root.c_str());
            std::printf("\n存在失败（失败 %d 项）\n", g_fail);
            return 1;
        }
        // maxCols = max(0+30, 2+30) = 32 → totalCols = 64；
        // cam1 k=63 → sc=63/2−2=29 有效 → 右缘 63；k=1/3 是中间零列（左右裁剪不触）→ cols=64
        std::printf("  深偏移拼接图：%lld 列\n", cols);
        check(cols == 64, "列数 = 64（maxCols 32 × 2 相机）");
        CheckCol(depth, rows, cols, 4, 1.002, "深偏移列 4 = cam0 col2");
        CheckCol(depth, rows, cols, 5, 2.000, "深偏移列 5 = cam1 col0（offset=2）");
        CheckCol(depth, rows, cols, 63, 2.029, "深偏移末列 63 = cam1 col29");

        std::vector<int> rects, ids;
        const int nBox = reader.FovBoxes(rects, ids);
        if (nBox == 2) {
            // cam1: k0=(0+2)*2+1=5 —— offset 修复验证点（修复前=1）
            check(rects[4] == 5, "cam1 框 x=5（(0+2)*2+1，offset 修复验证）");
        }
    }

    // ---- 用例 3：单相机直拼（无交织）----
    {
        std::printf("[阶段] 用例3 开始\n");
        std::fflush(stdout);
        Point3DAlg_SickReader reader(cfg);
        const float* depth = nullptr;
        long long cols = 0;
        int cnt = 0;
        const float* cloud = nullptr;
        try {
            reader.LoadRaw(root + "/cam0", -1);
            depth = reader.GetDepth();
            cols = reader.StitchCols();
            cloud = reader.GetCloud(&cnt);
        } catch (const std::exception& e) {
            std::printf("  [EXC] %s\n", e.what());
            std::fflush(stdout);
            check(false, "用例3 抛异常（消息见上）");
            RmDir(root.c_str());
            std::printf("\n存在失败（失败 %d 项）\n", g_fail);
            return 1;
        }
        check(cols == kLines, "单相机列数 = 30");
        CheckCol(depth, reader.StitchRows(), cols, 7, 1.007, "单相机列 7 = raw 1007×0.001");

        // GetCloud x 步距 = scanStep×mm_pulse（单相机不除 n）
        check(cnt > 0, "点云非空");
        if (cnt > 0) {
            const double xStep = kStep * cfg.mm_pulse;
            const double xErr = std::fabs(cloud[0] - 0.0) +
                                std::fabs(cloud[(size_t)(cnt - 1) * 3] - ((double)cols - 1) * xStep);
            char buf[96];
            std::snprintf(buf, sizeof(buf), "点云 x 步距 = %.5f mm（scanStep×mmPulse）", xStep);
            check(xErr < 1e-4, buf);
        }
    }

    // ---- 用例 4：蛇形 2 pass + 自然序（scan2 上行 → scan10 下行）----
    // 文件名故意用 2/10：字典序会把 scan10 排在 scan2 前（pass 判定全乱），
    // ListFiles 自然序修复后按扫描顺序加载——兼验两项修复。
    // pass0 上行 A 3000→3870（raw 1000+i）；pass1 下行 A 3840→2970（raw 2000+i）：
    //   part1 末行 3870 → part2 首行 3840（d=−30，方向翻转）→ 触发换行 → nPass=2。
    {
        std::printf("[阶段] 用例4 蛇形 2 pass 开始\n");
        std::fflush(stdout);
        // 重建目录：清掉 cam1，cam0 内放 scan2/scan10 两个 FOV
        RmDir(root.c_str());
        _mkdir(root.c_str());
        _mkdir((root + "/cam0").c_str());
        // 上行（pass0）：A_i = 3000 + 30i
        WriteDat(root + "/cam0/scan2.dat", 3000, kLines, kStep, 1000);
        // 下行（pass1）：A_i = 3840 − 30i（首行 3840，与 pass0 末行 3870 差 −30）
        {
            FILE* f = std::fopen((root + "/cam0/scan10.dat").c_str(), "wb");
            if (!f) {
                std::printf("写 scan10.dat 失败\n");
                return 1;
            }
            for (int i = 0; i < kLines; ++i)   // 图像块
                for (int c = 0; c < 2560; ++c) {
                    const uint16_t v = (uint16_t)(2000 + i);
                    std::fwrite(&v, sizeof(v), 1, f);
                }
            for (int i = 0; i < kLines; ++i) { // 编码器块（下行递减）
                const int32_t rec[5] = { 3840 - (int32_t)i * kStep, 0x40000000, 0, 0, 0 };
                std::fwrite(rec, sizeof(int32_t), 5, f);
            }
            std::fclose(f);
        }

        Point3DAlg_SickReader reader(cfg);
        const float* depth = nullptr;
        long long rows = 0, cols = 0;
        std::vector<int> rects, ids;
        int nBox = 0;
        try {
            reader.LoadRaw(root + "/cam0", -1);
            depth = reader.GetDepth();
            rows = reader.StitchRows();
            cols = reader.StitchCols();
            nBox = reader.FovBoxes(rects, ids);
        } catch (const std::exception& e) {
            std::printf("  [EXC] %s\n", e.what());
            std::fflush(stdout);
            check(false, "用例4 抛异常（消息见上）");
            RmDir(root.c_str());
            std::printf("\n存在失败（失败 %d 项）\n", g_fail);
            return 1;
        }
        std::printf("  蛇形拼接图：%lld 行 × %lld 列\n", rows, cols);
        std::fflush(stdout);
        // nPass=2（方向翻转换行）→ stitchRows = 2560×2；
        // aMin=2970（pass1 末）、aMax=3870（pass0 末）→ 列数 = 900/30+1 = 31：
        //   pass0 col_i = 1+i（占列 1..30）；pass1 col_i = 29−i（占列 0..29）
        check(rows == 2 * kSickWidth, "行数 = 2560×2（换行判定生效）");
        check(cols == kLines + 1, "列数 = 31（span 900/30 + 1）");
        CheckBand(depth, rows, cols, 3, 0, kSickWidth, 1.002, "蛇形 pass0 列 3（col_i=2）");
        CheckBand(depth, rows, cols, 3, kSickWidth, 2 * kSickWidth, 2.026, "蛇形 pass1 列 3（下行 2000+26）");
        CheckBand(depth, rows, cols, 29, 0, kSickWidth, 1.028, "蛇形 pass0 列 29");
        CheckBand(depth, rows, cols, 29, kSickWidth, 2 * kSickWidth, 2.000, "蛇形 pass1 列 29（下行首）");
        CheckBand(depth, rows, cols, 30, 0, kSickWidth, 1.029, "蛇形尾列 30 仅 pass0");
        CheckBand(depth, rows, cols, 0, kSickWidth, 2 * kSickWidth, 2.029, "蛇形首列 0 仅 pass1");
        check(nBox == 2, "蛇形 FOV 框 = 2");

        // 原始访问懒构建（P2 修复）：Raw()/Encoder() 首次调用时拼合，值正确
        const int32_t* enc = reader.Encoder();
        check(enc[0] == 3000 && enc[kLines - 1] == 3870, "懒构建编码器：pass0 首末行正确");
        check(enc[kLines] == 3840 && enc[2 * kLines - 1] == 2970, "懒构建编码器：pass1 下行序列");
    }

    // ---- 用例 5b：编码器前导 0 外推（2026-11-11 review 修复的回归守卫）----
    // 初版 begPos 从尾向头找"首个非零"恒等于 endPos，外推条件永假（死代码）——
    // 本用例数据前 5 行编码器为 0，若外推不生效则 aMin=0、列数爆炸、双相机校验崩。
    {
        std::printf("[阶段] 用例5b 前导 0 外推开始\n");
        std::fflush(stdout);
        RmDir(root.c_str());
        _mkdir(root.c_str());
        _mkdir((root + "/cam0").c_str());
        _mkdir((root + "/cam1").c_str());
        // cam0：前 5 行编码器 0，行 i≥5 时 A = 3000 + (i−5)×30（外推后 a[0..4] = 2850..2970）
        {
            FILE* f = std::fopen((root + "/cam0/scan0.dat").c_str(), "wb");
            if (!f) { std::printf("写 scan0.dat 失败\n"); return 1; }
            for (int i = 0; i < kLines; ++i)
                for (int c = 0; c < kSickWidth; ++c) {
                    const uint16_t v = (uint16_t)(1000 + i);
                    std::fwrite(&v, sizeof(v), 1, f);
                }
            for (int i = 0; i < kLines; ++i) {
                const int32_t a = (i < 5) ? 0 : (3000 + (int32_t)(i - 5) * kStep);
                const int32_t rec[5] = { a, 0x40000000, 0, 0, 0 };
                std::fwrite(rec, sizeof(int32_t), 5, f);
            }
            std::fclose(f);
        }
        // cam1：正常（无前导 0），A = 3000 + i×30
        WriteDat(root + "/cam1/scan0.dat", 3000, kLines, kStep, 2000);

        Point3DAlg_SickReader reader(cfg);
        const float* depth = nullptr;
        long long rows = 0, cols = 0;
        try {
            reader.LoadRaw({ root + "/cam0", root + "/cam1" }, -1);
            depth = reader.GetDepth();
            rows = reader.StitchRows();
            cols = reader.StitchCols();
        } catch (const std::exception& e) {
            std::printf("  [EXC] %s\n", e.what());
            std::fflush(stdout);
            check(false, "5b 抛异常（外推失效会在这里崩/错列）");
            RmDir(root.c_str());
            std::printf("\n存在失败（失败 %d 项）\n", g_fail);
            return 1;
        }
        const int32_t* enc = reader.Encoder();
        // 外推值：a[i] = a[5] − (5−i)×30 → a[0] = 3000 − 150 = 2850
        std::printf("  前导外推后 a[0]=%d（期望 2850）\n", enc[0]);
        std::fflush(stdout);
        check(enc[0] == 2850, "前导 0 外推：a[0] = 2850（修复验证，死代码时为 0）");
        check(enc[4] == 2970 && enc[5] == 3000, "外推-实测衔接连续");
        // cam0 aMin=2850、aMax=3870 → cam0 35 列；cam1 aMin=3000 → offset=(3000−2850)/30=5
        // maxCols = max(35, 5+30)=35 → totalCols=70；两相机重叠区 [5..34]
        check(rows == kSickWidth, "单 pass 行数");
        check(cols == 70, "列数 = 70（maxCols 35 × 2）");
        CheckCol(depth, rows, cols, 0, 1.000, "外推列 0 = cam0 col0");
        // 偶数列才属 cam0（k 奇 → cam1 左补零区为空）：col 6 ← cam0 sc=3（raw 1003）
        CheckCol(depth, rows, cols, 6, 1.003, "外推列 6 = cam0 col3（偶数列归属）");
        CheckCol(depth, rows, cols, 1, 0.0, "外推列 1 = cam1 左补 0（sc<0）");
        // cam1 首列 k = (0+5)*2+1 = 11 → B 侧 raw 2000+i 映射 cam1 col sc=k/2−5
        CheckCol(depth, rows, cols, 11, 2.000, "cam1 首列 11（offset=5）");
    }

    // ---- 用例 5：SaveDepth 保存接口（映射语义修复的产物级验证）----
    // 2026-11-11 修复：有效点映射 1..65535（旧实现 zmin 处 =0 与"无效=0"撞车）
    {
        const int w = 4, h = 4;
        float depth[16] = { 0 };
        for (int i = 0; i < 16; ++i)
            depth[i] = 0.001f * (i % 4 + 1);   // 行内 0.001..0.004mm
        const int rc = Point3DAlg_SaveDepth("_sd_test.png", depth, w, h, 0, 0);
        FILE* f = std::fopen("_sd_test.png", "rb");
        check(rc == 0 && f != nullptr, "SaveDepth 成功且文件可读");
        if (f) {
            char sig[8] = { 0 };
            std::fread(sig, 1, 8, f);
            std::fseek(f, 0, SEEK_END);
            const long sz = std::ftell(f);
            std::fclose(f);
            check(std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) == 0, "PNG 签名正确");
            check(sz > 60, "文件非空（4×4 16 位 PNG 带头）");
            std::remove("_sd_test.png");
        }
        // 全无效图：AutoZRange 失败 → -3
        float zero[16] = { 0 };
        check(Point3DAlg_SaveDepth("_sd_zero.png", zero, 4, 4, 0, 0) == -3, "全无效图 → -3");
        std::remove("_sd_zero.png");
    }

    // ---- 用例 6：运动表（Scan3DPart json rowY）+ FOV 重叠裁剪 ----
    // rowY = [10.0, 12.0]（mm）→ dy = 2.0mm → rowOff[1] = llround(2.0/0.0035) = 571；
    // overRows = 2560 − 571.43 = 1988.57 → overHalf = 994：
    //   FOV1（pass0）cropBot=994 → 有效 rr ∈ [0, 1566)；FOV2（pass1）cropTop=994
    //   → 有效 rr ∈ [1565, 3131)；stitchRows = 571 + 2560 = 3131。
    // 2026-11-12：运动表/裁剪路径此前零合成覆盖（真实数据走过、回归没守住）。
    {
        std::printf("[阶段] 用例6 运动表+重叠裁剪开始\n");
        std::fflush(stdout);
        RmDir(root.c_str());
        _mkdir(root.c_str());
        _mkdir((root + "/cam0").c_str());
        // 蛇形 2 pass（同用例 4 数据）：scan2 上行 raw 1000+i、scan10 下行 raw 2000+i
        WriteDat(root + "/cam0/scan2.dat", 3000, kLines, kStep, 1000);
        {
            FILE* f = std::fopen((root + "/cam0/scan10.dat").c_str(), "wb");
            if (!f) { std::printf("写 scan10.dat 失败\n"); return 1; }
            for (int i = 0; i < kLines; ++i)
                for (int c = 0; c < kSickWidth; ++c) {
                    const uint16_t v = (uint16_t)(2000 + i);
                    std::fwrite(&v, sizeof(v), 1, f);
                }
            for (int i = 0; i < kLines; ++i) {
                const int32_t rec[5] = { 3840 - (int32_t)i * kStep, 0x40000000, 0, 0, 0 };
                std::fwrite(rec, sizeof(int32_t), 5, f);
            }
            std::fclose(f);
        }
        // 运动表 json：Scan3DPart 前缀 + "Coordinate 3D" 第 3 列 = 行中心 y
        {
            FILE* f = std::fopen((root + "/cam0/Scan3DPart1.json").c_str(), "wb");
            if (!f) { std::printf("写 json 失败\n"); return 1; }
            const char* js = "{\"Coordinate 3D\": [[0,0,10.0,0],[0,0,12.0,0]]}";
            std::fwrite(js, 1, std::strlen(js), f);
            std::fclose(f);
        }

        Point3DAlg_SickReader reader(cfg);
        const float* depth = nullptr;
        long long rows = 0, cols = 0;
        try {
            reader.LoadRaw(root + "/cam0", -1);
            depth = reader.GetDepth();
            rows = reader.StitchRows();
            cols = reader.StitchCols();
        } catch (const std::exception& e) {
            std::printf("  [EXC] %s\n", e.what());
            std::fflush(stdout);
            check(false, "用例6 抛异常（消息见上）");
            RmDir(root.c_str());
            std::printf("\n存在失败（失败 %d 项）\n", g_fail);
            return 1;
        }
        std::printf("  运动表拼接图：%lld 行 × %lld 列\n", rows, cols);
        std::fflush(stdout);
        // rowOff[1] = llround(2.0/0.0035) = 571 → stitchRows = 571+2560 = 3131
        check(rows == 571 + kSickWidth, "stitchRows = 571+2560（运动表 rowY 驱动）");
        // overHalf = llround((2560−571.43)/2) = 994 → FOV1 有效 [0,1566)、FOV2 [1565,3131)
        CheckBand(depth, rows, cols, 3, 0, 1565, 1.002, "运动表 pass0 列 3（裁剪后段）");
        CheckBand(depth, rows, cols, 3, 1566, rows, 2.026, "运动表 pass1 列 3（裁剪后段）");
        // 重叠带语义：rr=1565 被 pass1 后写覆盖（pass0 cropBot 裁至 1566、pass1
        // cropTop 从 571+994=1565 起）——单行边界不断言，两段连续性已验证
        check(cols == kLines + 1, "列数 = 31（蛇形同用例 4）");
    }


    // ---- 用例 7：拼接质量自检（StitchQuality，多相机衔接带）----
    // cam0/cam1 同 z 场（raw 1000+i + 独立噪声 ±2 raw = ±2µm）→ 衔接带配对
    // z 差 σ ≈ 2.8µm、零 outlier；cam1 注入行级缺陷（+50 raw = +0.05mm）→ 检出。
    {
        std::printf("[阶段] 用例7 拼接质量自检开始\n");
        std::fflush(stdout);
        RmDir(root.c_str());
        _mkdir(root.c_str());
        _mkdir((root + "/cam0").c_str());
        _mkdir((root + "/cam1").c_str());
        for (int cam = 0; cam < 2; ++cam) {
            FILE* f = std::fopen((root + "/cam" + std::to_string(cam) + "/scan.dat").c_str(), "wb");
            if (!f) { std::printf("写 dat 失败\n"); return 1; }
            for (int i = 0; i < kLines; ++i)
                for (int c = 0; c < kSickWidth; ++c) {
                    int v = 1000 + i;
                    if (cam == 1 && i == 5) v += 50;               // 单行缺陷 +0.05mm
                    v += (std::rand() % 5) - 2;                    // ±2 raw 噪声
                    const uint16_t vv = (uint16_t)std::max(1, std::min(65535, v));
                    std::fwrite(&vv, sizeof(vv), 1, f);
                }
            for (int i = 0; i < kLines; ++i) {
                const int32_t rec[5] = { 3000 + (int32_t)i * kStep, 0x40000000, 0, 0, 0 };
                std::fwrite(rec, sizeof(int32_t), 5, f);
            }
            std::fclose(f);
        }

        Point3DAlg_SickReader reader(cfg);
        bool ok = false;
        try {
            reader.LoadRaw({ root + "/cam0", root + "/cam1" }, -1);
            reader.GetDepth();
            ok = true;
        } catch (const std::exception& e) {
            std::printf("  [EXC] %s\n", e.what());
        }
        check(ok, "用例7 双相机加载+拼接");
        if (ok) {
            Point3DAlg_StitchQualityResult q;
            const bool qok = reader.StitchQuality(0.02, q);
            check(qok, "StitchQuality 成功");
            if (qok) {
                std::printf("  pairs=%lld dhSGM=%.5f max=%.5f outlier=%lld\n",
                            q.pairs, q.dh_sigma, q.dh_max_abs, q.outlier);
                check(q.pairs > 0, "衔接带配对 > 0");
                // σ 含缺陷列贡献：1 列（3.3% 格）|dh|=0.05 → σ 理论 ≈0.0092
                // （sqrt(0.033×0.05² + 0.0028²)）；无缺陷场景 σ ≈ 0.0028
                check(q.dh_sigma < 0.012, "差值 σ 合理（缺陷列贡献后理论 0.0092）");
                check(q.outlier == 2560, "单行缺陷精确检出（outlier = 2560 格）");
                check(q.dh_max_abs > 0.04, "max|dh| ≈ 0.05");
            }
            // 单相机无衔接带
            Point3DAlg_SickReader solo(cfg);
            solo.LoadRaw(root + "/cam0", -1);
            solo.GetDepth();
            Point3DAlg_StitchQualityResult q2;
            check(!solo.StitchQuality(0.02, q2), "单相机返回 false");
        }
        RmDir(root.c_str());
    }

    RmDir(root.c_str());
    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
