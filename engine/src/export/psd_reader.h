// M3-01b / M3-03：PSD 解析（与 psd_writer 同规范口径：version 1 / RGB / 8·16bit）
//
// 用途有三：
//  ① 自检：把我们写出的 PSD 读回来做结构与像素断言（不依赖第三方）；
//  ② 未知 tagged block 收集：'8BIM'/'8B64' 整块留下来，供写出时**字节原样**回写
//     （M3-01 判据③「第三方 PSD 读→写往返字节一致」）；
//  ③ M3-03 外来 PSD 判定：没有任何 'lumi' 快照块的 PSD = 外来文件 → 降级为像素层
//     并显式告知（hasLuminaSnapshot=false），绝不静默当作可编辑文档。
//
// 不支持的情况一律明确报错（PSB / CMYK / Lab / 索引色 / 32bit），不半猜半读。
#ifndef LUMINA_EXPORT_PSD_READER_H_
#define LUMINA_EXPORT_PSD_READER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "export/psd_writer.h"
#include "imaging/imaging.h"

namespace lumina {
namespace psd {

struct ReadLayer {
  std::string name;
  int top = 0;
  int left = 0;
  int width = 0;
  int height = 0;
  uint8_t opacity = 255;
  bool visible = true;
  std::string blendKey = "norm";
  /** 交错 RGB（8bit：w*h*3；16bit：w*h*6，每样本大端） */
  std::vector<uint8_t> rgb;
  /** 用户蒙版（空 = 该层无蒙版） */
  std::vector<uint8_t> mask;
  /** 'lumi' 块内容（NodeGraph 快照 JSON）；空 = 该层无快照 */
  std::string luminaSnapshot;
  /** 该层的未知块（signature + key + data），写出时原样回写 */
  std::vector<UnknownBlock> unknown;
};

struct PsdInfo {
  int width = 0;
  int height = 0;
  int depth = 8;
  int channels = 3;
  /** 自底向上（layers[0] = 最底层） */
  std::vector<ReadLayer> layers;
  /** 合并图（交错 RGB） */
  std::vector<uint8_t> composite;
  /** 是否含我方 lumina: 节点快照（M3-03 降级判据） */
  bool hasLuminaSnapshot = false;
  /** 收集到的未知块总数（用于往返断言） */
  int unknownBlockCount = 0;
};

/** 解析 PSD。任何不自洽（截断、越界、不支持的格式）都返回 false + 原文错误。 */
bool readPsd(const std::string& path, PsdInfo* out, std::string* err);

/** M3-03：把 PSD 合并图解码为 imaging::Image（RGB，8bit；16bit 样本缩放为 8bit）。
 *  用于「打开 PSD」时把任意 PSD（外来或本方）先作为合并像素层呈现 —— 即「降级为像素层」。
 *  不依赖任何可编辑信息（节点快照），因此外来 PSD 也能安全显示。 */
bool decodePsdComposite(const std::string& path, imaging::Image* out, std::string* err);

}  // namespace psd
}  // namespace lumina

#endif  // LUMINA_EXPORT_PSD_READER_H_
