// XMP 读写服务实现
#include "xmp/xmp_service.h"

#include <cstdio>
#include <cmath>

#include "core/logger.h"
#include "core/str_util.h"
#include "rpc/version.h"

namespace lumina {
namespace xmp {

namespace {

// XMP exif 的 GPS 值是 "度,分.小数" + 半球引用，不是十进制度
std::string formatGpsCoordinate(double value, bool isLatitude) {
  char hemi = isLatitude ? (value >= 0 ? 'N' : 'S') : (value >= 0 ? 'E' : 'W');
  double abs = value >= 0 ? value : -value;
  int deg = static_cast<int>(abs);
  double minutes = (abs - static_cast<double>(deg)) * 60.0;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%d,%.6f%c", deg, minutes, hemi);
  return std::string(buf);
}

std::string formatAltitude(double alt) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.2f", alt);
  return std::string(buf);
}

// 开标签计数：必须排除 `</tag>`（闭标签）与 `<?` / `<!` 声明，
// 否则配对检查 opens == closes + selfClosing 永远成立，等于没检查。
size_t countTagOpens(const std::string& s) {
  size_t n = 0;
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    if (s[i] != '<') continue;
    const char c = s[i + 1];
    if (c == '?' || c == '!' || c == '/') continue;
    ++n;
  }
  return n;
}

size_t countTagCloses(const std::string& s) {
  size_t n = 0;
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    if (s[i] == '<' && s[i + 1] == '/') ++n;
  }
  return n;
}

size_t countSelfClosing(const std::string& s) {
  size_t n = 0;
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    if (s[i] == '/' && s[i + 1] == '>') ++n;
  }
  return n;
}

bool isBlankText(const std::string& s) {
  for (char c : s) {
    if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r')) return false;
  }
  return true;
}

// 检测 rdf:Description 内容层是否存在"裸文本"（不属于任何子元素的非空白文本）。
// 这正是历史事故的语义特征：整元素区间被元素内容替换 → 标签被吃掉、值裸露在
// Description 下（如 <rdf:Description ...>15.41[{...}]0000000asShot</rdf:Description>），
// Lightroom / Photoshop 直接读不了。用"标签数跌幅"去判会误伤合法删除（例如清除 GPS
// 时一个只有 8 个标签的小 sidecar 会降到 3 个，跌幅超过一半但完全合法）。
bool findBareTextInDescription(const std::string& s, std::string* snippet) {
  size_t descOpen = s.find("<rdf:Description");
  if (descOpen == std::string::npos) return false;
  size_t p = s.find('>', descOpen);
  if (p == std::string::npos) return false;
  ++p;
  size_t descClose = s.find("</rdf:Description>", p);
  if (descClose == std::string::npos) return false;

  int depth = 0;  // 0 = 直接位于 Description 内容层
  std::string pending;
  while (p < descClose) {
    if (s[p] == '<') {
      if (depth == 0 && !isBlankText(pending)) {
        if (snippet) {
          size_t k = 0;
          while (k < pending.size() && (pending[k] == ' ' || pending[k] == '\t' ||
                                        pending[k] == '\n' || pending[k] == '\r')) {
            ++k;
          }
          *snippet = pending.substr(k, 40);
        }
        return true;
      }
      pending.clear();
      size_t e = s.find('>', p);
      if (e == std::string::npos || e >= descClose) break;
      const bool closing = (p + 1 < s.size() && s[p + 1] == '/');
      const bool selfClosing = (e > p && s[e - 1] == '/');
      if (closing) {
        if (depth > 0) --depth;
      } else if (!selfClosing) {
        ++depth;
      }
      p = e + 1;
    } else {
      pending.push_back(s[p]);
      ++p;
    }
  }
  if (depth == 0 && !isBlankText(pending)) {
    if (snippet) {
      size_t k = 0;
      while (k < pending.size() && (pending[k] == ' ' || pending[k] == '\t' ||
                                    pending[k] == '\n' || pending[k] == '\r')) {
        ++k;
      }
      *snippet = pending.substr(k, 40);
    }
    return true;
  }
  return false;
}

