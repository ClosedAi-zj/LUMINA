#include "color/icc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "lcms2.h"

namespace lumina {
namespace color {
namespace {

std::once_flag g_initOnce;
bool g_lcmsOk = false;

constexpr const char* kSrgbName = "sRGB IEC61966-2.1";
constexpr const char* kAdobeName = "AdobeRGB (1998)";
constexpr const char* kPrintSimName = "LUMINA Print-Sim (synthetic press gamut)";

/**
 * 内置 profile 构造（惰性 + 缓存）。
 * 说明：不用 cmsCreate_sRGBProfile，因为它的描述串是 "sRGB built-in"，
 * Photoshop 会显示成非标准名；这里按标准基色 + IEC 61966-2-1 曲线自行构造，
 * 描述串写标准名，导出的文件在 PS 里就是常规 sRGB / AdobeRGB。
 */
cmsHPROFILE buildProfile(ProfileId id) {
  cmsCIExyY white;
  cmsCIExyYTRIPLE prim;
  cmsToneCurve* curves[3] = {nullptr, nullptr, nullptr};

  // 白点：sRGB / AdobeRGB 为 D65；打印模拟用纸白 D50（这正是"纸白模拟"要体现的差异）
  if (id == ProfileId::PrintSim) {
    white.x = 0.3457;
    white.y = 0.3585;
    white.Y = 1.0;
  } else {
    white.x = 0.3127;
    white.y = 0.3290;
    white.Y = 1.0;
  }

  if (id == ProfileId::Srgb) {
    prim.Red.x = 0.64;   prim.Red.y = 0.33;   prim.Red.Y = 1.0;
    prim.Green.x = 0.30; prim.Green.y = 0.60; prim.Green.Y = 1.0;
    prim.Blue.x = 0.15;  prim.Blue.y = 0.06;  prim.Blue.Y = 1.0;
    // IEC 61966-2-1 参数式曲线（type 4）：g, a, b, c, d
    const cmsFloat64Number params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
    cmsToneCurve* t = cmsBuildParametricToneCurve(nullptr, 4, params);
    if (!t) return nullptr;
    curves[0] = curves[1] = curves[2] = t;
  } else if (id == ProfileId::AdobeRgb) {
    prim.Red.x = 0.64;   prim.Red.y = 0.33;   prim.Red.Y = 1.0;
    prim.Green.x = 0.21; prim.Green.y = 0.71; prim.Green.Y = 1.0;
    prim.Blue.x = 0.15;  prim.Blue.y = 0.06;  prim.Blue.Y = 1.0;
    cmsToneCurve* t = cmsBuildGamma(nullptr, 563.0 / 256.0);  // ≈2.19921875
    if (!t) return nullptr;
    curves[0] = curves[1] = curves[2] = t;
  } else {
    // PrintSim：按典型胶印色域收窄基色 + 纸白 D50 + gamma 2.2。
    // 目的不是"正确还原某台印刷机"，而是让软打样在无真实 ICC 时也能真实裁切超色域色。
    prim.Red.x = 0.58;   prim.Red.y = 0.33;   prim.Red.Y = 1.0;
    prim.Green.x = 0.28; prim.Green.y = 0.58; prim.Green.Y = 1.0;
    prim.Blue.x = 0.16;  prim.Blue.y = 0.09;  prim.Blue.Y = 1.0;
    cmsToneCurve* t = cmsBuildGamma(nullptr, 2.2);
    if (!t) return nullptr;
    curves[0] = curves[1] = curves[2] = t;
  }

  cmsHPROFILE h = cmsCreateRGBProfile(&white, &prim, curves);
  cmsFreeToneCurve(curves[0]);  // 三个指针同一对象，只释放一次
  if (!h) return nullptr;

  cmsSetProfileVersion(h, kProfileVersion);
  cmsSetDeviceClass(h, cmsSigDisplayClass);

  const char* name = (id == ProfileId::Srgb)    ? kSrgbName
                     : (id == ProfileId::AdobeRgb) ? kAdobeName
                                                   : kPrintSimName;
  cmsMLU* mlu = cmsMLUalloc(nullptr, 1);
  if (mlu) {
    cmsMLUsetASCII(mlu, "en", "US", name);
    cmsWriteTag(h, cmsSigProfileDescriptionTag, mlu);
    cmsMLUfree(mlu);
  }
  cmsMLU* cp = cmsMLUalloc(nullptr, 1);
  if (cp) {
    cmsMLUsetASCII(cp, "en", "US", "No copyright, use freely");
    cmsWriteTag(h, cmsSigCopyrightTag, cp);
    cmsMLUfree(cp);
  }
  return h;
}

/** 缓存 profile 句柄（进程内共享，只读；lcms2 profile 句柄多线程只读安全） */
cmsHPROFILE cachedProfile(ProfileId id) {
  static cmsHPROFILE slots[3] = {nullptr, nullptr, nullptr};
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  const size_t idx = static_cast<size_t>(id);
  if (idx > 2) return nullptr;
  if (!slots[idx]) slots[idx] = buildProfile(id);
  return slots[idx];
}

}  // namespace

bool available() {
  std::call_once(g_initOnce, []() {
    g_lcmsOk = (cachedProfile(ProfileId::Srgb) != nullptr) &&
               (cachedProfile(ProfileId::AdobeRgb) != nullptr) &&
               (cachedProfile(ProfileId::PrintSim) != nullptr);
  });
  return g_lcmsOk;
}

const char* profileName(ProfileId id) {
  switch (id) {
    case ProfileId::Srgb: return kSrgbName;
    case ProfileId::AdobeRgb: return kAdobeName;
    default: return kPrintSimName;
  }
}

bool profileBytes(ProfileId id, std::vector<uint8_t>* out, std::string* err) {
  if (!out) {
    if (err) *err = "profileBytes: out 为空";
    return false;
  }
  if (!available()) {
    if (err) *err = "lcms2 不可用（profile 构造失败）";
    return false;
  }
  cmsHPROFILE h = cachedProfile(id);
  cmsUInt32Number len = 0;
  if (!cmsSaveProfileToMem(h, nullptr, &len) || len == 0) {
    if (err) *err = std::string("ICC 序列化失败: ") + profileName(id);
    return false;
  }
  out->resize(len);
  cmsUInt32Number written = len;
  if (!cmsSaveProfileToMem(h, out->data(), &written)) {
    if (err) *err = std::string("ICC 写入失败: ") + profileName(id);
    return false;
  }
  out->resize(written);

  /**
   * ICC 头 [24:36] 是 profile 的 dateTimeNumber（创建时间，6 个 uint16 大端：
   * 年/月/日/时/分/秒）。lcms2 序列化时填的是**本进程首次构造 profile 的时刻**，
   * 后果：同一张照片今天导出和明天导出，嵌进 JPG/TIFF/PSD/PNG 的 profile 字节不同
   *       → 导出物无法位级复现，任何 golden-hash 回归都会被这个时间戳打爆，
   *         用户也无法验证"同样的输入吐出同样的文件"。
   *
   * 处理方式：序列化之后原地规范化为固定时间 2000-01-01 00:00:00。
   *  - 为什么不直接设 header：lcms2 2.16 **没有** cmsSetHeaderCreated（只有
   *    cmsGetHeaderCreationDateTime），无法在保存前指定，只能保存后规范化。
   *  - 为什么用固定值而不是"当前时间"：确定性优先。dateTimeNumber 只是元信息，
   *    不参与色彩计算（不进入 PCS、不参与 transform、不参与 gamut/曲线求值），
   *    任何 CMM 都不会用它做运算，因此固定值与真实创建时间在色彩上等价。
   */
  constexpr size_t kDateTimeOffset = 24;
  constexpr size_t kDateTimeBytes = 12;  // 6 × uint16
  if (written < kDateTimeOffset + kDateTimeBytes) {
    if (err) {
      *err = std::string("ICC 头过短，无法规范化创建时间（") + std::to_string(written) +
             " 字节）: " + profileName(id);
    }
    return false;
  }
  static const uint16_t kFixedDateTime[6] = {2000, 1, 1, 0, 0, 0};
  for (size_t i = 0; i < 6; ++i) {
    const uint16_t v = kFixedDateTime[i];
    (*out)[kDateTimeOffset + i * 2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    (*out)[kDateTimeOffset + i * 2 + 1] = static_cast<uint8_t>(v & 0xFF);
  }
  // profileID [84:100]：实测 lcms2 2.16 写的是全 0（未计算 MD5），跨进程确定，
  // 故不需要干预；若将来 lcms 改成写入随机/MD5 值，这里必须一并规范化。

  /**
   * 自证没把 profile 改坏：规范化后立刻用 lcms2 重开并核对色彩空间。
   * 绝不允许"改完就返回 true"——一个坏 profile 会静默污染所有导出物。
   */
  cmsHPROFILE verify =
      cmsOpenProfileFromMem(out->data(), static_cast<cmsUInt32Number>(out->size()));
  if (!verify) {
    if (err) *err = std::string("ICC 规范化后重开失败（profile 已损坏）: ") + profileName(id);
    return false;
  }
  const cmsColorSpaceSignature colorSignature = cmsGetColorSpace(verify);
  cmsCloseProfile(verify);
  if (colorSignature != cmsSigRgbData) {
    if (err) {
      *err = std::string("ICC 规范化后色彩空间不是 RGB: ") + profileName(id);
    }
    return false;
  }
  return true;
}

namespace {

bool convertImpl(const uint8_t* src, int width, int height, ProfileId from, ProfileId to,
                 bool via16, std::vector<uint8_t>* out8, std::vector<uint16_t>* out16,
                 std::string* err) {
  if (!src || width <= 0 || height <= 0) {
    if (err) *err = "convert: 尺寸非法";
    return false;
  }
  if (!available()) {
    if (err) *err = "lcms2 不可用";
    return false;
  }
  const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
  cmsHPROFILE hs = cachedProfile(from);
  cmsHPROFILE hd = cachedProfile(to);

  // 目标位深：16bit 直出 or 经 16bit 中间再落 8bit
  const cmsUInt32Number midFmt = TYPE_RGB_16;
  cmsHTRANSFORM t1 = cmsCreateTransform(hs, TYPE_RGB_8, hd, midFmt,
                                        INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE);
  if (!t1) {
    if (err) *err = "lcms2 创建转换失败（RGB8→RGB16）";
    return false;
  }
  std::vector<uint16_t> mid(n * 3);
  cmsDoTransform(t1, src, mid.data(), static_cast<cmsUInt32Number>(n));
  cmsDeleteTransform(t1);

  if (!via16) {
    // 8bit 输出：从 16bit 中间回归 8bit（同一空间内 16→8 只做舍入，不引入色彩偏移）
    for (size_t i = 0; i < mid.size(); ++i) {
      const uint32_t v = (static_cast<uint32_t>(mid[i]) + 128u) / 257u;
      (*out8)[i] = static_cast<uint8_t>(std::min<uint32_t>(255u, v));
    }
    return true;
  }
  // 16bit 输出：transform 已直接落在目标空间的 16bit 网格上
  if (out16) *out16 = std::move(mid);
  return true;
}

/** 16bit 源 → 8bit 目标（往返测量的回程；只做一次最终量化） */
bool convert16To8(const uint16_t* src, int width, int height, ProfileId from, ProfileId to,
                  std::vector<uint8_t>* out, std::string* err) {
  if (!src || !out || width <= 0 || height <= 0) {
    if (err) *err = "convert16To8: 入参非法";
    return false;
  }
  cmsHTRANSFORM t = cmsCreateTransform(cachedProfile(from), TYPE_RGB_16, cachedProfile(to),
                                       TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC,
                                       cmsFLAGS_NOOPTIMIZE);
  if (!t) {
    if (err) *err = "lcms2 创建转换失败（RGB16→RGB8）";
    return false;
  }
  const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
  out->assign(n * 3, 0);
  cmsDoTransform(t, src, out->data(), static_cast<cmsUInt32Number>(n));
  cmsDeleteTransform(t);
  return true;
}

}  // namespace

bool convertRgb8(const uint8_t* src, int width, int height, ProfileId from, ProfileId to,
                 std::vector<uint8_t>* out, std::string* err) {
  if (!out) {
    if (err) *err = "convertRgb8: out 为空";
    return false;
  }
  out->assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3, 0);
  return convertImpl(src, width, height, from, to, false, out, nullptr, err);
}

bool convertRgb8Direct(const uint8_t* src, int width, int height, ProfileId from, ProfileId to,
                       std::vector<uint8_t>* out, std::string* err) {
  if (!out) {
    if (err) *err = "convertRgb8Direct: out 为空";
    return false;
  }
  if (!src || width <= 0 || height <= 0) {
    if (err) *err = "convertRgb8Direct: 尺寸非法";
    return false;
  }
  if (!available()) {
    if (err) *err = "lcms2 不可用";
    return false;
  }
  const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
  cmsHTRANSFORM t = cmsCreateTransform(cachedProfile(from), TYPE_RGB_8, cachedProfile(to),
                                       TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC,
                                       cmsFLAGS_NOOPTIMIZE);
  if (!t) {
    if (err) *err = "lcms2 创建转换失败（RGB8→RGB8）";
    return false;
  }
  out->assign(n * 3, 0);
  cmsDoTransform(t, src, out->data(), static_cast<cmsUInt32Number>(n));
  cmsDeleteTransform(t);
  return true;
}

bool convertRgb8To16(const uint8_t* src, int width, int height, ProfileId from, ProfileId to,
                     std::vector<uint16_t>* out, std::string* err) {
  if (!out) {
    if (err) *err = "convertRgb8To16: out 为空";
    return false;
  }
  return convertImpl(src, width, height, from, to, true, nullptr, out, err);
}

bool measureRoundtrip(const uint8_t* src, int width, int height, ProfileId from, ProfileId to,
                      RoundtripStats* stats, std::string* err) {
  if (!stats) {
    if (err) *err = "measureRoundtrip: stats 为空";
    return false;
  }
  std::vector<uint16_t> mid;
  std::vector<uint8_t> back;
  if (!convertImpl(src, width, height, from, to, true, nullptr, &mid, err)) return false;
  if (!convert16To8(mid.data(), width, height, to, from, &back, err)) return false;

  const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
  long long sum = 0, nonZero = 0;
  int maxAbs = 0;
  for (size_t i = 0; i < n; ++i) {
    const int d = std::abs(static_cast<int>(src[i]) - static_cast<int>(back[i]));
    sum += d;
    if (d != 0) ++nonZero;
    if (d > maxAbs) maxAbs = d;
  }
  stats->samples = static_cast<long long>(n);
  stats->meanAbs = n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
  stats->maxAbs = maxAbs;
  stats->nonZero = nonZero;
  return true;
}

bool softProofRgb8(const uint8_t* src, int width, int height, const SoftProofOptions& opts,
                   std::vector<uint8_t>* out, std::string* err) {
  if (!out || !src || width <= 0 || height <= 0) {
    if (err) *err = "softProofRgb8: 入参非法";
    return false;
  }
  if (!available()) {
    if (err) *err = "lcms2 不可用";
    return false;
  }
  cmsHPROFILE hp = nullptr;
  bool ownProof = false;
  if (!opts.targetProfilePath.empty()) {
    hp = cmsOpenProfileFromFile(opts.targetProfilePath.c_str(), "r");
    if (!hp) {
      if (err) *err = "无法打开打样目标 ICC: " + opts.targetProfilePath;
      return false;
    }
    ownProof = true;
  } else {
    hp = cachedProfile(opts.builtinTarget);
  }

  // lcms2 的纸白模拟由**打样意图**决定（绝对色度），没有独立的 paper flag；
  // 打样本身必须带 cmsFLAGS_SOFTPROOFING，否则会退化成普通转换（画面不变，等于没打样）。
  const cmsUInt32Number flags = cmsFLAGS_NOOPTIMIZE | cmsFLAGS_SOFTPROOFING;
  const int proofingIntent =
      opts.simulatePaper ? 3 /* INTENT_ABSOLUTE_COLORIMETRIC */ : opts.proofingIntent;
  cmsHTRANSFORM t = cmsCreateProofingTransform(cachedProfile(opts.source), TYPE_RGB_8,
                                              cachedProfile(ProfileId::Srgb), TYPE_RGB_8, hp,
                                              opts.intent, proofingIntent, flags);
  if (!t) {
    if (ownProof) cmsCloseProfile(hp);
    if (err) *err = "lcms2 创建软打样转换失败";
    return false;
  }
  const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
  out->assign(n * 3, 0);
  cmsDoTransform(t, src, out->data(), static_cast<cmsUInt32Number>(n));
  cmsDeleteTransform(t);
  if (ownProof) cmsCloseProfile(hp);
  return true;
}

double deltaE00(const double lab1[3], const double lab2[3]) {
  cmsCIELab a{lab1[0], lab1[1], lab1[2]};
  cmsCIELab b{lab2[0], lab2[1], lab2[2]};
  return static_cast<double>(cmsCIE2000DeltaE(&a, &b, 1.0, 1.0, 1.0));
}

bool srgb8ToLab(const uint8_t* src, int width, int height, std::vector<double>* lab,
                std::string* err) {
  if (!lab || !src || width <= 0 || height <= 0) {
    if (err) *err = "srgb8ToLab: 入参非法";
    return false;
  }
  if (!available()) {
    if (err) *err = "lcms2 不可用";
    return false;
  }
  cmsHPROFILE hlab = cmsCreateLab4Profile(nullptr);
  if (!hlab) {
    if (err) *err = "无法创建 Lab profile";
    return false;
  }
  cmsHTRANSFORM t = cmsCreateTransform(cachedProfile(ProfileId::Srgb), TYPE_RGB_8, hlab,
                                       TYPE_Lab_DBL, INTENT_ABSOLUTE_COLORIMETRIC,
                                       cmsFLAGS_NOOPTIMIZE);
  if (!t) {
    cmsCloseProfile(hlab);
    if (err) *err = "创建 sRGB→Lab 转换失败";
    return false;
  }
  const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
  lab->assign(n * 3, 0.0);
  cmsDoTransform(t, src, lab->data(), static_cast<cmsUInt32Number>(n));
  cmsDeleteTransform(t);
  cmsCloseProfile(hlab);
  return true;
}

}  // namespace color
}  // namespace lumina
