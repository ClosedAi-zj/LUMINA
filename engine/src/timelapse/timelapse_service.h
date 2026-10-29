// 延时工作台域服务（ipc-protocol §5.11 全 8 方法）
// 数据来源（唯一事实源 = engine/src/db/schema.sql）：
//   sequences(id,name,kind,frame_count,state,created_at) / sequence_frames(seq_id,frame_no,photo_id)
//   sequence_ramps(seq_id,keyframes_json,updated_at) —— 曝光坡度关键帧的持久化落点
//   photos.seq_id —— 仅作为 sequence_frames 缺失时的兜底聚合依据
//
// 三条红线在本模块的落点：
//   R1 不锁库 / 原片零写入：帧只读解码，写入只落 sidecar（crs:/lumina:/lrt:）与 {home}/cache。
//   R2 非破坏性：去闪只改 sidecar 的 crs:Exposure2012，坡度只写 lumina:ExposureRamp + lrt:。
//   R3 完整性透明：任何"部分成功"（部分帧 sidecar 写失败、部分帧解码失败）都必须出现在
//      result.warnings 里；视频渲染不允许丢帧凑数——有帧失败就整单失败（见 runRender）。
#ifndef LUMINA_TIMELAPSE_TIMELAPSE_SERVICE_H_
#define LUMINA_TIMELAPSE_TIMELAPSE_SERVICE_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/job_manager.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "raw/raw_decoder.h"
#include "rpc/dispatcher.h"
#include "imaging/imaging.h"
#include "xmp/xmp_service.h"

namespace lumina {
namespace timelapse {

// 曝光坡度关键帧（与 UI RampKeyframe 一比一）
struct Keyframe {
  int64_t frameNo = 0;
  double ev = 0.0;
  double temperatureK = 0.0;
};

// 序列里的单帧（frameNo 从 1 开始，与 UI FramePlayer 一致）
struct SeqFrame {
  int64_t frameNo = 0;
  int64_t photoId = 0;
  std::string filePath;
  std::string sidecarPath;
  bool missing = false;  // 源文件缺失（DB 标记 is_missing 或 photos 行已消失）
};

struct SequenceInfo {
  int64_t seqId = 0;
  std::string name;
  std::string state = "idle";  // idle|analyzing|rendering
  int64_t frameCount = 0;
};

// 渲染参数（ipc-protocol §5.11 timelapse.render）
struct RenderSpec {
  std::string resolution = "4K";  // 1080p|4K|6K
  int fps = 30;                   // 25|30|60
  std::string codec = "h264";     // h264|h265|prores
  // 延时 Phase B：逐帧调色（零值 ≈ 不调色）
  imaging::ToneCurve toneCurve;    // 阴影/中间调/高光 3 区 S 曲线
  imaging::ColorGrade colorGrade;  // 颜色分级三色轮
};

class TimelapseService {
 public:
  // decoder 允许为 nullptr（此时 RAW 帧回落 imaging::decodeImageFile，只对非 RAW 有效）
  TimelapseService(core::Config* config, db::Database* db, db::PhotoDao* photos,
                   xmp::XmpService* xmp, raw::RawDecoder* decoder);

  // ---- §5.11 八个方法，签名与其它域服务一致 ----
  // {} → [{seqId, name, frameCount, state, renderProgress}]
  json::JsonValue listSequences(const json::JsonValue& params, JobManager* jobs,
                               rpc::RpcError* err);
  // {seqId} → {name, frameCount, frames:[{frameNo,photoId}], exposureRamp:{keyframes},
  //            deflicker:{analyzed,brightnessGainPct?}, lrtCompatible}
  json::JsonValue getSequence(const json::JsonValue& params, rpc::RpcError* err);
  // {seqId} → {jobId}；job result {brightnessGainPct, flickerIndex, ...}
  json::JsonValue analyzeFlicker(const json::JsonValue& params, JobManager* jobs,
                                 rpc::RpcError* err);
  // {seqId, strength} → {jobId}；逐帧写 sidecar 的 crs:Exposure2012（非破坏）
  json::JsonValue applyDeflicker(const json::JsonValue& params, JobManager* jobs,
                                 rpc::RpcError* err);
  // {seqId, keyframes:[{frameNo,ev,temperatureK}]} → {}
  json::JsonValue setExposureRamp(const json::JsonValue& params, rpc::RpcError* err);
  // {seqId, frameNo, size} → {path}（size = 长边像素，UI 传 320/640/1080）
  json::JsonValue previewFrame(const json::JsonValue& params, rpc::RpcError* err);
  // {seqId, frameNo, size?} → {r:[256], g:[256], b:[256], luma:[256]}
  // 引擎侧直方图，规避前端 canvas 因 lumina-file:// 污染无法读像素的问题
  json::JsonValue frameHistogram(const json::JsonValue& params, rpc::RpcError* err);
  // {seqId, resolution, fps, codec} → {jobId}；ProRes 一律 5001 + data.alternative
  json::JsonValue render(const json::JsonValue& params, JobManager* jobs, rpc::RpcError* err);
  // {} → [JobInfo]（仅 timelapse.render 任务）
  json::JsonValue getRenderJobs(const json::JsonValue& params, JobManager* jobs,
                                rpc::RpcError* err);

