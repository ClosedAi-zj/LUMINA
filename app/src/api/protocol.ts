/**
 * LUMINA IPC 协议 TS 镜像（docs/ipc-protocol.md §2–§6 的唯一 TS 契约）。
 * 字段名与协议文档逐字一致；与 engine/src/rpc/version.h 由 CI 对齐 PROTOCOL_VERSION。
 *
 * 版本口径（M20/T2.5）：PROTOCOL_VERSION 维持 '1.0'（主版本不变）——T2.5 蒙版协议 v3
 * 是**纯增量字段**（masks[*].combine 新增 / optics.chromaticAberration 叶 #94 新增 /
 * kXmpSchemaVersion 2→3），不破坏既有消费方，故不升协议主版本；只有破坏性变更
 * （字段改名/删除/语义翻转）才需要升位并同步 engine/src/rpc/version.h。
 */

export const PROTOCOL_VERSION = '1.0';

// ---------- 通用类型（协议 §4） ----------

export type PhotoFormat = 'RW2' | 'JPG' | 'DNG' | 'ARW' | 'MP4' | 'PSD' | 'PNG';
export type PickFlag = 'pick' | 'reject' | 'none';
export type Rating = 0 | 1 | 2 | 3 | 4 | 5;
export type ThumbState = 'ready' | 'pending' | 'failed';
export type PairKind = 'raw+jpg' | 'single';
export type HealthState = 'ok' | 'missing' | 'corrupt' | 'unchecked';

export interface PhotoSummary {
  id: number;
  fileName: string;
  filePath: string;
  format: PhotoFormat;
  width: number;
  height: number;
  capturedAt: number;
  importedAt: number;
  camera: string;
  lens: string;
  iso: number;
  aperture: string;
  shutter: string;
  focalLength: string;
  gpsLat: number | null;
  gpsLng: number | null;
  rating: Rating;
  pick: PickFlag;
  colorLabel: string | null;
  hasEdits: boolean;
  hasGps: boolean;
  isMissing: boolean;
  isVideo: boolean;
  pairKind: PairKind;
  thumbState: ThumbState;
  thumbPath: string | null;
  seqId: number | null;
}

export interface PhotoDetail extends PhotoSummary {
  md5: string | null;
  fileSize: number;
  sidecarPath: string | null;
  sidecarMtime: number | null;
  whiteBalance: 'asShot' | 'custom';
  folderId: number;
  rootId: number;
  keywords: string[];
  description: string;
  seqInfo: { seqId: number; name: string; frameNo: number; frameCount: number } | null;
  health: HealthState;
  backup: { backedUp: boolean; backupPath: string | null; verifiedAt: number | null };
}

export interface Filter {
  folderIds?: number[];
  rootIds?: number[];
  formats?: PhotoFormat[];
  cameras?: string[];
  lenses?: string[];
  minRating?: 1 | 2 | 3 | 4 | 5;
  pick?: PickFlag;
  located?: boolean;
  edited?: boolean;
  missing?: boolean;
  isVideo?: boolean;
  dateFrom?: number;
  dateTo?: number;
  locationIds?: number[];
  seqId?: number;
}

export interface Sort {
  field: 'capturedAt' | 'importedAt' | 'fileName' | 'rating';
  order: 'asc' | 'desc';
}

export interface Page {
  offset: number;
  limit: number;
}

export interface QueryResult {
  total: number;
  items: PhotoSummary[];
}

export interface FolderNode {
  id: number;
  rootId: number;
  path: string;
  name: string;
  count: number;
  missing: number;
  children: FolderNode[];
}

// ---------- 编辑参数（协议 §4 EditParams） ----------

export interface CurvePoint {
  x: number;
  y: number;
}

export interface Hsl {
  hue: number;
  sat: number;
  lum: number;
}

/** 混色器 8 色带（1.1.0 新增 orange；顺序 = 色相环 0/30/60/120/180/240/280/320°） */
export type HslBand = 'red' | 'orange' | 'yellow' | 'green' | 'aqua' | 'blue' | 'purple' | 'magenta';

