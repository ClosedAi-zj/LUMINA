// MP4/MOV 容器直读：时长、分辨率、创建时间、编码（不依赖外部工具）
#ifndef LUMINA_METADATA_VIDEO_PROBE_H_
#define LUMINA_METADATA_VIDEO_PROBE_H_

#include <cstdint>
#include <string>

namespace lumina {
namespace metadata {

struct VideoInfo {
  int width = 0;
  int height = 0;
  double durationSec = 0.0;
  int64_t creationMs = 0;
  std::string codec;
  int64_t fileSize = 0;
};

// 探测视频容器信息；非 MP4/MOV 或解析失败返回 false
bool probeVideo(const std::string& path, VideoInfo* out, std::string* err);

}  // namespace metadata
}  // namespace lumina

#endif  // LUMINA_METADATA_VIDEO_PROBE_H_
