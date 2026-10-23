// EXIF 直读实现（最小 TIFF IFD 遍历，覆盖 M1 需要的字段）
#include "metadata/exif_reader.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace metadata {
namespace {

struct TiffReader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool littleEndian = true;

  uint16_t u16(size_t off) const {
    if (off + 2 > size) return 0;
    return littleEndian ? static_cast<uint16_t>(data[off] | (data[off + 1] << 8))
                        : static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
  }
  uint32_t u32(size_t off) const {
    if (off + 4 > size) return 0;
    if (littleEndian) {
      return static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8) |
             (static_cast<uint32_t>(data[off + 2]) << 16) | (static_cast<uint32_t>(data[off + 3]) << 24);
    }
    return static_cast<uint32_t>(data[off]) << 24 | (static_cast<uint32_t>(data[off + 1]) << 16) |
           (static_cast<uint32_t>(data[off + 2]) << 8) | static_cast<uint32_t>(data[off + 3]);
  }
  double rational(size_t off) const {
    uint32_t num = u32(off);
    uint32_t den = u32(off + 4);
    if (den == 0) return 0.0;
    return static_cast<double>(num) / den;
  }
  std::string ascii(size_t off, size_t len) const {
    std::string out;
    for (size_t i = 0; i < len && off + i < size; ++i) {
      char c = static_cast<char>(data[off + i]);
      if (c == '\0') break;
      out.push_back(c);
    }
    return trim(out);
  }
  // 读取一组连续有理数（GPS 的 DMS 三元组）
  bool readRationals(size_t ifdOff, uint16_t tag, double* values, int maxCount) const {
    uint16_t entries = u16(ifdOff);
    const uint8_t* p = data + ifdOff + 2;
    for (uint16_t i = 0; i < entries; ++i, p += 12) {
      size_t entryOff = static_cast<size_t>(p - data);
      if (entryOff + 12 > size) return false;
      uint16_t t = u16(entryOff);
      if (t != tag) continue;
      uint16_t tp = u16(entryOff + 2);
      if (tp != 5 && tp != 10) return false;
      uint32_t cnt = u32(entryOff + 4);
      size_t total = static_cast<size_t>(cnt) * 8;
      size_t dataOff = entryOff + 8;
      if (total > 4) dataOff = u32(entryOff + 8);
      int n = static_cast<int>(cnt);
      if (n > maxCount) n = maxCount;
      for (int k = 0; k < n; ++k) {
        values[k] = (tp == 5) ? rational(dataOff + 8 * k) : srational(dataOff + 8 * k);
      }
      return true;
    }
    return false;
  }

  double srational(size_t off) const {
    uint32_t n = u32(off);
    uint32_t d = u32(off + 4);
    if (d == 0) return 0.0;
    return static_cast<double>(static_cast<int32_t>(n)) / static_cast<double>(d);
  }

  // 取第 idx 个 IFD 入口（tag/type/count/valueOffset 按类型给出可读值）
  bool readEntryValue(size_t ifdOff, uint16_t tag, int type, uint32_t* count, std::string* text,
                      double* num, uint64_t* rawOffset) const;
};

bool TiffReader::readEntryValue(size_t ifdOff, uint16_t tag, int type, uint32_t* count,
                                std::string* text, double* num, uint64_t* rawOffset) const {
  uint16_t entries = u16(ifdOff);
  const uint8_t* p = data + ifdOff + 2;
  for (uint16_t i = 0; i < entries; ++i, p += 12) {
    size_t entryOff = static_cast<size_t>(p - data);
    if (entryOff + 12 > size) return false;
    uint16_t t = u16(entryOff);
    if (t != tag) continue;
    uint16_t tp = u16(entryOff + 2);
    uint32_t cnt = u32(entryOff + 4);
    size_t valueOff = entryOff + 8;
    size_t typeSize = 1;
    switch (tp) {
      case 1: typeSize = 1; break;
      case 2: typeSize = 1; break;
      case 3: typeSize = 2; break;
      case 4: typeSize = 4; break;
      case 5: typeSize = 8; break;
      case 7: typeSize = 1; break;
      case 9: typeSize = 4; break;
      case 10: typeSize = 8; break;
      default: typeSize = 1; break;
    }
    size_t total = static_cast<size_t>(cnt) * typeSize;
    size_t dataOff = valueOff;
    if (total > 4) dataOff = u32(valueOff);
    *count = cnt;
    *rawOffset = dataOff;
    if (type == 2 && text) {
      *text = ascii(dataOff, total);
    } else if (num) {
      if (tp == 3) {
        *num = u16(dataOff);
      } else if (tp == 4) {
        *num = u32(dataOff);
      } else if (tp == 5) {
        *num = rational(dataOff);
      } else if (tp == 10) {
        uint32_t n = u32(dataOff);
        uint32_t d = u32(dataOff + 4);
        *num = d ? static_cast<double>(static_cast<int32_t>(n)) / static_cast<double>(d) : 0.0;
      } else {
        *num = 0.0;
      }
    }
    return true;
  }
  return false;
}

