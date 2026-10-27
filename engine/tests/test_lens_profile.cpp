// M2 T2.11 验收（N24）：镜头 profile 库 + CA 新叶 #94（93→94 口径）
//   L1  profile 畸变 ±100 → 四角位移单调、符号正确、≥1px
//   L2  暗角 −100 → 四角能量降 ≥30%、中心 ≤±2%（+100 自动校正方向相反）
//   L3  3px 色边样本边带能量降 ≥50%（CA 新叶有效性，profile scaleR/scaleB 回位）
//   L4  二进制/资源无 LCP 数据——依赖清单（NOTICE.md）+ 文件扫描双证
//   未命中 → identity + warning（零静默）；O4 单次重采样保持；焦距插值；
//   叶 #94 全链同步（collect 读叶 / render_plan 路由 geometry.apply）。
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <functional>
#include <string>
#include <vector>

#include "core/str_util.h"
#include "imaging/image_buffer.h"
#include "json/json_value.h"
#include "plan/geometry_chain.h"
#include "plan/lens_profile.h"
#include "plan/render_plan.h"

using namespace lumina;
using lumina::imaging::ImageBuf;
using lumina::json::JsonValue;
using lumina::plan::GeometryChainParams;
using lumina::plan::GeometryChainStats;

static int g_fail = 0;
// msg 先物化为 std::string 再经 c_str() 传 printf（同 af32b7a：std::string 直传
// printf 可变参数是 UB——MinGW GCC 8 不报错但 SSO 缓冲被当 char* 解引用）。
#define CHECK(cond, msg)                                                 \
  do {                                                                   \
    const std::string check_msg_ = (msg);                                \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__,                 \
                  check_msg_.c_str());                                   \
      ++g_fail;                                                          \
    } else {                                                             \
      std::printf("  [PASS] %s\n", check_msg_.c_str());                  \
    }                                                                    \
  } while (0)

#ifndef LUMINA_LENS_PROFILES_SRC_DIR
#define LUMINA_LENS_PROFILES_SRC_DIR "data/lens-profiles"
#endif

static ImageBuf makeUniform(int w, int h, uint16_t v) {
  ImageBuf buf(w, h);
  for (size_t i = 0; i < buf.data.size(); i += 3) {
    buf.data[i] = v;
    buf.data[i + 1] = v;
    buf.data[i + 2] = v;
  }
  return buf;
}

// 条纹场景（高频边缘 → 横向 CA 色边显著）
static ImageBuf makeStripes(int w, int h) {
  ImageBuf buf(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      const int g = ((x / 6 + y / 6) % 2) ? 50000 : 8000;
      p[0] = static_cast<uint16_t>(g);
      p[1] = static_cast<uint16_t>(g);
      p[2] = static_cast<uint16_t>(g);
    }
  }
  return buf;
}

// 模拟采集缺陷：R/B 相对 G 的径向放大（横向 CA）——理想图 → 传感器图
// 采集语义：理想点 r_i 落在 r_i·m 处 → 传感器(r) = 理想(r/m)。双线性最近 4 邻。
static ImageBuf applyCaptureCa(const ImageBuf& ideal, double mR, double mB) {
  ImageBuf out(ideal.w, ideal.h);
  const double ccx = (ideal.w - 1) / 2.0;
  const double ccy = (ideal.h - 1) / 2.0;
  for (int y = 0; y < ideal.h; ++y) {
    for (int x = 0; x < ideal.w; ++x) {
      const double dx = x - ccx;
      const double dy = y - ccy;
      uint16_t* q = &out.data[(static_cast<size_t>(y) * ideal.w + x) * 3];
      for (int c = 0; c < 3; ++c) {
        const double m = (c == 0) ? mR : (c == 2 ? mB : 1.0);
        const double sx = ccx + dx / m;
        const double sy = ccy + dy / m;
        const int x0 = static_cast<int>(std::floor(sx));
        const int y0 = static_cast<int>(std::floor(sy));
        if (x0 < 0 || y0 < 0 || x0 >= ideal.w - 1 || y0 >= ideal.h - 1) {
          q[c] = ideal.data[(static_cast<size_t>(y) * ideal.w + x) * 3 + c];
          continue;
        }
        const double fx = sx - x0;
        const double fy = sy - y0;
        auto at = [&](int xx, int yy) {
          return ideal.data[(static_cast<size_t>(yy) * ideal.w + xx) * 3 + c];
        };
        const double v = at(x0, y0) * (1 - fx) * (1 - fy) + at(x0 + 1, y0) * fx * (1 - fy) +
                         at(x0, y0 + 1) * (1 - fx) * fy + at(x0 + 1, y0 + 1) * fx * fy;
        q[c] = static_cast<uint16_t>(std::lround(v));
      }
    }
  }
  return out;
}