// 写入前的结构自检。红线：宁可拒写并报错，也绝不把用户的 sidecar 写坏。
// 判据（三条都要过）：
//   1. 结构标签齐全（xmpmeta / RDF / Description 的开闭都在）
//   2. 标签配对平衡（开 == 闭 + 自闭合），且 Description 内容层无裸文本
//   3. 结果可被重新 parse
// 注意：**不能**用"标签数跌幅"判据 —— 删除节点是合法操作，小 sidecar 清空后
// 跌幅很容易超过一半（曾导致 map.clearPhotoLocation 在有定位的小文件上静默失效）。
bool validateSidecarOutput(const std::string& before, const std::string& after, std::string* err) {
  (void)before;  // 保留参数：可用于后续更细的差分比对
  if (after.find("<x:xmpmeta") == std::string::npos ||
      after.find("</rdf:Description>") == std::string::npos ||
      after.find("</x:xmpmeta>") == std::string::npos) {
    if (err) *err = "sidecar structure check failed: packet is not well-formed";
    return false;
  }
  const size_t opens = countTagOpens(after);
  const size_t closes = countTagCloses(after);
  const size_t selfCloses = countSelfClosing(after);
  if (opens != closes + selfCloses) {
    if (err) {
      *err = "sidecar structure check failed: unbalanced tags (open=" + std::to_string(opens) +
             " close=" + std::to_string(closes) + " self-close=" + std::to_string(selfCloses) + ")";
    }
    return false;
  }
  std::string bare;
  if (findBareTextInDescription(after, &bare)) {
    if (err) {
      *err = "sidecar structure check failed: bare text outside any element in rdf:Description (\"" +
             bare + "\") — packet would be unreadable by Lightroom/Photoshop";
    }
    return false;
  }
  std::string parseErr;
  std::unique_ptr<XmpDocument> reparse = XmpDocument::parse(after, &parseErr);
  if (!reparse) {
    if (err) *err = "sidecar structure check failed: re-parse failed (" + parseErr + ")";
    return false;
  }
  return true;
}

}  // namespace

XmpService::XmpService(core::Config* config, db::PhotoDao* photos, db::Database* db,
                       core::ThreadPool* pool)
    : config_(config), photos_(photos), db_(db), pool_(pool) {}

std::string XmpService::sidecarPathFor(const std::string& photoPath) const {
  return replaceExtension(photoPath, "xmp");
}

std::shared_ptr<std::mutex> XmpService::photoWriteMutex(int64_t photoId) {
  std::lock_guard<std::mutex> lock(mutexMap_);
  auto it = writeMutexes_.find(photoId);
  if (it != writeMutexes_.end()) return it->second;
  auto m = std::make_shared<std::mutex>();
  writeMutexes_[photoId] = m;
  return m;
}

bool XmpService::loadParamsFromPath(const std::string& photoPath, const std::string& sidecarPath,
                                    json::JsonValue* out, std::string* err) {
  *out = XmpMapper::defaultParams();
  if (!fileExists(sidecarPath)) {
    // 无 sidecar = 未调整，返回默认参数（sidecarPath 由上层置空）
    return true;
  }
  std::string bytes;
  if (!readFile(sidecarPath, &bytes, err)) {
    return false;
  }
  std::string parseErr;
  std::unique_ptr<XmpDocument> doc = XmpDocument::parse(bytes, &parseErr);
  if (!doc) {
    if (err) *err = "xmp parse failed: " + sidecarPath + " (" + parseErr + ")";
    return false;
  }
  std::vector<std::string> warnings;
  XmpMapper::readFromDoc(*doc, out, &warnings);
  for (const std::string& w : warnings) {
    logWarn("xmp", photoPath + ": " + w);
  }
  return true;
}

bool XmpService::loadParams(int64_t photoId, json::JsonValue* out, std::string* err) {
  PhotoRecord p;
  if (!photos_->getById(photoId, &p, err)) {
    if (err->empty()) *err = "photo not found: " + std::to_string(photoId);
    return false;
  }
  std::string sidecar = p.sidecarPath.empty() ? sidecarPathFor(p.filePath) : p.sidecarPath;
  return loadParamsFromPath(p.filePath, sidecar, out, err);
}

