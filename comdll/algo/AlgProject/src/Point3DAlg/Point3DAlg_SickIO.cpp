// Point3DAlg_SickIO.cpp
// SICK 一步式 C 接口：读扫描文件夹 → 点云（内部走 Point3DAlg_SickReader 类链路，
// 换算参数自动从两个 json 读取，见 Point3DAlg_SickReader.h）。

#include "include/Point3DAlg_api.h"
#include "include/Point3DAlg_SickReader.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

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
