/**
 * MCP 渲染层执行器（v1.3.0-m3）
 *
 * UI 状态（当前照片 / 调整参数 / 蒙版 / 模块路由）在 renderer 的 zustand store 里，
 * 主进程只做管道。执行器订阅 preload 暴露的 lumina:mcp:call 通道，把 MCP 指令
 * 分发到既有 store 真实动作（与用户鼠标操作同一条代码路径），结果原路回传。
 *
 * 挂载点唯一：app/src/main.tsx 顶部 initMcpExecutor()。
 * 纪律：只调用既有导出 API；未知参数键报错不静默；任何错误都回传给 MCP 客户端。
 */

import { api } from '../api/engine';
import { errText } from '../api/rpc-client';
import { useDevelop, normalizeParams, temperatureToKelvin } from '../store/developStore';
import { useLibrary } from '../store/libraryStore';
import { useUi, type AppView } from '../store/uiStore';
import type { EditParams, MaskLayer } from '../api/protocol';

// ---------- set_develop_params 白名单（顶层键；嵌套对象整体下发由 deepMerge 语义承接） ----------

const PARAM_KEYS = new Set<string>([
  'processVersion', 'whiteBalance', 'temperature', 'tint', 'exposure', 'contrast',
  'highlights', 'shadows', 'whites', 'blacks', 'clarity', 'texture', 'dehaze',
  'vibrance', 'saturation', 'toneCurveRGB', 'toneCurveR', 'toneCurveG', 'toneCurveB',
  'parametric', 'hsl', 'colorGrade', 'calibration', 'detail', 'optics', 'geometry',
  'masks', 'removals', 'adjustmentStack',
]);

const MASK_LOCAL_KEYS = new Set<string>([
  'exposure', 'contrast', 'highlights', 'shadows', 'temperature', 'tint',
  'saturation', 'clarity', 'dehaze',
]);

const MASK_TYPES = new Set<string>(['linear', 'radial', 'brush']);

const MODULE_VIEWS: AppView[] = [
  'library', 'develop', 'ingest', 'health', 'culling', 'map',
  'timelapse', 'landscape', 'export', 'portfolio', 'gallery',
];

// ---------- 帮助 ----------

class McpOpError extends Error {}

function requireDevelopSession(): { photoId: number; sessionId: number; params: EditParams } {
  const st = useDevelop.getState();
  if (st.sessionId === null || st.params === null || st.photoId === null) {
    throw new McpOpError('当前没有打开的修图会话，请先调用 open_photo');
  }
  return { photoId: st.photoId, sessionId: st.sessionId, params: st.params };
}

function requireNumber(v: unknown, name: string): number {
  if (typeof v !== 'number' || !Number.isFinite(v)) throw new McpOpError(`参数 ${name} 必须是数字`);
  return v;
}

function clamp(v: number, lo: number, hi: number): number {
  return Math.min(hi, Math.max(lo, v));
}

/** list_photos 输出裁剪：MCP 响应保持精炼，元数据细项走 lumina_status/photoDetail */
function slimPhoto(p: {
  id: number; fileName: string; filePath: string; format: string;
  camera: string; rating: number; pick: string; capturedAt: number;
  hasEdits: boolean; isMissing: boolean;
}): Record<string, unknown> {
  return {
    id: p.id, fileName: p.fileName, filePath: p.filePath, format: p.format,
    camera: p.camera, rating: p.rating, pick: p.pick, capturedAt: p.capturedAt,
    hasEdits: p.hasEdits, isMissing: p.isMissing,
  };
}

// ---------- 各 op 实现 ----------

async function opStatus(): Promise<Record<string, unknown>> {
  const info = await window.lumina.getInfo();
  const dev = useDevelop.getState();
  const lib = useLibrary.getState();
  return {
    app: 'LUMINA',
    module: useUi.getState().view,
    engine: { mode: info.isMock ? 'mock' : 'real', enginePath: info.enginePath, engineInfo: info.engineInfo },
    homeDir: info.homeDir,
    currentPhoto: dev.photoId,
    developSessionId: dev.sessionId,
    revision: dev.revision,
    libraryCurrentPhotoId: lib.currentPhotoId,
    librarySelectedIds: lib.selectedIds,
  };
}

