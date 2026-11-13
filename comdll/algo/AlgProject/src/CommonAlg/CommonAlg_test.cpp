// CommonAlg_test.cpp —— 通用模块验证（FileUtil / Log / Json）。
// 参数写死，输出到 build/output/<Config>/ 下临时文件，跑完自清理。

#include "include/CommonAlg_FileUtil.h"
#include "include/CommonAlg_Json.h"
#include "include/CommonAlg_Log.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <direct.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0;

// 删空目录（std::remove 在 Windows 上对目录行为不一致，用平台 API）
static void RemoveEmptyDir(const char* dir)
{
#ifdef _WIN32
    _rmdir(dir);
#else
    rmdir(dir);
#endif
}

static void check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_fail;
}

// ---------------- FileUtil ----------------
static void testFileUtil()
{
    std::printf("== FileUtil ==\n");
    const std::string base = "commonalg_test_tmp";
    const std::string sub = base + "/sub";
    CommonAlg_MakeDir(sub);
    check(CommonAlg_DirExists(base) && CommonAlg_DirExists(sub), "MakeDir 多级创建");

    // 写 3 个文件（2 个 .dat + 1 个 .json）
    CommonAlg_WriteFile(sub + "/b.dat", "hello-b");
    CommonAlg_WriteFile(sub + "/a.dat", "hello-a");
    CommonAlg_WriteFile(sub + "/c.json", "{}");
    check(CommonAlg_FileExists(sub + "/a.dat"), "WriteFile/FileExists");

    const std::vector<std::string> dats = CommonAlg_ListFiles(sub, ".dat");
    check(dats.size() == 2, "ListFiles .dat → 2 个");
    check(!dats.empty() && CommonAlg_FileName(dats[0]) == "a.dat", "ListFiles 按名排序");

    const std::vector<std::string> parts = CommonAlg_ListFiles(sub, ".json");
    check(parts.size() == 1 && CommonAlg_FileName(parts[0]) == "c.json",
          "ListFiles .json 筛选");

    // 读回
    std::string content;
    check(CommonAlg_ReadFile(sub + "/a.dat", content) && content == "hello-a", "ReadFile 内容正确");
    check(!CommonAlg_ReadFile(sub + "/nope.dat", content), "ReadFile 不存在返回 false");

    // 追加
    CommonAlg_AppendFile(sub + "/a.dat", "-more");
    CommonAlg_ReadFile(sub + "/a.dat", content);
    check(content == "hello-a-more", "AppendFile 追加");

    // 路径操作
    check(CommonAlg_FileName("/a/b/c.dat") == "c.dat", "FileName");
    check(CommonAlg_FileStem("/a/b/c.dat") == "c", "FileStem");
    check(CommonAlg_DirName("/a/b/c.dat") == "/a/b", "DirName");
    check(CommonAlg_Extension("/a/b/c.dat") == ".dat", "Extension");
    check(CommonAlg_JoinPath("/a/b", "c.dat") == "/a/b/c.dat", "JoinPath");
    check(CommonAlg_ReplaceExtension("/a/b/c.dat", ".xml") == "/a/b/c.xml", "ReplaceExtension");
    // 无扩展名 + 目录名带点
    check(CommonAlg_ReplaceExtension("/a.b/c", ".txt") == "/a.b/c.txt", "ReplaceExtension 无扩展名");
    check(CommonAlg_Extension("/a/b/c") == "", "Extension 无扩展名");

    // 按序清理：先文件、再子目录、再父目录（std::remove 不能删非空目录）
    std::remove((sub + "/a.dat").c_str());
    std::remove((sub + "/b.dat").c_str());
    std::remove((sub + "/c.json").c_str());
    RemoveEmptyDir(sub.c_str());
    RemoveEmptyDir(base.c_str());

    // ---- 边界补强：路径含中文与空格 ----
    // CommonAlg 的 A 系列接口按 936 码页（GBK）解释 char 路径，
    // 故用“测试数据/数据”的 GBK 字节直接构造路径（字面量 \x 转义，不依赖执行字符集）。
    {
        const std::string cdir = "commonalg_\xB2\xE2\xCA\xD4\xCA\xFD\xBE\xDD dir";
        check(CommonAlg_MakeDir(cdir), "MakeDir 中文+空格路径");
        check(CommonAlg_DirExists(cdir), "DirExists 中文+空格路径");
        const std::string cfile = cdir + "/\xCA\xFD\xBE\xDD.dat";
        check(CommonAlg_WriteFile(cfile, "cn-data") && CommonAlg_FileExists(cfile),
              "WriteFile 中文+空格路径");
        std::string back;
        check(CommonAlg_ReadFile(cfile, back) && back == "cn-data", "ReadFile 中文+空格路径");
        const std::vector<std::string> listed = CommonAlg_ListFiles(cdir, ".dat");
        check(listed.size() == 1 && CommonAlg_FileName(listed[0]) == "\xCA\xFD\xBE\xDD.dat",
              "ListFiles 中文文件名");
        check(CommonAlg_FileStem(cfile) == "\xCA\xFD\xBE\xDD" &&
              CommonAlg_Extension(cfile) == ".dat", "FileStem/Extension 中文文件名");
        std::remove(cfile.c_str());
        RemoveEmptyDir(cdir.c_str());
    }

    // ---- 边界补强：不存在路径的写失败容错（返回 false 且不抛异常） ----
    {
        check(!CommonAlg_WriteFile("commonalg_nosuch_dir/f.dat", "x"),
              "不存在目录 WriteFile 返回 false");
        check(!CommonAlg_AppendFile("commonalg_nosuch_dir/f.dat", "x"),
              "不存在目录 AppendFile 返回 false");
        // 父路径是普通文件（不是目录）→ 打开必然失败（跨平台）
        CommonAlg_WriteFile("commonalg_test_plainfile", "p");
        check(!CommonAlg_WriteFile("commonalg_test_plainfile/x.dat", "x"),
              "父路径为文件 WriteFile 返回 false");
        std::remove("commonalg_test_plainfile");
    }

    // ---- 边界补强：只读写失败容错（Windows 目录只读属性不阻止新建文件，
    //      故以"只读文件覆写/追加失败"验证 fopen 写路径容错；结束恢复属性） ----
    {
        const std::string rodir = "commonalg_test_rodir";
        CommonAlg_MakeDir(rodir);
        const std::string rofile = rodir + "/existing.dat";
        CommonAlg_WriteFile(rofile, "ro");
#ifdef _WIN32
        check(SetFileAttributesA(rofile.c_str(), FILE_ATTRIBUTE_READONLY) != 0,
              "设置只读文件属性");
        check(!CommonAlg_WriteFile(rofile, "x"), "只读文件覆写失败");
        check(!CommonAlg_AppendFile(rofile, "x"), "只读文件追加失败");
        SetFileAttributesA(rofile.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::string roBack;
        check(CommonAlg_ReadFile(rofile, roBack) && roBack == "ro", "只读期间文件内容未损坏");
#endif
        std::remove(rofile.c_str());
        RemoveEmptyDir(rodir.c_str());
    }

    // ---- 边界补强：存在与不存在混合的枚举 ----
    {
        const std::string mroot = "commonalg_test_mixdir";
        CommonAlg_MakeDir(mroot + "/nested");
        CommonAlg_WriteFile(mroot + "/f1.txt", "1");
        CommonAlg_WriteFile(mroot + "/f2.txt", "2");
        CommonAlg_WriteFile(mroot + "/f3.dat", "3");
        CommonAlg_WriteFile(mroot + "/nested/deep.txt", "d");   // 子目录内容不得混入枚举
        const std::vector<std::string> all = CommonAlg_ListFiles(mroot, "");
        check(all.size() == 3, "ListFiles 空后缀=全部文件（3 个，跳过子目录）");
        const std::vector<std::string> txt = CommonAlg_ListFiles(mroot, ".txt");
        check(txt.size() == 2 && CommonAlg_FileName(txt[0]) == "f1.txt" &&
              CommonAlg_FileName(txt[1]) == "f2.txt", "ListFiles .txt 混合枚举 2 个");
        check(CommonAlg_ListFiles(mroot, ".xyz").empty(), "ListFiles 无匹配返回空");
        check(CommonAlg_ListFiles(mroot + "/nested", ".txt").size() == 1,
              "ListFiles 子目录单独枚举");
        check(CommonAlg_ListFiles("commonalg_test_missing_dir_9x", ".txt").empty(),
              "ListFiles 不存在目录返回空");
        check(CommonAlg_DirExists(mroot + "/nested") && !CommonAlg_DirExists(mroot + "/f1.txt"),
              "DirExists 目录 true/文件 false");
        check(CommonAlg_FileExists(mroot + "/f1.txt") && !CommonAlg_FileExists(mroot + "/nested"),
              "FileExists 文件 true/目录 false");
        std::remove((mroot + "/nested/deep.txt").c_str());
        std::remove((mroot + "/f1.txt").c_str());
        std::remove((mroot + "/f2.txt").c_str());
        std::remove((mroot + "/f3.dat").c_str());
        RemoveEmptyDir((mroot + "/nested").c_str());
        RemoveEmptyDir(mroot.c_str());
    }
}

// ---------------- Log ----------------
static void testLog()
{
    std::printf("== Log ==\n");
    const std::string logPath = "commonalg_test.log";
    std::remove(logPath.c_str());

    check(CommonAlg_LogSetFile(logPath), "LogSetFile");
    CommonAlg_LogSetLevel(CommonAlg_LogLevel::Info);

    LOG_INFO("测试信息 %d", 42);
    LOG_WARN("测试警告 %s", "warn");
    LOG_ERROR("测试错误 %.2f", 1.5);
    LOG_DEBUG("这条应被过滤掉");          // 低于 Info，不写
    LOG_TRACE("这条也应被过滤");

    std::string content;
    CommonAlg_ReadFile(logPath, content);
    check(content.find("测试信息 42") != std::string::npos, "INFO 已写入");
    check(content.find("测试警告 warn") != std::string::npos, "WARN 已写入");
    check(content.find("测试错误 1.50") != std::string::npos, "ERROR 已写入");
    check(content.find("过滤掉") == std::string::npos &&
          content.find("也应被过滤") == std::string::npos, "低于级别被过滤");
    // 格式检查：[时间][级别][文件:行]
    check(content.find("[INFO ]") != std::string::npos &&
          content.find("[WARN ]") != std::string::npos &&
          content.find("[ERROR]") != std::string::npos, "级别标签格式");

    // 切到 Debug 后 Debug 级别可见
    CommonAlg_LogSetLevel(CommonAlg_LogLevel::Debug);
    LOG_DEBUG("debug-visible");
    CommonAlg_ReadFile(logPath, content);
    check(content.find("debug-visible") != std::string::npos, "切级后 Debug 可见");

    // Off 全关
    CommonAlg_LogSetLevel(CommonAlg_LogLevel::Off);
    const size_t before = content.size();
    LOG_ERROR("off-should-not-appear");
    CommonAlg_ReadFile(logPath, content);
    check(content.size() == before, "Off 级别全部关闭");

    CommonAlg_LogSetLevel(CommonAlg_LogLevel::Info);
    CommonAlg_LogCloseFile();
    std::remove(logPath.c_str());

    // 深层路径：目录由调用方创建（库不代劳）
    // 先清掉上次运行可能残留的目录，否则"目录不存在"的前置条件不成立
    std::remove("commonalg_logdir/sub/app.log");
    RemoveEmptyDir("commonalg_logdir/sub");
    RemoveEmptyDir("commonalg_logdir");
    const std::string deepLog = "commonalg_logdir/sub/app.log";
    check(!CommonAlg_LogSetFile(deepLog), "目录不存在时 LogSetFile 返回 false");
    CommonAlg_MakeDir("commonalg_logdir/sub");
    check(CommonAlg_LogSetFile(deepLog), "建好目录后 LogSetFile 成功");
    LOG_INFO("deep-log");
    std::string deepContent;
    CommonAlg_ReadFile(deepLog, deepContent);
    check(deepContent.find("deep-log") != std::string::npos, "深层路径日志写入");
    CommonAlg_LogCloseFile();
    std::remove(deepLog.c_str());
    RemoveEmptyDir("commonalg_logdir/sub");
    RemoveEmptyDir("commonalg_logdir");

    // 无法打开时返回 false 且不残留状态（再写日志只进 stderr，不崩）
    // 用 Windows 非法文件名（含 '?' 通配符）确保 fopen 必然失败
    check(!CommonAlg_LogSetFile("commonalg_bad?.log"), "LogSetFile 失败返回 false");
    LOG_INFO("after-failed-setfile");        // 不应崩溃
    check(true, "SetFile 失败后写日志不崩");
    CommonAlg_LogCloseFile();

    // ---- 边界补强：4 线程并发写同一日志文件（简单验证：不崩、无交叉损坏、条数正确） ----
    {
        const std::string concPath = "commonalg_test_concurrent.log";
        std::remove(concPath.c_str());
        check(CommonAlg_LogSetFile(concPath), "LogSetFile 并发用例");
        CommonAlg_LogSetLevel(CommonAlg_LogLevel::Info);
        {
            std::vector<std::thread> workers;
            for (int w = 0; w < 4; ++w) {
                workers.emplace_back([w]() {
                    for (int i = 0; i < 20; ++i)
                        LOG_INFO("w%d-%d", w, i);
                });
            }
            for (std::thread& t : workers)
                t.join();
        }
        std::string content;
        check(CommonAlg_ReadFile(concPath, content), "并发后日志文件可读");
        bool intact = true;    // 每条记录以 \n 结尾：\n 后必须是下一条的 '['（无交叉写入）
        for (size_t pos = content.find('\n');
             intact && pos != std::string::npos && pos + 1 < content.size();
             pos = content.find('\n', pos + 1))
            intact = (content[pos + 1] == '[');
        check(intact, "并发写入无交叉损坏");
        int records = 0;
        for (size_t pos = content.find("CommonAlg_test.cpp:");
             pos != std::string::npos;
             pos = content.find("CommonAlg_test.cpp:", pos + 1))
            ++records;
        check(records == 80, "4 线程 × 20 条 = 80 条");
        CommonAlg_LogSetLevel(CommonAlg_LogLevel::Info);
        CommonAlg_LogCloseFile();
        std::remove(concPath.c_str());
    }
}

// ---------------- Json ----------------
static void testJson()
{
    std::printf("== Json ==\n");

    // 标准 UTF-8 JSON
    const std::string p1 = "commonalg_test1.json";
    CommonAlg_WriteFile(p1,
        "{\n"
        "  \"name\": \"part-a\",\n"
        "  \"scale\": 0.002382,\n"
        "  \"count\": 13,\n"
        "  \"xyz\": [0, 1.5, -2.5, 9],\n"
        "  \"groups\": [[1, 2], [3.5, 4.5, 5.5]],\n"
        "  \"rows\": [[0, 0, 10.5], [1, 1, 20.5]]\n"
        "}\n");
    {
        JsonDoc doc;
        check(doc.load(p1.c_str()), "load UTF-8");
        check(doc["name"].string_() == "part-a", "string_ 取值");
        check(std::fabs(doc["scale"].float_() - 0.002382) < 1e-9, "float_ 取值");
        check(doc["count"].float_() == 13.0, "整数转 double");
        const std::vector<double> xyz = doc["xyz"].array();
        check(xyz.size() == 4 && xyz[2] == -2.5, "array 数值数组");
        const std::vector<double> g1 = doc["groups"].group(1);
        check(g1.size() == 3 && g1[0] == 3.5, "group 二维数组第 1 组");
        check(doc["rows"].size() == 2, "size 数组长度");
        check(doc["rows"].at(1).array()[2] == 20.5, "at 数组第 i 行");
        // 缺键/类型错 → 默认值
        check(doc["nope"].float_(-9) == -9.0, "缺键返回默认值");
        check(doc["name"].float_(-1) == -1.0, "类型不符返回默认值");
        check(!doc["nope"].has(), "has 缺键 false");
        check(doc["groups"].group(9).empty(), "group 越界返回空");
        check(doc["rows"].at(99).array().empty(), "at 越界返回空");
    }
    std::remove(p1.c_str());

    // array 对非数值元素：跳过而非填 0
    const std::string pMix = "commonalg_test_mix.json";
    CommonAlg_WriteFile(pMix, "{\"mix\": [1, \"abc\", 3, null, 5]}");
    {
        JsonDoc doc;
        doc.load(pMix.c_str());
        const std::vector<double> a = doc["mix"].array();
        check(a.size() == 3 && a[0] == 1.0 && a[1] == 3.0 && a[2] == 5.0,
              "array 跳过非数值元素（不填 0）");
    }
    std::remove(pMix.c_str());

    // GBK 编码 JSON（中文注释键）——nlohmann 严格 UTF-8，靠编码检测兜底
    const std::string p2 = "commonalg_test2.json";
    {
        // GBK 字节：0xBE 0xB5 0xCD 0xB7 = "镜头"
        const char gbk[] = "{\n"
                           "  \"LensRatio\": \"5x\",\n"
                           "  \"LensRatio \xBE\xB5\xCD\xB7 5x 10x\": 0,\n"
                           "  \"scanpulses\": 0.02\n"
                           "}\n";
        CommonAlg_WriteFile(p2, std::string(gbk, sizeof(gbk) - 1));
        JsonDoc doc;
        check(doc.load(p2.c_str()), "load GBK（编码自动转 UTF-8）");
        check(doc["LensRatio"].string_() == "5x", "GBK 文件按键取值正确");
        check(std::fabs(doc["scanpulses"].float_() - 0.02) < 1e-9, "GBK 文件数值正确");
    }
    std::remove(p2.c_str());

    // UTF-8 BOM
    const std::string p3 = "commonalg_test3.json";
    CommonAlg_WriteFile(p3, std::string("\xEF\xBB\xBF{\"k\": 42}", 12));
    {
        JsonDoc doc;
        check(doc.load(p3.c_str()) && doc["k"].float_() == 42.0, "load UTF-8 BOM");
    }
    std::remove(p3.c_str());

    // 编码工具直接调用
    {
        std::string utf8;
        const std::string ascii = "{\"a\":1}";
        check(CommonAlg_IsValidUtf8(ascii) && CommonAlg_ToUtf8(ascii, utf8) && utf8 == ascii,
              "IsValidUtf8/ToUtf8 纯 ASCII");
        const std::string gbkStr("\xBE\xB5\xCD\xB7", 4);
        check(!CommonAlg_IsValidUtf8(gbkStr), "IsValidUtf8 GBK 返回 false");
        std::string converted;
        check(CommonAlg_GbkToUtf8(gbkStr, converted) && converted.size() == 6,
              "GbkToUtf8 4 字节 GBK → 6 字节 UTF-8");
    }

    // 坏 JSON
    const std::string p4 = "commonalg_test4.json";
    CommonAlg_WriteFile(p4, "{ this is not json");
    {
        JsonDoc doc;
        check(!doc.load(p4.c_str()), "坏 JSON 返回 false");
        check(doc["x"].float_(-5) == -5.0, "坏 JSON 后取值安全");
    }
    std::remove(p4.c_str());

    // 文件不存在
    {
        JsonDoc doc;
        check(!doc.load("definitely_not_exist_12345.json"), "文件不存在返回 false");
    }

    // ---- 边界补强：BOM + GBK 混合（ToUtf8 剥 BOM 后不重检测，剩余 GBK 字节 parse 必败） ----
    {
        const std::string pBomGbk = "commonalg_test_bom_gbk.json";
        const std::string bomGbk = std::string("\xEF\xBB\xBF") +
                                   "{\"LensRatio \xBE\xB5\xCD\xB7\": 0}";
        CommonAlg_WriteFile(pBomGbk, bomGbk);
        JsonDoc doc;
        check(!doc.load(pBomGbk.c_str()), "BOM+GBK 混合返回 false（BOM 后不重检测）");
        std::remove(pBomGbk.c_str());
    }

    // ---- 边界补强：非法 JSON（中段截断，非只有头部） ----
    {
        const std::string pCut = "commonalg_test_cut.json";
        CommonAlg_WriteFile(pCut,
            "{\n"
            "  \"name\": \"part-a\",\n"
            "  \"xyz\": [1, 2,\n");          // 数组中段截断
        JsonDoc doc;
        check(!doc.load(pCut.c_str()), "中段截断 JSON 返回 false");
        check(doc["name"].string_("dft") == "dft", "截断 JSON 后取值安全");
        std::remove(pCut.c_str());
    }

    // ---- 边界补强：嵌套结构取值 ----
    {
        const std::string pNest = "commonalg_test_nest.json";
        CommonAlg_WriteFile(pNest,
            "{\"rows\": [[1, 2, 3], [4, \"x\", 6], [7, 8, 9]],"
            " \"tags\": [[\"a\", \"b\"], [\"c\"]]}");
        JsonDoc doc;
        check(doc.load(pNest.c_str()), "load 嵌套 JSON");
        check(doc["rows"].at(0).array().size() == 3 &&
              doc["rows"].at(0).array()[2] == 3.0, "嵌套第 0 行取值");
        const std::vector<double> mid = doc["rows"].at(1).array();
        check(mid.size() == 2 && mid[0] == 4.0 && mid[1] == 6.0, "嵌套内层跳过非数值（不填 0）");
        check(doc["rows"].at(2).at(1).float_(-1) == 8.0, "at 链式取内层元素");
        check(doc["rows"].at(5).at(0).float_(-7) == -7.0, "越界 at 链返回默认值");
        check(doc["rows"].group(2).size() == 3 && doc["rows"].group(2)[0] == 7.0,
              "group 取嵌套二维数组");
        check(doc["tags"].at(0).at(1).string_() == "b", "嵌套字符串元素取值");
        std::remove(pNest.c_str());
    }

    // ---- 边界补强：数值边界（大整数/负数/科学计数法） ----
    {
        const std::string pNum = "commonalg_test_num.json";
        CommonAlg_WriteFile(pNum,
            "{\"big\": 12345678901234567890, \"neg\": -987654.321,"
            " \"tiny\": 1.25e-8, \"huge\": 1e300}");
        JsonDoc doc;
        check(doc.load(pNum.c_str()), "load 数值边界 JSON");
        const double big = doc["big"].float_();
        check(big > 1e19 && std::fabs(big - 1.2345678901234567e19) / big < 1e-15,
              "大整数（超 int64）按 double 取值");
        check(doc["neg"].float_() < 0 &&
              std::fabs(doc["neg"].float_() + 987654.321) < 1e-9, "负数取值");
        check(std::fabs(doc["tiny"].float_() - 1.25e-8) < 1e-20, "科学计数法极小值");
        check(doc["huge"].float_() == 1e300, "极大值 1e300");
        std::remove(pNum.c_str());
    }

    // ---- 边界补强：空对象 / 空数组 / 空字符串 / null ----
    {
        const std::string pEmpty = "commonalg_test_empty.json";
        CommonAlg_WriteFile(pEmpty, "{\"obj\": {}, \"arr\": [], \"s\": \"\", \"n\": null}");
        JsonDoc doc;
        check(doc.load(pEmpty.c_str()), "load 空容器 JSON");
        check(doc["arr"].array().empty() && doc["arr"].size() == 0, "空数组取值安全");
        check(doc["arr"].at(0).float_(-3) == -3.0, "空数组 at(0) 默认值");
        check(doc["obj"].has() && doc["obj"].float_(7) == 7.0,
              "空对象存在但取值走默认");
        check(doc["s"].has() && doc["s"].string_() == "", "空字符串可取且 has true");
        check(!doc["n"].has() && doc["n"].float_(-2) == -2.0, "null 视为缺键");
        std::remove(pEmpty.c_str());
    }

    // ---- 边界补强：键不存在容错（全部取值方法不抛异常） ----
    {
        const std::string pMiss = "commonalg_test_miss.json";
        CommonAlg_WriteFile(pMiss, "{\"a\": 1}");
        JsonDoc doc;
        check(doc.load(pMiss.c_str()), "load 键容错 JSON");
        check(!doc["missing"].has(), "键不存在 has false");
        check(doc["missing"].float_(-11) == -11.0 &&
              doc["missing"].string_("dft") == "dft", "键不存在返回默认值");
        check(doc["missing"].array().empty() && doc["missing"].size() == 0,
              "键不存在 array/size 安全");
        check(doc["missing"].at(0).float_(-4) == -4.0 &&
              doc["missing"].group(0).empty(), "键不存在 at/group 安全");
        std::remove(pMiss.c_str());
    }
}

int main()
{
    testFileUtil();
    testLog();
    testJson();
    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部测试通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
