// 导出队列：预设持久化 + 异步导出 job（JPG / PNG / TIFF 未压缩；PSD 明确 5001 降级）
// 红线：导出只读「原片 + sidecar」，绝不改写原文件；单张失败逐条进结果明细，不中断整批。
#ifndef LUMINA_EXPORT_EXPORTER_H_
#define LUMINA_EXPORT_EXPORTER_H_

#include <map>
#include <mutex>
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
#include "xmp/xmp_service.h"

namespace lumina {
namespace exporter {

// 水印参数（ipc-protocol §5.10 export.createJob spec.watermark）
struct WatermarkSpec {
  bool enabled = false;
  std::string position = "bottom-right";  // bottom-right|bottom-left|top-right|top-left|center
  double scale = 0.88;                    // 相对基准尺寸的倍率（基准 = 图宽 12%，见 applyWatermark）
  double opacity = 100.0;                 // 0..100
  std::string text;
  std::string imagePath;

  static WatermarkSpec fromJson(const json::JsonValue& v);
  json::JsonValue toJson() const;
};

// 导出规格（ipc-protocol §5.10）
struct ExportSpec {
  std::string format = "jpg";  // jpg|tiff|png|psd
  int longEdge = 2048;         // 长边像素（只缩不放过大图时按比例）
  std::string colorSpace = "sRGB";  // sRGB|AdobeRGB（AdobeRGB 为线性矩阵近似，非 ICC）
  int quality = 90;                 // 1..100（jpg 有效）
  bool psdKeepLayers = false;       // psd：是否保留节点分层（false = 仅合成单层）
  int psdDepth = 8;                 // psd：位深 8 | 16（16 = 8bit 拓宽，引擎工作空间为 8bit）
  WatermarkSpec watermark;
  std::string namingTemplate = "{date}-{name}";
  std::string outputDir;        // 空 = {home}/exports
  static bool isPsdDepthValid(int depth) { return depth == 8 || depth == 16; }

  static bool fromJson(const json::JsonValue& v, ExportSpec* out, std::string* err);
  json::JsonValue toJson() const;
};

class Exporter {
 public:
  Exporter(core::Config* config, db::PhotoDao* photos, db::Database* db, raw::RawDecoder* decoder,
           xmp::XmpService* xmp);

  // ---- 预设（export.listPresets / savePreset） ----
  json::JsonValue listPresets(rpc::RpcError* err);
  json::JsonValue savePreset(const json::JsonValue& params, rpc::RpcError* err);

  // ---- 队列（export.createJob / listJobs / cancelJob / retryJob） ----
  json::JsonValue createJob(const json::JsonValue& params, JobManager* jobs, rpc::RpcError* err);
  json::JsonValue listJobs(const json::JsonValue& params, JobManager* jobs, rpc::RpcError* err);
  json::JsonValue cancelJob(const json::JsonValue& params, JobManager* jobs, rpc::RpcError* err);
  json::JsonValue retryJob(const json::JsonValue& params, JobManager* jobs, rpc::RpcError* err);

  // JobManager 工作线程内执行（methods.cpp 装配 job 时调用）
  void runExport(const std::vector<int64_t>& photoIds, const ExportSpec& spec, JobContext& ctx);

  /**
   * 测试入口：TIFF 写出与导出走**同一实现**（测试不复制 TIFF 布局代码，避免两边各自漂移）。
   * 生产路径只由 exportOne 调用。
   */
  static bool writeTiffForTest(const std::string& path, const imaging::Image& image,
                               const std::vector<uint8_t>& icc, std::string* err) {
    return writeTiff(path, image, icc, err);
  }

 private:
  // 单张导出：解码 → 近似色调 → 长边缩放 → 色彩空间 → 水印 → 命名 → 编码
  bool exportOne(const PhotoRecord& record, const ExportSpec& spec, const std::string& outDir,
                 int seqNo, int seqTotal, std::string* outPath, std::string* warning,
                 std::string* error);

  // PSD 单张导出（M3-01）：分层（节点累积前缀）或单层合成，按 spec 决定
  bool exportOnePsd(const PhotoRecord& record, const ExportSpec& spec, const std::string& outDir,
                    int seqNo, int seqTotal, const imaging::Image& base,
                    const json::JsonValue& editParams, bool haveEditParams, std::string* outPath,
                    std::string* warning, std::string* error);

  // 命名模板：{date} 本地展开，其余占位符复用 import::RenameEngine（{seq}{name}{camera}…）
  static std::string expandTemplate(const std::string& tmpl, const PhotoRecord& record, int seqNo,
                                    int seqTotal, const std::string& extension);

  // 最小 baseline TIFF 写出（RGB8 未压缩，小端），icc 非空时写 tag 34675
  static bool writeTiff(const std::string& path, const imaging::Image& image,
                        const std::vector<uint8_t>& icc, std::string* err);
  /**
   * 色彩空间落地（M4-01）：走 lcms2 ICC 转换（矩阵近似已删除）；失败时降级 sRGB 并写
   * warning。achieved 返回**实际**写入文件的色彩空间 —— 嵌入 ICC 必须与它一致，不得谎报。
   */
  static void applyColorSpace(imaging::Image* image, const std::string& colorSpace,
                              std::string* achieved, std::string* warning);
  // 水印：图片水印真合成；文字水印用内置 5x7 点阵（仅 ASCII，非 ASCII 记 warning 不静默）
  static bool applyWatermark(imaging::Image* image, const WatermarkSpec& wm, std::string* warning,
                             std::string* err);

  struct RetryEntry {
    std::vector<int64_t> photoIds;
    ExportSpec spec;
  };

  core::Config* config_;
  db::PhotoDao* photos_;
  db::Database* db_;
  raw::RawDecoder* decoder_;
  xmp::XmpService* xmp_;
  std::mutex retryMutex_;
  std::map<int64_t, RetryEntry> retry_;  // jobId → 入参（retryJob 重跑同一批）
};

}  // namespace exporter
}  // namespace lumina

#endif  // LUMINA_EXPORT_EXPORTER_H_
