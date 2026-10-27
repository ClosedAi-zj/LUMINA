// 方法注册实现：协议 §5 全量注册；Batch 5–8 领域返回明确错误（不静默）
#include "rpc/methods.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "color/icc.h"
#include "core/logger.h"
#include "core/md5.h"
#include "core/str_util.h"
#include "export/exporter.h"
#include "export/psd_reader.h"
#include "gallery/gallery_service.h"
#include "imaging/imaging.h"
#include "import/rename.h"
#include "library/folder_tree.h"
#include "map/map_service.h"
#include "metadata/exif_reader.h"
#include "metadata/video_probe.h"
#include "plan/render_exec.h"
#include "raw/embedded_proxy.h"
#include "rpc/version.h"
#include "specialty/denoise_service.h"
#include "specialty/hdr_service.h"
#include "timelapse/timelapse_service.h"
#include "xmp/xmp_mapper.h"

#include <tuple>

namespace lumina {
namespace rpc {
namespace {

// 未实现领域（Batch 5–8）的统一出口：显式错误，不得静默
RpcError notYet(const std::string& method, const std::string& batch) {
  json::JsonValue data = json::JsonValue::objectValue();
  data.setItem("method", method);
  data.setItem("plannedBatch", batch);
  return RpcError(kInternalError, "功能尚未实现（" + batch + "）: " + method, data);
}

std::vector<int64_t> idsOf(const json::JsonValue& params, const char* key) {
  std::vector<int64_t> out;
  const json::JsonValue& v = params.get(key);
  if (v.isArray()) {
    for (const auto& item : v.arrayItems()) out.push_back(item.asInt());
  } else if (v.isNumber()) {
    out.push_back(v.asInt());
  }
  return out;
}

// 等比缩放后的高度（长边不超过 targetWidth）
int thumbScaledHeight(int width, int height, int targetWidth) {
  if (width <= 0 || height <= 0) return targetWidth;
  int h = targetWidth * height / width;
  return h <= 0 ? targetWidth : h;
}

}  // namespace

void seedPresetData(EngineServices* s) {
  struct PresetSeed {
    const char* id;
    const char* name;
  };
  static const PresetSeed kSeeds[] = {
      {"sunset-slant", "余晖斜刺"},
      {"vast-mountain-mist", "苍穹·山岚"},
      {"deep-night", "深夜深色"},
      {"architecture-perspective", "建筑·透视"},
      {"sunrise-golden-peak", "日出金顶"},
      {"landscape-basic", "风光·基础"},
  };
  for (const PresetSeed& seed : kSeeds) {
    std::string sql = "INSERT INTO presets(name, params_json) VALUES(?,?) "
                      "ON CONFLICT(name) DO UPDATE SET params_json=excluded.params_json;";
    std::string err;
    auto stmt = s->db->prepareCached(std::string("seedPreset_") + seed.id, sql, &err);
    if (!stmt || !stmt->valid()) continue;
    stmt->bindText(1, seed.name);
    std::string name(seed.name);
    json::JsonValue params = xmp::XmpMapper::defaultParams();
    if (name == "余晖斜刺") {
      params.setItem("exposure", 0.35);
      params.setItem("contrast", 22.0);
      params.setItem("highlights", -35.0);
      params.setItem("shadows", 28.0);
      params.setItem("clarity", 18.0);
      params.setItem("dehaze", 12.0);
      params.setItem("temperature", 5900);
      params.setItem("tint", 8);
    } else if (name == "苍穹·山岚") {
      params.setItem("exposure", 0.15);
      params.setItem("contrast", 12.0);
      params.setItem("dehaze", 22.0);
      params.setItem("vibrance", 18.0);
      params.setItem("saturation", -6.0);
    } else if (name == "深夜深色") {
      params.setItem("exposure", -0.4);
      params.setItem("contrast", -8.0);
      params.setItem("blacks", 15.0);
      params.setItem("saturation", -18.0);
    } else if (name == "建筑·透视") {
      params.setItem("contrast", 30.0);
      params.setItem("clarity", 25.0);
      params.setItem("sharpenDetail", 30.0);
      params.setItem("hasCrop", true);
    } else if (name == "日出金顶") {
      params.setItem("exposure", 0.25);
      params.setItem("temperature", 6400);
      params.setItem("tint", 14);
      params.setItem("highlights", -20.0);
      params.setItem("vibrance", 25.0);
    } else {
      params.setItem("exposure", 0.0);
      params.setItem("contrast", 8.0);
      params.setItem("vibrance", 12.0);
    }
    stmt->bindText(2, params.serialize());
    stmt->step(nullptr);
    stmt->reset();
  }
}

void registerMethods(Dispatcher* d, EngineServices* s) {
  // ================= 5.0 系统与握手 =================
  d->registerMethod("system.initialize",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("protocolVersion", kProtocolVersion);
                      out.setItem("engineVersion", kEngineVersion);
                      json::JsonValue capabilities = json::JsonValue::objectValue();
                      capabilities.setItem("libraw", true);
                      capabilities.setItem("fts5", true);
                      capabilities.setItem("mock", true);
                      json::JsonValue codecs = json::JsonValue::arrayValue();
                      // 能力声明必须与实际可用集一致（M3 起 psd 带图层导出已落地）：
                      // jpeg / tiff / png / psd 全可用；psd 支持 8/16bit 与分层。
                      codecs.addItem("jpeg");
                      codecs.addItem("tiff");
                      codecs.addItem("png");
                      codecs.addItem("psd");
                      capabilities.setItem("codecs", codecs);
                      json::JsonValue colors = json::JsonValue::arrayValue();
                      // M4：ICC 色彩管理（lcms2）+ 导出嵌入 ICC + 软打样
                      colors.addItem("sRGB");
                      colors.addItem("AdobeRGB");
                      capabilities.setItem("colorSpaces", colors);
                      capabilities.setItem("icc", color::available());
                      capabilities.setItem("softProof", color::available());
                      capabilities.setItem("printPdf", true);
                      out.setItem("capabilities", capabilities);
                      out.setItem("homeDir", s->config->homeDir());
                      out.setItem("dbPath", s->config->dbPath());
                      return out;
                    });

  d->registerMethod("system.shutdown",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      (void)err;
                      if (s->requestShutdown) s->requestShutdown();
                      return json::JsonValue::objectValue();
                    });

  d->registerMethod("system.ping", [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
    (void)params;
    (void)err;
    json::JsonValue out = json::JsonValue::objectValue();
    out.setItem("pong", true);
    out.setItem("uptimeMs", s->server->uptimeMs());
    return out;
  });

