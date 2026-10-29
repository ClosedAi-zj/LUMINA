// 风光专项 · 景深堆栈（focus stacking）：连拍识别（曝光冻结的连拍）+ 软焦点加权融合（全清晰）+ 产物入索引
// 设计要点：
//   1) 检测不落库、不缓存：photos 表的 captured_at / seq_id / aperture / iso / shutter / focalLength
//      就是全部依据，每次现算保证「导入新片后立刻可见」，groupId 取组内最小 photoId。
//   2) 与 HDR 包围的关键区别：景深堆栈是「曝光冻结」的连拍（各帧 aperture/iso/shutter/focalLength
//      完全相同），而 HDR 包围恰恰是「曝光不同」——所以按「曝光是否一致」天然分流两类素材。
//   3) 识别是真融合：以全图最清晰帧为对齐基准，按局部拉普拉斯幅度（对焦清晰度高者权重高）
//      软加权各帧，得到全清晰、过渡平滑的图像（不是简单平均、也不是硬切）。
//   4) 产物必须真入索引：写 {home}/cache/composite/ 后 upsert 进 photos 表拿新 photoId，
//      图库里能直接看到、能进修图台（只写文件不入库等于用户看不到，属静默失败）。
#ifndef LUMINA_SPECIALTY_FOCUS_STACK_SERVICE_H_
#define LUMINA_SPECIALTY_FOCUS_STACK_SERVICE_H_

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

// 景深堆栈选项（ipc-protocol §5.13 stack.composite options）
struct FocusStackOptions {
  bool autoAlign = true;          // 平移对齐（金字塔搜索 ±32px）
  std::string outputFormat = "dng";  // dng|tiff
  double smoothness = 4.0;        // 焦点权重指数（越高越「硬」选择、过渡越锐）
};

class FocusStackService {
 public:
  FocusStackService(core::Config* config, db::Database* db, db::PhotoDao* photos,
                    raw::RawDecoder* decoder);

  // {filter?, maxGapSeconds?} → [{groupId, photoIds, frameCount}]
  // 只识别「曝光冻结 + 时序接近 + 序列连续」的连拍（景深堆栈素材）。
  json::JsonValue detectStackGroups(const json::JsonValue& params, rpc::RpcError* err);

  // {groupId, options:{autoAlign, outputFormat, smoothness}} → {jobId}；完成 result {photoId}
  json::JsonValue composite(const json::JsonValue& params, JobManager* jobs, rpc::RpcError* err);

  // JobManager 工作线程内执行：解码 → 选基准 → 对齐 → 融合 → 编码 → 入库
  void runStack(const std::vector<int64_t>& photoIds, const FocusStackOptions& options,
                JobContext& ctx);

  // 识别实现：detectStackGroups 与 composite 共用同一份判定（保证 groupId 稳定）。
  // maxGapSeconds / maxFrames 均从 params 读取（有默认值），composite 重新识别时复用同一份 params。
  struct StackGroup {
    int64_t groupId = 0;
    std::vector<int64_t> photoIds;
  };
  std::vector<StackGroup> detect(const json::JsonValue& params, std::string* err);

 private:
  // 解码一帧：RAW/DNG 走 LibRaw，JPG/PNG 回落通用解码器（两条都是真解码）
  bool decodeFrame(const PhotoRecord& record, imaging::Image* out, std::string* err) const;
  // 产物文件名：stack-<groupId>-<时间戳>.<ext>，重名自动加后缀
  std::string compositePath(int64_t groupId, const std::string& extension) const;

  core::Config* config_;
  db::Database* db_;
  db::PhotoDao* photos_;
  raw::RawDecoder* decoder_;
};

}  // namespace specialty
}  // namespace lumina

#endif  // LUMINA_SPECIALTY_FOCUS_STACK_SERVICE_H_
