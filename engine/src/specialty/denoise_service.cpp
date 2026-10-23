// 风光专项 · 降噪实现：真 NLM（非局部均值）+ 亮度蒙版 + 非破坏 sidecar
// 红线：
//   1) 原片零写入：只读 file_path，结果落 {home}/cache/denoise/。
//   2) 不许用均值/高斯模糊冒充 NLM：权重必须来自「块间均方差」的指数衰减。
//   3) 零静默：解不出来 / rect 越界 / strength 越界 / sidecar 写失败，一律抛异常或返回带中文 message 的 RpcError。
#include "specialty/denoise_service.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/jpeg_encoder.h"
#include "core/logger.h"
#include "core/str_util.h"
#include "specialty/hdr_service.h"  // writeRgb8Tiff：与 HDR 产物共用同一套 TIFF 落地
#include "xmp/xmp_document.h"

namespace lumina {
namespace specialty {

namespace {

// ---- NLM 常规参数 ----
constexpr int kPatchRadius = 3;    // 相似块 7x7
constexpr int kSearchRadius = 10;  // 搜索窗 21x21
constexpr size_t kLutSize = 65026;  // 块均方差上限 255²
constexpr double kWeakHFactor = 0.35;  // 亮部（亮度蒙版外）的 h 缩放
constexpr double kPreviewMaxPixels = 1200000.0;  // 预览参与 NLM 的最大像素数

struct NlmOptions {
  double h = 12.0;             // 滤波强度（8bit 灰度域）
  bool luminanceMask = false;  // 亮度蒙版
};

uint8_t clampByte(double v) {
  if (v <= 0.0) return 0;
  if (v >= 255.0) return 255;
  return static_cast<uint8_t>(v + 0.5);
}

// 亮度蒙版权重：1 = 暗部（满强度），0 = 亮部（弱强度）
float maskWeight(uint8_t lum) {
  if (lum <= 48) return 1.0f;
  if (lum >= 160) return 0.0f;
  return static_cast<float>(160.0 - static_cast<double>(lum)) / 112.0f;
}

void buildLut(std::vector<float>* lut, double h) {
  lut->resize(kLutSize);
  const double h2 = h * h > 1e-9 ? h * h : 1e-9;
  for (size_t i = 0; i < kLutSize; ++i) {
    (*lut)[i] = static_cast<float>(std::exp(-static_cast<double>(i) / h2));
  }
}

// 真 NLM：对每个像素，在 21x21 搜索窗内按 7x7 块的均方差给邻居加权。
// 实现用「按偏移量的块均方差 + 可分离盒式滑动和」把复杂度从 O(N·441·49) 降到 O(N·441)。
// 返回 false 表示被取消。
bool nlmDenoise(const imaging::Image& in, imaging::Image* out, const NlmOptions& opt,
                JobContext* ctx) {
  const int width = in.width;
  const int height = in.height;
  if (width <= 0 || height <= 0) return false;
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (in.pixels.size() < pixels * 3) return false;

  const int p = kPatchRadius;
  const int r = kSearchRadius;
  const int span = 2 * p + 1;
  const int spanArea = span * span;

  std::vector<uint8_t> lum(pixels);
  std::vector<float> mask;
  if (opt.luminanceMask) mask.resize(pixels);
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t* px = &in.pixels[i * 3];
    const uint8_t l =
        static_cast<uint8_t>((77 * static_cast<int>(px[0]) + 150 * static_cast<int>(px[1]) +
                              29 * static_cast<int>(px[2])) >> 8);
    lum[i] = l;
    if (opt.luminanceMask) mask[i] = maskWeight(l);
  }

  std::vector<float> lutFull;
  std::vector<float> lutWeak;
  buildLut(&lutFull, opt.h);
  buildLut(&lutWeak, std::max(1.0, opt.h * kWeakHFactor));

  out->width = width;
  out->height = height;
  out->pixels.assign(pixels * 3, 0);

  const int bandRows = std::max(16, std::min(height, 262144 / std::max(1, width)));
  const int bandCount = (height + bandRows - 1) / bandRows;

  unsigned hardware = std::thread::hardware_concurrency();
  int threads = static_cast<int>(hardware == 0 ? 4u : hardware);
  threads = std::max(1, std::min(threads, 8));
  threads = std::min(threads, bandCount);

