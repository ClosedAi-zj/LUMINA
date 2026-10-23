// MD5 实现：导入校验与存储健康共用（无第三方依赖）
#ifndef LUMINA_CORE_MD5_H_
#define LUMINA_CORE_MD5_H_

#include <cstdint>
#include <string>

namespace lumina {

// 计算内存块 MD5，返回 32 位小写十六进制串
std::string md5OfBuffer(const void* data, size_t size);

// 计算文件 MD5；失败返回空串并填 err（绝不静默）
std::string md5OfFile(const std::string& path, std::string* err);

// 流式 MD5 上下文（大文件分块）
class Md5Stream {
 public:
  Md5Stream();
  void update(const void* data, size_t size);
  std::string hexDigest();

 private:
  void transform(const uint8_t block[64]);

  uint32_t state_[4];
  uint64_t count_;
  uint8_t buffer_[64];
  size_t bufferLen_;
};

}  // namespace lumina

#endif  // LUMINA_CORE_MD5_H_
