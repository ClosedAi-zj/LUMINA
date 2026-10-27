// T2.9 曲线精度收口：C1/C2/C3 回归 + C4 逐码值平滑门（N25）
// C1/C2/C3 口径 = docs/v1.2.0/M2-增量PRD.md:215-217：
//   C1 复合曲线影响三通道；C2 分通道选择性（其余通道 ≤1 LSB）；C3 参数曲线四段
//   + 三分界（分界点移动使拐点位置可测移动）。
// C4 口径 = M2-架构设计.md:909：曲线查表 0..65535 逐步进，相邻输出差 ≤1 LSB，
//   且无周期性平台。256 点 LUT 在此门必然失败（~257 码平台 + ~243 LSB 跳变），
//   1024 栅格 + 分段线性插值通过——本测试同时内嵌 256 LUT 旧语义对照，证明
//   门槛可区分两代实现（防止假绿）。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "imaging/image_buffer.h"
#include "json/json_value.h"
#include "plan/op_table.h"
#include "plan/ops/ops.h"

using namespace lumina;
using lumina::imaging::ImageBuf;
using lumina::json::JsonValue;
using lumina::plan::OpTable;
using lumina::plan::Operator;

static int g_fail = 0;
#define CHECK(cond, msg)                                                 \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg);           \
      ++g_fail;                                                          \
    } else {                                                             \
      std::printf("  [PASS] %s\n", msg);                                 \
    }                                                                    \
  } while (0)

static JsonValue curvePoints(std::vector<std::pair<double, double>> pts) {
  JsonValue arr = JsonValue::arrayValue();
  for (const auto& pt : pts) {
    JsonValue o = JsonValue::objectValue();
    o.setItem("x", pt.first);
    o.setItem("y", pt.second);
    arr.addItem(o);
  }
  return arr;
}

static bool applyParams(const char* opName, const JsonValue& params,
                        ImageBuf* buf, std::string* err) {
  const Operator* op = OpTable::instance().find(opName);
  if (!op) {
    if (err) *err = std::string("op not found: ") + opName;
    return false;
  }
  std::vector<std::string> names{opName};
  std::map<std::string, JsonValue> byOp{{opName, params}};
  return OpTable::instance().evaluate(names, buf, byOp, err);
}

static bool sameBytes(const ImageBuf& a, const ImageBuf& b) {
  return a.w == b.w && a.h == b.h && a.data == b.data;
}

// 三段条带：暗(0.10)/中(0.45)/亮(0.85)——C1/C2 探针（码空间，同 test_ops93）
static ImageBuf makeToneStripes(int w, int h) {
  ImageBuf buf(w, h);
  const uint16_t tones[3] = {
      static_cast<uint16_t>(0.10 * 65535.0 + 0.5),
      static_cast<uint16_t>(0.45 * 65535.0 + 0.5),
      static_cast<uint16_t>(0.85 * 65535.0 + 0.5)};
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      p[0] = p[1] = p[2] = tones[std::min(2, x * 3 / w)];
    }
  }
  return buf;
}

// 逐码值 ramp：像素 i = 码值 i（0..65535，三通道同值）——C4 主夹具
static ImageBuf makeCodeRamp() {
  ImageBuf buf(256, 256);
  for (uint32_t i = 0; i < 65536; ++i) {
    const uint16_t c = static_cast<uint16_t>(i);
    buf.data[i * 3] = buf.data[i * 3 + 1] = buf.data[i * 3 + 2] = c;
  }
  return buf;
}

static JsonValue numParams(const char* key, double v) {
  JsonValue p = JsonValue::objectValue();
  p.setItem(key, v);
  return p;
}

// 旧 256 点 LUT 语义对照（T1.3 口径：round 量化索引），用于证明 C4 门槛
// 能区分 256/1024 两代实现。返回 {maxAdj, maxRun}（16bit 域）。
static std::pair<int, int> legacy256Stats(const std::vector<std::pair<double, double>>& pts) {
  std::vector<std::pair<double, double>> p = pts;
  std::sort(p.begin(), p.end());
  if (p.front().first > 0) p.insert(p.begin(), {0.0, p.front().second});
  if (p.back().first < 255) p.push_back({255.0, p.back().second});
  std::vector<double> lut(256);
  for (int i = 0; i < 256; ++i) {
    const double u = static_cast<double>(i);
    size_t k = 0;
    while (k + 1 < p.size() && p[k + 1].first < u) ++k;
    const double t = (u - p[k].first) / std::max(1e-9, p[k + 1].first - p[k].first);
    lut[i] = p[k].second + (p[k + 1].second - p[k].second) * t;
  }
  int maxAdj = 0;
  int maxRun = 1;
  int run = 1;
  int prev = -1;
  for (uint32_t c = 0; c <= 65535; ++c) {
    const double u = c / 65535.0 * 255.0;
    const int idx = std::clamp(static_cast<int>(u + 0.5), 0, 255);
    const int out = static_cast<int>(lut[idx] / 255.0 * 65535.0 + 0.5);
    if (c > 0) {
      maxAdj = std::max(maxAdj, std::abs(out - prev));
      run = (out == prev) ? run + 1 : 1;
      maxRun = std::max(maxRun, run);
    }
    prev = out;
  }
  return {maxAdj, maxRun};
}