/**
 * 蒙版布尔组合方式（T2.5 协议 v3，架构 §3.1.3 fold）。
 * fold 只依赖「数组序 + combine」：首条固定按 replace；
 *   replace   = 直接采用本蒙版系数
 *   intersect = 与前序结果相乘（交集）
 *   subtract  = 前序结果 × (1 - 本蒙版系数)（只减面积，不施加本蒙版 local）
 * 必填：UI 构造端契约——新蒙版必须显式声明组合方式（buildMask 工厂统一写 'replace'）。
 * 引擎读端对缺失该字段的蒙版（v1/v2 存量 sidecar）一律按 "replace" 处理；
 * 渲染层读入侧兜底见 developStore.normalizeParams（masks 逐项补 replace）。
 * 非法串引擎按 replace + graph 层校验兜底。
 */
export type MaskCombine = 'replace' | 'intersect' | 'subtract';

export interface MaskLayer {
  id: string;
  type: 'linear' | 'radial' | 'brush' | 'aiSky' | 'aiSubject' | 'aiBackground' | 'luminanceRange' | 'colorRange' | 'depth';
  enabled: boolean;
  opacity: number; // 0..100
  /**
   * 强度 -100..100（v3 口径：缺省 100，引擎写端 ==100 时省略该键）。
   * <0 = 局部调整反向（≠反相蒙版，架构 §3.1.3）。可选：UI 读引擎回传时用 ?? 100 兜底。
   */
  amount?: number;
  /** v3 新增（必填，见 MaskCombine 注释）；v2 存量读入侧按 replace 兜底 */
  combine: MaskCombine;
  geometry?: { x1: number; y1: number; x2: number; y2: number } | { cx: number; cy: number; radius: number; ratio: number };
  strokes?: { x: number; y: number; radius: number; flow: number; density: number; erase: boolean }[];
  range?: { low: number; high: number; samples: number[] };
  local: {
    exposure: number;
    contrast: number;
    highlights: number;
    shadows: number;
    temperature: number;
    tint: number;
    saturation: number;
    clarity: number;
    dehaze: number;
  };
}

export interface RemovalArea {
  id: string;
  rect: { x: number; y: number; w: number; h: number };
  strokes: { x: number; y: number; radius: number }[];
}

export interface AdjustmentLayer {
  id: string;
  name: string;
  kind: string;
  enabled: boolean;
  blendMode: 'normal' | 'multiply' | 'screen' | 'overlay' | string;
  opacity: number;
  params: Partial<EditParams>;
}

export interface EditParams {
  processVersion: '15.4';
  whiteBalance: 'asShot' | 'custom';
  temperature?: number;
  tint?: number;
  exposure: number; // -5.0..+5.0 EV
  contrast: number; // -100..100
  highlights: number;
  shadows: number;
  whites: number;
  blacks: number;
  clarity: number;
  texture: number;
  dehaze: number;
  vibrance: number;
  saturation: number;
  toneCurveRGB: CurvePoint[];
  toneCurveR?: CurvePoint[];
  toneCurveG?: CurvePoint[];
  toneCurveB?: CurvePoint[];
  parametric?: { highlights: number; lights: number; darks: number; shadows: number; shadowSplit: number; lightSplit: number; midSplit: number };
  hsl: Record<HslBand, Hsl>;
  colorGrade: {
    shadowHue: number;
    shadowSat: number;
    midtoneHue: number;
    midtoneSat: number;
    highlightHue: number;
    highlightSat: number;
    blend: number;
    balance: number;
  };
  calibration: {
    shadowTint: number;
    redHue: number;
    redSat: number;
    greenHue: number;
    greenSat: number;
    blueHue: number;
    blueSat: number;
  };
  detail: {
    sharpness: number;
    sharpenRadius: number;
    sharpenDetail: number;
    sharpenMasking: number;
    luminanceSmoothing: number;
    colorNR: number;
    colorNRDetail: number;
  };
  optics: {
    lensProfileEnable: boolean;
    manualDistortion: number;
    chromaticAberration: number; // 叶 #94（T2.5/Q3）：CA 强度 -100..100，↔ crs:ChromaticAberrationScale
    vignetteAmount: number;
    vignetteMidpoint: number;
    vignetteFeather: number;
    grainAmount: number;
    grainFrequency: number;
    grainSize: number;
  };
  geometry: {
    hasCrop: boolean;
    cropTop?: number;
    cropBottom?: number;
    cropLeft?: number;
    cropRight?: number;
    cropAngle?: number;
    perspective: { vertical: number; horizontal: number; rotate: number; scale: number; aspect: number; x: number; y: number };
  };
  masks: MaskLayer[];
  removals?: RemovalArea[];
  adjustmentStack?: AdjustmentLayer[];
}

