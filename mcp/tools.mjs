// LUMINA MCP 工具集：精选核心工作流（导入/整理/显影编辑/分析/导出/健康）
// 每个工具 = { name, description, inputSchema, handler(engine, args) }
// handler 返回纯对象（自动 JSON 序列化为 text content）；
// 返回对象可带 _images: [{ data: base64, mimeType }] 附加图片 content block。
import { readFile } from "node:fs/promises";

// ---- 常用 JSON Schema 片段 ----
const photoIds = {
  type: "array",
  items: { type: "integer" },
  minItems: 1,
  description: "照片 ID 列表（先经 lumina_library_query / lumina_library_search 获得）",
};
const developParams = {
  type: "object",
  description:
    "显影参数（NodeGraph 叶子键）。常用：exposure(-5..5 曝光)、contrast(对比)、highlights、shadows、whites、blacks、clarity(清晰度)、dehaze(去雾)、vibrance(自然饱和)、saturation(饱和)、temperature(色温,整数)、tint(色调,整数)、sharpenDetail(锐化细节)。未指定的键保持不变。",
  additionalProperties: true,
};
const filterSchema = {
  type: "object",
  description: "过滤条件（全部可选，组合生效）",
  properties: {
    folderIds: { type: "array", items: { type: "integer" }, description: "文件夹 ID" },
    formats: { type: "array", items: { type: "string" }, description: '格式，如 ["jpg","nef","arw"]' },
    cameras: { type: "array", items: { type: "string" }, description: "机身型号" },
    lenses: { type: "array", items: { type: "string" }, description: "镜头型号" },
    minRating: { type: "integer", minimum: 0, maximum: 5, description: "最低星级" },
    maxRating: { type: "integer", minimum: 0, maximum: 5, description: "最高星级" },
    pick: { type: "string", enum: ["pick", "reject", "none"], description: "筛选旗标" },
    colorLabel: { type: "string", description: "颜色标签" },
    located: { type: "boolean", description: "是否已标注 GPS 位置" },
    edited: { type: "boolean", description: "是否已编辑" },
    hasMasks: { type: "boolean", description: "是否含蒙版" },
    missing: { type: "boolean", description: "文件是否丢失" },
    isVideo: { type: "boolean", description: "是否视频" },
    dateFrom: { type: "integer", description: "拍摄时间起（Unix 秒）" },
    dateTo: { type: "integer", description: "拍摄时间止（Unix 秒）" },
  },
  additionalProperties: false,
};
const pageSchema = {
  type: "object",
  properties: {
    offset: { type: "integer", minimum: 0, default: 0 },
    limit: { type: "integer", minimum: 1, maximum: 1000, default: 200 },
  },
  additionalProperties: false,
};
const sortSchema = {
  type: "object",
  properties: {
    field: { type: "string", enum: ["capturedAt", "importedAt", "fileName", "rating"], default: "capturedAt" },
    order: { type: "string", enum: ["asc", "desc"], default: "asc" },
  },
  additionalProperties: false,
};
const waitOpts = {
  wait: { type: "boolean", default: true, description: "是否等待任务终态后再返回（推荐 true）" },
  waitTimeoutMs: { type: "integer", default: 180000, description: "等待终态的超时（毫秒），超时后任务仍在后台可再查询" },
};

// 小工具函数
const reqStr = (args, key) => {
  const v = args[key];
  if (typeof v !== "string" || !v.trim()) throw new Error(`参数 ${key} 必须为非空字符串`);
  return v;
};
const maybeImage = (out, returnImage, filePath, mimeType) =>
  returnImage
    ? readFile(filePath)
        .then((buf) => ({ ...out, _images: [{ data: buf.toString("base64"), mimeType }] }))
    : Promise.resolve(out);

/** 一站式编辑：beginSession → setParams → endSession，agent 无需管理会话 */
async function editPhoto(engine, args) {
  const begin = await engine.request("edit.beginSession", { id: args.photoId });
  const sessionId = begin?.sessionId;
  if (!sessionId) throw new Error(`edit.beginSession 未返回 sessionId: ${JSON.stringify(begin)}`);
  try {
    const set = await engine.request("edit.setParams", {
      sessionId,
      params: args.params ?? {},
      commitHistory: args.commitHistory ?? true,
    });
    const end = await engine.request("edit.endSession", { sessionId, flush: true });
    return { photoId: args.photoId, params: end?.params ?? set?.params ?? null, committed: true };
  } catch (err) {
    try {
      await engine.request("edit.endSession", { sessionId, flush: false });
    } catch {
      /* 会话已死，忽略 */
    }
    throw err;
  }
}