// 色边边带能量：annulus 环带内 mean(|R−G| + |B−G|)（暗角增益乘三通道同权，
// 环带限中半径以压低增益方差对度量的干扰）
static double fringeEnergy(const ImageBuf& buf, double rLo, double rHi) {
  const double ccx = (buf.w - 1) / 2.0;
  const double ccy = (buf.h - 1) / 2.0;
  const double maxR = std::hypot(ccx, ccy);
  double sum = 0.0;
  int n = 0;
  for (int y = 0; y < buf.h; ++y) {
    for (int x = 0; x < buf.w; ++x) {
      const double r = std::hypot(x - ccx, y - ccy) / maxR;
      if (r < rLo || r > rHi) continue;
      const uint16_t* p = &buf.data[(static_cast<size_t>(y) * buf.w + x) * 3];
      sum += std::abs(static_cast<int>(p[0]) - static_cast<int>(p[1])) +
             std::abs(static_cast<int>(p[2]) - static_cast<int>(p[1]));
      ++n;
    }
  }
  return n > 0 ? sum / n : 0.0;
}

// 指定锚点的方块均值（L2 链级测点：r≈0.84 处既有显著暗角增益、又避开
// CA 校正在极角越界采样的黑边区）
static double boxMeanAt(const ImageBuf& buf, int x0, int y0, int box) {
  double sum = 0.0;
  int n = 0;
  for (int y = y0; y < y0 + box; ++y) {
    for (int x = x0; x < x0 + box; ++x) {
      const uint16_t* p = &buf.data[(static_cast<size_t>(y) * buf.w + x) * 3];
      sum += (p[0] + p[1] + p[2]) / 3.0;
      ++n;
    }
  }
  return sum / std::max(1, n);
}

static double centerMean(const ImageBuf& buf, int box) {
  const int cx = buf.w / 2;
  const int cy = buf.h / 2;
  double sum = 0.0;
  int n = 0;
  for (int y = cy - box / 2; y < cy + box / 2; ++y) {
    for (int x = cx - box / 2; x < cx + box / 2; ++x) {
      const uint16_t* p = &buf.data[(static_cast<size_t>(y) * buf.w + x) * 3];
      sum += (p[0] + p[1] + p[2]) / 3.0;
      ++n;
    }
  }
  return sum / std::max(1, n);
}

static GeometryChainParams profileParams() {
  GeometryChainParams p;
  p.lensProfileEnable = 1.0;
  p.lensProfileMake = "Panasonic";
  p.lensProfileModel = "DC-S5M2";
  p.lensProfileLensMake = "Panasonic";
  p.lensProfileLensModel = "LUMIX S 20-60mm F3.5-5.6";
  p.lensProfileFocalMm = 20.0;
  return p;
}

// MinGW GCC 8.1 兼容：<filesystem> 与 windows.h 同 TU 冲突 → dirent 递归遍历
static void walkFiles(const std::string& dir, int depth,
                      const std::function<void(const std::string&)>& fn) {
  if (depth <= 0) return;
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return;
  while (const dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    const std::string path = dir + "/" + name;
    DIR* sub = opendir(path.c_str());
    if (sub != nullptr) {
      closedir(sub);
      walkFiles(path, depth - 1, fn);
    } else {
      fn(path);
    }
  }
  closedir(d);
}