  std::atomic<int> nextBand(0);
  std::atomic<int> doneBands(0);
  std::atomic<bool> stop(false);

  auto worker = [&]() {
    std::vector<int> diff(static_cast<size_t>(bandRows + 2 * p) * width, 0);
    std::vector<int> hsum(static_cast<size_t>(bandRows + 2 * p) * width, 0);
    std::vector<int> vsum(width, 0);
    std::vector<float> accR;
    std::vector<float> accG;
    std::vector<float> accB;
    std::vector<float> wsum;

    for (;;) {
      const int band = nextBand.fetch_add(1);
      if (band >= bandCount || stop.load()) return;
      if (ctx != nullptr && ctx->isCancelled()) {
        stop.store(true);
        return;
      }
      const int y0 = band * bandRows;
      const int y1 = std::min(height, y0 + bandRows);
      const int bandHeight = y1 - y0;
      const size_t bandPixels = static_cast<size_t>(bandHeight) * width;

      // 自权重先置 1（等于把自身像素以权重 1 计入），保证边界像素 wsum 不会为 0
      accR.resize(bandPixels);
      accG.resize(bandPixels);
      accB.resize(bandPixels);
      wsum.assign(bandPixels, 1.0f);
      for (int y = 0; y < bandHeight; ++y) {
        for (int x = 0; x < width; ++x) {
          const size_t si = (static_cast<size_t>(y0 + y) * width + x) * 3;
          const size_t di = static_cast<size_t>(y) * width + x;
          accR[di] = static_cast<float>(in.pixels[si + 0]);
          accG[di] = static_cast<float>(in.pixels[si + 1]);
          accB[di] = static_cast<float>(in.pixels[si + 2]);
        }
      }

      for (int dy = -r; dy <= r; ++dy) {
        if (stop.load()) return;
        if (ctx != nullptr && ctx->isCancelled()) {
          stop.store(true);
          return;
        }
        for (int dx = -r; dx <= r; ++dx) {
          if (dx == 0 && dy == 0) continue;  // 自身已在自权重里
          // 只处理「块与平移块都完整落在图内」的像素，避免边界补零污染权值
          const int xLo = std::max(p, p - dx);
          const int xHi = std::min(width - p, width - p - dx);
          const int yLo = std::max(p, p - dy);
          const int yHi = std::min(height - p, height - p - dy);
          if (xHi - xLo <= 2 * p) continue;
          if (yHi - yLo <= 2 * p) continue;
          const int ys = std::max(yLo, y0 - p);
          const int ye = std::min(yHi, y1 + p);
          if (ye - ys <= 2 * p) continue;

          // 1) 块内逐像素平方差
          for (int y = ys; y < ye; ++y) {
            const uint8_t* a = &lum[static_cast<size_t>(y) * width];
            const uint8_t* b = &lum[static_cast<size_t>(y + dy) * width + dx];
            int* d = &diff[static_cast<size_t>(y - ys) * width];
            for (int x = xLo; x < xHi; ++x) {
              const int t = static_cast<int>(a[x]) - static_cast<int>(b[x]);
              d[x] = t * t;
            }
          }
          // 2) 水平方向滑动求和（半径 p）
          for (int y = ys; y < ye; ++y) {
            const int* d = &diff[static_cast<size_t>(y - ys) * width];
            int* hs = &hsum[static_cast<size_t>(y - ys) * width];
            int sum = 0;
            for (int x = xLo; x < xLo + span; ++x) sum += d[x];
            hs[xLo + p] = sum;
            for (int x = xLo + p + 1; x < xHi - p; ++x) {
              sum += d[x + p] - d[x - p - 1];
              hs[x] = sum;
            }
          }
          // 3) 垂直方向滑动求和 → 7x7 块均方差 → 权重
          const int oy0 = std::max(ys + p, y0);
          const int oy1 = std::min(ye - p, y1);
          if (oy1 <= oy0) continue;
          std::fill(vsum.begin() + xLo + p, vsum.begin() + (xHi - p), 0);
          for (int k = -p; k <= p; ++k) {
            const int* hs = &hsum[static_cast<size_t>(oy0 + k - ys) * width];
            for (int x = xLo + p; x < xHi - p; ++x) vsum[x] += hs[x];
          }
          for (int y = oy0; y < oy1; ++y) {
            const size_t rowBase = static_cast<size_t>(y - y0) * width;
            const size_t srcBase = static_cast<size_t>(y + dy) * width + dx;
            const float* maskRow = opt.luminanceMask ? &mask[static_cast<size_t>(y) * width] : nullptr;
            for (int x = xLo + p; x < xHi - p; ++x) {
              const int index = vsum[x] / spanArea;
              double w = lutFull[static_cast<size_t>(index)];
              if (maskRow != nullptr) {
                const double weak = lutWeak[static_cast<size_t>(index)];
                const double m = maskRow[x];
                w = weak + (w - weak) * m;  // 暗部取满强度，亮部取弱强度
              }
              const size_t di = rowBase + x;
              const size_t si = (srcBase + x) * 3;
              accR[di] += static_cast<float>(w * in.pixels[si + 0]);
              accG[di] += static_cast<float>(w * in.pixels[si + 1]);
              accB[di] += static_cast<float>(w * in.pixels[si + 2]);
              wsum[di] += static_cast<float>(w);
            }
            if (y + 1 < oy1) {
              const int* addRow = &hsum[static_cast<size_t>(y + 1 + p - ys) * width];
              const int* subRow = &hsum[static_cast<size_t>(y - p - ys) * width];
              for (int x = xLo + p; x < xHi - p; ++x) vsum[x] += addRow[x] - subRow[x];
            }
          }
        }
      }

      for (int y = 0; y < bandHeight; ++y) {
        for (int x = 0; x < width; ++x) {
          const size_t di = static_cast<size_t>(y) * width + x;
          const double ws = wsum[di] > 1e-6f ? static_cast<double>(wsum[di]) : 1.0;
          const size_t oi = (static_cast<size_t>(y0 + y) * width + x) * 3;
          out->pixels[oi + 0] = clampByte(static_cast<double>(accR[di]) / ws);
          out->pixels[oi + 1] = clampByte(static_cast<double>(accG[di]) / ws);
          out->pixels[oi + 2] = clampByte(static_cast<double>(accB[di]) / ws);
        }
      }

      const int done = doneBands.fetch_add(1) + 1;
      if (ctx != nullptr) {
        ctx->progress(2 + 78 * done / std::max(1, bandCount),
                      "NLM 降噪 " + std::to_string(done) + "/" + std::to_string(bandCount) +
                          " 行带");
      }
    }
  };