int64_t parseExifDate(const std::string& text) {
  // "YYYY:MM:DD HH:MM:SS" → epoch ms（按 UTC 计算，避免依赖 _mkgmtime）
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (sscanf(text.c_str(), "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3) return 0;
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  static const int kDaysBeforeMonth[13] = {0,   0,   31,  59,  90,  120, 151,
                                           181, 212, 243, 273, 304, 334};
  int64_t days = 365 * (y - 1970) + (y - 1969) / 4;
  days += kDaysBeforeMonth[mo] + (d - 1);
  if (mo > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) ++days;
  int64_t secs = days * 86400 + h * 3600 + mi * 60 + s;
  return secs * 1000;
}

bool parseTiff(const uint8_t* data, size_t size, ExifInfo* out) {
  if (size < 8) return false;
  TiffReader r;
  r.data = data;
  r.size = size;
  if (data[0] == 'I' && data[1] == 'I') r.littleEndian = true;
  else if (data[0] == 'M' && data[1] == 'M') r.littleEndian = false;
  else return false;
  if (r.u16(2) != 42 && r.u16(2) != 0x4f52 && r.u16(2) != 0x0055) return false;
  size_t ifd0 = r.u32(4);
  if (ifd0 <= 0 || ifd0 >= size) return false;
  bool any = false;
  uint32_t count = 0;
  std::string text;
  double num = 0.0;
  uint64_t off = 0;
  auto read = [&](size_t ifd, uint16_t tag, int type, std::string* t, double* n) -> bool {
    uint32_t c = 0;
    uint64_t o = 0;
    bool ok = r.readEntryValue(ifd, tag, type, &c, t, n, &o);
    if (ok) {
      count = c;
      off = o;
    }
    return ok;
  };
  if (read(ifd0, 0x010f, 2, &text, nullptr)) { out->make = text; any = true; }
  if (read(ifd0, 0x0110, 2, &text, nullptr)) { out->model = text; any = true; }
  if (read(ifd0, 0x0132, 2, &text, nullptr)) {
    int64_t t = parseExifDate(text);
    if (t > 0) out->timestampMs = t;
    any = true;
  }
  if (read(ifd0, 0x0112, 3, nullptr, &num)) {
    // orientation 暂时不使用，但标记已解析
    any = true;
  }
  if (read(ifd0, 0x0131, 2, &text, nullptr)) { out->software = text; any = true; }

  // Exif 子 IFD
  uint32_t exifIfd = 0;
  uint64_t exifOff = 0;
  if (read(ifd0, 0x8769, 4, nullptr, nullptr)) {
    exifIfd = static_cast<uint32_t>(off);
  }
  if (exifIfd > 0 && exifIfd < size) {
    if (read(exifIfd, 0x9003, 2, &text, nullptr)) {
      int64_t t = parseExifDate(text);
      if (t > 0) out->timestampMs = t;
      any = true;
    }
    if (read(exifIfd, 0x8827, 3, nullptr, &num)) { out->iso = static_cast<int>(num + 0.5); any = true; }
    if (read(exifIfd, 0x829a, 5, nullptr, &num)) { out->shutter = num; any = true; }
    if (read(exifIfd, 0x829d, 5, nullptr, &num)) { out->aperture = num; any = true; }
    if (read(exifIfd, 0x920a, 5, nullptr, &num)) { out->focalLength = static_cast<int>(num + 0.5); any = true; }
    if (read(exifIfd, 0xa434, 2, &text, nullptr)) { out->lens = text; any = true; }
    if (read(exifIfd, 0xa002, 4, nullptr, &num)) { /* 像素宽度 */ out->width = static_cast<int>(num); }
    if (read(exifIfd, 0xa003, 4, nullptr, &num)) { out->height = static_cast<int>(num); }
  }

  // GPS 子 IFD
  uint32_t gpsIfd = 0;
  if (read(ifd0, 0x8825, 4, nullptr, nullptr)) gpsIfd = static_cast<uint32_t>(off);
  if (gpsIfd > 0 && gpsIfd < size) {
    std::string latref, lonref;
    read(gpsIfd, 0x0001, 2, &latref, nullptr);
    read(gpsIfd, 0x0003, 2, &lonref, nullptr);
    double lat[3] = {0, 0, 0};
    double lon[3] = {0, 0, 0};
    bool okLat = r.readRationals(gpsIfd, 0x0002, lat, 3);
    bool okLon = r.readRationals(gpsIfd, 0x0004, lon, 3);
    if (okLat && okLon) {
      out->gpsLat = lat[0] + lat[1] / 60.0 + lat[2] / 3600.0;
      out->gpsLng = lon[0] + lon[1] / 60.0 + lon[2] / 3600.0;
      if (!latref.empty() && (latref[0] == 'S' || latref[0] == 's')) out->gpsLat = -out->gpsLat;
      if (!lonref.empty() && (lonref[0] == 'W' || lonref[0] == 'w')) out->gpsLng = -out->gpsLng;
      if (read(gpsIfd, 0x0006, 5, nullptr, &num)) out->gpsAlt = num;
      out->hasGps = true;
      any = true;
    }
  }
  (void)count;
  return any;
}

// 从 JPEG 中提取 APP1/Exif 段
bool extractJpegExif(const std::string& bytes, std::vector<uint8_t>* tiff) {
  if (bytes.size() < 4) return false;
  size_t pos = 2;
  while (pos + 4 <= bytes.size()) {
    if (static_cast<uint8_t>(bytes[pos]) != 0xFF) return false;
    uint8_t marker = static_cast<uint8_t>(bytes[pos + 1]);
    if (marker == 0xD8 || marker == 0x01) { pos += 2; continue; }
    if (marker == 0xDA) return false;  // 压缩数据开始
    if (pos + 4 > bytes.size()) return false;
    uint16_t len = (static_cast<uint8_t>(bytes[pos + 2]) << 8) | static_cast<uint8_t>(bytes[pos + 3]);
    if (len < 2) return false;
    if (marker == 0xE1 && len >= 8) {
      std::string sig = bytes.substr(pos + 4, 6);
      if (sig == "Exif\0" || sig == "Exif") {
        const uint8_t* start = reinterpret_cast<const uint8_t*>(bytes.data()) + pos + 4 + 6;
        size_t n = static_cast<size_t>(len) - 8;
        tiff->assign(start, start + n);
        return true;
      }
    }
    pos += 2 + len;
  }
  return false;
}

// 从 PNG 中提取 eXIf chunk
bool extractPngExif(const std::string& bytes, std::vector<uint8_t>* tiff) {
  if (bytes.size() < 16) return false;
  if (bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0) return false;
  size_t pos = 8;
  while (pos + 8 <= bytes.size()) {
    uint32_t len = (static_cast<uint8_t>(bytes[pos]) << 24) | (static_cast<uint8_t>(bytes[pos + 1]) << 16) |
                   (static_cast<uint8_t>(bytes[pos + 2]) << 8) | static_cast<uint8_t>(bytes[pos + 3]);
    std::string type = bytes.substr(pos + 4, 4);
    if (pos + 12 + len > bytes.size()) return false;
    if (type == "eXIf") {
      const uint8_t* start = reinterpret_cast<const uint8_t*>(bytes.data()) + pos + 8;
      tiff->assign(start, start + len);
      return true;
    }
    if (type == "IEND") return false;
    pos += 12 + static_cast<size_t>(len);
  }
  return false;
}

}  // namespace

bool readExifFromFile(const std::string& path, ExifInfo* out, std::string* err) {
  std::string bytes;
  if (!readFile(path, &bytes, err)) return false;
  if (bytes.size() < 8) {
    if (err) *err = "file too small for exif: " + path;
    return false;
  }
  std::vector<uint8_t> tiff;
  bool ok = false;
  if (static_cast<uint8_t>(bytes[0]) == 0xFF && static_cast<uint8_t>(bytes[1]) == 0xD8) {
    ok = extractJpegExif(bytes, &tiff);
  } else if (bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) {
    ok = extractPngExif(bytes, &tiff);
  } else if ((bytes[0] == 'I' && bytes[1] == 'I') || (bytes[0] == 'M' && bytes[1] == 'M')) {
    tiff.assign(bytes.begin(), bytes.end());
    ok = true;
  }
  if (!ok || tiff.size() < 8) {
    if (err) *err = "no exif payload: " + path;
    return false;
  }
  if (!parseTiff(tiff.data(), tiff.size(), out)) {
    if (err) *err = "exif parse failed: " + path;
    return false;
  }
  return true;
}

}  // namespace metadata
}  // namespace lumina
