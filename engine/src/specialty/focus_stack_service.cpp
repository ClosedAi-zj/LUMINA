// 风光专项 · 景深堆栈实现：连拍识别（曝光冻结）+ 软焦点加权融合（全清晰）+ 线性 DNG / TIFF 落盘 + 产物入库
// 红线：原片零写入（只读 file_path）；解码失败 / 组失效 / 编码失败一律抛异常让 job 变 failed，
//      绝不返回一张空图或空 result 假装成功。
#include "specialty/focus_stack_service.h"
#include "specialty/compositing_common.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/logger.h"
#include "core/str_util.h"
#include "imaging/imaging.h"

namespace lumina {
namespace specialty {

namespace {

// "2026-10-22 15:47:01.123" → "20261022-154701"（产物文件名用，避免中文与冒号）
std::string compactStamp(int64_t ms) {
  const std::string text = formatTimestamp(ms);
  std::string digits;
  digits.reserve(17);
  for (char c : text) {
    if (c >= '0' && c <= '9') digits.push_back(c);
  }
  if (digits.size() < 14) return "00000000-000000";
  return digits.substr(0, 8) + "-" + digits.substr(8, 6);
}

// DNG DateTime 格式：YYYY:MM:DD HH:MM:SS
std::string dngDateTime(int64_t ms) {
  std::string text = formatTimestamp(ms > 0 ? ms : nowMs());
  if (text.size() < 19) return "1970:01:01 00:00:00";
  text = text.substr(0, 19);
  text[4] = ':';
  text[7] = ':';
  return text;
}

// 文件名数字尾号（"P0000123.JPG" → 123），用于"无 seq_id 时按文件名连续"判定
bool nameParts(const std::string& fileName, std::string* prefix, int64_t* number) {
  std::string stem = fileName;
  const size_t dot = stem.find_last_of('.');
  if (dot != std::string::npos) stem = stem.substr(0, dot);
  size_t i = stem.size();
  while (i > 0 && stem[i - 1] >= '0' && stem[i - 1] <= '9') --i;
  if (i == stem.size()) return false;
  *prefix = stem.substr(0, i);
  int64_t value = 0;
  for (size_t k = i; k < stem.size(); ++k) {
    value = value * 10 + (stem[k] - '0');
    if (value > 1000000000000LL) return false;
  }
  *number = value;
  return true;
}

// 全局焦点测度：灰度图下采样后，逐像素四邻域拉普拉斯幅度之和。值越大整体越清晰，
// 用来从连拍里挑出"最锐"的那一帧当对齐基准（曝光冻结的连拍里，对焦清晰的帧全图纹理最多）。
double globalFocusMeasure(const imaging::Image& img) {
  compositing::GrayImage g = compositing::toGray(img);
  g = compositing::halfScale(g);  // 降两级，抑制单像素噪点、突出整体清晰度
  g = compositing::halfScale(g);
  if (g.width < 3 || g.height < 3) return 0.0;
  double sum = 0.0;
  for (int y = 1; y < g.height - 1; ++y) {
    for (int x = 1; x < g.width - 1; ++x) {
      const int i = y * g.width + x;
      const double lap = std::fabs(static_cast<double>(g.pixels[i - 1]) +
                                   static_cast<double>(g.pixels[i + 1]) +
                                   static_cast<double>(g.pixels[i - g.width]) +
                                   static_cast<double>(g.pixels[i + g.width]) -
                                   4.0 * g.pixels[i]);
      sum += lap;
    }
  }
  return sum;
}

}  // namespace

// ============================ FocusStackService ============================

FocusStackService::FocusStackService(core::Config* config, db::Database* db,
                                     db::PhotoDao* photos, raw::RawDecoder* decoder)
    : config_(config), db_(db), photos_(photos), decoder_(decoder) {}

std::string FocusStackService::compositePath(int64_t groupId,
                                            const std::string& extension) const {
  const std::string dir = joinPath(config_ ? config_->cacheDir() : std::string("."), "composite");
  if (!dirExists(dir) && !makeDirs(dir)) return std::string();
  const std::string base = "stack-" + std::to_string(groupId) + "-" + compactStamp(nowMs());
  std::string path = joinPath(dir, base + "." + extension);
  for (int n = 2; n < 1000 && fileExists(path); ++n) {
    path = joinPath(dir, base + "-" + std::to_string(n) + "." + extension);
  }
  return path;
}

bool FocusStackService::decodeFrame(const PhotoRecord& record, imaging::Image* out,
                                    std::string* err) const {
  if (!decoder_) {
    if (err) *err = "RAW 解码器未就绪";
    return false;
  }
  std::string rawErr;
  if (decoder_->decodeToBitmap(record.filePath, /*half=*/false, out, &rawErr)) return true;
  std::string genericErr;
  if (imaging::decodeImageFile(record.filePath, out, &genericErr)) return true;
  if (err) {
    *err = "解码失败：" + record.filePath + "（RAW: " + rawErr + "；通用: " + genericErr + "）";
  }
  return false;
}

std::vector<FocusStackService::StackGroup> FocusStackService::detect(
    const json::JsonValue& params, std::string* err) {
  std::vector<StackGroup> groups;
  if (!db_) {
    if (err) *err = "数据库未就绪";
    return groups;
  }

  // maxGapSeconds：默认 30s，范围 (0, 600]
  double maxGapSeconds = 30.0;
  const json::JsonValue& gap = params.get("maxGapSeconds");
  if (gap.isNumber()) {
    const double v = gap.asDouble(maxGapSeconds);
    if (v <= 0.0 || v > 600.0) {
      if (err) *err = "params.maxGapSeconds 超出范围 (0, 600]：" + std::to_string(v);
      return groups;
    }
    maxGapSeconds = v;
  }
  // maxFrames：默认 15，范围 [2, 100]
  int maxFrames = 15;
  const json::JsonValue& mf = params.get("maxFrames");
  if (mf.isNumber()) {
    const int v = mf.asInt(maxFrames);
    if (v < 2 || v > 100) {
      if (err) *err = "params.maxFrames 超出范围 [2, 100]：" + std::to_string(v);
      return groups;
    }
    maxFrames = v;
  }

  // filter 只用来收窄候选集：组识别本身看真实拍摄时间与曝光，一律从 photos 现算。
  std::vector<int64_t> allowed;
  bool hasFilter = false;
  const json::JsonValue& filterValue = params.get("filter");
  if (filterValue.isObject() && !filterValue.empty()) {
    Filter filter;
    std::string filterErr;
    if (!filterFromJson(filterValue, &filter, &filterErr)) {
      if (err) *err = "params.filter 非法：" + filterErr;
      return groups;
    }
    std::string listErr;
    if (!photos_ || !photos_->listIds(filter, &allowed, &listErr)) {
      if (err) *err = "按 filter 取候选照片失败：" + listErr;
      return groups;
    }
    hasFilter = true;
    std::sort(allowed.begin(), allowed.end());
    if (allowed.empty()) return groups;
  }

  const std::string sql =
      "SELECT id,file_name,seq_id,iso,aperture,shutter,focalLength,captured_at FROM photos "
      "WHERE deleted=0 AND is_missing=0 AND captured_at>0 "
      "ORDER BY captured_at ASC, id ASC;";
  std::string dbErr;
  auto stmt = db_->prepareCached("specialty.stack.candidates", sql, &dbErr);
  if (!stmt || !stmt->valid()) {
    if (err) *err = "读取照片列表失败：" + dbErr;
    return groups;
  }

  struct Candidate {
    int64_t id = 0;
    std::string prefix;
    int64_t number = -1;
    int64_t seqId = 0;
    bool hasSeq = false;
    int iso = 0;
    std::string aperture;
    std::string shutter;
    std::string focalLength;
    int64_t capturedAt = 0;
  };
  std::vector<Candidate> candidates;
  while (stmt->step(&dbErr)) {
    const int64_t id = stmt->columnInt64(0);
    if (hasFilter && !std::binary_search(allowed.begin(), allowed.end(), id)) continue;
    Candidate c;
    c.id = id;
    c.hasSeq = !stmt->columnIsNull(2);
    c.seqId = c.hasSeq ? stmt->columnInt64(2) : 0;
    c.iso = stmt->columnInt(3);
    c.aperture = stmt->columnText(4);
    c.shutter = stmt->columnText(5);
    c.focalLength = stmt->columnText(6);
    c.capturedAt = stmt->columnInt64(7);
    nameParts(stmt->columnText(1), &c.prefix, &c.number);
    candidates.push_back(c);
  }
  stmt->reset();
  if (!dbErr.empty()) {
    if (err) *err = "遍历照片列表失败：" + dbErr;
    return groups;
  }

  const int64_t maxGapMs = static_cast<int64_t>(maxGapSeconds * 1000.0 + 0.5);
  auto sameExposure = [](const Candidate& a, const Candidate& b) -> bool {
    return a.iso == b.iso && a.aperture == b.aperture && a.shutter == b.shutter &&
           a.focalLength == b.focalLength;
  };
  auto canJoin = [&maxGapMs](const Candidate& prev, const Candidate& cur) -> bool {
    const int64_t gap = cur.capturedAt - prev.capturedAt;
    if (gap < 0 || gap > maxGapMs) return false;
    if (prev.hasSeq && cur.hasSeq && prev.seqId == cur.seqId) return true;
    if (prev.number >= 0 && cur.number == prev.number + 1 && prev.prefix == cur.prefix) return true;
    return false;
  };

  // 连拍串 → 按「曝光切换」再切段（景深堆栈要求曝光冻结）→ 按 maxFrames 切片 → 成组
  size_t start = 0;
  while (start < candidates.size()) {
    size_t end = start + 1;
    while (end < candidates.size() && canJoin(candidates[end - 1], candidates[end])) ++end;

    // 在已连拍的串内，按曝光签名变化切出"曝光冻结"的子段
    size_t seg = start;
    while (seg < end) {
      size_t segEnd = seg + 1;
      while (segEnd < end && sameExposure(candidates[segEnd], candidates[seg])) ++segEnd;
      for (size_t chunk = seg; chunk < segEnd;
           chunk += static_cast<size_t>(maxFrames)) {
        const size_t chunkEnd = std::min(segEnd, chunk + static_cast<size_t>(maxFrames));
        if (chunkEnd - chunk < 2) continue;  // 需要 ≥2 帧（推荐 ≥3，但 ≥2 即可成组）
        int64_t groupId = 0;
        StackGroup group;
        for (size_t i = chunk; i < chunkEnd; ++i) {
          group.photoIds.push_back(candidates[i].id);
          if (groupId == 0 || candidates[i].id < groupId) groupId = candidates[i].id;
        }
        group.groupId = groupId;
        groups.push_back(group);
      }
      seg = segEnd;
    }
    start = end;
  }
  return groups;
}

json::JsonValue FocusStackService::detectStackGroups(const json::JsonValue& params,
                                                    rpc::RpcError* err) {
  std::string scanErr;
  const std::vector<StackGroup> groups = detect(params, &scanErr);
  if (!scanErr.empty()) {
    *err = rpc::RpcError(rpc::kDbError, "识别景深堆栈组失败：" + scanErr);
    return json::JsonValue::nullValue();
  }

  json::JsonValue out = json::JsonValue::arrayValue();
  for (const StackGroup& group : groups) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("groupId", group.groupId);
    json::JsonValue ids = json::JsonValue::arrayValue();
    for (int64_t id : group.photoIds) ids.addItem(id);
    item.setItem("photoIds", ids);
    item.setItem("frameCount", static_cast<int64_t>(group.photoIds.size()));
    out.addItem(item);
  }
  return out;
}

json::JsonValue FocusStackService::composite(const json::JsonValue& params, JobManager* jobs,
                                            rpc::RpcError* err) {
  if (!jobs) {
    *err = rpc::RpcError(rpc::kInternalError, "任务管理器未就绪");
    return json::JsonValue::nullValue();
  }
  if (!photos_ || !decoder_ || !config_) {
    *err = rpc::RpcError(rpc::kInternalError, "景深堆栈服务未就绪（依赖缺失）");
    return json::JsonValue::nullValue();
  }
  const int64_t groupId = params.get("groupId").asInt(0);
  if (groupId <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.groupId 缺失或非法：" + std::to_string(groupId));
    return json::JsonValue::nullValue();
  }

  FocusStackOptions options;
  const json::JsonValue& optionValue = params.get("options");
  if (!optionValue.isNull() && !optionValue.isObject()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.options 必须是对象");
    return json::JsonValue::nullValue();
  }
  if (optionValue.isObject()) {
    options.autoAlign = optionValue.get("autoAlign").asBool(true);
    options.smoothness = optionValue.get("smoothness").asDouble(4.0);
    if (!std::isfinite(options.smoothness) || options.smoothness < 0.0) options.smoothness = 4.0;
    const std::string format = toLowerAscii(optionValue.get("outputFormat").asString("dng"));
    if (format != "dng" && format != "tiff") {
      *err = rpc::RpcError(rpc::kInvalidParams,
                           "options.outputFormat 仅支持 dng | tiff，收到：" + format);
      return json::JsonValue::nullValue();
    }
    options.outputFormat = format;
  }

  // 组 ID 是组内最小 photoId；重新识别一遍保证"合成的一定是当前库里真实存在的景深堆栈组"
  std::string scanErr;
  const std::vector<StackGroup> groups = detect(params, &scanErr);
  if (!scanErr.empty()) {
    *err = rpc::RpcError(rpc::kDbError, "识别景深堆栈组失败：" + scanErr);
    return json::JsonValue::nullValue();
  }
  std::vector<int64_t> photoIds;
  for (const StackGroup& group : groups) {
    if (group.groupId == groupId) {
      photoIds = group.photoIds;
      break;
    }
  }
  if (photoIds.size() < 2) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "景深堆栈组不存在或已失效（groupId=" + std::to_string(groupId) +
                             "），请重新调用 stack.detectStackGroups 后再合成");
    return json::JsonValue::nullValue();
  }
  for (int64_t photoId : photoIds) {
    PhotoRecord record;
    std::string dbErr;
    if (!photos_->getById(photoId, &record, &dbErr)) {
      *err = rpc::RpcError(rpc::kFileNotFound,
                           "景深堆栈组内照片不存在（photoId=" + std::to_string(photoId) +
                               "）：" + (dbErr.empty() ? std::string("无此记录") : dbErr));
      return json::JsonValue::nullValue();
    }
    if (record.isMissing || !fileExists(record.filePath)) {
      *err = rpc::RpcError(rpc::kFileNotFound,
                           "景深堆栈组内照片源文件缺失（photoId=" + std::to_string(photoId) +
                               "）：" + record.filePath);
      return json::JsonValue::nullValue();
    }
  }

  const int64_t jobId = jobs->start("stack.composite", [this, photoIds, options](JobContext& ctx) {
    runStack(photoIds, options, ctx);
  });
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("jobId", jobId);
  return out;
}

