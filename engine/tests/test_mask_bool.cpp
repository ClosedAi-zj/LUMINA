// M2 T2.6 验收（N22）：布尔 fold 三态 + 施加纪律 + A5 零静默 + A6 JSON 往返
//
// - fold 三态（§3.1.3 伪码逐字）：Replace: f_i=raw_i；Intersect: f_i=f_{i-1}*raw_i；
//   Subtract: f_i=f_{i-1}*(1-raw_i)；首条 combine 无意义（按 replace）。
// - 相减不施加 local：subtract 蒙版携带 local 时输出与不含它逐字节相同（appliesLocal=false），
//   且 fold 值向后续 intersect 蒙版传递（减面积不是死语义）。
// - amount 正负语义：amount<0 → w<0 → 局部调整反向（≠反相蒙版，§3.1.3）。
// - A5：depth/aiSky/aiSubject/aiBackground/未知类型一律显式报错「本版不支持（v1.3）」，
//   且失败时不得改动任何像素（零静默 + 无半写）。
// - A6：masks 参数 序列化→解析→再序列化 字节稳定；往返后 fold 平面等值。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "imaging/image_buffer.h"
#include "json/json_value.h"
#include "plan/op_table.h"
#include "plan/ops/mask_field.h"
#include "plan/ops/ops.h"

using namespace lumina;
using lumina::imaging::ImageBuf;
using lumina::json::JsonValue;
using lumina::plan::OpTable;

static int g_fail = 0;
#define CHECK(cond, msg)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg);  \
      ++g_fail;                                                 \
    } else {                                                    \
      std::printf("  [PASS] %s\n", msg);                        \
    }                                                           \
  } while (0)

