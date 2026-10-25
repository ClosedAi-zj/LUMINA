// T0.4：16bit 重采样核（架构 §4 S1 几何 / M0 T0.4）
// - 采样必须在线性域进行：log16 → float 解码 → 核采样 → 重新 log16 编码
//   （log16 非线性，直接在码值域做卷积会把暗部权重放大，结果错误）
// - 核：Box（缩略图快路径）/ CatmullRom（默认）/ Lanczos3（导出高质量）
// - 可分离两趟（水平 → 垂直），与几何单应配合时总采样次数 = 1（由几何层保证）
#ifndef LUMINA_IMAGING_RESAMPLE_H_
#define LUMINA_IMAGING_RESAMPLE_H_

#include <string>

#include "imaging/image_buffer.h"

namespace lumina {
namespace imaging {

enum class Kernel {
  Box = 0,        // 等权重 1px 半径：面积平均，缩略图快路径
  CatmullRom = 1, // 2px 半径三次 B 样条插值：默认
  Lanczos3 = 2,   // 3px 半径 sinc 窗：锐利，导出路径
};

// 核半径（support）：Box=1, CatmullRom=2, Lanczos3=3
int kernelRadius(Kernel k);

// 归一化核权重：|t| 超出半径返回 0；Box 对 |t|<=0.5 返回 1
double kernelWeight(Kernel k, double t);

// 整图重采样 src(w,h) → out(dstW,dstH)。
// 线性域两趟可分离重采样；空间/元数据（除 w/h 外）原样继承。
// 失败（非法尺寸/参数）返回 false 并写 err。
bool resampleImage16(const ImageBuf& src, int dstW, int dstH, Kernel k,
                     ImageBuf* out, std::string* err);

}  // namespace imaging
}  // namespace lumina

#endif  // LUMINA_IMAGING_RESAMPLE_H_