  // 编码器探测：返回 ffmpeg.exe 绝对路径（找不到返回空串，err 说明原因）。
  // 探测顺序：环境变量 LUMINA_FFMPEG（显式指定优先）→ 系统 PATH。
  static std::string detectFfmpeg(std::string* err);

 private:
  // 序列装载：0=成功 1=序列不存在 2=DB 错误（err 带原因）
  int loadSequence(int64_t seqId, SequenceInfo* info, std::vector<SeqFrame>* frames,
                   std::string* err);

  // ---- job 工作体（JobManager 工作线程内执行） ----
  void runAnalyze(int64_t seqId, std::vector<SeqFrame> frames, JobContext& ctx);
  void runApplyDeflicker(int64_t seqId, std::vector<SeqFrame> frames, double strength,
                         JobContext& ctx);
  void runRender(int64_t seqId, std::vector<SeqFrame> frames, const RenderSpec& spec,
                 JobContext& ctx);

  // 单帧平均亮度（0..255）。source 回填实际数据来源，便于结果里说明测量依据。
  bool frameBrightness(const SeqFrame& frame, double* out, std::string* source, std::string* err);
  // 单帧解码为位图（内嵌代理优先 → LibRaw → 通用解码）；只做亮度统计，尺寸不敏感
  bool decodeFrame(const SeqFrame& frame, imaging::Image* out, std::string* source,
                   std::string* err);
  // 需要足够像素时用这个：内嵌代理分辨率不够（wantLongEdge）就改走 LibRaw 真解码，
  // 避免把 160px 的缩略预览放大成 1080p/4K（预览与渲染都走它）
  bool decodeFrameForSize(const SeqFrame& frame, int wantLongEdge, imaging::Image* out,
                          std::string* source, std::string* err);

  // ---- 坡度 ----
  void loadRamp(int64_t seqId, const std::vector<SeqFrame>& frames, std::vector<Keyframe>* out,
                std::string* note);
  bool saveRampRow(int64_t seqId, const std::vector<Keyframe>& keys, std::string* err);
  // 帧号 → 插值后的 EV（关键帧之间线性插值，两端各自取端点值）
  static double rampEvAt(const std::vector<Keyframe>& keys, int64_t frameNo);

  // ---- sidecar 直写（lumina:Deflicker / lrt: 回写）----
  struct SidecarField {
    std::string prefix;
    std::string name;
    std::string value;
    bool remove = false;  // true = 删除该字段（清空坡度时用）
  };
  bool writeSidecarText(const SeqFrame& frame, const std::vector<SidecarField>& fields,
                        std::string* err);
  bool readSidecarText(const SeqFrame& frame, const std::string& prefix,
                       const std::string& name, std::string* value, bool* found, std::string* err);
  // 去闪量化结果标记（lumina:Deflicker，JSON 文本，落在首帧 sidecar）
  bool writeDeflickerMarker(const SeqFrame& frame, const json::JsonValue& marker,
                            std::string* err);

  // ---- job 追踪（listSequences 的实时 state/renderProgress）----
  void trackJob(int64_t jobId, int64_t seqId, const std::string& phase);
  void setSequenceState(int64_t seqId, const std::string& state);
  // 有在跑的任务时覆盖 state/progress；返回 true = 已覆盖
  bool liveState(int64_t seqId, JobManager* jobs, std::string* state, int* progress);

  std::string cacheTimelapseDir() const;

  core::Config* config_;
  db::Database* db_;
  db::PhotoDao* photos_;
  xmp::XmpService* xmp_;
  raw::RawDecoder* decoder_;

  struct TrackedJob {
    int64_t seqId = 0;
    std::string phase;  // analyzing|rendering
  };
  std::mutex trackedMutex_;
  std::map<int64_t, TrackedJob> tracked_;  // jobId → 归属序列
};

}  // namespace timelapse
}  // namespace lumina

#endif  // LUMINA_TIMELAPSE_TIMELAPSE_SERVICE_H_