void FocusStackService::runStack(const std::vector<int64_t>& photoIds,
                                 const FocusStackOptions& options, JobContext& ctx) {
  if (!photos_ || !decoder_ || !config_) throw std::runtime_error("景深堆栈服务未就绪（依赖缺失）");
  const int total = static_cast<int>(photoIds.size());
  if (total < 2) throw std::runtime_error("景深堆栈至少需要 2 帧，当前 " + std::to_string(total) + " 帧");

  // ---- 1. 读取记录 + 全分辨率解码 ----
  std::vector<PhotoRecord> records(static_cast<size_t>(total));
  std::vector<imaging::Image> frames(static_cast<size_t>(total));

  for (int i = 0; i < total; ++i) {
    if (ctx.isCancelled()) return;
    PhotoRecord record;
    std::string dbErr;
    if (!photos_->getById(photoIds[static_cast<size_t>(i)], &record, &dbErr)) {
      throw std::runtime_error("读取照片失败（photoId=" + std::to_string(photoIds[static_cast<size_t>(i)]) +
                               "）：" + (dbErr.empty() ? std::string("无此记录") : dbErr));
    }
    records[static_cast<size_t>(i)] = record;
    ctx.progress(2 + 26 * i / std::max(1, total),
                 "解码第 " + std::to_string(i + 1) + "/" + std::to_string(total) + " 帧");
    std::string decodeErr;
    if (!decodeFrame(record, &frames[static_cast<size_t>(i)], &decodeErr)) {
      throw std::runtime_error("第 " + std::to_string(i + 1) + " 帧" + decodeErr);
    }
    if (frames[static_cast<size_t>(i)].width <= 0 || frames[static_cast<size_t>(i)].height <= 0) {
      throw std::runtime_error("第 " + std::to_string(i + 1) + " 帧解码结果为空：" + record.filePath);
    }
  }
  if (ctx.isCancelled()) return;

  // ---- 2. 参考帧 = 全局焦点测度最高的帧（最锐），作为对齐基准 ----
  int refIndex = 0;
  {
    double best = -1.0;
    for (int i = 0; i < total; ++i) {
      const double fm = globalFocusMeasure(frames[static_cast<size_t>(i)]);
      if (fm > best) {
        best = fm;
        refIndex = i;
      }
    }
  }
  const imaging::Image& ref = frames[static_cast<size_t>(refIndex)];

  // 尺寸不一致时统一到参考帧尺寸（曝光冻结的真实连拍不会不一致；不一致说明混入了别的素材，
  // 缩放而不是报错能保住结果，但会记 warning）
  std::string warning;
  for (int i = 0; i < total; ++i) {
    imaging::Image& frame = frames[static_cast<size_t>(i)];
    if (frame.width == ref.width && frame.height == ref.height) continue;
    imaging::Image scaled;
    if (!imaging::resizeImage(frame, &scaled, ref.width, ref.height)) {
      throw std::runtime_error("第 " + std::to_string(i + 1) + " 帧尺寸归一化失败");
    }
    if (warning.empty()) {
      warning = "组内帧尺寸不一致，已统一到参考帧 " + std::to_string(ref.width) + "x" +
                std::to_string(ref.height);
    }
    frame = scaled;
  }
  ctx.progress(30, "解码完成");

  // ---- 3. 平移对齐（autoAlign）----
  std::vector<compositing::FrameView> views(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i) {
    views[static_cast<size_t>(i)].image = &frames[static_cast<size_t>(i)];
  }
  if (options.autoAlign) {
    const compositing::GrayImage grayRef = compositing::toGray(ref);
    int maxShift = 0;
    for (int i = 0; i < total; ++i) {
      if (ctx.isCancelled()) return;
      if (i == refIndex) continue;
      const compositing::GrayImage grayTarget =
          compositing::toGray(frames[static_cast<size_t>(i)]);
      int dx = 0;
      int dy = 0;
      compositing::estimateShift(grayRef, grayTarget, compositing::kOutputMaxShift, &dx, &dy);
      views[static_cast<size_t>(i)].offsetX = dx;
      views[static_cast<size_t>(i)].offsetY = dy;
      maxShift = std::max(maxShift, std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy));
      ctx.progress(30 + 15 * (i + 1) / std::max(1, total),
                   "对齐第 " + std::to_string(i + 1) + "/" + std::to_string(total) + " 帧");
    }
    logInfo("specialty", "stack job " + std::to_string(ctx.jobId()) + " aligned, maxShift=" +
                             std::to_string(maxShift) + "px");
  }

  // ---- 4. 软焦点加权融合（得到全清晰图）----
  ctx.progress(45, "开始景深融合");
  imaging::Image fused;
  bool cancelled = false;
  compositing::focusStackFrames(views, options.smoothness, &ctx, &fused, &cancelled);
  if (cancelled || ctx.isCancelled()) return;
  if (fused.width <= 0 || fused.height <= 0) throw std::runtime_error("景深融合产出为空");

  // ---- 5. 落盘 ----
  ctx.progress(86, "写出合成结果");
  int64_t groupId = photoIds.front();
  for (int64_t id : photoIds) groupId = std::min(groupId, id);
  const std::string extension = options.outputFormat == "tiff" ? "tif" : "dng";
  const std::string path = compositePath(groupId, extension);
  if (path.empty()) throw std::runtime_error("无法创建合成输出目录（{home}/cache/composite）");
  std::string encodeErr;
  const bool encoded =
      options.outputFormat == "tiff"
          ? compositing::writeRgb8Tiff(path, fused, &encodeErr)
          : compositing::writeLinearRgbDng(
                path, fused, dngDateTime(records[static_cast<size_t>(refIndex)].capturedAt),
                &encodeErr);
  if (!encoded) {
    removeFile(path);
    throw std::runtime_error("合成结果编码失败：" + (encodeErr.empty() ? path : encodeErr));
  }
  if (fileSize(path) <= 0) {
    removeFile(path);
    throw std::runtime_error("合成结果文件为空：" + path);
  }

  // ---- 6. 产物入索引（图库可见 + 可进修图台）----
  const PhotoRecord& refRecord = records[static_cast<size_t>(refIndex)];
  PhotoRecord product;
  // photos.folder_id / root_id 是开启的外键（PRAGMA foreign_keys=ON），写 0 会直接
  // FOREIGN KEY constraint failed。合成产物沿用参考帧的归属（图库里和原片同组），
  // 参考帧没有归属时（异常数据）才按缓存目录登记一个专用根/文件夹。
  product.folderId = refRecord.folderId;
  product.rootId = refRecord.rootId;
  if (product.folderId <= 0 || product.rootId <= 0) {
    std::string ensureErr;
    const std::string compositeDir = joinPath(config_->cacheDir(), "composite");
    const int64_t rootId = photos_->ensureRoot(compositeDir, "Focus Stack 合成", &ensureErr);
    const int64_t folderId =
        rootId > 0 ? photos_->ensureFolder(rootId, "", "Focus Stack 合成", &ensureErr) : 0;
    if (rootId <= 0 || folderId <= 0) {
      removeFile(path);
      throw std::runtime_error("合成产物入库失败：无法登记归属文件夹（" + ensureErr + "）");
    }
    product.rootId = rootId;
    product.folderId = folderId;
  }
  product.fileName = baseName(path);
  product.filePath = path;
  product.format = options.outputFormat == "tiff" ? "TIFF" : "DNG";
  product.fileSize = fileSize(path);
  product.width = fused.width;
  product.height = fused.height;
  product.capturedAt = refRecord.capturedAt;
  product.importedAt = nowMs();
  product.camera = refRecord.camera;
  product.lens = refRecord.lens;
  product.iso = refRecord.iso;
  product.aperture = refRecord.aperture;
  product.shutter = refRecord.shutter;
  product.focalLength = refRecord.focalLength;
  product.whiteBalance = refRecord.whiteBalance;
  product.location = refRecord.location;
  product.date = refRecord.capturedAt > 0 ? formatMonth(refRecord.capturedAt) : refRecord.date;
  product.sidecarPath = replaceExtension(path, "xmp");
  product.pairKind = "single";
  product.health = "ok";
  product.hasGps = refRecord.hasGps;
  product.gpsLat = refRecord.gpsLat;
  product.gpsLng = refRecord.gpsLng;
  product.gpsAlt = refRecord.gpsAlt;
  product.hasLocationId = refRecord.hasLocationId;
  product.locationId = refRecord.locationId;

  int64_t newPhotoId = 0;
  std::string upsertErr;
  if (!photos_->upsert(product, &newPhotoId, &upsertErr) || newPhotoId <= 0) {
    // 入库失败不能把文件留在缓存里冒充"完成"，也不能静默成功
    removeFile(path);
    throw std::runtime_error("合成产物入库失败：" +
                             (upsertErr.empty() ? std::string("未返回 photoId") : upsertErr));
  }

  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("photoId", newPhotoId);
  result.setItem("path", path);
  result.setItem("groupId", groupId);
  result.setItem("frames", static_cast<int64_t>(total));
  result.setItem("outputFormat", options.outputFormat);
  result.setItem("autoAlign", options.autoAlign);
  result.setItem("smoothness", options.smoothness);
  if (!warning.empty()) result.setItem("warning", warning);
  ctx.setResult(result);
  logInfo("specialty", "stack job " + std::to_string(ctx.jobId()) + " done: photoId=" +
                           std::to_string(newPhotoId) + " frames=" + std::to_string(total) +
                           " format=" + options.outputFormat + " path=" + path);
}

}  // namespace specialty
}  // namespace lumina