export interface JobInfo {
  jobId: number;
  kind: string;
  state: 'queued' | 'running' | 'done' | 'failed' | 'cancelled';
  progress: number; // 0..100
  detail: string;
  etaMs: number | null;
}

// ---------- 各域返回结构（协议 §5） ----------

export interface InitializeResult {
  protocolVersion: string;
  engineVersion: string;
  capabilities: { libraw: boolean; fts5: boolean; codecs: string[] | 'none'; mock: boolean };
  homeDir: string;
  dbPath: string;
}

export interface VolumeInfo {
  path: string;
  label: string;
  totalBytes: number;
  freeBytes: number;
  isRemovable: boolean;
}

export interface LibraryStats {
  totalPhotos: number;
  totalBytes: number;
  verified: number;
  missing: number;
  edited: number;
  byFormat: Record<string, number>;
  months: { month: string; count: number }[];
}

export interface FacetValue {
  value: string;
  count: number;
}

export interface FacetsResult {
  cameras: FacetValue[];
  lenses: FacetValue[];
  formats: FacetValue[];
  ratings: FacetValue[];
  locations: FacetValue[];
}

export interface TimelineBucket {
  month: string;
  count: number;
  firstPhotoId: number;
}

export interface SmartCollection {
  id: number;
  name: string;
  filter: Filter;
}

export interface AlbumInfo {
  id: number;
  name: string;
  kind: 'collection' | 'portfolio';
  itemCount: number;
  coverPhotoId: number;
  published?: boolean;
}

export interface ThumbItem {
  id: number;
  state: ThumbState;
  path?: string;
}

export interface PreviewBase {
  path: string;
  width: number;
  height: number;
  source: 'libraw' | 'embedded-jpeg' | 'native';
}

export interface XmpReadResult {
  params: EditParams;
  /** sidecar schema 版本（T2.5 起引擎新写全为 3；>3 时引擎发 1006 schemaTooNew，走 warnings 展示，UI 不得猜测降级读取） */
  schemaVersion: number;
  processVersion: string;
  /** 引擎逐条告警（含 1006 schemaTooNew、1007 外来命名空间等）；UI 有告警必须展示，不许静默吞掉 */
  warnings: string[];
  sidecarPath: string;
  sidecarMtime: number;
  foreignNamespaces: string[];
}

export interface XmpNamespace {
  prefix: string;
  uri: string;
  fieldCount: number;
}

export interface EditSessionBegin {
  sessionId: number;
  params: EditParams;
  revision: number;
  historyLength: number;
}

export interface EditSessionState {
  params: EditParams;
  revision: number;
  historyIndex?: number;
}

/** 协议 v1.0.1 §5.5：edit.endSession 唯一定义的四字段返回（UI 凭 sidecarMtime 熄灭指示灯，不自行判断） */
export interface EndSessionResult {
  flushed: boolean;
  params: EditParams;
  revision: number;
  sidecarMtime: number | null;
}

/** 协议 v1.0.1 §5.7：raw.selfTest（L1 判定）返回 */
export interface RawSelfTestResult {
  ok: boolean;
  sampled: number;
  succeeded: number;
  decodeMsP50: number;
  decodeMsP95: number;
  failures: { photoId: number; error: string }[];
}

/** 协议 §5.15：mock.generate 完成事件 result */
export interface MockGenerateResult {
  dbPhotos: number;
  imageFiles: number;
  sidecars: number;
  missing: number;
  diskBytes: number;
  durationMs: number;
  dngSelfTest: { ok: boolean; sampled: number; succeeded: number; decodeMsP50: number };
  readmePath: string;
}

