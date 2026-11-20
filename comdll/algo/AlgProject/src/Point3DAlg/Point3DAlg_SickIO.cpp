// Point3DAlg_SickIO.cpp
// SICK 一步式 C 接口：读扫描文件夹 → 点云（内部走 Point3DAlg_SickReader 类链路，
// 换算参数自动从两个 json 读取，见 Point3DAlg_SickReader.h）。
// 1.1.0 追加：版本/生命周期、通用点云容器释放、SICK 深层接口（句柄族，
// 声明见 Point3DAlg_SickApi.h——原始图/编码器/行中心 Y/强度/拼接质量/FOV 框）。
// 本文件全部接口不依赖 PCL，任何构建配置下都导出。

#include "include/Point3DAlg_api.h"
#include "include/Point3DAlg_SickReader.h"
#include "include/Point3DAlg_SickApi.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// =====================================================================
// 版本与生命周期（占位实现，对齐 Image2DAlg；模块无全局状态）
// =====================================================================

const char* Point3DAlg_GetVersion(void)
{
    return "1.3.0";
}

int Point3DAlg_Init(void)
{
    return 0;
}

void Point3DAlg_Cleanup(void)
{
}

// =====================================================================
// 通用点云容器释放（NULL 安全）
// =====================================================================

void Point3DAlg_FreeCloud(Point3DAlg_PointCloud* cloud)
{
    if (!cloud)
        return;
    free(cloud->xyz);
    cloud->xyz = nullptr;
    cloud->count = 0;
}

// =====================================================================
// 一步式读取（既有接口）
// =====================================================================

int Point3DAlg_ReadSickCloud(const char* folder, int frame,
                             const char* recipe_path, const char* common_path,
                             float** out_xyz, int* out_count)
{
    if (!folder || !folder[0] || !out_xyz || !out_count)
        return -1;
    *out_xyz = nullptr;
    *out_count = 0;

#ifdef _WIN32
    // ---- 预检：无 .dat → -2；帧号越界 → -3（对齐 api.h 错误码契约；
    //      此前两类错误在异常里被吞成 -4，调用方无法区分）----
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((std::string(folder) + "/*.dat").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            return -2;
        int nDat = 0;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                ++nDat;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (frame >= 0 && frame >= nDat)
            return -3;
    }
#endif

    try {
        Point3DAlg_SickConfig cfg;
        char err[256] = { 0 };
        const int rc = Point3DAlg_LoadSickConfig(recipe_path, common_path, &cfg, err, sizeof(err));
        if (rc != 0)
            return -4;

        Point3DAlg_SickReader reader(cfg);
        reader.LoadRaw(std::vector<std::string>{ std::string(folder) }, frame);

        int count = 0;
        const float* cloud = reader.GetCloud(&count);
        float* xyz = (float*)malloc((size_t)count * 3 * sizeof(float));
        if (!xyz)
            return -4;
        memcpy(xyz, cloud, (size_t)count * 3 * sizeof(float));
        *out_xyz = xyz;
        *out_count = count;
        return 0;
    } catch (const std::exception&) {
        return -4;   // 配置/数据错误，统一错误码
    }
}

void Point3DAlg_FreeBuffer(void* p)
{
    free(p);
}

// =====================================================================
// SICK 深层接口（句柄族；声明与错误码/布局注释见 Point3DAlg_SickApi.h）
// =====================================================================

// 句柄实现：cfg 可整体替换（LoadRawInto 重读配置时），reader 用堆指针
// 以便"先建新、加载成功再换入"——加载失败时保留旧状态（可重试）。
struct Point3DAlg_SickReaderImpl
{
    Point3DAlg_SickConfig cfg;                     // 当前配置
    std::unique_ptr<Point3DAlg_SickReader> reader; // C++ 读取类（未加载 = 空）

    Point3DAlg_SickReaderImpl() : cfg() {}
    explicit Point3DAlg_SickReaderImpl(const Point3DAlg_SickConfig& c) : cfg(c) {}
};

