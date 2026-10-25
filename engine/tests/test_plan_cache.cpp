// T0.5 测试：三键缓存（换曝光不废 kGeom）+ Plan 编译/求值 + iccInput 预留
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "imaging/image_buffer.h"
#include "plan/cache_key.h"
#include "plan/op_table.h"
#include "plan/render_plan.h"

using lumina::imaging::ImageBuf;
using lumina::plan::CacheBucket;
using lumina::plan::compilePlan;
using lumina::plan::computeCacheKeys;
using lumina::plan::executePlan;
using lumina::plan::OpTable;
using lumina::plan::RenderPlan;
using P = std::pair<std::string, std::string>;

static int g_fail = 0;
static ImageBuf makeGradient();
#define CHECK(cond, msg)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

// case1：分桶映射抽查
static void testBucketMapping() {
  using lumina::plan::bucketForParam;
  CHECK(bucketForParam("geometry.cropAngle") == CacheBucket::kGeom, "cropAngle→kGeom");
  CHECK(bucketForParam("geometry.perspective.vertical") == CacheBucket::kGeom,
        "perspective→kGeom");
  CHECK(bucketForParam("optics.manualDistortion") == CacheBucket::kGeom,
        "distortion→kGeom");
  CHECK(bucketForParam("basic.exposure") == CacheBucket::kTone, "exposure→kTone");
  CHECK(bucketForParam("basic.whiteBalance") == CacheBucket::kTone, "wb→kTone");
  CHECK(bucketForParam("calibration.redHue") == CacheBucket::kTone, "calib→kTone");
  CHECK(bucketForParam("detail.colorNR") == CacheBucket::kTone, "NR→kTone");
  CHECK(bucketForParam("curve.toneCurveRGB") == CacheBucket::kDisplay, "curve→kDisplay");
  CHECK(bucketForParam("hsl.blue.sat") == CacheBucket::kDisplay, "hsl→kDisplay");
  CHECK(bucketForParam("optics.grainAmount") == CacheBucket::kDisplay, "grain→kDisplay");
  CHECK(bucketForParam("masks[]") == CacheBucket::kDisplay, "masks→kDisplay");
  CHECK(bucketForParam("detail.sharpness") == CacheBucket::kDisplay,
        "sharpen→kDisplay");
  CHECK(bucketForParam("some.unknown.param") == CacheBucket::kDisplay,
        "unknown→kDisplay(conservative)");
}

// case2：换曝光 → kGeom 键不变、kTone/kDisplay 键变（缓存命中计数模拟）
static void testExposureDoesNotInvalidateGeom() {
  const std::vector<P> base = {{"basic.exposure", "0.0"},
                               {"geometry.cropAngle", "0"}};
  const std::vector<P> exposed = {{"basic.exposure", "1.0"},
                                  {"geometry.cropAngle", "0"}};
  std::string a[lumina::plan::kCacheBucketCount];
  std::string b[lumina::plan::kCacheBucketCount];
  CHECK(computeCacheKeys(42, base, a), "keys a");
  CHECK(computeCacheKeys(42, exposed, b), "keys b");
  CHECK(a[0] == b[0], "kGeom unchanged when exposure changes");
  CHECK(a[1] != b[1], "kTone changes when exposure changes");
  CHECK(a[2] != b[2], "kDisplay changes when exposure changes");

  // 换几何 → 三键全变（kGeom 变；下游全部重算）
  const std::vector<P> rotated = {{"basic.exposure", "0.0"},
                                  {"geometry.cropAngle", "5"}};
  std::string c[lumina::plan::kCacheBucketCount];
  CHECK(computeCacheKeys(42, rotated, c), "keys c");
  CHECK(a[0] != c[0], "kGeom changes when angle changes");

  // 换 photoId → 全键变
  std::string d[lumina::plan::kCacheBucketCount];
  CHECK(computeCacheKeys(43, base, d), "keys d");
  CHECK(a[0] != d[0] && a[1] != d[1] && a[2] != d[2], "photoId salts all keys");

  // 命中计数：模拟两层缓存查询，曝光变更只 miss kTone/kDisplay
  std::map<std::string, int> hits;
  auto lookup = [&](const std::string& k) { return ++hits[k]; };
  lookup(a[0]); lookup(a[1]); lookup(a[2]);   // 首渲染填充
  lookup(b[0]); lookup(b[1]); lookup(b[2]);   // 曝光后渲染
  CHECK(hits[b[0]] == 2, "kGeom cache hit after exposure change");
  CHECK(hits[b[1]] == 1 && hits[b[2]] == 1, "kTone/kDisplay cache miss");
}

