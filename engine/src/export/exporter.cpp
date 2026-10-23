// 导出队列实现：预设持久化（SQLite export_presets）+ 异步导出 job（JPG / TIFF / PNG）
// 红线 1：绝不改写原片，只读「原片 + sidecar」；目标文件落在 outputDir（缺省 {home}/exports）。
// 红线 2：任何单张失败都进 result.failed 明细，不中断整批；不允许静默成功。
// 降级（明确报错，不伪造）：psd（带图层）在 createJob 阶段返回 5001 codecUnavailable。
// 文字水印：ASCII 走内置 3x5 点阵；非 ASCII（中文等）走系统字体光栅化（text_raster）。
#include "export/exporter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/jpeg_encoder.h"
#include "core/logger.h"
#include "core/str_util.h"
#include "export/text_raster.h"
#include "imaging/imaging.h"
#include "import/rename.h"

namespace lumina {
namespace exporter {
namespace {

// ---- 小工具 ----

void appendWarning(std::string* warning, const std::string& text) {
  if (!warning) return;
  if (!warning->empty()) *warning += " | ";
  *warning += text;
}

double clampDouble(double v, double lo, double hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

json::JsonValue failureItem(int64_t photoId, const std::string& message) {
  json::JsonValue item = json::JsonValue::objectValue();
  item.setItem("photoId", photoId);
  item.setItem("error", message);
  return item;
}

json::JsonValue warningItem(int64_t photoId, const std::string& message) {
  json::JsonValue item = json::JsonValue::objectValue();
  item.setItem("photoId", photoId);
  item.setItem("message", message);
  return item;
}

// 模板里的 {date} 不属于 import::RenameEngine 的 token 表，导出侧先本地展开。
std::string replaceToken(const std::string& text, const std::string& token, const std::string& value) {
  if (token.empty()) return text;
  std::string out;
  out.reserve(text.size());
  size_t pos = 0;
  while (pos < text.size()) {
    size_t hit = text.find(token, pos);
    if (hit == std::string::npos) {
      out.append(text, pos, std::string::npos);
      break;
    }
    out.append(text, pos, hit - pos);
    out += value;
    pos = hit + token.size();
  }
  return out;
}

// 校验命名模板：{date} 展开成定长文本后交给 RenameEngine 校验其余占位符（未知占位符必须在
// 提交 job 前就报错，否则会被静默替换成空串 → 文件名凭空缩水，属静默失败）。
bool validateNamingTemplate(const std::string& tmpl, std::string* err) {
  std::string probe = replaceToken(tmpl, "{date}", "2026-01-01");
  return import::RenameEngine::validate(probe, err);
}

std::string extensionForFormat(const std::string& format) {
  if (format == "tiff") return "tif";
  if (format == "png") return "png";
  if (format == "psd") return "psd";
  return "jpg";
}

// ---- 5x7 点阵的紧凑替代：3x5 内置点阵（仅 ASCII，非 ASCII 记 warning，不静默） ----
// 每个字形 15 个字符 = 5 行 × 3 列，"1" 落墨。
const char* glyphBits(char c) {
  switch (c) {
    case 'A': return "010101111101101";
    case 'B': return "110101110101110";
    case 'C': return "011100100100011";
    case 'D': return "110101101101110";
    case 'E': return "111100110100111";
    case 'F': return "111100110100100";
    case 'G': return "011100101101011";
    case 'H': return "101101111101101";
    case 'I': return "111010010010111";
    case 'J': return "001001001101010";
    case 'K': return "101101110101101";
    case 'L': return "100100100100111";
    case 'M': return "101111111101101";
    case 'N': return "110111101101101";
    case 'O': return "010101101101010";
    case 'P': return "110101110100100";
    case 'Q': return "010101101111011";
    case 'R': return "110101110101101";
    case 'S': return "011100010001110";
    case 'T': return "111010010010010";
    case 'U': return "101101101101011";
    case 'V': return "101101101101010";
    case 'W': return "101101111111101";
    case 'X': return "101101010101101";
    case 'Y': return "101101010010010";
    case 'Z': return "111001010100111";
    case '0': return "111101101101111";
    case '1': return "010110010010111";
    case '2': return "111001111100111";
    case '3': return "111001111001111";
    case '4': return "101101111001001";
    case '5': return "111100111001111";
    case '6': return "111100111101111";
    case '7': return "111001001001001";
    case '8': return "111101111101111";
    case '9': return "111101111001111";
    case ' ': return "000000000000000";
    case '-': return "000000111000000";
    case '.': return "000000000000010";
    case '_': return "000000000000111";
    case ':': return "000010000010000";
    case '/': return "001001010100100";
    case '(': return "001010010010001";
    case ')': return "100010010010100";
    case '+': return "000010111010000";
    case ',': return "000000000010100";
    case '\'': return "010010000000000";
    case '?': return "111001010000010";
    default: return nullptr;
  }
}

char toUpperAsciiSimple(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

void blendPixel(uint8_t* px, uint8_t r, uint8_t g, uint8_t b, double alpha) {
  if (!px) return;
  const double a = clampDouble(alpha, 0.0, 1.0);
  px[0] = static_cast<uint8_t>(px[0] * (1.0 - a) + r * a + 0.5);
  px[1] = static_cast<uint8_t>(px[1] * (1.0 - a) + g * a + 0.5);
  px[2] = static_cast<uint8_t>(px[2] * (1.0 - a) + b * a + 0.5);
}

// 水印锚点：margin 为边距，返回左上角坐标
void anchorOrigin(const std::string& position, int width, int height, int markW, int markH,
                  int margin, int* x0, int* y0) {
  int x = margin;
  int y = margin;
  if (position == "bottom-right") {
    x = width - markW - margin;
    y = height - markH - margin;
  } else if (position == "bottom-left") {
    x = margin;
    y = height - markH - margin;
  } else if (position == "top-right") {
    x = width - markW - margin;
    y = margin;
  } else if (position == "top-left") {
    x = margin;
    y = margin;
  } else {  // center
    x = (width - markW) / 2;
    y = (height - markH) / 2;
  }
  *x0 = clampInt(x, 0, width > 0 ? width - 1 : 0);
  *y0 = clampInt(y, 0, height > 0 ? height - 1 : 0);
}

// ---- baseline TIFF 写入（RGB8、未压缩、小端、单条 strip） ----
void put16(std::string* out, uint16_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put32(std::string* out, uint32_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>((v >> 16) & 0xFF));
  out->push_back(static_cast<char>((v >> 24) & 0xFF));
}

}  // namespace

// ============================ WatermarkSpec ============================

WatermarkSpec WatermarkSpec::fromJson(const json::JsonValue& v) {
  WatermarkSpec w;
  if (!v.isObject()) return w;
  w.enabled = v.get("enabled").asBool(false);

  static const char* kPositions[] = {"bottom-right", "bottom-left", "top-right", "top-left",
                                     "center"};
  const std::string pos = v.get("position").asString("bottom-right");
  bool known = false;
  for (const char* p : kPositions) {
    if (pos == p) {
      known = true;
      break;
    }
  }
  w.position = known ? pos : "bottom-right";

  w.scale = clampDouble(v.get("scale").asDouble(0.88), 0.05, 8.0);
  w.opacity = clampDouble(v.get("opacity").asDouble(100.0), 0.0, 100.0);
  w.text = v.get("text").asString();
  w.imagePath = v.get("imagePath").asString();
  return w;
}

json::JsonValue WatermarkSpec::toJson() const {
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("enabled", enabled);
  out.setItem("position", position);
  out.setItem("scale", scale);
  out.setItem("opacity", opacity);
  if (!text.empty()) out.setItem("text", text);
  if (!imagePath.empty()) out.setItem("imagePath", imagePath);
  return out;
}

// ============================ ExportSpec ============================

bool ExportSpec::fromJson(const json::JsonValue& v, ExportSpec* out, std::string* err) {
  if (!out) {
    if (err) *err = "spec 输出指针为空";
    return false;
  }
  if (!v.isObject()) {
    if (err) *err = "spec 必须是对象";
    return false;
  }

  ExportSpec spec;
  std::string format = toLowerAscii(v.get("format").asString("jpg"));
  if (format.empty()) format = "jpg";
  if (format == "jpeg") format = "jpg";
  if (format == "tif") format = "tiff";
  if (format != "jpg" && format != "tiff" && format != "png" && format != "psd") {
    if (err) {
      *err = "不支持的导出格式: " + format + "（支持 jpg|tiff|png；psd 为 v1.0.0 降级项）";
    }
    return false;
  }
  spec.format = format;

  const int longEdge = static_cast<int>(v.get("longEdge").asInt(2048));
  if (longEdge < 64 || longEdge > 20000) {
    if (err) *err = "longEdge 超出范围 [64, 20000]: " + std::to_string(longEdge);
    return false;
  }
  spec.longEdge = longEdge;

  const std::string colorSpace = v.get("colorSpace").asString("sRGB");
  const std::string lowerSpace = toLowerAscii(colorSpace);
  if (lowerSpace == "srgb" || lowerSpace.empty()) {
    spec.colorSpace = "sRGB";
  } else if (lowerSpace == "adobergb" || lowerSpace == "adobe-rgb" || lowerSpace == "adobe rgb") {
    spec.colorSpace = "AdobeRGB";
  } else {
    if (err) *err = "不支持的色彩空间: " + colorSpace + "（支持 sRGB|AdobeRGB）";
    return false;
  }

  spec.quality = clampInt(static_cast<int>(v.get("quality").asInt(90)), 1, 100);
  spec.psdKeepLayers = v.get("psdKeepLayers").asBool(false);
  spec.watermark = WatermarkSpec::fromJson(v.get("watermark"));

  const std::string tmpl = v.get("namingTemplate").asString("{date}-{name}");
  spec.namingTemplate = tmpl.empty() ? std::string("{date}-{name}") : tmpl;
  std::string tmplErr;
  if (!validateNamingTemplate(spec.namingTemplate, &tmplErr)) {
    if (err) *err = "namingTemplate 非法: " + tmplErr;
    return false;
  }

  const std::string outDir = v.get("outputDir").asString();
  spec.outputDir = outDir.empty() ? std::string() : normalizePath(outDir);

  *out = spec;
  return true;
}

json::JsonValue ExportSpec::toJson() const {
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("format", format);
  out.setItem("longEdge", longEdge);
  out.setItem("colorSpace", colorSpace);
  out.setItem("quality", quality);
  out.setItem("psdKeepLayers", psdKeepLayers);
  out.setItem("watermark", watermark.toJson());
  out.setItem("namingTemplate", namingTemplate);
  out.setItem("outputDir", outputDir);
  return out;
}

// ============================ Exporter ============================

Exporter::Exporter(core::Config* config, db::PhotoDao* photos, db::Database* db,
                   raw::RawDecoder* decoder, xmp::XmpService* xmp)
    : config_(config), photos_(photos), db_(db), decoder_(decoder), xmp_(xmp) {}

json::JsonValue Exporter::listPresets(rpc::RpcError* err) {
  json::JsonValue out = json::JsonValue::arrayValue();
  if (!db_) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "数据库未就绪");
    return json::JsonValue::nullValue();
  }
  std::string sqlErr;
  auto stmt = db_->prepareCached("export.presets.list",
                                 "SELECT id,name,spec_json FROM export_presets ORDER BY id ASC;",
                                 &sqlErr);
  if (!stmt || !stmt->valid()) {
    if (err) *err = rpc::RpcError(rpc::kDbError, "读取导出预设失败: " + sqlErr);
    return json::JsonValue::nullValue();
  }
  while (stmt->step(&sqlErr)) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("id", stmt->columnInt64(0));
    item.setItem("name", stmt->columnText(1));