export interface SnapshotInfo {
  snapshotId: number;
  name: string;
  createdAt: number;
}

export interface VCopyInfo {
  vcopyId: number;
  name: string;
  createdAt: number;
}

export interface PresetInfo {
  id: number;
  name: string;
  params: EditParams;
}

export interface Histogram {
  r: number[];
  g: number[];
  b: number[];
}

export interface ScanSourceResult {
  totalFiles: number;
  totalBytes: number;
  byFormat: Record<string, number>;
  duplicates: number;
  rawWithJpg: number;
  jpgOnly: number;
  unreadable: { path: string; error: string }[];
  /** @since 1.0 UI 扩展字段：预览条样本文件（引擎可选返回，mock 返回） */
  sampleFiles?: { fileName: string; bytes: number; format: PhotoFormat }[];
}

export interface CheckTargetResult {
  freeBytes: number;
  ratio: number;
  warning: boolean;
}

export interface ImportOptions {
  source: { kind: 'volume' | 'folder'; path: string };
  target: string;
  backup: { enabled: boolean; targetPath: string; timing: 'onImport' | 'nightly' };
  verifyMd5: boolean;
  renameTemplate: string;
  dedupe: 'skip';
  pairRawJpg: true;
  postActions: { enterCulling: boolean; applyPreset: string | null };
}

export interface HealthStatus {
  verified: number;
  total: number;
  missing: number;
  corrupt: number;
  backup: { state: 'ok' | 'warning' | 'none'; lastSuccessAt: number; target: string };
  md5: { percent: number; running: boolean };
  abnormal: boolean;
}

export interface HealthIssueItem {
  issueId: number;
  photoId: number;
  path: string;
  lastKnown: string;
  kind: 'missing' | 'corrupt';
  detectedAt: number;
}

export interface BackupConfig {
  enabled: boolean;
  targetPath: string;
  timing: 'onImport' | 'nightly';
  lastRunAt: number;
}

export interface ExportSpec {
  format: 'jpg' | 'tiff' | 'png' | 'psd';
  longEdge: number;
  colorSpace: 'sRGB' | 'AdobeRGB';
  quality: number;
  psdKeepLayers: boolean;
  watermark: { enabled: boolean; position: string; scale: number; opacity: number; text?: string; imagePath?: string };
  namingTemplate: string;
  outputDir: string;
}

export interface ExportPreset {
  id: number;
  name: string;
  spec: ExportSpec;
}

export interface TimelapseSequenceSummary {
  seqId: number;
  name: string;
  frameCount: number;
  state: 'idle' | 'analyzing' | 'rendering';
  renderProgress: number;
}

export interface RampKeyframe {
  frameNo: number;
  ev: number;
  temperatureK: number;
}

export interface TimelapseSequenceDetail {
  name: string;
  frameCount: number;
  frames: { frameNo: number; photoId: number }[];
  exposureRamp: { keyframes: RampKeyframe[] };
  deflicker: { analyzed: boolean; brightnessGainPct?: number };
  lrtCompatible: boolean;
}

export interface BracketGroup {
  groupId: number;
  photoIds: number[];
  exposureStops: number;
}

export interface MapLocation {
  locationId?: number;
  name: string;
  lat: number;
  lng: number;
  count: number;
  cluster: boolean;
  children?: MapLocation[];
}

export interface LocationDetail {
  name: string;
  lat: number;
  lng: number;
  altitude: number;
  count: number;
  timeSpan: { from: number; to: number };
  favorite: boolean;
  samplePhotoId: number;
}

// map.setPhotoLocation / map.clearPhotoLocation 的写入结果。
// sidecarFailed + warnings 是「零静默」红线的落点：定位可能已进库但 XMP 未同步，
// 此时必须让用户看见，而不是只在日志里留一行。
export interface LocationWriteResult {
  updated: number;
  locationId?: number;
  sidecarFailed: number;
  warnings: string[];
}