bool XmpService::setExifGps(int64_t photoId, double lat, double lng, double alt,
                            const std::string& name, std::string* err) {
  PhotoRecord p;
  if (!photos_->getById(photoId, &p, err)) {
    if (err && err->empty()) *err = "photo not found: " + std::to_string(photoId);
    return false;
  }
  std::lock_guard<std::mutex> lock(*photoWriteMutex(photoId));
  std::string sidecar = p.sidecarPath.empty() ? sidecarPathFor(p.filePath) : p.sidecarPath;
  std::unique_ptr<XmpDocument> doc;
  if (fileExists(sidecar)) {
    std::string bytes;
    if (!readFile(sidecar, &bytes, err)) return false;
    std::string parseErr;
    doc = XmpDocument::parse(bytes, &parseErr);
    if (!doc) {
      if (err) *err = "xmp parse failed: " + sidecar + " (" + parseErr + ")";
      return false;
    }
  } else {
    doc = std::unique_ptr<XmpDocument>(new XmpDocument(XmpDocument::createNew()));
  }
  doc->setText("exif", "GPSLatitude", formatGpsCoordinate(lat, true));
  doc->setText("exif", "GPSLongitude", formatGpsCoordinate(lng, false));
  doc->setText("exif", "GPSAltitude", formatAltitude(alt));
  doc->setText("exif", "GPSAltitudeRef", alt < 0 ? "1" : "0");
  if (!name.empty()) doc->setText("lumina", "LocationName", name);

  std::string output = doc->serialize();
  std::string checkErr;
  if (!validateSidecarOutput(doc->raw(), output, &checkErr)) {
    logError("xmp", "refusing to write gps into " + sidecar + ": " + checkErr);
    if (err) *err = checkErr;
    return false;
  }
  if (!writeFileAtomic(sidecar, output, err)) return false;
  photos_->setSidecar(photoId, sidecar, fileMtimeMs(sidecar), true, err);
  return true;
}

bool XmpService::clearExifGps(int64_t photoId, std::string* err) {
  PhotoRecord p;
  if (!photos_->getById(photoId, &p, err)) {
    if (err && err->empty()) *err = "photo not found: " + std::to_string(photoId);
    return false;
  }
  std::string sidecar = p.sidecarPath.empty() ? sidecarPathFor(p.filePath) : p.sidecarPath;
  if (!fileExists(sidecar)) return true;  // 没有 sidecar 就没有可清的定位
  std::lock_guard<std::mutex> lock(*photoWriteMutex(photoId));
  std::string bytes;
  if (!readFile(sidecar, &bytes, err)) return false;
  std::string parseErr;
  std::unique_ptr<XmpDocument> doc = XmpDocument::parse(bytes, &parseErr);
  if (!doc) {
    if (err) *err = "xmp parse failed: " + sidecar + " (" + parseErr + ")";
    return false;
  }
  doc->remove("exif", "GPSLatitude");
  doc->remove("exif", "GPSLongitude");
  doc->remove("exif", "GPSAltitude");
  doc->remove("exif", "GPSAltitudeRef");
  doc->remove("lumina", "LocationName");
  std::string output = doc->serialize();
  std::string checkErr;
  if (!validateSidecarOutput(doc->raw(), output, &checkErr)) {
    logError("xmp", "refusing to clear gps in " + sidecar + ": " + checkErr);
    if (err) *err = checkErr;
    return false;
  }
  if (!writeFileAtomic(sidecar, output, err)) return false;
  photos_->setSidecar(photoId, sidecar, fileMtimeMs(sidecar), true, err);
  return true;
}