export const TOOLS = [
  // ================= 系统 =================
  {
    name: "lumina_system_info",
    description:
      "获取 LUMINA 引擎信息：版本、协议版本、库主目录、数据库路径、能力（LibRaw/FTS5/编解码器）。连接后先调它确认引擎健康。",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
    handler: async (engine) => {
      const ping = await engine.request("system.ping", {});
      return { ...engine.handshake, uptimeMs: ping.uptimeMs, engineRunning: true };
    },
  },
  {
    name: "lumina_job_status",
    description: "查询后台任务状态（导入/扫描/导出/健康检查等长任务）。传入 jobId 查单个，不传则列出本会话跟踪到的全部任务。",
    inputSchema: {
      type: "object",
      properties: { jobId: { type: "integer", description: "任务 ID（省略则列出全部）" } },
      additionalProperties: false,
    },
    handler: async (engine, args) => {
      if (args.jobId != null) {
        const job = engine.jobs.get(Number(args.jobId));
        return job ?? { jobId: args.jobId, found: false, note: "本 MCP 会话未跟踪到该任务（任务可能在连接前启动）" };
      }
      return { jobs: [...engine.jobs.values()].sort((a, b) => a.jobId - b.jobId) };
    },
  },
  {
    name: "lumina_job_cancel",
    description: "取消一个后台任务（自动尝试导入/健康/导出各自的取消入口）。",
    inputSchema: {
      type: "object",
      properties: { jobId: { type: "integer" } },
      required: ["jobId"],
      additionalProperties: false,
    },
    handler: (engine, args) => engine.cancelJob(Number(args.jobId)),
  },

  // ================= 整理 =================
  {
    name: "lumina_library_stats",
    description: "图库统计：照片总数、各类数量等。",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
    handler: (engine) => engine.request("library.getStats", {}),
  },
  {
    name: "lumina_library_query",
    description: "按条件分页查询照片列表（星级/旗标/机身/镜头/格式/日期/文件夹等），返回照片摘要（含 id、fileName、filePath、rating、拍摄时间等）。",
    inputSchema: {
      type: "object",
      properties: { filter: filterSchema, sort: sortSchema, page: pageSchema },
      additionalProperties: false,
    },
    handler: (engine, args) =>
      engine.request("library.queryPhotos", {
        filter: args.filter ?? {},
        sort: args.sort ?? { field: "capturedAt", order: "desc" },
        page: args.page ?? { offset: 0, limit: 200 },
      }),
  },
  {
    name: "lumina_library_search",
    description: "全文搜索照片（FTS5：文件名、关键词、描述、EXIF 等），分页返回。",
    inputSchema: {
      type: "object",
      properties: { text: { type: "string", description: "搜索词" }, page: pageSchema },
      required: ["text"],
      additionalProperties: false,
    },
    handler: (engine, args) => engine.request("library.search", { text: reqStr(args, "text"), page: args.page }),
  },
  {
    name: "lumina_library_facets",
    description: "获取分面计数（按机身/镜头/星级/文件夹等维度的数量分布），用于构建筛选器。",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
    handler: (engine) => engine.request("library.getFacets", {}),
  },
  {
    name: "lumina_folder_tree",
    description: "获取库内文件夹树（含各文件夹照片数与 folderId，可作 queryPhotos.filter.folderIds 的入参）。",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
    handler: (engine) => engine.request("library.getFolderTree", {}),
  },
  {
    name: "lumina_photo_detail",
    description: "获取单张照片完整信息：EXIF、星级、旗标、关键词、显影参数摘要、文件夹路径等。",
    inputSchema: {
      type: "object",
      properties: { photoId: { type: "integer" } },
      required: ["photoId"],
      additionalProperties: false,
    },
    handler: (engine, args) => engine.request("library.getPhotoDetail", { id: args.photoId }),
  },
  {
    name: "lumina_photo_update",
    description: "批量更新照片元数据：星级(0-5)、旗标(pick/reject/none)、颜色标签、关键词数组、描述。改动会同步 XMP sidecar。",
    inputSchema: {
      type: "object",
      properties: {
        photoIds,
        patch: {
          type: "object",
          properties: {
            rating: { type: "integer", minimum: 0, maximum: 5 },
            pick: { type: "string", enum: ["pick", "reject", "none"] },
            colorLabel: { type: "string" },
            keywords: { type: "array", items: { type: "string" } },
            description: { type: "string" },
          },
          additionalProperties: false,
          description: "要更新的字段（只传需要改的）",
        },
      },
      required: ["photoIds", "patch"],
      additionalProperties: false,
    },
    handler: (engine, args) =>
      engine.request("library.updatePhotos", { ids: args.photoIds, patch: args.patch }),
  },
  {
    name: "lumina_photo_remove",
    description: "从图库移除照片（软删除，可用 lumina_photo_restore 恢复；不删除磁盘原文件）。",
    inputSchema: { type: "object", properties: { photoIds }, required: ["photoIds"], additionalProperties: false },
    handler: (engine, args) => engine.request("library.removePhotos", { ids: args.photoIds }),
  },
  {
    name: "lumina_photo_restore",
    description: "恢复之前移除的照片。",
    inputSchema: { type: "object", properties: { photoIds }, required: ["photoIds"], additionalProperties: false },
    handler: (engine, args) => engine.request("library.restorePhotos", { ids: args.photoIds }),
  },

  // ================= 相册 =================
  {
    name: "lumina_album_list",
    description: "列出相册/收藏集（kind: collection=普通相册, published=已发布画廊）。",
    inputSchema: {
      type: "object",
      properties: { kind: { type: "string", enum: ["collection", "published"] } },
      additionalProperties: false,
    },
    handler: (engine, args) => engine.request("albums.list", { kind: args.kind ?? "collection" }),
  },
  {
    name: "lumina_album_create",
    description: "创建相册。",
    inputSchema: {
      type: "object",
      properties: {
        name: { type: "string", description: "相册名" },
        kind: { type: "string", enum: ["collection", "published"], default: "collection" },
      },
      required: ["name"],
      additionalProperties: false,
    },
    handler: (engine, args) => engine.request("albums.create", { name: reqStr(args, "name"), kind: args.kind ?? "collection" }),
  },
  {
    name: "lumina_album_add_photos",
    description: "把照片加入相册。",
    inputSchema: {
      type: "object",
      properties: { albumId: { type: "integer" }, photoIds },
      required: ["albumId", "photoIds"],
      additionalProperties: false,
    },
    handler: (engine, args) => engine.request("albums.addPhotos", { albumId: args.albumId, ids: args.photoIds }),
  },
  {
    name: "lumina_album_remove_photos",
    description: "从相册移除照片（不动图库）。",
    inputSchema: {
      type: "object",
      properties: { albumId: { type: "integer" }, photoIds },
      required: ["albumId", "photoIds"],
      additionalProperties: false,
    },
    handler: (engine, args) => engine.request("albums.removePhotos", { albumId: args.albumId, ids: args.photoIds }),
  },

  // ================= 导入 =================
  {
    name: "lumina_import_scan",
    description: "扫描一个文件夹（含子目录）里的照片文件，返回总数、字节数、格式分布、RAW+JPG 配对、重复与不可读清单，不写库。",
    inputSchema: {
      type: "object",
      properties: { sourcePath: { type: "string", description: "源文件夹绝对路径" }, ...waitOpts },
      required: ["sourcePath"],
      additionalProperties: false,
    },
    handler: (engine, args) =>
      engine.startJob("import.scanSource", { source: { kind: "folder", path: reqStr(args, "sourcePath") } }, args),
  },
  {
    name: "lumina_import_check_target",
    description: "检查导入目标磁盘：剩余空间是否够、是否有写入权限。",
    inputSchema: {
      type: "object",
      properties: {
        targetPath: { type: "string", description: "目标目录" },
        requiredBytes: { type: "integer", description: "预计需要的字节数（可用 lumina_import_scan 的 totalBytes）" },
      },
      required: ["targetPath"],
      additionalProperties: false,
    },
    handler: (engine, args) =>
      engine.request("import.checkTarget", { targetPath: reqStr(args, "targetPath"), requiredBytes: args.requiredBytes ?? 0 }),
  },
  {
    name: "lumina_import_start",
    description:
      "执行导入：把源文件夹照片复制进图库目录并入库（内容级 MD5 去重、RAW+JPG 配对、可选 MD5 校验）。长任务，默认等待完成。",
    inputSchema: {
      type: "object",
      properties: {
        sourcePath: { type: "string", description: "源文件夹绝对路径" },
        target: { type: "string", description: "图库目标目录（如未初始化库可用独立的空目录）" },
        verifyMd5: { type: "boolean", default: false, description: "导入后逐文件 MD5 校验" },
        pairRawJpg: { type: "boolean", default: true, description: "RAW+JPG 成组" },
        renameTemplate: {
          type: "string",
          default: "{YYYY}-{MM}-{DD}_{camera}_{name}",
          description: "重命名模板，可用占位符 {YYYY}{MM}{DD}{camera}{lens}{name}{seq}",
        },
        ...waitOpts,
      },
      required: ["sourcePath", "target"],
      additionalProperties: false,
    },
    handler: (engine, args) =>
      engine.startJob(
        "import.start",
        {
          source: { kind: "folder", path: reqStr(args, "sourcePath") },
          target: reqStr(args, "target"),
          verifyMd5: args.verifyMd5 ?? false,
          pairRawJpg: args.pairRawJpg ?? true,
          renameTemplate: args.renameTemplate ?? "{YYYY}-{MM}-{DD}_{camera}_{name}",
        },
        args,
      ),
  },

  // ================= 显影编辑 =================
  {
    name: "lumina_edit_photo",
    description:
      "一站式编辑照片显影参数（自动管理编辑会话，等价 beginSession→setParams→endSession）。改动写入 XMP sidecar 并进编辑历史。",
    inputSchema: {
      type: "object",
      properties: {
        photoId: { type: "integer" },
        params: developParams,
        commitHistory: { type: "boolean", default: true, description: "是否记入撤销历史" },
      },
      required: ["photoId", "params"],
      additionalProperties: false,
    },
    handler: editPhoto,
  },
  {
    name: "lumina_develop_render",
    description: "按显影参数渲染出图（PNG 到缓存目录），可回传图像本体供 agent 直接查看效果。",
    inputSchema: {
      type: "object",
      properties: {
        photoId: { type: "integer" },
        params: { ...developParams, description: "显影参数；省略则用该照片当前已保存的参数" },
        longEdge: { type: "integer", default: 1600, description: "输出长边像素" },
        returnImage: { type: "boolean", default: false, description: "true 时把 PNG 以图片 content 回传（注意 token 开销）" },
      },
      required: ["photoId"],
      additionalProperties: false,
    },
    handler: async (engine, args) => {
      const detail = await engine.request("library.getPhotoDetail", { id: args.photoId });
      const inputPath = detail?.filePath ?? detail?.photo?.filePath;
      if (!inputPath) throw new Error(`photoId=${args.photoId} 未找到 filePath`);
      const rendered = await engine.request("develop.render", {
        inputPath,
        photoId: args.photoId,
        params: args.params ?? {},
        longEdge: args.longEdge ?? 1600,
      });
      const out = { path: rendered.path, width: rendered.width, height: rendered.height };
      return maybeImage(out, !!args.returnImage, rendered.path, "image/png");
    },
  },
  {
    name: "lumina_preview_get",
    description: "获取照片预览图（JPEG，来自原片解码），可回传图像本体让 agent 直接『看』照片。",
    inputSchema: {
      type: "object",
      properties: {
        photoId: { type: "integer" },
        returnImage: { type: "boolean", default: false, description: "true 时以图片 content 回传（注意 token 开销）" },
      },
      required: ["photoId"],
      additionalProperties: false,
    },
    handler: (engine, args) =>
      engine.request("preview.getBase", { id: args.photoId }).then((r) =>
        maybeImage({ path: r.path, width: r.width, height: r.height, source: r.source }, !!args.returnImage, r.path, "image/jpeg"),
      ),
  },
  {
    name: "lumina_presets_list",
    description: "列出内置显影预设（含各预设的完整参数）。",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
    handler: (engine) => engine.request("presets.list", {}),
  },
  {
    name: "lumina_presets_apply",
    description: "把预设批量应用到照片（写入各照片显影参数与 sidecar）。",
    inputSchema: {
      type: "object",
      properties: { presetId: { type: "string" }, photoIds },
      required: ["presetId", "photoIds"],
      additionalProperties: false,
    },
    handler: (engine, args) => engine.request("presets.apply", { presetId: reqStr(args, "presetId"), targetIds: args.photoIds }),
  },

  // ================= 分析 =================
  {
    name: "lumina_histogram",
    description: "计算照片直方图（RGB 各 256 桶；edited=true 时按已保存显影参数渲染后统计）。",
    inputSchema: {
      type: "object",
      properties: { photoId: { type: "integer" }, edited: { type: "boolean", default: false } },
      required: ["photoId"],
      additionalProperties: false,
    },
    handler: (engine, args) => engine.request("analysis.histogram", { id: args.photoId, edited: args.edited ?? false }),
  },
  {
    name: "lumina_exposure_check",
    description: "检查照片曝光：欠曝/过曝像素占比（按显影后统计），辅助 agent 判断是否需要调整曝光。",
    inputSchema: {
      type: "object",
      properties: { photoId: { type: "integer" } },
      required: ["photoId"],
      additionalProperties: false,
    },
    handler: (engine, args) => engine.request("analysis.getExposedRange", { id: args.photoId }),
  },

  // ================= 导出 =================
  {
    name: "lumina_export_create",
    description:
      "创建导出任务：按格式(jpg/tiff/png)、长边、色彩空间、质量、水印、命名模板导出照片。只读原片+sidecar，绝不改写原文件。长任务，默认等待完成。",
    inputSchema: {
      type: "object",
      properties: {
        photoIds,
        spec: {
          type: "object",
          properties: {
            format: { type: "string", enum: ["jpg", "tiff", "png"], default: "jpg", description: "psd 暂不支持" },
            longEdge: { type: "integer", default: 2048, description: "输出长边像素（只缩小不放大）" },
            colorSpace: { type: "string", enum: ["sRGB", "AdobeRGB"], default: "sRGB" },
            quality: { type: "integer", minimum: 1, maximum: 100, default: 90, description: "仅 jpg 有效" },
            namingTemplate: { type: "string", default: "{date}-{name}" },
            outputDir: { type: "string", description: "输出目录；省略则 {home}/exports" },
            watermark: {
              type: "object",
              description: "文字/图片水印",
              properties: {
                enabled: { type: "boolean", default: true },
                text: { type: "string", description: "文字水印内容（仅 ASCII）" },
                imagePath: { type: "string", description: "图片水印路径" },
                position: { type: "string", enum: ["bottom-right", "bottom-left", "top-right", "top-left", "center"] },
                opacity: { type: "number", minimum: 0, maximum: 100 },
                scale: { type: "number", description: "相对基准尺寸倍率，默认 0.88" },
              },
              additionalProperties: false,
            },
          },
          additionalProperties: false,
        },
        ...waitOpts,
      },
      required: ["photoIds", "spec"],
      additionalProperties: false,
    },
    handler: (engine, args) => engine.startJob("export.createJob", { photoIds: args.photoIds, spec: args.spec ?? {} }, args),
  },
  {
    name: "lumina_export_list",
    description: "查询导出任务队列状态。",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
    handler: (engine) => engine.request("export.listJobs", {}),
  },

  // ================= 健康 =================
  {
    name: "lumina_health_status",
    description: "图库健康总览：已验证/丢失/损坏计数、MD5 校验进度、备份状态。",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
    handler: (engine) => engine.request("health.getStatus", {}),
  },
  {
    name: "lumina_health_scan",
    description: "全库健康扫描（文件在否、MD5 是否一致），长任务，默认等待完成。",
    inputSchema: {
      type: "object",
      properties: { ...waitOpts, waitTimeoutMs: { type: "integer", default: 600000, description: "全库扫描较慢，默认等 10 分钟" } },
      additionalProperties: false,
    },
    handler: (engine, args) => engine.startJob("health.scanFull", {}, { waitTimeoutMs: 600000, ...args }),
  },
];
