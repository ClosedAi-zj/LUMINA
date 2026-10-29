// 延时工作台实现：序列聚合 / 去闪量化 / 逐帧 sidecar 补偿 / Holy Grail 坡度 / 帧预览 / 视频渲染
//
// 实现口径（与 ipc-protocol §5.11 对齐，逐条说明"真做"与"诚实降级"）：
//  1. listSequences / getSequence：全部从 sequences + sequence_frames + photos 现算，
//     没有预置数据、没有兜底假帧。
//  2. analyzeFlicker：逐帧真解码取平均亮度（内嵌 JPEG 代理优先，LibRaw 次之），
//     算高通残差 RMS 得到 flickerIndex，算中位亮度偏离得到 brightnessGainPct。
//     关键帧测不出来就整单失败，绝不返回估算的假数字。
//  3. applyDeflicker：把每帧应补偿的 EV 以"读-改-写"方式落到该帧 sidecar 的
//     crs:Exposure2012（非破坏：原片零写入，其它字段字节保留）。
//  4. setExposureRamp：关键帧落 sequence_ramps 表 + 关键帧所在帧的 lumina:ExposureRamp，
//     并把 lrt:Keyframes/lrt:Ramp 回写到首帧（LRTimelapse / Lightroom 可读）。
//  5. previewFrame：真解码单帧 → 等比缩放到 size → JPEG 落 {home}/cache/timelapse/。
//  6. render：真逐帧渲染（解码 + 烧录 sidecar 色调 + 坡度 EV 插值）→ 交给 PATH 里的
//     ffmpeg 编码为 H.264 / H.265。ffmpeg 不在就**在 RPC 阶段**返回 5001，绝不返回一个
//     什么都不生成的 jobId；有帧渲染失败则整单失败（视频不允许静默丢帧）。
//     ProRes 在 v1.0.0 无编码器，一律 5001 + data.alternative="h264"（协议 §9-Q3）。
#include "timelapse/timelapse_service.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#include "core/logger.h"
#include "core/str_util.h"
#include "imaging/imaging.h"
#include "raw/embedded_proxy.h"
#include "xmp/xmp_document.h"

namespace lumina {
namespace timelapse {

namespace {

constexpr int kMaxWarnings = 20;  // warnings 只留前 20 条，其余折算成一条汇总（避免 198 条刷屏）

double round3(double v) {
  if (!std::isfinite(v)) return 0.0;
  return std::round(v * 1000.0) / 1000.0;
}

double clampDouble(double v, double lo, double hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// 平均亮度（Rec.709 权重，0..255）。采样步长自适应，保证大图上也不慢。
double meanLuma(const imaging::Image& image) {
  if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) return 0.0;
  const size_t pixelCount = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
  double sum = 0.0;
  size_t count = 0;
  for (size_t i = 0; i < pixelCount; ++i) {
    const size_t off = i * 3;
    if (off + 2 >= image.pixels.size()) break;
    sum += 0.2126 * image.pixels[off] + 0.7152 * image.pixels[off + 1] +
           0.0722 * image.pixels[off + 2];
    ++count;
  }
  return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

double medianOf(std::vector<double> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const size_t n = values.size();
  if (n % 2 == 1) return values[n / 2];
  return (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

// 去闪量化：
//   reference = 各帧亮度的中位数（整段序列的目标亮度，比均值抗单帧异常）
//   residual  = 每帧亮度 - 5 帧中心滑动平均（高通项 = 闪烁成分，慢速曝光变化被滤掉）
//   flickerIndex = 100 * RMS(residual) / mean           —— 越大越闪
//   brightnessGainPct = 100 * (reference / mean - 1)    —— 整段需要补偿的亮度百分比
struct FlickerStats {
  double mean = 0.0;
  double reference = 0.0;
  double residualRms = 0.0;
  double flickerIndex = 0.0;
  double brightnessGainPct = 0.0;
};

FlickerStats computeFlickerStats(const std::vector<double>& brightness) {
  FlickerStats s;
  const size_t n = brightness.size();
  if (n == 0) return s;
  double sum = 0.0;
  for (double b : brightness) sum += b;
  s.mean = sum / static_cast<double>(n);
  s.reference = medianOf(brightness);
  if (s.mean <= 1e-6) return s;

  const int half = 2;  // 5 帧窗口
  double sq = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const long lo = static_cast<long>(i) - half;
    const long hi = static_cast<long>(i) + half;
    double local = 0.0;
    int cnt = 0;
    for (long j = lo; j <= hi; ++j) {
      if (j < 0 || j >= static_cast<long>(n)) continue;
      local += brightness[static_cast<size_t>(j)];
      ++cnt;
    }
    if (cnt == 0) continue;
    const double residual = brightness[i] - local / cnt;
    sq += residual * residual;
  }
  s.residualRms = std::sqrt(sq / static_cast<double>(n));
  s.flickerIndex = 100.0 * s.residualRms / s.mean;
  s.brightnessGainPct = 100.0 * (s.reference / s.mean - 1.0);
  return s;
}

// warnings 收集器：前 kMaxWarnings 条原样保留，之后只累加计数
class WarningList {
 public:
  void add(const std::string& text) {
    ++total_;
    if (items_.size() < static_cast<size_t>(kMaxWarnings)) items_.addItem(text);
  }
  int total() const { return total_; }
  json::JsonValue toJson() const {
    json::JsonValue out = json::JsonValue::arrayValue();
    for (const auto& item : items_.arrayItems()) out.addItem(item);
    if (total_ > kMaxWarnings) {
      out.addItem("另有 " + std::to_string(total_ - kMaxWarnings) + " 条同类问题未逐条列出");
    }
    return out;
  }

 private:
  json::JsonValue items_ = json::JsonValue::arrayValue();
  int total_ = 0;
};

std::string quotePath(const std::string& path) { return "\"" + path + "\""; }

// 运行外部编码器并把 stdout/stderr 落到 logPath。
// 为什么不用 std::system：Windows 下 system() 会走 cmd.exe /c，命令以带空格的引号路径开头
// 时 cmd 会吃掉首尾引号（"D:/app/x y/ffmpeg.exe" → 'D:/app/x' 不是内部命令）。
// CreateProcess 直接吃命令行，引号语义由 CRT 规则决定，路径含空格/中文都稳。
bool runProcessToLog(const std::string& exePath, const std::string& args,
                     const std::string& logPath, int* exitCode, std::string* err) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  const std::wstring logW = utf8ToUtf16(logPath);
  HANDLE logHandle = CreateFileW(logW.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
  if (logHandle == INVALID_HANDLE_VALUE) {
    if (err) *err = "无法创建 ffmpeg 日志文件: " + logPath;
    return false;
  }
  HANDLE nullHandle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  STARTUPINFOW si;
  std::memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = logHandle;
  si.hStdError = logHandle;
  si.hStdInput = (nullHandle == INVALID_HANDLE_VALUE) ? GetStdHandle(STD_INPUT_HANDLE) : nullHandle;

  PROCESS_INFORMATION pi;
  std::memset(&pi, 0, sizeof(pi));
  const std::wstring cmdW = utf8ToUtf16(quotePath(exePath) + " " + args);
  std::vector<wchar_t> cmdBuf(cmdW.begin(), cmdW.end());
  cmdBuf.push_back(L'\0');

  const BOOL started = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(logHandle);
  if (nullHandle != INVALID_HANDLE_VALUE) CloseHandle(nullHandle);
  if (!started) {
    if (err) *err = "启动外部编码器失败（Win32 错误码 " + std::to_string(GetLastError()) + "）";
    return false;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (exitCode) *exitCode = static_cast<int>(code);
  return true;
#else
  (void)exePath;
  (void)args;
  (void)logPath;
  if (err) *err = "当前平台不支持外部编码器调用";
  if (exitCode) *exitCode = -1;
  return false;
#endif
}

// 读日志尾部，用于把 ffmpeg 的真实错误带进 job 失败信息（否则用户只看到一个退出码）
std::string tailOfFile(const std::string& path, size_t maxChars) {
  std::string text;
  std::string err;
  if (!readFile(path, &text, &err)) return std::string();
  for (char& c : text) {
    if (c == '\r' || c == '\n') c = ' ';
  }
  text = trim(text);
  if (text.size() > maxChars) text = text.substr(text.size() - maxChars);
  return text;
}

int resolutionWidth(const std::string& resolution) {
  if (resolution == "1080p") return 1920;
  if (resolution == "4K") return 3840;
  if (resolution == "6K") return 6144;
  return 0;
}

const char* ffmpegVideoEncoder(const std::string& codec) {
  if (codec == "h265") return "libx265";
  return "libx264";
}

// 等比缩放到指定宽（视频必须定宽）；高度取偶数，yuv420p 才能编码
bool scaleToWidth(const imaging::Image& src, int targetWidth, imaging::Image* dst,
                  std::string* err) {
  if (src.width <= 0 || src.height <= 0) {
    if (err) *err = "源图为空，无法缩放";
    return false;
  }
  if (targetWidth < 2) targetWidth = 2;
  int targetHeight = static_cast<int>(static_cast<long long>(targetWidth) * src.height / src.width);
  if (targetHeight < 2) targetHeight = 2;
  if (targetHeight % 2 != 0) targetHeight += 1;
  if (targetWidth % 2 != 0) targetWidth += 1;
  if (targetWidth == src.width && targetHeight == src.height) {
    *dst = src;
    return true;
  }
  if (!imaging::resizeImage(src, dst, targetWidth, targetHeight)) {
    if (err) {
      *err = "缩放失败（目标 " + std::to_string(targetWidth) + "x" + std::to_string(targetHeight) + "）";
    }
    return false;
  }
  return true;
}

// 等比缩放到长边不超过 longEdge（预览用：只缩不放，避免把小图放大成糊图）
bool scaleToLongEdge(const imaging::Image& src, int longEdge, imaging::Image* dst) {
  const int native = std::max(src.width, src.height);
  if (native <= 0 || longEdge <= 0 || native <= longEdge) {
    *dst = src;
    return true;
  }
  const double k = static_cast<double>(longEdge) / static_cast<double>(native);
  int w = static_cast<int>(src.width * k + 0.5);
  int h = static_cast<int>(src.height * k + 0.5);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  return imaging::resizeImage(src, dst, w, h);
}

// lrt:Keyframes 文本 → 关键帧（LRT 用 0 基帧号，引擎/UI 用 1 基）
bool parseLrtKeyframes(const std::string& text, std::vector<Keyframe>* out) {
  out->clear();
  size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
                                 text[pos] == '\r' || text[pos] == ',')) {
      ++pos;
    }
    const size_t start = pos;
    while (pos < text.size() && text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\n' &&
           text[pos] != '\r') {
      ++pos;
    }
    if (pos <= start) break;
    const std::string token = text.substr(start, pos - start);
    // token 形如 "50,0.35,5750"
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= token.size()) {
      const size_t comma = token.find(',', p);
      if (comma == std::string::npos) {
        parts.push_back(token.substr(p));
        break;
      }
      parts.push_back(token.substr(p, comma - p));
      p = comma + 1;
    }
    if (parts.size() < 3) continue;
    Keyframe k;
    k.frameNo = static_cast<int64_t>(std::atof(parts[0].c_str())) + 1;
    k.ev = std::atof(parts[1].c_str());
    k.temperatureK = std::atof(parts[2].c_str());
    if (k.frameNo > 0) out->push_back(k);
  }
  std::sort(out->begin(), out->end(),
            [](const Keyframe& a, const Keyframe& b) { return a.frameNo < b.frameNo; });
  return !out->empty();
}

// 关键帧 → lrt:Keyframes 文本（0 基帧号，LRT 约定）
std::string formatLrtKeyframes(const std::vector<Keyframe>& keys) {
  std::string out;
  char buf[96];
  for (size_t i = 0; i < keys.size(); ++i) {
    std::snprintf(buf, sizeof(buf), "%lld,%.3f,%.0f",
                  static_cast<long long>(keys[i].frameNo - 1), keys[i].ev,
                  keys[i].temperatureK);
    if (i > 0) out += " ";
    out += buf;
  }
  return out;
}

// [{frameNo,ev,temperatureK}] → 关键帧列表（容忍脏值：非法 frameNo 直接跳过）
void appendKeyframesFromJson(const json::JsonValue& arr, std::vector<Keyframe>* out) {
  if (!arr.isArray()) return;
  for (const auto& item : arr.arrayItems()) {
    Keyframe k;
    k.frameNo = item.get("frameNo").asInt(0);
    k.ev = item.get("ev").asDouble(0.0);
    k.temperatureK = item.get("temperatureK").asDouble(0.0);
    if (k.frameNo > 0) out->push_back(k);
  }
}

json::JsonValue keyframesToJson(const std::vector<Keyframe>& keys) {
  json::JsonValue arr = json::JsonValue::arrayValue();
  for (const Keyframe& k : keys) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("frameNo", k.frameNo);
    item.setItem("ev", round3(k.ev));
    item.setItem("temperatureK", round3(k.temperatureK));
    arr.addItem(item);
  }
  return arr;
}

}  // namespace

