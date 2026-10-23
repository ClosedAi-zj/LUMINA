// RAW 容器内嵌 JPEG 代理提取（LibRaw unpack_thumb），失败返回 2003 语义
#ifndef LUMINA_RAW_EMBEDDED_PROXY_H_
#define LUMINA_RAW_EMBEDDED_PROXY_H_

#include <string>

namespace lumina {
namespace raw {

struct EmbeddedProxy {
  std::string outPath;
  int width = 0;
  int height = 0;
  int64_t bytes = 0;
};

// 提取内嵌 JPEG 到 outPath；无内嵌代理返回 false（err 说明原因）
bool extractEmbeddedJpeg(const std::string& rawPath, const std::string& outPath, EmbeddedProxy* out,
                         std::string* err);

// 仅探测是否存在内嵌代理（不解码像素）
bool hasEmbeddedProxy(const std::string& rawPath, std::string* err);

}  // namespace raw
}  // namespace lumina

#endif  // LUMINA_RAW_EMBEDDED_PROXY_H_