Point3DAlg_SickReaderH Point3DAlg_CreateSickReader(const Point3DAlg_SickConfig* cfg)
{
    if (!cfg)
        return nullptr;
    try {
        return new Point3DAlg_SickReaderImpl(*cfg);
    } catch (...) {
        return nullptr;
    }
}

void Point3DAlg_FreeSickReader(Point3DAlg_SickReaderH handle)
{
    delete handle;
}

// 拆分 ';' 分隔的多相机路径列表（跳过空段；至少 1 个非空段才合法）。
static bool SplitFolders(const char* folder_arg, std::vector<std::string>& out)
{
    if (!folder_arg || !folder_arg[0])
        return false;
    const std::string src(folder_arg);
    size_t beg = 0;
    while (beg <= src.size()) {
        const size_t sep = src.find(';', beg);
        const std::string part = src.substr(beg,
                                            (sep == std::string::npos) ? std::string::npos : sep - beg);
        if (!part.empty())
            out.push_back(part);
        if (sep == std::string::npos)
            break;
        beg = sep + 1;
    }
    return !out.empty();
}

// 加载前预检（与 ReadSickCloud 同策略：把类异常区分为专用错误码）。
// 返回 0 = 通过；-2 = 任一文件夹无 .dat；-3 = frame 越界。
static int PrecheckFolders(const std::vector<std::string>& folders, int frame)
{
#ifdef _WIN32
    for (const std::string& f : folders) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((f + "/*.dat").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            return -2;
        int nDat = 0;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                ++nDat;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (frame >= 0 && frame >= nDat)
            return -3;
    }
#endif
    return 0;
}

int Point3DAlg_LoadRawInto(Point3DAlg_SickReaderH handle,
                           const char* folder, int frame,
                           const char* recipe_path, const char* common_path)
{
    if (!handle || !folder || !folder[0])
        return -1;
    std::vector<std::string> folders;
    if (!SplitFolders(folder, folders))
        return -1;
    try {
        // 配置：非空路径 → 重读并替换；NULL/空串 → 沿用句柄现有 cfg
        Point3DAlg_SickConfig cfg = handle->cfg;
        const bool reread = (recipe_path && recipe_path[0]) || (common_path && common_path[0]);
        if (reread) {
            char err[256] = { 0 };
            if (Point3DAlg_LoadSickConfig(recipe_path, common_path, &cfg, err, sizeof(err)) != 0)
                return -4;
        }
        const int pre = PrecheckFolders(folders, frame);
        if (pre != 0)
            return pre;
        // 先建新对象并完成加载，成功后才换入句柄（失败保留旧状态，可重试）
        std::unique_ptr<Point3DAlg_SickReader> fresh(new Point3DAlg_SickReader(cfg));
        fresh->LoadRaw(folders, frame);
        handle->cfg = cfg;
        handle->reader.swap(fresh);
        return 0;
    } catch (const std::exception&) {
        return -4;   // 配置/数据错误（LoadRaw 校验失败、读文件异常等）
    } catch (...) {
        return -99;
    }
}

// 句柄已加载判定（reader 存在且至少一台相机）
static bool SickLoaded(const Point3DAlg_SickReaderImpl* handle)
{
    return handle && handle->reader && handle->reader->DeviceCount() > 0;
}

int Point3DAlg_GetSickInfo(Point3DAlg_SickReaderH handle,
                           int* out_width, long long* out_rows,
                           long long* out_stitch_rows, long long* out_stitch_cols,
                           int* out_device_count, int* out_has_intensity)
{
    if (!handle)
        return -1;
    try {
        if (!SickLoaded(handle))
            return -2;
        const Point3DAlg_SickReader& r = *handle->reader;
        if (out_width)         *out_width = r.Width();
        if (out_rows)          *out_rows = r.Rows();
        // Stitch* 未 GetDepth 时为 0（不触发拼接，语义见头文件注释）
        if (out_stitch_rows)   *out_stitch_rows = r.StitchRows();
        if (out_stitch_cols)   *out_stitch_cols = r.StitchCols();
        if (out_device_count)  *out_device_count = r.DeviceCount();
        if (out_has_intensity) *out_has_intensity = r.HasIntensity() ? 1 : 0;
        return 0;
    } catch (...) {
        return -99;
    }
}