  std::vector<std::thread> pool;
  for (int i = 0; i < threads - 1; ++i) pool.emplace_back(worker);
  worker();
  for (std::thread& t : pool) t.join();
  return !stop.load();
}

// 从源图裁一块（纯拷贝，不做插值）
imaging::Image cropImage(const imaging::Image& src, int x0, int y0, int w, int h) {
  imaging::Image out;
  out.width = w;
  out.height = h;
  out.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 0);
  for (int y = 0; y < h; ++y) {
    const uint8_t* srcRow = &src.pixels[(static_cast<size_t>(y0 + y) * src.width + x0) * 3];
    uint8_t* dstRow = &out.pixels[static_cast<size_t>(y) * w * 3];
    for (int i = 0; i < w * 3; ++i) dstRow[i] = srcRow[i];
  }
  return out;
}

}  // namespace

DenoiseService::DenoiseService(core::Config* config, db::PhotoDao* photos, raw::RawDecoder* decoder,
                               xmp::XmpService* xmp)
    : config_(config), photos_(photos), decoder_(decoder), xmp_(xmp) {}

std::string DenoiseService::cachePath(int64_t photoId, int strength) const {
  const std::string dir = joinPath(config_ ? config_->cacheDir() : std::string("."), "denoise");
  if (!dirExists(dir) && !makeDirs(dir)) return std::string();
  return joinPath(dir, std::to_string(photoId) + "-" + std::to_string(strength) + ".tiff");
}

std::string DenoiseService::previewPath(int64_t photoId) const {
  const std::string dir = joinPath(config_ ? config_->cacheDir() : std::string("."), "denoise");
  if (!dirExists(dir) && !makeDirs(dir)) return std::string();
  return joinPath(dir, "preview-" + std::to_string(photoId) + ".jpg");
}