async function opListPhotos(params: Record<string, unknown>): Promise<Record<string, unknown>> {
  const offset = Math.max(0, Math.trunc(params.offset !== undefined ? requireNumber(params.offset, 'offset') : 0));
  const limit = clamp(Math.trunc(params.limit !== undefined ? requireNumber(params.limit, 'limit') : 50), 1, 200);
  const page = { offset, limit };
  const res = typeof params.searchText === 'string' && params.searchText.trim() !== ''
    ? await api.library.search(params.searchText.trim(), page)
    : await api.library.queryPhotos(undefined, undefined, page);
  return { total: res.total, offset, limit, items: res.items.map(slimPhoto) };
}

async function opOpenPhoto(params: Record<string, unknown>): Promise<Record<string, unknown>> {
  const photoId = Math.trunc(requireNumber(params.photoId, 'photoId'));
  // 先验证存在性：openPhoto 失败只 toast，MCP 侧需要明确错误
  try {
    await api.library.getPhotoDetail(photoId);
  } catch (err) {
    throw new McpOpError(`photoId=${photoId} 不存在或不可访问：${errText(err)}`);
  }
  useLibrary.getState().setCurrent(photoId);
  await useDevelop.getState().openPhoto(photoId);
  const dev = useDevelop.getState();
  if (dev.sessionId === null) throw new McpOpError(`photoId=${photoId} 会话建立失败（解码失败或引擎不可用）`);
  useUi.getState().navigate('develop');
  return { photoId: dev.photoId, sessionId: dev.sessionId, revision: dev.revision, view: 'develop' };
}

async function opGetDevelopParams(): Promise<Record<string, unknown>> {
  const { photoId, sessionId, params } = requireDevelopSession();
  return {
    photoId,
    sessionId,
    revision: useDevelop.getState().revision,
    params,
    temperatureKelvin: typeof params.temperature === 'number' ? temperatureToKelvin(params.temperature) : null,
  };
}

async function opSetDevelopParams(rawParams: Record<string, unknown>): Promise<Record<string, unknown>> {
  requireDevelopSession(); // 校验会话存在（白名单校验不依赖其返回值）
  if (rawParams.edits === undefined || rawParams.edits === null || typeof rawParams.edits !== 'object' || Array.isArray(rawParams.edits)) {
    throw new McpOpError('参数 edits 必须是非空对象（如 {"edits":{"exposure":0.5}}）');
  }
  const edits = rawParams.edits as Record<string, unknown>;
  const unknown = Object.keys(edits).filter((k) => !PARAM_KEYS.has(k));
  if (unknown.length > 0) {
    throw new McpOpError(`未知参数键: ${unknown.join(', ')}（合法顶层键: ${[...PARAM_KEYS].join(', ')}）`);
  }
  const patch: Record<string, unknown> = { ...edits };
  // LR 语义对齐（developStore.setNumeric 同款）：手动动 temperature/tint → whiteBalance 升 custom
  const dev = useDevelop.getState();
  if ((patch.temperature !== undefined || patch.tint !== undefined) && dev.params && dev.params.whiteBalance !== 'custom') {
    patch.whiteBalance = 'custom';
  }
  await useDevelop.getState().applyPatch(patch as Partial<EditParams>, true, 'MCP 调整');
  const after = useDevelop.getState();
  return { photoId: after.photoId, revision: after.revision, params: after.params };
}

