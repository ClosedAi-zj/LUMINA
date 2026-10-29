// v1.3.0 · 景深堆栈（focus stacking）纯函数回归
//
// 不依赖 DB / RPC：直接验证共享合成辅助的两个核心函数：
//   T1 compositing::focusStackFrames —— 每个区域应选择"该区域最清晰"的那一帧：
//       帧 A 左半为高频竖条纹（清晰）、右半为平灰（模糊）；帧 B 相反；
//       帧 C 全平灰（应得 ~0 权重）。融合结果左半必须等于 A、不等于 B，右半相反。
//   T2 单帧输入必须原样返回（权重退化到参考帧，绝不输出黑图）。
//   T3 compositing::estimateShift —— 平移对齐精度：右移 (+5,+3) 必须精确检出；零位移必须得 (0,0)。
//
// 这些正是"景深堆栈"与"简单平均 / 硬切"的本质区别，也保证后续 align+融合管线有据可依。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imaging/imaging.h"
#include "specialty/compositing_common.h"

using namespace lumina;

namespace {

int gFailures = 0;

void check(bool condition, const std::string& name) {
  if (condition) {
    printf("  [PASS] %s\n", name.c_str());
  } else {
    printf("  [FAIL] %s\n", name.c_str());
    ++gFailures;
  }
}

// 64×64 合成帧：half = 32。左半 = 高频竖条纹（偶数列亮、奇数列暗），右半 = 平灰（128）。
// swap=true 时左右对调（即"另一帧"）。
imaging::Image makeStripeFrame(bool swap) {
  const int W = 64;
  const int H = 64;
  imaging::Image img;
  img.width = W;
  img.height = H;
  img.pixels.resize(static_cast<size_t>(W) * H * 3, 0);
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const bool leftHalf = (x < W / 2);
      const bool stripeRegion = swap ? !leftHalf : leftHalf;  // swap: 右半条纹
      uint8_t v;
      if (stripeRegion) {
        v = (x % 2 == 0) ? 255 : 0;  // 高频竖条纹
      } else {
        v = 128;  // 平灰（模糊）
      }
      const size_t di = (static_cast<size_t>(y) * W + x) * 3;
      img.pixels[di + 0] = v;
      img.pixels[di + 1] = v;
      img.pixels[di + 2] = v;
    }
  }
  return img;
}

// 全平灰帧（用作第 3 帧：应得 ~0 权重）
imaging::Image makeFlatFrame() {
  const int W = 64;
  const int H = 64;
  imaging::Image img;
  img.width = W;
  img.height = H;
  img.pixels.resize(static_cast<size_t>(W) * H * 3, 128);
  return img;
}

}  // namespace