bool DenoiseService::decodePhoto(const PhotoRecord& record, imaging::Image* out,
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

// crs:LuminanceSmoothing（LR 可读）+ lumina:DenoiseStrength / DenoiseLuminanceMask（本引擎可读）。
// 前两个走 XmpService 的唯一写入口（含外部修改检测与 per-photo 锁），
// lumina: 两个字段映射表尚未收录，这里沿用 XmpService::setExifGps 的同一手法
// （解析 → 只改这两个节点 → 原子写 → 回填 sidecar mtime），不改动共享的 xmp_mapper。
bool DenoiseService::writeDerivedParams(int64_t photoId, int strength, bool luminanceMask,
                                        std::string* err) {
  if (!xmp_) {
    if (err) *err = "XMP 服务未就绪";
    return false;
  }
  json::JsonValue params = json::JsonValue::objectValue();
  params.setItem("luminanceSmoothing", strength);  // 与 LR 同量纲 0..100
  std::string saveErr;
  if (!xmp_->saveParams(photoId, params, &saveErr)) {
    if (err) *err = saveErr.empty() ? std::string("saveParams 失败") : saveErr;
    return false;
  }

  PhotoRecord record;
  std::string dbErr;
  if (!photos_->getById(photoId, &record, &dbErr)) {
    if (err) *err = "读取照片失败：" + (dbErr.empty() ? std::string("无此记录") : dbErr);
    return false;
  }
  const std::string sidecar =
      record.sidecarPath.empty() ? xmp_->sidecarPathFor(record.filePath) : record.sidecarPath;
  std::string bytes;
  if (!fileExists(sidecar)) {
    if (err) *err = "sidecar 不存在：" + sidecar;
    return false;
  }
  if (!readFile(sidecar, &bytes, err)) return false;

  std::string parseErr;
  std::unique_ptr<xmp::XmpDocument> doc = xmp::XmpDocument::parse(bytes, &parseErr);
  if (!doc) {
    if (err) *err = "sidecar 解析失败：" + sidecar + "（" + parseErr + "）";
    return false;
  }
  doc->setText("lumina", "DenoiseStrength", std::to_string(strength));
  doc->setText("lumina", "DenoiseLuminanceMask", luminanceMask ? "True" : "False");
  const std::string output = doc->serialize();
  std::string writeErr;
  if (!writeFileAtomic(sidecar, output, &writeErr)) {
    if (err) *err = writeErr.empty() ? ("写入失败：" + sidecar) : writeErr;
    return false;
  }
  std::string mtimeErr;
  if (!photos_->setSidecar(photoId, sidecar, fileMtimeMs(sidecar), /*hasEdits=*/true, &mtimeErr)) {
    if (err) *err = "sidecar 已写入，但索引 mtime 回填失败：" + mtimeErr;
    return false;
  }
  return true;
}

json::JsonValue DenoiseService::apply(const json::JsonValue& params, JobManager* jobs,
                                      rpc::RpcError* err) {
  if (!jobs) {
    *err = rpc::RpcError(rpc::kInternalError, "任务管理器未就绪");
    return json::JsonValue::nullValue();
  }
  if (!photos_ || !decoder_ || !config_ || !xmp_) {
    *err = rpc::RpcError(rpc::kInternalError, "降噪服务未就绪（依赖缺失）");
    return json::JsonValue::nullValue();
  }
  const int64_t photoId = params.get("photoId").asInt(0);
  if (photoId <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.photoId 缺失或非法：" + std::to_string(photoId));
    return json::JsonValue::nullValue();
  }
  if (!params.contains("strength")) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.strength 必填（0..100）");
    return json::JsonValue::nullValue();
  }
  const int strength = static_cast<int>(params.get("strength").asInt(50));
  if (strength < 0 || strength > 100) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "params.strength 超出范围 [0, 100]：" + std::to_string(strength));
    return json::JsonValue::nullValue();
  }
  const bool luminanceMask = params.get("luminanceMask").asBool(false);

  PhotoRecord record;
  std::string dbErr;
  if (!photos_->getById(photoId, &record, &dbErr)) {
    *err = rpc::RpcError(rpc::kFileNotFound,
                         "照片不存在（photoId=" + std::to_string(photoId) +
                             "）：" + (dbErr.empty() ? std::string("无此记录") : dbErr));
    return json::JsonValue::nullValue();
  }
  if (record.isMissing || !fileExists(record.filePath)) {
    *err = rpc::RpcError(rpc::kFileNotFound, "照片源文件缺失：" + record.filePath);
    return json::JsonValue::nullValue();
  }

  const int64_t jobId = jobs->start("denoise.apply", [this, photoId, strength, luminanceMask](
                                                         JobContext& ctx) {
    runApply(photoId, strength, luminanceMask, ctx);
  });
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("jobId", jobId);
  return out;
}