    ExportSpec spec;
    std::string parseErr;
    json::JsonValue parsed;
    const std::string rawSpec = stmt->columnText(2);
    if (!rawSpec.empty() && json::JsonValue::parse(rawSpec, &parsed, &parseErr) &&
        ExportSpec::fromJson(parsed, &spec, &parseErr)) {
      item.setItem("spec", spec.toJson());
    } else {
      // 预设 spec 损坏不得静默成"空 spec"：记日志 + 用默认 spec 占位，UI 可重存覆盖
      logWarn("export", "preset " + stmt->columnText(1) + " spec 解析失败: " + parseErr);
      ExportSpec fallback;
      item.setItem("spec", fallback.toJson());
      item.setItem("specError", parseErr);
    }
    out.addItem(item);
  }
  if (!sqlErr.empty()) {
    if (err) *err = rpc::RpcError(rpc::kDbError, "遍历导出预设失败: " + sqlErr);
    return json::JsonValue::nullValue();
  }
  stmt->reset();
  return out;
}

json::JsonValue Exporter::savePreset(const json::JsonValue& params, rpc::RpcError* err) {
  if (!db_) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "数据库未就绪");
    return json::JsonValue::nullValue();
  }
  const std::string name = trim(params.get("name").asString());
  if (name.empty()) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "name 不能为空");
    return json::JsonValue::nullValue();
  }
  ExportSpec spec;
  std::string specErr;
  if (!ExportSpec::fromJson(params.get("spec"), &spec, &specErr)) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "spec 非法: " + specErr);
    return json::JsonValue::nullValue();
  }
  const std::string specJson = spec.toJson().serialize();

  // 同名预设覆盖：先 UPDATE（同名已存在则复用 id），再在不存在时 INSERT ... RETURNING id
  // （教训：不能依赖 last_insert_rowid，ON CONFLICT 分支不会更新它）
  std::string sqlErr;
  {
    auto upd = db_->prepareCached("export.presets.update",
                                  "UPDATE export_presets SET spec_json=? WHERE name=?;", &sqlErr);
    if (!upd || !upd->valid()) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "写入导出预设失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
    upd->bindText(1, specJson);
    upd->bindText(2, name);
    const bool ok = upd->step(&sqlErr);
    upd->reset();
    if (!ok) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "更新导出预设失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
  }

  int64_t presetId = 0;
  {
    auto sel = db_->prepareCached("export.presets.byName",
                                  "SELECT id FROM export_presets WHERE name=? ORDER BY id ASC LIMIT 1;",
                                  &sqlErr);
    if (!sel || !sel->valid()) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "读取导出预设失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
    sel->bindText(1, name);
    const bool hasRow = sel->step(&sqlErr);
    if (hasRow) presetId = sel->columnInt64(0);
    sel->reset();
    if (!sqlErr.empty()) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "读取导出预设失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
  }

  if (presetId == 0) {
    auto ins = db_->prepareCached(
        "export.presets.insert",
        "INSERT INTO export_presets(name,spec_json) VALUES(?,?) RETURNING id;", &sqlErr);
    if (!ins || !ins->valid()) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "新增导出预设失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
    ins->bindText(1, name);
    ins->bindText(2, specJson);
    const bool hasRow = ins->step(&sqlErr);
    if (hasRow) presetId = ins->columnInt64(0);
    ins->reset();
    if (presetId == 0) {
      if (err) {
        *err = rpc::RpcError(rpc::kDbError, "新增导出预设失败: " + (sqlErr.empty() ? "未返回 id" : sqlErr));
      }
      return json::JsonValue::nullValue();
    }
  }

  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("id", presetId);
  return out;
}

