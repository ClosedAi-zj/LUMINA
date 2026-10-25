// LibRaw 解码入口实现
#include "raw/raw_decoder.h"

#include <algorithm>
#include <cstring>

#include "core/logger.h"
#include "core/str_util.h"
#include "libraw/libraw.h"

namespace lumina {
namespace raw {
namespace {

std::string modelString(const char* s) { return s ? std::string(s) : std::string(); }

}  // namespace

bool isSupportedExtension(const std::string& ext) {
  static const char* kRaw[] = {"rw2", "dng", "arw", "cr2", "cr3", "nef", "nrw", "orf", "raf",
                               "pef", "ptx", "r3d", "srw", "x3f", "erf", "kdc", "dcr", "iiq",
                               "3fr", "fff", "mef", "mos", "raw", "rwz", "srf"};
  static const char* kStd[] = {"jpg", "jpeg", "png", "tif", "tiff", "psd", "bmp", "webp"};
  for (const char* e : kRaw) {
    if (extensionOf(std::string("x.") + ext) == e) return true;
  }
  for (const char* e : kStd) {
    if (extensionOf(std::string("x.") + ext) == e) return true;
  }
  return false;
}

std::string formatOfExtension(const std::string& ext) {
  std::string e = extensionOf(std::string("x.") + ext);
  std::string upper;
  for (char c : e) upper.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
  if (upper == "JPG" || upper == "JPEG") return "JPG";
  if (upper == "TIF" || upper == "TIFF") return "TIFF";
  if (upper == "PNG") return "PNG";
  if (upper == "PSD") return "PSD";
  if (upper == "MP4" || upper == "MOV" || upper == "M4V" || upper == "AVI" || upper == "MPG" ||
      upper == "MKV" || upper == "WEBM") {
    return "MP4";
  }
  if (upper.empty()) return std::string("JPG");
  return upper;
}

bool RawDecoder::decodeToImage16(const std::string& path, bool half, imaging::ImageBuf* out,
                                 std::string* err) {
  if (!fileExists(path)) {
    if (err) *err = "file not found: " + path;
    return false;
  }
  LibRaw processor;
  processor.imgdata.params.half_size = half ? 1 : 0;
  processor.imgdata.params.use_camera_wb = 1;
  processor.imgdata.params.output_color = 1;       // sRGB 原色矩阵（色域映射留 M4）
  processor.imgdata.params.output_bps = 16;        // M0：拆除 8bit 天花板
  processor.imgdata.params.gamm[0] = 1.0;          // gamma 线性：scene-linear 输出
  processor.imgdata.params.gamm[1] = 1.0;
  processor.imgdata.params.no_auto_bright = 1;     // 关自动提亮，保线性
  processor.imgdata.params.use_auto_wb = 0;
  processor.imgdata.params.exp_correc = 0;
  processor.imgdata.params.exp_shift = 1.0;
  processor.imgdata.params.exp_preser = 0.6;
  processor.imgdata.params.fbdd_noiserd = 0;
  processor.imgdata.params.threshold = 0.0;
  processor.imgdata.params.med_passes = 0;
  processor.imgdata.params.user_qual = 1;

  int rc = processor.open_file(path.c_str());
  if (rc != LIBRAW_SUCCESS) {
    if (err) *err = "libraw open failed (" + std::to_string(rc) + "): " + path;
    return false;
  }
  rc = processor.unpack();
  if (rc != LIBRAW_SUCCESS) {
    if (err) *err = "libraw unpack failed (" + std::to_string(rc) + "): " + path;
    return false;
  }
  // 黑白电平在 unpack 后即可读（供 S0 归一化与可重现性记录）
  const float black = static_cast<float>(processor.imgdata.color.black);
  const float white = static_cast<float>(processor.imgdata.color.maximum);

  rc = processor.dcraw_process();
  if (rc != LIBRAW_SUCCESS) {
    if (err) *err = "libraw process failed (" + std::to_string(rc) + "): " + path;
    return false;
  }
  libraw_processed_image_t* image = processor.dcraw_make_mem_image(&rc);
  if (!image || rc != LIBRAW_SUCCESS) {
    if (image) LibRaw::dcraw_clear_mem(image);
    if (err) *err = "libraw make_mem_image failed (" + std::to_string(rc) + "): " + path;
    return false;
  }
  if (image->type != LIBRAW_IMAGE_BITMAP || image->colors != 3 || image->bits != 16) {
    if (err) *err = "unexpected libraw 16bit image layout: " + path;
    LibRaw::dcraw_clear_mem(image);
    return false;
  }
  out->w = static_cast<int>(image->width);
  out->h = static_cast<int>(image->height);
  out->space = imaging::ColorSpaceTag::SceneLinearCamera;
  out->blackPoint = black;
  out->whitePoint = white > 0.f ? white : 65535.f;
  const size_t n = static_cast<size_t>(out->w) * out->h * 3;
  out->data.resize(n);
  const uint16_t* src = reinterpret_cast<const uint16_t*>(image->data);
  // LibRaw 16bit 输出：[black..maximum] 已线性映射到 [0..65535]，直接按 65535 归一化
  for (size_t i = 0; i < n; ++i) {
    out->data[i] = imaging::log16Encode(static_cast<float>(src[i]) / 65535.0f);
  }
  LibRaw::dcraw_clear_mem(image);
  return true;
}

bool RawDecoder::decodeToBitmap(const std::string& path, bool half, imaging::Image* out,
                                std::string* err) {
  if (!fileExists(path)) {
    if (err) *err = "file not found: " + path;
    return false;
  }
  LibRaw processor;
  processor.imgdata.params.half_size = half ? 1 : 0;
  processor.imgdata.params.use_camera_wb = 1;
  processor.imgdata.params.output_color = 1;      // sRGB
  processor.imgdata.params.output_bps = 8;
  processor.imgdata.params.no_auto_bright = 1;     // 不自动提亮，保线性感
  processor.imgdata.params.use_auto_wb = 0;
  processor.imgdata.params.exp_correc = 0;
  processor.imgdata.params.exp_shift = 1.0;
  processor.imgdata.params.exp_preser = 0.6;
  processor.imgdata.params.fbdd_noiserd = 0;
  processor.imgdata.params.threshold = 0.0;
  processor.imgdata.params.med_passes = 0;
  processor.imgdata.params.user_qual = 1;  // AHD 为默认已足够；VCD/DCB 太慢

  int rc = processor.open_file(path.c_str());
  if (rc != LIBRAW_SUCCESS) {
    if (err) *err = "libraw open failed (" + std::to_string(rc) + "): " + path;
    return false;
  }
  rc = processor.unpack();
  if (rc != LIBRAW_SUCCESS) {
    if (err) *err = "libraw unpack failed (" + std::to_string(rc) + "): " + path;
    return false;
  }
  rc = processor.dcraw_process();
  if (rc != LIBRAW_SUCCESS) {
    if (err) *err = "libraw process failed (" + std::to_string(rc) + "): " + path;
    return false;
  }
  libraw_processed_image_t* image = processor.dcraw_make_mem_image(&rc);
  if (!image || rc != LIBRAW_SUCCESS) {
    if (image) LibRaw::dcraw_clear_mem(image);
    if (err) *err = "libraw make_mem_image failed (" + std::to_string(rc) + "): " + path;
    return false;
  }
  if (image->type != LIBRAW_IMAGE_BITMAP || image->colors != 3) {
    if (err) *err = "unexpected libraw image layout: " + path;
    LibRaw::dcraw_clear_mem(image);
    return false;
  }
  out->width = static_cast<int>(image->width);
  out->height = static_cast<int>(image->height);
  size_t n = static_cast<size_t>(out->width) * out->height * 3;
  if (image->bits == 8) {
    out->pixels.assign(image->data, image->data + n);
  } else {
    // 16bit 输出 → 降到 8bit
    out->pixels.resize(n);
    const uint16_t* src = reinterpret_cast<const uint16_t*>(image->data);
    for (size_t i = 0; i < n; ++i) {
      out->pixels[i] = static_cast<uint8_t>(src[i] >> 8);
    }
  }
  LibRaw::dcraw_clear_mem(image);
  return true;
}

bool RawDecoder::decodeToJpeg(const std::string& path, const std::string& outPath, bool half,
                              int quality, std::string* err) {
  imaging::Image image;
  if (!decodeToBitmap(path, half, &image, err)) return false;
  if (!imaging::encodeJpeg(outPath, image, quality, err)) return false;
  return true;
}

bool RawDecoder::readMetadata(const std::string& path, RawMeta* out, std::string* err) {
  if (!fileExists(path)) {
    if (err) *err = "file not found: " + path;
    return false;
  }
  LibRaw processor;
  int rc = processor.open_file(path.c_str());
  if (rc != LIBRAW_SUCCESS) {
    if (err) *err = "libraw open failed (" + std::to_string(rc) + "): " + path;
    return false;
  }
  const libraw_imgother_t& other = processor.imgdata.other;
  out->make = modelString(processor.imgdata.idata.make);
  out->model = modelString(processor.imgdata.idata.model);
  out->iso = other.iso_speed;
  out->aperture = other.aperture;
  out->shutter = other.shutter;
  out->focalLength = static_cast<int>(other.focal_len + 0.5);
  out->width = static_cast<int>(processor.imgdata.sizes.width);
  out->height = static_cast<int>(processor.imgdata.sizes.height);
  // LibRaw 的 other.timestamp 是 time_t（秒）；本引擎全链路约定毫秒epoch
  // （formatDate/时间轴/DB captured_at 均按 ms）。少乘 1000 会让所有 RAW 照片
  // 的拍摄时间变成 1970-01-21（真实 RW2 实测抓到：captured_at=1766968071 秒值）。
  out->timestampMs = static_cast<int64_t>(other.timestamp) * 1000;
  out->hasGps = (other.parsed_gps.gpsparsed != 0);
  if (out->hasGps) {
    out->gpsLat = other.parsed_gps.latitude[0] + other.parsed_gps.latitude[1] / 60.0 +
                  other.parsed_gps.latitude[2] / 3600.0;
    if (other.parsed_gps.latref == 'S' || other.parsed_gps.latref == 's') out->gpsLat = -out->gpsLat;
    out->gpsLng = other.parsed_gps.longitude[0] + other.parsed_gps.longitude[1] / 60.0 +
                  other.parsed_gps.longitude[2] / 3600.0;
    if (other.parsed_gps.longref == 'W' || other.parsed_gps.longref == 'w') out->gpsLng = -out->gpsLng;
    out->gpsAlt = other.parsed_gps.altitude;
  }
  return true;
}

bool RawDecoder::selfTest(db::PhotoDao* photos, int sample, const std::vector<int64_t>& ids,
                          SelfTestResult* out, std::string* err) {
  out->ok = false;
  out->sampled = 0;
  out->succeeded = 0;
  out->decodeMsP50 = 0.0;
  out->decodeMsP95 = 0.0;
  out->failures.clear();

  // ---- 1. 确定抽样集合：ids 优先，否则从 RAW 格式照片随机抽取 ----
  struct SampleItem {
    int64_t photoId = 0;
    std::string filePath;
  };
  std::vector<SampleItem> items;
  if (!ids.empty()) {
    for (int64_t id : ids) {
      PhotoRecord p;
      std::string dbErr;
      if (!photos->getById(id, &p, &dbErr)) {
        SelfTestFailure f;
        f.photoId = id;
        f.error = "photo not found: " + std::to_string(id);
        out->failures.push_back(f);
        continue;
      }
      SampleItem item;
      item.photoId = p.id;
      item.filePath = p.filePath;
      items.push_back(item);
    }
  } else {
    if (sample <= 0) sample = 32;
    static const char* kSql =
        "SELECT id, file_path FROM photos WHERE deleted=0 AND is_missing=0 AND format IN "
        "('RW2','DNG','ARW','CR2','CR3','NEF','NRW','ORF','RAF','PEF','R3D','SRW','X3F') "
        "ORDER BY RANDOM() LIMIT ?;";
    auto stmt = photos->database()->prepareCached("rawSelfTestSample", kSql, err);
    if (!stmt || !stmt->valid()) return false;
    stmt->bindInt64(1, sample);
    while (stmt->step(err)) {
      SampleItem item;
      item.photoId = stmt->columnInt64(0);
      item.filePath = stmt->columnText(1);
      items.push_back(item);
    }
    stmt->reset();
  }

  out->sampled = static_cast<int>(items.size());
  if (out->sampled == 0) {
    if (err) *err = "no sampleable RAW photos in library";
    return false;
  }

  // ---- 2. 逐张真解码 + 均值判定（L1：非全黑/非全白） ----
  std::vector<int64_t> decodeMs;
  for (const SampleItem& item : items) {
    int64_t t0 = nowMs();
    imaging::Image image;
    std::string decodeErr;
    if (!decodeToBitmap(item.filePath, /*half=*/true, &image, &decodeErr)) {
      SelfTestFailure f;
      f.photoId = item.photoId;
      f.error = decodeErr;
      out->failures.push_back(f);
      continue;
    }
    decodeMs.push_back(nowMs() - t0);
    if (image.pixels.empty()) {
      SelfTestFailure f;
      f.photoId = item.photoId;
      f.error = "decoded image is empty";
      out->failures.push_back(f);
      continue;
    }
    int64_t sum = 0;
    for (uint8_t v : image.pixels) sum += v;
    double mean = static_cast<double>(sum) / static_cast<double>(image.pixels.size());
    if (mean < 16.0 || mean > 240.0) {
      SelfTestFailure f;
      f.photoId = item.photoId;
      f.error = "output mean out of range [16,240]: " + std::to_string(mean);
      out->failures.push_back(f);
      continue;
    }
    ++out->succeeded;
  }

  // ---- 3. 分位统计 ----
  if (!decodeMs.empty()) {
    std::sort(decodeMs.begin(), decodeMs.end());
    size_t n = decodeMs.size();
    out->decodeMsP50 = static_cast<double>(decodeMs[n / 2]);
    size_t idx95 = static_cast<size_t>(static_cast<double>(n) * 0.95);
    if (idx95 >= n) idx95 = n - 1;
    out->decodeMsP95 = static_cast<double>(decodeMs[idx95]);
  }
  out->ok = out->failures.empty() && out->succeeded == out->sampled;
  return true;
}

}  // namespace raw
}  // namespace lumina