// ============================== 构造 ==============================

TimelapseService::TimelapseService(core::Config* config, db::Database* db, db::PhotoDao* photos,
                                   xmp::XmpService* xmp, raw::RawDecoder* decoder)
    : config_(config), db_(db), photos_(photos), xmp_(xmp), decoder_(decoder) {}

std::string TimelapseService::cacheTimelapseDir() const {
  const std::string base = config_ ? config_->cacheDir() : std::string("cache");
  return joinPath(base, "timelapse");
}

// ============================== 编码器探测 ==============================

std::string TimelapseService::detectFfmpeg(std::string* err) {
  // 1) 显式指定优先（部署/离线环境可以不进 PATH）
  const char* override = std::getenv("LUMINA_FFMPEG");
  if (override && *override) {
    const std::string candidate = normalizePath(override);
    if (fileExists(candidate)) return candidate;
    if (err) *err = std::string("LUMINA_FFMPEG 指向的文件不存在: ") + candidate;
    logWarn("timelapse", std::string("LUMINA_FFMPEG not found: ") + candidate);
    return std::string();
  }
  // 2) 系统 PATH
  const char* pathEnv = std::getenv("PATH");
  if (!pathEnv || !*pathEnv) {
    if (err) *err = "环境变量 PATH 为空，无法定位 ffmpeg.exe";
    return std::string();
  }
  const std::string paths(pathEnv);
  size_t pos = 0;
  while (pos <= paths.size()) {
    const size_t sep = paths.find(';', pos);
    const std::string dir =
        (sep == std::string::npos) ? paths.substr(pos) : paths.substr(pos, sep - pos);
    if (!dir.empty()) {
      const std::string candidate = joinPath(trim(dir), "ffmpeg.exe");
      if (fileExists(candidate)) return candidate;
    }
    if (sep == std::string::npos) break;
    pos = sep + 1;
  }
  if (err) *err = "系统 PATH 中未找到 ffmpeg.exe";
  return std::string();
}

// ============================== 序列装载 ==============================

int TimelapseService::loadSequence(int64_t seqId, SequenceInfo* info,
                                   std::vector<SeqFrame>* frames, std::string* err) {
  if (!db_) {
    if (err) *err = "数据库未就绪";
    return 2;
  }
  info->seqId = seqId;
  info->frameCount = 0;
  info->state = "idle";
  info->name.clear();
  frames->clear();

  bool hasRow = false;
  {
    std::string dbErr;
    auto q = db_->prepareCached("tlSeqById",
                                "SELECT name, frame_count, state FROM sequences WHERE id=?;", &dbErr);
    if (!q || !q->valid()) {
      if (err) *err = dbErr.empty() ? "sequences 查询准备失败" : dbErr;
      return 2;
    }
    q->bindInt64(1, seqId);
    if (q->step(&dbErr)) {
      hasRow = true;
      info->name = q->columnIsNull(0) ? std::string() : q->columnText(0);
      info->frameCount = q->columnIsNull(1) ? 0 : q->columnInt64(1);
      const std::string st = q->columnIsNull(2) ? std::string() : q->columnText(2);
      if (st == "analyzing" || st == "rendering" || st == "idle") info->state = st;
    }
    q->reset();
  }

  // 帧清单：sequence_frames 是主事实源（frame_no 显式给定，排序稳定）
  {
    std::string dbErr;
    auto q = db_->prepareCached(
        "tlSeqFrames",
        "SELECT sf.frame_no, sf.photo_id, p.file_path, p.sidecar_path, p.is_missing, p.id "
        "FROM sequence_frames sf LEFT JOIN photos p ON p.id = sf.photo_id "
        "WHERE sf.seq_id=? ORDER BY sf.frame_no ASC;",
        &dbErr);
    if (!q || !q->valid()) {
      if (err) *err = dbErr.empty() ? "sequence_frames 查询准备失败" : dbErr;
      return 2;
    }
    q->bindInt64(1, seqId);
    while (q->step(&dbErr)) {
      SeqFrame f;
      f.frameNo = q->columnInt64(0);
      f.photoId = q->columnInt64(1);
      const bool photoGone = q->columnIsNull(5);
      f.filePath = q->columnIsNull(2) ? std::string() : q->columnText(2);
      f.sidecarPath = q->columnIsNull(3) ? std::string() : q->columnText(3);
      f.missing = photoGone || q->columnIsNull(4) ? photoGone : (q->columnInt(4) != 0);
      if (!photoGone && !f.filePath.empty() && f.sidecarPath.empty() && xmp_) {
        f.sidecarPath = xmp_->sidecarPathFor(f.filePath);
      }
      frames->push_back(f);
    }
    q->reset();
  }

  // 兜底：sequence_frames 为空但 photos.seq_id 有值（导入时自动成组只写了 denormal 列）
  if (frames->empty()) {
    std::string dbErr;
    auto q = db_->prepareCached(
        "tlSeqFramesByPhotoSeq",
        "SELECT id, file_path, sidecar_path, is_missing FROM photos "
        "WHERE seq_id=? AND deleted=0 ORDER BY captured_at ASC, id ASC;",
        &dbErr);
    if (q && q->valid()) {
      q->bindInt64(1, seqId);
      int64_t frameNo = 0;
      while (q->step(&dbErr)) {
        SeqFrame f;
        f.frameNo = ++frameNo;
        f.photoId = q->columnInt64(0);
        f.filePath = q->columnIsNull(1) ? std::string() : q->columnText(1);
        f.sidecarPath = q->columnIsNull(2) ? std::string() : q->columnText(2);
        f.missing = !q->columnIsNull(3) && q->columnInt(3) != 0;
        if (!f.filePath.empty() && f.sidecarPath.empty() && xmp_) {
          f.sidecarPath = xmp_->sidecarPathFor(f.filePath);
        }
        frames->push_back(f);
      }
      q->reset();
    }
  }

  if (!hasRow && frames->empty()) {
    if (err) *err = "延时序列不存在: " + std::to_string(seqId);
    return 1;
  }
  if (info->name.empty()) info->name = "延时序列 " + std::to_string(seqId);
  if (info->frameCount <= 0) info->frameCount = static_cast<int64_t>(frames->size());
  return 0;
}

// ============================== job 追踪 ==============================

void TimelapseService::trackJob(int64_t jobId, int64_t seqId, const std::string& phase) {
  std::lock_guard<std::mutex> lock(trackedMutex_);
  // 纯内存提示表，只用于"运行中的 state/progress"覆盖；超量时整体丢弃即可（不影响功能）
  if (tracked_.size() > 512) tracked_.clear();
  TrackedJob entry;
  entry.seqId = seqId;
  entry.phase = phase;
  tracked_[jobId] = entry;
}

void TimelapseService::setSequenceState(int64_t seqId, const std::string& state) {
  if (!db_) return;
  std::string dbErr;
  auto q = db_->prepareCached("tlSeqSetState", "UPDATE sequences SET state=? WHERE id=?;", &dbErr);
  if (!q || !q->valid()) {
    logWarn("timelapse", "sequence state update prepare failed: " + dbErr);
    return;
  }
  q->bindText(1, state);
  q->bindInt64(2, seqId);
  q->step(&dbErr);
  q->reset();
}

bool TimelapseService::liveState(int64_t seqId, JobManager* jobs, std::string* state,
                                 int* progress) {
  if (!jobs) return false;
  std::lock_guard<std::mutex> lock(trackedMutex_);
  for (const auto& kv : tracked_) {
    if (kv.second.seqId != seqId) continue;
    JobState st;
    if (!jobs->getJob(kv.first, &st)) continue;
    if (st.state != "running" && st.state != "queued") continue;
    *state = st.state == "queued" ? "idle" : kv.second.phase;
    *progress = st.progress;
    return true;
  }
  return false;
}

// ============================== 5.11.1 listSequences ==============================

