// T1.3 算子公共基座：ParamOp（参数注入）+ 像素数学共享辅助
// 每个文件一个注册入口；93 叶全部为 CPU oracle 真实现。
// 域纪律：S0–S2 算子在线性光 log16 码域（码值偏移 = 线性增益）；
//         S4–S6 算子作用在「感知码域」（code/65535 视为显示值）——
//         对 S3 ToneMap 前后均成立（log16 码即感知坐标，架构 §2 D1）。
#ifndef LUMINA_PLAN_OPS_OPS_H_
#define LUMINA_PLAN_OPS_OPS_H_

#include <cmath>
#include <string>
#include <vector>

#include "graph/node_graph.h"
#include "plan/op_table.h"

namespace lumina {
namespace plan {

bool registerBasicOps(OpTable* table, std::string* err);       // S0 WB / S2 基础色调（14 叶）
bool registerCurveOps(OpTable* table, std::string* err);       // S4 曲线（11 叶）
bool registerColorOps(OpTable* table, std::string* err);       // S4 HSL + 颜色分级（32 叶）
bool registerCalibrationOps(OpTable* table, std::string* err); // S2 校准（7 叶）
bool registerDetailOps(OpTable* table, std::string* err);      // S1.5 NR / S5 锐化（7 叶）
bool registerLensOps(OpTable* table, std::string* err);        // S1 镜头（2 叶）
bool registerGeometryOps(OpTable* table, std::string* err);    // S1 几何（13 叶）
bool registerEffectsOps(OpTable* table, std::string* err);     // S6 暗角/颗粒（6 叶）
bool registerMaskOps(OpTable* table, std::string* err);        // 随宿主节点的蒙版（1 叶）

// ---- 参数化算子基座 ----
class ParamOp : public Operator {
 public:
  explicit ParamOp(const char* n, Stage s) : n_(n), s_(s) {}
  const char* name() const override { return n_; }
  Stage stage() const override { return s_; }
  void setParams(const json::JsonValue& p) override { params_ = p; }
  bool checkBuf(imaging::ImageBuf* buf, std::string* err) const {
    if (!buf || !buf->valid()) {
      if (err) *err = std::string(n_) + ": invalid buffer";
      return false;
    }
    return true;
  }
  // 读参数（叶路径键；缺省/非数返回 def）
  double num(const char* key, double def) const {
    return params_.get(key).asDouble(def);
  }
  const json::JsonValue& params() const { return params_; }

 protected:
  const char* n_;
  Stage s_;
  json::JsonValue params_ = json::JsonValue::objectValue();
};

// identity 占位（mask.apply 等待 T2.2 的节点继续用）
class IdentityOp : public Operator {
 public:
  IdentityOp(const char* n, Stage s) : n_(n), s_(s) {}
  const char* name() const override { return n_; }
  Stage stage() const override { return s_; }
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!buf || !buf->valid()) {
      if (err) *err = std::string(n_) + ": invalid buffer";
      return false;
    }
    return true;  // identity：占位不改动像素
  }
 private:
  const char* n_;
  Stage s_;
};

struct OpSpec {
  const char* name;
  Stage stage;
};
inline bool registerBatch(OpTable* table, const OpSpec* specs, size_t n, std::string* err) {
  bool ok = true;
  for (size_t i = 0; i < n; ++i)
    ok = table->registerOp(std::make_shared<IdentityOp>(specs[i].name, specs[i].stage), err) && ok;
  return ok;
}

// 蒙版求值入口（§3.3）：layer = op(base)；out = mix(base, Blend(layer, base, mode),
// mask*opacity)。mask 平面为 per-pixel 0..1（null = 全 1）；opacity 0..100。
bool applyMasked(const Operator& op, const imaging::ImageBuf& base,
                 const std::vector<float>* mask, int opacity,
                 const std::string& blendMode, imaging::ImageBuf* out,
                 std::string* err);

// ---- 共享像素数学（ops/*.cpp 内复用；跨文件测试经公共入口） ----

// 码域亮度 0..1（感知坐标）：三通道码值均值 / 65535
inline double perceptualLum(const uint16_t* p) {
  return (static_cast<double>(p[0]) + p[1] + p[2]) / (3.0 * 65535.0);
}
// T1.5 float 工作链版本：经 codeAt 兼容 uint16 / float 两种缓冲
inline double perceptualLumAt(const imaging::ImageBuf& b, size_t i) {
  return (static_cast<double>(imaging::codeAt(b, i)) + imaging::codeAt(b, i + 1) +
          imaging::codeAt(b, i + 2)) /
         (3.0 * 65535.0);
}
// 平滑衰减（smoothstep 版）：edge0→0，edge1→1
inline double smooth01(double x, double edge0, double edge1) {
  if (edge1 <= edge0) return x >= edge1 ? 1.0 : 0.0;
  double t = (x - edge0) / (edge1 - edge0);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  return t * t * (3.0 - 2.0 * t);
}
inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
// 确定性伪随机（grain 用）：[0,1)，与像素坐标/种子绑定，跨运行稳定
inline double hashNoise(int x, int y, int seed) {
  unsigned int h = static_cast<unsigned int>(x) * 374761393u +
                   static_cast<unsigned int>(y) * 668265263u +
                   static_cast<unsigned int>(seed) * 2147483647u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h = h ^ (h >> 16);
  return static_cast<double>(h & 0xFFFFFF) / 16777216.0;
}

// 可分离盒式模糊（float 平面，半径 px；就地 out）
void boxBlurPlane(const float* src, float* dst, int w, int h, int radius);

// RGB↔HSL（0..1 域；h ∈ [0,1)）
void rgbToHsl(double r, double g, double b, double* h, double* s, double* l);
void hslToRgb(double h, double s, double l, double* r, double* g, double* b);

}  // namespace plan
}  // namespace lumina

#endif  // LUMINA_PLAN_OPS_OPS_H_
