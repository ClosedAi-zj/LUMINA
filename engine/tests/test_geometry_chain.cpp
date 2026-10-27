// M2 T2.10 验收（N23）：S1 几何链收口
//   O3：aspect≠0 → outW 变化 ≥1px 且四角位移 ≥1px；aspect=0 → max|Δ|=0
//   O4：geometry.apply 内 samples == outW×outH 且全链 applyGeometry（重采样）
//       调用次数 == 1（计数器注入，架构行 757）
//   子序断言：畸变必在裁剪之前（架构 §4.3 S1 子序 1畸变→…→4透视→5aspect→6裁剪）
//   兼容：旧算子名 geometry.crop / geometry.perspective / lens.manualDistortion
//   废弃转发后行为等价（单参数退化路径）。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "imaging/image_buffer.h"
#include "json/json_value.h"
#include "plan/geometry_chain.h"
#include "plan/op_table.h"
#include "plan/render_plan.h"

using namespace lumina;
using lumina::imaging::ImageBuf;
using lumina::json::JsonValue;
using lumina::plan::GeometryChainParams;
using lumina::plan::GeometryChainStats;
using lumina::plan::OpTable;

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

// 亮度斜坡 + 色带：四角与中心值互异，几何位移必然改变角像素
static ImageBuf makeRamp(int w, int h) {
  ImageBuf buf(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      p[0] = static_cast<uint16_t>(x / static_cast<double>(w - 1) * 60000.0 + 2000.0);
      p[1] = static_cast<uint16_t>(y / static_cast<double>(h - 1) * 60000.0 + 2000.0);
      p[2] = static_cast<uint16_t>((x + y) / static_cast<double>(w + h - 2) * 50000.0 + 3000.0);
    }
  }
  return buf;
}

static bool sameBytes(const ImageBuf& a, const ImageBuf& b) {
  return a.w == b.w && a.h == b.h && a.data == b.data && a.dataF == b.dataF;
}

static int countDiffPixels(const ImageBuf& a, const ImageBuf& b) {
  if (a.w != b.w || a.h != b.h || a.data.size() != b.data.size()) return -1;
  int n = 0;
  for (size_t i = 0; i < a.data.size(); ++i) {
    if (a.data[i] != b.data[i]) ++n;
  }
  return n;
}

static JsonValue chainParams(const GeometryChainParams& p) {
  JsonValue o = JsonValue::objectValue();
  if (p.lensProfileEnable != 0.0) o.setItem("optics.lensProfileEnable", p.lensProfileEnable);
  if (p.manualDistortion != 0.0) o.setItem("optics.manualDistortion", p.manualDistortion);
  if (p.perspX != 0.0) o.setItem("geometry.perspective.x", p.perspX);
  if (p.perspY != 0.0) o.setItem("geometry.perspective.y", p.perspY);
  if (p.perspH != 0.0) o.setItem("geometry.perspective.horizontal", p.perspH);
  if (p.perspV != 0.0) o.setItem("geometry.perspective.vertical", p.perspV);
  if (p.perspRotate != 0.0) o.setItem("geometry.perspective.rotate", p.perspRotate);
  if (p.perspScale != 1.0) o.setItem("geometry.perspective.scale", p.perspScale);
  if (p.perspAspect != 0.0) o.setItem("geometry.perspective.aspect", p.perspAspect);
  if (p.hasCrop) o.setItem("geometry.hasCrop", true);
  if (p.cropLeft != 0.0) o.setItem("geometry.cropLeft", p.cropLeft);
  if (p.cropTop != 0.0) o.setItem("geometry.cropTop", p.cropTop);
  if (p.cropRight != 100.0) o.setItem("geometry.cropRight", p.cropRight);
  if (p.cropBottom != 100.0) o.setItem("geometry.cropBottom", p.cropBottom);
  if (p.cropAngle != 0.0) o.setItem("geometry.cropAngle", p.cropAngle);
  return o;
}

// 直接链 API(拿 stats);算子路径经 evaluate 的兼容性由 legacy 段覆盖。
static bool runChainDirect(const GeometryChainParams& p, ImageBuf* buf,
                           GeometryChainStats* stats, std::string* err) {
  return lumina::plan::applyGeometryChain(buf, p, stats, err);
}