  d->registerMethod("system.getSettings",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      (void)err;
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("settings", s->config->settings());
                      return out;
                    });

  d->registerMethod("system.setSettings",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      const json::JsonValue& patch = params.get("patch");
                      if (!patch.isObject()) {
                        *err = RpcError(kInvalidParams, "params.patch must be an object");
                        return json::JsonValue::nullValue();
                      }
                      std::string saveErr;
                      if (!s->config->mergePatch(patch, &saveErr)) {
                        *err = RpcError(kIoError, saveErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("settings", s->config->settings());
                      return out;
                    });

  d->registerMethod("system.listVolumes",
                    [](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      (void)err;
                      json::JsonValue out = json::JsonValue::arrayValue();
                      for (const VolumeInfo& v : listVolumes()) {
                        json::JsonValue item = json::JsonValue::objectValue();
                        item.setItem("path", v.path);
                        item.setItem("label", v.label);
                        item.setItem("totalBytes", static_cast<int64_t>(v.totalBytes));
                        item.setItem("freeBytes", static_cast<int64_t>(v.freeBytes));
                        item.setItem("isRemovable", v.isRemovable);
                        out.addItem(item);
                      }
                      return out;
                    });

  // ================= 5.1 图库索引与查询 =================
  d->registerMethod("library.getStats",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      return s->query->getStats(err);
                    });
  d->registerMethod("library.queryPhotos",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return s->query->queryPhotos(params, err);
                    });
  d->registerMethod("library.search",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return s->query->search(params, err);
                    });
  d->registerMethod("library.getFacets",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return s->query->getFacets(params, err);
                    });
  d->registerMethod("library.getTimeline",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return s->query->getTimeline(params, err);
                    });
  d->registerMethod("library.getFolderTree",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      json::JsonValue out = json::JsonValue::arrayValue();
                      std::string dbErr;
                      for (const FolderNode& node : library::buildFolderTree(s->photos, &dbErr)) {
                        out.addItem(folderNodeToJson(node));
                      }
                      return out;
                    });
  d->registerMethod("library.getPhotoDetail",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return s->query->getPhotoDetail(params, err);
                    });
  d->registerMethod("library.updatePhotos",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      json::JsonValue result = s->query->updatePhotos(params, err);
                      if (!err->ok()) return json::JsonValue::nullValue();
                      // sidecar 侧同步：rating/pick/keywords/description 写 exif:/lumina:
                      std::vector<int64_t> ids = idsOf(params, "ids");
                      const json::JsonValue& patch = params.get("patch");
                      int sidecarUpdated = 0;
                      for (int64_t id : ids) {
                        json::JsonValue current;
                        std::string loadErr;
                        if (!s->xmp->loadParams(id, &current, &loadErr)) continue;
                        bool changed = false;
                        if (patch.contains("rating")) {
                          current.setItem("rating", patch.get("rating").asInt());
                          changed = true;
                        }
                        if (patch.contains("pick")) {
                          current.setItem("pick", patch.get("pick").asString());
                          changed = true;
                        }
                        if (patch.contains("colorLabel")) {
                          current.setItem("colorLabel", patch.get("colorLabel").asString());
                          changed = true;
                        }
                        if (patch.contains("keywords")) {
                          json::JsonValue kw = json::JsonValue::arrayValue();
                          const json::JsonValue& src = patch.get("keywords");
                          if (src.isArray()) kw = src;
                          current.setItem("keywords", kw);
                          changed = true;
                        }
                        if (patch.contains("description")) {
                          current.setItem("description", patch.get("description").asString());
                          changed = true;
                        }
                        if (changed) {
                          std::string saveErr;
                          if (s->xmp->saveParams(id, current, &saveErr)) ++sidecarUpdated;
                          else logWarn("library", "sidecar sync skipped for " +
                                                   std::to_string(id) + ": " + saveErr);
                        }
                      }
                      result.setItem("sidecarUpdated", sidecarUpdated);
                      return result;
                    });
  d->registerMethod("library.removePhotos",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return s->query->removePhotos(params, err);
                    });
  d->registerMethod("library.restorePhotos",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return s->query->restorePhotos(params, err);
                    });
  d->registerMethod("library.listSmartCollections",
                    [](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      (void)err;
                      json::JsonValue out = json::JsonValue::arrayValue();
                      for (const SmartCollection& c : library::SmartCollections::all()) {
                        json::JsonValue item = json::JsonValue::objectValue();
                        item.setItem("id", c.id);
                        item.setItem("name", c.name);
                        item.setItem("filter", filterToJson(c.filter));
                        out.addItem(item);
                      }
                      return out;
                    });
  d->registerMethod("library.runSmartCollection",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::string id = params.get("id").asString();
                      SmartCollection collection;
                      if (!library::SmartCollections::find(id, &collection)) {
                        *err = RpcError(kInvalidParams, "unknown smart collection id: " + id);
                        return json::JsonValue::nullValue();
                      }
                      Page page;
                      pageFromJson(params.get("page"), &page);
                      QueryResult result;
                      std::string dbErr;
                      if (!s->photos->query(collection.filter, Sort(), page, &result, &dbErr)) {
                        *err = RpcError(kDbError, dbErr);
                        return json::JsonValue::nullValue();
                      }
                      return toQueryResult(result);
                    });

  // ================= 5.2 相册 / 精选集 / 画册 =================
  d->registerMethod("albums.list",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::string kind = params.get("kind").asString();
                      std::vector<AlbumRecord> list;
                      std::string dbErr;
                      if (!s->albums->list(kind, &list, &dbErr)) {
                        *err = RpcError(kDbError, dbErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::arrayValue();
                      for (const AlbumRecord& a : list) {
                        json::JsonValue item = json::JsonValue::objectValue();
                        item.setItem("id", a.id);
                        item.setItem("name", a.name);
                        item.setItem("kind", a.kind);
                        item.setItem("itemCount", a.itemCount);
                        item.setItem("coverPhotoId",
                                     a.hasCover ? json::JsonValue::integer(a.coverPhotoId)
                                                : json::JsonValue::nullValue());
                        if (a.hasPublishedAt) item.setItem("published", true);
                        out.addItem(item);
                      }
                      return out;
                    });
  // 画册成员清单（按 position 排序）——UI 的成员回显与拖拽重排依赖它，
  // 之前引擎缺这个方法，作品集页面只能显示提示文案（"待 albums.getItems 契约补齐"）。
  d->registerMethod("albums.getItems",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t albumId = params.get("albumId").asInt();
                      if (albumId <= 0) {
                        *err = RpcError(kInvalidParams, "params.albumId is required");
                        return json::JsonValue::nullValue();
                      }
                      std::vector<int64_t> ids;
                      std::string dbErr;
                      if (!s->albums->listItems(albumId, &ids, &dbErr)) {
                        *err = RpcError(kDbError, dbErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("albumId", albumId);
                      json::JsonValue items = json::JsonValue::arrayValue();
                      for (int64_t id : ids) items.addItem(json::JsonValue::integer(id));
                      out.setItem("items", items);
                      out.setItem("total", static_cast<int64_t>(ids.size()));
                      return out;
                    });
  d->registerMethod("albums.create",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::string name = params.get("name").asString();
                      if (name.empty()) {
                        *err = RpcError(kInvalidParams, "params.name is required");
                        return json::JsonValue::nullValue();
                      }
                      std::string kind = params.get("kind").asString("collection");
                      std::string dbErr;
                      int64_t id = s->albums->create(name, kind, &dbErr);
                      if (id == 0) {
                        *err = RpcError(kDbError, dbErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("id", id);
                      return out;
                    });
  d->registerMethod("albums.addPhotos",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t albumId = params.get("albumId").asInt();
                      std::vector<int64_t> ids = idsOf(params, "ids");
                      int added = 0;
                      std::string dbErr;
                      if (!s->albums->addPhotos(albumId, ids, &added, &dbErr)) {
                        *err = RpcError(kDbError, dbErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("added", added);
                      return out;
                    });
  d->registerMethod("albums.removePhotos",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t albumId = params.get("albumId").asInt();
                      std::vector<int64_t> ids = idsOf(params, "ids");
                      int removed = 0;
                      std::string dbErr;
                      if (!s->albums->removePhotos(albumId, ids, &removed, &dbErr)) {
                        *err = RpcError(kDbError, dbErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("removed", removed);
                      return out;
                    });
  d->registerMethod("albums.reorder",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t albumId = params.get("albumId").asInt();
                      std::vector<int64_t> ordered = idsOf(params, "orderedIds");
                      std::string dbErr;
                      if (!s->albums->reorder(albumId, ordered, &dbErr)) {
                        *err = RpcError(kDbError, dbErr);
                        return json::JsonValue::nullValue();
                      }
                      return json::JsonValue::objectValue();
                    });

  // ================= 5.3 缩略图与预览 =================
  d->registerMethod("thumb.ensureBatch",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::vector<int64_t> ids = idsOf(params, "ids");
                      int size = static_cast<int>(params.get("size").asInt(320));
                      return s->thumbs->ensureBatch(ids, size, err);
                    });
  d->registerMethod("thumb.getStatus",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::vector<int64_t> ids = idsOf(params, "ids");
                      int size = static_cast<int>(params.get("size").asInt(320));
                      return s->thumbs->getStatus(ids, size, err);
                    });
  d->registerMethod("preview.getBase",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      PhotoRecord p;
                      std::string dbErr;
                      if (!s->photos->getById(id, &p, &dbErr)) {
                        *err = RpcError(kFileNotFound, "photo not found: " + std::to_string(id));
                        return json::JsonValue::nullValue();
                      }
                      if (!fileExists(p.filePath)) {
                        // 离线原片：1001，UI 禁编辑导出
                        *err = RpcError(kFileNotFound, "original file missing: " + p.filePath);
                        return json::JsonValue::nullValue();
                      }
                      std::string target = joinPath(s->config->previewsDir(),
                                                    std::to_string(id) + ".jpg");
                      std::string source = "libraw";
                      std::string ext = extensionOf(p.filePath);
                      imaging::Image image;
                      if (raw::isSupportedExtension(ext) && ext != "jpg" && ext != "jpeg" &&
                          ext != "png") {
                        std::string decodeErr;
                        if (!s->decoder->decodeToBitmap(p.filePath, true, &image, &decodeErr)) {
                          *err = RpcError(kRawDecodeFailed, decodeErr);
                          return json::JsonValue::nullValue();
                        }
                      } else {
                        std::string decodeErr;
                        if (!imaging::decodeImageFile(p.filePath, &image, &decodeErr)) {
                          *err = RpcError(kRawDecodeFailed, decodeErr);
                          return json::JsonValue::nullValue();
                        }
                        source = "native";
                      }
                      if (image.width > 1600) {
                        imaging::Image scaled;
                        imaging::resizeImage(image, &scaled, 1600,
                                             thumbScaledHeight(image.width, image.height, 1600));
                        image = scaled;
                      }
                      std::string writeErr;
                      if (!imaging::encodeJpeg(target, image, 90, &writeErr)) {
                        *err = RpcError(kIoError, writeErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("path", target);
                      out.setItem("width", image.width);
                      out.setItem("height", image.height);
                      out.setItem("source", source);
                      return out;
                    });
  // ================= 5.4 develop.render（T1.5：Plan CPU 后端 / 导出同实现） =================
  // 入参：photoId（生产路径：解码原片）或 inputPath（断言/harness：指定位图文件，口径
  //       = 预览底图，保证与 GPU 预览逐像素同输入）；params = EditParams（缺省时读
  //       sidecar）；longEdge 缺省 1600（只缩不放，0 = 保持原尺寸）。
  // 出参：{ path, width, height }；PNG 营业目录 rendersDir/develop/，文件名含 kDisplay
  //       缓存键（同参数同图自动复用）。求值失败零静默（kInternalError + 原文）。
  d->registerMethod("develop.render",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      const std::string inputPath = params.get("inputPath").asString();
                      const int64_t id = params.get("photoId").asInt();
                      PhotoRecord p;
                      std::string file = inputPath;
                      if (file.empty()) {
                        std::string dbErr;
                        if (!s->photos->getById(id, &p, &dbErr)) {
                          *err = RpcError(kFileNotFound, "photo not found: " + std::to_string(id));
                          return json::JsonValue::nullValue();
                        }
                        if (!fileExists(p.filePath)) {
                          *err = RpcError(kFileNotFound, "original file missing: " + p.filePath);
                          return json::JsonValue::nullValue();
                        }
                        file = p.filePath;
                      }
                      int longEdge = static_cast<int>(params.get("longEdge").asInt(1600));
                      if (longEdge < 0 || longEdge > 20000) longEdge = 1600;

                      imaging::Image image;
                      std::string ext = extensionOf(file);
                      std::string decodeErr;
                      bool ok = false;
                      if (raw::isSupportedExtension(ext) && ext != "jpg" && ext != "jpeg" &&
                          ext != "png") {
                        ok = s->decoder->decodeToBitmap(file, true, &image, &decodeErr);
                      } else if (ext == "psd") {
                        // M3-03：PSD 合并图解码（外来/本方都先作为合并像素层呈现）
                        ok = lumina::psd::decodePsdComposite(file, &image, &decodeErr);
                      } else {
                        ok = imaging::decodeImageFile(file, &image, &decodeErr);
                      }
                      if (!ok) {
                        *err = RpcError(kRawDecodeFailed, decodeErr);
                        return json::JsonValue::nullValue();
                      }
                      if (longEdge > 0) {
                        const int nativeLong = std::max(image.width, image.height);
                        if (nativeLong > 0 && longEdge < nativeLong) {
                          imaging::Image scaled;
                          if (!imaging::resizeImage(
                                  image, &scaled, longEdge,
                                  thumbScaledHeight(image.width, image.height, longEdge))) {
                            *err = RpcError(kInternalError, "develop.render 缩放失败");
                            return json::JsonValue::nullValue();
                          }
                          image = scaled;
                        }
                      }

                      // 参数：显式 params 优先；否则读 sidecar（photoId 路径）
                      json::JsonValue editParams = params.get("params");
                      if (!editParams.isObject()) {
                        if (id <= 0) {
                          *err = RpcError(kInvalidParams, "develop.render 需要 params 或 photoId");
                          return json::JsonValue::nullValue();
                        }
                        std::string loadErr;
                        if (!s->xmp->loadParams(id, &editParams, &loadErr)) {
                          *err = RpcError(kInternalError, "sidecar 参数读取失败: " + loadErr);
                          return json::JsonValue::nullValue();
                        }
                      }

                      imaging::ImageBuf buf = plan::displayToPlanBuf(image);
                      plan::RenderPlan renderPlan;
                      std::string planErr;
                      if (!plan::runEditPlan(editParams, &buf, &renderPlan, &planErr)) {
                        *err = RpcError(kInternalError, "Plan 求值失败: " + planErr);
                        return json::JsonValue::nullValue();
                      }
                      imaging::Image outImage;
                      if (!plan::planBufToDisplay(buf, &outImage, &planErr)) {
                        *err = RpcError(kInternalError, "Plan 输出编码失败: " + planErr);
                        return json::JsonValue::nullValue();
                      }

                      const std::string outDir =
                          joinPath(s->config->rendersDir(), "develop");
                      if (!dirExists(outDir)) makeDirs(outDir);
                      // 文件名 = kDisplay 缓存键（含上游 kTone 键；同图同参自动复用）
                      const std::string target =
                          joinPath(outDir, "develop-" + renderPlan.cacheKeys[2] + ".png");
                      std::string writeErr;
                      if (!imaging::encodePng(target, outImage, &writeErr)) {
                        *err = RpcError(kIoError, writeErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("path", target);
                      out.setItem("width", outImage.width);
                      out.setItem("height", outImage.height);
                      return out;
                    });
  // ================= 5.4b softproof.preview（M4-03：软打样预览 + ΔE 报告） =================
  // 入参：photoId 或 inputPath、params（缺省读 sidecar）、longEdge（默认 1200）、
  //       targetProfile（ICC 文件路径，空 = 内置 AdobeRGB 作为演示目标）、
  //       intent（0..3，默认 1 相对色度）、simulatePaper（默认 true）。
  // 出参：{ path, width, height, target, deltaE:{mean,median,p95,max}, samples }。
  // 说明：ΔE00 = 打样后 vs 打样前（同一 sRGB Lab 口径），用于"这一版打样把画面推了多远"
  //       的量化；PRD 要求的"与 Photoshop 软打样比对"需外部参考，不在本 op 内冒充。
  d->registerMethod("softproof.preview",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      const std::string inputPath = params.get("inputPath").asString();
                      const int64_t id = params.get("photoId").asInt();
                      PhotoRecord p;
                      std::string file = inputPath;
                      if (file.empty()) {
                        std::string dbErr;
                        if (!s->photos->getById(id, &p, &dbErr)) {
                          *err = RpcError(kFileNotFound, "photo not found: " + std::to_string(id));
                          return json::JsonValue::nullValue();
                        }
                        if (!fileExists(p.filePath)) {
                          *err = RpcError(kFileNotFound, "original file missing: " + p.filePath);
                          return json::JsonValue::nullValue();
                        }
                        file = p.filePath;
                      }
                      int longEdge = static_cast<int>(params.get("longEdge").asInt(1200));
                      if (longEdge < 64 || longEdge > 8000) longEdge = 1200;

                      imaging::Image image;
                      const std::string ext = extensionOf(file);
                      std::string decodeErr;
                      bool ok = false;
                      if (raw::isSupportedExtension(ext) && ext != "jpg" && ext != "jpeg" &&
                          ext != "png") {
                        ok = s->decoder->decodeToBitmap(file, true, &image, &decodeErr);
                      } else if (ext == "psd") {
                        ok = lumina::psd::decodePsdComposite(file, &image, &decodeErr);
                      } else {
                        ok = imaging::decodeImageFile(file, &image, &decodeErr);
                      }
                      if (!ok) {
                        *err = RpcError(kRawDecodeFailed, decodeErr);
                        return json::JsonValue::nullValue();
                      }
                      const int nativeLong = std::max(image.width, image.height);
                      if (nativeLong > longEdge) {
                        imaging::Image scaled;
                        if (!imaging::resizeImage(
                                image, &scaled, longEdge,
                                thumbScaledHeight(image.width, image.height, longEdge))) {
                          *err = RpcError(kInternalError, "softproof.preview 缩放失败");
                          return json::JsonValue::nullValue();
                        }
                        image = scaled;
                      }

                      json::JsonValue editParams = params.get("params");
                      if (!editParams.isObject()) {
                        if (inputPath.empty() && id > 0) {
                          std::string loadErr;
                          if (!s->xmp->loadParams(id, &editParams, &loadErr)) {
                            *err = RpcError(kInternalError, "sidecar 参数读取失败: " + loadErr);
                            return json::JsonValue::nullValue();
                          }
                        } else {
                          editParams = json::JsonValue::objectValue();
                        }
                      }

                      imaging::ImageBuf buf = plan::displayToPlanBuf(image);
                      plan::RenderPlan renderPlan;
                      std::string planErr;
                      if (!plan::runEditPlan(editParams, &buf, &renderPlan, &planErr)) {
                        *err = RpcError(kInternalError, "Plan 求值失败: " + planErr);
                        return json::JsonValue::nullValue();
                      }
                      imaging::Image before;
                      if (!plan::planBufToDisplay(buf, &before, &planErr)) {
                        *err = RpcError(kInternalError, "Plan 输出编码失败: " + planErr);
                        return json::JsonValue::nullValue();
                      }

                      color::SoftProofOptions opts;
                      opts.targetProfilePath = params.get("targetProfile").asString();
                      opts.intent = static_cast<int>(params.get("intent").asInt(1));
                      if (opts.intent < 0 || opts.intent > 3) opts.intent = 1;
                      opts.proofingIntent = opts.intent;
                      opts.simulatePaper = params.get("simulatePaper").asBool(true);

                      std::vector<uint8_t> proofed;
                      if (!color::softProofRgb8(before.pixels.data(), before.width, before.height,
                                                opts, &proofed, &planErr)) {
                        *err = RpcError(kInternalError, "软打样转换失败: " + planErr);
                        return json::JsonValue::nullValue();
                      }
                      imaging::Image after = before;
                      after.pixels.swap(proofed);

                      // ---- ΔE00：打样前 vs 打样后（sRGB Lab，绝对意图） ----
                      std::vector<double> labA, labB;
                      if (!color::srgb8ToLab(before.pixels.data(), before.width, before.height,
                                             &labA, &planErr) ||
                          !color::srgb8ToLab(after.pixels.data(), after.width, after.height, &labB,
                                             &planErr)) {
                        *err = RpcError(kInternalError, "ΔE 计算失败: " + planErr);
                        return json::JsonValue::nullValue();
                      }
                      std::vector<double> des;
                      des.reserve(static_cast<size_t>(before.width) * before.height);
                      for (size_t i = 0; i < des.capacity(); ++i) {
                        des.push_back(color::deltaE00(&labA[i * 3], &labB[i * 3]));
                      }
                      std::sort(des.begin(), des.end());
                      double sum = 0.0;
                      for (double d : des) sum += d;

                      const std::string outDir = joinPath(s->config->rendersDir(), "softproof");
                      if (!dirExists(outDir)) makeDirs(outDir);
                      uint64_t h = 1469598103934665603ull;  // FNV-1a，确定性命名
                      const std::string key = renderPlan.cacheKeys[2] + "|" +
                                              opts.targetProfilePath + "|" +
                                              std::to_string(opts.intent) +
                                              (opts.simulatePaper ? "|paper" : "|nopaper");
                      for (unsigned char c : key) {
                        h ^= c;
                        h *= 1099511628211ull;
                      }
                      const std::string target =
                          joinPath(outDir, "softproof-" + std::to_string(h) + ".png");
                      std::string writeErr;
                      if (!imaging::encodePng(target, after, &writeErr)) {
                        *err = RpcError(kIoError, writeErr);
                        return json::JsonValue::nullValue();
                      }

                      json::JsonValue de = json::JsonValue::objectValue();
                      de.setItem("mean", des.empty() ? 0.0 : sum / static_cast<double>(des.size()));
                      de.setItem("median", des.empty() ? 0.0 : des[des.size() / 2]);
                      de.setItem("p95",
                                 des.empty() ? 0.0 : des[static_cast<size_t>(des.size() * 95 / 100)]);
                      de.setItem("max", des.empty() ? 0.0 : des.back());

                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("path", target);
                      out.setItem("width", after.width);
                      out.setItem("height", after.height);
                      out.setItem("target", opts.targetProfilePath.empty()
                                                 ? std::string(color::profileName(opts.builtinTarget))
                                                 : opts.targetProfilePath);
                      out.setItem("simulatePaper", opts.simulatePaper);
                      out.setItem("deltaE", de);
                      out.setItem("samples", static_cast<int64_t>(des.size()));
                      return out;
                    });
  d->registerMethod("cache.clear",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::string scope = params.get("scope").asString("thumbs");
                      std::string dir = s->config->thumbsDir();
                      if (scope == "previews") dir = s->config->previewsDir();
                      else if (scope == "renders") dir = s->config->rendersDir();
                      else if (scope != "thumbs") {
                        *err = RpcError(kInvalidParams, "params.scope must be thumbs|previews|renders");
                        return json::JsonValue::nullValue();
                      }
                      int64_t freed = 0;
                      std::vector<DirEntry> entries;
                      std::string listErr;
                      if (listDir(dir, &entries, &listErr)) {
                        for (const DirEntry& e : entries) {
                          if (e.isDir) continue;
                          freed += e.size;
                          removeFile(joinPath(dir, e.name));
                        }
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("freedBytes", freed);
                      return out;
                    });

  // ================= 5.4 XMP 读写 =================
  d->registerMethod("xmp.read",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      return s->xmp->read(id, err);
                    });
  d->registerMethod("xmp.write",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      return s->xmp->write(id, params, err);
                    });
  d->registerMethod("xmp.checkExternal",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      return s->xmp->checkExternal(id, err);
                    });
  d->registerMethod("xmp.listNamespaces",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      return s->xmp->listNamespaces(id, err);
                    });
  d->registerMethod("xmp.migrationStatus",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      return s->migrator->status(err);
                    });

  // ================= 5.5 编辑会话 =================
  d->registerMethod("edit.beginSession",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      return s->sessions->begin(id, err);
                    });
  d->registerMethod("edit.setParams",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t sessionId = params.get("sessionId").asInt();
                      bool commit = params.contains("commitHistory")
                                        ? params.get("commitHistory").asBool(true)
                                        : true;
                      return s->sessions->setParams(sessionId, params, commit, err);
                    });
  d->registerMethod("edit.undo",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t sessionId = params.get("sessionId").asInt();
                      return s->sessions->undo(sessionId, err);
                    });
  d->registerMethod("edit.redo",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t sessionId = params.get("sessionId").asInt();
                      return s->sessions->redo(sessionId, err);
                    });
  d->registerMethod("edit.endSession",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t sessionId = params.get("sessionId").asInt();
                      bool flush = params.get("flush").asBool(true);
                      return s->sessions->end(sessionId, flush, err);
                    });
  d->registerMethod("edit.getSnapshots",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      return s->sessions->snapshots(id, err);
                    });
  d->registerMethod("edit.createSnapshot",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      std::string name = params.get("name").asString("snapshot");
                      int64_t snapshotId = s->sessions->createSnapshot(id, name, err);
                      if (snapshotId == 0) return json::JsonValue::nullValue();
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("snapshotId", snapshotId);
                      return out;
                    });
  d->registerMethod("edit.restoreSnapshot",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      int64_t snapshotId = params.get("snapshotId").asInt();
                      json::JsonValue params2 = s->sessions->restoreSnapshot(id, snapshotId, err);
                      if (!err->ok()) return json::JsonValue::nullValue();
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("params", params2);
                      return out;
                    });
  d->registerMethod("vcopy.create",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      std::string name = params.get("name").asString();
                      return s->vcopy->create(id, name, err);
                    });
  d->registerMethod("vcopy.list",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      return s->vcopy->list(id, err);
                    });
  d->registerMethod("presets.list",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      std::string sql = "SELECT id, name, params_json FROM presets ORDER BY id;";
                      std::string sqlErr;
                      auto stmt = s->db->prepareCached("presetList", sql, &sqlErr);
                      if (!stmt || !stmt->valid()) {
                        *err = RpcError(kDbError, sqlErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::arrayValue();
                      while (stmt->step(&sqlErr)) {
                        json::JsonValue item = json::JsonValue::objectValue();
                        item.setItem("id", std::to_string(stmt->columnInt64(0)));
                        item.setItem("name", stmt->columnText(1));
                        json::JsonValue params2;
                        std::string parseErr;
                        if (json::JsonValue::parse(stmt->columnText(2), &params2, &parseErr)) {
                          item.setItem("params", params2);
                        } else {
                          item.setItem("params", json::JsonValue::objectValue());
                        }
                        out.addItem(item);
                      }
                      stmt->reset();
                      return out;
                    });
  d->registerMethod("presets.apply",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::string presetId = params.get("presetId").asString();
                      std::vector<int64_t> ids = idsOf(params, "targetIds");
                      std::string sql = "SELECT params_json FROM presets WHERE id=? OR name=?;";
                      std::string sqlErr;
                      auto stmt = s->db->prepareCached("presetGet", sql, &sqlErr);
                      if (!stmt || !stmt->valid()) {
                        *err = RpcError(kDbError, sqlErr);
                        return json::JsonValue::nullValue();
                      }
                      stmt->bindInt64(1, atoll(presetId.c_str()));
                      stmt->bindText(2, presetId);
                      if (!stmt->step(&sqlErr)) {
                        *err = RpcError(kInvalidParams, "preset not found: " + presetId);
                        stmt->reset();
                        return json::JsonValue::nullValue();
                      }
                      std::string text = stmt->columnText(0);
                      stmt->reset();
                      json::JsonValue presetParams;
                      std::string parseErr;
                      if (!json::JsonValue::parse(text, &presetParams, &parseErr)) {
                        *err = RpcError(kInternalError, "preset corrupt: " + parseErr);
                        return json::JsonValue::nullValue();
                      }
                      int applied = 0;
                      for (int64_t id : ids) {
                        std::string saveErr;
                        if (s->xmp->saveParams(id, presetParams, &saveErr)) ++applied;
                        else logWarn("presets", "apply skipped for " + std::to_string(id) + ": " +
                                                     saveErr);
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("applied", applied);
                      return out;
                    });

  // ================= 5.6 参数同步 / 序列 =================
  d->registerMethod("edit.syncToSelection",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t sourceId = params.get("sourceId").asInt();
                      std::vector<int64_t> targets = idsOf(params, "targetIds");
                      bool includeMasks = params.get("includeMasks").asBool(true);
                      json::JsonValue source;
                      std::string loadErr;
                      if (!s->xmp->loadParams(sourceId, &source, &loadErr)) {
                        *err = RpcError(kXmpParseError, loadErr);
                        return json::JsonValue::nullValue();
                      }
                      if (!includeMasks) source.eraseKey("masks");
                      int updated = 0;
                      for (int64_t id : targets) {
                        std::string saveErr;
                        if (s->xmp->saveParams(id, source, &saveErr)) ++updated;
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("updated", updated);
                      return out;
                    });
  d->registerMethod("edit.syncToSequence",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t sourceId = params.get("sourceId").asInt();
                      int64_t seqId = params.get("seqId").asInt();
                      bool includeMasks = params.get("includeMasks").asBool(true);
                      json::JsonValue source;
                      std::string loadErr;
                      if (!s->xmp->loadParams(sourceId, &source, &loadErr)) {
                        *err = RpcError(kXmpParseError, loadErr);
                        return json::JsonValue::nullValue();
                      }
                      if (!includeMasks) source.eraseKey("masks");
                      std::string sql = "SELECT photo_id FROM sequence_frames WHERE seq_id=?;";
                      std::string sqlErr;
                      auto stmt = s->db->prepareCached("syncSeqFrames", sql, &sqlErr);
                      if (!stmt || !stmt->valid()) {
                        *err = RpcError(kDbError, sqlErr);
                        return json::JsonValue::nullValue();
                      }
                      stmt->bindInt64(1, seqId);
                      int updated = 0;
                      while (stmt->step(&sqlErr)) {
                        int64_t id = stmt->columnInt64(0);
                        std::string saveErr;
                        if (s->xmp->saveParams(id, source, &saveErr)) ++updated;
                      }
                      stmt->reset();
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("updated", updated);
                      return out;
                    });

  // ================= 5.7 RAW 解码与渲染 =================
  // L1 判定（ipc-protocol v1.0.1）：抽样走 LibRaw 真解码，100% 成功且均值 ∈ [16,240]
  d->registerMethod("raw.selfTest",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int sample = params.contains("sample") ? params.get("sample").asInt(32) : 32;
                      if (sample <= 0) sample = 32;
                      std::vector<int64_t> ids;
                      const json::JsonValue& idsJson = params.get("ids");
                      if (idsJson.isArray()) {
                        for (const auto& v : idsJson.arrayItems()) ids.push_back(v.asInt());
                      }
                      raw::SelfTestResult result;
                      std::string selfErr;
                      if (!s->decoder->selfTest(s->photos, sample, ids, &result, &selfErr)) {
                        *err = RpcError(kInternalError, selfErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("ok", result.ok);
                      out.setItem("sampled", result.sampled);
                      out.setItem("succeeded", result.succeeded);
                      out.setItem("decodeMsP50", result.decodeMsP50);
                      out.setItem("decodeMsP95", result.decodeMsP95);
                      json::JsonValue failures = json::JsonValue::arrayValue();
                      for (const raw::SelfTestFailure& f : result.failures) {
                        json::JsonValue item = json::JsonValue::objectValue();
                        item.setItem("photoId", f.photoId);
                        item.setItem("error", f.error);
                        failures.addItem(item);
                      }
                      out.setItem("failures", failures);
                      return out;
                    });
  d->registerMethod("raw.decode",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      std::string scale = params.get("scale").asString("full");
                      PhotoRecord p;
                      std::string dbErr;
                      if (!s->photos->getById(id, &p, &dbErr)) {
                        *err = RpcError(kFileNotFound, "photo not found: " + std::to_string(id));
                        return json::JsonValue::nullValue();
                      }
                      if (!fileExists(p.filePath)) {
                        *err = RpcError(kFileNotFound, "original file missing: " + p.filePath);
                        return json::JsonValue::nullValue();
                      }
                      std::string target =
                          joinPath(s->config->rendersDir(),
                                   std::to_string(id) + "-" + scale + ".jpg");
                      std::string decodeErr;
                      if (!s->decoder->decodeToJpeg(p.filePath, target, scale == "half", 92,
                                                    &decodeErr)) {
                        *err = RpcError(kRawDecodeFailed, decodeErr);
                        return json::JsonValue::nullValue();
                      }
                      imaging::Image probe;
                      std::string probeErr;
                      imaging::decodeImageFile(target, &probe, &probeErr);
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("path", target);
                      out.setItem("width", probe.width);
                      out.setItem("height", probe.height);
                      return out;
                    });
  d->registerMethod("raw.getProxy",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      PhotoRecord p;
                      std::string dbErr;
                      if (!s->photos->getById(id, &p, &dbErr)) {
                        *err = RpcError(kFileNotFound, "photo not found: " + std::to_string(id));
                        return json::JsonValue::nullValue();
                      }
                      if (!fileExists(p.filePath)) {
                        *err = RpcError(kFileNotFound, "original file missing: " + p.filePath);
                        return json::JsonValue::nullValue();
                      }
                      std::string target = joinPath(s->config->previewsDir(),
                                                    std::to_string(id) + "-proxy.jpg");
                      raw::EmbeddedProxy proxy;
                      std::string extractErr;
                      if (!raw::extractEmbeddedJpeg(p.filePath, target, &proxy, &extractErr)) {
                        *err = RpcError(kProxyMissing, extractErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("path", target);
                      out.setItem("width", proxy.width);
                      out.setItem("height", proxy.height);
                      return out;
                    });
  d->registerMethod("analysis.histogram",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      bool edited = params.get("edited").asBool(false);
                      PhotoRecord p;
                      std::string dbErr;
                      if (!s->photos->getById(id, &p, &dbErr)) {
                        *err = RpcError(kFileNotFound, "photo not found: " + std::to_string(id));
                        return json::JsonValue::nullValue();
                      }
                      if (!fileExists(p.filePath)) {
                        *err = RpcError(kFileNotFound, "original file missing: " + p.filePath);
                        return json::JsonValue::nullValue();
                      }
                      imaging::Image image;
                      std::string ext = extensionOf(p.filePath);
                      std::string decodeErr;
                      bool ok = false;
                      if (raw::isSupportedExtension(ext) && ext != "jpg" && ext != "jpeg" &&
                          ext != "png") {
                        ok = s->decoder->decodeToBitmap(p.filePath, true, &image, &decodeErr);
                      } else {
                        ok = imaging::decodeImageFile(p.filePath, &image, &decodeErr);
                      }
                      if (!ok) {
                        *err = RpcError(kRawDecodeFailed, decodeErr);
                        return json::JsonValue::nullValue();
                      }
                      if (edited) {
                        // T1.5：edited 直方图与导出/预览同一 Plan 实现（旧 applyToneApprox 已销毁）
                        json::JsonValue params2;
                        std::string loadErr;
                        if (s->xmp->loadParams(id, &params2, &loadErr)) {
                          imaging::ImageBuf buf = plan::displayToPlanBuf(image);
                          std::string planErr;
                          if (!plan::runEditPlan(params2, &buf, nullptr, &planErr)) {
                            logWarn("analysis", "histogram Plan 求值失败，按原片直方图: " + planErr);
                          } else {
                            imaging::Image rendered;
                            if (plan::planBufToDisplay(buf, &rendered, &planErr)) {
                              image = std::move(rendered);
                            } else {
                              logWarn("analysis", "histogram 输出编码失败，按原片直方图: " + planErr);
                            }
                          }
                        }
                      }
                      int64_t r[256], g[256], b[256];
                      imaging::computeHistogram(image, r, g, b);
                      auto toArray = [](const int64_t* v) {
                        json::JsonValue out = json::JsonValue::arrayValue();
                        for (int i = 0; i < 256; ++i) out.addItem(v[i]);
                        return out;
                      };
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("r", toArray(r));
                      out.setItem("g", toArray(g));
                      out.setItem("b", toArray(b));
                      return out;
                    });
  d->registerMethod("analysis.getExposedRange",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t id = params.get("id").asInt();
                      PhotoRecord p;
                      std::string dbErr;
                      if (!s->photos->getById(id, &p, &dbErr)) {
                        *err = RpcError(kFileNotFound, "photo not found: " + std::to_string(id));
                        return json::JsonValue::nullValue();
                      }
                      if (!fileExists(p.filePath)) {
                        *err = RpcError(kFileNotFound, "original file missing: " + p.filePath);
                        return json::JsonValue::nullValue();
                      }
                      imaging::Image image;
                      std::string ext = extensionOf(p.filePath);
                      std::string decodeErr;
                      bool ok = false;
                      if (raw::isSupportedExtension(ext) && ext != "jpg" && ext != "jpeg" &&
                          ext != "png") {
                        ok = s->decoder->decodeToBitmap(p.filePath, true, &image, &decodeErr);
                      } else {
                        ok = imaging::decodeImageFile(p.filePath, &image, &decodeErr);
                      }
                      if (!ok) {
                        *err = RpcError(kRawDecodeFailed, decodeErr);
                        return json::JsonValue::nullValue();
                      }
                      size_t total = image.pixels.size() / 3;
                      int64_t under = 0, over = 0;
                      for (size_t i = 0; i < image.pixels.size(); i += 3) {
                        int lum = (image.pixels[i] * 299 + image.pixels[i + 1] * 587 +
                                   image.pixels[i + 2] * 114) / 1000;
                        if (lum < 8) ++under;
                        else if (lum > 247) ++over;
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("underexposedPct",
                                  total ? static_cast<double>(under) * 100.0 / total : 0.0);
                      out.setItem("overexposedPct",
                                  total ? static_cast<double>(over) * 100.0 / total : 0.0);
                      return out;
                    });

  // ================= 5.8 导入与去重 =================
  d->registerMethod("import.scanSource",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      const json::JsonValue& source = params.get("source");
                      std::string kind = source.get("kind").asString("folder");
                      std::string path = source.get("path").asString();
                      if (path.empty()) {
                        *err = RpcError(kInvalidParams, "params.source.path is required");
                        return json::JsonValue::nullValue();
                      }
                      if (kind == "volume" && !dirExists(path)) {
                        *err = RpcError(kSourceUnreadable, "volume not available: " + path);
                        return json::JsonValue::nullValue();
                      }
                      if (!dirExists(path)) {
                        *err = RpcError(kFileNotFound, "source folder not found: " + path);
                        return json::JsonValue::nullValue();
                      }
                      int64_t jobId = s->jobs->start("import.scan",
                                                     [s, path](JobContext& ctx) {
                                                       import::ScanResult scan;
                                                       std::string scanErr;
                                                       bool ok = import::ImportScanner::scanFolder(
                                                           path, &scan,
                                                           [&ctx](int percent, const std::string& detail) {
                                                             ctx.progress(percent, detail);
                                                           },
                                                           [&ctx]() { return ctx.isCancelled(); },
                                                           &scanErr);
                                                       if (!ok) return;
                                                       json::JsonValue byFormat =
                                                           json::JsonValue::objectValue();
                                                       for (const auto& kv : scan.byFormat) {
                                                         byFormat.setItem(kv.first, kv.second);
                                                       }
                                                       json::JsonValue unreadable =
                                                           json::JsonValue::arrayValue();
                                                       for (const auto& u : scan.unreadable) {
                                                         json::JsonValue item =
                                                             json::JsonValue::objectValue();
                                                         item.setItem("path", u.first);
                                                         item.setItem("error", u.second);
                                                         unreadable.addItem(item);
                                                       }
                                                       json::JsonValue result =
                                                           json::JsonValue::objectValue();
                                                       result.setItem("totalFiles", scan.totalFiles);
                                                       result.setItem("totalBytes", scan.totalBytes);
                                                       result.setItem("byFormat", byFormat);
                                                       result.setItem("duplicates", scan.duplicates);
                                                       result.setItem("rawWithJpg", scan.rawWithJpg);
                                                       result.setItem("jpgOnly", scan.jpgOnly);
                                                       result.setItem("unreadable", unreadable);
                                                       ctx.setResult(result);
                                                       ctx.progress(100, "scan done");
                                                     });
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("jobId", jobId);
                      return out;
                    });
  d->registerMethod("import.checkTarget",
                    [](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::string targetPath = params.get("targetPath").asString();
                      int64_t required = params.get("requiredBytes").asInt();
                      if (targetPath.empty()) {
                        *err = RpcError(kInvalidParams, "params.targetPath is required");
                        return json::JsonValue::nullValue();
                      }
                      // 目标目录不存在时必须单独报错：freeBytesOf 对不存在的路径返回 0，
                      // 会被误判成"空间不足"，把用户引到完全错误的方向。
                      if (!dirExists(targetPath)) {
                        *err = RpcError(kInvalidParams,
                                        "目标目录不存在: " + targetPath + "（请先创建或选择已存在的目录）");
                        return json::JsonValue::nullValue();
                      }
                      uint64_t total = 0;
                      uint64_t free = freeBytesOf(targetPath, &total);
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("freeBytes", static_cast<int64_t>(free));
                      double ratio = required > 0 ? static_cast<double>(free) / required : 999.0;
                      out.setItem("ratio", ratio);
                      out.setItem("warning", free < static_cast<uint64_t>(required) * 11 / 10);
                      if (free < static_cast<uint64_t>(required)) {
                        *err = RpcError(kInsufficientSpace,
                                        "目标剩余空间不足：需要 " +
                                            std::to_string(required / (1024 * 1024 * 1024)) +
                                            " GB，可用 " +
                                            std::to_string(free / (1024 * 1024 * 1024)) + " GB");
                        return json::JsonValue::nullValue();
                      }
                      return out;
                    });
  d->registerMethod("import.start",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      const json::JsonValue& source = params.get("source");
                      std::string sourcePath = source.get("path").asString();
                      std::string targetPath = params.get("target").asString();
                      if (sourcePath.empty() || targetPath.empty()) {
                        *err = RpcError(kInvalidParams, "params.source.path and params.target are required");
                        return json::JsonValue::nullValue();
                      }
                      std::string templateErr;
                      std::string renameTemplate =
                          params.get("renameTemplate").asString("{YYYY}-{MM}-{DD}_{camera}_{name}");
                      if (!import::RenameEngine::validate(renameTemplate, &templateErr)) {
                        *err = RpcError(kInvalidParams, templateErr);
                        return json::JsonValue::nullValue();
                      }
                      import::ImportOptions opts;
                      opts.sourcePath = sourcePath;
                      opts.targetPath = targetPath;
                      const json::JsonValue& backup = params.get("backup");
                      if (backup.isObject()) {
                        opts.backupEnabled = backup.get("enabled").asBool(false);
                        opts.backupTarget = backup.get("targetPath").asString();
                        opts.backupTiming = backup.get("timing").asString("onImport");
                      }
                      opts.verifyMd5 = params.get("verifyMd5").asBool(false);
                      opts.renameTemplate = renameTemplate;
                      opts.pairRawJpg = params.get("pairRawJpg").asBool(true);
                      opts.enterCulling = params.get("postActions").get("enterCulling").asBool(false);
                      opts.applyPreset = params.get("postActions").get("applyPreset").asString();
                      int64_t jobId = s->jobs->start("import", [s, opts](JobContext& ctx) {
                        s->importer->run(opts, ctx);
                      });
                      s->config->pushRecentSource(sourcePath);
                      std::string saveErr;
                      s->config->save(&saveErr);
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("jobId", jobId);
                      return out;
                    });
  d->registerMethod("import.cancel",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t jobId = params.get("jobId").asInt();
                      JobState state;
                      if (!s->jobs->getJob(jobId, &state)) {
                        *err = RpcError(kJobNotFound, "job not found: " + std::to_string(jobId));
                        return json::JsonValue::nullValue();
                      }
                      s->jobs->cancel(jobId);
                      return json::JsonValue::objectValue();
                    });
  d->registerMethod("import.resolveMissingFolder",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::string path = params.get("path").asString();
                      std::string action = params.get("action").asString();
                      std::string newPath = params.get("newPath").asString();
                      if (path.empty() || action.empty()) {
                        *err = RpcError(kInvalidParams, "params.path and params.action are required");
                        return json::JsonValue::nullValue();
                      }
                      int affected = 0;
                      std::string dbErr;
                      if (action == "relocate" || action == "backfill") {
                        if (newPath.empty()) {
                          *err = RpcError(kInvalidParams, "params.newPath required for relocate/backfill");
                          return json::JsonValue::nullValue();
                        }
                        std::string sql = "SELECT id, file_path, root_id, file_name FROM photos WHERE "
                                          "file_path LIKE ? AND deleted=0;";
                        auto stmt = s->db->prepareCached("resolveFolder", sql, &dbErr);
                        if (!stmt || !stmt->valid()) {
                          *err = RpcError(kDbError, dbErr);
                          return json::JsonValue::nullValue();
                        }
                        stmt->bindText(1, path + "/%");
                        // {id, oldPath, rootId, fileName}
                        std::vector<std::tuple<int64_t, std::string, int64_t, std::string>> updates;
                        while (stmt->step(&dbErr)) {
                          updates.emplace_back(stmt->columnInt64(0), stmt->columnText(1),
                                               stmt->columnInt64(2), stmt->columnText(3));
                        }
                        stmt->reset();
                        // M2 T2.2b（I3）：手写 `UPDATE photos SET file_path=?` 字符串手术
                        // 会让 files.folder_id/photos.file_id 变陈旧、sidecar_path 不跟随
                        // （移动后参数读不到）。改走 ImageDao::relocate：photos.id 不变、
                        // files 行跟随新位置、sidecar_path 用 XmpService 口径重算。
                        if (!s->images) {
                          *err = RpcError(kInternalError, "ImageDao 未装配（engine 装配错误）");
                          return json::JsonValue::nullValue();
                        }
                        for (const auto& u : updates) {
                          const int64_t photoId = std::get<0>(u);
                          const std::string& oldPath = std::get<1>(u);
                          const int64_t rootId = std::get<2>(u);
                          const std::string replaced = newPath + oldPath.substr(path.size());
                          // 目标相对库根的 rel_path：relocate 只在根内搬（与 files 身份模型一致）
                          const std::string rootPrefix = s->images->rootPath(rootId, &dbErr);
                          std::string relPath;
                          if (rootPrefix.empty()) {
                            *err = RpcError(kDbError, "root not found for photo " +
                                                          std::to_string(photoId) + ": " + dbErr);
                            return json::JsonValue::nullValue();
                          }
                          if (replaced.rfind(rootPrefix, 0) == 0) {
                            relPath = replaced.substr(rootPrefix.size());
                            while (!relPath.empty() && (relPath[0] == '/' || relPath[0] == '\\')) {
                              relPath.erase(0, 1);
                            }
                            relPath = parentPath(relPath);
                          } else {
                            // 目标在库根之外：不猜、不静默（用户需要先把该根重定位）
                            *err = RpcError(kInvalidParams,
                                            "目标路径 " + replaced + " 不在库根 " + rootPrefix +
                                                " 之内，请先调整库根或目标路径");
                            return json::JsonValue::nullValue();
                          }
                          std::string relErr;
                          if (!s->images->relocate(photoId, rootId, relPath, &relErr)) {
                            *err = RpcError(kDbError, "relocate failed for photo " +
                                                          std::to_string(photoId) + ": " + relErr);
                            return json::JsonValue::nullValue();
                          }
                          // 路径在位与否照旧显式呈现，不静默
                          std::string e;
                          s->photos->setMissing(photoId, !fileExists(replaced), &e);
                          ++affected;
                        }
                      } else if (action == "orphan") {
                        std::string sql = "SELECT id FROM photos WHERE file_path LIKE ? AND deleted=0;";
                        auto stmt = s->db->prepareCached("orphanFolder", sql, &dbErr);
                        if (!stmt || !stmt->valid()) {
                          *err = RpcError(kDbError, dbErr);
                          return json::JsonValue::nullValue();
                        }
                        stmt->bindText(1, path + "/%");
                        std::vector<int64_t> ids;
                        while (stmt->step(&dbErr)) ids.push_back(stmt->columnInt64(0));
                        stmt->reset();
                        for (int64_t id : ids) {
                          std::string e;
                          s->photos->setMissing(id, true, &e);
                          s->photos->setHealth(id, "missing", &e);
                          s->health->recordIssue(id, "missing", "", "", &e);
                          ++affected;
                        }
                      } else {
                        *err = RpcError(kInvalidParams, "params.action must be relocate|orphan|backfill");
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("affected", affected);
                      return out;
                    });
  d->registerMethod("import.listRecentSources",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      (void)err;
                      json::JsonValue out = json::JsonValue::arrayValue();
                      for (const std::string& p : s->config->recentSources()) out.addItem(p);
                      return out;
                    });

  // ================= 5.9 存储健康 =================
  d->registerMethod("health.getStatus",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      HealthCounts counts;
                      std::string dbErr;
                      if (!s->health->counts(&counts, &dbErr)) {
                        *err = RpcError(kDbError, dbErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue backup = json::JsonValue::objectValue();
                      backup.setItem("state", "none");
                      backup.setItem("lastSuccessAt", json::JsonValue::nullValue());
                      backup.setItem("target", s->config->backupTarget());
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("verified", counts.verified);
                      out.setItem("total", counts.total);
                      out.setItem("missing", counts.missing);
                      out.setItem("corrupt", counts.corrupt);
                      out.setItem("backup", backup);
                      json::JsonValue md5 = json::JsonValue::objectValue();
                      md5.setItem("percent", counts.total > 0
                                                 ? static_cast<double>(counts.verified) * 100.0 /
                                                       counts.total
                                                 : 0.0);
                      md5.setItem("running", false);
                      out.setItem("md5", md5);
                      out.setItem("abnormal", counts.missing > 0 || counts.corrupt > 0);
                      return out;
                    });
  d->registerMethod("health.scanFull",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      int64_t jobId = s->jobs->start("health.scan",
                                                     [s](JobContext& ctx) {
                                                       s->healthScanner->runFullScan(ctx);
                                                     });
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("jobId", jobId);
                      return out;
                    });
  d->registerMethod("health.listIssues",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::string kind = params.get("kind").asString();
                      Page page;
                      pageFromJson(params.get("page"), &page);
                      std::vector<HealthIssue> issues;
                      int64_t total = 0;
                      std::string dbErr;
                      if (!s->health->listIssues(kind, page.offset / (page.limit > 0 ? page.limit : 1),
                                                 page.limit, &issues, &total, &dbErr)) {
                        *err = RpcError(kDbError, dbErr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue items = json::JsonValue::arrayValue();
                      for (const HealthIssue& issue : issues) {
                        json::JsonValue item = json::JsonValue::objectValue();
                        item.setItem("issueId", issue.issueId);
                        item.setItem("photoId", issue.photoId);
                        item.setItem("path", issue.path);
                        item.setItem("lastKnown", issue.lastKnown);
                        item.setItem("kind", issue.kind);
                        item.setItem("detectedAt", issue.detectedAt);
                        items.addItem(item);
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("total", total);
                      out.setItem("items", items);
                      return out;
                    });
  d->registerMethod("health.resolveIssue",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t issueId = params.get("issueId").asInt();
                      std::string action = params.get("action").asString();
                      std::string newPath = params.get("newPath").asString();
                      std::string dbErr;
                      auto stmt = s->db->prepareCached(
                          "issueGet", "SELECT photo_id, path, kind FROM health_issues WHERE id=?;",
                          &dbErr);
                      if (!stmt || !stmt->valid()) {
                        *err = RpcError(kDbError, dbErr);
                        return json::JsonValue::nullValue();
                      }
                      stmt->bindInt64(1, issueId);
                      if (!stmt->step(&dbErr)) {
                        *err = RpcError(kJobNotFound, "issue not found: " + std::to_string(issueId));
                        stmt->reset();
                        return json::JsonValue::nullValue();
                      }
                      int64_t photoId = stmt->columnInt64(0);
                      std::string oldPath = stmt->columnText(1);
                      stmt->reset();
                      int resolved = 0;
                      if (action == "relocate" || action == "backfill") {
                        if (newPath.empty()) {
                          *err = RpcError(kInvalidParams, "params.newPath required");
                          return json::JsonValue::nullValue();
                        }
                        std::string e;
                        PhotoRecord p;
                        s->photos->getById(photoId, &p, &e);
                        std::string target = newPath.empty() ? p.filePath : newPath;
                        // M2 T2.2b（I3）：手写 UPDATE photos SET file_path=? 会留下
                        // files.folder_id 陈旧 + sidecar_path 不跟随（参数读不到）。
                        // 改走 ImageDao::relocate（id 不变、files 行跟随、sidecar 重算）。
                        // 特例：newPath 为空 = 原路径已恢复（文件回到 lastKnown），
                        // 无需搬迁身份，只补 missing/health 状态。
                        if (!target.empty() && s->images && target != p.filePath) {
                          const std::string rootPrefix = s->images->rootPath(p.rootId, &dbErr);
                          if (rootPrefix.empty() || target.rfind(rootPrefix, 0) != 0) {
                            *err = RpcError(kInvalidParams,
                                            "目标路径 " + target + " 不在库根 " + rootPrefix +
                                                " 之内，请先调整库根或目标路径");
                            return json::JsonValue::nullValue();
                          }
                          std::string relPath = target.substr(rootPrefix.size());
                          while (!relPath.empty() && (relPath[0] == '/' || relPath[0] == '\\')) {
                            relPath.erase(0, 1);
                          }
                          relPath = parentPath(relPath);
                          std::string relErr;
                          if (!s->images->relocate(photoId, p.rootId, relPath, &relErr)) {
                            *err = RpcError(kDbError, "relocate failed for photo " +
                                                          std::to_string(photoId) + ": " + relErr);
                            return json::JsonValue::nullValue();
                          }
                        }
                        s->photos->setMissing(photoId, !fileExists(target), &e);
                        s->photos->setHealth(photoId, fileExists(target) ? "ok" : "missing", &e);
                        ++resolved;
                      } else if (action == "restore") {
                        std::string restored;
                        std::string restoreErr;
                        if (s->backup->restorePhoto(photoId, &restored, &restoreErr)) ++resolved;
                        else {
                          *err = RpcError(kFileNotFound, restoreErr);
                          return json::JsonValue::nullValue();
                        }
                      } else if (action == "orphan") {
                        std::string e;
                        s->photos->setMissing(photoId, true, &e);
                        s->photos->setHealth(photoId, "missing", &e);
                        ++resolved;
                      } else {
                        *err = RpcError(kInvalidParams,
                                        "params.action must be relocate|orphan|backfill|restore");
                        return json::JsonValue::nullValue();
                      }
                      s->health->resolveIssue(issueId, "resolved", &dbErr);
                      (void)oldPath;
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("resolved", resolved);
                      return out;
                    });
  d->registerMethod("health.restoreFromBackup",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t photoId = params.get("photoId").asInt();
                      std::string restored;
                      std::string restoreErr;
                      if (!s->backup->restorePhoto(photoId, &restored, &restoreErr)) {
                        *err = RpcError(kFileNotFound, restoreErr);
                        return json::JsonValue::nullValue();
                      }
                      int64_t jobId = s->jobs->start("health.restore", [](JobContext& ctx) {
                        json::JsonValue result = json::JsonValue::objectValue();
                        ctx.setResult(result);
                        ctx.progress(100, "restored");
                      });
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("jobId", jobId);
                      out.setItem("restoredPath", restored);
                      return out;
                    });
  d->registerMethod("health.verifyPhotos",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::vector<int64_t> ids = idsOf(params, "ids");
                      if (ids.empty()) {
                        std::string dbErr;
                        if (!s->photos->listIds(Filter(), &ids, &dbErr)) {
                          *err = RpcError(kDbError, dbErr);
                          return json::JsonValue::nullValue();
                        }
                      }
                      int64_t jobId = s->jobs->start("health.verify",
                                                     [s, ids](JobContext& ctx) {
                                                       s->healthScanner->verifyPhotos(ids, ctx);
                                                     });
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("jobId", jobId);
                      return out;
                    });
  d->registerMethod("health.getBackupConfig",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      (void)err;
                      return s->backup->configJson();
                    });
  // WP-Beta（@since 1.1.0）：health 域专属取消（JobManager 取消包装 + 存在性校验）
  d->registerMethod("health.cancelJob",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      int64_t jobId = params.get("jobId").asInt();
                      JobState state;
                      if (!s->jobs->getJob(jobId, &state)) {
                        *err = RpcError(kJobNotFound, "job not found: " + std::to_string(jobId));
                        return json::JsonValue::nullValue();
                      }
                      s->jobs->cancel(jobId);
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("cancelled", true);
                      return out;
                    });
  // WP-A5（@since 1.1.0）：双备份差异报告导出
  d->registerMethod("health.exportDiffReport",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return s->backup->exportDiffReport(params, err);
                    });
  // WP-A6（@since 1.1.0）：原片备份触发（runBackup 走 JobManager，异步 + 事件齐备）
  d->registerMethod("health.backupNow",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      // target 可选：缺省用 settings.backupTarget；显式传入则持久化，
                      // 保证与 health.exportDiffReport / 后续增量跑同一目标（不静默选盘）
                      std::string target = params.get("target").asString();
                      if (!target.empty()) {
                        s->config->setBackupTarget(target);
                        std::string saveErr;
                        if (!s->config->save(&saveErr)) {
                          *err = RpcError(kIoError, "备份目标持久化失败: " + saveErr);
                          return json::JsonValue::nullValue();
                        }
                      }
                      if (s->config->backupTarget().empty()) {
                        *err = RpcError(kInvalidParams,
                                        "备份目标未配置：请传 params.target 或先经 "
                                        "health.setBackupTarget 配置（零静默，不静默选盘）");
                        return json::JsonValue::nullValue();
                      }
                      int64_t jobId = s->jobs->start("health.backup",
                                                     [s](JobContext& ctx) {
                                                       s->backup->runBackup(ctx);
                                                     });
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("jobId", jobId);
                      return out;
                    });
  // WP-A6（@since 1.1.0）：备份目标配置（读回走既有 health.getBackupConfig）
  d->registerMethod("health.setBackupTarget",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      std::string path = params.get("path").asString();
                      if (path.empty()) {
                        *err = RpcError(kInvalidParams, "params.path is required");
                        return json::JsonValue::nullValue();
                      }
                      // 先建目录：权限/路径问题在这里显式失败，而不是等备份 job 跑一半
                      if (!dirExists(path) && !makeDirs(path)) {
                        *err = RpcError(kIoError, "无法创建备份目录: " + path);
                        return json::JsonValue::nullValue();
                      }
                      s->config->setBackupTarget(path);
                      std::string saveErr;
                      if (!s->config->save(&saveErr)) {
                        *err = RpcError(kIoError, "设置持久化失败: " + saveErr);
                        return json::JsonValue::nullValue();
                      }
                      return s->backup->configJson();
                    });

  // ================= 5.15 模拟图库 =================
  d->registerMethod("mock.generate",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      mock::MockSpec spec;
                      std::string specErr;
                      if (!mock::MockSpec::fromJson(params, &spec, &specErr)) {
                        *err = RpcError(kInvalidParams, specErr);
                        return json::JsonValue::nullValue();
                      }
                      int64_t jobId = s->jobs->start("mock.generate",
                                                     [s, spec](JobContext& ctx) {
                                                       s->mock->run(spec, ctx);
                                                     });
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("jobId", jobId);
                      return out;
                    });
  d->registerMethod("mock.status",
                    [s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      return s->mock->status(err);
                    });

  // ================= 5.10 导出队列（已实现）：预设落 SQLite + 异步 job =================
  // 与 map 域同一装配方式：服务实例由 registerMethods 持有，lambda 共享所有权。
  auto exportSvc =
      std::make_shared<exporter::Exporter>(s->config, s->photos, s->db, s->decoder, s->xmp);
  d->registerMethod("export.createJob",
                    [exportSvc, s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return exportSvc->createJob(params, s->jobs, err);
                    });
  d->registerMethod("export.listJobs",
                    [exportSvc, s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return exportSvc->listJobs(params, s->jobs, err);
                    });
  d->registerMethod("export.cancelJob",
                    [exportSvc, s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return exportSvc->cancelJob(params, s->jobs, err);
                    });
  d->registerMethod("export.retryJob",
                    [exportSvc, s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return exportSvc->retryJob(params, s->jobs, err);
                    });
  d->registerMethod("export.listPresets",
                    [exportSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      (void)params;
                      return exportSvc->listPresets(err);
                    });
  d->registerMethod("export.savePreset",
                    [exportSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return exportSvc->savePreset(params, err);
                    });

  // ================= 5.10 M3-03：psd.inspect（PSD 自检 / 外来判定） =================
  // 入参：{ path }；出参：{ path, width, height, depth, channelCount, layerCount,
  //        hasLuminaSnapshot, foreign, unknownBlockCount }。
  // foreign = !hasLuminaSnapshot：该 PSD 非 LUMINA 导出 → 打开时降级为合并像素层并显式告知。
  d->registerMethod("psd.inspect",
                    [](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      const std::string path = params.get("path").asString();
                      if (path.empty()) {
                        *err = RpcError(kInvalidParams, "psd.inspect 需要 path");
                        return json::JsonValue::nullValue();
                      }
                      lumina::psd::PsdInfo info;
                      std::string rerr;
                      if (!lumina::psd::readPsd(path, &info, &rerr)) {
                        *err = RpcError(kIoError, rerr);
                        return json::JsonValue::nullValue();
                      }
                      json::JsonValue out = json::JsonValue::objectValue();
                      out.setItem("path", path);
                      out.setItem("width", info.width);
                      out.setItem("height", info.height);
                      out.setItem("depth", info.depth);
                      out.setItem("channelCount", info.channels);
                      out.setItem("layerCount", static_cast<int>(info.layers.size()));
                      out.setItem("hasLuminaSnapshot", info.hasLuminaSnapshot);
                      out.setItem("foreign", !info.hasLuminaSnapshot);
                      out.setItem("unknownBlockCount", info.unknownBlockCount);
                      return out;
                    });

  // ================= 5.11 Web 客户画廊（已实现）：静态站生成 + 密码门禁 + 反馈回收 =================
  // 与 export/map 同一装配方式：shared_ptr 由 lambda 持有，生命周期跟 dispatcher 走。
  auto gallerySvc = std::make_shared<gallery::GalleryService>(s->config, s->db, s->photos, s->albums,
                                                              s->thumbs);
  d->registerMethod("gallery.publish",
                    [gallerySvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return gallerySvc->publish(params, err);
                    });
  d->registerMethod("gallery.revoke",
                    [gallerySvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return gallerySvc->revoke(params, err);
                    });
  d->registerMethod("gallery.listPublished",
                    [gallerySvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return gallerySvc->listPublished(params, err);
                    });
  d->registerMethod("gallery.importFeedback",
                    [gallerySvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return gallerySvc->importFeedback(params, err);
                    });
  d->registerMethod("gallery.getFeedback",
                    [gallerySvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return gallerySvc->getFeedback(params, err);
                    });

  // ================= 5.12 延时工作台（已实现）：序列/去闪/Holy Grail 关键帧/渲染队列 =================
  auto timelapseSvc = std::make_shared<timelapse::TimelapseService>(s->config, s->db, s->photos,
                                                                    s->xmp, s->decoder);
  d->registerMethod("timelapse.listSequences",
                    [timelapseSvc, s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return timelapseSvc->listSequences(params, s->jobs, err);
                    });
  d->registerMethod("timelapse.getSequence",
                    [timelapseSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return timelapseSvc->getSequence(params, err);
                    });
  d->registerMethod("timelapse.analyzeFlicker",
                    [timelapseSvc, s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return timelapseSvc->analyzeFlicker(params, s->jobs, err);
                    });
  d->registerMethod("timelapse.applyDeflicker",
                    [timelapseSvc, s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return timelapseSvc->applyDeflicker(params, s->jobs, err);
                    });
  d->registerMethod("timelapse.setExposureRamp",
                    [timelapseSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return timelapseSvc->setExposureRamp(params, err);
                    });
  d->registerMethod("timelapse.previewFrame",
                    [timelapseSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return timelapseSvc->previewFrame(params, err);
                    });
  d->registerMethod("timelapse.render",
                    [timelapseSvc, s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return timelapseSvc->render(params, s->jobs, err);
                    });
  d->registerMethod("timelapse.getRenderJobs",
                    [timelapseSvc, s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return timelapseSvc->getRenderJobs(params, s->jobs, err);
                    });

  // ================= 5.13 风光专项（已实现）：HDR 包围合成 + NLM 降噪 =================
  auto hdrSvc = std::make_shared<specialty::HdrService>(s->config, s->db, s->photos, s->decoder);
  d->registerMethod("hdr.detectBracketGroups",
                    [hdrSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return hdrSvc->detectBracketGroups(params, err);
                    });
  d->registerMethod("hdr.composite",
                    [hdrSvc, s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return hdrSvc->composite(params, s->jobs, err);
                    });

  auto denoiseSvc =
      std::make_shared<specialty::DenoiseService>(s->config, s->photos, s->decoder, s->xmp);
  d->registerMethod("denoise.apply",
                    [denoiseSvc, s](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return denoiseSvc->apply(params, s->jobs, err);
                    });
  d->registerMethod("denoise.preview",
                    [denoiseSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return denoiseSvc->preview(params, err);
                    });

  // ---- 地图域（已实现）：聚合从 photos.gps_* 现算，地点按名称登记拿稳定 id ----
  auto mapSvc = std::make_shared<map::MapService>(s->config, s->db, s->photos, s->xmp);
  d->registerMethod("map.getLocations",
                    [mapSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return mapSvc->getLocations(params, err);
                    });
  d->registerMethod("map.getLocationDetail",
                    [mapSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return mapSvc->getLocationDetail(params, err);
                    });
  d->registerMethod("map.setPhotoLocation",
                    [mapSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return mapSvc->setPhotoLocation(params, err);
                    });
  d->registerMethod("map.clearPhotoLocation",
                    [mapSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return mapSvc->clearPhotoLocation(params, err);
                    });
  d->registerMethod("map.listUnlocated",
                    [mapSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return mapSvc->listUnlocated(params, err);
                    });
  d->registerMethod("map.toggleFavorite",
                    [mapSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return mapSvc->toggleFavorite(params, err);
                    });
  // ---- WP-A4（@since 1.1.0）：离线瓦片 + GPX ----
  d->registerMethod("map.getTile",
                    [mapSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      // 二进制不进 JSON（协议红线）：只回瓦片文件路径
                      return mapSvc->getTile(params, err);
                    });
  d->registerMethod("map.importGpx",
                    [mapSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return mapSvc->importGpx(params, err);
                    });
  d->registerMethod("map.applyGpxPoints",
                    [mapSvc](const json::JsonValue& params, RpcError* err) -> json::JsonValue {
                      return mapSvc->applyGpxPoints(params, err);
                    });

  // 至此协议 §5 全部 15 个域均已有真实实现。notYet() 仅作为将来新增方法的统一出口保留。
}

}  // namespace rpc
}  // namespace lumina
