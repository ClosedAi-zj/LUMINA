// 导入重命名模板引擎实现
#include "import/rename.h"

#include <map>

#include "core/str_util.h"

namespace lumina {
namespace import {
namespace {

const std::map<std::string, std::string>& supportedTokens() {
  static const std::map<std::string, std::string> kTokens = {
      {"YYYY", "拍摄年(4位)"},   {"MM", "拍摄月"},        {"DD", "拍摄日"},
      {"HH", "拍摄时"},           {"mm", "拍摄分"},        {"ss", "拍摄秒"},
      {"camera", "机型"},         {"lens", "镜头"},        {"location", "地点"},
      {"seq", "序列内序号"},      {"seqTotal", "序列总帧"},
      {"iso", "ISO"},             {"name", "原文件名"},    {"ext", "扩展名"},
      {"focal", "焦距"},          {"fw", "光圈"}};
  return kTokens;
}

std::string sanitizeFileName(const std::string& in) {
  static const char* kIllegal = "\\/:*?\"<>|";
  std::string out;
  for (char c : in) {
    bool bad = false;
    for (const char* p = kIllegal; *p; ++p) {
      if (c == *p) {
        bad = true;
        break;
      }
    }
    out.push_back(bad ? '_' : c);
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out;
}

}  // namespace

bool RenameEngine::validate(const std::string& templateText, std::string* err) {
  if (templateText.empty()) {
    if (err) *err = "rename template must not be empty";
    return false;
  }
  size_t pos = 0;
  while ((pos = templateText.find('{', pos)) != std::string::npos) {
    size_t end = templateText.find('}', pos);
    if (end == std::string::npos) {
      if (err) *err = "unclosed '{' in rename template";
      return false;
    }
    std::string token = templateText.substr(pos + 1, end - pos - 1);
    if (supportedTokens().find(token) == supportedTokens().end()) {
      if (err) *err = "unsupported token {" + token + "}";
      return false;
    }
    pos = end + 1;
  }
  return true;
}

std::string RenameEngine::apply(const std::string& templateText, const RenameContext& ctx) {
  std::string out;
  for (size_t i = 0; i < templateText.size(); ++i) {
    if (templateText[i] != '{') {
      out.push_back(templateText[i]);
      continue;
    }
    size_t end = templateText.find('}', i);
    if (end == std::string::npos) {
      out.push_back(templateText[i]);
      continue;
    }
    std::string token = templateText.substr(i + 1, end - i - 1);
    std::string value;
    if (token == "YYYY") value = formatDate(ctx.capturedAtMs).substr(0, 4);
    else if (token == "MM") value = formatDate(ctx.capturedAtMs).substr(5, 2);
    else if (token == "DD") value = formatDate(ctx.capturedAtMs).substr(8, 2);
    else if (token == "HH") value = formatTimestamp(ctx.capturedAtMs).substr(11, 2);
    else if (token == "mm") value = formatTimestamp(ctx.capturedAtMs).substr(14, 2);
    else if (token == "ss") value = formatTimestamp(ctx.capturedAtMs).substr(17, 2);
    else if (token == "camera") value = ctx.camera;
    else if (token == "lens") value = ctx.lens;
    else if (token == "location") value = ctx.location;
    else if (token == "seq") value = padNumber(ctx.seqNo, 4);
    else if (token == "seqTotal") value = padNumber(ctx.seqTotal, 4);
    else if (token == "iso") value = std::to_string(ctx.iso);
    else if (token == "name") value = ctx.originalName;
    else if (token == "ext") value = ctx.extension;
    else value.clear();
    out += sanitizeFileName(value);
    i = end;
  }
  if (out.empty()) out = sanitizeFileName(ctx.originalName);
  return sanitizeFileName(out);
}

}  // namespace import
}  // namespace lumina
