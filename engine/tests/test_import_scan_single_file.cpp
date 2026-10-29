// 单文件导入回归：用户自由选择权 —— 单张照片 / 单个视频可直接导入，不必整夹。
//
// 能力事实（实现于 scanFolder）：路径指向文件本身时按单条目扫描，
// 格式白名单与目录扫描同一套（RAW/JPEG/视频）；目录语义零变化。
//
// 锁定：
//   S1 单个 .RW2 文件路径 → ok，1 条目，isRaw=true，大小正确
//   S2 单个 .mp4 → ok，isVideo=true（视频本就在导入白名单）
//   S3 不支持格式（.txt）→ 失败且原因可读（unsupported file type）
//   S4 路径不存在 → 失败（source folder not found），错误信息保持原文
//   S5 目录扫描行为零回退：夹内 rw2+mp4 收录、txt 排除
#include <cstdio>
#include <cstring>

#include "core/str_util.h"
#include "import/scanner.h"

using namespace lumina;

int gFailures = 0;

void check(bool condition, const char* name) {
  if (condition) {
    printf("  [PASS] %s\n", name);
  } else {
    printf("  [FAIL] %s\n", name);
    ++gFailures;
  }
}

const char* kRoot = "D:/LUMINA/build/runtime/test-import-single/home";

int main() {
  printf("=== 单文件导入 · scanFolder 文件路径语义 ===\n");

  makeDirs(kRoot);

  // 夹具：最小文件即可 —— 扫描层只看扩展名与 stat，不解析内容
  const std::string rw2 = std::string(kRoot) + "/L1000012.RW2";
  const std::string mp4 = std::string(kRoot) + "/L1000013.MP4";
  const std::string txt = std::string(kRoot) + "/readme.txt";
  const char* kRawBlob = "FAKERAW\x01\x02\x03\x04";  // 12B 占位，扫描层不读内容
  writeFileBytes(rw2, kRawBlob, nullptr);
  writeFileBytes(mp4, kRawBlob, nullptr);
  writeFileBytes(txt, "hello", nullptr);

  import::ScanResult out;
  std::string err;

  // S1 单个 RAW
  out = import::ScanResult();
  err.clear();
  bool ok = import::ImportScanner::scanFolder(rw2, &out, nullptr, nullptr, &err);
  check(ok && out.totalFiles == 1 && out.items.size() == 1, "S1 单个 RW2 → 1 条目");
  check(out.items.size() == 1 && out.items[0].isRaw && !out.items[0].isVideo, "S1 isRaw=true isVideo=false");
  check(out.totalBytes == static_cast<int64_t>(std::strlen(kRawBlob)), "S1 totalBytes = 文件大小");
  check(out.items.size() == 1 && out.items[0].pairKey == "L1000012", "S1 pairKey = 去扩展名基名");

  // S2 单个视频
  out = import::ScanResult();
  err.clear();
  ok = import::ImportScanner::scanFolder(mp4, &out, nullptr, nullptr, &err);
  check(ok && out.totalFiles == 1 && out.items.size() == 1 && out.items[0].isVideo, "S2 单个 MP4 → isVideo=true");

  // S3 不支持格式 → 失败且原因可读（不静默成功）
  out = import::ScanResult();
  err.clear();
  ok = import::ImportScanner::scanFolder(txt, &out, nullptr, nullptr, &err);
  check(!ok && err.find("unsupported file type") != std::string::npos, "S3 .txt → unsupported file type");

  // S4 路径不存在 → 原错误语义保持
  out = import::ScanResult();
  err.clear();
  ok = import::ImportScanner::scanFolder(std::string(kRoot) + "/nope.RW2", &out, nullptr, nullptr, &err);
  check(!ok && err.find("source folder not found") != std::string::npos, "S4 不存在路径 → source folder not found");

  // S5 目录语义零回退
  out = import::ScanResult();
  err.clear();
  ok = import::ImportScanner::scanFolder(kRoot, &out, nullptr, nullptr, &err);
  check(ok && out.totalFiles == 2 && out.items.size() == 2, "S5 目录扫描 → rw2+mp4 共 2 条目（txt 排除）");

  // 清理
  removeFile(rw2);
  removeFile(mp4);
  removeFile(txt);

  printf(gFailures == 0 ? "ALL PASS\n" : "FAILED %d\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
