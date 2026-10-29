// perf: benchmark develop.render CPU pipeline cost at 1024/1280/1600
//
// MEASUREMENT ONLY — does NOT touch app/ and does NOT change the develop.render
// contract. It replicates the exact CPU path that `develop.render` runs
// (engine/src/rpc/methods.cpp:528) on the engine side, minus the JSON-RPC
// framing / I/O:
//
//   decode -> resize(longEdge) -> displayToPlanBuf -> runEditPlan -> planBufToDisplay
//
// The resize call mirrors methods.cpp:566-578 EXACTLY:
//   targetWidth = longEdge
//   targetHeight = longEdge * height / width   (thumbScaledHeight, integer math)
//
// ASSUMPTION (stated per task brief): no decodable fixture image
// (JPEG/PNG/RAW) exists under /d/LUMINA/samples or /d/LUMINA/engine (only
// /d/LUMINA/samples/rw2 RAW files, which would require the libraw decode path
// and are not used here). We therefore synthesize an in-memory 4000x6000 RGB
// `imaging::Image` directly and run the plan pipeline on it. The measured cost
// is the per-render CPU work at each output size, which is what the lag fix
// (cap longEdge to 1280 during interaction) is meant to reduce.
//
// Build: linked into the engine test set (see CMakeLists.txt). Run the binary
// directly; it prints avg/min ms per size.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "imaging/imaging.h"
#include "imaging/image_buffer.h"
#include "json/json_value.h"
#include "plan/render_exec.h"

using lumina::imaging::Image;
using lumina::imaging::ImageBuf;
using lumina::json::JsonValue;
using lumina::plan::RenderPlan;

// Exactly replicate develop.render's resize (engine/src/rpc/methods.cpp:566-578).
static void resizeLikeDevelop(const Image& src, int longEdge, Image* dst) {
  const int tw = longEdge;
  int th = longEdge * src.height / src.width;  // thumbScaledHeight, integer math
  if (th <= 0) th = longEdge;
  (void)lumina::imaging::resizeImage(src, dst, tw, th);
}

// Deterministic synthetic 8-bit RGB so ops (curves/sharpen/vignette) do real work.
static Image makeSynthetic(int w, int h) {
  Image img;
  img.width = w;
  img.height = h;
  img.pixels.resize(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 3;
      img.pixels[i + 0] = static_cast<uint8_t>((x * 255) / (w - 1));
      img.pixels[i + 1] = static_cast<uint8_t>((y * 255) / (h - 1));
      img.pixels[i + 2] = static_cast<uint8_t>(((x + y) * 255) / (w + h - 2));
    }
  }
  return img;
}

int main() {
  // Realistic, mostly-neutral EditParams with a few non-zero tone/exposure ops
  // (mirrors what the develop module sends while dragging sliders).
  static const char* kParams = R"({
    "processVersion": "15.4",
    "whiteBalance": "asShot",
    "exposure": 0.3,
    "contrast": 8,
    "highlights": -15,
    "shadows": 10,
    "white": 5,
    "black": -5,
    "clarity": 12,
    "vibrance": 8,
    "saturation": 0,
    "toneCurveRGB": [{"x":0,"y":0},{"x":64,"y":52},{"x":128,"y":126},{"x":192,"y":190},{"x":255,"y":255}],
    "hsl": {"blue": {"hue": 0, "sat": 15, "lum": 0}},
    "optics": {"vignetteAmount": -20, "vignetteMidpoint": 50},
    "detail": {"sharpness": 25, "sharpenRadius": 1, "sharpenDetail": 25,
               "luminanceSmoothing": 0, "colorNR": 0},
    "colorGrade": {"shadowHue": 200, "shadowSat": 15, "midtoneHue": 0, "midtoneSat": 0,
                   "highlightHue": 0, "highlightSat": 0, "blend": 60, "balance": 0}
  })";
  JsonValue params;
  std::string perr;
  if (!JsonValue::parse(kParams, &params, &perr)) {
    std::printf("PARAM PARSE FAIL: %s\n", perr.c_str());
    return 2;
  }

  // Native photo size a RAW decode would produce (landscape here; develop.render
  // caps targetWidth = longEdge, height scales down proportionally).
  const int NATIVE_W = 4000, NATIVE_H = 6000;
  const Image native = makeSynthetic(NATIVE_W, NATIVE_H);
  std::printf(
      "input: synthesized %dx%d RGB (ASSUMPTION: no decodable fixture under "
      "/d/LUMINA/samples or /d/LUMINA/engine; using in-memory 4000x6000)\n",
      NATIVE_W, NATIVE_H);

  const int sizes[] = {640, 1024, 1280, 1600};
  const int kIters = 15;

  for (int longEdge : sizes) {
    // Warm-up: pays op registration / first-call init (std::call_once in runEditPlan).
    Image warm;
    resizeLikeDevelop(native, longEdge, &warm);
    ImageBuf wbuf = lumina::plan::displayToPlanBuf(warm);
    RenderPlan wplan;
    std::string we;
    if (!lumina::plan::runEditPlan(params, &wbuf, &wplan, &we)) {
      std::printf("WARMUP FAIL le=%d: %s\n", longEdge, we.c_str());
      return 3;
    }
    Image wout;
    if (!lumina::plan::planBufToDisplay(wbuf, &wout, &we)) {
      std::printf("WARMUP FAIL2 le=%d: %s\n", longEdge, we.c_str());
      return 3;
    }

    double resizeMs = 0, resizeMin = 1e9;
    double planMs = 0, planMin = 1e9;
    for (int it = 0; it < kIters; ++it) {
      Image s;
      auto r0 = std::chrono::high_resolution_clock::now();
      resizeLikeDevelop(native, longEdge, &s);
      auto r1 = std::chrono::high_resolution_clock::now();
      const double rms = std::chrono::duration<double, std::milli>(r1 - r0).count();
      resizeMs += rms;
      if (rms < resizeMin) resizeMin = rms;

      auto p0 = std::chrono::high_resolution_clock::now();
      ImageBuf b = lumina::plan::displayToPlanBuf(s);
      RenderPlan p;
      std::string e;
      if (!lumina::plan::runEditPlan(params, &b, &p, &e)) {
        std::printf("RUN FAIL le=%d it=%d: %s\n", longEdge, it, e.c_str());
        return 3;
      }
      Image o;
      if (!lumina::plan::planBufToDisplay(b, &o, &e)) {
        std::printf("RUN FAIL2 le=%d it=%d: %s\n", longEdge, it, e.c_str());
        return 3;
      }
      auto p1 = std::chrono::high_resolution_clock::now();
      const double pms = std::chrono::duration<double, std::milli>(p1 - p0).count();
      planMs += pms;
      if (pms < planMin) planMin = pms;
    }

    const int outW = longEdge;
    const int outH = longEdge * NATIVE_H / NATIVE_W;
    const double totalAvg = (resizeMs + planMs) / kIters;
    const double totalMin = resizeMin + planMin;
    std::printf(
        "longEdge=%d -> out %dx%d (%.2f Mpx) | resize(avg)=%.2fms(min=%.2f) "
        "plan(avg)=%.2fms(min=%.2f) | TOTAL(avg)=%.2fms(min=%.2f) | n=%d\n",
        longEdge, outW, outH,
        static_cast<double>(outW) * outH / 1e6,
        resizeMs / kIters, resizeMin, planMs / kIters, planMin, totalAvg, totalMin,
        kIters);
  }
  return 0;
}
