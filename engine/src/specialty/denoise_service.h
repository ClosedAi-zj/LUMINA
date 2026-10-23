// 风光专项 · AI 降噪：真 NLM（非局部均值）+ 亮度蒙版，非破坏（原片零写入）
// 设计要点：
//   1) 算法是真 NLM：7x7 相似块窗口、21x21 搜索窗、权重 w=exp(-d²/h²)，d² 为块内均方差；
//      不是均值/高斯模糊冒名。纯 CPU + 多线程按行带并行，可被 job 取消。
//   2) 亮度蒙版（luminanceMask）：暗部用满强度 h、亮部用 0.35h 弱强度，中间按亮度线性插值，
//      对应 LR 里「暗部噪点更明显、亮部不需要重压」的直觉。
//   3) 非破坏：结果写 {home}/cache/denoise/{photoId}-{strength}.tiff，参数写 sidecar
//      （crs:LuminanceSmoothing 供 LR 读，lumina:DenoiseStrength / DenoiseLuminanceMask 供本引擎读），
//      photos 表只更新 sidecar 指针与 mtime，原片一个字节都不改。
#ifndef LUMINA_SPECIALTY_DENOISE_SERVICE_H_
#define LUMINA_SPECIALTY_DENOISE_SERVICE_H_

#include <cstdint>
#include <string>

#include "core/config.h"
#include "core/job_manager.h"
#include "db/photo_dao.h"
#include "imaging/imaging.h"
#include "json/json_value.h"
#include "raw/raw_decoder.h"
#include "rpc/dispatcher.h"
#include "xmp/xmp_service.h"

namespace lumina {
namespace specialty {

class DenoiseService {
 public:
  DenoiseService(core::Config* config, db::PhotoDao* photos, raw::RawDecoder* decoder,
                 xmp::XmpService* xmp);

  // {photoId, strength, luminanceMask} → {jobId}；完成 result {photoId, path, ...}
  json::JsonValue apply(const json::JsonValue& params, JobManager* jobs, rpc::RpcError* err);

  // {photoId, strength, rect:{x,y,w,h}} → {path}（左原图 / 右降噪的局部对比小样）
  json::JsonValue preview(const json::JsonValue& params, rpc::RpcError* err);

  // JobManager 工作线程内执行：解码 → NLM → 缓存落盘 → 写 sidecar 参数
  void runApply(int64_t photoId, int strength, bool luminanceMask, JobContext& ctx);

  // 缓存路径（绝对路径；目录不存在时会尝试创建，失败返回空串）
  std::string cachePath(int64_t photoId, int strength) const;
  std::string previewPath(int64_t photoId) const;

 private:
  bool decodePhoto(const PhotoRecord& record, imaging::Image* out, std::string* err) const;
  // 写 crs:LuminanceSmoothing + lumina:DenoiseStrength / DenoiseLuminanceMask
  bool writeDerivedParams(int64_t photoId, int strength, bool luminanceMask, std::string* err);

  core::Config* config_;
  db::PhotoDao* photos_;
  raw::RawDecoder* decoder_;
  xmp::XmpService* xmp_;
};

}  // namespace specialty
}  // namespace lumina

#endif  // LUMINA_SPECIALTY_DENOISE_SERVICE_H_
