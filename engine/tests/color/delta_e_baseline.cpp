// M0 ΔE 基线测量（架构文档 679 行风险条款：T0.2 后拿现状数字，基线 >6 触发降级决策）
// 参考集：RW2 内嵌的相机 JPEG（LibRaw unpack_thumb，相机自家渲染 = Q9 参考集的可用代理，
//         S5M2 相机 JPEG 200 张固定清单到位后由 T4.5 替换为完整参考集）
// 被测：引擎现状管线（decodeToBitmap 8bit sRGB + tone approx）
// 指标：ΔE76（CIE Lab，D65）逐像素，输出 mean/median/p95 JSON
// 用法：deltae_baseline <dir-or-file.rw2>... ；仅测量，不写任何照片文件
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

// stb_image 实现在 lumina_core/imaging.cpp 中（此处仅声明，避免重复定义）
#include "stb_image.h"

#include "imaging/image_buffer.h"
#include "libraw/libraw.h"
#include "raw/raw_decoder.h"

using namespace lumina;

namespace {

struct Stats {
  double mean = 0, median = 0, p95 = 0;
};

void boxDownscale(const std::vector<double>& src, int sw, int sh, std::vector<double>& dst,
                  int dw, int dh) {
  dst.assign(static_cast<size_t>(dw) * dh * 3, 0.0);
  for (int y = 0; y < dh; ++y) {
    int y0 = static_cast<int>(static_cast<int64_t>(y) * sh / dh);
    int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * sh / dh));
    for (int x = 0; x < dw; ++x) {
      int x0 = static_cast<int>(static_cast<int64_t>(x) * sw / dw);
      int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * sw / dw));
      double acc[3] = {0, 0, 0};
      int n = 0;
      for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
          const double* p = &src[(static_cast<size_t>(yy) * sw + xx) * 3];
          acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
          ++n;
        }
      double* q = &dst[(static_cast<size_t>(y) * dw + x) * 3];
      q[0] = acc[0] / n; q[1] = acc[1] / n; q[2] = acc[2] / n;
    }
  }
}

double srgbToLinear(double c) {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

void srgbToLab(const double* rgb, double* lab) {
  double r = srgbToLinear(rgb[0] / 255.0), g = srgbToLinear(rgb[1] / 255.0),
         b = srgbToLinear(rgb[2] / 255.0);
  // sRGB(D65) → XYZ
  double X = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
  double Y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
  double Z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;
  const double Xn = 0.95047, Yn = 1.0, Zn = 1.08883;
  auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0; };
  double fx = f(X / Xn), fy = f(Y / Yn), fz = f(Z / Zn);
  lab[0] = 116.0 * fy - 16.0;
  lab[1] = 500.0 * (fx - fy);
  lab[2] = 200.0 * (fy - fz);
}

Stats compare(const std::vector<double>& a, const std::vector<double>& b) {
  std::vector<double> des;
  des.reserve(a.size() / 3);
  for (size_t i = 0; i + 2 < a.size(); i += 3) {
    double la[3], lb[3];
    srgbToLab(&a[i], la);
    srgbToLab(&b[i], lb);
    double d = std::sqrt((la[0] - lb[0]) * (la[0] - lb[0]) + (la[1] - lb[1]) * (la[1] - lb[1]) +
                         (la[2] - lb[2]) * (la[2] - lb[2]));
    des.push_back(d);
  }
  std::sort(des.begin(), des.end());
  Stats s;
  double sum = 0;
  for (double d : des) sum += d;
  s.mean = des.empty() ? 0 : sum / des.size();
  s.median = des.empty() ? 0 : des[des.size() / 2];
  s.p95 = des.empty() ? 0 : des[static_cast<size_t>(des.size() * 0.95)];
  return s;
}

