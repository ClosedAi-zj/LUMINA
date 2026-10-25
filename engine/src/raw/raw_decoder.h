// LibRaw 解码入口：真解码 → sRGB 位图 / JPEG 落盘；RAW 元数据读取
#ifndef LUMINA_RAW_RAW_DECODER_H_
#define LUMINA_RAW_RAW_DECODER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "db/photo_dao.h"
#include "imaging/image_buffer.h"
#include "imaging/imaging.h"

namespace lumina {
namespace raw {

// 从 RAW/标准图像文件读出的元数据（写入 photos 表）
struct RawMeta {
  std::string make;
  std::string model;
  std::string lens;
  int iso = 0;
  double aperture = 0.0;   // f 值
  double shutter = 0.0;     // 秒
  int focalLength = 0;      // mm
  int64_t timestampMs = 0;  // epoch ms
  int width = 0;
  int height = 0;
  bool hasGps = false;
  double gpsLat = 0.0;
  double gpsLng = 0.0;
  double gpsAlt = 0.0;
};

// 引擎支持的输入格式（白名单，协议 2002 的依据）
bool isSupportedExtension(const std::string& ext);
std::string formatOfExtension(const std::string& ext);

// L1 自检：单张抽样失败详情（ipc-protocol v1.0.1 §5.7 raw.selfTest）
struct SelfTestFailure {
  int64_t photoId = 0;
  std::string error;
};

// L1 自检汇总：ok = 抽样 100% 成功且全部输出均值 ∈ [16,240]
struct SelfTestResult {
  bool ok = false;
  int sampled = 0;
  int succeeded = 0;
  double decodeMsP50 = 0.0;
  double decodeMsP95 = 0.0;
  std::vector<SelfTestFailure> failures;
};

class RawDecoder {
 public:
  // 真解码为 sRGB 位图；half=true 时降采样加速（大图/缩略图）
  bool decodeToBitmap(const std::string& path, bool half, imaging::Image* out, std::string* err);

  // M0 T0.2：真解码为 16bit 线性 scene-linear ImageBuf（log16 编码）。
  // output_bps=16、gamma 线性（gamm=1/1）、关自动提亮；blackPoint/whitePoint 记录
  // LibRaw color.black/color.maximum 供管线可重现。S0 解码唯一入口（M1 起）。
  bool decodeToImage16(const std::string& path, bool half, imaging::ImageBuf* out,
                       std::string* err);

  // 真解码并直接写 sRGB JPEG 到 outPath（预览/缩略图落盘）
  bool decodeToJpeg(const std::string& path, const std::string& outPath, bool half, int quality,
                    std::string* err);

  // 只读元数据（不解码像素）
  bool readMetadata(const std::string& path, RawMeta* out, std::string* err);

  // L1 判定（raw.selfTest）：抽样走 LibRaw open_file → unpack → dcraw_process →
  // copy_mem_image；ids 非空时按给定 id 集合抽样，否则从 RAW 格式照片随机抽 sample 张。
  // 要求 100% 解码成功且输出均值 ∈ [16,240]（非全黑/非全白）。
  bool selfTest(db::PhotoDao* photos, int sample, const std::vector<int64_t>& ids,
                SelfTestResult* out, std::string* err);
};

}  // namespace raw
}  // namespace lumina

#endif  // LUMINA_RAW_RAW_DECODER_H_
