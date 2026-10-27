// M3-01：PSD 写出（Adobe Photoshop File Formats 规范，version 1 / RGB）
//
// 设计取舍（写进代码而不是散落在文档里）：
// - **只做 version 1（PSD，非 PSB）**：PSB 的所有长度字段为 8 字节， ours 用 4 字节；
//   超过 30000px 或 2GB 的画幅会明确报错，绝不静默写坏文件。
// - **图层 = NodeGraph 节点，存「应用到该节点为止的累积结果」**：
//   这样 PSD 顶层可见 = 最终效果，逐层往下隐藏 = 回退中间状态；合成图层
//   （merged composite）= 顶层像素，与我们的渲染同源，M3-02 才有可比对象。
//   （另一种做法「每层存该节点的独立效果」会让叠加结果 ≠ 最终效果，PS 里观感错误。）
// - **未知 tagged block 原样回写**：读第三方 PSD 时把 '8BIM'/'8B64' 块整块
//   （signature + key + length + data + padding）留下来，写出时字节不动。
// - **padding**：Adobe 规范是 pad 到偶数（round2），我们自己写的块把数据长度补到
//   4 的倍数，这样 round2 与常见实现的 round4 等价，第三方解析不会错位。
// - 压缩支持 Raw(0) 与 RLE(1，PackBits)；RLE 行计数表为 u16（PSD，非 PSB）。
#ifndef LUMINA_EXPORT_PSD_WRITER_H_
#define LUMINA_EXPORT_PSD_WRITER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace lumina {
namespace psd {

/** 未知（或我们不解释的）tagged block：原样回写，字节不动 */
struct UnknownBlock {
  std::string signature;        // "8BIM" 或 "8B64"
  std::string key;              // 4 字符，如 "lumi" / "luni" / "lyid"
  std::vector<uint8_t> data;    // length 字段所指的数据（不含 key 与 length）
};

/** PSD 图层（通道顺序固定 R,G,B[,用户蒙版]） */
struct Layer {
  std::string name;
  int top = 0;
  int left = 0;
  int width = 0;
  int height = 0;
  /** 不透明度 0..255（255 = 完全不透明；PSD 记录就是这个口径，不是百分比） */
  uint8_t opacity = 255;
  /** false = 隐藏（PSD flags bit1 = hide，取反写入） */
  bool visible = true;
  /** 混合模式 4 字符键（"norm" / "mul " / "scrn" / "over" / "sLit" / "hLit" / "lite" / "dark"） */
  std::string blendKey = "norm";
  /** RGB 像素，长度 = width*height*3*(depth/8)；行优先，无行 padding */
  std::vector<uint8_t> rgb;
  /** 可选用户蒙版（单通道，0=全遮蔽 255=全显示，长度 = width*height*(depth/8)）；空 = 无蒙版 */
  std::vector<uint8_t> mask;
  /** lumina: NodeGraph 快照（JSON 文本）。非空时写成 '8BIM'+"lumi" 块。 */
  std::string luminaSnapshot;
  /** 原样回写的未知块（读第三方 PSD 时收集） */
  std::vector<UnknownBlock> unknown;
};

struct WriteOptions {
  int width = 0;
  int height = 0;
  int depth = 8;             // 8 | 16
  bool rle = true;           // true = RLE(PackBits)，false = Raw
  bool writeComposite = true;  // 是否写合并图（PS 兼容建议写；M3-02 对拍依赖它）
  /** 分辨率资源（Image Resource 0x03ED），默认 72dpi；0 = 不写该资源 */
  int dpi = 72;
};

struct Document {
  /** 图层自底向上：layers[0] 是最底层，最后一个是顶层 */
  std::vector<Layer> layers;
  /** 合并图（RGB，尺寸必须等于 WriteOptions 的 width/height） */
  std::vector<uint8_t> composite;
};

/** 写出 PSD。任一入参不自洽（尺寸/像素长度/位深）都返回 false + 原文错误，不写半成品文件。 */
bool writePsd(const std::string& path, const Document& doc, const WriteOptions& opts,
              std::string* err);

/** PackBits 编/解码（PSD RLE 就是它）。单独暴露是为了被单元测试直接打，
 *  不靠"写出再读回"间接验证——编码写错了在往返里会被自证为对。 */
void packBitsEncode(const uint8_t* src, int len, std::vector<uint8_t>* out);
bool packBitsDecode(const uint8_t* src, int len, std::vector<uint8_t>* out, int expected);

/** 大端写辅助（PSD 全大端；x86 上必须逐字节写，不能直接 memcpy） */
void putU16BE(std::vector<uint8_t>* out, uint16_t v);
void putU32BE(std::vector<uint8_t>* out, uint32_t v);
void putI16BE(std::vector<uint8_t>* out, int16_t v);
void putI32BE(std::vector<uint8_t>* out, int32_t v);

/** 混合模式名 → PSD 4 字符键（未收录的模式回退 "norm" 并记 warning，不静默） */
std::string blendKeyFromName(const std::string& name, std::string* warning);

}  // namespace psd
}  // namespace lumina

#endif  // LUMINA_EXPORT_PSD_WRITER_H_