int main() {
  printf("=== v1.3.0 · 景深堆栈纯函数回归 ===\n");

  const int W = 64;
  const int H = 64;
  const imaging::Image frameA = makeStripeFrame(/*swap=*/false);  // 左半条纹
  const imaging::Image frameB = makeStripeFrame(/*swap=*/true);   // 右半条纹
  const imaging::Image frameC = makeFlatFrame();                   // 全平灰

  // ---------------- T1：区域选择正确性 ----------------
  printf("\n[T1] focusStackFrames 区域选择：左半取 A、右半取 B\n");
  {
    std::vector<compositing::FrameView> frames;
    compositing::FrameView va{&frameA, 0, 0};
    compositing::FrameView vb{&frameB, 0, 0};
    compositing::FrameView vc{&frameC, 0, 0};
    frames.push_back(va);
    frames.push_back(vb);
    frames.push_back(vc);

    imaging::Image out;
    bool cancelled = false;
    compositing::focusStackFrames(frames, 4.0, nullptr, &out, &cancelled);

    check(!cancelled, "融合未被取消");
    check(out.width == W && out.height == H, "输出尺寸 64×64");
    check(out.pixels.size() == static_cast<size_t>(W) * H * 3, "输出像素缓冲完整");

    int maxDiffLeftA = 0;   // 左半：输出 vs A（应≈0）
    int maxDiffLeftB = 0;   // 左半：输出 vs B（应很大，B 在左半是平灰）
    int maxDiffRightA = 0;  // 右半：输出 vs A（应很大，A 在右半是平灰）
    int maxDiffRightB = 0;  // 右半：输出 vs B（应≈0）
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        const size_t di = (static_cast<size_t>(y) * W + x) * 3;
        const int ov = out.pixels[di];  // 单通道即可（三通道同值）
        const int av = frameA.pixels[di];
        const int bv = frameB.pixels[di];
        if (x < W / 2) {
          maxDiffLeftA = std::max(maxDiffLeftA, std::abs(ov - av));
          maxDiffLeftB = std::max(maxDiffLeftB, std::abs(ov - bv));
        } else {
          maxDiffRightA = std::max(maxDiffRightA, std::abs(ov - av));
          maxDiffRightB = std::max(maxDiffRightB, std::abs(ov - bv));
        }
      }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "左半输出匹配帧 A（max diff=%d ≤ 8）", maxDiffLeftA);
    check(maxDiffLeftA <= 8, buf);
    snprintf(buf, sizeof(buf), "左半输出不匹配平灰帧 B（max diff=%d > 8）", maxDiffLeftB);
    check(maxDiffLeftB > 8, buf);
    snprintf(buf, sizeof(buf), "右半输出匹配帧 B（max diff=%d ≤ 8）", maxDiffRightB);
    check(maxDiffRightB <= 8, buf);
    snprintf(buf, sizeof(buf), "右半输出不匹配平灰帧 A（max diff=%d > 8）", maxDiffRightA);
    check(maxDiffRightA > 8, buf);
  }

  // ---------------- T2：单帧原样返回 ----------------
  printf("\n[T2] focusStackFrames 单帧输入原样返回（退化到参考帧）\n");
  {
    std::vector<compositing::FrameView> frames;
    compositing::FrameView v{&frameA, 0, 0};
    frames.push_back(v);
    imaging::Image out;
    bool cancelled = false;
    compositing::focusStackFrames(frames, 4.0, nullptr, &out, &cancelled);
    check(!cancelled, "单帧融合未被取消");

    int maxDiff = 0;
    for (size_t i = 0; i < out.pixels.size(); ++i) {
      maxDiff = std::max(maxDiff, std::abs(static_cast<int>(out.pixels[i]) -
                                           static_cast<int>(frameA.pixels[i])));
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "单帧输出与输入逐像素一致（max diff=%d ≤ 1）", maxDiff);
    check(maxDiff <= 1, buf);
  }

  // ---------------- T3：estimateShift 对齐精度 ----------------
  printf("\n[T3] estimateShift 平移对齐精度\n");
  {
    const int GW = 64;
    const int GH = 64;
    compositing::GrayImage ref;
    ref.width = GW;
    ref.height = GH;
    ref.pixels.resize(static_cast<size_t>(GW) * GH, 0);
    // 高频随机图案（LCG）：保证对齐唯一、非周期，便于精确检出整数位移
    uint32_t seed = 0x12345u;
    for (size_t i = 0; i < ref.pixels.size(); ++i) {
      seed = seed * 1103515245u + 12345u;
      ref.pixels[i] = static_cast<uint8_t>((seed >> 8) & 0xFF);
    }

    // 目标 = ref 右移 (+5, +3)
    compositing::GrayImage tgt;
    tgt.width = GW;
    tgt.height = GH;
    tgt.pixels.assign(static_cast<size_t>(GW) * GH, 0);
    const int shiftX = 5;
    const int shiftY = 3;
    for (int y = 0; y < GH; ++y) {
      for (int x = 0; x < GW; ++x) {
        const int sx = x - shiftX;
        const int sy = y - shiftY;
        if (sx >= 0 && sx < GW && sy >= 0 && sy < GH) {
          tgt.pixels[static_cast<size_t>(y) * GW + x] =
              ref.pixels[static_cast<size_t>(sy) * GW + sx];
        }
      }
    }

    int dx = -1, dy = -1;
    compositing::estimateShift(ref, tgt, 32, &dx, &dy);
    char buf[128];
    snprintf(buf, sizeof(buf), "estimateShift 检出 (+5,+3)：实测 (%d,%d)", dx, dy);
    check(dx == shiftX && dy == shiftY, buf);

    // 零位移
    compositing::GrayImage tgt0 = ref;  // 精确复制
    int zx = -1, zy = -1;
    compositing::estimateShift(ref, tgt0, 32, &zx, &zy);
    snprintf(buf, sizeof(buf), "estimateShift 零位移得 (0,0)：实测 (%d,%d)", zx, zy);
    check(zx == 0 && zy == 0, buf);
  }

  printf("\n=== 结果：%s（失败 %d 项）===\n", gFailures == 0 ? "PASS" : "FAIL", gFailures);
  return gFailures == 0 ? 0 : 1;
}
