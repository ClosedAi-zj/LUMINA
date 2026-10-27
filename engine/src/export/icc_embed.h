// M4-02 导出嵌入 ICC
//
// - JPEG：SOI 之后插入 APP2 段（"ICC_PROFILE\0" + 序号/总数 + profile 字节），
//         超长按 65519 字节切片，符合 ICC 在 JPEG 中的约定（ICC.1 Annex B.4）。
// - TIFF：写 tag 34675（0x8773, type UNDEFINED），偏移指向像素数据之后的 profile 块。
// - PSD ：写 image resource 0x040F（见 psd_writer）。
//
// 原则：嵌入失败即导出失败（不允许"标了 AdobeRGB 却没有 profile"的文件）。
#ifndef LUMINA_EXPORT_ICC_EMBED_H_
#define LUMINA_EXPORT_ICC_EMBED_H_

#include <cstdint>
#include <string>
#include <vector>

namespace lumina {
namespace icc_embed {

/** JPEG APP2 单段可承载的 profile 最大字节数（65535 − 2 长度 − 12 标识 − 2 序号/总数） */
constexpr size_t kJpegChunkMax = 65519;

/** 是否已含 ICC_PROFILE APP2 段（用于避免重复嵌入） */
bool jpegHasIcc(const std::string& path, bool* has, std::string* err);

/** 在 JPEG 的 SOI 之后插入 ICC（已含则返回 false + 明确原因，不静默重复写） */
bool injectJpegIcc(const std::string& path, const std::vector<uint8_t>& icc, std::string* err);

}  // namespace icc_embed
}  // namespace lumina

#endif  // LUMINA_EXPORT_ICC_EMBED_H_