std::vector<std::string> collectRw2(const std::vector<std::string>& args) {
  std::vector<std::string> out;
  for (const std::string& a : args) {
    DIR* d = opendir(a.c_str());
    if (d) {
      struct dirent* e;
      while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n.size() > 4 && (n.substr(n.size() - 4) == ".RW2" || n.substr(n.size() - 4) == ".rw2"))
          out.push_back(a + "/" + n);
      }
      closedir(d);
    } else {
      out.push_back(a);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: deltae_baseline <rw2-file-or-dir>...\n");
    return 2;
  }
  std::vector<std::string> files = collectRw2(std::vector<std::string>(argv + 1, argv + argc));
  if (files.empty()) {
    printf("{\"error\": \"no rw2 samples found\"}\n");
    return 1;
  }
  const int kSmall = 128;  // 比对分辨率（长边），足够统计稳定
  raw::RawDecoder decoder;
  printf("{\"pairs\": [\n");
  std::vector<Stats> all;
  bool first = true;
  for (const std::string& path : files) {
    // 1) 引擎现状管线（8bit sRGB）
    imaging::Image img8;
    std::string err;
    if (!decoder.decodeToBitmap(path, true, &img8, &err)) {
      fprintf(stderr, "skip (engine decode failed): %s: %s\n", path.c_str(), err.c_str());
      continue;
    }
    std::vector<double> eng(img8.pixels.size());
    for (size_t i = 0; i < img8.pixels.size(); ++i) eng[i] = img8.pixels[i];
    int ew = img8.width, eh = img8.height;

    // 2) 相机内嵌 JPEG 参考（LibRaw thumb）
    LibRaw proc;
    if (proc.open_file(path.c_str()) != LIBRAW_SUCCESS || proc.unpack_thumb() != LIBRAW_SUCCESS) {
      fprintf(stderr, "skip (no thumb): %s\n", path.c_str());
      continue;
    }
    int trc = 0;
    libraw_processed_image_t* thumb = proc.dcraw_make_mem_thumb(&trc);
    if (!thumb || thumb->type != LIBRAW_IMAGE_JPEG) {
      if (thumb) LibRaw::dcraw_clear_mem(thumb);
      fprintf(stderr, "skip (thumb not jpeg): %s\n", path.c_str());
      continue;
    }
    int tw, th, tc;
    unsigned char* j = stbi_load_from_memory(thumb->data, thumb->data_size, &tw, &th, &tc, 3);
    LibRaw::dcraw_clear_mem(thumb);
    if (!j) {
      fprintf(stderr, "skip (jpeg decode failed): %s\n", path.c_str());
      continue;
    }
    std::vector<double> cam(j, j + static_cast<size_t>(tw) * th * 3);
    stbi_image_free(j);

    // 3) 统一缩到 kSmall 长边再逐像素 ΔE76
    int dw, dh;
    if (ew >= eh) { dw = kSmall; dh = std::max(1, eh * kSmall / ew); }
    else { dh = kSmall; dw = std::max(1, ew * kSmall / eh); }
    std::vector<double> engS, camS;
    boxDownscale(eng, ew, eh, engS, dw, dh);
    boxDownscale(cam, tw, th, camS, dw, dh);
    Stats s = compare(engS, camS);
    all.push_back(s);
    printf("%s  {\"file\": \"%s\", \"engine\": \"%dx%d\", \"camJpeg\": \"%dx%d\", "
           "\"meanDE\": %.2f, \"medianDE\": %.2f, \"p95DE\": %.2f}",
           first ? "" : ",\n", path.c_str(), ew, eh, tw, th, s.mean, s.median, s.p95);
    first = false;
  }
  // 汇总
  Stats agg;
  if (!all.empty()) {
    std::vector<double> means, meds, p95s;
    for (const Stats& s : all) { means.push_back(s.mean); meds.push_back(s.median); p95s.push_back(s.p95); }
    auto avg = [](std::vector<double> v) { double s = 0; for (double x : v) s += x; return s / v.size(); };
    auto med = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    agg.mean = avg(means); agg.median = med(meds); agg.p95 = med(p95s);
  }
  printf("\n], \"summary\": {\"n\": %zu, \"meanDE\": %.2f, \"medianDE\": %.2f, \"p95DE\": %.2f,"
         " \"reference\": \"embedded-camera-jpeg\", \"metric\": \"dE76\"}}\n",
         all.size(), agg.mean, agg.median, agg.p95);
  return 0;
}