export interface PublishResult {
  token: string;
  staticDir: string;
  expiresAt: number;
  previewUrl: string;
}

export interface PublishedGallery {
  albumId: number;
  token: string;
  previewUrl: string;
  expiresAt: number;
  stats: { photos: number; favorites: number; completionRate: number };
}

// ---------- 事件（协议 §6） ----------

export interface JobProgressEvent {
  jobId: number;
  kind: string;
  state: 'running';
  progress: number;
  detail: string;
  etaMs?: number;
  extra?: Record<string, unknown>;
}

export interface JobCompletedEvent {
  jobId: number;
  kind: string;
  state: 'done' | 'failed' | 'cancelled';
  result?: Record<string, unknown>;
  error?: { code: number; message: string; data?: unknown };
}

export interface ThumbReadyEvent {
  photoId: number;
  size: number;
  path: string;
}

export interface FsChangedEvent {
  rootId: number;
  path: string;
  change: 'missing' | 'restored' | 'modified';
}

export interface HealthIssueEvent {
  issueId: number;
  photoId: number;
  kind: 'missing' | 'corrupt';
  path: string;
}

export interface ImportFileResultEvent {
  jobId: number;
  path: string;
  status: 'ok' | 'duplicate' | 'failed';
  error?: string;
  photoId?: number;
}

export interface EngineLogEvent {
  level: string;
  module: string;
  message: string;
}

// ---------- 错误码（协议 §3） ----------

export const RPC_ERRORS = {
  PARSE_ERROR: -32700,
  INVALID_REQUEST: -32600,
  METHOD_NOT_FOUND: -32601,
  INVALID_PARAMS: -32602,
  INTERNAL_ERROR: -32603,
  FILE_NOT_FOUND: 1001,
  IO_ERROR: 1002,
  DB_ERROR: 1003,
  XMP_PARSE_ERROR: 1004,
  XMP_EXTERNAL_CONFLICT: 1005,
  SCHEMA_TOO_NEW: 1006,
  XMP_WRITE_FAILED: 1007,
  RAW_DECODE_FAILED: 2001,
  UNSUPPORTED_FORMAT: 2002,
  PROXY_MISSING: 2003,
  JOB_NOT_FOUND: 3001,
  JOB_BUSY: 3002,
  JOB_CANCELLED: 3003,
  JOB_FAILED: 3004,
  INSUFFICIENT_SPACE: 4001,
  MD5_MISMATCH: 4002,
  SOURCE_UNREADABLE: 4003,
  CODEC_UNAVAILABLE: 5001,
  ENGINE_SHUTTING_DOWN: 5002,
  GALLERY_CONFLICT: 6001,
} as const;

/** 错误码 → 用户可见提示（UI 纪律：错误必须映射为可行动文案，禁止吞错） */
export const ERROR_MESSAGES: Record<number, string> = {
  [-32700]: '数据解析失败，请重试',
  [-32600]: '请求格式不正确',
  [-32601]: '功能暂未实现（方法未注册）',
  [-32602]: '参数校验失败',
  [-32603]: '引擎内部错误，请查看日志',
  [1001]: '文件或目录不存在',
  [1002]: '磁盘读写失败',
  [1003]: '数据库操作失败',
  [1004]: 'XMP sidecar 解析失败，建议隔离该文件',
  [1005]: '文件已被其他工具修改',
  [1006]: '该文件由更高版本软件编辑，部分参数可能不显示',
  [1007]: 'XMP 写入失败（磁盘满或只读）',
  [2001]: 'RAW 解码失败',
  [2002]: '不支持的文件格式',
  [2003]: '内嵌代理提取失败',
  [3001]: '任务不存在',
  [3002]: '同类任务正在运行，请稍后',
  [3003]: '任务已取消',
  [3004]: '任务失败',
  [4001]: '目标磁盘剩余空间不足',
  [4002]: '文件校验失败（MD5 不符），已进入隔离视图',
  [4003]: '源文件不可读',
  [5001]: '编码器不可达',
  [5002]: '引擎正在退出',
  [6001]: '画廊发布冲突（重复发布或密码冲突）',
};