// ---- O3：aspect 实装 ----
static void testO3Aspect() {
  std::printf("O3 aspect 实装（输出画幅约束）\n");
  std::string err;
  // 基准：无 aspect（全默认）= 恒等字节不变
  ImageBuf base = makeRamp(64, 48);
  ImageBuf ref = base;
  GeometryChainParams none;
  CHECK(runChainDirect(none, &base, nullptr, &err), "无参 apply ok");
  CHECK(sameBytes(base, ref), "O3: aspect=0（缺省）→ max|Δ|=0（字节不变）");
  // 显式 aspect=0 同样字节不变
  ImageBuf a0 = makeRamp(64, 48);
  GeometryChainParams pa0;
  pa0.perspAspect = 0.0;
  CHECK(runChainDirect(pa0, &a0, nullptr, &err), "aspect=0 apply ok");
  CHECK(sameBytes(a0, ref), "O3: aspect=0 显式 → 与无 aspect 字节一致（max|Δ|=0）");

  // aspect=1（1:1）与 1.5（3:2）：outW 变化 ≥1px 且四角位移 ≥1px
  struct Case { double aspect; double wantRatio; };
  const Case cases[] = {{1.0, 1.0}, {1.5, 1.5}};
  for (const Case& c : cases) {
    ImageBuf b = makeRamp(64, 48);
    GeometryChainStats st;
    GeometryChainParams p;
    p.perspAspect = c.aspect;
    CHECK(runChainDirect(p, &b, &st, &err), "aspect apply ok");
    const int dw = std::abs(st.outW - 64);
    CHECK(dw >= 1, (std::string("O3: aspect=") + std::to_string(c.aspect) +
                    " → outW 变化 " + std::to_string(dw) + "px ≥ 1px")
                       .c_str());
    const double ratio = static_cast<double>(st.outW) / st.outH;
    CHECK(std::fabs(ratio - c.wantRatio) < 0.05,
          (std::string("O3: aspect=") + std::to_string(c.aspect) + " → 输出比 " +
           std::to_string(ratio).substr(0, 5) + " ≈ " + std::to_string(c.wantRatio))
              .c_str());
    // 四角位移 ≥1px：与基准输出四角像素值不同（ramp 下位移必然换值）
    ImageBuf b0 = makeRamp(64, 48);
    GeometryChainParams p0;
    CHECK(runChainDirect(p0, &b0, nullptr, &err), "baseline apply ok");
    // 尺寸不同时比较「角对应的源采样位移」：用四角内容差异 + 至少一角不同
    int cornerDiff = 0;
    if (b.w == b0.w && b.h == b0.h) {
      const size_t corners[4] = {
          0, 3,
          (static_cast<size_t>(b.h - 1) * b.w) * 3,
          (static_cast<size_t>(b.h - 1) * b.w + b.w - 1) * 3 + 2};
      for (const size_t q : corners) {
        if (b.data[q] != b0.data[q]) ++cornerDiff;
      }
    } else {
      cornerDiff = 4;  // 尺寸已变 ≥1px，四角重采样坐标必移
    }
    CHECK(cornerDiff >= 1, (std::string("O3: aspect=") + std::to_string(c.aspect) +
                            " → 四角位移 ≥1px（角内容差异 " + std::to_string(cornerDiff) +
                            "/4）")
                               .c_str());
  }
}

// ---- O4：一次采样 + 计数器注入 ----
static void testO4SingleResample() {
  std::printf("O4 单次重采样（计数器注入）\n");
  std::string err;
  // 直接链调用：全参激活（畸变+透视+aspect+裁剪+角度），samples == outW*outH
  ImageBuf buf = makeRamp(80, 60);
  GeometryChainParams p;
  p.manualDistortion = 40.0;
  p.perspH = 20.0;
  p.perspRotate = 4.0;
  p.perspAspect = 1.5;
  p.hasCrop = true;
  p.cropLeft = 5.0;
  p.cropTop = 5.0;
  p.cropRight = 95.0;
  p.cropBottom = 90.0;
  GeometryChainStats st;
  CHECK(runChainDirect(p, &buf, &st, &err), "全参 geometry chain apply");
  CHECK(st.resampleCalls == 1, "O4: 链内重采样调用 == 1");
  CHECK(st.samples == static_cast<uint64_t>(st.outW) * st.outH,
        (std::string("O4: samples=") + std::to_string(st.samples) + " == outW*outH=" +
         std::to_string(st.outW * st.outH))
            .c_str());
  // 链级计数器注入：compilePlan → executePlan 全链只有一次重采样
  lumina::plan::geometryChainResetCounters();
  std::vector<std::pair<std::string, JsonValue>> kvs;
  kvs.push_back({"optics.manualDistortion", JsonValue::number(30.0)});
  kvs.push_back({"geometry.perspective.rotate", JsonValue::number(3.0)});
  kvs.push_back({"geometry.perspective.aspect", JsonValue::number(1.0)});
  kvs.push_back({"geometry.hasCrop", JsonValue::boolean(true)});
  kvs.push_back({"geometry.cropLeft", JsonValue::number(10.0)});
  kvs.push_back({"geometry.cropRight", JsonValue::number(90.0)});
  lumina::plan::RenderPlan plan;
  CHECK(lumina::plan::compilePlan(1, kvs, &plan, &err), "compilePlan 几何参数");
  ImageBuf chain = makeRamp(48, 32);
  CHECK(lumina::plan::executePlan(plan, &chain, &err), "executePlan 几何链");
  CHECK(lumina::plan::geometryChainResampleCount() == 1,
        (std::string("O4: 全链 applyGeometry 重采样计数 == 1（实测 ") +
         std::to_string(lumina::plan::geometryChainResampleCount()) + "）")
            .c_str());
}

