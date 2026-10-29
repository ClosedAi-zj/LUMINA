// 风光专项 · 共享合成辅助：灰度金字塔对齐 + 帧采样 + TIFF/DNG 落地 + 景深堆栈融合
// HDR 包围合成与景深堆栈（focus stacking）共用同一套：平移对齐、帧加权采样、
// 线性 DNG / RGB8 TIFF 写出。集中在此，避免两份实现漂移。
#ifndef LUMINA_SPECIALTY_COMPOSITING_COMMON_H_
#define LUMINA_SPECIALTY_COMPOSITING_COMMON_H_

#include <cstdint>
#include <vector>

#include "imaging/imaging.h"

namespace lumina {

class JobContext;  // 前向声明：focusStackFrames 仅持指针，避免在本头引入 job_manager

namespace compositing {

// ============================ 灰度金字塔（autoAlign）============================

struct GrayImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;
};

GrayImage toGray(const imaging::Image& image);
GrayImage halfScale(const GrayImage& src);

// 平均绝对差：ref(x,y) 与 tgt(x+dx,y+dy) 的差异。重叠不足 1/3 视为无效匹配。
double shiftScore(const GrayImage& ref, const GrayImage& tgt, int dx, int dy);

// 曝光归一化：包围曝光各帧整体亮度差好几档，直接比灰度会把「曝光差」误当成「位移」。
// 所以先做零均值/单位方差拉伸，只保留结构信息（这正是平移搜索需要的）。
GrayImage normalizeContrast(const GrayImage& src);

// 由粗到细的平移搜索：1/8 上 ±maxShift/8，逐级细化到全分辨率 ±2，最终钳制到 ±maxShift。
// 若「不位移」的成绩不比最优差 2% 以上，就判定为无需对齐（宁可不动，也不要把对齐误差
// 当成抖动打进融合结果）。
void estimateShift(const GrayImage& ref, const GrayImage& target, int maxShift, int* outDx,
                   int* outDy);

constexpr int kOutputMaxShift = 32;  // autoAlign 平移搜索上限（像素）

// ============================ 帧采样（融合公共）============================

// 一帧在当前合成里的视图：指向原始位图 + 由 autoAlign 求得的平移偏移。
struct FrameView {
  const imaging::Image* image = nullptr;
  int offsetX = 0;
  int offsetY = 0;
};

double sampleLum(const FrameView& view, int x, int y);
void sampleRgb(const FrameView& view, int x, int y, double* r, double* g, double* b);

// ============================ TIFF / DNG 写出 ============================
// baseline RGB8 TIFF（未压缩、小端、单条 strip）：与 export 的 TIFF 布局一致，
// 保证同一引擎导出的 tif 与 HDR / 景深堆栈合成的 tif 用同一套读取假设。
bool writeRgb8Tiff(const std::string& path, const imaging::Image& image, std::string* err);

// 线性 RGB DNG：TIFF-EP 容器 + DNGVersion 1.4 + LinearRaw(34892)，内嵌 JPEG 预览 SubIFD，
// 使图库缩略图 / raw.getProxy 能直接出图（CFA 已在上游融合为 RGB，这里不再伪造马赛克，
// 所以是「线性 RGB DNG」而非 CFA DNG）。dateTime 用 "YYYY:MM:DD HH:MM:SS"。
bool writeLinearRgbDng(const std::string& path, const imaging::Image& image,
                       const std::string& dateTime, std::string* err);

// ============================ 景深堆栈融合 ============================
// 软焦点加权混合：每像素按局部拉普拉斯幅度（对焦清晰度高者权重高）加权各帧，
// 平滑过渡得到全清晰图。smoothness 为焦点权重指数（越高越「硬」选择、过渡越锐）。
// ctx 可空（纯函数测试用）；按行感知取消并上报进度（45→85%）。
void focusStackFrames(const std::vector<FrameView>& frames, double smoothness, JobContext* ctx,
                      imaging::Image* out, bool* cancelled);

}  // namespace compositing
}  // namespace lumina

#endif  // LUMINA_SPECIALTY_COMPOSITING_COMMON_H_