json::JsonValue TimelapseService::listSequences(const json::JsonValue& params, JobManager* jobs,
                                                rpc::RpcError* err) {
  (void)params;
  if (!db_) {
    *err = rpc::RpcError(rpc::kInternalError, "数据库未就绪");
    return json::JsonValue::nullValue();
  }

  // 帧数以 sequence_frames 实际条数为准（sequences.frame_count 只是导入时的快照）
  struct Row {
    int64_t seqId = 0;
    std::string name;
    std::string state = "idle";
    int64_t frameCount = 0;
  };
  std::vector<Row> rows;
  std::map<int64_t, std::string> knownNames;

  {
    std::string dbErr;
    auto q = db_->prepareCached(
        "tlSequencesList",
        "SELECT s.id, s.name, s.state, s.frame_count, COUNT(sf.photo_id) AS cnt "
        "FROM sequences s LEFT JOIN sequence_frames sf ON sf.seq_id = s.id "
        "GROUP BY s.id ORDER BY s.id ASC;",
        &dbErr);
    if (!q || !q->valid()) {
      *err = rpc::RpcError(rpc::kDbError, "timelapse.listSequences 查询准备失败: " + dbErr);
      return json::JsonValue::nullValue();
    }
    while (q->step(&dbErr)) {
      Row r;
      r.seqId = q->columnInt64(0);
      r.name = q->columnIsNull(1) ? std::string() : q->columnText(1);
      const std::string st = q->columnIsNull(2) ? std::string() : q->columnText(2);
      if (st == "analyzing" || st == "rendering" || st == "idle") r.state = st;
      const int64_t snapshot = q->columnIsNull(3) ? 0 : q->columnInt64(3);
      const int64_t actual = q->columnIsNull(4) ? 0 : q->columnInt64(4);
      r.frameCount = actual > 0 ? actual : snapshot;
      if (r.name.empty()) r.name = "延时序列 " + std::to_string(r.seqId);
      knownNames[r.seqId] = r.name;
      rows.push_back(r);
    }
    q->reset();
  }

  // 只有 photos.seq_id、没有 sequences 行的历史数据也要能列出来（否则 UI 里序列凭空消失）
  {
    std::string dbErr;
    auto q = db_->prepareCached("tlOrphanSeqCounts",
                                "SELECT seq_id, COUNT(*) FROM photos WHERE seq_id IS NOT NULL AND "
                                "seq_id>0 AND deleted=0 GROUP BY seq_id;",
                                &dbErr);
    if (q && q->valid()) {
      while (q->step(&dbErr)) {
        const int64_t seqId = q->columnInt64(0);
        const int64_t cnt = q->columnInt64(1);
        if (knownNames.find(seqId) != knownNames.end()) continue;
        Row r;
        r.seqId = seqId;
        r.name = "延时序列 " + std::to_string(seqId);
        r.frameCount = cnt;
        rows.push_back(r);
      }
      q->reset();
    }
  }

  json::JsonValue out = json::JsonValue::arrayValue();
  for (const Row& r : rows) {
    std::string state = r.state;
    int progress = 0;
    liveState(r.seqId, jobs, &state, &progress);
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("seqId", r.seqId);
    item.setItem("name", r.name);
    item.setItem("frameCount", r.frameCount);
    item.setItem("state", state);
    item.setItem("renderProgress", progress);
    out.addItem(item);
  }
  return out;
}

// ============================== 5.11.2 getSequence ==============================

json::JsonValue TimelapseService::getSequence(const json::JsonValue& params, rpc::RpcError* err) {
  const int64_t seqId = params.get("seqId").asInt(0);
  if (seqId <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.seqId 必填且必须为正整数");
    return json::JsonValue::nullValue();
  }
  SequenceInfo info;
  std::vector<SeqFrame> frames;
  std::string loadErr;
  const int rc = loadSequence(seqId, &info, &frames, &loadErr);
  if (rc == 1) {
    *err = rpc::RpcError(rpc::kFileNotFound, loadErr);
    return json::JsonValue::nullValue();
  }
  if (rc == 2) {
    *err = rpc::RpcError(rpc::kDbError, "读取序列失败: " + loadErr);
    return json::JsonValue::nullValue();
  }

  WarningList warnings;

  json::JsonValue framesJson = json::JsonValue::arrayValue();
  int64_t missingFrames = 0;
  for (const SeqFrame& f : frames) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("frameNo", f.frameNo);
    item.setItem("photoId", f.photoId);
    framesJson.addItem(item);
    const bool gone = f.missing || f.filePath.empty() || !fileExists(f.filePath);
    if (gone) ++missingFrames;
  }
  if (missingFrames > 0) {
    warnings.add("有 " + std::to_string(missingFrames) + " 帧源文件缺失（预览/渲染会跳过或失败）");
  }

  // ---- 曝光坡度 ----
  std::vector<Keyframe> ramp;
  std::string rampNote;
  loadRamp(seqId, frames, &ramp, &rampNote);
  if (!rampNote.empty()) warnings.add(rampNote);

  // ---- 去闪状态（lumina:Deflicker 标记，落在首帧 sidecar）----
  bool analyzed = false;
  double gainPct = 0.0;
  bool lrtCompatible = false;
  if (!frames.empty()) {
    std::string markerText;
    bool found = false;
    std::string readErr;
    if (readSidecarText(frames.front(), "lumina", "Deflicker", &markerText, &found, &readErr)) {
      if (found) {
        json::JsonValue marker;
        std::string parseErr;
        if (json::JsonValue::parse(markerText, &marker, &parseErr)) {
          analyzed = marker.get("analyzed").asBool(false);
          gainPct = marker.get("brightnessGainPct").asDouble(0.0);
        } else {
          warnings.add("lumina:Deflicker 内容无法解析: " + parseErr);
        }
      }
    } else if (!readErr.empty()) {
      warnings.add("首帧 sidecar 读取失败（去闪状态未知）: " + readErr);
    }

    std::string lrtKeys;
    bool hasLrt = false;
    std::string lrtErr;
    if (readSidecarText(frames.front(), "lrt", "Keyframes", &lrtKeys, &hasLrt, &lrtErr)) {
      lrtCompatible = hasLrt && !lrtKeys.empty();
    } else if (!lrtErr.empty()) {
      warnings.add("首帧 sidecar 读取失败（LRT 兼容性未知）: " + lrtErr);
    }
  }

  json::JsonValue rampJson = json::JsonValue::objectValue();
  rampJson.setItem("keyframes", keyframesToJson(ramp));

  json::JsonValue deflickerJson = json::JsonValue::objectValue();
  deflickerJson.setItem("analyzed", analyzed);
  if (analyzed) deflickerJson.setItem("brightnessGainPct", round3(gainPct));

  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("name", info.name);
  out.setItem("frameCount", frames.empty() ? info.frameCount : static_cast<int64_t>(frames.size()));
  out.setItem("frames", framesJson);
  out.setItem("exposureRamp", rampJson);
  out.setItem("deflicker", deflickerJson);
  out.setItem("lrtCompatible", lrtCompatible);
  // 只增字段：UI 忽略未知键；这里把"部分成功/降级"的原因显式暴露，避免静默
  out.setItem("missingFrames", missingFrames);
  if (warnings.total() > 0) out.setItem("warnings", warnings.toJson());
  return out;
}

// ============================== 单帧解码 / 亮度测量 ==============================

bool TimelapseService::decodeFrame(const SeqFrame& frame, imaging::Image* out,
                                   std::string* source, std::string* err) {
  if (frame.filePath.empty()) {
    if (err) *err = "帧 " + std::to_string(frame.frameNo) + " 没有文件路径（photo " +
                    std::to_string(frame.photoId) + " 索引缺失）";
    return false;
  }
  if (!fileExists(frame.filePath)) {
    if (err) *err = "帧 " + std::to_string(frame.frameNo) + " 源文件不存在: " + frame.filePath;
    return false;
  }

  const std::string ext = extensionOf(frame.filePath);
  if (raw::isSupportedExtension(ext)) {
    // 1) 内嵌 JPEG 代理：最快，且是写入器给出的真实显影（不是占位图）
    const std::string tmp = joinPath(cacheTimelapseDir(),
                                     "proxy-" + std::to_string(frame.photoId) + "-" +
                                         randomHex(6) + ".jpg");
    if (makeDirs(cacheTimelapseDir())) {
      raw::EmbeddedProxy proxy;
      std::string proxyErr;
      if (raw::extractEmbeddedJpeg(frame.filePath, tmp, &proxy, &proxyErr)) {
        imaging::Image image;
        std::string decodeErr;
        if (imaging::decodeImageFile(tmp, &image, &decodeErr)) {
          removeFile(tmp);
          *out = image;
          if (source) *source = "embedded-preview";
          return true;
        }
      }
      removeFile(tmp);
    }
    // 2) LibRaw 真解码（半尺寸加速，亮度统计足够）
    std::string rawErr;
    if (decoder_ && decoder_->decodeToBitmap(frame.filePath, true, out, &rawErr)) {
      if (source) *source = "libraw-half";
      return true;
    }
    // 3) 通用解码兜底（RAW 一般走不到这里；走到了说明非标准封装）
    imaging::Image image;
    std::string genericErr;
    if (imaging::decodeImageFile(frame.filePath, &image, &genericErr)) {
      *out = image;
      if (source) *source = "generic-decode";
      return true;
    }
    if (err) {
      *err = "帧 " + std::to_string(frame.frameNo) + " 解码失败（raw: " +
             (rawErr.empty() ? std::string("不可用") : rawErr) + "；通用: " + genericErr + "）";
    }
    return false;
  }

  imaging::Image image;
  std::string decodeErr;
  if (!imaging::decodeImageFile(frame.filePath, &image, &decodeErr)) {
    if (err) {
      *err = "帧 " + std::to_string(frame.frameNo) + " 解码失败: " + decodeErr;
    }
    return false;
  }
  *out = image;
  if (source) *source = "direct-decode";
  return true;
}

// 需要"够大"的像素时走这里：内嵌代理分辨率不足就改走 LibRaw 真解码，
// 避免把相机缩略图（可能只有 160px）放大成 1080p/4K/6K。
bool TimelapseService::decodeFrameForSize(const SeqFrame& frame, int wantLongEdge,
                                          imaging::Image* out, std::string* source,
                                          std::string* err) {
  if (frame.filePath.empty()) {
    if (err) *err = "帧 " + std::to_string(frame.frameNo) + " 没有文件路径（photo " +
                    std::to_string(frame.photoId) + " 索引缺失）";
    return false;
  }
  if (!fileExists(frame.filePath)) {
    if (err) *err = "帧 " + std::to_string(frame.frameNo) + " 源文件不存在: " + frame.filePath;
    return false;
  }
  const std::string ext = extensionOf(frame.filePath);
  if (!raw::isSupportedExtension(ext)) {
    return decodeFrame(frame, out, source, err);
  }

  // 1) 内嵌代理：只有分辨率够用才用（wantLongEdge 的 80% 起算，允许 1.25 倍插值）
  imaging::Image proxyImage;
  bool haveProxy = false;
  const std::string tmp = joinPath(cacheTimelapseDir(),
                                   "proxy-" + std::to_string(frame.photoId) + "-" + randomHex(6) +
                                       ".jpg");
  if (makeDirs(cacheTimelapseDir())) {
    raw::EmbeddedProxy proxy;
    std::string proxyErr;
    if (raw::extractEmbeddedJpeg(frame.filePath, tmp, &proxy, &proxyErr)) {
      std::string decodeErr;
      if (imaging::decodeImageFile(tmp, &proxyImage, &decodeErr)) haveProxy = true;
    }
    removeFile(tmp);
  }
  const int proxyLong = haveProxy ? std::max(proxyImage.width, proxyImage.height) : 0;
  const int needed = static_cast<int>(wantLongEdge * 0.8);
  if (haveProxy && proxyLong >= needed) {
    *out = proxyImage;
    if (source) *source = "embedded-preview";
    return true;
  }

  // 2) LibRaw 真解码：目标 ≤2048 用半尺寸（快），4K/6K 用全尺寸（清晰）
  std::string rawErr;
  if (decoder_) {
    const bool half = wantLongEdge <= 2048;
    imaging::Image full;
    if (decoder_->decodeToBitmap(frame.filePath, half, &full, &rawErr)) {
      *out = full;
      if (source) *source = half ? "libraw-half" : "libraw-full";
      return true;
    }
  }

  // 3) 退一步用代理（分辨率不足，调用方在 warnings 里体现）
  if (haveProxy) {
    *out = proxyImage;
    if (source) *source = "embedded-preview-upscaled";
    return true;
  }

  // 4) 通用解码兜底
  imaging::Image image;
  std::string genericErr;
  if (imaging::decodeImageFile(frame.filePath, &image, &genericErr)) {
    *out = image;
    if (source) *source = "generic-decode";
    return true;
  }
  if (err) {
    *err = "帧 " + std::to_string(frame.frameNo) + " 解码失败（raw: " +
           (rawErr.empty() ? std::string("不可用") : rawErr) + "；通用: " + genericErr + "）";
  }
  return false;
}

