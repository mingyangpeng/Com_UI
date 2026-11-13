// ComUI 统一 C ABI 适配层（BusinessAlg）：
// 把本库的业务 C 接口包装成宿主 native 执行器要求的单一入口签名
// int32 method(const AlgoCInput*, AlgoCOutput*) + algo_free 释放约定（见 docs/THIRD_PARTY_OPS.md §5.5）。
//
// 注意（ABI v1 限制）：AlgoCOutput 只有 image/cloud 输出端口，无数值/text 输出通道——
// RegionRatio 的两个结果（defect_ratio / area）无法作为端口输出，写入 out->error
// 作节点消息返回（与 Point3DAlg_ComuiManualRegist 的 fitness 消息同惯例；
// 宿主当前版本仅在 rc!=0 时读取该字段，成功消息待宿主支持后即可显示），
// node.json 不声明输出端口。
#include "AlgExport.h"
#include "BusinessAlg/include/BusinessAlg_api.h"
#include "Point3DAlg/tools/Point3DAlg_DomeFit.h"

#include <vector>
#include "comui_native_abi.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern "C" {

// ---------------- 统一释放约定 ----------------

BUSINESSALG_EXPORT void algo_free(void* p) {
    free(p);
}

// ---------------- 算子入口：区域缺陷率统计（node.json: business.region.ratio） ----------------
// 参数（按 node.json params 声明顺序提取，宿主 marshal 不带参数名）：
//   0=min_z(number) 1=max_z(number) 2=roi_x(number) 3=roi_y(number)
//   4=roi_w(number) 5=roi_h(number)
// 输入：1 片点云（SoA float[3N]）；输出：无端口，结果并入节点消息。

BUSINESSALG_EXPORT int BusinessAlg_ComuiRegionRatio(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 1 || !in->clouds) { out->error = "输入未连线"; return 1; }

    const AlgoCCloud* src = &in->clouds[0];
    if (src->count <= 0 || !src->xyz) { out->error = "输入点云为空"; return 2; }

    // 按声明顺序提取 6 个 number 参数（kind==0）
    double p[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };   // min_z max_z roi_x roi_y roi_w roi_h
    int seen = 0;
    for (int i = 0; i < in->paramCount && seen < 6; i++)
        if (in->params[i].kind == 0) p[seen++] = in->params[i].d;
    if (seen < 6) { out->error = "参数不足（需要 min_z/max_z/roi_x/roi_y/roi_w/roi_h）"; return 3; }

    double defect_ratio = 0.0;
    double area = 0.0;
    const int rc = BusinessAlg_ComputeRegionRatio(src->xyz, src->count,
                                                  p[0], p[1], p[2], p[3], p[4], p[5],
                                                  &defect_ratio, &area);
    if (rc != 0) {
        static char err[128];
        if (rc == -1) snprintf(err, sizeof(err), "参数非法（空点云或 ROI 宽高<=0）");
        else if (rc == 2) snprintf(err, sizeof(err), "过滤后无有效投影点");
        else snprintf(err, sizeof(err), "计算失败（错误码 %d）", rc);
        out->error = err;
        return 4;
    }

    // 结果并入节点消息（ABI v1 无数值输出端口）
    static char msg[192];
    snprintf(msg, sizeof(msg), "defect_ratio=%.4f area=%.2f mm2", defect_ratio, area);
    out->error = msg;
    return 0;
}

} // extern "C"

// ---------------- 算子入口：bump 高度 NG 判定（node.json: business.bump_ng_check） ----------------
// 业务编排示例：Point3DAlg_DomeFit（基准/被测各拟合）→ Point3DAlg_BumpCompare（一对一
// 匹配 + 高度差 NG）。参数（按 node.json 声明顺序）：ng_threshold(number, 0.02)
// match_radius(number, 0.3) height_threshold(number, 0.02 DomeFit 基面分离阈值)
// 输入：2 片点云（clouds[0]=基准，clouds[1]=被测）
// 输出：NG 差异点云（每配对 bump 一点：x/y=apex，z=高度偏差，正=被测高）+ 统计消息

