// T1.1：NodeGraph 数据结构（架构 §3，决策 D2/D3）
// - 节点 = 参数组粒度（用户拍板，不得再议）：{id,type,order,enabled,opacity,blendMode,mask?,params}
// - 8 类节点与 R6 右栏 8 组严格 1:1（basic/curve/color/detail/lens/geometry/effects/calibration）
// - 主链 = nodes[] 有序数组（单输入单输出，LR 语义）；DAG 预留不启用
// - params 用保序 JsonValue 承载（与 sidecar JSON 同构，序列化字节稳定）
#ifndef LUMINA_GRAPH_NODE_GRAPH_H_
#define LUMINA_GRAPH_NODE_GRAPH_H_

#include <string>
#include <vector>

#include "json/json_value.h"

namespace lumina {
namespace graph {

enum class NodeType {
  Basic = 0,        // 白平衡(2) + 基础(12) = 14 叶
  Curve = 1,        // 复合 + 分通道(3) + 参数曲线(7) = 11 叶
  Color = 2,        // HSL 8 带(24) + 颜色分级(8) = 32 叶（分离色调归此）
  Detail = 3,       // 锐化(4) + 降噪(3) = 7 叶
  Lens = 4,         // 配置文件(1) + 畸变(1) = 2 叶
  Geometry = 5,     // 裁剪(5) + 拉直(1) + 透视(7) = 13 叶
  Effects = 6,      // 暗角(3) + 颗粒(3) = 6 叶
  Calibration = 7,  // 阴影/红/绿/蓝原色(7) = 7 叶
  kCount,
};

// 节点类型字符串（sidecar JSON 口径，不得增减改名）
const char* nodeTypeName(NodeType t);
bool nodeTypeFromString(const std::string& s, NodeType* out);

// T2.5 协议 v3：蒙版布尔组合方式（架构 §3.1.3 fold 语义 + §3.5 类图）。
// fold 只依赖「数组序 + combine」，不依赖 GPU 槽位序：
//   Replace   : f_i = rawFactor(mask_i)          （首条固定按 replace）
//   Intersect : f_i = f_{i-1} * rawFactor(mask_i)
//   Subtract  : f_i = f_{i-1} * (1 - rawFactor(mask_i))，只减面积不施加 local
enum class MaskCombine {
  Replace = 0,
  Intersect = 1,
  Subtract = 2,
};

// sidecar JSON 字符串口径（"replace" | "intersect" | "subtract"，不得增减改名）
const char* maskCombineName(MaskCombine c);
bool maskCombineFromString(const std::string& s, MaskCombine* out);

struct NodeMask {
  std::string maskId;
  bool invert = false;
  double feather = 0.0;
};

struct GraphNode {
  std::string id;
  NodeType type = NodeType::Basic;
  int order = 0;
  bool enabled = true;
  int opacity = 100;                 // 0..100
  std::string blendMode = "normal";  // normal|multiply|screen|overlay|softLight
  bool hasMask = false;
  NodeMask mask;
  json::JsonValue params = json::JsonValue::objectValue();  // 保序
};

struct GraphMask {
  std::string id;
  std::string type;      // radial|linear|gradient|brush|range...
  bool enabled = true;
  int opacity = 100;     // 0..100
  int amount = 100;      // 强度 -100..100；<0 = 局部调整反向（≠反相蒙版，§3.1.3）
  MaskCombine combine = MaskCombine::Replace;  // v3 新增；v2 读端缺省 replace（同值语义）
  json::JsonValue geometry = json::JsonValue::nullValue();
  json::JsonValue range = json::JsonValue::nullValue();
  json::JsonValue strokes = json::JsonValue::nullValue();
};

struct NodeGraph {
  int schemaVersion = 3;  // T2.5：默认 3（v3 = combine + 94 叶口径）
  std::vector<GraphNode> nodes;
  std::vector<GraphMask> masks;

  // v2 与 v3 都合法（v2 读端向后可读，不提供 v3→v2 降级但必须能读）
  bool valid() const {
    return (schemaVersion == 2 || schemaVersion == 3) && !nodes.empty();
  }
};

// 混合模式合法集合
bool isBlendModeValid(const std::string& m);

// 默认图构造：无编辑照片 = identity 全链。
// 8 个全局节点（每种 type 一个），params 全默认值、enabled=true、无蒙版、无 masks。
NodeGraph defaultGraph();

// 节点类型 → 默认参数对象（保序 JsonValue；与 §4 表单位一致，同值直写 crs:）
json::JsonValue defaultParamsFor(NodeType t);

}  // namespace graph
}  // namespace lumina

#endif  // LUMINA_GRAPH_NODE_GRAPH_H_