void DenoiseService::runApply(int64_t photoId, int strength, bool luminanceMask, JobContext& ctx) {
  if (!photos_ || !decoder_ || !config_ || !xmp_) {
    throw std::runtime_error("降噪服务未就绪（依赖缺失）");
  }
  PhotoRecord record;
  std::string dbErr;
  if (!photos_->getById(photoId, &record, &dbErr)) {
    throw std::runtime_error("读取照片失败（photoId=" + std::to_string(photoId) +
                             "）：" + (dbErr.empty() ? std::string("无此记录") : dbErr));
  }
  if (record.isMissing || !fileExists(record.filePath)) {
    throw std::runtime_error("照片源文件缺失：" + record.filePath);
  }

  ctx.progress(2, "解码原片");
  imaging::Image image;
  std::string decodeErr;
  if (!decodePhoto(record, &image, &decodeErr)) throw std::runtime_error(decodeErr);
  if (ctx.isCancelled()) return;

  NlmOptions options;
  options.h = 1.0 + 0.24 * static_cast<double>(strength);  // strength 0..100 → h 1..25
  options.luminanceMask = luminanceMask;
  ctx.progress(5, "NLM 降噪开始（" + std::to_string(image.width) + "x" +
                      std::to_string(image.height) + "，h=" + std::to_string(options.h) + "）");
  imaging::Image denoised;
  if (!nlmDenoise(image, &denoised, options, &ctx)) {
    if (ctx.isCancelled()) return;
    throw std::runtime_error("NLM 降噪失败（输入图像非法）");
  }
  if (ctx.isCancelled()) return;

  const std::string path = cachePath(photoId, strength);
  if (path.empty()) {
    throw std::runtime_error("无法创建降噪缓存目录（{home}/cache/denoise）");
  }
  ctx.progress(84, "写出降噪缓存");
  std::string encodeErr;
  if (!writeRgb8Tiff(path, denoised, &encodeErr)) {
    removeFile(path);
    throw std::runtime_error("降噪结果编码失败：" + (encodeErr.empty() ? path : encodeErr));
  }
  if (fileSize(path) <= 0) {
    removeFile(path);
    throw std::runtime_error("降噪结果文件为空：" + path);
  }

  ctx.progress(92, "写入 sidecar 参数");
  std::string sidecarErr;
  if (!writeDerivedParams(photoId, strength, luminanceMask, &sidecarErr)) {
    throw std::runtime_error("降噪结果已写入缓存（" + path +
                             "），但参数写入 sidecar 失败：" + sidecarErr);
  }

  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("photoId", photoId);
  result.setItem("path", path);
  result.setItem("strength", strength);
  result.setItem("luminanceMask", luminanceMask);
  result.setItem("h", options.h);
  result.setItem("width", image.width);
  result.setItem("height", image.height);
  result.setItem("sidecarPath", record.sidecarPath);
  ctx.setResult(result);
  logInfo("specialty", "denoise job " + std::to_string(ctx.jobId()) + " done: photoId=" +
                           std::to_string(photoId) + " strength=" + std::to_string(strength) +
                           " mask=" + (luminanceMask ? "true" : "false") + " path=" + path);
}