int Point3DAlg_GetSickRowY(Point3DAlg_SickReaderH handle,
                           double** out_row_y, int* out_count)
{
    if (!handle || !out_row_y || !out_count)
        return -1;
    *out_row_y = nullptr;
    *out_count = 0;
    try {
        if (!SickLoaded(handle))
            return -2;
        const std::vector<double>& rowY = handle->reader->RowY();
        if (rowY.empty())
            return -2;
        double* buf = (double*)malloc(rowY.size() * sizeof(double));
        if (!buf)
            return -4;
        memcpy(buf, rowY.data(), rowY.size() * sizeof(double));
        *out_row_y = buf;
        *out_count = (int)rowY.size();
        return 0;
    } catch (...) {
        return -99;
    }
}

int Point3DAlg_GetSickRaw(Point3DAlg_SickReaderH handle,
                          unsigned short** out_raw, int* out_w, long long* out_h,
                          int** out_encoder)
{
    if (!handle || !out_w || !out_h)
        return -1;
    if (out_raw)     *out_raw = nullptr;
    if (out_encoder) *out_encoder = nullptr;
    *out_w = 0;
    *out_h = 0;
    try {
        if (!SickLoaded(handle))
            return -2;
        const int w = handle->reader->Width();
        const long long rows = handle->reader->Rows();
        if (w <= 0 || rows <= 0)
            return -2;
        const unsigned short* raw = handle->reader->Raw();      // 懒拼合（首次调用）
        const int32_t* enc = handle->reader->Encoder();
        if (!raw || !enc)
            return -2;
        *out_w = w;
        *out_h = rows;
        if (out_raw) {
            unsigned short* buf = (unsigned short*)malloc((size_t)rows * (size_t)w
                                                          * sizeof(unsigned short));
            if (!buf)
                return -4;
            memcpy(buf, raw, (size_t)rows * (size_t)w * sizeof(unsigned short));
            *out_raw = buf;
        }
        if (out_encoder) {
            int* buf = (int*)malloc((size_t)rows * sizeof(int));
            if (!buf) {
                // 部分失败：回收本调用已交付的 raw 副本，保持"出错即全空"契约
                if (out_raw) { free(*out_raw); *out_raw = nullptr; }
                *out_w = 0;
                *out_h = 0;
                return -4;
            }
            memcpy(buf, enc, (size_t)rows * sizeof(int));
            *out_encoder = buf;
        }
        return 0;
    } catch (...) {
        return -99;
    }
}