async function opAddMask(rawParams: Record<string, unknown>): Promise<Record<string, unknown>> {
  requireDevelopSession();
  const type = rawParams.type;
  if (typeof type !== 'string' || !MASK_TYPES.has(type)) {
    throw new McpOpError(`蒙版 type 必须是 linear | radial | brush（收到: ${String(type)}）`);
  }
  await useDevelop.getState().addMask(type as 'linear' | 'radial' | 'brush');
  const maskId = useDevelop.getState().selectedMaskId;
  if (maskId === null) throw new McpOpError('蒙版创建失败（selectedMaskId 为空）');

  const patch: Partial<MaskLayer> = {};
  if (rawParams.amount !== undefined) {
    patch.amount = clamp(requireNumber(rawParams.amount, 'amount'), -100, 100);
  }
  if (rawParams.local !== undefined) {
    if (typeof rawParams.local !== 'object' || rawParams.local === null) throw new McpOpError('参数 local 必须是对象');
    const localIn = rawParams.local as Record<string, unknown>;
    const unknown = Object.keys(localIn).filter((k) => !MASK_LOCAL_KEYS.has(k));
    if (unknown.length > 0) {
      throw new McpOpError(`未知蒙版局部参数键: ${unknown.join(', ')}（合法键: ${[...MASK_LOCAL_KEYS].join(', ')}）`);
    }
    for (const [k, v] of Object.entries(localIn)) {
      if (typeof v !== 'number' || !Number.isFinite(v)) throw new McpOpError(`蒙版局部参数 ${k} 必须是数字`);
    }
    patch.local = { ...useDevelop.getState().params!.masks.find((m) => m.id === maskId)!.local, ...localIn } as MaskLayer['local'];
  }
  if (rawParams.geometry !== undefined) {
    if (typeof rawParams.geometry !== 'object' || rawParams.geometry === null) throw new McpOpError('参数 geometry 必须是对象');
    patch.geometry = rawParams.geometry as MaskLayer['geometry'];
  }
  if (Object.keys(patch).length > 0) {
    await useDevelop.getState().updateMask(maskId, patch);
  }
  const mask = useDevelop.getState().params!.masks.find((m) => m.id === maskId) ?? null;
  return { maskId, mask };
}

async function opCreateSnapshot(rawParams: Record<string, unknown>): Promise<Record<string, unknown>> {
  const { photoId } = requireDevelopSession();
  const name = typeof rawParams.name === 'string' && rawParams.name.trim() !== '' ? rawParams.name.trim() : 'MCP 快照';
  const res = await api.edit.createSnapshot(photoId, name);
  return { snapshotId: res.snapshotId, name, photoId };
}

async function opRestoreSnapshot(rawParams: Record<string, unknown>): Promise<Record<string, unknown>> {
  const { photoId } = requireDevelopSession();
  const snapshotId = Math.trunc(requireNumber(rawParams.snapshotId, 'snapshotId'));
  const res = await api.edit.restoreSnapshot(photoId, snapshotId);
  const normalized = normalizeParams(res.params);
  await useDevelop.getState().applyPatch(normalized, true, 'MCP 恢复快照');
  return { photoId, snapshotId, revision: useDevelop.getState().revision };
}

