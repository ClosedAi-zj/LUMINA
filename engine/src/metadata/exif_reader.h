// 标准图像（JPEG/PNG/TIFF）EXIF 直读：相机、曝光、GPS（RAW 走 LibRaw）
#ifndef LUMINA_METADATA_EXIF_READER_H_
#define LUMINA_METADATA_EXIF_READER_H_

#include <cstdint>
#include <string>

namespace lumina {
namespace metadata {

struct ExifInfo {
  std::string make;
  std::string model;
  std::string lens;
  std::string software;
  int iso = 0;
  double aperture = 0.0;
  double shutter = 0.0;
  int focalLength = 0;
  int64_t timestampMs = 0;
  int width = 0;
  int height = 0;
  int orientation = 1;  // EXIF 0x0112（1-8；≥5 表示存储位图需旋转，宽高互换）
  bool hasGps = false;
  double gpsLat = 0.0;
  double gpsLng = 0.0;
  double gpsAlt = 0.0;
};

// 读取 EXIF；无 EXIF 或格式不支持返回 false（调用方可退回 LibRaw）
bool readExifFromFile(const std::string& path, ExifInfo* out, std::string* err);

}  // namespace metadata
}  // namespace lumina

#endif  // LUMINA_METADATA_EXIF_READER_H_
