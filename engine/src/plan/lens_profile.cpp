// M2 T2.11 镜头 profile 库实现（N6，架构 §3.4 / §5 类图 LensProfileDb）
// 设计注释见 lens_profile.h。JSON 解析复用 engine 自带 json::JsonValue（无新增依赖）。
#include "plan/lens_profile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/str_util.h"
#include "json/json_value.h"

#ifdef _WIN32
#include <windows.h>
#include <dirent.h>  // MinGW GCC 8.1 的 <filesystem> 与 windows.h 同 TU 冲突，用 dirent
#else
#include <dirent.h>
#endif

#ifndef LUMINA_LENS_PROFILES_SRC_DIR
#define LUMINA_LENS_PROFILES_SRC_DIR "data/lens-profiles"
#endif

namespace lumina {
namespace plan {

namespace {

// 目录下全部 *.json 文件路径（dirent 扁平扫描；MinGW GCC 8 兼容，不用 <filesystem>）
std::vector<std::string> listJsonFiles(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return out;
  while (const dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
      out.push_back(dir + "/" + name);
    }
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

bool dirExists(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return false;
  closedir(d);
  return true;
}

// 匹配归一化：去空白 + 小写（架构 §3.4「去空白/大小写/常见别名」的 M2 最小口径）
std::string normKey(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

LensFocalBucket parseBucket(const json::JsonValue& j) {
  LensFocalBucket b;
  b.focalMm = j.get("focalMm").asDouble(0.0);
  const json::JsonValue& d = j.get("distortion");
  b.k1 = d.get("k1").asDouble(0.0);
  b.k2 = d.get("k2").asDouble(0.0);
  b.k3 = d.get("k3").asDouble(0.0);
  const json::JsonValue& ca = j.get("ca");
  b.scaleR = ca.get("scaleR").asDouble(1.0);
  b.scaleB = ca.get("scaleB").asDouble(1.0);
  const json::JsonValue& v = j.get("vignetting");
  b.vignA = v.get("a").asDouble(0.0);
  b.vignB = v.get("b").asDouble(0.0);
  b.vignC = v.get("c").asDouble(0.0);
  return b;
}

bool parseProfile(const std::string& text, LensProfile* out, std::string* err) {
  json::JsonValue root;
  if (!json::JsonValue::parse(text, &root, err)) return false;
  if (!root.isObject()) {
    if (err) *err = "root is not an object";
    return false;
  }
  out->source = root.get("source").asString("self-built");
  out->attribution = root.get("attribution").asString();
  if (root.get("source").asString() == "lensfun-db(CC-BY-SA-3.0)" &&
      out->attribution.empty()) {
    if (err) *err = "source=lensfun-db requires attribution（NOTICE.md 同步）";
    return false;
  }
  out->cameraMake = root.get("camera").get("make").asString();
  out->cameraModel = root.get("camera").get("model").asString();
  out->lensMake = root.get("lens").get("make").asString();
  out->lensModel = root.get("lens").get("model").asString();
  for (const json::JsonValue& b : root.get("focalBuckets").arrayItems()) {
    out->buckets.push_back(parseBucket(b));
  }
  std::sort(out->buckets.begin(), out->buckets.end(),
            [](const LensFocalBucket& a, const LensFocalBucket& c) {
              return a.focalMm < c.focalMm;
            });
  return out->valid();
}

}  // namespace

LensProfileDb& LensProfileDb::global() {
  static LensProfileDb db;
  static std::once_flag once;
  std::call_once(once, [] {
    // 惰性加载顺序：环境变量 → exe 旁 data/ → 源码树 data/（CMake 注入，构建/测试兜底）
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("LUMINA_LENS_PROFILES_DIR")) dirs.push_back(env);
#ifdef _WIN32
    char exePath[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
      std::string exeDir(exePath);
      const size_t slash = exeDir.find_last_of("\\/");
      if (slash != std::string::npos) exeDir.resize(slash);
      dirs.push_back(exeDir + "\\data\\lens-profiles");
    }
#endif
    dirs.push_back(LUMINA_LENS_PROFILES_SRC_DIR);
    std::string err;
    for (const std::string& d : dirs) {
      std::string e;
      if (db.loadDir(d, &e)) break;
      err += e;
    }
    if (!err.empty()) lensProfileSetWarning("lens profile db load: " + err);
  });
  return db;
}

bool LensProfileDb::loadDir(const std::string& dir, std::string* err) {
  if (!dirExists(dir)) {
    if (err) *err = "dir not found: " + dir + "; ";
    return false;
  }
  const std::vector<std::string> files = listJsonFiles(dir);
  if (files.empty()) {
    if (err) *err = "no *.json under: " + dir + "; ";
    return false;
  }
  std::vector<LensProfile> loaded;
  std::string errs;
  for (const std::string& path : files) {
    std::string text;
    std::string e;
    if (!lumina::readFile(path, &text, &e)) {
      errs += e + "; ";
      continue;
    }
    LensProfile p;
    if (!parseProfile(text, &p, &e)) {
      errs += path + ": " + e + "; ";
      continue;
    }
    loaded.push_back(std::move(p));
  }
  std::lock_guard<std::mutex> lock(mu_);
  profiles_ = std::move(loaded);
  if (!errs.empty()) {
    if (err) *err = errs;
    // 部分文件损坏不算整体失败（其余 profile 可用；错误已拼接进 err 可观测）
  }
  return !profiles_.empty();
}

size_t LensProfileDb::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return profiles_.size();
}

bool LensProfileDb::match(const std::string& cameraMake, const std::string& cameraModel,
                          const std::string& lensMake, const std::string& lensModel,
                          double focalMm, LensProfile* out) const {
  std::lock_guard<std::mutex> lock(mu_);
  const std::string cm = normKey(cameraModel);
  const std::string lm = normKey(lensModel);
  for (const LensProfile& p : profiles_) {
    // camera.make+model 精确 → lens.make+model 归一化（make 可空，model 必须相等）
    if (!normKey(p.cameraModel).empty() && normKey(p.cameraModel) != cm) continue;
    if (!cameraMake.empty() && !p.cameraMake.empty() &&
        normKey(p.cameraMake) != normKey(cameraMake)) continue;
    if (normKey(p.lensModel) != lm) continue;
    if (!lensMake.empty() && !p.lensMake.empty() &&
        normKey(p.lensMake) != normKey(lensMake)) continue;
    if (out != nullptr) *out = p;  // 含 buckets，调用方再插值
    return true;
  }
  return false;
}

LensFocalBucket LensProfile::bucketForFocal(double focalMm) const {
  if (buckets.empty()) return LensFocalBucket{};
  if (focalMm <= 0.0 || focalMm <= buckets.front().focalMm) return buckets.front();
  if (focalMm >= buckets.back().focalMm) return buckets.back();
  for (size_t i = 1; i < buckets.size(); ++i) {
    const LensFocalBucket& lo = buckets[i - 1];
    const LensFocalBucket& hi = buckets[i];
    if (focalMm <= hi.focalMm) {
      const double t = (focalMm - lo.focalMm) / std::max(1e-9, hi.focalMm - lo.focalMm);
      LensFocalBucket b;
      b.focalMm = focalMm;
      b.k1 = lo.k1 + (hi.k1 - lo.k1) * t;
      b.k2 = lo.k2 + (hi.k2 - lo.k2) * t;
      b.k3 = lo.k3 + (hi.k3 - lo.k3) * t;
      b.scaleR = lo.scaleR + (hi.scaleR - lo.scaleR) * t;
      b.scaleB = lo.scaleB + (hi.scaleB - lo.scaleB) * t;
      b.vignA = lo.vignA + (hi.vignA - lo.vignA) * t;
      b.vignB = lo.vignB + (hi.vignB - lo.vignB) * t;
      b.vignC = lo.vignC + (hi.vignC - lo.vignC) * t;
      return b;
    }
  }
  return buckets.back();
}

double lensDistortRadiusFactor(double r, double k1, double k2, double k3, double amount) {
  const double d = 1.0 + (k1 + (k2 + k3 * r * r) * r * r) * r * r;
  const double s = std::max(-100.0, std::min(100.0, amount)) / 100.0;
  // factor = d^s：s=1 全量校正（src 半径乘 d），s=−1 反向放大，s=0 恒等
  return std::pow(std::max(0.05, d), s);
}

double lensVignetteGain(double rNorm, double a, double b, double c, double amount) {
  const double rn = std::max(0.0, rNorm);
  const double v = 1.0 + (a + (b + c * rn * rn) * rn * rn) * rn * rn;
  const double s = std::max(-100.0, std::min(100.0, amount)) / 100.0;
  return std::pow(std::max(0.05, std::min(20.0, v)), s);
}

double lensCaScale(double profileScale, double leafAmount) {
  const double leaf = std::max(-100.0, std::min(100.0, leafAmount)) / 100.0;
  return 1.0 + (profileScale - 1.0) * (1.0 + leaf);
}

bool estimateLateralCa(const imaging::ImageBuf& buf, double* scaleR, double* scaleB) {
  if (!buf.valid() || buf.w < 8 || buf.h < 8) return false;
  const double ccx = (buf.w - 1) / 2.0;
  const double ccy = (buf.h - 1) / 2.0;
  // 边缘带（外圈 3% 框）：横向 CA 随半径增大，边缘信号最显著
  const int band = std::max(2, static_cast<int>(std::min(buf.w, buf.h) * 0.03));
  std::vector<std::pair<double, double>> pts;  // 边缘带像素坐标
  for (int y = 0; y < buf.h; y += 2) {
    for (int x = 0; x < buf.w; x += 2) {
      if (x < band || y < band || x >= buf.w - band || y >= buf.h - band) {
        pts.emplace_back(x, y);
      }
    }
  }
  if (pts.empty()) return false;
  auto channelAt = [&buf](int x, int y, int c) -> double {
    return imaging::codeAt(buf, (static_cast<size_t>(y) * buf.w + x) * 3 + c);
  };
  // 候选缩放 25 档：±0.3% 限幅（架构 §3.4）；平坦图 SAD 无差别 → 取 1.0 不猜测
  auto estimate = [&](int ch) -> double {
    double bestM = 1.0;
    double bestSad = -1.0;
    for (int i = -12; i <= 12; ++i) {
      const double m = 1.0 + i * (0.003 / 12.0);
      double sad = 0.0;
      for (const auto& pt : pts) {
        const double dx = pt.first - ccx;
        const double dy = pt.second - ccy;
        const double sx = ccx + dx * m;
        const double sy = ccy + dy * m;
        const int x0 = static_cast<int>(std::floor(sx));
        const int y0 = static_cast<int>(std::floor(sy));
        if (x0 < 0 || y0 < 0 || x0 >= buf.w - 1 || y0 >= buf.h - 1) continue;
        const double fx = sx - x0;
        const double fy = sy - y0;
        const double p00 = channelAt(x0, y0, ch);
        const double p10 = channelAt(x0 + 1, y0, ch);
        const double p01 = channelAt(x0, y0 + 1, ch);
        const double p11 = channelAt(x0 + 1, y0 + 1, ch);
        const double top = p00 + (p10 - p00) * fx;
        const double bot = p01 + (p11 - p01) * fx;
        const double v = top + (bot - top) * fy;
        sad += std::fabs(v - channelAt(static_cast<int>(pt.first), static_cast<int>(pt.second), 1));
      }
      if (bestSad < 0.0 || sad < bestSad) {
        bestSad = sad;
        bestM = m;
      }
    }
    return bestM;
  };
  const double mR = estimate(0);
  const double mB = estimate(2);
  // 估计出的就是「采集缺陷」：校正 scale = 缺陷的倒数（还原语义与 profile scaleR/B 一致）
  if (scaleR) *scaleR = 1.0 / std::max(0.5, mR);
  if (scaleB) *scaleB = 1.0 / std::max(0.5, mB);
  return true;
}

namespace {
std::mutex g_warnMu;
std::string g_lastWarning;
}  // namespace

void lensProfileSetWarning(const std::string& w) {
  std::lock_guard<std::mutex> lock(g_warnMu);
  g_lastWarning = w;
}
std::string lensProfileLastWarning() {
  std::lock_guard<std::mutex> lock(g_warnMu);
  return g_lastWarning;
}
void lensProfileClearWarning() {
  std::lock_guard<std::mutex> lock(g_warnMu);
  g_lastWarning.clear();
}

}  // namespace plan
}  // namespace lumina