bool TimelapseService::frameBrightness(const SeqFrame& frame, double* out, std::string* source,
                                       std::string* err) {
  imaging::Image image;
  std::string src;
  if (!decodeFrame(frame, &image, &src, err)) return false;
  if (source) *source = src;
  // 亮度统计不需要原尺寸：先缩到 320 宽，198 帧也不卡
  imaging::Image small;
  if (image.width > 320) {
    if (!scaleToLongEdge(image, 320, &small)) return false;
  } else {
    small = image;
  }
  *out = meanLuma(small);
  return true;
}

// ============================== 5.11.3 analyzeFlicker ==============================

json::JsonValue TimelapseService::analyzeFlicker(const json::JsonValue& params, JobManager* jobs,
                                                 rpc::RpcError* err) {
  const int64_t seqId = params.get("seqId").asInt(0);
  if (seqId <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.seqId 必填且必须为正整数");
    return json::JsonValue::nullValue();
  }
  if (!jobs) {
    *err = rpc::RpcError(rpc::kInternalError, "任务管理器未就绪");
    return json::JsonValue::nullValue();
  }
  SequenceInfo info;
  std::vector<SeqFrame> frames;
  std::string loadErr;
  const int rc = loadSequence(seqId, &info, &frames, &loadErr);
  if (rc == 1) {
    *err = rpc::RpcError(rpc::kFileNotFound, loadErr);
    return json::JsonValue::nullValue();
  }
  if (rc == 2) {
    *err = rpc::RpcError(rpc::kDbError, "读取序列失败: " + loadErr);
    return json::JsonValue::nullValue();
  }
  if (frames.size() < 2) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "延时序列帧数不足（当前 " + std::to_string(frames.size()) +
                             " 帧），无法做去闪分析");
    return json::JsonValue::nullValue();
  }

  setSequenceState(seqId, "analyzing");
  const int64_t jobId = jobs->start("timelapse.analyzeFlicker",
                                    [this, seqId, frames](JobContext& ctx) {
                                      runAnalyze(seqId, frames, ctx);
                                    });
  trackJob(jobId, seqId, "analyzing");
  logInfo("timelapse", "analyzeFlicker job " + std::to_string(jobId) + " started for seq " +
                           std::to_string(seqId) + " (" + std::to_string(frames.size()) + " frames)");
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("jobId", jobId);
  return out;
}

void TimelapseService::runAnalyze(int64_t seqId, std::vector<SeqFrame> frames, JobContext& ctx) {
  // 不论成功/失败/取消，序列状态都必须回到 idle（否则 UI 永远转圈）
  struct StateReset {
    TimelapseService* self;
    int64_t seqId;
    ~StateReset() { self->setSequenceState(seqId, "idle"); }
  } guard{this, seqId};

  const int total = static_cast<int>(frames.size());
  WarningList warnings;
  std::vector<double> brightness;
  std::string source;
  int measured = 0;
  bool cancelled = false;

  ctx.progress(0, "去闪分析：测量 " + std::to_string(total) + " 帧亮度");
  for (int i = 0; i < total; ++i) {
    if (ctx.isCancelled()) {
      cancelled = true;
      break;
    }
    double value = 0.0;
    std::string src;
    std::string frameErr;
    if (frameBrightness(frames[static_cast<size_t>(i)], &value, &src, &frameErr)) {
      if (source.empty()) source = src;
      brightness.push_back(value);
      ++measured;
    } else {
      warnings.add(frameErr.empty() ? ("帧 " + std::to_string(frames[i].frameNo) + " 亮度测量失败")
                                    : frameErr);
    }
    const int percent = (i + 1) * 100 / total;
    ctx.progress(percent, "已测量 " + std::to_string(measured) + "/" + std::to_string(i + 1) + " 帧");
  }

  if (measured < 2) {
    throw std::runtime_error("去闪分析失败：可测量帧数不足（成功 " + std::to_string(measured) +
                             " / 共 " + std::to_string(total) + " 帧）" +
                             (warnings.total() > 0 ? "，详见事件错误或引擎日志" : ""));
  }

  const FlickerStats stats = computeFlickerStats(brightness);

  // 量化结果落首帧 sidecar（lumina:Deflicker），getSequence 的 deflicker.analyzed 由此而来
  json::JsonValue marker = json::JsonValue::objectValue();
  marker.setItem("analyzed", true);
  marker.setItem("brightnessGainPct", round3(stats.brightnessGainPct));
  marker.setItem("flickerIndex", round3(stats.flickerIndex));
  marker.setItem("meanBrightness", round3(stats.mean));
  marker.setItem("referenceBrightness", round3(stats.reference));
  marker.setItem("frames", measured);
  marker.setItem("analyzedAt", nowMs());
  std::string markerErr;
  if (!writeDeflickerMarker(frames.front(), marker, &markerErr)) {
    warnings.add("去闪量化结果未能写入首帧 sidecar: " + markerErr);
  }

  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("seqId", seqId);
  result.setItem("frames", total);
  result.setItem("analyzed", measured);
  result.setItem("brightnessGainPct", round3(stats.brightnessGainPct));
  result.setItem("flickerIndex", round3(stats.flickerIndex));
  result.setItem("meanBrightness", round3(stats.mean));
  result.setItem("referenceBrightness", round3(stats.reference));
  result.setItem("source", source.empty() ? std::string("unknown") : source);
  result.setItem("warnings", warnings.toJson());
  result.setItem("cancelled", cancelled);
  ctx.setResult(result);
  logInfo("timelapse", "analyzeFlicker job " + std::to_string(ctx.jobId()) + " finished: measured=" +
                           std::to_string(measured) + "/" + std::to_string(total) +
                           " flickerIndex=" + std::to_string(round3(stats.flickerIndex)) +
                           " gainPct=" + std::to_string(round3(stats.brightnessGainPct)));
}

// ============================== 5.11.4 applyDeflicker ==============================

json::JsonValue TimelapseService::applyDeflicker(const json::JsonValue& params, JobManager* jobs,
                                                 rpc::RpcError* err) {
  const int64_t seqId = params.get("seqId").asInt(0);
  if (seqId <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.seqId 必填且必须为正整数");
    return json::JsonValue::nullValue();
  }
  const double strength = params.get("strength").asDouble(50.0);
  if (!(strength > 0.0) || strength > 100.0) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "params.strength 必须在 1..100 之间（当前 " +
                             std::to_string(round3(strength)) + "）");
    return json::JsonValue::nullValue();
  }
  if (!jobs) {
    *err = rpc::RpcError(rpc::kInternalError, "任务管理器未就绪");
    return json::JsonValue::nullValue();
  }
  if (!xmp_) {
    *err = rpc::RpcError(rpc::kInternalError, "XMP 服务未就绪，无法写入去闪参数");
    return json::JsonValue::nullValue();
  }
  SequenceInfo info;
  std::vector<SeqFrame> frames;
  std::string loadErr;
  const int rc = loadSequence(seqId, &info, &frames, &loadErr);
  if (rc == 1) {
    *err = rpc::RpcError(rpc::kFileNotFound, loadErr);
    return json::JsonValue::nullValue();
  }
  if (rc == 2) {
    *err = rpc::RpcError(rpc::kDbError, "读取序列失败: " + loadErr);
    return json::JsonValue::nullValue();
  }
  if (frames.size() < 2) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "延时序列帧数不足（当前 " + std::to_string(frames.size()) +
                             " 帧），无法应用去闪");
    return json::JsonValue::nullValue();
  }

  setSequenceState(seqId, "analyzing");
  const int64_t jobId =
      jobs->start("timelapse.applyDeflicker", [this, seqId, frames, strength](JobContext& ctx) {
        runApplyDeflicker(seqId, frames, strength, ctx);
      });
  trackJob(jobId, seqId, "analyzing");
  logInfo("timelapse", "applyDeflicker job " + std::to_string(jobId) + " started for seq " +
                           std::to_string(seqId) + " strength=" + std::to_string(round3(strength)));
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("jobId", jobId);
  return out;
}