namespace {

bool runOne(const std::string& op, const JsonValue& params, ImageBuf* buf,
            std::string* err) {
  std::vector<std::string> names{op};
  std::map<std::string, JsonValue> byOp{{op, params}};
  return OpTable::instance().evaluate(names, buf, byOp, err);
}

double maxAbsDeltaCode(const ImageBuf& a, const ImageBuf& b) {
  if (a.w != b.w || a.h != b.h || a.data.size() != b.data.size()) return 1e18;
  double m = 0.0;
  for (size_t i = 0; i < a.data.size(); ++i) {
    const double d = std::fabs(static_cast<double>(a.data[i]) - static_cast<double>(b.data[i]));
    if (d > m) m = d;
  }
  return m;
}

bool sameBytes(const ImageBuf& a, const ImageBuf& b) {
  return a.w == b.w && a.h == b.h && a.data == b.data;
}

// 64×48 中灰夹具
ImageBuf makeGray(int w, int h, double v) {
  ImageBuf buf(w, h);
  const uint16_t c = static_cast<uint16_t>(v * 65535.0 + 0.5);
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = c;
  return buf;
}

JsonValue makeRadial(const std::string& id, double cx, double cy, double radius,
                     const char* combine, const JsonValue& local, bool enabled = true) {
  JsonValue geo = JsonValue::objectValue();
  geo.setItem("cx", cx);
  geo.setItem("cy", cy);
  geo.setItem("radius", radius);
  geo.setItem("ratio", 1.0);
  geo.setItem("feather", 0.0);  // 硬边：采样点因子非 0 即 1，fold 断言精确
  JsonValue m = JsonValue::objectValue();
  m.setItem("id", id);
  m.setItem("type", "radial");
  m.setItem("enabled", enabled);
  m.setItem("opacity", 100);
  m.setItem("amount", 100);
  m.setItem("combine", combine);
  m.setItem("geometry", geo);
  m.setItem("local", local);
  return m;
}

JsonValue exposureLocal(double ev) {
  JsonValue l = JsonValue::objectValue();
  l.setItem("exposure", ev);
  return l;
}

bool buildStack(const JsonValue& masks, const ImageBuf& base,
                lumina::plan::MaskStack* stack, std::string* err) {
  return lumina::plan::buildMaskStack(masks, base, stack, err);
}

JsonValue masksParam(const std::vector<JsonValue>& items) {
  JsonValue params = JsonValue::objectValue();
  JsonValue arr = JsonValue::arrayValue();
  for (const JsonValue& m : items) arr.addItem(m);
  params.setItem("masks", arr);
  return params;
}

// ---- fold 三态 ----
void testFoldThreeStates() {
  std::printf("fold 三态（replace/intersect/subtract，§3.1.3 伪码逐字）\n");
  const ImageBuf base = makeGray(64, 48, 0.5);
  JsonValue m1 = makeRadial("m1", 0.5, 0.5, 0.3, "replace", JsonValue::objectValue());
  JsonValue m2 = makeRadial("m2", 0.5, 0.5, 0.15, "replace", JsonValue::objectValue());

  // raw 值（硬边）：中心像素 raw=1；环带像素（d≈0.2）raw1=1、raw2=0
  double raw1c = 0.0, raw2c = 0.0, raw1r = 0.0, raw2r = 0.0;
  std::string err;
  lumina::plan::MaskSpec s1, s2;
  CHECK(lumina::plan::parseMaskSpec(m1, &s1, &err) && lumina::plan::parseMaskSpec(m2, &s2, &err),
        "m1/m2 MaskSpec 解析成功");
  lumina::plan::maskFactorAt(s1, base, 32, 24, &raw1c, &err);
  lumina::plan::maskFactorAt(s2, base, 32, 24, &raw2c, &err);
  lumina::plan::maskFactorAt(s1, base, 32, 14, &raw1r, &err);
  lumina::plan::maskFactorAt(s2, base, 32, 14, &raw2r, &err);
  std::printf("    raw: center(r1=%.0f,r2=%.0f) ring(r1=%.0f,r2=%.0f)\n", raw1c, raw2c, raw1r,
              raw2r);
  CHECK(raw1c == 1.0 && raw2c == 1.0 && raw1r == 1.0 && raw2r == 0.0,
        "硬边采样点落位正确（中心双覆盖、环带仅 m1）");

  const char* kCombines[3] = {"replace", "intersect", "subtract"};
  const double kExpectC[3] = {raw2c, raw1c * raw2c, raw1c * (1.0 - raw2c)};
  const double kExpectR[3] = {raw2r, raw1r * raw2r, raw1r * (1.0 - raw2r)};
  for (int i = 0; i < 3; ++i) {
    JsonValue m2i = makeRadial("m2", 0.5, 0.5, 0.15, kCombines[i], JsonValue::objectValue());
    JsonValue arr = JsonValue::arrayValue();
    arr.addItem(m1);
    arr.addItem(m2i);
    lumina::plan::MaskStack stack;
    if (!buildStack(arr, base, &stack, &err)) {
      CHECK(false, (std::string("buildMaskStack(") + kCombines[i] + ") 失败: " + err).c_str());
      continue;
    }
    CHECK(stack.planes.size() == 2 && stack.appliesLocal.size() == 2,
          (std::string("[") + kCombines[i] + "] planes 与 masks 一一对应").c_str());
    const float fCenter = stack.planes[1].f[24 * 64 + 32];
    const float fRing = stack.planes[1].f[14 * 64 + 32];
    std::printf("    %-9s planes[1]: center=%.4f（期望 %.4f） ring=%.4f（期望 %.4f）\n",
                kCombines[i], fCenter, kExpectC[i], fRing, kExpectR[i]);
    CHECK(std::fabs(fCenter - kExpectC[i]) < 1e-6 && std::fabs(fRing - kExpectR[i]) < 1e-6,
          (std::string("fold ") + kCombines[i] + " = §3.1.3 公式").c_str());
    CHECK(stack.appliesLocal[0] == true &&
              stack.appliesLocal[1] == (i != 2),
          (std::string("appliesLocal: subtract 为 false（") + kCombines[i] + "）").c_str());
  }
  // 首条 combine 无意义（按 replace）：单条 intersect 蒙版 = 其 raw
  {
    JsonValue only = makeRadial("only", 0.5, 0.5, 0.3, "intersect", JsonValue::objectValue());
    JsonValue arr = JsonValue::arrayValue();
    arr.addItem(only);
    lumina::plan::MaskStack stack;
    CHECK(buildStack(arr, base, &stack, &err), "单条 intersect buildMaskStack 成功");
    CHECK(std::fabs(stack.planes[0].f[24 * 64 + 32] - raw1c) < 1e-6,
          "首条 combine 无意义（f_0 = raw_0，§3.1.3）");
  }
  // disabled 项不参与 fold：生效权重 = 前一 fold 透传
  {
    JsonValue m2d = makeRadial("m2", 0.5, 0.5, 0.15, "intersect", JsonValue::objectValue(), false);
    JsonValue arr = JsonValue::arrayValue();
    arr.addItem(m1);
    arr.addItem(m2d);
    lumina::plan::MaskStack stack;
    CHECK(buildStack(arr, base, &stack, &err), "disabled 项 buildMaskStack 成功");
    CHECK(stack.appliesLocal[1] == false &&
              std::fabs(stack.planes[1].f[24 * 64 + 32] - raw1c) < 1e-6,
          "disabled 项透传前一 fold 且不施加 local");
  }
}

// ---- 相减不施加 local（施加纪律，走 MaskApplyOp 端到端）----
void testSubtractSkipsLocal() {
  std::printf("相减不施加 local（MaskApplyOp 端到端）\n");
  // 0.25 中灰（非 0.5）：+1EV 后为 0.5，replace 叠加 +5EV 才不至于饱和在 65535
  // （0.5 底 + 1EV 已顶格，m2 的任何正向 exposure 都看不出差异）。
  const ImageBuf base = makeGray(64, 48, 0.25);
  std::string err;
  JsonValue m1 = makeRadial("m1", 0.5, 0.5, 0.3, "replace", exposureLocal(1.0));
  JsonValue m2sub =
      makeRadial("m2", 0.5, 0.5, 0.15, "subtract", exposureLocal(5.0));  // local 必须被忽略
  JsonValue m2rep = makeRadial("m2", 0.5, 0.5, 0.15, "replace", exposureLocal(5.0));

  ImageBuf onlyM1 = base;
  ImageBuf withSub = base;
  ImageBuf withRep = base;
  const bool ok1 = runOne("mask.apply", masksParam({m1}), &onlyM1, &err);
  const bool ok2 = runOne("mask.apply", masksParam({m1, m2sub}), &withSub, &err);
  const bool ok3 = runOne("mask.apply", masksParam({m1, m2rep}), &withRep, &err);
  CHECK(ok1 && ok2 && ok3, "三组施加全部执行成功");
  if (!(ok1 && ok2 && ok3)) return;
  const double dSub = maxAbsDeltaCode(onlyM1, withSub);
  const double dRep = maxAbsDeltaCode(onlyM1, withRep);
  std::printf("    [+subtract local] max|Δ|=%.1f code；[+replace local] max|Δ|=%.1f code\n",
              dSub, dRep);
  CHECK(sameBytes(onlyM1, withSub), "subtract 蒙版携带 local：输出逐字节不变（不施加 local）");
  CHECK(dRep > 0.0, "同几何 replace 蒙版确实产生差异（subtract 的静默不是巧合）");

  // fold 传递：subtract 缩减后续 intersect 蒙版的生效面积（减面积不是死语义）
  JsonValue m3 = makeRadial("m3", 0.5, 0.5, 0.15, "intersect", JsonValue::objectValue());
  JsonValue arrA = JsonValue::arrayValue();
  arrA.addItem(m1);
  arrA.addItem(m3);
  JsonValue arrB = JsonValue::arrayValue();
  JsonValue m2sub2 = makeRadial("m2", 0.5, 0.5, 0.15, "subtract", JsonValue::objectValue());
  arrB.addItem(m1);
  arrB.addItem(m2sub2);
  arrB.addItem(m3);
  lumina::plan::MaskStack sa, sb;
  CHECK(buildStack(arrA, base, &sa, &err) && buildStack(arrB, base, &sb, &err),
        "fold 传递两组 buildMaskStack 成功");
  const float fA = sa.planes[1].f[24 * 64 + 32];  // m3 生效权重 = raw3
  const float fB = sb.planes[2].f[24 * 64 + 32];  // = raw1*(1-raw2)*raw3 = 0
  CHECK(fA > 0.5f && fB == 0.0f,
        "subtract 缩减后续 intersect 链的生效权重（中心 raw1*(1-raw2)*raw3=0）");
}

// ---- amount 正负语义 ----
void testAmountSemantics() {
  std::printf("amount 正负语义（负 amount = 局部调整反向，≠反相蒙版）\n");
  const ImageBuf base = makeGray(64, 48, 0.5);
  std::string err;
  JsonValue geoBase = JsonValue::objectValue();
  geoBase.setItem("cx", 0.5);
  geoBase.setItem("cy", 0.5);
  geoBase.setItem("radius", 0.3);
  geoBase.setItem("ratio", 1.0);
  geoBase.setItem("feather", 0.0);

  double signedDelta[3] = {0.0, 0.0, 0.0};  // +100 / -100 / +50
  const int kAmounts[3] = {100, -100, 50};
  for (int i = 0; i < 3; ++i) {
    JsonValue m = makeRadial("m1", 0.5, 0.5, 0.3, "replace", exposureLocal(0.8));
    m.setItem("amount", kAmounts[i]);
    ImageBuf got = base;
    if (!runOne("mask.apply", masksParam({m}), &got, &err)) {
      CHECK(false, (std::string("amount=") + std::to_string(kAmounts[i]) + " 执行失败").c_str());
      return;
    }
    const size_t i0 = (24 * 64 + 32) * 3;
    signedDelta[i] = static_cast<double>(got.data[i0]) - base.data[i0];
  }
  std::printf("    amount +100 → %+.1f code；-100 → %+.1f code；+50 → %+.1f code\n",
              signedDelta[0], signedDelta[1], signedDelta[2]);
  CHECK(signedDelta[0] > 0.0 && signedDelta[1] < 0.0,
        "amount<0 使局部调整反向（exposure 反向变暗）");
  CHECK(std::fabs(signedDelta[2]) > 0.0 && std::fabs(signedDelta[2]) < std::fabs(signedDelta[0]),
        "amount 减半 → 效果减弱（强度倍率语义）");
}

// ---- A5：不支持类型显式失败（零静默 + 无半写）----
void testUnsupportedTypesExplicit() {
  std::printf("A5 不支持类型显式失败（depth/ai*/未知，无回落画笔分支）\n");
  const ImageBuf base = makeGray(64, 48, 0.5);
  const char* kTypes[] = {"depth", "aiSky", "aiSubject", "aiBackground", "blob"};
  for (const char* t : kTypes) {
    JsonValue m = makeRadial("m1", 0.5, 0.5, 0.3, "replace", exposureLocal(1.0));
    m.setItem("type", t);
    ImageBuf got = base;
    std::string err;
    const bool ok = runOne("mask.apply", masksParam({m}), &got, &err);
    std::printf("    type=%-13s ok=%d err=\"%s\"\n", t, ok ? 1 : 0, err.c_str());
    CHECK(!ok && err.find("本版不支持") != std::string::npos,
          (std::string("type '") + t + "' 显式报错（本版不支持）").c_str());
    CHECK(sameBytes(got, base), (std::string("type '") + t + "' 失败时零像素改动").c_str());
    // maskFactorAt 同口径
    lumina::plan::MaskSpec spec;
    CHECK(lumina::plan::parseMaskSpec(m, &spec, &err), "parseMaskSpec 接受 JSON（求值层拒绝）");
    double f = -1.0;
    CHECK(!lumina::plan::maskFactorAt(spec, base, 32, 24, &f, &err) &&
              err.find("本版不支持") != std::string::npos,
          (std::string("maskFactorAt('") + t + "') 同口径显式失败").c_str());
  }
  // combine 非法同样显式失败（与 T2.5 gmaskFromJson 同口径）
  {
    JsonValue m = makeRadial("m1", 0.5, 0.5, 0.3, "replace", JsonValue::objectValue());
    m.setItem("combine", "xor");
    ImageBuf got = base;
    std::string err;
    CHECK(!runOne("mask.apply", masksParam({m}), &got, &err) &&
              err.find("combine") != std::string::npos,
        "非法 combine 显式失败");
  }
  // 未知类型枚举登记（typeFromString 拒绝 → kUnknown）
  {
    lumina::plan::MaskType mt = lumina::plan::MaskType::kUnknown;
    CHECK(!lumina::plan::maskTypeFromString("brush2", &mt),
          "maskTypeFromString 未知串返回 false");
  }
}

// ---- A6：JSON 往返字节稳定 + fold 平面等值 ----
void testJsonRoundTrip() {
  std::printf("A6 masks 参数 JSON 往返（字节稳定 + fold 平面等值）\n");
  const ImageBuf base = makeGray(64, 48, 0.5);
  std::vector<JsonValue> items;
  items.push_back(makeRadial("m1", 0.4, 0.5, 0.3, "replace", exposureLocal(0.6)));
  items.push_back(makeRadial("m2", 0.6, 0.5, 0.3, "intersect", exposureLocal(-0.4)));
  JsonValue m3 = makeRadial("m3", 0.5, 0.5, 0.2, "subtract", JsonValue::objectValue());
  m3.setItem("opacity", 80);
  m3.setItem("amount", -30);
  items.push_back(m3);
  const JsonValue params = masksParam(items);

  const std::string text1 = params.serialize();
  JsonValue parsed;
  std::string err;
  CHECK(JsonValue::parse(text1, &parsed, &err), "序列化→解析成功");
  const std::string text2 = parsed.serialize();
  CHECK(text1 == text2, "序列化→解析→再序列化 字节稳定（A6）");

  lumina::plan::MaskStack s1, s2;
  CHECK(buildStack(params.get("masks"), base, &s1, &err) &&
            buildStack(parsed.get("masks"), base, &s2, &err),
        "往返两侧 buildMaskStack 成功");
  bool planesEqual = s1.planes.size() == s2.planes.size();
  for (size_t i = 0; planesEqual && i < s1.planes.size(); ++i) {
    planesEqual = s1.planes[i].f == s2.planes[i].f;
  }
  CHECK(planesEqual, "往返后 fold 平面逐点等值");
  bool flagsEqual = s1.appliesLocal == s2.appliesLocal;
  CHECK(flagsEqual, "往返后 appliesLocal 等值");
}

// ---- fold 依赖数组序（subtract 顺序反例）----
void testFoldOrderSensitivity() {
  std::printf("fold 依赖数组序：[A,B(sub)] 与 [B,A(sub)] fold 平面不同\n");
  const ImageBuf base = makeGray(64, 48, 0.5);
  std::string err;
  JsonValue a = makeRadial("A", 0.35, 0.5, 0.25, "replace", JsonValue::objectValue());
  JsonValue b = makeRadial("B", 0.65, 0.5, 0.25, "subtract", JsonValue::objectValue());
  lumina::plan::MaskStack s1, s2;
  CHECK(buildStack(masksParam({a, b}).get("masks"), base, &s1, &err) &&
            buildStack(masksParam({b, a}).get("masks"), base, &s2, &err),
        "两组顺序 buildMaskStack 成功");
  double maxDiff = 0.0;
  for (size_t p = 0; p < s1.planes[1].f.size(); ++p) {
    maxDiff = std::max(maxDiff,
                       std::fabs(static_cast<double>(s1.planes[1].f[p]) -
                                 static_cast<double>(s2.planes[1].f[p])));
  }
  std::printf("    planes[1] 最大差 = %.4f\n", maxDiff);
  CHECK(maxDiff > 0.05, "交换数组序改变 fold 结果（顺序被真实执行，非装饰）");
}

}  // namespace

int main() {
  std::string err;
  if (!lumina::plan::registerBuiltinOps(&lumina::plan::OpTable::instance(), &err)) {
    std::printf("FATAL registerBuiltinOps: %s\n", err.c_str());
    return 2;
  }
  testFoldThreeStates();
  testSubtractSkipsLocal();
  testAmountSemantics();
  testUnsupportedTypesExplicit();
  testJsonRoundTrip();
  testFoldOrderSensitivity();
  std::printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
  return g_fail == 0 ? 0 : 1;
}