json::JsonValue Exporter::createJob(const json::JsonValue& params, JobManager* jobs,
                                    rpc::RpcError* err) {
  if (!jobs) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "任务管理器未就绪");
    return json::JsonValue::nullValue();
  }
  const std::vector<int64_t> photoIds = [&params]() {
    std::vector<int64_t> ids;
    const json::JsonValue& arr = params.get("photoIds");
    if (arr.isArray()) {
      for (const auto& item : arr.arrayItems()) {
        const int64_t id = item.asInt(0);
        if (id > 0) ids.push_back(id);
      }
    }
    return ids;
  }();
  if (photoIds.empty()) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "photoIds 不能为空");
    return json::JsonValue::nullValue();
  }

  ExportSpec spec;
  std::string specErr;
  if (!ExportSpec::fromJson(params.get("spec"), &spec, &specErr)) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "spec 非法: " + specErr);
    return json::JsonValue::nullValue();
  }

  // 降级项：psd（带图层）没有可靠编码实现 → 明确报 5001，绝不"成功但文件是 jpg"
  // （png 自 WP-C3 起由 stb_image_write 支持，见 imaging::encodePng）
  if (spec.format == "psd") {
    json::JsonValue data = json::JsonValue::objectValue();
    data.setItem("format", spec.format);
    json::JsonValue supported = json::JsonValue::arrayValue();
    supported.addItem("jpg");
    supported.addItem("tiff");
    supported.addItem("png");
    data.setItem("supported", supported);
    data.setItem("alternative", "jpg");
    if (err) {
      *err = rpc::RpcError(rpc::kCodecUnavailable,
                           "导出格式暂不可用: " + spec.format + "（v1.0.0 支持 jpg/tiff/png）", data);
    }
    return json::JsonValue::nullValue();
  }

  const int64_t jobId = jobs->start("export.run", [this, photoIds, spec](JobContext& ctx) {
    runExport(photoIds, spec, ctx);
  });
  {
    std::lock_guard<std::mutex> lock(retryMutex_);
    retry_[jobId] = RetryEntry{photoIds, spec};
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("jobId", jobId);
  return out;
}