void TimelapseService::runApplyDeflicker(int64_t seqId, std::vector<SeqFrame> frames,
                                         double strength, JobContext& ctx) {
  struct StateReset {
    TimelapseService* self;
    int64_t seqId;
    ~StateReset() { self->setSequenceState(seqId, "idle"); }
  } guard{this, seqId};

  const int total = static_cast<int>(frames.size());
  const double ratio = strength / 100.0;
  WarningList warnings;
  std::vector<double> brightness;
  std::vector<size_t> measuredIndex;  // brightness[k] 对应的 frames 下标
  std::string source;
  int measured = 0;
  bool cancelled = false;

  // ---- 阶段 1（0..45%）：测量每帧亮度，得到目标亮度 ----
  ctx.progress(0, "去闪：测量 " + std::to_string(total) + " 帧亮度");
  for (int i = 0; i < total; ++i) {
    if (ctx.isCancelled()) {
      cancelled = true;
      break;
    }
    double value = 0.0;
    std::string src;
    std::string frameErr;
    if (frameBrightness(frames[static_cast<size_t>(i)], &value, &src, &frameErr)) {
      if (source.empty()) source = src;
      brightness.push_back(value);
      measuredIndex.push_back(static_cast<size_t>(i));
      ++measured;
    } else {
      warnings.add(frameErr.empty() ? ("帧 " + std::to_string(frames[i].frameNo) + " 亮度测量失败")
                                    : frameErr);
    }
    ctx.progress((i + 1) * 45 / total, "已测量 " + std::to_string(measured) + "/" +
                                           std::to_string(i + 1) + " 帧");
  }
  if (cancelled) {
    json::JsonValue result = json::JsonValue::objectValue();
    result.setItem("seqId", seqId);
    result.setItem("frames", total);
    result.setItem("compensated", 0);
    result.setItem("cancelled", true);
    result.setItem("warnings", warnings.toJson());
    ctx.setResult(result);
    return;
  }
  if (measured < 2) {
    throw std::runtime_error("去闪失败：可测量帧数不足（成功 " + std::to_string(measured) + " / 共 " +
                             std::to_string(total) + " 帧）");
  }

  const FlickerStats stats = computeFlickerStats(brightness);
  if (stats.reference <= 1e-6) {
    throw std::runtime_error("去闪失败：参考亮度为 0（整段序列接近全黑，无法补偿）");
  }

  // ---- 阶段 2（45..100%）：逐帧写 sidecar 的 crs:Exposure2012（读-改-写，非破坏）----
  int compensated = 0;
  int skipped = 0;
  int failed = 0;
  double evSum = 0.0;
  for (size_t k = 0; k < measuredIndex.size(); ++k) {
    if (ctx.isCancelled()) {
      cancelled = true;
      break;
    }
    const SeqFrame& frame = frames[measuredIndex[k]];
    const double b = brightness[k];
    if (b <= 1e-6) {
      ++skipped;
      warnings.add("帧 " + std::to_string(frame.frameNo) + " 亮度为 0，跳过补偿");
      continue;
    }
    // 目标：把该帧亮度拉到序列参考亮度。applyToneApprox 用 2^EV 线性放大 sRGB 值，
    // 所以补偿量直接是 log2(reference / brightness)，再乘用户强度。
    double ev = std::log2(stats.reference / b) * ratio;
    ev = clampDouble(ev, -3.0, 3.0);
    if (std::fabs(ev) < 0.005) {
      ++skipped;
      continue;
    }

    json::JsonValue paramsJson;
    std::string loadParamsErr;
    if (!xmp_->loadParams(frame.photoId, &paramsJson, &loadParamsErr)) {
      ++failed;
      warnings.add("帧 " + std::to_string(frame.frameNo) + " 读取 sidecar 失败: " + loadParamsErr);
      continue;
    }
    const double oldEv = paramsJson.get("exposure").asDouble(0.0);
    const double newEv = clampDouble(oldEv + ev, -5.0, 5.0);
    paramsJson.setItem("exposure", round3(newEv));
    std::string saveErr;
    if (!xmp_->saveParams(frame.photoId, paramsJson, &saveErr)) {
      ++failed;
      warnings.add("帧 " + std::to_string(frame.frameNo) + " 写 sidecar 失败: " + saveErr);
      continue;
    }
    ++compensated;
    evSum += newEv - oldEv;
    const int percent = 45 + static_cast<int>((k + 1) * 55 / measuredIndex.size());
    ctx.progress(percent, "已补偿 " + std::to_string(compensated) + " 帧（失败 " +
                              std::to_string(failed) + "）");
  }

  if (compensated == 0 && failed > 0) {
    throw std::runtime_error("去闪失败：所有帧的 sidecar 都写不进去（失败 " +
                             std::to_string(failed) + " 帧），请检查文件权限或外部修改冲突");
  }
  if (compensated == 0 && failed == 0) {
    // 零静默：亮度本来就稳，什么都没改也必须让用户看见
    warnings.add("序列亮度已稳定（每帧补偿量都小于 0.005 EV），本次没有修改任何帧");
  }

  // 记录"已应用去闪"的量化状态（analysed / applied + 平均补偿量）
  json::JsonValue marker = json::JsonValue::objectValue();
  marker.setItem("analyzed", true);
  marker.setItem("applied", true);
  marker.setItem("strength", round3(strength));
  marker.setItem("brightnessGainPct", round3(stats.brightnessGainPct));
  marker.setItem("flickerIndex", round3(stats.flickerIndex));
  marker.setItem("meanEv", round3(compensated > 0 ? evSum / compensated : 0.0));
  marker.setItem("appliedFrames", compensated);
  marker.setItem("analyzedAt", nowMs());
  std::string markerErr;
  if (!writeDeflickerMarker(frames.front(), marker, &markerErr)) {
    warnings.add("去闪结果标记未能写入首帧 sidecar: " + markerErr);
  }

  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("seqId", seqId);
  result.setItem("frames", total);
  result.setItem("measured", measured);
  result.setItem("compensated", compensated);
  result.setItem("skipped", skipped);
  result.setItem("failed", failed);
  result.setItem("strength", round3(strength));
  result.setItem("meanEv", round3(compensated > 0 ? evSum / compensated : 0.0));
  result.setItem("referenceBrightness", round3(stats.reference));
  result.setItem("source", source.empty() ? std::string("unknown") : source);
  result.setItem("warnings", warnings.toJson());
  result.setItem("cancelled", cancelled);
  ctx.setResult(result);
  logInfo("timelapse", "applyDeflicker job " + std::to_string(ctx.jobId()) + " finished: seq=" +
                           std::to_string(seqId) + " compensated=" + std::to_string(compensated) +
                           " skipped=" + std::to_string(skipped) + " failed=" +
                           std::to_string(failed));
}

// ============================== 坡度：读 / 写 / 插值 ==============================

void TimelapseService::loadRamp(int64_t seqId, const std::vector<SeqFrame>& frames,
                                std::vector<Keyframe>* out, std::string* note) {
  out->clear();
  if (note) note->clear();

  // 1) sequence_ramps（引擎写入的唯一真相源）
  //    注意：行存在但 keyframes_json 是空数组 = 用户清空了坡度，这是权威结论，
  //    不能再往下兜底（否则会把 sidecar 里的旧关键帧"复活"）。
  bool authoritative = false;
  if (db_) {
    std::string dbErr;
    auto q = db_->prepareCached("tlRampGet", "SELECT keyframes_json FROM sequence_ramps WHERE seq_id=?;",
                                &dbErr);
    if (q && q->valid()) {
      q->bindInt64(1, seqId);
      if (q->step(&dbErr) && !q->columnIsNull(0)) {
        json::JsonValue parsed;
        std::string parseErr;
        if (json::JsonValue::parse(q->columnText(0), &parsed, &parseErr) && parsed.isArray()) {
          appendKeyframesFromJson(parsed, out);
          authoritative = true;
        } else if (note) {
          *note = "sequence_ramps 关键帧 JSON 解析失败: " + parseErr;
        }
      }
      q->reset();
    } else if (note) {
      *note = "sequence_ramps 读取失败: " + dbErr;
    }
  }
  if (authoritative) return;

  // 2) 兜底 A：首帧 sidecar 的 lumina:ExposureRamp（引擎自己写的整块 JSON）
  //    注意：这里必须自己读，不能指望 xmp_->loadParams —— XmpMapper::flattenParams 会丢掉
  //    对象型顶层键（见 writeToDoc 前的 flattenParams），lumina:ExposureRamp 这类整块 JSON
  //    经 saveParams 写不进去（同样的坑也影响 masks/adjustmentStack 的写入路径）。
  if (frames.empty()) return;
  {
    std::string text;
    bool found = false;
    std::string readErr;
    if (readSidecarText(frames.front(), "lumina", "ExposureRamp", &text, &found, &readErr) &&
        found) {
      json::JsonValue parsed;
      std::string parseErr;
      if (json::JsonValue::parse(text, &parsed, &parseErr)) {
        appendKeyframesFromJson(parsed.get("keyframes"), out);
        if (!out->empty()) return;
      } else if (note) {
        *note = "lumina:ExposureRamp 解析失败: " + parseErr;
      }
    }
  }

  // 3) 兜底 B：首帧 sidecar 的 lrt:Keyframes（导入/LRT 侧已有的关键帧，读回来不丢）
  std::string text;
  bool found = false;
  std::string readErr;
  if (readSidecarText(frames.front(), "lrt", "Keyframes", &text, &found, &readErr) && found) {
    if (!parseLrtKeyframes(text, out)) out->clear();
  }
}

bool TimelapseService::saveRampRow(int64_t seqId, const std::vector<Keyframe>& keys,
                                   std::string* err) {
  if (!db_) {
    if (err) *err = "数据库未就绪";
    return false;
  }
  std::string dbErr;
  auto q = db_->prepareCached(
      "tlRampUpsert",
      "INSERT INTO sequence_ramps(seq_id, keyframes_json, updated_at) VALUES(?,?,?) "
      "ON CONFLICT(seq_id) DO UPDATE SET keyframes_json=excluded.keyframes_json, "
      "updated_at=excluded.updated_at;",
      &dbErr);
  if (!q || !q->valid()) {
    if (err) *err = dbErr.empty() ? "sequence_ramps 写入准备失败" : dbErr;
    return false;
  }
  q->bindInt64(1, seqId);
  q->bindText(2, keyframesToJson(keys).serialize());
  q->bindInt64(3, nowMs());
  const bool ok = q->step(&dbErr);
  q->reset();
  if (!ok && err) *err = dbErr.empty() ? "sequence_ramps 写入失败" : dbErr;
  return ok;
}

double TimelapseService::rampEvAt(const std::vector<Keyframe>& keys, int64_t frameNo) {
  if (keys.empty()) return 0.0;
  if (frameNo <= keys.front().frameNo) return keys.front().ev;
  if (frameNo >= keys.back().frameNo) return keys.back().ev;
  for (size_t i = 1; i < keys.size(); ++i) {
    if (frameNo <= keys[i].frameNo) {
      const Keyframe& a = keys[i - 1];
      const Keyframe& b = keys[i];
      const int64_t span = b.frameNo - a.frameNo;
      if (span <= 0) return b.ev;
      const double t = static_cast<double>(frameNo - a.frameNo) / static_cast<double>(span);
      return a.ev + (b.ev - a.ev) * t;
    }
  }
  return keys.back().ev;
}

// ============================== 5.11.5 setExposureRamp ==============================

