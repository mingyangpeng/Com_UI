#include "include/Image2DAlg_api.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <stdlib.h>
#include <string.h>

using namespace cv;

static int g_initialized = 0;

const char* Image2DAlg_GetVersion(void) {
    return "2.3.0";
}

int Image2DAlg_Init(void) {
    if (g_initialized) return 0;
    g_initialized = 1;
    return 0;
}

void Image2DAlg_Cleanup(void) {
    g_initialized = 0;
}

void Image2DAlg_FreeImage(Image2DAlg_Image* image) {
    if (image && image->data) {
        free(image->data);
        image->data = NULL;
    }
}

int Image2DAlg_ReadImage(const char* img_path, Image2DAlg_Image* out_image) {
    if (!img_path || !out_image) return -1;
    Mat img = imread(img_path, IMREAD_UNCHANGED);
    if (img.empty()) return -2;
    Mat bgra;
    if (img.channels() == 4) {
        bgra = img;
        if (!bgra.isContinuous()) {
            bgra = bgra.clone();
        }
    } else if (img.channels() == 3) {
        cvtColor(img, bgra, COLOR_BGR2BGRA);
    } else if (img.channels() == 1) {
        cvtColor(img, bgra, COLOR_GRAY2BGRA);
    } else {
        return -3;
    }
    if (!bgra.isContinuous()) {
        bgra = bgra.clone();
    }
    out_image->width = bgra.cols;
    out_image->height = bgra.rows;
    out_image->channels = 4;
    size_t sz = (size_t)bgra.cols * (size_t)bgra.rows * 4;
    out_image->data = (unsigned char*)malloc(sz);
    if (!out_image->data) return -4;
    memcpy(out_image->data, bgra.data, sz);
    return 0;
}