async function opExportPhoto(rawParams: Record<string, unknown>): Promise<Record<string, unknown>> {
  const dev = useDevelop.getState();
  const photoId = rawParams.photoId !== undefined
    ? Math.trunc(requireNumber(rawParams.photoId, 'photoId'))
    : (dev.photoId ?? (() => { throw new McpOpError('未指定 photoId 且当前无打开的修图会话'); })());
  if (typeof rawParams.outPath !== 'string' || rawParams.outPath.trim() === '') {
    throw new McpOpError('参数 outPath 必填（导出目录，或以扩展名结尾的单文件路径）');
  }
  const outPath = rawParams.outPath.trim().replace(/[\\/]+$/, '');
  const format = (rawParams.format !== undefined ? String(rawParams.format).toLowerCase() : 'jpg');
  if (!['jpg', 'tiff', 'png', 'psd'].includes(format)) {
    throw new McpOpError(`format 必须是 jpg | tiff | png | psd（收到: ${format}）`);
  }
  const quality = rawParams.quality !== undefined ? clamp(Math.trunc(requireNumber(rawParams.quality, 'quality')), 1, 100) : 90;
  const longEdge = rawParams.longEdge !== undefined ? clamp(Math.trunc(requireNumber(rawParams.longEdge, 'longEdge')), 256, 16384) : 4096;

  // outPath 语义：无扩展名 → 目录；有扩展名 → 单文件名（去掉扩展名当命名模板）
  const base = outPath.split(/[\\/]/).pop() ?? '';
  const isFile = /\.[A-Za-z0-9]+$/.test(base);
  const outputDir = isFile ? outPath.slice(0, outPath.length - base.length - 1) : outPath;
  const namingTemplate = isFile ? base.replace(/\.[^.]+$/, '') : '{date}_{name}_{seq}';

  const spec = {
    format: format as 'jpg' | 'tiff' | 'png' | 'psd',
    longEdge,
    colorSpace: 'sRGB' as const,
    quality,
    psdKeepLayers: false,
    watermark: { enabled: false, position: 'bottom-right', scale: 0.88, opacity: 60 },
    namingTemplate,
    outputDir,
  };
  const { jobId } = await api.exportApi.createJob([photoId], spec);

  // 轮询任务态直到 done/failed（默认上限 120s；engine export 是异步 job，必须等结果）
  const timeoutMs = rawParams.timeoutMs !== undefined ? clamp(requireNumber(rawParams.timeoutMs, 'timeoutMs'), 5_000, 300_000) : 120_000;
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 600));
    const jobs = await api.exportApi.listJobs();
    const job = jobs.find((j) => j.jobId === jobId);
    if (!job) throw new McpOpError(`导出任务 ${jobId} 从队列消失（引擎可能重启）`);
    if (job.state === 'done') {
      return { jobId, state: 'done', outputDir, namingTemplate, format, photoId, detail: job.detail };
    }
    if (job.state === 'failed') {
      throw new McpOpError(`导出任务 ${jobId} 失败：${job.detail || '引擎未给出原因'}`);
    }
    if (job.state === 'cancelled') {
      throw new McpOpError(`导出任务 ${jobId} 被取消`);
    }
  }
  throw new McpOpError(`导出任务 ${jobId} 在 ${Math.round(timeoutMs / 1000)}s 内未完成（仍在队列/执行中），可稍后用 lumina_status 或导出队列查看`);
}

async function opSwitchModule(rawParams: Record<string, unknown>): Promise<Record<string, unknown>> {
  const module = rawParams.module;
  if (typeof module !== 'string' || !MODULE_VIEWS.includes(module as AppView)) {
    throw new McpOpError(`module 必须是 ${MODULE_VIEWS.join(' | ')}（收到: ${String(module)}）`);
  }
  useUi.getState().navigate(module as AppView);
  return { view: useUi.getState().view };
}

// ---------- 分发与接线 ----------

const OPS: Record<string, (params: Record<string, unknown>) => Promise<Record<string, unknown>>> = {
  lumina_status: () => opStatus(),
  list_photos: (p) => opListPhotos(p),
  open_photo: (p) => opOpenPhoto(p),
  get_develop_params: () => opGetDevelopParams(),
  set_develop_params: (p) => opSetDevelopParams(p),
  add_mask: (p) => opAddMask(p),
  create_snapshot: (p) => opCreateSnapshot(p),
  restore_snapshot: (p) => opRestoreSnapshot(p),
  export_photo: (p) => opExportPhoto(p),
  switch_module: (p) => opSwitchModule(p),
};

async function dispatch(op: string, params: Record<string, unknown>): Promise<Record<string, unknown>> {
  const handler = OPS[op];
  if (!handler) {
    throw new McpOpError(`未知 op: ${op}（可用: ${Object.keys(OPS).join(', ')}）`);
  }
  return handler(params ?? {});
}

async function handleMcpCall(req: { reqId: number; op: string; params?: unknown }): Promise<void> {
  try {
    const result = await dispatch(req.op, (req.params ?? {}) as Record<string, unknown>);
    window.lumina.resolveMcpCall({ reqId: req.reqId, ok: true, result });
  } catch (err) {
    window.lumina.resolveMcpCall({
      reqId: req.reqId,
      ok: false,
      error: err instanceof McpOpError ? err.message : `${req.op} 执行失败: ${errText(err)}`,
    });
  }
}

/** 唯一挂载入口（app/src/main.tsx 调用一次；幂等） */
let mounted = false;
export function initMcpExecutor(): void {
  if (mounted) return;
  mounted = true;
  window.lumina.onMcpCall((req) => void handleMcpCall(req));
  console.log('[mcp-executor] mounted, ops =', Object.keys(OPS).join(','));
}