// ---- C1：复合曲线影响三通道 ----
static void testC1Composite() {
  std::printf("C1 复合曲线影响三通道\n");
  std::string err;
  ImageBuf buf = makeToneStripes(48, 8);
  ImageBuf ref = buf;
  JsonValue p = JsonValue::objectValue();
  p.setItem("curve.toneCurveRGB", curvePoints({{0, 0}, {128, 100}, {255, 220}}));
  CHECK(applyParams("curve.toneCurveRGB", p, &buf, &err), "C1: composite curve apply");
  int moved[3] = {0, 0, 0};
  for (size_t i = 0; i + 2 < buf.data.size(); i += 3) {
    for (int c = 0; c < 3; ++c) {
      if (buf.data[i + c] != ref.data[i + c]) ++moved[c];
    }
  }
  CHECK(moved[0] > 0 && moved[1] > 0 && moved[2] > 0, "C1: R/G/B 三通道均产生非零差异");
}

// ---- C2：分通道选择性 ----
static void testC2ChannelIsolation() {
  std::printf("C2 分通道选择性\n");
  std::string err;
  ImageBuf buf = makeToneStripes(48, 8);
  ImageBuf ref = buf;
  JsonValue p = JsonValue::objectValue();
  p.setItem("curve.toneCurveR", curvePoints({{0, 0}, {128, 100}, {255, 220}}));
  CHECK(applyParams("curve.toneCurveR", p, &buf, &err), "C2: R-only curve apply");
  int movedR = 0;
  int maxOtherDelta = 0;
  for (size_t i = 0; i + 2 < buf.data.size(); i += 3) {
    if (buf.data[i] != ref.data[i]) ++movedR;
    maxOtherDelta = std::max(maxOtherDelta,
                             std::abs(static_cast<int>(buf.data[i + 1]) - static_cast<int>(ref.data[i + 1])));
    maxOtherDelta = std::max(maxOtherDelta,
                             std::abs(static_cast<int>(buf.data[i + 2]) - static_cast<int>(ref.data[i + 2])));
  }
  CHECK(movedR > 0, "C2: R 通道产生差异");
  CHECK(maxOtherDelta <= 1, "C2: G/B 通道 max|Δ| ≤ 1 LSB");
}

// ---- C3：参数曲线四段 + 三分界 ----
static void testC3Parametric() {
  std::printf("C3 参数曲线四段 + 三分界\n");
  std::string err;
  // 四段各自产生分段差异（对应分区中点码域探针）
  struct Zone { const char* key; float lin; const char* label; };
  const Zone zones[] = {
      {"parametric.highlights", 0.85f, "highlights"},
      {"parametric.lights", 0.60f, "lights"},
      {"parametric.darks", 0.30f, "darks"},
      {"parametric.shadows", 0.10f, "shadows"},
  };
  for (const Zone& z : zones) {
    ImageBuf buf(4, 4);
    const uint16_t c = static_cast<uint16_t>(z.lin * 65535.0 + 0.5);
    for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = c;
    ImageBuf ref = buf;
    CHECK(applyParams("curve.parametric", numParams(z.key, 50.0), &buf, &err),
          (std::string("C3: ") + z.label + " apply").c_str());
    CHECK(buf.data[0] != ref.data[0],
          (std::string("C3: ") + z.label + " 段产生差异").c_str());
  }
  // 三分界：shadowSplit 25 → 45 使 darks 权重分布移动（分区口径拐点可测）
  ImageBuf a = makeCodeRamp();
  ImageBuf b = makeCodeRamp();
  JsonValue pa = JsonValue::objectValue();
  pa.setItem("parametric.darks", 60.0);
  pa.setItem("parametric.shadowSplit", 25.0);
  JsonValue pb = JsonValue::objectValue();
  pb.setItem("parametric.darks", 60.0);
  pb.setItem("parametric.shadowSplit", 45.0);
  CHECK(applyParams("curve.parametric", pa, &a, &err), "C3: split=25 apply");
  CHECK(applyParams("curve.parametric", pb, &b, &err), "C3: split=45 apply");
  const size_t probe = static_cast<size_t>(120.0 / 255.0 * 65535.0) * 3;  // 过渡带内
  const int d = std::abs(static_cast<int>(a.data[probe]) - static_cast<int>(b.data[probe]));
  CHECK(d >= 2, "C3: 分界移动使过渡带差异 ≥ 2 LSB（拐点可测移动）");
  const size_t hi = static_cast<size_t>(0.90 * 65535.0) * 3;  // 分区远端
  CHECK(a.data[hi] == b.data[hi], "C3: 分区远端不受分界移动影响");
}