json::JsonValue TimelapseService::setExposureRamp(const json::JsonValue& params,
                                                  rpc::RpcError* err) {
  const int64_t seqId = params.get("seqId").asInt(0);
  if (seqId <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.seqId 必填且必须为正整数");
    return json::JsonValue::nullValue();
  }
  const json::JsonValue& input = params.get("keyframes");
  if (!input.isArray()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.keyframes 必须是数组");
    return json::JsonValue::nullValue();
  }

  SequenceInfo info;
  std::vector<SeqFrame> frames;
  std::string loadErr;
  const int rc = loadSequence(seqId, &info, &frames, &loadErr);
  if (rc == 1) {
    *err = rpc::RpcError(rpc::kFileNotFound, loadErr);
    return json::JsonValue::nullValue();
  }
  if (rc == 2) {
    *err = rpc::RpcError(rpc::kDbError, "读取序列失败: " + loadErr);
    return json::JsonValue::nullValue();
  }

  std::vector<Keyframe> keys;
  for (const auto& item : input.arrayItems()) {
    if (!item.isObject()) {
      *err = rpc::RpcError(rpc::kInvalidParams, "每个关键帧必须是对象 {frameNo, ev, temperatureK}");
      return json::JsonValue::nullValue();
    }
    Keyframe k;
    k.frameNo = item.get("frameNo").asInt(0);
    k.ev = item.get("ev").asDouble(0.0);
    k.temperatureK = item.get("temperatureK").asDouble(0.0);
    if (k.frameNo <= 0) {
      *err = rpc::RpcError(rpc::kInvalidParams, "关键帧 frameNo 必须为正整数（从 1 开始计帧）");
      return json::JsonValue::nullValue();
    }
    if (k.ev < -5.0 || k.ev > 5.0) {
      *err = rpc::RpcError(rpc::kInvalidParams,
                           "关键帧 " + std::to_string(k.frameNo) + " 的 ev 超出范围 [-5, 5]");
      return json::JsonValue::nullValue();
    }
    if (k.temperatureK < 2000.0 || k.temperatureK > 12000.0) {
      *err = rpc::RpcError(rpc::kInvalidParams,
                           "关键帧 " + std::to_string(k.frameNo) +
                               " 的 temperatureK 超出范围 [2000, 12000]");
      return json::JsonValue::nullValue();
    }
    keys.push_back(k);
  }
  std::sort(keys.begin(), keys.end(),
            [](const Keyframe& a, const Keyframe& b) { return a.frameNo < b.frameNo; });
  for (size_t i = 1; i < keys.size(); ++i) {
    if (keys[i].frameNo == keys[i - 1].frameNo) {
      *err = rpc::RpcError(rpc::kInvalidParams,
                           "关键帧重复：第 " + std::to_string(keys[i].frameNo) + " 帧出现多次");
      return json::JsonValue::nullValue();
    }
  }

  // ---- 1. 落库（sequence_ramps）----
  std::string saveErr;
  if (!saveRampRow(seqId, keys, &saveErr)) {
    *err = rpc::RpcError(rpc::kDbError, "曝光坡度保存失败: " + saveErr);
    return json::JsonValue::nullValue();
  }

  // ---- 2. 镜像到关键帧所在帧的 sidecar（lumina:ExposureRamp，JSON 块）----
  json::JsonValue rampJson = json::JsonValue::objectValue();
  rampJson.setItem("seqId", seqId);
  rampJson.setItem("frameCount", frames.empty() ? info.frameCount
                                                : static_cast<int64_t>(frames.size()));
  rampJson.setItem("updatedAt", nowMs());
  rampJson.setItem("keyframes", keyframesToJson(keys));

  WarningList warnings;
  int sidecarWritten = 0;
  std::map<int64_t, size_t> frameIndex;  // frameNo → frames 下标
  for (size_t i = 0; i < frames.size(); ++i) frameIndex[frames[i].frameNo] = i;
  if (xmp_) {
    for (const Keyframe& k : keys) {
      auto it = frameIndex.find(k.frameNo);
      if (it == frameIndex.end()) {
        // 关键帧落在序列范围外：不静默丢弃，明确告知
        warnings.add("关键帧第 " + std::to_string(k.frameNo) + " 帧不在该序列中，已跳过 sidecar 写入");
        continue;
      }
      // 直接写 sidecar 的 lumina:ExposureRamp（整块 JSON 文本）。
      // 不走 xmp_->saveParams：XmpMapper::flattenParams 会丢弃对象型顶层键，
      // 那条路会把 exposureRamp 静默吞掉（写入返回 true 但文件里什么都没写）。
      std::vector<SidecarField> fields;
      SidecarField field;
      field.prefix = "lumina";
      field.name = "ExposureRamp";
      field.value = rampJson.serialize();
      fields.push_back(field);
      std::string patchErr;
      if (writeSidecarText(frames[it->second], fields, &patchErr)) {
        ++sidecarWritten;
      } else {
        warnings.add("第 " + std::to_string(k.frameNo) + " 帧 lumina:ExposureRamp 写入失败: " +
                     patchErr);
      }
    }
    // 清空坡度：首帧写空 keyframes，并删除 lrt: 两个节点（否则 LRT/Lightroom 仍会读到旧关键帧）
    if (keys.empty() && !frames.empty()) {
      std::vector<SidecarField> fields;
      SidecarField rampField;
      rampField.prefix = "lumina";
      rampField.name = "ExposureRamp";
      rampField.value = rampJson.serialize();
      fields.push_back(rampField);
      SidecarField removeKeys;
      removeKeys.prefix = "lrt";
      removeKeys.name = "Keyframes";
      removeKeys.remove = true;
      fields.push_back(removeKeys);
      SidecarField removeRamp;
      removeRamp.prefix = "lrt";
      removeRamp.name = "Ramp";
      removeRamp.remove = true;
      fields.push_back(removeRamp);
      std::string clearErr;
      if (writeSidecarText(frames.front(), fields, &clearErr)) {
        ++sidecarWritten;
      } else {
        warnings.add("清空 lumina:ExposureRamp / lrt:Keyframes 失败: " + clearErr);
      }
    }
  } else {
    warnings.add("XMP 服务未就绪：坡度只写入了数据库，未写入 sidecar");
  }

  // ---- 3. 尽力回写 lrt:（LRTimelapse / Lightroom 可读；失败只记 warning）----
  bool lrtWritten = false;
  if (!frames.empty() && !keys.empty()) {
    std::vector<SidecarField> fields;
    SidecarField keyframesField;
    keyframesField.prefix = "lrt";
    keyframesField.name = "Keyframes";
    keyframesField.value = formatLrtKeyframes(keys);
    fields.push_back(keyframesField);
    SidecarField rampField;
    rampField.prefix = "lrt";
    rampField.name = "Ramp";
    rampField.value = "Linear";
    fields.push_back(rampField);
    std::string lrtErr;
    if (writeSidecarText(frames.front(), fields, &lrtErr)) {
      lrtWritten = true;
    } else {
      warnings.add("lrt: 关键帧回写失败（Lightroom/LRTimelapse 将读不到坡度）: " + lrtErr);
    }
  }

  // 协议要求返回 {}；这里只增字段，把"部分成功"显式交代出来（UI 忽略未知键）
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("seqId", seqId);
  out.setItem("keyframes", static_cast<int64_t>(keys.size()));
  out.setItem("sidecarWritten", sidecarWritten);
  out.setItem("lrtWritten", lrtWritten);
  if (warnings.total() > 0) out.setItem("warnings", warnings.toJson());
  logInfo("timelapse", "setExposureRamp seq=" + std::to_string(seqId) + " keyframes=" +
                           std::to_string(keys.size()) + " sidecar=" +
                           std::to_string(sidecarWritten) +
                           " lrt=" + (lrtWritten ? "ok" : "failed"));
  return out;
}

// ============================== 5.11.6 previewFrame ==============================

json::JsonValue TimelapseService::previewFrame(const json::JsonValue& params, rpc::RpcError* err) {
  const int64_t seqId = params.get("seqId").asInt(0);
  const int64_t frameNo = params.get("frameNo").asInt(0);
  if (seqId <= 0 || frameNo <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.seqId / params.frameNo 必须为正整数");
    return json::JsonValue::nullValue();
  }
  int size = static_cast<int>(params.get("size").asInt(1080));
  if (size < 64) size = 64;
  if (size > 4096) size = 4096;

  SequenceInfo info;
  std::vector<SeqFrame> frames;
  std::string loadErr;
  const int rc = loadSequence(seqId, &info, &frames, &loadErr);
  if (rc == 1) {
    *err = rpc::RpcError(rpc::kFileNotFound, loadErr);
    return json::JsonValue::nullValue();
  }
  if (rc == 2) {
    *err = rpc::RpcError(rpc::kDbError, "读取序列失败: " + loadErr);
    return json::JsonValue::nullValue();
  }

  const SeqFrame* target = nullptr;
  for (const SeqFrame& f : frames) {
    if (f.frameNo == frameNo) {
      target = &f;
      break;
    }
  }
  if (!target) {
    *err = rpc::RpcError(rpc::kFileNotFound,
                         "序列 " + std::to_string(seqId) + " 中没有第 " + std::to_string(frameNo) +
                             " 帧");
    return json::JsonValue::nullValue();
  }

  imaging::Image image;
  std::string source;
  std::string decodeErr;
  if (!decodeFrameForSize(*target, size, &image, &source, &decodeErr)) {
    *err = rpc::RpcError(rpc::kRawDecodeFailed, decodeErr);
    return json::JsonValue::nullValue();
  }
  imaging::Image scaled;
  if (!scaleToLongEdge(image, size, &scaled)) {
    *err = rpc::RpcError(rpc::kInternalError, "帧预览缩放失败");
    return json::JsonValue::nullValue();
  }

  const std::string dir = cacheTimelapseDir();
  if (!dirExists(dir) && !makeDirs(dir)) {
    *err = rpc::RpcError(rpc::kIoError, "创建预览缓存目录失败: " + dir);
    return json::JsonValue::nullValue();
  }
  const std::string path =
      joinPath(dir, std::to_string(seqId) + "-" + std::to_string(frameNo) + ".jpg");
  std::string encodeErr;
  if (!imaging::encodeJpeg(path, scaled, 88, &encodeErr)) {
    *err = rpc::RpcError(rpc::kIoError, "帧预览落盘失败: " + encodeErr);
    return json::JsonValue::nullValue();
  }

  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("path", path);
  out.setItem("width", scaled.width);
  out.setItem("height", scaled.height);
  out.setItem("source", source);
  return out;
}

// ============================== 5.11.8 frameHistogram ==============================

json::JsonValue TimelapseService::frameHistogram(const json::JsonValue& params, rpc::RpcError* err) {
  const int64_t seqId = params.get("seqId").asInt(0);
  const int64_t frameNo = params.get("frameNo").asInt(0);
  if (seqId <= 0 || frameNo <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.seqId / params.frameNo 必须为正整数");
    return json::JsonValue::nullValue();
  }
  // 直方图尺度无关：取小值解码更快（与 previewFrame 的 decodeFrameForSize 同口径）
  int size = static_cast<int>(params.get("size").asInt(512));
  if (size < 64) size = 64;
  if (size > 4096) size = 4096;

  SequenceInfo info;
  std::vector<SeqFrame> frames;
  std::string loadErr;
  const int rc = loadSequence(seqId, &info, &frames, &loadErr);
  if (rc == 1) {
    *err = rpc::RpcError(rpc::kFileNotFound, loadErr);
    return json::JsonValue::nullValue();
  }
  if (rc == 2) {
    *err = rpc::RpcError(rpc::kDbError, "读取序列失败: " + loadErr);
    return json::JsonValue::nullValue();
  }

  const SeqFrame* target = nullptr;
  for (const SeqFrame& f : frames) {
    if (f.frameNo == frameNo) {
      target = &f;
      break;
    }
  }
  if (!target) {
    *err = rpc::RpcError(rpc::kFileNotFound,
                         "序列 " + std::to_string(seqId) + " 中没有第 " + std::to_string(frameNo) +
                             " 帧");
    return json::JsonValue::nullValue();
  }

  imaging::Image image;
  std::string source;
  std::string decodeErr;
  if (!decodeFrameForSize(*target, size, &image, &source, &decodeErr)) {
    *err = rpc::RpcError(rpc::kRawDecodeFailed, decodeErr);
    return json::JsonValue::nullValue();
  }
  int64_t r[256], g[256], b[256], luma[256];
  imaging::computeHistogram(image, r, g, b, luma);
  auto toArray = [](const int64_t* v) {
    json::JsonValue out = json::JsonValue::arrayValue();
    for (int i = 0; i < 256; ++i) out.addItem(v[i]);
    return out;
  };
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("r", toArray(r));
  out.setItem("g", toArray(g));
  out.setItem("b", toArray(b));
  out.setItem("luma", toArray(luma));
  return out;
}

// ============================== 5.11.7 render ==============================