static std::vector<std::string> listDir(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return out;
  while (const dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    out.push_back(dir + "/" + name);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

int main() {
  // ---------------- P0 数据库加载 + 匹配/未命中 ----------------
  std::printf("[P0] profile 库加载 + 匹配\n");
  lumina::plan::lensProfileClearWarning();
  lumina::plan::LensProfileDb& db = lumina::plan::LensProfileDb::global();
  CHECK(db.size() >= 3, "profile 库加载（data/lens-profiles，实际 " +
                            std::to_string(db.size()) + " 条）");
  lumina::plan::LensProfile prof;
  CHECK(db.match("Panasonic", "DC-S5M2", "Panasonic", "LUMIX S 20-60mm F3.5-5.6", 20.0,
                 &prof) && prof.valid(),
        "match：Panasonic S5M2 + 20-60 命中");
  CHECK(db.match("Panasonic", "DC-S5M2", "", "lumix s 20-60mm f3.5-5.6", 35.0, &prof) &&
            std::fabs(prof.bucketForFocal(35.0).k1 + 0.031) < 1e-9,
        "match：lens 归一化（大小写/空白）命中 + 焦距直取桶");
  // 焦距插值：40mm ∈ (35,60) → k1 介于两桶之间
  const double k40 = prof.bucketForFocal(40.0).k1;
  CHECK(k40 > -0.031 && k40 < 0.014,
        "焦距线性插值：40mm k1=" + std::to_string(k40) + " ∈ (-0.031, 0.014)");
  CHECK(!db.match("Panasonic", "DC-S5M2", "", "Sigma 999mm F0.7", 20.0, nullptr),
        "match：未知镜头未命中");

  // ---------------- L1 畸变 ±100：四角位移单调、符号正确、≥1px ----------------
  std::printf("\n[L1] profile 畸变 ±100 四角位移\n");
  {
    // 模型级：corner r=1 处 src 半径乘数随 amount 单调、符号正确（k1<0 桶形缺陷：
    // +100 校正乘数 d(1)<1 拉回内侧，−100 放大乘数 >1）
    const double fPlus = lumina::plan::lensDistortRadiusFactor(1.0, -0.082, 0.021, -0.004, 100.0);
    const double fZero = lumina::plan::lensDistortRadiusFactor(1.0, -0.082, 0.021, -0.004, 0.0);
    const double fMinus = lumina::plan::lensDistortRadiusFactor(1.0, -0.082, 0.021, -0.004, -100.0);
    CHECK(std::fabs(fZero - 1.0) < 1e-12, "L1: amount=0 恒等");
    CHECK(fPlus < 1.0 && fMinus > 1.0, "L1: 符号正确（+100 校正 <1 < −100 放大）");
    // 单调：位移幅度 |f−1| 随 |amount| 单调增（d(1)<1 → 乘数随 amount 递减，
    // 但位移在 0 两侧各自单调、且方向相反 = 符号正确）
    bool monoPlus = true;
    bool monoMinus = true;
    double prevPlus = 0.0;
    double prevMinus = 0.0;
    for (int a = 0; a <= 100; a += 10) {
      const double f = lumina::plan::lensDistortRadiusFactor(1.0, -0.082, 0.021, -0.004,
                                                             static_cast<double>(a));
      const double disp = std::fabs(f - 1.0);
      if (a > 0 && disp <= prevPlus) monoPlus = false;
      const double fm = lumina::plan::lensDistortRadiusFactor(1.0, -0.082, 0.021, -0.004,
                                                              static_cast<double>(-a));
      const double dispm = std::fabs(fm - 1.0);
      if (a > 0 && dispm <= prevMinus) monoMinus = false;
      prevPlus = disp;
      prevMinus = dispm;
    }
    CHECK(monoPlus && monoMinus, "L1: 位移幅度随 |amount| 单调（−100..100 双向）");
    // 链级：2000×1500 图，+100 校正下角落位移 ≥1px（均匀图位移不可见 → 斜坡图）
    ImageBuf ramp(2000, 1500);
    for (int y = 0; y < ramp.h; ++y) {
      for (int x = 0; x < ramp.w; ++x) {
        uint16_t* q = &ramp.data[(static_cast<size_t>(y) * ramp.w + x) * 3];
        q[0] = q[1] = q[2] = static_cast<uint16_t>((x + y) % 255 * 250);
      }
    }
    ImageBuf id1 = ramp;
    ImageBuf id2 = ramp;
    GeometryChainStats st;
    GeometryChainParams p = profileParams();
    CHECK(lumina::plan::applyGeometryChain(&id1, p, &st, nullptr), "L1: profile 链执行");
    // 位移量 = 角像素值差非零（斜坡 1 码 ≈ 1px 步进）
    const uint16_t* c0 = &ramp.data[0];
    const uint16_t* c1 = &id1.data[0];
    CHECK(c0[0] != c1[0], "L1: profile 校正生效（角内容位移，≥1px 量级）");
    CHECK(st.profileHit && st.subSteps.size() >= 1, "L1: stats.profileHit + 子步 1 记录");
  }

  // ---------------- L2 暗角 −100：四角降 ≥30%、中心 ≤±2% ----------------
  std::printf("\n[L2] profile 暗角 −100 压角\n");
  {
    const double a = 0.46, b = -0.03, c = 0.03;  // 内置 Panasonic@20mm 档
    const double gCorner = lumina::plan::lensVignetteGain(1.0, a, b, c, -100.0);
    const double gCenter = lumina::plan::lensVignetteGain(0.0, a, b, c, -100.0);
    CHECK(gCorner <= 0.70, "L2: −100 四角增益 " + std::to_string(gCorner) +
                               " ≤ 0.70（能量降 ≥30%）");
    CHECK(std::fabs(gCenter - 1.0) <= 0.02, "L2: 中心增益 ±2% 内");
    // 链级：自动校正（+100）方向相反——均匀图近角（r≈0.84）增亮、中心不变
    // （不取极角像素：CA 校正在极角越界采样按口径填黑，会污染亮度度量）
    ImageBuf uni = makeUniform(960, 640, 20000);
    ImageBuf out = uni;
    GeometryChainStats st;
    CHECK(lumina::plan::applyGeometryChain(&out, profileParams(), &st, nullptr),
          "L2: profile 链执行（均匀图）");
    const double cm0 = boxMeanAt(uni, 77, 51, 40);
    const double cm1 = boxMeanAt(out, 77, 51, 40);
    const double ce0 = centerMean(uni, 40);
    const double ce1 = centerMean(out, 40);
    CHECK(cm1 > cm0 * 1.25, "L2: 自动校正 +100 → 近角增益 " +
                                std::to_string(cm1 / cm0) + " > 1.25（v(0.84)≈1.32）");
    CHECK(std::fabs(ce1 / ce0 - 1.0) <= 0.02, "L2: 中心 ≤±2%（实测 " +
                                                   std::to_string(ce1 / ce0) + "）");
    CHECK(st.resampleCalls == 1, "L2: 暗角仅乘不采样——重采样仍 1 次（O4）");
  }

  // ---------------- L3 CA 新叶：3px 色边边带能量降 ≥50% ----------------
  std::printf("\n[L3] CA 叶 #94 色边校正\n");
  {
    const double mR = 1.0083, mB = 0.9917;  // Panasonic@20mm（对角 577px @960×640）
    ImageBuf ideal = makeStripes(960, 640);
    ImageBuf captured = applyCaptureCa(ideal, mR, mB);
    const double eBefore = fringeEnergy(captured, 0.2, 0.9);
    // 色边幅度：对角 577px × 0.0083 ≈ 4.8px 错位（含 3px 样本带），条纹 6px 周期 → 显著
    CHECK(eBefore > 1000.0, "L3: 采集缺陷色边显著（边带能量 " +
                                std::to_string(eBefore) + "）");
    ImageBuf out = captured;
    GeometryChainStats st;
    GeometryChainParams p = profileParams();
    p.chromaticAberration = 0.0;  // 叶 0 = profile 自动全量校正
    CHECK(lumina::plan::applyGeometryChain(&out, p, &st, nullptr), "L3: 校正链执行");
    const double eAfter = fringeEnergy(out, 0.2, 0.9);
    CHECK(eAfter <= eBefore * 0.5, "L3: 边带能量降 ≥50%（" +
                                       std::to_string(eBefore) + " → " +
                                       std::to_string(eAfter) + "）");
    CHECK(st.subSteps.size() >= 2 &&
              std::find(st.subSteps.begin(), st.subSteps.end(), 2) != st.subSteps.end(),
          "L3: 子步 2（CA）执行记录");
    // 叶 #94 强度口径：leaf=−100 → scale 回 1（校正关）
    const double sOff = lumina::plan::lensCaScale(mR, -100.0);
    const double s2x = lumina::plan::lensCaScale(mR, 100.0);
    CHECK(std::fabs(sOff - 1.0) < 1e-12, "L3: leaf=−100 → 校正关（scale=1）");
    CHECK(s2x > mR, "L3: leaf=+100 → 强度 2×");
  }

  // ---------------- L4 无 LCP/DCP：依赖清单 + 扫描双证 ----------------
  std::printf("\n[L4] 无 LCP/DCP 数据双证\n");
  {
    // ctest 工作目录是 build 目录 → 源码树根由 CMake 注入的 profile 目录上溯三级
    // （<root>/engine/data/lens-profiles → <root>）
    const std::string srcDir = LUMINA_LENS_PROFILES_SRC_DIR;
    std::string repoRoot = srcDir;
    for (int i = 0; i < 3; ++i) {
      repoRoot = repoRoot.substr(0, repoRoot.find_last_of("/\\"));
    }
    // 证 1：依赖清单（NOTICE.md L4 声明）
    std::string notice;
    std::string err;
    CHECK(lumina::readFile(repoRoot + "/NOTICE.md", &notice, &err) &&
              notice.find("LCP") != std::string::npos &&
              notice.find("不含 Adobe LCP / DCP") != std::string::npos,
          "L4 证 1：NOTICE.md 依赖清单声明「不含 Adobe LCP / DCP」");
    // 证 2：文件扫描——engine/ + docs/ 全树无 *.lcp / *.dcp
    int lcpCount = 0;
    for (const char* dir : {"engine", "docs"}) {
      walkFiles(repoRoot + "/" + dir, 8, [&](const std::string& path) {
        std::string lower;
        for (char ch : path) lower.push_back(static_cast<char>(std::tolower((unsigned char)ch)));
        const size_t n = lower.size();
        if ((n > 4 && lower.compare(n - 4, 4, ".lcp") == 0) ||
            (n > 4 && lower.compare(n - 4, 4, ".dcp") == 0)) {
          ++lcpCount;
        }
      });
    }
    CHECK(lcpCount == 0, "L4 证 2：engine/ + docs/ 扫描 0 个 *.lcp/*.dcp");
    // 数据目录 source 白名单
    bool srcOk = true;
    for (const std::string& path : listDir(srcDir)) {
      if (path.size() <= 5 || path.compare(path.size() - 5, 5, ".json") != 0) continue;
      std::string text;
      if (!lumina::readFile(path, &text, &err)) continue;
      JsonValue j;
      if (!JsonValue::parse(text, &j, &err)) continue;
      const std::string s = j.get("source").asString();
      if (s != "self-built" && s != "lensfun-db(CC-BY-SA-3.0)") srcOk = false;
    }
    CHECK(srcOk, "L4 证 2b：profile JSON source ∈ {self-built, lensfun-db(CC-BY-SA-3.0)}");
  }

  // ---------------- 未命中：identity + warning（零静默） ----------------
  std::printf("\n[Miss] 未命中 → identity + warning\n");
  {
    ImageBuf uni = makeUniform(320, 240, 30000);
    ImageBuf out = uni;
    GeometryChainParams p = profileParams();
    p.lensProfileLensModel = "Sigma 999mm F0.7";
    GeometryChainStats st;
    CHECK(lumina::plan::applyGeometryChain(&out, p, &st, nullptr), "miss: 链执行成功");
    CHECK(!st.profileHit && st.warning.find("未检测到镜头配置") != std::string::npos,
          "miss: stats.warning 显式告知（零静默）");
    CHECK(out.data == uni.data && out.w == uni.w && out.h == uni.h,
          "miss: identity 字节不变（不猜测）");
    CHECK(!lumina::plan::lensProfileLastWarning().empty(),
          "miss: lensProfileLastWarning 可观测");
  }

  // ---------------- O4 + 叶 #94 全链同步 ----------------
  std::printf("\n[Sync] O4 单次重采样 + 叶 #94 全链\n");
  {
    lumina::plan::geometryChainResetCounters();
    ImageBuf out = makeStripes(480, 320);
    CHECK(lumina::plan::applyGeometryChain(&out, profileParams(), nullptr, nullptr),
          "sync: 链执行");
    CHECK(lumina::plan::geometryChainResampleCount() == 1,
          "sync: O4 profile 全活时 applyGeometryChain 恰 1 次");
    // collect 读叶 #94
    JsonValue params = JsonValue::objectValue();
    params.setItem("optics.chromaticAberration", static_cast<int64_t>(50));
    params.setItem("optics.lensProfileEnable", static_cast<int64_t>(1));
    const GeometryChainParams cp = lumina::plan::collectGeometryChainParams(params);
    CHECK(cp.chromaticAberration == 50.0 && cp.lensProfileEnable == 1.0,
          "sync: collectGeometryChainParams 读叶 #94（optics.chromaticAberration）");
    // render_plan 路由：叶 #94 → geometry.apply（kPipelineOrder 行 2/3/4 的宿主算子）
    lumina::plan::RenderPlan plan;
    const std::vector<std::pair<std::string, std::string>> leaves = {
        {"optics.chromaticAberration", "50"}};
    CHECK(lumina::plan::compilePlan(1, leaves, &plan, nullptr) &&
              std::find(plan.opSequence.begin(), plan.opSequence.end(), "geometry.apply") !=
                  plan.opSequence.end() &&
              plan.paramsByOp.count("geometry.apply") == 1,
          "sync: 叶 #94 路由 geometry.apply（S1 单算子消费，无第二次重采样）");
  }

  std::printf("\n=== 结果：%s（失败 %d 项）===\n", g_fail == 0 ? "全部通过" : "存在失败",
              g_fail);
  return g_fail == 0 ? 0 : 1;
}