json::JsonValue Exporter::listJobs(const json::JsonValue& params, JobManager* jobs,
                                   rpc::RpcError* err) {
  if (!jobs) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "任务管理器未就绪");
    return json::JsonValue::nullValue();
  }
  const std::string stateFilter = params.get("state").asString();
  json::JsonValue out = json::JsonValue::arrayValue();
  for (const JobState& st : jobs->list(stateFilter)) {
    if (st.kind != "export.run") continue;
    json::JsonValue item = st.toJson();
    if (st.state == "failed") {
      json::JsonValue e = json::JsonValue::objectValue();
      e.setItem("code", st.errorCode != 0 ? st.errorCode : rpc::kJobFailed);
      e.setItem("message", st.errorMessage);
      item.setItem("error", e);
    }
    if (st.state == "done" && !st.result.isNull()) item.setItem("result", st.result);
    out.addItem(item);
  }
  return out;
}

json::JsonValue Exporter::cancelJob(const json::JsonValue& params, JobManager* jobs,
                                    rpc::RpcError* err) {
  if (!jobs) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "任务管理器未就绪");
    return json::JsonValue::nullValue();
  }
  const int64_t jobId = params.get("jobId").asInt(0);
  if (jobId <= 0) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "jobId 非法: " + std::to_string(jobId));
    return json::JsonValue::nullValue();
  }
  if (!jobs->cancel(jobId)) {
    if (err) *err = rpc::RpcError(rpc::kJobNotFound, "任务不存在: " + std::to_string(jobId));
    return json::JsonValue::nullValue();
  }
  return json::JsonValue::objectValue();
}

json::JsonValue Exporter::retryJob(const json::JsonValue& params, JobManager* jobs,
                                   rpc::RpcError* err) {
  if (!jobs) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "任务管理器未就绪");
    return json::JsonValue::nullValue();
  }
  const int64_t jobId = params.get("jobId").asInt(0);
  if (jobId <= 0) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "jobId 非法: " + std::to_string(jobId));
    return json::JsonValue::nullValue();
  }

  RetryEntry entry;
  {
    std::lock_guard<std::mutex> lock(retryMutex_);
    auto it = retry_.find(jobId);
    if (it == retry_.end()) {
      if (err) {
        *err = rpc::RpcError(rpc::kJobNotFound,
                             "任务不存在或不属于导出队列，无法重试: " + std::to_string(jobId));
      }
      return json::JsonValue::nullValue();
    }
    entry = it->second;
  }

  JobState state;
  const bool hasJob = jobs->getJob(jobId, &state);
  const bool stillRunning =
      hasJob && (state.state == "queued" || state.state == "running") && !jobs->isCancelled(jobId);
  if (stillRunning) {
    if (err) {
      *err = rpc::RpcError(rpc::kJobBusy,
                           "任务仍在运行，请先取消再重试: " + std::to_string(jobId));
    }
    return json::JsonValue::nullValue();
  }
  if (entry.photoIds.empty()) {
    if (err) {
      *err = rpc::RpcError(rpc::kJobNotFound, "重试入参已丢失: " + std::to_string(jobId));
    }
    return json::JsonValue::nullValue();
  }

  const std::vector<int64_t> photoIds = entry.photoIds;
  const ExportSpec spec = entry.spec;
  const int64_t newJobId =
      jobs->start("export.run", [this, photoIds, spec](JobContext& ctx) {
        runExport(photoIds, spec, ctx);
      });
  {
    std::lock_guard<std::mutex> lock(retryMutex_);
    retry_.erase(jobId);
    retry_[newJobId] = RetryEntry{photoIds, spec};
  }
  // 协议定义返回 {}；额外带 jobId（只增字段，不破坏兼容），UI 可直接订阅新 job 的进度
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("jobId", newJobId);
  return out;
}

