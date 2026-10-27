// T2.6 蒙版数学唯一权威（架构 §3.1.1 数据结构 / §3.1.3 fold 语义 / §1.4 蒙版规范）：
// - 因子求值：linear / radial / brush（256² 距离场）/ luminanceRange / colorRange
// - 布尔 fold：replace / intersect / subtract（§3.1.3 伪码逐字实现）
// - 唯一消费者 = MaskApplyOp（plan/ops/mask.cpp）；GPU mask pass（T2.7）逐字镜像本文件。
// 域纪律：
// - 坐标 = 裁后归一化 uv：u = (x+0.5)/w、v = (y+0.5)/h、v 向下（图像顶 = 0）
// - 范围蒙版基于进入蒙版阶段的像素（S4 末 buf，display 域 u = code/65535）
// - A5（§1.4）：depth/aiSky/aiSubject/aiBackground/未知类型一律显式报错
//   「本版不支持（v1.3）」，禁止回落画笔分支（零静默）
#ifndef LUMINA_PLAN_OPS_MASK_FIELD_H_
#define LUMINA_PLAN_OPS_MASK_FIELD_H_

#include <string>
#include <vector>

#include "graph/node_graph.h"  // graph::MaskCombine（T2.5 协议枚举，本层别名复用）
#include "imaging/image_buffer.h"
#include "json/json_value.h"

namespace lumina {
namespace plan {

// T2.5 已在 graph 命名空间定义 MaskCombine（sidecar 字符串口径的唯一事实源）；
// 本层以别名复用，禁止第二定义（避免两套枚举漂移）。
using MaskCombine = graph::MaskCombine;

// 蒙版类型（§3.1.1）。P0 实装前 5 种；depth/ai* 保留枚举位但求值显式报错
// 「本版不支持（v1.3）」（A5，§1.4 置灰不静默）。
enum class MaskType {
  Linear = 0,
  Radial,
  Brush,
  LuminanceRange,
  ColorRange,
  Depth,
  AiSky,
  AiSubject,
  AiBackground,
  kUnknown,
};

const char* maskTypeName(MaskType t);
// "linear"|"radial"|"brush"|"luminanceRange"|"colorRange"|"depth"|"aiSky"|
// "aiSubject"|"aiBackground" → 枚举；其余返回 false（调用方按 kUnknown 显式报错）
bool maskTypeFromString(const std::string& s, MaskType* out);

// = GraphMask 的求值视图（与 sidecar JSON 同构；架构 §3.1.1 原文）
struct MaskSpec {
  std::string id;
  MaskType type = MaskType::kUnknown;
  bool enabled = true;
  MaskCombine combine = MaskCombine::Replace;
  int opacity = 100;  // 0..100（越界截断）
  int amount = 100;   // -100..100（越界截断）；<0 = 局部调整反向（≠反相蒙版，§3.1.3）
  json::JsonValue geometry;  // linear: x1,y1,x2,y2 | radial: cx,cy,radius,ratio,feather
  json::JsonValue range;     // 亮度 {low,high,smooth} | 颜色 {hueCenter,hueWidth,satLow,satHigh,smooth}
  json::JsonValue strokes;   // brush: [{x,y,radius,flow,density,erase}]（density 本版不消费，同 maskField.ts）
  json::JsonValue local;     // 9 叶局部调整（§3.1.4）
};

// 单通道权重平面（uv 空间；尺寸 = 进入蒙版阶段的 buf，即裁后尺寸）
struct MaskPlane {
  int w = 0;
  int h = 0;
  std::vector<float> f;  // 0..1（fold 后生效权重）
};

struct MaskStack {
  std::vector<MaskPlane> planes;   // 与 masks[] 一一对应（disabled 项占位透传）
  std::vector<bool> appliesLocal;  // subtract 蒙版为 false（只参与 fold，不施加 local）
};

// sidecar JSON 单项 → MaskSpec。非对象 / combine 非法 → false + err；
// 数值越界（opacity/amount）截断到合法域；type 字符串未知 → kUnknown（求值时报错）。
bool parseMaskSpec(const json::JsonValue& j, MaskSpec* out, std::string* err);

// 生产入口（CPU 权威）。masks = sidecar 的 masks[] 数组原样；buf 必须是进入
// 蒙版阶段的当前像素（S4 末、裁后尺寸）。fold = §3.1.3 伪码逐字：
//   f_0 = raw(0)（首条 combine 无意义，按 replace）
//   Replace   : f_i = raw_i
//   Intersect : f_i = f_{i-1} * raw_i
//   Subtract  : f_i = f_{i-1} * (1 - raw_i)（只传递 fold，不施加 local）
// disabled 项不参与 fold（生效权重 = 前一 fold 原样透传，appliesLocal=false）。
// 任何不支持类型（depth/ai*/未知）→ false + err（A5 零静默）。
bool buildMaskStack(const json::JsonValue& masks, const imaging::ImageBuf& buf,
                    MaskStack* out, std::string* err);

// 单个蒙版的 raw 因子（GPU 镜像的参考实现与单测直接调用；不做 fold）。
// brush 每次调用重建 256² 场（参考实现口径；性能敏感路径走 buildMaskStack）。
bool maskFactorAt(const MaskSpec& m, const imaging::ImageBuf& buf, int x, int y,
                  double* out, std::string* err);

// 画笔场（§1.4：256² 距离场，与 app/src/modules/develop/webgl/maskField.ts
// 逐字镜像：FIELD_CELL=256、smoothstep5(r/2,r,d)、画笔组取 max / 擦除组乘法衰减、
// w 含 flow/100、erase 翻转处断开连线、density 不消费）。
// 输出按 8bit 量化（round(f*255)/255），与预览 LUMINANCE 纹理路径严格同值。
constexpr int kBrushFieldCell = 256;
void buildBrushField256(const json::JsonValue& strokes,
                        float out[kBrushFieldCell * kBrushFieldCell]);
// 双线性采样（与 maskField.ts sampleAtlas 同式；u/v 为画面归一化坐标，顶=0）
double sampleBrushField256(const float field[kBrushFieldCell * kBrushFieldCell],
                           double u, double v);

}  // namespace plan
}  // namespace lumina

#endif  // LUMINA_PLAN_OPS_MASK_FIELD_H_
