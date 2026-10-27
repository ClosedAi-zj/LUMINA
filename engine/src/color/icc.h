// M4-01 色彩管理（Little CMS 2.16 接入）
//
// 设计要点：
//  - 内置两个工作色彩空间 profile（sRGB IEC61966-2.1 / AdobeRGB(1998)），均由
//    lcms2 以标准基色 + 色调曲线构造，**不再使用矩阵近似**；
//  - 转换走 lcms2 transform（相对色度意图），中间态用 16bit 承载，避免 8bit 量化
//    把往返误差放大到 2 LSB 以上；
//  - profile 可序列化为 ICC 字节，供导出写入（M4-02：JPEG APP2 / TIFF tag 34675 /
//    PSD image resource 0x040F）。
#ifndef LUMINA_COLOR_ICC_H_
#define LUMINA_COLOR_ICC_H_

#include <cstdint>
#include <string>
#include <vector>

namespace lumina {
namespace color {

enum class ProfileId {
  Srgb = 0,
  AdobeRgb = 1,
  /**
   * 合成印刷色域（Print-Sim）：基色按典型胶印色域收窄、白点为纸白（D50）。
   * 用途：软打样**演示/自检**目标 —— sRGB 源经它往返会真实发生色域裁切，
   * 因此 ΔE 非零、画面可见变化。**不是**任何真实纸墨的 ICC（PRD Q9 不分发商业 profile）。
   */
  PrintSim = 2,
};

// ICC profile 版本（导出写入的 profile 版本；4.3 与 PS/AI 主流一致）
constexpr double kProfileVersion = 4.3;

/** 内置 profile 的人类可读名（写进 ICC 描述与报告） */
const char* profileName(ProfileId id);

/** 内置 profile 的 ICC 字节（每次返回新副本；失败返回 false + 原文错误） */
bool profileBytes(ProfileId id, std::vector<uint8_t>* out, std::string* err);

/**
 * 8bit RGB 图像色彩空间转换。
 * 中间态为 16bit，故 sRGB→AdobeRGB 的往返误差只来自最终 8bit 落盘。
 */
bool convertRgb8(const uint8_t* src, int width, int height, ProfileId from, ProfileId to,
                 std::vector<uint8_t>* out, std::string* err);

/** 8bit RGB → 16bit RGB（PSD 16bit 导出用：先转目标空间再拓宽位深） */
bool convertRgb8To16(const uint8_t* src, int width, int height, ProfileId from, ProfileId to,
                     std::vector<uint16_t>* out, std::string* err);

/**
 * 往返统计（供 test_icc 出 m4-icc-roundtrip.json）：
 *  src8 --(16bit 中间)--> mid8 --(16bit 中间)--> back8
 *  meanAbs / maxAbs 以 0..255 计。
 */
struct RoundtripStats {
  double meanAbs = 0.0;
  int maxAbs = 0;
  long long samples = 0;
  long long nonZero = 0;
};

bool measureRoundtrip(const uint8_t* src, int width, int height, ProfileId from, ProfileId to,
                      RoundtripStats* stats, std::string* err);

/**
 * 8bit → 8bit 单跳转换（导出落盘用；逐通道舍入，不做抖动）。
 * 与 convertRgb8 的差别：明确不需要 16bit 中间态时可直接 8→8（lcms2 内部仍高精度）。
 */
bool convertRgb8Direct(const uint8_t* src, int width, int height, ProfileId from, ProfileId to,
                       std::vector<uint8_t>* out, std::string* err);

/** lcms2 是否可用（编译期接入成功；失败时导出走无 ICC 兜底并记 warning） */
bool available();

// ---- 软打样（M4-03）----

/**
 * 软打样预览：src（sRGB）→ 目标（打样设备 profile）→ sRGB 显示。
 * 使用 cmsCreateProofingTransform（SOFTPROOFING），意图 relative colorimetric，
 * 黑点补偿关闭（与 PS「模拟纸张/油墨」默认一致）。
 */
struct SoftProofOptions {
  ProfileId source = ProfileId::Srgb;
  /** 未提供 targetProfilePath 时使用的内置目标（演示/自检用；PRD Q9 不分发第三方商业 profile） */
  ProfileId builtinTarget = ProfileId::PrintSim;
  std::string targetProfilePath;  // 用户加载的打印 ICC（空 = 用 builtinTarget）
  int intent = 1;                 // 0 感知 / 1 相对色度
  int proofingIntent = 1;
  bool simulatePaper = true;      // 纸白模拟（等价于打样意图取绝对色度）
};

bool softProofRgb8(const uint8_t* src, int width, int height, const SoftProofOptions& opts,
                   std::vector<uint8_t>* out, std::string* err);

/** 单像素 ΔE00（0..100），供软打样/导出校色报告使用 */
double deltaE00(const double lab1[3], const double lab2[3]);

/** 把 sRGB 8bit 像素转 Lab（D50, ICC 绝对意图口径），失败返回 false */
bool srgb8ToLab(const uint8_t* src, int width, int height, std::vector<double>* lab,
                std::string* err);

}  // namespace color
}  // namespace lumina

#endif  // LUMINA_COLOR_ICC_H_