void Exporter::runExport(const std::vector<int64_t>& photoIds, const ExportSpec& spec,
                         JobContext& ctx) {
  // 注意：homeDir() 内部会加 Config::mutex_，此处不持任何 Config 锁，直接取即可
  const std::string outDir =
      spec.outputDir.empty() ? joinPath(config_ ? config_->homeDir() : std::string("."), "exports")
                             : spec.outputDir;
  const int total = static_cast<int>(photoIds.size());

  json::JsonValue outputs = json::JsonValue::arrayValue();
  json::JsonValue failed = json::JsonValue::arrayValue();
  json::JsonValue warnings = json::JsonValue::arrayValue();
  int exported = 0;
  int cancelledAt = -1;

  ctx.progress(0, "准备导出 " + std::to_string(total) + " 张");
  for (int i = 0; i < total; ++i) {
    if (ctx.isCancelled()) {
      cancelledAt = i;
      logInfo("export", "job " + std::to_string(ctx.jobId()) + " cancelled before item " +
                            std::to_string(i + 1) + "/" + std::to_string(total));
      break;
    }
    PhotoRecord record;
    std::string dbErr;
    std::string outPath;
    std::string warning;
    std::string error;
    if (!photos_ || !photos_->getById(photoIds[static_cast<size_t>(i)], &record, &dbErr)) {
      const std::string message =
          dbErr.empty() ? std::string("照片不存在") : ("读取照片失败: " + dbErr);
      failed.addItem(failureItem(photoIds[static_cast<size_t>(i)], message));
      logWarn("export", message);
    } else if (exportOne(record, spec, outDir, i + 1, total, &outPath, &warning, &error)) {
      json::JsonValue item = json::JsonValue::objectValue();
      item.setItem("photoId", record.id);
      item.setItem("path", outPath);
      outputs.addItem(item);
      ++exported;
      if (!warning.empty()) warnings.addItem(warningItem(record.id, warning));
    } else {
      failed.addItem(failureItem(record.id, error));
      logWarn("export", "photo " + std::to_string(record.id) + " 导出失败: " + error);
    }

    const int percent = total > 0 ? ((i + 1) * 100 / total) : 100;
    ctx.progress(percent, "已导出 " + std::to_string(exported) + "/" + std::to_string(i + 1) +
                              "（失败 " + std::to_string(failed.size()) + "）");
  }

  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("outputs", outputs);
  result.setItem("exported", exported);
  result.setItem("failed", failed);
  result.setItem("warnings", warnings);
  result.setItem("total", total);
  result.setItem("outputDir", outDir);
  if (cancelledAt >= 0) result.setItem("cancelledAt", cancelledAt);
  ctx.setResult(result);
  logInfo("export", "job " + std::to_string(ctx.jobId()) + " finished: exported=" +
                        std::to_string(exported) + " failed=" + std::to_string(failed.size()) +
                        " dir=" + outDir);
}

bool Exporter::exportOne(const PhotoRecord& record, const ExportSpec& spec,
                         const std::string& outDir, int seqNo, int seqTotal, std::string* outPath,
                         std::string* warning, std::string* error) {
  if (!decoder_ || !photos_) {
    if (error) *error = "导出服务未就绪";
    return false;
  }
  if (record.isMissing) {
    if (error) *error = "源文件缺失（索引标记 missing）: " + record.filePath;
    return false;
  }
  if (!fileExists(record.filePath)) {
    if (error) *error = "源文件不存在: " + record.filePath;
    return false;
  }

  // ---- 1. LibRaw 真解码（half=false：导出要全分辨率） ----
  // JPG/PNG 对片与纯 JPEG 图库无法走 LibRaw，回落到通用解码器（两条通路都是真解码，
  // 不存在"解不出来就输出占位图"这种假实现）。
  imaging::Image image;
  std::string decodeErr;
  if (!decoder_->decodeToBitmap(record.filePath, false, &image, &decodeErr)) {
    std::string genericErr;
    if (!imaging::decodeImageFile(record.filePath, &image, &genericErr)) {
      if (error) {
        *error = "解码失败: " + record.filePath + "（raw: " + decodeErr + "；通用: " + genericErr +
                 "）";
      }
      return false;
    }
  }

  // ---- 2. 长边缩放（只缩不放：原片长边小于目标时保持原尺寸） ----
  const int nativeLong = std::max(image.width, image.height);
  if (nativeLong > 0 && spec.longEdge < nativeLong) {
    const double scale = static_cast<double>(spec.longEdge) / static_cast<double>(nativeLong);
    int targetW = static_cast<int>(image.width * scale + 0.5);
    int targetH = static_cast<int>(image.height * scale + 0.5);
    if (targetW < 1) targetW = 1;
    if (targetH < 1) targetH = 1;
    imaging::Image scaled;
    if (!imaging::resizeImage(image, &scaled, targetW, targetH)) {
      if (error) *error = "缩放失败（目标 " + std::to_string(targetW) + "x" +
                          std::to_string(targetH) + "）";
      return false;
    }
    image = scaled;
  }

  // ---- 3. 应用 sidecar 参数（CPU 近似渲染：曝光/对比/饱和度；失败只记 warning） ----
  if (xmp_) {
    const std::string sidecar = record.sidecarPath.empty()
                                    ? xmp_->sidecarPathFor(record.filePath)
                                    : record.sidecarPath;
    json::JsonValue params;
    std::string paramErr;
    if (xmp_->loadParamsFromPath(record.filePath, sidecar, &params, &paramErr)) {
      imaging::ToneAdjust adjust;
      adjust.exposure = params.get("exposure").asDouble(0.0);
      adjust.contrast = params.get("contrast").asDouble(0.0);
      adjust.saturation = params.get("saturation").asDouble(0.0);
      imaging::applyToneApprox(&image, adjust);
      // WP-A3：HSL 8 色带导出管线（hsl.{band}.{hue,sat,lum} → CPU 近似渲染；
      // 全 0 跳过，避免无谓的逐像素 RGB↔HSL 往返）
      const json::JsonValue& hsl = params.get("hsl");
      if (hsl.isObject()) {
        imaging::HslBands bands;
        for (const auto& band : hsl.objectItems()) {
          const int idx = imaging::HslBands::bandIndex(band.first);
          if (idx < 0 || !band.second.isObject()) continue;
          bands.hue[idx] = band.second.get("hue").asDouble(0.0);
          bands.sat[idx] = band.second.get("sat").asDouble(0.0);
          bands.lum[idx] = band.second.get("lum").asDouble(0.0);
        }
        if (bands.any()) imaging::applyHslApprox(&image, bands);
      }
    } else {
      appendWarning(warning, "sidecar 参数读取失败，按原片导出: " + paramErr);
    }
  }

  // ---- 4. 色彩空间（sRGB 直通 / AdobeRGB 线性矩阵近似，非 ICC 精确转换） ----
  applyColorSpace(&image, spec.colorSpace);

  // ---- 5. 水印（真合成；失败即报错，不允许"导出了无水印的文件"） ----
  if (spec.watermark.enabled) {
    std::string wmErr;
    if (!applyWatermark(&image, spec.watermark, warning, &wmErr)) {
      if (error) *error = "水印合成失败: " + wmErr;
      return false;
    }
  }

  // ---- 6. 目标目录 + 命名 ----
  if (!dirExists(outDir) && !makeDirs(outDir)) {
    if (error) *error = "创建导出目录失败: " + outDir;
    return false;
  }
  const std::string extension = extensionForFormat(spec.format);
  std::string fileName =
      expandTemplate(spec.namingTemplate, record, seqNo, seqTotal, extension) + "." + extension;
  std::string target = joinPath(outDir, fileName);
  if (fileExists(target)) {
    const std::string stem = baseNameNoExt(fileName);
    for (int n = 2; n < 10000; ++n) {
      const std::string candidate =
          joinPath(outDir, stem + "-" + std::to_string(n) + "." + extension);
      if (!fileExists(candidate)) {
        target = candidate;
        break;
      }
    }
  }

  // ---- 7. 编码落盘 ----
  std::string encodeErr;
  bool ok = false;
  if (spec.format == "jpg") {
    ok = core::jpeg::encodeToFile(target, image, spec.quality, &encodeErr);
  } else if (spec.format == "tiff") {
    ok = writeTiff(target, image, &encodeErr);
  } else if (spec.format == "png") {
    ok = imaging::encodePng(target, image, &encodeErr);
  } else {
    // createJob 已拦截；这里再兜一层，避免将来加格式时静默写错
    if (error) *error = "导出格式未实现: " + spec.format;
    return false;
  }
  if (!ok) {
    if (error) *error = "写文件失败: " + (encodeErr.empty() ? target : encodeErr);
    return false;
  }
  const int64_t bytes = fileSize(target);
  if (bytes <= 0) {
    if (error) *error = "导出文件为空: " + target;
    removeFile(target);
    return false;
  }

  if (outPath) *outPath = target;
  return true;
}

