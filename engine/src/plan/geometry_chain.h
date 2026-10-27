// T2.10 S1 几何链（N3，架构 §4.3 / M2-架构设计.md:909-910）
// 单一算子 geometry.apply 的实现基座：把 lens（profile/畸变）与 geometry（透视/
// aspect/裁剪）合成为一次逆映射采样（O4：全链 applyGeometry 调用次数 == 1）。
// 内部子序（架构 §4.3 S1 子序，执行序由本模块固定）：
//   1 镜头 profile 畸变（k1–k3，T2.11 接库前 identity）
//   2 色差校正 CA（T2.11 新叶，占位 identity）
//   3 光学暗角校正（无叶，占位 identity）
//   4 透视/变换（perspective 7 叶）
//   5 aspect 宽高比（O3 实装：输出画幅强制 w/h = aspect，内容 x 向适配）
//   6 裁剪 + 拉直 + 唯一一次重采样
// 复合采样：dst 像素 → 逆单应 → 中间图坐标 → 畸变逆映射 D → 单次双线性采样
// （= 架构子序「畸变先于裁剪」的合成形式 src(D(H_inv(dx)))；值域 = log16 码域，
//  与 GPU FRAG_GEO / oracle93 applyGeometryStage 三边逐式一致）。
#ifndef LUMINA_PLAN_GEOMETRY_CHAIN_H_
#define LUMINA_PLAN_GEOMETRY_CHAIN_H_

#include <cstdint>
#include <string>
#include <vector>

#include "json/json_value.h"
#include "plan/ops/ops.h"

namespace lumina {
namespace plan {

// S1 全部几何参数（叶路径口径收拢；缺省值 = 恒等）
struct GeometryChainParams {
  // lens
  double lensProfileEnable = 0.0;  // optics.lensProfileEnable（选择器，T2.11 前 identity）
  double manualDistortion = 0.0;   // optics.manualDistortion（-100..100，k = v/100*0.3）
  // T2.11 镜头 profile 匹配输入（非用户叶：生产由 EXIF 元数据填充，测试直注）
  std::string lensProfileMake;     // optics.lensProfileMake
  std::string lensProfileModel;    // optics.lensProfileModel
  std::string lensProfileLensMake;   // optics.lensProfileLensMake
  std::string lensProfileLensModel;  // optics.lensProfileLensModel
  double lensProfileFocalMm = 0.0;   // optics.lensProfileFocalMm（<=0 取首桶）
  double chromaticAberration = 0.0;  // 叶 #94 optics.chromaticAberration（-100..100 强度）
  // geometry.perspective.*
  double perspX = 0.0;
  double perspY = 0.0;
  double perspH = 0.0;
  double perspV = 0.0;
  double perspRotate = 0.0;
  double perspScale = 1.0;   // 倍数语义（1 = 恒等），与 protocol/TransformBar 一致
  double perspAspect = 0.0;  // 目标宽高比 w/h（0 = 自由；UI ASPECTS 1:1/3:2/…）
  // geometry.crop.*
  bool hasCrop = false;
  double cropLeft = 0.0;
  double cropTop = 0.0;
  double cropRight = 100.0;
  double cropBottom = 100.0;
  double cropAngle = 0.0;
};

// 从算子参数集（叶路径键）收集（缺省补恒等值）
GeometryChainParams collectGeometryChainParams(const json::JsonValue& params);

// O4 计数器（架构行 757：「全链 applyGeometry 调用次数 == 1（计数器注入）」）。
// 进程级累计；测试用 reset → run → check 三段式断言。
void geometryChainResetCounters();
int geometryChainResampleCount();

struct GeometryChainStats {
  uint64_t samples = 0;       // 本链采样次数（恒 == outW*outH，O4）
  int outW = 0;
  int outH = 0;
  int resampleCalls = 0;      // 本链重采样调用数（恒 1）
  // 实际执行的子步号（按执行序；1/4/5/6 激活时记录，2/3 占位无叶不记录）
  std::vector<int> subSteps;
  // T2.11：profile 命中与零静默告知（未命中 → identity + warning，不猜测）
  bool profileHit = false;
  std::string warning;  // 未命中 / 数据库未加载等显式告知（空 = 无）
};

// 就地变换：buf 进（源全图）→ 几何链出（可能改尺寸）。全参恒等时字节不变。
bool applyGeometryChain(imaging::ImageBuf* buf, const GeometryChainParams& p,
                        GeometryChainStats* statsOut, std::string* err);

}  // namespace plan
}  // namespace lumina

#endif  // LUMINA_PLAN_GEOMETRY_CHAIN_H_
