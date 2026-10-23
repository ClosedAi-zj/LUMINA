// 导入重命名模板引擎：{YYYY}/{MM}/{camera}/{seq} 等占位符
#ifndef LUMINA_IMPORT_RENAME_H_
#define LUMINA_IMPORT_RENAME_H_

#include <string>

namespace lumina {
namespace import {

// 模板可用的上下文
struct RenameContext {
  int64_t capturedAtMs = 0;
  std::string camera;
  std::string lens;
  std::string location;
  std::string seqName;
  int seqNo = 1;
  int seqTotal = 1;
  int iso = 0;
  std::string originalName;  // 不含扩展名
  std::string extension;     // 不含点
};

class RenameEngine {
 public:
  // 校验模板（占位符是否受支持）
  static bool validate(const std::string& templateText, std::string* err);
  // 应用模板；含非法文件名字符时自动替换为下划线
  static std::string apply(const std::string& templateText, const RenameContext& ctx);
};

}  // namespace import
}  // namespace lumina

#endif  // LUMINA_IMPORT_RENAME_H_