std::string Exporter::expandTemplate(const std::string& tmpl, const PhotoRecord& record, int seqNo,
                                     int seqTotal, const std::string& extension) {
  const std::string base = tmpl.empty() ? std::string("{date}-{name}") : tmpl;
  const std::string dateText =
      record.capturedAt > 0 ? formatDate(record.capturedAt) : std::string("undated");

  import::RenameContext ctx;
  ctx.capturedAtMs = record.capturedAt;
  ctx.camera = record.camera;
  ctx.lens = record.lens;
  ctx.location = record.location;
  ctx.seqNo = seqNo;
  ctx.seqTotal = seqTotal;
  ctx.iso = record.iso;
  ctx.originalName =
      baseNameNoExt(record.fileName.empty() ? record.filePath : record.fileName);
  ctx.extension = extension;

  const std::string expanded = replaceToken(base, "{date}", dateText);
  std::string name = import::RenameEngine::apply(expanded, ctx);
  if (name.empty()) name = baseNameNoExt(record.filePath);
  if (name.empty()) name = "export";
  return name;
}

bool Exporter::writeTiff(const std::string& path, const imaging::Image& image, std::string* err) {
  if (image.width <= 0 || image.height <= 0) {
    if (err) *err = "tiff: 空图像";
    return false;
  }
  const size_t need = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3;
  if (image.pixels.size() < need) {
    if (err) *err = "tiff: 像素缓冲不足（需 " + std::to_string(need) + "，实有 " +
                    std::to_string(image.pixels.size()) + "）";
    return false;
  }

  const uint32_t w = static_cast<uint32_t>(image.width);
  const uint32_t h = static_cast<uint32_t>(image.height);
  const uint32_t pixelBytes = static_cast<uint32_t>(need);

  // IFD 布局：13 个条目（按 tag 升序），紧随其后的值区依次为 BitsPerSample / XRes / YRes
  const uint16_t kEntryCount = 13;
  const size_t ifdOffset = 8;
  const size_t ifdSize = 2 + static_cast<size_t>(kEntryCount) * 12 + 4;
  const size_t bitsOffset = ifdOffset + ifdSize;   // 3 × SHORT = 6 字节
  const size_t xResOffset = bitsOffset + 6;        // RATIONAL = 8 字节
  const size_t yResOffset = xResOffset + 8;        // RATIONAL = 8 字节
  const size_t dataOffset = yResOffset + 8;

  std::string out;
  out.reserve(dataOffset + static_cast<size_t>(pixelBytes));

  put16(&out, 0x4949);  // "II" 小端
  put16(&out, 42);      // TIFF magic
  put32(&out, static_cast<uint32_t>(ifdOffset));
  put16(&out, kEntryCount);

  auto entry = [&out](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
    put16(&out, tag);
    put16(&out, type);
    put32(&out, count);
    put32(&out, value);
  };
  entry(256, 3, 1, w);                                        // ImageWidth
  entry(257, 3, 1, h);                                        // ImageLength
  entry(258, 3, 3, static_cast<uint32_t>(bitsOffset));        // BitsPerSample = 8,8,8
  entry(259, 3, 1, 1);                                        // Compression = none
  entry(262, 3, 1, 2);                                        // Photometric = RGB
  entry(273, 4, 1, static_cast<uint32_t>(dataOffset));        // StripOffsets
  entry(277, 3, 1, 3);                                        // SamplesPerPixel
  entry(278, 3, 1, h);                                        // RowsPerStrip = 单条
  entry(279, 4, 1, pixelBytes);                               // StripByteCounts
  entry(282, 5, 1, static_cast<uint32_t>(xResOffset));        // XResolution
  entry(283, 5, 1, static_cast<uint32_t>(yResOffset));        // YResolution
  entry(284, 3, 1, 1);                                        // PlanarConfiguration = chunky
  entry(296, 3, 1, 2);                                        // ResolutionUnit = inch
  put32(&out, 0);                                             // 无下一个 IFD

  put16(&out, 8);
  put16(&out, 8);
  put16(&out, 8);
  put32(&out, 72);  // XResolution 72/1
  put32(&out, 1);
  put32(&out, 72);  // YResolution 72/1
  put32(&out, 1);

  out.append(reinterpret_cast<const char*>(image.pixels.data()), need);
  if (!writeFileBytes(path, out, err)) return false;
  return true;
}