int Point3DAlg_GetSickIntensity(Point3DAlg_SickReaderH handle,
                                unsigned char** out_img, int* out_w, long long* out_h,
                                unsigned char** out_cloud_int, int* out_point_count)
{
    if (!handle || !out_img || !out_w || !out_h)
        return -1;
    if (out_cloud_int)    *out_cloud_int = nullptr;
    if (out_point_count)  *out_point_count = 0;
    *out_img = nullptr;
    *out_w = 0;
    *out_h = 0;
    try {
        if (!SickLoaded(handle))
            return -2;
        // 强度图随拼接图生成：触发 GetDepth（无有效数据会抛异常 → -4）
        const float* depth = handle->reader->GetDepth();
        if (!handle->reader->HasIntensity())
            return -2;
        const std::vector<unsigned char>& img = handle->reader->GetIntensity();
        const long long sRows = handle->reader->StitchRows();
        const long long sCols = handle->reader->StitchCols();
        if (img.empty() || sRows <= 0 || sCols <= 0 ||
            img.size() != (size_t)sRows * (size_t)sCols || !depth)
            return -2;
        unsigned char* imgBuf = (unsigned char*)malloc(img.size());
        if (!imgBuf)
            return -4;

        // 逐点强度（可选输出）：按 GetCloud 的生成顺序（拼接图行主序、
        // 跳过 0 深度格）遍历，第 j 个非零格即第 j 个点，取强度图同格值。
        // 先取点云并校验、再交付强度图——保证任何失败路径都无部分交付
        //（头文件契约："输出指针在出错时统一置 NULL/0"）
        int count = 0;
        const float* cloud = nullptr;
        if (out_cloud_int && out_point_count) {
            cloud = handle->reader->GetCloud(&count);
            if (!cloud || count <= 0) {
                free(imgBuf);
                return -2;
            }
        }

        memcpy(imgBuf, img.data(), img.size());
        *out_img = imgBuf;
        *out_w = (int)sCols;
        *out_h = sRows;

        if (out_cloud_int && out_point_count) {
            unsigned char* ci = (unsigned char*)malloc((size_t)count);
            if (!ci) {
                // 部分失败：回收已交付的强度图，保持"出错即全空"契约
                free(imgBuf);
                *out_img = nullptr;
                *out_w = 0;
                *out_h = 0;
                return -4;
            }
            size_t j = 0;
            for (long long r = 0; r < sRows && j < (size_t)count; ++r) {
                const float* zRow = depth + (size_t)r * (size_t)sCols;
                const unsigned char* iRow = &img[(size_t)r * (size_t)sCols];
                for (long long c = 0; c < sCols; ++c) {
                    if (zRow[c] == 0.0f)
                        continue;
                    ci[j++] = iRow[c];
                    if (j >= (size_t)count)
                        break;
                }
            }
            if (j < (size_t)count)
                memset(ci + j, 0, (size_t)count - j);   // 防御：与 GetCloud 不同步时补 0
            *out_cloud_int = ci;
            *out_point_count = count;
        }
        return 0;
    } catch (const std::exception&) {
        return -4;
    } catch (...) {
        return -99;
    }
}

int Point3DAlg_StitchQuality(Point3DAlg_SickReaderH handle,
                             double diff_threshold,
                             Point3DAlg_StitchQualityResult* out)
{
    if (!handle || !out)
        return -1;
    if (diff_threshold <= 0.0)
        return -3;
    try {
        if (!SickLoaded(handle))
            return -2;
        (void)handle->reader->GetDepth();   // 拼接质量基于深度图（无有效数据抛异常 → -4）
        if (!handle->reader->StitchQuality(diff_threshold, *out))
            return -2;   // 单相机（无衔接带）或无有效配对格
        return 0;
    } catch (const std::exception&) {
        return -4;
    } catch (...) {
        return -99;
    }
}

int Point3DAlg_GetFovBoxes(Point3DAlg_SickReaderH handle,
                           int** out_rects, int** out_ids, int* out_count)
{
    if (!handle || !out_rects || !out_ids || !out_count)
        return -1;
    *out_rects = nullptr;
    *out_ids = nullptr;
    *out_count = 0;
    try {
        if (!SickLoaded(handle))
            return -2;
        (void)handle->reader->GetDepth();   // FOV 框随拼接记录（最终拼接图坐标）
        std::vector<int> rects, ids;
        const int n = handle->reader->FovBoxes(rects, ids);
        if (n <= 0 || rects.size() < (size_t)n * 4 || ids.size() < (size_t)n)
            return -2;
        int* rbuf = (int*)malloc((size_t)n * 4 * sizeof(int));
        if (!rbuf)
            return -4;
        int* ibuf = (int*)malloc((size_t)n * sizeof(int));
        if (!ibuf) {
            free(rbuf);
            return -4;
        }
        memcpy(rbuf, rects.data(), (size_t)n * 4 * sizeof(int));
        memcpy(ibuf, ids.data(), (size_t)n * sizeof(int));
        *out_rects = rbuf;
        *out_ids = ibuf;
        *out_count = n;
        return 0;
    } catch (const std::exception&) {
        return -4;
    } catch (...) {
        return -99;
    }
}