json::JsonValue TimelapseService::render(const json::JsonValue& params, JobManager* jobs,
                                         rpc::RpcError* err) {
  const int64_t seqId = params.get("seqId").asInt(0);
  if (seqId <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.seqId 必填且必须为正整数");
    return json::JsonValue::nullValue();
  }
  RenderSpec spec;
  spec.resolution = params.get("resolution").asString("4K");
  spec.fps = static_cast<int>(params.get("fps").asInt(30));
  spec.codec = params.get("codec").asString("h264");

  // 延时 Phase B：逐帧调色（色调曲线 + 颜色分级），零值近似恒等
  const json::JsonValue colorAdjust = params.get("colorAdjust");
  if (!colorAdjust.isNull()) {
    const json::JsonValue tc = colorAdjust.get("toneCurve");
    spec.toneCurve.shadows = tc.get("shadows").asDouble(0.0);
    spec.toneCurve.midtone = tc.get("midtone").asDouble(0.0);
    spec.toneCurve.highlight = tc.get("highlight").asDouble(0.0);
    const json::JsonValue cg = colorAdjust.get("colorGrade");
    spec.colorGrade.shadowHue = cg.get("shadowHue").asDouble(0.0);
    spec.colorGrade.shadowSat = cg.get("shadowSat").asDouble(0.0);
    spec.colorGrade.midtoneHue = cg.get("midtoneHue").asDouble(0.0);
    spec.colorGrade.midtoneSat = cg.get("midtoneSat").asDouble(0.0);
    spec.colorGrade.highlightHue = cg.get("highlightHue").asDouble(0.0);
    spec.colorGrade.highlightSat = cg.get("highlightSat").asDouble(0.0);
    spec.colorGrade.blend = cg.get("blend").asDouble(0.0);
    spec.colorGrade.balance = cg.get("balance").asDouble(0.0);
  }

  if (resolutionWidth(spec.resolution) == 0) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "params.resolution 必须是 1080p / 4K / 6K（当前: " + spec.resolution + "）");
    return json::JsonValue::nullValue();
  }
  if (spec.fps != 25 && spec.fps != 30 && spec.fps != 60) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "params.fps 只能是 25 / 30 / 60（当前: " + std::to_string(spec.fps) + "）");
    return json::JsonValue::nullValue();
  }
  if (spec.codec != "h264" && spec.codec != "h265" && spec.codec != "prores") {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "params.codec 只能是 h264 / h265 / prores（当前: " + spec.codec + "）");
    return json::JsonValue::nullValue();
  }

  // ProRes：v1.0.0 没有 ProRes 编码器 —— 协议 §9-Q3 要求 5001 + data.alternative，UI 置灰
  if (spec.codec == "prores") {
    json::JsonValue data = json::JsonValue::objectValue();
    data.setItem("codec", "prores");
    data.setItem("alternative", "h264");
    json::JsonValue supported = json::JsonValue::arrayValue();
    supported.addItem("h264");
    supported.addItem("h265");
    data.setItem("supported", supported);
    *err = rpc::RpcError(rpc::kCodecUnavailable,
                         "ProRes 编码不可用（v1.0.0 无 ProRes 编码器），请改用 H.264", data);
    return json::JsonValue::nullValue();
  }

  // ffmpeg 探测：不在就当场失败，绝不返回一个什么都不生成的 jobId
  std::string ffmpegErr;
  const std::string ffmpeg = detectFfmpeg(&ffmpegErr);
  if (ffmpeg.empty()) {
    json::JsonValue data = json::JsonValue::objectValue();
    data.setItem("reason", "ffmpeg-missing");
    data.setItem("codec", spec.codec);
    data.setItem("hint", "安装 ffmpeg 并加入 PATH，或用环境变量 LUMINA_FFMPEG 指定 ffmpeg.exe");
    *err = rpc::RpcError(rpc::kCodecUnavailable,
                         "未找到 ffmpeg.exe，无法编码视频（" +
                             (ffmpegErr.empty() ? std::string("PATH 中不存在") : ffmpegErr) + "）",
                         data);
    return json::JsonValue::nullValue();
  }
  if (!jobs) {
    *err = rpc::RpcError(rpc::kInternalError, "任务管理器未就绪");
    return json::JsonValue::nullValue();
  }

  SequenceInfo info;
  std::vector<SeqFrame> frames;
  std::string loadErr;
  const int rc = loadSequence(seqId, &info, &frames, &loadErr);
  if (rc == 1) {
    *err = rpc::RpcError(rpc::kFileNotFound, loadErr);
    return json::JsonValue::nullValue();
  }
  if (rc == 2) {
    *err = rpc::RpcError(rpc::kDbError, "读取序列失败: " + loadErr);
    return json::JsonValue::nullValue();
  }
  if (frames.empty()) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "延时序列 " + std::to_string(seqId) + " 没有任何帧，无法渲染");
    return json::JsonValue::nullValue();
  }

  setSequenceState(seqId, "rendering");
  const int64_t jobId = jobs->start("timelapse.render",
                                    [this, seqId, frames, spec](JobContext& ctx) {
                                      runRender(seqId, frames, spec, ctx);
                                    });
  trackJob(jobId, seqId, "rendering");
  logInfo("timelapse", "render job " + std::to_string(jobId) + " started for seq " +
                           std::to_string(seqId) + " " + spec.resolution + "/" +
                           std::to_string(spec.fps) + "fps/" + spec.codec + " ffmpeg=" + ffmpeg);
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("jobId", jobId);
  out.setItem("frames", static_cast<int64_t>(frames.size()));
  out.setItem("ffmpeg", ffmpeg);
  return out;
}

void TimelapseService::runRender(int64_t seqId, std::vector<SeqFrame> frames,
                                 const RenderSpec& spec, JobContext& ctx) {
  struct StateReset {
    TimelapseService* self;
    int64_t seqId;
    ~StateReset() { self->setSequenceState(seqId, "idle"); }
  } guard{this, seqId};

  const int total = static_cast<int>(frames.size());
  const int targetWidth = resolutionWidth(spec.resolution);
  const std::string cacheDir = cacheTimelapseDir();
  const std::string workDir =
      joinPath(cacheDir, "render-" + std::to_string(seqId) + "-" + std::to_string(ctx.jobId()));
  const std::string logPath = workDir + "/ffmpeg.log";
  if (!dirExists(workDir) && !makeDirs(workDir)) {
    throw std::runtime_error("渲染失败：无法创建临时目录 " + workDir);
  }

  // 坡度插值（渲染时按帧烧录 EV；温度在 v1.0.0 渲染里不做，见 warnings）
  std::vector<Keyframe> ramp;
  std::string rampNote;
  loadRamp(seqId, frames, &ramp, &rampNote);
  WarningList warnings;
  if (!rampNote.empty()) warnings.add(rampNote);
  if (!ramp.empty()) {
    bool hasTemperature = false;
    for (const Keyframe& k : ramp) {
      if (std::fabs(k.temperatureK - 5600.0) > 1.0) hasTemperature = true;
    }
    if (hasTemperature) {
      warnings.add("曝光坡度里的色温关键帧未在 v1.0.0 渲染中烧录（只烧录 EV），需要色温过渡请用 LRTimelapse 导出");
    }
  }

  // ---- 阶段 1（0..85%）：逐帧渲染成 JPEG 序列 ----
  std::vector<std::string> frameFiles;
  frameFiles.reserve(static_cast<size_t>(total));
  int rendered = 0;
  int failed = 0;
  std::string firstFailure;

  ctx.progress(0, "渲染准备：" + std::to_string(total) + " 帧 → " + spec.resolution);
  for (int i = 0; i < total; ++i) {
    if (ctx.isCancelled()) {
      // 取消：清掉半成品帧，不留垃圾
      for (const std::string& f : frameFiles) removeFile(f);
      json::JsonValue result = json::JsonValue::objectValue();
      result.setItem("seqId", seqId);
      result.setItem("cancelled", true);
      result.setItem("rendered", rendered);
      result.setItem("warnings", warnings.toJson());
      ctx.setResult(result);
      return;
    }
    const SeqFrame& frame = frames[static_cast<size_t>(i)];
    imaging::Image image;
    std::string source;
    std::string decodeErr;
    if (!decodeFrameForSize(frame, targetWidth, &image, &source, &decodeErr)) {
      ++failed;
      if (firstFailure.empty()) firstFailure = decodeErr;
      continue;
    }
    if (source == "embedded-preview-upscaled") {
      warnings.add("帧 " + std::to_string(frame.frameNo) +
                   " 只能用低分辨率内嵌预览放大输出（RAW 解码不可用）");
    }
    // sidecar 色调烧录（与导出口径一致：exposure/contrast/saturation）
    if (xmp_) {
      json::JsonValue paramsJson;
      std::string paramErr;
      if (xmp_->loadParams(frame.photoId, &paramsJson, &paramErr)) {
        imaging::ToneAdjust adjust;
        adjust.exposure = paramsJson.get("exposure").asDouble(0.0);
        adjust.contrast = paramsJson.get("contrast").asDouble(0.0);
        adjust.saturation = paramsJson.get("saturation").asDouble(0.0);
        // Holy Grail 坡度：把插值 EV 叠加到该帧（sidecar 的 exposure 已含去闪补偿，
        // 坡度只写在 lumina:ExposureRamp/lrt:，两者不重复计）
        adjust.exposure += rampEvAt(ramp, frame.frameNo);
        imaging::applyToneApprox(&image, adjust);
      } else {
        warnings.add("帧 " + std::to_string(frame.frameNo) + " sidecar 读取失败，按原片渲染: " +
                     paramErr);
      }
    }
    // 延时 Phase B：逐帧调色（色调曲线 → 颜色分级），在全分辨率像素上施加，
    // 不被后续 scaleToWidth 缩放模糊；与「曝光→对比→饱和」顺序一致（在 tone approx 之后）
    imaging::applyToneCurve(&image, spec.toneCurve);
    imaging::applyColorGrade(&image, spec.colorGrade);
    imaging::Image scaled;
    std::string scaleErr;
    if (!scaleToWidth(image, targetWidth, &scaled, &scaleErr)) {
      ++failed;
      if (firstFailure.empty()) firstFailure = "帧 " + std::to_string(frame.frameNo) + ": " + scaleErr;
      continue;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "f%06d.jpg", i + 1);
    const std::string framePath = joinPath(workDir, name);
    std::string encodeErr;
    if (!imaging::encodeJpeg(framePath, scaled, 92, &encodeErr)) {
      ++failed;
      if (firstFailure.empty()) {
        firstFailure = "帧 " + std::to_string(frame.frameNo) + " 编码失败: " + encodeErr;
      }
      continue;
    }
    frameFiles.push_back(framePath);
    ++rendered;
    const int percent = (i + 1) * 85 / total;
    ctx.progress(percent, "已渲染 " + std::to_string(rendered) + "/" + std::to_string(i + 1) + " 帧");
  }

  // 视频不允许缺帧：任何一帧失败都整单失败（否则产物时长/节奏会静默错位）
  if (failed > 0) {
    for (const std::string& f : frameFiles) removeFile(f);
    throw std::runtime_error("渲染失败：" + std::to_string(failed) + " 帧渲染失败（共 " +
                             std::to_string(total) + " 帧）。首个错误：" + firstFailure);
  }
  if (rendered == 0) {
    throw std::runtime_error("渲染失败：没有任何帧被成功渲染");
  }

  // ---- 阶段 2（85..100%）：ffmpeg 编码 ----
  const std::string outDir = config_ ? config_->rendersDir() : cacheDir;
  if (!dirExists(outDir) && !makeDirs(outDir)) {
    for (const std::string& f : frameFiles) removeFile(f);
    throw std::runtime_error("渲染失败：无法创建输出目录 " + outDir);
  }
  const std::string outPath = joinPath(
      outDir, "timelapse-" + std::to_string(seqId) + "-" + std::to_string(ctx.jobId()) + "-" +
                 spec.resolution + "-" + std::to_string(spec.fps) + "fps-" + spec.codec + ".mp4");

  std::string ffmpegErr;
  const std::string ffmpeg = detectFfmpeg(&ffmpegErr);
  if (ffmpeg.empty()) {
    for (const std::string& f : frameFiles) removeFile(f);
    throw std::runtime_error("渲染失败：编码阶段找不到 ffmpeg.exe（" +
                             (ffmpegErr.empty() ? std::string("PATH 中不存在") : ffmpegErr) + "）");
  }

  const char* encoder = ffmpegVideoEncoder(spec.codec);
  const std::string quality =
      spec.codec == "h265" ? std::string("-crf 20 -preset medium -tag:v hvc1")
                           : std::string("-crf 18 -preset medium");
  // 命令行拼装成 args（exe 由 runProcessToLog 负责加引号）
  const std::string args = "-y -hide_banner -loglevel error -framerate " +
                           std::to_string(spec.fps) + " -start_number 1 -i " +
                           quotePath(joinPath(workDir, "f%06d.jpg")) + " -c:v " + encoder +
                           " -pix_fmt yuv420p " + quality + " -movflags +faststart " +
                           quotePath(outPath);
  ctx.progress(85, "ffmpeg 编码中（" + std::string(encoder) + "）…");
  logInfo("timelapse", "render job " + std::to_string(ctx.jobId()) + " exec: " + quotePath(ffmpeg) +
                           " " + args);
  int exitCode = -1;
  std::string processErr;
  const bool started = runProcessToLog(ffmpeg, args, logPath, &exitCode, &processErr);
  if (!started || exitCode != 0) {
    const std::string logTail = tailOfFile(logPath, 240);
    for (const std::string& f : frameFiles) removeFile(f);
    throw std::runtime_error(
        std::string(started ? "ffmpeg 编码失败（退出码 " + std::to_string(exitCode) + "）"
                            : "ffmpeg 无法启动") +
        (logTail.empty() ? std::string("，日志见 ") + logPath : "：" + logTail));
  }
  if (!fileExists(outPath) || fileSize(outPath) <= 0) {
    throw std::runtime_error("ffmpeg 返回成功但产物不存在或为空: " + outPath + "（日志见 " + logPath +
                             "）");
  }
  ctx.progress(100, "渲染完成");

  // 清掉中间帧与成功日志（失败时保留日志用于排障）
  for (const std::string& f : frameFiles) removeFile(f);
  removeFile(logPath);

  const double framesPerSec = spec.fps > 0 ? static_cast<double>(rendered) / spec.fps : 0.0;
  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("seqId", seqId);
  result.setItem("path", outPath);
  result.setItem("frames", rendered);
  result.setItem("resolution", spec.resolution);
  result.setItem("width", targetWidth);
  result.setItem("fps", spec.fps);
  result.setItem("codec", spec.codec);
  result.setItem("durationSec", round3(framesPerSec));
  result.setItem("sizeBytes", fileSize(outPath));
  result.setItem("warnings", warnings.toJson());
  ctx.setResult(result);
  logInfo("timelapse", "render job " + std::to_string(ctx.jobId()) + " finished: " + outPath +
                           " (" + std::to_string(rendered) + " frames)");
}