bool XmpService::saveParams(int64_t photoId, const json::JsonValue& params, std::string* err,
                            bool force) {
  PhotoRecord p;
  if (!photos_->getById(photoId, &p, err)) {
    if (err->empty()) *err = "photo not found: " + std::to_string(photoId);
    return false;
  }
  std::lock_guard<std::mutex> lock(*photoWriteMutex(photoId));
  std::string sidecar = p.sidecarPath.empty() ? sidecarPathFor(p.filePath) : p.sidecarPath;
  if (!force && fileExists(sidecar)) {
    int64_t current = fileMtimeMs(sidecar);
    if (p.hasSidecarMtime && current > 0 && current != p.sidecarMtime) {
      // 红线：外部修改检测，绝不覆盖
      if (err) *err = "external conflict";
      logWarn("xmp", "external modification detected for " + sidecar);
      return false;
    }
  }
  std::unique_ptr<XmpDocument> doc;
  if (fileExists(sidecar)) {
    std::string bytes;
    if (!readFile(sidecar, &bytes, err)) return false;
    std::string parseErr;
    doc = XmpDocument::parse(bytes, &parseErr);
    if (!doc) {
      if (err) *err = "xmp parse failed: " + sidecar + " (" + parseErr + ")";
      return false;
    }
  } else {
    doc = std::unique_ptr<XmpDocument>(new XmpDocument(XmpDocument::createNew()));
  }
  XmpMapper::writeToDoc(params, doc.get());
  std::string output = doc->serialize();
  std::string checkErr;
  if (!validateSidecarOutput(doc->raw(), output, &checkErr)) {
    // 零静默：结构校验失败必须显式上报，且不落盘（原文件保持不变）
    logError("xmp", "refusing to write " + sidecar + ": " + checkErr);
    if (err) *err = checkErr;
    return false;
  }
  if (!writeFileAtomic(sidecar, output, err)) {
    return false;
  }
  int64_t newMtime = fileMtimeMs(sidecar);
  photos_->setSidecar(photoId, sidecar, newMtime, true, err);
  logInfo("xmp", "wrote sidecar " + sidecar + " (" + std::to_string(output.size()) + " bytes)");
  return true;
}