json::JsonValue DenoiseService::preview(const json::JsonValue& params, rpc::RpcError* err) {
  if (!photos_ || !decoder_ || !config_) {
    *err = rpc::RpcError(rpc::kInternalError, "降噪服务未就绪（依赖缺失）");
    return json::JsonValue::nullValue();
  }
  const int64_t photoId = params.get("photoId").asInt(0);
  if (photoId <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.photoId 缺失或非法：" + std::to_string(photoId));
    return json::JsonValue::nullValue();
  }
  if (!params.contains("strength")) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.strength 必填（0..100）");
    return json::JsonValue::nullValue();
  }
  const int strength = static_cast<int>(params.get("strength").asInt(50));
  if (strength < 0 || strength > 100) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "params.strength 超出范围 [0, 100]：" + std::to_string(strength));
    return json::JsonValue::nullValue();
  }

  const json::JsonValue& rect = params.get("rect");
  if (!rect.isObject()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.rect 必须是 {x,y,w,h} 对象");
    return json::JsonValue::nullValue();
  }
  static const char* kRectKeys[] = {"x", "y", "w", "h"};
  for (const char* key : kRectKeys) {
    if (!rect.contains(key) || !rect.get(key).isNumber()) {
      *err = rpc::RpcError(rpc::kInvalidParams,
                           std::string("params.rect.") + key + " 缺失或不是数字");
      return json::JsonValue::nullValue();
    }
  }
  const double rx = rect.get("x").asDouble();
  const double ry = rect.get("y").asDouble();
  const double rw = rect.get("w").asDouble();
  const double rh = rect.get("h").asDouble();
  if (rw <= 0.0 || rh <= 0.0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.rect 宽高必须为正数");
    return json::JsonValue::nullValue();
  }

  PhotoRecord record;
  std::string dbErr;
  if (!photos_->getById(photoId, &record, &dbErr)) {
    *err = rpc::RpcError(rpc::kFileNotFound,
                         "照片不存在（photoId=" + std::to_string(photoId) +
                             "）：" + (dbErr.empty() ? std::string("无此记录") : dbErr));
    return json::JsonValue::nullValue();
  }
  if (record.isMissing || !fileExists(record.filePath)) {
    *err = rpc::RpcError(rpc::kFileNotFound, "照片源文件缺失：" + record.filePath);
    return json::JsonValue::nullValue();
  }

  imaging::Image image;
  std::string decodeErr;
  if (!decodePhoto(record, &image, &decodeErr)) {
    *err = rpc::RpcError(rpc::kRawDecodeFailed, decodeErr);
    return json::JsonValue::nullValue();
  }

  // rect 约定：全部 ≤ 1 视为归一化比例（UI 传 0.25/0.25/0.5/0.5），否则按像素处理
  const bool normalized = rx >= 0.0 && ry >= 0.0 && rx <= 1.0 && ry <= 1.0 && rw <= 1.0 &&
                          rh <= 1.0;
  int px = 0;
  int py = 0;
  int pw = 0;
  int ph = 0;
  if (normalized) {
    if (rx + rw > 1.0001 || ry + rh > 1.0001) {
      *err = rpc::RpcError(rpc::kInvalidParams,
                           "params.rect 归一化越界：x+w 与 y+h 必须 ≤ 1");
      return json::JsonValue::nullValue();
    }
    px = static_cast<int>(rx * image.width + 0.5);
    py = static_cast<int>(ry * image.height + 0.5);
    pw = static_cast<int>(rw * image.width + 0.5);
    ph = static_cast<int>(rh * image.height + 0.5);
  } else {
    px = static_cast<int>(rx + 0.5);
    py = static_cast<int>(ry + 0.5);
    pw = static_cast<int>(rw + 0.5);
    ph = static_cast<int>(rh + 0.5);
  }
  if (pw <= 0 || ph <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.rect 太小：换算后宽高为 0");
    return json::JsonValue::nullValue();
  }
  if (px < 0 || py < 0 || px + pw > image.width || py + ph > image.height) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "params.rect 越界：region=(%d,%d,%d,%d) 超出图幅 %dx%d", px, py,
                  pw, ph, image.width, image.height);
    *err = rpc::RpcError(rpc::kInvalidParams, buf);
    return json::JsonValue::nullValue();
  }

  // 带一圈 halo 再做 NLM，避免裁剪边界因缺少邻居而出现亮/暗边
  const int halo = kPatchRadius + kSearchRadius;
  const int ex0 = std::max(0, px - halo);
  const int ey0 = std::max(0, py - halo);
  const int ex1 = std::min(image.width, px + pw + halo);
  const int ey1 = std::min(image.height, py + ph + halo);
  imaging::Image work = cropImage(image, ex0, ey0, ex1 - ex0, ey1 - ey0);

  // 预览是「小样」：参与 NLM 的像素封顶，超出先等比缩小（否则一张大图会拖过 UI 超时）
  double scale = 1.0;
  const double area = static_cast<double>(work.width) * static_cast<double>(work.height);
  if (area > kPreviewMaxPixels) {
    scale = std::sqrt(kPreviewMaxPixels / area);
    const int targetW = std::max(1, static_cast<int>(work.width * scale + 0.5));
    const int targetH = std::max(1, static_cast<int>(work.height * scale + 0.5));
    imaging::Image scaled;
    if (!imaging::resizeImage(work, &scaled, targetW, targetH)) {
      *err = rpc::RpcError(rpc::kInternalError, "预览区域缩小失败");
      return json::JsonValue::nullValue();
    }
    work = scaled;
  }
  const int ix = std::max(0, static_cast<int>((px - ex0) * scale + 0.5));
  const int iy = std::max(0, static_cast<int>((py - ey0) * scale + 0.5));
  const int iw = std::min(work.width - ix, std::max(1, static_cast<int>(pw * scale + 0.5)));
  const int ih = std::min(work.height - iy, std::max(1, static_cast<int>(ph * scale + 0.5)));
  if (iw <= 0 || ih <= 0) {
    *err = rpc::RpcError(rpc::kInternalError, "预览区域换算失败");
    return json::JsonValue::nullValue();
  }

  NlmOptions options;
  options.h = 1.0 + 0.24 * static_cast<double>(strength);
  options.luminanceMask = params.get("luminanceMask").asBool(false);
  imaging::Image denoisedWork;
  if (!nlmDenoise(work, &denoisedWork, options, nullptr)) {
    *err = rpc::RpcError(rpc::kInternalError, "预览降噪失败（输入图像非法）");
    return json::JsonValue::nullValue();
  }

  const imaging::Image before = cropImage(work, ix, iy, iw, ih);
  const imaging::Image after = cropImage(denoisedWork, ix, iy, iw, ih);

  // 左右对比：中间留 2px 白线分隔
  const int divider = 2;
  imaging::Image compare;
  compare.width = iw * 2 + divider;
  compare.height = ih;
  compare.pixels.assign(static_cast<size_t>(compare.width) * compare.height * 3, 255);
  for (int y = 0; y < ih; ++y) {
    for (int x = 0; x < iw; ++x) {
      const size_t si = (static_cast<size_t>(y) * iw + x) * 3;
      const size_t left = (static_cast<size_t>(y) * compare.width + x) * 3;
      const size_t right = (static_cast<size_t>(y) * compare.width + iw + divider + x) * 3;
      for (int c = 0; c < 3; ++c) {
        compare.pixels[left + c] = before.pixels[si + c];
        compare.pixels[right + c] = after.pixels[si + c];
      }
    }
  }

  // 太小的对比图放大到便于观察（box 缩放，不做锐化，保持"所见即所得"）
  if (compare.width < 480) {
    const int factor = (480 + compare.width - 1) / compare.width;
    imaging::Image enlarged;
    if (imaging::resizeImage(compare, &enlarged, compare.width * factor, compare.height * factor)) {
      compare = enlarged;
    }
  }

  const std::string path = previewPath(photoId);
  if (path.empty()) {
    *err = rpc::RpcError(rpc::kIoError, "无法创建预览缓存目录（{home}/cache/denoise）");
    return json::JsonValue::nullValue();
  }
  std::string encodeErr;
  if (!core::jpeg::encodeToFile(path, compare, 90, &encodeErr)) {
    *err = rpc::RpcError(rpc::kIoError, "预览编码失败：" +
                                            (encodeErr.empty() ? std::string(path) : encodeErr));
    return json::JsonValue::nullValue();
  }

  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("path", path);
  out.setItem("photoId", photoId);
  out.setItem("strength", strength);
  out.setItem("width", compare.width);
  out.setItem("height", compare.height);
  json::JsonValue usedRect = json::JsonValue::objectValue();
  usedRect.setItem("x", px);
  usedRect.setItem("y", py);
  usedRect.setItem("w", pw);
  usedRect.setItem("h", ph);
  out.setItem("rect", usedRect);
  return out;
}

}  // namespace specialty
}  // namespace lumina