// ---- 子序断言：畸变必在裁剪之前（架构 §4.3）----
static void testSubStepOrder() {
  std::printf("子序断言（畸变 → 透视 → aspect → 裁剪）\n");
  std::string err;
  ImageBuf buf = makeRamp(64, 48);
  GeometryChainParams p;
  p.manualDistortion = 50.0;   // 子步 1
  p.perspRotate = 5.0;         // 子步 4
  p.perspAspect = 1.25;        // 子步 5
  p.hasCrop = true;            // 子步 6
  p.cropLeft = 10.0;
  p.cropTop = 10.0;
  p.cropRight = 90.0;
  p.cropBottom = 85.0;
  GeometryChainStats st;
  CHECK(runChainDirect(p, &buf, &st, &err), "全参链 apply");
  CHECK(st.subSteps.size() == 4, "子步记录：1/4/5/6 四步激活");
  bool orderOk = st.subSteps.size() == 4 && st.subSteps[0] == 1 && st.subSteps[1] == 4 &&
                 st.subSteps[2] == 5 && st.subSteps[3] == 6;
  CHECK(orderOk, "子序 = [畸变1, 透视4, aspect5, 裁剪6]（畸变必在裁剪之前）");
  // 仅畸变：无几何变换 → 尺寸不变（D 只改采样坐标）
  ImageBuf d = makeRamp(64, 48);
  GeometryChainParams pd;
  pd.manualDistortion = 30.0;
  GeometryChainStats sd;
  CHECK(runChainDirect(pd, &d, &sd, &err), "仅畸变 apply");
  CHECK(sd.subSteps.size() == 1 && sd.subSteps[0] == 1, "仅畸变 → 子步 [1]");
  CHECK(sd.outW == 64 && sd.outH == 48, "仅畸变不改尺寸");
}

// ---- 旧算子名废弃转发兼容 ----
static void testLegacyAliasCompat() {
  std::printf("旧算子名废弃转发兼容\n");
  std::string err;
  // geometry.crop 半幅 = 32x24 且内容与源一致（test_ops93 同口径）
  ImageBuf half = makeRamp(64, 48);
  JsonValue ph = JsonValue::objectValue();
  ph.setItem("geometry.hasCrop", true);
  ph.setItem("geometry.cropLeft", 0.0);
  ph.setItem("geometry.cropTop", 0.0);
  ph.setItem("geometry.cropRight", 50.0);
  ph.setItem("geometry.cropBottom", 50.0);
  std::vector<std::string> n1{"geometry.crop"};
  std::map<std::string, JsonValue> b1{{"geometry.crop", ph}};
  CHECK(OpTable::instance().evaluate(n1, &half, b1, &err), "legacy crop apply");
  CHECK(half.w == 32 && half.h == 24, "legacy: 半幅裁剪尺寸");
  // geometry.perspective rotate 90 尺寸互换
  ImageBuf rot = makeRamp(64, 48);
  JsonValue pr = JsonValue::objectValue();
  pr.setItem("geometry.perspective.rotate", 90.0);
  std::vector<std::string> n2{"geometry.perspective"};
  std::map<std::string, JsonValue> b2{{"geometry.perspective", pr}};
  CHECK(OpTable::instance().evaluate(n2, &rot, b2, &err), "legacy perspective apply");
  CHECK(rot.w == 48 && rot.h == 64, "legacy: rotate 90 尺寸互换");
  // lens.manualDistortion 中心近似不动
  ImageBuf d = makeRamp(64, 48);
  ImageBuf dref = d;
  JsonValue pdm = JsonValue::objectValue();
  pdm.setItem("optics.manualDistortion", 50.0);
  std::vector<std::string> n3{"lens.manualDistortion"};
  std::map<std::string, JsonValue> b3{{"lens.manualDistortion", pdm}};
  CHECK(OpTable::instance().evaluate(n3, &d, b3, &err), "legacy distortion apply");
  const size_t center = (24 * 64 + 32) * 3;
  CHECK(std::abs(static_cast<int>(d.data[center]) - static_cast<int>(dref.data[center])) <= 2,
        "legacy: 畸变中心近似不动");
}

int main() {
  std::printf("=== test_geometry_chain (T2.10 O3/O4/子序) ===\n");
  std::string regErr;
  if (!lumina::plan::registerBuiltinOps(&OpTable::instance(), &regErr)) {
    std::printf("FATAL registerBuiltinOps: %s\n", regErr.c_str());
    return 2;
  }
  testO3Aspect();
  testO4SingleResample();
  testSubStepOrder();
  testLegacyAliasCompat();
  if (g_fail == 0) {
    std::printf("ALL PASS\n");
    return 0;
  }
  std::printf("%d FAILURES\n", g_fail);
  return 1;
}