json::JsonValue XmpService::read(int64_t photoId, rpc::RpcError* err) {
  PhotoRecord p;
  std::string dbErr;
  if (!photos_->getById(photoId, &p, &dbErr)) {
    *err = rpc::RpcError(rpc::kFileNotFound, "photo not found: " + std::to_string(photoId));
    return json::JsonValue::nullValue();
  }
  std::string sidecar = p.sidecarPath.empty() ? sidecarPathFor(p.filePath) : p.sidecarPath;
  json::JsonValue params;
  std::string loadErr;
  if (!loadParamsFromPath(p.filePath, sidecar, &params, &loadErr)) {
    // 红线 R1：sidecar 解析失败必须显式告知，禁止静默
    *err = rpc::RpcError(rpc::kXmpParseError, loadErr);
    return json::JsonValue::nullValue();
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("params", params);
  int schemaVersion = static_cast<int>(params.get("schemaVersion").asInt(kXmpSchemaVersion));
  out.setItem("schemaVersion", schemaVersion);
  out.setItem("processVersion", params.get("processVersion").asString(kMaxProcessVersion));
  json::JsonValue warnings = json::JsonValue::arrayValue();
  if (fileExists(sidecar)) {
    std::string bytes;
    std::string readErr;
    if (readFile(sidecar, &bytes, &readErr)) {
      std::string parseErr;
      std::unique_ptr<XmpDocument> doc = XmpDocument::parse(bytes, &parseErr);
      if (doc) {
        std::string pv;
        if (XmpMapper::processVersionTooNew(*doc, &pv)) {
          warnings.addItem("ProcessVersion " + pv + " 高于引擎认知，部分参数可能不显示");
        }
        json::JsonValue foreign = json::JsonValue::arrayValue();
        for (const std::string& prefix : doc->foreignPrefixes()) {
          foreign.addItem(prefix);
        }
        out.setItem("foreignNamespaces", foreign);
      }
    }
  }
  if (!out.contains("foreignNamespaces")) {
    out.setItem("foreignNamespaces", json::JsonValue::arrayValue());
  }
  out.setItem("warnings", warnings);
  if (fileExists(sidecar)) {
    out.setItem("sidecarPath", sidecar);
    out.setItem("sidecarMtime", fileMtimeMs(sidecar));
  } else {
    out.setItem("sidecarPath", json::JsonValue::nullValue());
    out.setItem("sidecarMtime", json::JsonValue::nullValue());
  }
  return out;
}

json::JsonValue XmpService::write(int64_t photoId, const json::JsonValue& params,
                                  rpc::RpcError* err) {
  const json::JsonValue& p = params.get("params");
  if (!p.isObject()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.params must be an EditParams object");
    return json::JsonValue::nullValue();
  }
  std::string saveErr;
  if (!saveParams(photoId, p, &saveErr)) {
    if (saveErr == "external conflict") {
      PhotoRecord record;
      std::string dbErr;
      photos_->getById(photoId, &record, &dbErr);
      std::string sidecar =
          record.sidecarPath.empty() ? sidecarPathFor(record.filePath) : record.sidecarPath;
      json::JsonValue data = json::JsonValue::objectValue();
      data.setItem("sidecarMtime", fileMtimeMs(sidecar));
      data.setItem("knownMtime", record.sidecarMtime);
      data.setItem("sidecarPath", sidecar);
      *err = rpc::RpcError(rpc::kXmpExternalConflict,
                           "sidecar 已被外部修改，请选择 重读 / 覆盖 / 另存", data);
      return json::JsonValue::nullValue();
    }
    if (saveErr.find("xmp parse failed") != std::string::npos) {
      *err = rpc::RpcError(rpc::kXmpParseError, saveErr);
      return json::JsonValue::nullValue();
    }
    *err = rpc::RpcError(rpc::kXmpWriteFailed, saveErr);
    return json::JsonValue::nullValue();
  }
  PhotoRecord record;
  std::string dbErr;
  photos_->getById(photoId, &record, &dbErr);
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("sidecarMtime", record.sidecarMtime);
  return out;
}

json::JsonValue XmpService::checkExternal(int64_t photoId, rpc::RpcError* err) {
  PhotoRecord p;
  std::string dbErr;
  if (!photos_->getById(photoId, &p, &dbErr)) {
    *err = rpc::RpcError(rpc::kFileNotFound, "photo not found: " + std::to_string(photoId));
    return json::JsonValue::nullValue();
  }
  std::string sidecar = p.sidecarPath.empty() ? sidecarPathFor(p.filePath) : p.sidecarPath;
  json::JsonValue out = json::JsonValue::objectValue();
  if (!fileExists(sidecar)) {
    out.setItem("changed", false);
    out.setItem("sidecarMtime", json::JsonValue::nullValue());
    return out;
  }
  int64_t current = fileMtimeMs(sidecar);
  bool changed = p.hasSidecarMtime && current != p.sidecarMtime;
  out.setItem("changed", changed);
  out.setItem("sidecarMtime", current);
  return out;
}

json::JsonValue XmpService::listNamespaces(int64_t photoId, rpc::RpcError* err) {
  PhotoRecord p;
  std::string dbErr;
  if (!photos_->getById(photoId, &p, &dbErr)) {
    *err = rpc::RpcError(rpc::kFileNotFound, "photo not found: " + std::to_string(photoId));
    return json::JsonValue::nullValue();
  }
  std::string sidecar = p.sidecarPath.empty() ? sidecarPathFor(p.filePath) : p.sidecarPath;
  json::JsonValue out = json::JsonValue::arrayValue();
  if (!fileExists(sidecar)) return out;
  std::string bytes;
  std::string readErr;
  if (!readFile(sidecar, &bytes, &readErr)) {
    *err = rpc::RpcError(rpc::kIoError, readErr);
    return json::JsonValue::nullValue();
  }
  std::string parseErr;
  std::unique_ptr<XmpDocument> doc = XmpDocument::parse(bytes, &parseErr);
  if (!doc) {
    *err = rpc::RpcError(rpc::kXmpParseError, parseErr);
    return json::JsonValue::nullValue();
  }
  for (const auto& ns : doc->namespaceFieldCounts()) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("prefix", ns.first);
    std::string uri;
    for (const auto& declared : doc->declaredNamespaces()) {
      if (declared.first == ns.first) uri = declared.second;
    }
    item.setItem("uri", uri);
    item.setItem("fieldCount", ns.second);
    out.addItem(item);
  }
  return out;
}

}  // namespace xmp
}  // namespace lumina
