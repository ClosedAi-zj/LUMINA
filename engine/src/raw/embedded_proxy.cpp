// RAW 容器内嵌 JPEG 代理提取实现
#include "raw/embedded_proxy.h"

#include "core/logger.h"
#include "core/str_util.h"
#include "libraw/libraw.h"

namespace lumina {
namespace raw {

bool extractEmbeddedJpeg(const std::string& rawPath, const std::string& outPath,
                         EmbeddedProxy* out, std::string* err) {
  if (!fileExists(rawPath)) {
    if (err) *err = "file not found: " + rawPath;
    return false;
  }
  LibRaw processor;
  int rc = processor.open_file(rawPath.c_str());
  if (rc != LIBRAW_SUCCESS) {
    if (err) *err = "libraw open failed (" + std::to_string(rc) + "): " + rawPath;
    return false;
  }
  rc = processor.unpack_thumb();
  if (rc != LIBRAW_SUCCESS) {
    if (err) *err = "no embedded proxy (" + std::to_string(rc) + "): " + rawPath;
    return false;
  }
  const libraw_thumbnail_t& thumb = processor.imgdata.thumbnail;
  if (thumb.tlength <= 0 || thumb.thumb == nullptr) {
    if (err) *err = "embedded proxy empty: " + rawPath;
    return false;
  }
  std::string data(reinterpret_cast<const char*>(thumb.thumb),
                   static_cast<size_t>(thumb.tlength));
  if (!writeFileAtomic(outPath, data, err)) {
    return false;
  }
  if (out) {
    out->outPath = outPath;
    out->width = static_cast<int>(thumb.twidth);
    out->height = static_cast<int>(thumb.theight);
    out->bytes = static_cast<int64_t>(thumb.tlength);
  }
  logInfo("raw", "extracted embedded proxy " + std::to_string(thumb.tlength) + " bytes -> " + outPath);
  return true;
}

bool hasEmbeddedProxy(const std::string& rawPath, std::string* err) {
  if (!fileExists(rawPath)) {
    if (err) *err = "file not found: " + rawPath;
    return false;
  }
  LibRaw processor;
  int rc = processor.open_file(rawPath.c_str());
  if (rc != LIBRAW_SUCCESS) {
    if (err) *err = "libraw open failed (" + std::to_string(rc) + "): " + rawPath;
    return false;
  }
  rc = processor.unpack_thumb();
  if (rc != LIBRAW_SUCCESS) {
    if (err) *err = "no embedded proxy";
    return false;
  }
  return processor.imgdata.thumbnail.tlength > 0;
}

}  // namespace raw
}  // namespace lumina