// case3：键稳定性（顺序无关 + 重复 path 覆盖）
static void testKeyStability() {
  const std::vector<P> p1 = {{"basic.exposure", "1"}, {"curve.toneCurveRGB", "0,0 255,255"}};
  const std::vector<P> p2 = {{"curve.toneCurveRGB", "0,0 255,255"}, {"basic.exposure", "1"}};
  std::string a[3], b[3];
  CHECK(computeCacheKeys(7, p1, a) && computeCacheKeys(7, p2, b), "keys ok");
  CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2], "order-insensitive keys");
  const std::vector<P> p3 = {{"basic.exposure", "1"}, {"basic.exposure", "2"}};
  std::string c[3];
  CHECK(computeCacheKeys(7, p3, c), "dup keys ok");
  std::string d[3];
  CHECK(computeCacheKeys(7, {{"basic.exposure", "2"}}, d), "last-wins keys ok");
  CHECK(c[1] == d[1], "duplicate param last-wins");
}

// case4：Plan 编译 → 算子序列按 stage 有序 + execute 全绿
static void testCompileAndExecute() {
  OpTable::instance();  // 确保单例先建
  lumina::plan::registerBuiltinOps(&OpTable::instance(), nullptr);
  ImageBuf buf = makeGradient();
  const std::vector<P> params = {
      {"basic.exposure", "0.5"},
      {"curve.toneCurveRGB", "0,0 128,118 255,255"},
      {"hsl.blue.sat", "10"},
      {"optics.vignetteAmount", "-0.3"},
      {"detail.sharpness", "25"},
      {"geometry.cropAngle", "0"},
  };
  RenderPlan plan;
  std::string err;
  CHECK(compilePlan(1, params, &plan, &err), "compile ok");
  CHECK(plan.valid(), "plan valid");
  // 序列 stage 单调不降
  int prev = -1;
  bool ordered = true;
  for (const std::string& n : plan.opSequence) {
    const lumina::plan::Operator* op = OpTable::instance().find(n);
    if (!op) { ordered = false; break; }
    const int s = lumina::plan::stageIndex(op->stage());
    if (s < prev) ordered = false;
    prev = s;
  }
  CHECK(ordered, "opSequence stage-ordered");
  CHECK(executePlan(plan, &buf, &err), "execute ok");
}

// case5：iccInput 预留位已注册且可在 S4 首位求值
static void testIccInputReserved() {
  const lumina::plan::Operator* op = OpTable::instance().find("iccInput");
  CHECK(op != nullptr, "iccInput registered");
  if (op) {
    CHECK(op->stage() == lumina::plan::Stage::S4Color, "iccInput at S4Color");
  }
  ImageBuf buf = makeGradient();
  lumina::plan::OpTable& t = OpTable::instance();
  // S4 序列：iccInput 居首合法；排在 color 算子之后不违规（同 stage）但序列必须先于 HSL 由 compilePlan 保证
  CHECK(t.evaluate({"iccInput", "color.hsl"}, &buf, nullptr), "iccInput before hsl ok");
}

// case6：非法序列拒绝（S4 曲线在前、S2 曝光在后 → stage 逆序）
static void testOrderViolationRejected() {
  ImageBuf buf = makeGradient();
  std::string err;
  CHECK(!OpTable::instance().evaluate({"curve.toneCurveRGB", "basic.exposure"}, &buf, &err),
        "reverse order rejected");
}

static ImageBuf makeGradient() {
  ImageBuf buf(32, 24, lumina::imaging::ColorSpaceTag::EncodedSRGB);
  for (int y = 0; y < 24; ++y) {
    for (int x = 0; x < 32; ++x) {
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * 32 + x) * 3];
      p[0] = static_cast<uint16_t>(x * 2048);
      p[1] = static_cast<uint16_t>(y * 2730);
      p[2] = static_cast<uint16_t>((x + y) * 1000);
    }
  }
  return buf;
}

int main() {
  testBucketMapping();
  testExposureDoesNotInvalidateGeom();
  testKeyStability();
  testCompileAndExecute();
  testIccInputReserved();
  testOrderViolationRejected();
  if (g_fail == 0) {
    std::printf("test_plan_cache: ALL PASS (6 cases)\n");
    return 0;
  }
  std::printf("test_plan_cache: %d FAILURES\n", g_fail);
  return 1;
}