void Exporter::applyColorSpace(imaging::Image* image, const std::string& colorSpace) {
  if (!image || image->pixels.empty() || colorSpace != "AdobeRGB") return;  // sRGB 直通
  // sRGB(linear) → AdobeRGB(1998)(linear) 矩阵（D65；Bruce Lindbloom 标准矩阵）
  // 说明：v1.0.0 不做 ICC 精确转换（无 ICC profile 嵌入、无 BPC），仅矩阵近似。
  static const double kMatrix[3][3] = {
      {1.96253, -0.61068, -0.34137},
      {-0.97876, 1.91615, 0.04240},
      {0.02869, -0.14067, 1.34926},
  };
  const double kAdobeGamma = 563.0 / 256.0;  // ≈ 2.19921875
  const size_t count = image->pixels.size() / 3;
  uint8_t* px = image->pixels.data();
  for (size_t i = 0; i < count; ++i) {
    const double r = px[i * 3 + 0] / 255.0;
    const double g = px[i * 3 + 1] / 255.0;
    const double b = px[i * 3 + 2] / 255.0;
    // sRGB 解码 gamma（分段函数）
    auto toLinear = [](double v) {
      return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    // AdobeRGB 编码 gamma（纯幂函数）
    auto fromLinear = [kAdobeGamma](double v) {
      const double c = v < 0.0 ? 0.0 : v;
      return std::pow(c, 1.0 / kAdobeGamma);
    };
    const double lr = toLinear(r);
    const double lg = toLinear(g);
    const double lb = toLinear(b);
    double ar = kMatrix[0][0] * lr + kMatrix[0][1] * lg + kMatrix[0][2] * lb;
    double ag = kMatrix[1][0] * lr + kMatrix[1][1] * lg + kMatrix[1][2] * lb;
    double ab = kMatrix[2][0] * lr + kMatrix[2][1] * lg + kMatrix[2][2] * lb;
    ar = clampDouble(ar, 0.0, 1.0);
    ag = clampDouble(ag, 0.0, 1.0);
    ab = clampDouble(ab, 0.0, 1.0);
    px[i * 3 + 0] = static_cast<uint8_t>(fromLinear(ar) * 255.0 + 0.5);
    px[i * 3 + 1] = static_cast<uint8_t>(fromLinear(ag) * 255.0 + 0.5);
    px[i * 3 + 2] = static_cast<uint8_t>(fromLinear(ab) * 255.0 + 0.5);
  }
}

bool Exporter::applyWatermark(imaging::Image* image, const WatermarkSpec& wm, std::string* warning,
                              std::string* err) {
  if (!image || image->width <= 0 || image->height <= 0 || image->pixels.empty()) {
    if (err) *err = "空图像";
    return false;
  }
  if (!wm.enabled) return true;

  const int width = image->width;
  const int height = image->height;
  const double alpha = clampDouble(wm.opacity / 100.0, 0.0, 1.0);
  const int margin = std::max(4, height / 50);

  if (!wm.imagePath.empty()) {
    // ---- 图片水印：真解码 + 真合成 ----
    imaging::Image mark;
    std::string decodeErr;
    if (!imaging::decodeImageFile(wm.imagePath, &mark, &decodeErr)) {
      if (err) *err = "水印图片无法读取: " + wm.imagePath + "（" + decodeErr + "）";
      return false;
    }
    if (mark.width <= 0 || mark.height <= 0) {
      if (err) *err = "水印图片尺寸非法: " + wm.imagePath;
      return false;
    }
    // 基准宽度 = 图宽 12%，scale 为相对倍率
    int markW = static_cast<int>(width * 0.12 * wm.scale + 0.5);
    if (markW < 1) markW = 1;
    int markH = static_cast<int>(static_cast<double>(mark.height) * markW / mark.width + 0.5);
    if (markH < 1) markH = 1;

    imaging::Image scaled;
    if (!imaging::resizeImage(mark, &scaled, markW, markH)) {
      if (err) *err = "水印图片缩放失败: " + wm.imagePath;
      return false;
    }
    int x0 = 0;
    int y0 = 0;
    anchorOrigin(wm.position, width, height, scaled.width, scaled.height, margin, &x0, &y0);
    for (int y = 0; y < scaled.height; ++y) {
      const int ty = y0 + y;
      if (ty < 0 || ty >= height) continue;
      for (int x = 0; x < scaled.width; ++x) {
        const int tx = x0 + x;
        if (tx < 0 || tx >= width) continue;
        const size_t si = (static_cast<size_t>(y) * scaled.width + x) * 3;
        const size_t di = (static_cast<size_t>(ty) * width + tx) * 3;
        if (si + 2 >= scaled.pixels.size() || di + 2 >= image->pixels.size()) continue;
        blendPixel(&image->pixels[di], scaled.pixels[si + 0], scaled.pixels[si + 1],
                   scaled.pixels[si + 2], alpha);
      }
    }
    // 解码走 stb（RGB），PNG 透明通道不参与合成 —— 如实标注，不假装支持
    appendWarning(warning, "图片水印按 RGB 合成（保留源图不含 alpha 通道）");
    return true;
  }

  if (wm.text.empty()) {
    if (err) *err = "watermark.enabled=true 但 text 与 imagePath 均为空";
    return false;
  }

  // ---- 文字水印：非 ASCII（中文等）走系统字体光栅化；ASCII 走内置点阵 ----
  const int block = std::max(1, height / 100);  // 字高 ≈ 5% 图高
  const int glyphW = 3 * block;
  const int glyphH = 5 * block;
  const int advance = 4 * block;
  const std::string text = wm.text;

  if (textraster::hasNonAscii(text)) {
    // GDI 系统字体光栅化（白字 + 覆盖度 alpha）；失败回退点阵并如实记 warning。
    // 字高与点阵路径视觉对齐：点阵字形高 = 5×block = 5% 图高，CJK 字形约占 em 盒 86%，
    // 故 lfHeight 取 ≈5.7% 图高（height*5/88），两种路径观感一致。
    textraster::RasterText rt;
    std::string rasterErr;
    if (textraster::rasterizeText(text, std::max(12, height * 5 / 88), &rt, &rasterErr) && rt.width > 0 &&
        rt.height > 0) {
      int rx0 = 0;
      int ry0 = 0;
      anchorOrigin(wm.position, width, height, rt.width, rt.height, margin, &rx0, &ry0);
      for (int y = 0; y < rt.height; ++y) {
        const int ty = ry0 + y;
        if (ty < 0 || ty >= height) continue;
        for (int x = 0; x < rt.width; ++x) {
          const int tx = rx0 + x;
          if (tx < 0 || tx >= width) continue;
          const size_t si = (static_cast<size_t>(y) * rt.width + x) * 4;
          if (si + 3 >= rt.rgba.size()) continue;
          const double coverage = rt.rgba[si + 3] / 255.0;
          if (coverage <= 0.0) continue;
          const size_t di = (static_cast<size_t>(ty) * width + tx) * 3;
          if (di + 2 >= image->pixels.size()) continue;
          blendPixel(&image->pixels[di], 255, 255, 255, alpha * coverage);
        }
      }
      return true;
    }
    appendWarning(warning, "非 ASCII 水印系统字体渲染失败，已回退内置点阵: " + rasterErr);
  }

  const int textW = static_cast<int>(text.size()) * advance - (advance - glyphW);
  int x0 = 0;
  int y0 = 0;
  anchorOrigin(wm.position, width, height, textW > 0 ? textW : glyphW, glyphH, margin, &x0, &y0);

  int unsupported = 0;
  int penX = x0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char raw = text[i];
    const char upper = toUpperAsciiSimple(raw);
    const char* bits = glyphBits(upper);
    if (!bits) {
      // 非 ASCII（UTF-8 多字节首位 >= 0x80）或未收录字符：统一用 '?'，并计数告知调用方
      ++unsupported;
      bits = glyphBits('?');
    }
    if (bits) {
      for (int gy = 0; gy < 5; ++gy) {
        for (int gx = 0; gx < 3; ++gx) {
          if (bits[gy * 3 + gx] != '1') continue;
          const int px0 = penX + gx * block;
          const int py0 = y0 + gy * block;
          for (int dy = 0; dy < block; ++dy) {
            const int ty = py0 + dy;
            if (ty < 0 || ty >= height) continue;
            for (int dx = 0; dx < block; ++dx) {
              const int tx = px0 + dx;
              if (tx < 0 || tx >= width) continue;
              const size_t di = (static_cast<size_t>(ty) * width + tx) * 3;
              if (di + 2 >= image->pixels.size()) continue;
              blendPixel(&image->pixels[di], 255, 255, 255, alpha);
            }
          }
        }
      }
    }
    penX += advance;
  }
  if (unsupported > 0) {
    appendWarning(warning, "文字水印含 " + std::to_string(unsupported) +
                               " 个非 ASCII/未收录字符，已以 '?' 呈现（v1.0.0 内置点阵仅 ASCII）");
  }
  return true;
}

}  // namespace exporter
}  // namespace lumina
