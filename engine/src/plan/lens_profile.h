// M2 T2.11 镜头 profile 库接口（N5，架构 §3.4 / M2-架构设计.md:159-161）
//
// 职责：自建 profile JSON（engine/data/lens-profiles/*.json）加载 + 机身/镜头/焦距
// 匹配 + Brown-Conrady 畸变 / 横向 CA / 光学暗角求值。**只取数据不链接 lensfun 代码**
// （§6 依赖决策）；禁止 Adobe LCP/DCP（R1 红线，L4 双证）。
//
// 模型口径（与 geometry_chain S1 子步 1/2/3 消费方式一致）：
//   畸变（缺陷模型）：d(r) = 1 + k1·r² + k2·r⁴ + k3·r⁶，r = 像素半径 / 对角半径 ∈ [0,1]
//     采集语义：理想点 r_i 落在传感器 r_i·d(r_i) 处。校正（逆映射采样）= src 半径乘 d(r_dst)。
//     amount ∈ [−100,100]：+100 = 全量校正（乘 d），−100 = 反向放大（乘 1/d），0 = 恒等。
//   横向 CA：R/B 相对 G 的径向放大 scaleR/scaleB（采集语义同上）；校正 = 通道按
//     scale 采样回位。叶 #94 optics.chromaticAberration ∈ [−100,100] 为强度：
//     有效 scale = 1 + (scale−1)·(1 + leaf/100)（0=自动全量，−100=关，+100=2×）。
//   暗角：形状 v(r) = 1 + A·r² + B·r⁴ + C·r⁶（>1 表示镜头压角 1/v）。增益仅乘不采样：
//     gain = v(r)^s，s = amount/100；自动校正 s=+1（乘 v 还原），−100 = 施加缺陷（压角）。
#ifndef LUMINA_PLAN_LENS_PROFILE_H_
#define LUMINA_PLAN_LENS_PROFILE_H_

#include <mutex>
#include <string>
#include <vector>

#include "imaging/image_buffer.h"

namespace lumina {
namespace plan {

// 单焦距档系数（profile JSON focalBuckets[] 的一项）
struct LensFocalBucket {
  double focalMm = 0.0;
  double k1 = 0.0, k2 = 0.0, k3 = 0.0;  // Brown-Conrady 径向畸变（缺陷模型）
  double scaleR = 1.0, scaleB = 1.0;    // 横向 CA（R/B 相对 G 的径向放大）
  double vignA = 0.0, vignB = 0.0, vignC = 0.0;  // 光学暗角形状系数
};

struct LensProfile {
  std::string cameraMake, cameraModel;
  std::string lensMake, lensModel;
  std::string source;      // self-built | lensfun-db(CC-BY-SA-3.0)
  std::string attribution; // source=lensfun-db 时必填（同步进 NOTICE.md）
  std::vector<LensFocalBucket> buckets;  // 按 focalMm 升序
  bool valid() const { return !buckets.empty() && !cameraModel.empty() && !lensModel.empty(); }
  // 焦距 → 桶：两桶之间线性插值各系数；越界取端桶；focalMm<=0 取首桶
  LensFocalBucket bucketForFocal(double focalMm) const;
};

// profile 数据库：目录加载（*.json）+ 匹配。global() 单例惰性加载
// LUMINA_LENS_PROFILES_DIR 环境变量 / exe 旁 / 源码树 data 目录（CMake 注入宏）。
class LensProfileDb {
 public:
  static LensProfileDb& global();
  // 加载目录下全部 *.json（覆盖已加载内容）；单文件损坏跳过并记 err 拼接
  bool loadDir(const std::string& dir, std::string* err);
  size_t size() const;
  // camera.make+model 精确（大小写/空白归一化）→ lens.make+model 归一化匹配
  // → 焦距插值。未命中返回 false（零静默：调用方 identity + warning，不猜测）。
  bool match(const std::string& cameraMake, const std::string& cameraModel,
             const std::string& lensMake, const std::string& lensModel, double focalMm,
             LensProfile* out) const;

 private:
  mutable std::mutex mu_;
  std::vector<LensProfile> profiles_;
  bool globalTried_ = false;  // global() 惰性加载只试一次（避免每次 miss 重扫盘）
};

// ---- 模型求值（纯函数，L1/L2/L3 直接驱动）----

// 畸变：dst 归一化半径 r 的 src 半径乘数。amount −100..100（+100 全量校正）。
double lensDistortRadiusFactor(double r, double k1, double k2, double k3, double amount);

// 暗角：归一化半径 rNorm 的增益乘数（仅乘不采样）。amount −100..100。
double lensVignetteGain(double rNorm, double a, double b, double c, double amount);

// CA：profile scale + 叶强度 → 有效通道径向缩放。
double lensCaScale(double profileScale, double leafAmount);

// 无 profile 时的无参自动估计（架构 §3.4：边缘通道相关最大化，限幅 ±0.3%）。
// 均匀/平坦图返回 1.0（不猜测）。buf 需 valid()。
bool estimateLateralCa(const imaging::ImageBuf& buf, double* scaleR, double* scaleB);

// 未命中/未加载 warning（零静默：identity 但可观测；进程级，mutex 保护）
void lensProfileSetWarning(const std::string& w);
std::string lensProfileLastWarning();
void lensProfileClearWarning();

}  // namespace plan
}  // namespace lumina

#endif  // LUMINA_PLAN_LENS_PROFILE_H_