// ============================== 5.11.8 getRenderJobs ==============================

json::JsonValue TimelapseService::getRenderJobs(const json::JsonValue& params, JobManager* jobs,
                                                rpc::RpcError* err) {
  (void)params;
  if (!jobs) {
    *err = rpc::RpcError(rpc::kInternalError, "任务管理器未就绪");
    return json::JsonValue::nullValue();
  }
  json::JsonValue out = json::JsonValue::arrayValue();
  for (const JobState& st : jobs->list(std::string())) {
    if (st.kind != "timelapse.render") continue;
    json::JsonValue item = st.toJson();
    if (st.state == "failed") {
      json::JsonValue e = json::JsonValue::objectValue();
      e.setItem("code", st.errorCode != 0 ? st.errorCode : rpc::kJobFailed);
      e.setItem("message", st.errorMessage);
      item.setItem("error", e);
    }
    if (st.state == "done" && !st.result.isNull()) item.setItem("result", st.result);
    out.addItem(item);
  }
  return out;
}

// ============================== sidecar 直写 / 读取 ==============================

bool TimelapseService::readSidecarText(const SeqFrame& frame, const std::string& prefix,
                                       const std::string& name, std::string* value, bool* found,
                                       std::string* err) {
  if (value) value->clear();
  if (found) *found = false;
  std::string sidecar = frame.sidecarPath;
  if (sidecar.empty() && xmp_ && !frame.filePath.empty()) {
    sidecar = xmp_->sidecarPathFor(frame.filePath);
  }
  if (sidecar.empty()) {
    if (err) *err = "帧 " + std::to_string(frame.frameNo) + " 没有 sidecar 路径";
    return false;
  }
  if (!fileExists(sidecar)) return true;  // 没有 sidecar = 字段不存在（不是错误）
  std::string bytes;
  std::string readErr;
  if (!readFile(sidecar, &bytes, &readErr)) {
    if (err) *err = "读取 sidecar 失败: " + readErr;
    return false;
  }
  std::string parseErr;
  std::unique_ptr<xmp::XmpDocument> doc = xmp::XmpDocument::parse(bytes, &parseErr);
  if (!doc) {
    if (err) *err = "sidecar 解析失败: " + sidecar + " (" + parseErr + ")";
    return false;
  }
  if (!doc->has(prefix, name)) return true;
  if (value) *value = doc->get(prefix, name);
  if (found) *found = true;
  return true;
}

bool TimelapseService::writeSidecarText(const SeqFrame& frame,
                                        const std::vector<SidecarField>& fields,
                                        std::string* err) {
  if (!photos_) {
    if (err) *err = "照片服务未就绪";
    return false;
  }
  if (fields.empty()) return true;
  // 与 XmpService::saveParams / setExifGps / XmpMigrator::migrateOne 共用同一把
  // per-photo 写锁，且**全程持有**（读-改-写整体在临界区内）。
  // 不加锁时：timelapse 写 ExposureRamp 与修图台写 crs: 并发 → 后写者用自己读到的
  // 旧文档覆盖前者，exactly 丢更新（此前 timelapse_service 是唯一漏锁的 sidecar 写方）。
  // 锁序：本函数调用的 photos_->setSidecar / xmp_->sidecarPathFor 都不取其他
  // per-photo 锁，且调用方（writeKeyframes/writeDeflickerMarker）未持有任何锁，
  // 因此不存在 ABBA 死锁。
  static std::mutex kNoXmpFallbackMutex;  // xmp_ 为空时退化为全局串行化（正常构造不会发生）
  std::shared_ptr<std::mutex> photoLock =
      xmp_ ? xmp_->photoWriteMutex(frame.photoId) : std::shared_ptr<std::mutex>();
  std::lock_guard<std::mutex> lock(photoLock ? *photoLock : kNoXmpFallbackMutex);
  std::string sidecar = frame.sidecarPath;
  if (sidecar.empty() && xmp_ && !frame.filePath.empty()) {
    sidecar = xmp_->sidecarPathFor(frame.filePath);
  }
  if (sidecar.empty()) {
    if (err) *err = "帧 " + std::to_string(frame.frameNo) + " 没有 sidecar 路径";
    return false;
  }

  std::unique_ptr<xmp::XmpDocument> doc;
  if (fileExists(sidecar)) {
    std::string bytes;
    std::string readErr;
    if (!readFile(sidecar, &bytes, &readErr)) {
      if (err) *err = "读取 sidecar 失败: " + readErr;
      return false;
    }
    std::string parseErr;
    doc = xmp::XmpDocument::parse(bytes, &parseErr);
    if (!doc) {
      // 红线：解析不了的 sidecar 一律不覆盖（可能是用户手改/第三方工具写的）
      if (err) *err = "sidecar 解析失败，拒绝写入: " + sidecar + " (" + parseErr + ")";
      return false;
    }
  } else {
    doc = std::unique_ptr<xmp::XmpDocument>(new xmp::XmpDocument(xmp::XmpDocument::createNew()));
  }

  for (const SidecarField& f : fields) {
    if (f.remove) {
      doc->remove(f.prefix, f.name);
    } else {
      doc->setText(f.prefix, f.name, f.value);
    }
  }
  const std::string output = doc->serialize();

  // 结构自检：回写前重新解析，确认字段真的写进去了（否则拒绝落盘）
  std::string checkParseErr;
  std::unique_ptr<xmp::XmpDocument> check = xmp::XmpDocument::parse(output, &checkParseErr);
  if (!check) {
    if (err) *err = "sidecar 结构自检失败，拒绝写入: " + checkParseErr;
    return false;
  }
  for (const SidecarField& f : fields) {
    if (f.remove) {
      if (check->has(f.prefix, f.name)) {
        if (err) *err = "sidecar 自检失败：字段未能删除 " + f.prefix + ":" + f.name;
        return false;
      }
      continue;
    }
    if (!check->has(f.prefix, f.name) || check->get(f.prefix, f.name) != f.value) {
      if (err) *err = "sidecar 自检字段不一致，拒绝写入: " + f.prefix + ":" + f.name;
      return false;
    }
  }

  std::string writeErr;
  if (!writeFileAtomic(sidecar, output, &writeErr)) {
    if (err) *err = "sidecar 落盘失败: " + writeErr;
    return false;
  }
  // 刷新 DB 里的 sidecar mtime，否则后续编辑会话会误判"外部修改"而拒写
  std::string dbErr;
  if (!photos_->setSidecar(frame.photoId, sidecar, fileMtimeMs(sidecar), true, &dbErr)) {
    logWarn("timelapse", "sidecar mtime refresh failed for photo " + std::to_string(frame.photoId) +
                             ": " + dbErr);
  }
  return true;
}

bool TimelapseService::writeDeflickerMarker(const SeqFrame& frame,
                                           const json::JsonValue& marker, std::string* err) {
  std::vector<SidecarField> fields;
  SidecarField f;
  f.prefix = "lumina";
  f.name = "Deflicker";
  f.value = marker.serialize();
  fields.push_back(f);
  return writeSidecarText(frame, fields, err);
}

}  // namespace timelapse
}  // namespace lumina