// ---- C4：0..65535 逐码值平滑（相邻差 ≤1 LSB，无周期性平台）----
static void testC4Precision() {
  std::printf("C4 逐码值平滑（1024 栅格 + 分段线性插值）\n");
  std::string err;
  // 全程斜率 ≤1 的两条实测曲线：凸形压暗 + 抬黑（斜率 >1 的曲线物理上
  // 必然 >1 LSB/码，不在 C4 门槛语义内）
  const std::vector<std::pair<double, double>> curves[] = {
      {{0, 0}, {128, 100}, {255, 220}},   // 斜率 0.781 / 0.945
      {{0, 16}, {255, 255}},              // 斜率 0.937
  };
  for (size_t ci = 0; ci < 2; ++ci) {
    ImageBuf buf = makeCodeRamp();
    JsonValue p = JsonValue::objectValue();
    p.setItem("curve.toneCurveRGB", curvePoints(curves[ci]));
    CHECK(applyParams("curve.toneCurveRGB", p, &buf, &err),
          (std::string("C4: curve #") + std::to_string(ci) + " apply").c_str());
    for (int c = 0; c < 3; ++c) {
      int maxAdj = 0;
      int maxRun = 1;
      int run = 1;
      int distinct = 1;
      int changed = 0;
      bool monotonic = true;
      int prev = buf.data[c];
      for (uint32_t i = 1; i < 65536; ++i) {
        const int out = buf.data[i * 3 + c];
        if (out != static_cast<int>(i)) ++changed;
        maxAdj = std::max(maxAdj, std::abs(out - prev));
        if (out < prev) monotonic = false;
        run = (out == prev) ? run + 1 : 1;
        maxRun = std::max(maxRun, run);
        if (out != prev) ++distinct;
        prev = out;
      }
      const std::string tag = "C4 curve#" + std::to_string(ci) + " ch" + std::to_string(c);
      CHECK(changed >= 40000,
            (tag + ": 曲线实际作用于全谱（changed=" + std::to_string(changed) + "）").c_str());
      CHECK(maxAdj <= 1, (tag + ": 相邻输出差 max=" + std::to_string(maxAdj) + " ≤ 1 LSB").c_str());
      CHECK(maxRun <= 8, (tag + ": 无周期性平台（maxRun=" + std::to_string(maxRun) + " ≤ 8）").c_str());
      CHECK(distinct >= 40000, (tag + ": distinct=" + std::to_string(distinct) + " ≥ 40000").c_str());
      CHECK(monotonic, (tag + ": 单调不减（无过冲）").c_str());
      if (c == 0) {
        // 256 点 LUT 旧语义对照：同曲线下必然失败（平台 ~257 码、跳变 ~243 LSB）
        const auto legacy = legacy256Stats(curves[ci]);
        std::printf("    [对照] 256 LUT 旧语义同曲线: maxAdj=%d LSB, maxRun=%d 码\n",
                    legacy.first, legacy.second);
        CHECK(legacy.first > 100 && legacy.second > 200,
              "C4 门槛可区分两代实现（256 LUT 对照 maxAdj>100 且 maxRun>200）");
      }
    }
  }
  // 恒等回归：线性点走 identity 跳过，字节不变
  ImageBuf id = makeCodeRamp();
  ImageBuf idref = id;
  JsonValue pid = JsonValue::objectValue();
  pid.setItem("curve.toneCurveRGB", curvePoints({{0, 0}, {255, 255}}));
  CHECK(applyParams("curve.toneCurveRGB", pid, &id, &err), "C4: identity apply");
  CHECK(sameBytes(id, idref), "C4: 线性点恒等跳过（字节不变）");
}

int main() {
  std::printf("=== test_curve_precision (T2.9 C1-C4) ===\n");
  std::string regErr;
  if (!lumina::plan::registerBuiltinOps(&OpTable::instance(), &regErr)) {
    std::printf("FATAL registerBuiltinOps: %s\n", regErr.c_str());
    return 2;
  }
  testC1Composite();
  testC2ChannelIsolation();
  testC3Parametric();
  testC4Precision();
  if (g_fail == 0) {
    std::printf("ALL PASS\n");
    return 0;
  }
  std::printf("%d FAILURES\n", g_fail);
  return 1;
}
