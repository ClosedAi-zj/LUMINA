// 风光专项 · HDR：包围组识别（连拍间隔 + EV）+ 曝光融合（Mertens 风格）+ 产物入索引
// 设计要点：
//   1) 检测不落库、不缓存：photos 表里的 captured_at / seq_id / shutter / aperture / iso 就是全部依据，
//      每次现算保证「导入新片后立刻可见」，groupId 取组内最小 photoId 保证同一组每次调用都是同一个 id。
//   2) 合成是真融合：按帧亮度权重（对比度 × 饱和度 × 曝光良好度）加权，亮部吃欠曝帧、暗部吃过曝帧，
//      不是简单平均；autoAlign 做金字塔平移搜索（±32px），deghost 对偏离中位亮度的区域降权。
//   3) 产物必须真入索引：写 {home}/cache/composite/ 后 upsert 进 photos 表拿新 photoId，
//      图库里能直接看到、能进修图台（只写文件不入库等于用户看不到，属静默失败）。
#ifndef LUMINA_SPECIALTY_HDR_SERVICE_H_
#define LUMINA_SPECIALTY_HDR_SERVICE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/job_manager.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "imaging/imaging.h"
#include "json/json_value.h"
#include "raw/raw_decoder.h"
#include "rpc/dispatcher.h"

namespace lumina {
namespace specialty {

// ---- 图像落盘（HDR / 景深堆栈产物共用，禁止各处自己拼文件头）----
// 实现已集中到 specialty/compositing_common.{h,cpp}（命名空间 lumina::compositing），
// 由 HDR 与景深堆栈同时复用，见 compositing::writeRgb8Tiff / compositing::writeLinearRgbDng。

// 一个包围组（ipc-protocol §5.12 hdr.detectBracketGroups）
struct BracketGroup {
  int64_t groupId = 0;
  std::vector<int64_t> photoIds;
  double exposureStops = 0.0;
};

// 识别阈值：默认 3 秒窗口、≥1 档曝光差、一组最多 9 帧（真实相机包围最多 9 张）
struct BracketScanOptions {
  double maxGapSeconds = 3.0;
  double minStops = 1.0;
  int maxFrames = 9;
};

// 合成选项（ipc-protocol §5.12 hdr.composite options）
struct HdrOptions {
  bool deghost = true;
  bool autoAlign = true;
  std::string outputFormat = "dng";  // dng|tiff
};

class HdrService {
 public:
  HdrService(core::Config* config, db::Database* db, db::PhotoDao* photos,
             raw::RawDecoder* decoder);

  // {filter?, maxGapSeconds?, minStops?} → [{groupId, photoIds, exposureStops}]
  json::JsonValue detectBracketGroups(const json::JsonValue& params, rpc::RpcError* err);

  // {groupId, options:{deghost, autoAlign, outputFormat}} → {jobId}；完成 result {photoId}
  json::JsonValue composite(const json::JsonValue& params, JobManager* jobs,
                            rpc::RpcError* err);

  // JobManager 工作线程内执行：解码 → 对齐 → 融合 → 编码 → 入库
  void runComposite(const std::vector<int64_t>& photoIds, const HdrOptions& options,
                    JobContext& ctx);

  // 识别实现：detectBracketGroups 与 composite 共用同一份判定（保证 groupId 稳定）
  std::vector<BracketGroup> detect(const json::JsonValue& params, const BracketScanOptions& options,
                                   std::string* err);

 private:
  // 解码一帧：RAW/DNG 走 LibRaw，JPG/PNG 回落通用解码器（两条都是真解码）
  bool decodeFrame(const PhotoRecord& record, imaging::Image* out, std::string* err) const;
  // 产物文件名：hdr-<groupId>-<时间戳>.<ext>，重名自动加后缀
  std::string compositePath(int64_t groupId, const std::string& extension) const;

  core::Config* config_;
  db::Database* db_;
  db::PhotoDao* photos_;
  raw::RawDecoder* decoder_;
};

}  // namespace specialty
}  // namespace lumina

#endif  // LUMINA_SPECIALTY_HDR_SERVICE_H_
