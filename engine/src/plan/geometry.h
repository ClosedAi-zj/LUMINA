// T0.4：几何阶段（架构 §4 S1 / M0 T0.4）
// - 裁剪 + 拉直(角度) + 透视系数 + 输出缩放 合成为单一 3x3 单应（double 精度）
// - 单应正向：src（裁剪坐标系，像素）→ dst（输出像素）；求逆后每 dst 像素
//   逆映射一次 + 单次源采样 → 采样次数恒 = 输出像素数（断言）
// - 源采样由调用方注入（通常为 imaging::resampleImage16 的核），本模块只负责
//   坐标映射与计数；双三次以上质量由核参数保证
#ifndef LUMINA_PLAN_GEOMETRY_H_
#define LUMINA_PLAN_GEOMETRY_H_

#include <cstdint>
#include <string>

#include "imaging/image_buffer.h"
#include "imaging/resample.h"

namespace lumina {
namespace plan {

struct GeometryParams {
  // 裁剪区域（源图像像素坐标；w/h <= 0 表示不裁剪，取整图）
  int cropX = 0;
  int cropY = 0;
  int cropW = 0;
  int cropH = 0;
  double angleDeg = 0.0;      // 拉直角度：逆时针为正，绕裁剪中心旋转
  double perspectiveH = 0.0;  // 水平透视系数：w' = 1 + pH * x_norm + pV * y_norm
  double perspectiveV = 0.0;  // 垂直透视系数（x_norm/y_norm ∈ [-1,1] 归一化）
  double scale = 1.0;         // 输出相对裁剪区域的缩放（>0）
};

// 3x3 单应（行主序），乘法与求逆
struct Mat3 {
  double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

// src→dst 正向单应；失败（非法参数/奇异矩阵）返回 false 并写 err
bool buildForwardHomography(const GeometryParams& p, const imaging::ImageBuf& src,
                            Mat3* fwd, int* outW, int* outH, std::string* err);

// 行主序 3x3 求逆（伴随法）；奇异（行列式≈0）返回 false
bool invert3(const Mat3& a, Mat3* inv);

// 计算正向单应下四角映射的包围盒（输出尺寸依据）
void mapQuadBounds(const Mat3& fwd, int w, int h,
                   double* minX, double* minY, double* maxX, double* maxY);

// 几何变换主入口：
// - 合成单应 + 求逆，每 dst 像素逆映射得到 src 坐标，核采样一次
// - sampleCount 非空时累加采样次数（== dst 像素数，供断言）
// - 失败返回 false 并写 err
bool applyGeometry(const imaging::ImageBuf& src, const GeometryParams& p,
                   imaging::Kernel kernel, imaging::ImageBuf* out,
                   uint64_t* sampleCount, std::string* err);

}  // namespace plan
}  // namespace lumina

#endif  // LUMINA_PLAN_GEOMETRY_H_