extern "C" BUSINESSALG_EXPORT int BusinessAlg_ComuiBumpNgCheck(const AlgoCInput* in, AlgoCOutput* out) {
    memset(out, 0, sizeof(*out));
    if (in->cloudCount < 2 || !in->clouds) { out->error = "需要两片输入点云（基准/被测）"; return 1; }
    const AlgoCCloud* ca = &in->clouds[0];
    const AlgoCCloud* cb = &in->clouds[1];
    if (ca->count <= 0 || !ca->xyz) { out->error = "基准点云为空"; return 2; }
    if (cb->count <= 0 || !cb->xyz) { out->error = "被测点云为空"; return 2; }

    double ngThr = 0.02, matchR = 0.3, hThr = 0.02, clusterTol = 0.25;
    int seen = 0;
    for (int i = 0; i < in->paramCount && seen < 4; i++)
        if (in->params[i].kind == 0) {
            if (seen == 0) ngThr = in->params[i].d;
            else if (seen == 1) matchR = in->params[i].d;
            else if (seen == 2) hThr = in->params[i].d;
            else if (seen == 3) clusterTol = in->params[i].d;
            ++seen;
        }

    auto toCloud = [](const AlgoCCloud* c) {
        pcl::PointCloud<pcl::PointXYZ>::Ptr pc(new pcl::PointCloud<pcl::PointXYZ>);
        pc->reserve(c->count);
        for (int i = 0; i < c->count; ++i)
            pc->push_back(pcl::PointXYZ(c->xyz[i * 3], c->xyz[i * 3 + 1], c->xyz[i * 3 + 2]));
        return pc;
    };

    Point3DAlg_DomeFitParams fp;
    fp.height_threshold = hThr;
    fp.cluster_tolerance = clusterTol;
    std::vector<Point3DAlg_DomeBump> refB, testB;
    if (!Point3DAlg_DomeFit(toCloud(ca), fp, refB, nullptr)) {
        out->error = "基准云未检出 bump（检查基面上是否有凸台）";
        return 4;
    }
    if (!Point3DAlg_DomeFit(toCloud(cb), fp, testB, nullptr)) {
        out->error = "被测云未检出 bump";
        return 4;
    }

    Point3DAlg_BumpCompareParams cp;
    cp.ng_threshold = ngThr;
    cp.match_radius = matchR;
    Point3DAlg_BumpCompareResult cr;
    Point3DAlg_BumpCompare(refB, testB, cp, cr);
    if (cr.matched <= 0) {
        out->error = "无配对 bump（检查两片云 XY 是否对齐 / match_radius 是否够大）";
        return 4;
    }

    // NG 差异点云：每配对一点（apex XY + z=高度偏差）
    float* outXyz = (float*)malloc(cr.diff_h.size() * 3 * sizeof(float));
    if (!outXyz) { out->error = "内存不足"; return 3; }
    for (size_t i = 0; i < cr.diff_h.size(); ++i) {
        outXyz[i * 3] = (float)cr.diff_xy[i * 2];
        outXyz[i * 3 + 1] = (float)cr.diff_xy[i * 2 + 1];
        outXyz[i * 3 + 2] = (float)cr.diff_h[i];
    }
    AlgoCCloudOut* arr = (AlgoCCloudOut*)malloc(sizeof(AlgoCCloudOut));
    if (!arr) { free(outXyz); out->error = "内存不足"; return 3; }
    arr->count = (int)cr.diff_h.size();
    arr->xyz = outXyz;
    arr->rgb = nullptr;
    out->clouds = arr;
    out->cloudCount = 1;

    static char msgBuf[512];
    snprintf(msgBuf, sizeof(msgBuf),
             "bump 配对 %d/%d/%d（配对/漏测/多出）| NG %d 个（|dh|>%.4f，%.2f%%）| "
             "dh mean %+.5f rms %.5f max %.5f",
             cr.matched, cr.unmatched_ref, cr.unmatched_test,
             cr.ng_count, ngThr, 100.0 * cr.ng_ratio,
             cr.dh_mean, cr.dh_rms, cr.dh_max_abs);
    out->error = msgBuf;
    return 0;
}
