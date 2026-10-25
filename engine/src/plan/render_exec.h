// T1.5：Plan 端到端执行入口（销毁三套实现后的唯一"渲染像素"路径）。
// 调用方：export/exporter.cpp（导出）、rpc develop.render（CPU 降级/断言）、
//         analysis.histogram（edited 直方图）。
// 禁止再造公式副本（架构 §2.2：CPU 唯一权威 = 本入口，GPU 只许逼近它）。
//
// 显示域 ↔ Plan 域转换口径 = 预览管线（renderer.ts imageToCodes / oracle93 encode8）：
//   进：8bit sRGB → srgbToLinear → log16Encode（码域）
//   出：log16Decode → linearToSrgb → floor(+0.5 + 确定性 TPDF)（8bit，与预览 encode8 同式）
#ifndef LUMINA_PLAN_RENDER_EXEC_H_
#define LUMINA_PLAN_RENDER_EXEC_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "imaging/image_buffer.h"
#include "imaging/imaging.h"
#include "json/json_value.h"
#include "plan/render_plan.h"

namespace lumina {
namespace plan {

// 8bit sRGB 显示图 → log16 Plan 输入缓冲（SceneLinearWorking 码域）。
// 要求 image 为 RGB8 交错（imaging::Image 契约），宽高 > 0。
imaging::ImageBuf displayToPlanBuf(const imaging::Image& image);

// log16 Plan 输出缓冲 → 8bit sRGB 显示图（含确定性 TPDF，与预览 encode8 同式）。
bool planBufToDisplay(const imaging::ImageBuf& buf, imaging::Image* out, std::string* err);

// EditParams（嵌套 JSON，sidecar 口径）→ 叶路径 kv（compilePlan 入参口径）。
// 叶路径口径 = param-leaves.json / opForParam：
//   basic.*（14 叶 + whiteBalance/temperature/tint）、calibration.*、detail.*、
//   geometry.{hasCrop,cropTop,cropBottom,cropLeft,cropRight,cropAngle}、
//   geometry.perspective.*、optics.*、curve.toneCurve{RGB,R,G,B}（数组原样）、
//   parametric.*、hsl.{band}.{hue,sat,lum}、colorGrade.*、masks（数组原样）。
// processVersion 为元数据不产出算子，跳过。未知顶层键忽略（零静默由调用方日志承担）。
bool flattenEditParams(const json::JsonValue& params,
                       std::vector<std::pair<std::string, json::JsonValue>>* out,
                       std::string* err);

// 端到端：EditParams → flatten → compilePlan → executePlan（首次调用线程安全注册算子）。
// planOut 可为 null（调用方不需要缓存键时）。任何失败写 err 返回 false，零静默。
bool runEditPlan(const json::JsonValue& editParams, imaging::ImageBuf* buf,
                 RenderPlan* planOut, std::string* err);

}  // namespace plan
}  // namespace lumina

#endif  // LUMINA_PLAN_RENDER_EXEC_H_
