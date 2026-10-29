/**
 * 领域 API 封装 —— 按协议 15 域分组的类型化请求入口。
 * UI 组件一律经此层调用引擎（真实引擎或 mock），不得散落字符串方法名。
 */

import { rpc } from './rpc-client';
import type {
  AlbumInfo,
  BackupConfig,
  BracketGroup,
  CheckTargetResult,
  EditSessionBegin,
  EditSessionState,
  EditParams,
  EndSessionResult,
  ExportPreset,
  ExportSpec,
  FacetsResult,
  Filter,
  FolderNode,
  HealthIssueItem,
  HealthStatus,
  Histogram,
  InitializeResult,
  JobInfo,
  LibraryStats,
  LocationDetail,
  LocationWriteResult,
  MapLocation,
  MockGenerateResult,
  Page,
  PhotoDetail,
  PhotoSummary,
  PresetInfo,
  PreviewBase,
  PublishResult,
  PublishedGallery,
  QueryResult,
  RawSelfTestResult,
  ScanSourceResult,
  SmartCollection,
  SnapshotInfo,
  Sort,
  StackGroup,
  ThumbItem,
  ThumbSize,
  TimelineBucket,
  TimelapseSequenceDetail,
  TimelapseSequenceSummary,
  TimelapseColorAdjust,
  TimelapseFrameHistogram,
  VCopyInfo,
  VolumeInfo,
} from './protocol';

export const api = {
  system: {
    initialize: (uiVersion: string) => rpc.request<InitializeResult>('system.initialize', { uiVersion }),
    ping: () => rpc.request<{ pong: boolean; uptimeMs: number }>('system.ping'),
    getSettings: () => rpc.request<{ settings: Record<string, unknown> }>('system.getSettings'),
    setSettings: (patch: Record<string, unknown>) => rpc.request<{ settings: Record<string, unknown> }>('system.setSettings', { patch }),
    listVolumes: () => rpc.request<VolumeInfo[]>('system.listVolumes'),
  },
  library: {
    getStats: () => rpc.request<LibraryStats>('library.getStats'),
    queryPhotos: (filter: Filter | undefined, sort: Sort | undefined, page: Page) =>
      rpc.request<QueryResult>('library.queryPhotos', { filter, sort, page }),
    search: (text: string, page: Page) => rpc.request<QueryResult>('library.search', { text, page }),
    getFacets: (filter?: Filter) => rpc.request<FacetsResult>('library.getFacets', { filter }),
    getTimeline: () => rpc.request<TimelineBucket[]>('library.getTimeline'),
    getFolderTree: () => rpc.request<FolderNode[]>('library.getFolderTree'),
    getPhotoDetail: (id: number) => rpc.request<PhotoDetail>('library.getPhotoDetail', { id }),
    updatePhotos: (ids: number[], patch: { rating?: number; pick?: string; colorLabel?: string | null; keywords?: string[]; description?: string }) =>
      rpc.request<{ updated: number }>('library.updatePhotos', { ids, patch }),
    removePhotos: (ids: number[]) => rpc.request<{ removed: number }>('library.removePhotos', { ids }),
    restorePhotos: (ids: number[]) => rpc.request<{ restored: number }>('library.restorePhotos', { ids }),
    listSmartCollections: () => rpc.request<SmartCollection[]>('library.listSmartCollections'),
    runSmartCollection: (id: number, page: Page) => rpc.request<QueryResult>('library.runSmartCollection', { id, page }),
  },
  albums: {
    list: () => rpc.request<AlbumInfo[]>('albums.list'),
    create: (name: string, kind: 'collection' | 'portfolio') => rpc.request<{ id: number }>('albums.create', { name, kind }),
    addPhotos: (albumId: number, ids: number[]) => rpc.request<{ added: number }>('albums.addPhotos', { albumId, ids }),
    removePhotos: (albumId: number, ids: number[]) => rpc.request<{ removed: number }>('albums.removePhotos', { albumId, ids }),
    reorder: (albumId: number, orderedIds: number[]) => rpc.request<Record<string, never>>('albums.reorder', { albumId, orderedIds }),
    getItems: (albumId: number) =>
      rpc.request<{ albumId: number; items: number[]; total: number }>('albums.getItems', { albumId }),
  },
  thumb: {
    ensureBatch: (ids: number[], size: ThumbSize) => rpc.request<{ items: ThumbItem[] }>('thumb.ensureBatch', { ids, size }),
    getStatus: (ids: number[], size: 320 | 640) => rpc.request<{ items: ThumbItem[] }>('thumb.getStatus', { ids, size }),
  },
  preview: {
    getBase: (id: number) => rpc.request<PreviewBase>('preview.getBase', { id }),
  },
  // T1.5：develop.render —— Plan CPU 后端（与导出同一实现；WebGL 降级 / 导出=预览断言）
  develop: {
    // params 省略时引擎读 sidecar（photoId 路径），因此这里可选
    render: (req: { photoId?: number; inputPath?: string; params?: EditParams; longEdge?: number }) =>
      rpc.request<{ path: string; width: number; height: number }>('develop.render', req),
    /**
     * M4-03 软打样预览：返回打样后 PNG 路径 + ΔE00 统计（打样后 vs 打样前）。
     * targetProfile 为空 = 内置演示目标；真实印刷需传入打印机 ICC 文件路径。
     */
    softProofPreview: (req: {
      photoId?: number;
      inputPath?: string;
      params?: EditParams;
      longEdge?: number;
      targetProfile?: string;
      intent?: number;
      simulatePaper?: boolean;
    }) =>
      rpc.request<{
        path: string;
        width: number;
        height: number;
        target: string;
        simulatePaper: boolean;
        deltaE: { mean: number; median: number; p95: number; max: number };
        samples: number;
      }>('softproof.preview', req),
  },
  cache: {
    clear: (scope: 'thumbs' | 'previews' | 'renders') => rpc.request<{ freedBytes: number }>('cache.clear', { scope }),
  },
  xmp: {
    read: (id: number) => rpc.request<import('./protocol').XmpReadResult>('xmp.read', { id }),
    write: (id: number, params: EditParams) => rpc.request<{ sidecarMtime: number }>('xmp.write', { id, params }),
    checkExternal: (id: number) => rpc.request<{ changed: boolean; sidecarMtime: number }>('xmp.checkExternal', { id }),
    listNamespaces: (id: number) => rpc.request<{ prefix: string; uri: string; fieldCount: number }[]>('xmp.listNamespaces', { id }),
    migrationStatus: () => rpc.request<{ current: number; distribution: Record<string, number>; migrated: number }>('xmp.migrationStatus'),
  },
  edit: {
    beginSession: (id: number) => rpc.request<EditSessionBegin>('edit.beginSession', { id }),
    setParams: (sessionId: number, patch: Partial<EditParams>, commitHistory = true) =>
      rpc.request<{ params: EditParams; revision: number }>('edit.setParams', { sessionId, patch, commitHistory }),
    undo: (sessionId: number) => rpc.request<EditSessionState>('edit.undo', { sessionId }),
    redo: (sessionId: number) => rpc.request<EditSessionState>('edit.redo', { sessionId }),
    endSession: (sessionId: number, flush = true) => rpc.request<EndSessionResult>('edit.endSession', { sessionId, flush }),
    getSnapshots: (id: number) => rpc.request<SnapshotInfo[]>('edit.getSnapshots', { id }),
    createSnapshot: (id: number, name: string) => rpc.request<{ snapshotId: number }>('edit.createSnapshot', { id, name }),
    restoreSnapshot: (id: number, snapshotId: number) => rpc.request<{ params: EditParams }>('edit.restoreSnapshot', { id, snapshotId }),
    syncToSelection: (sourceId: number, targetIds: number[], includeMasks: boolean) =>
      rpc.request<{ updated: number }>('edit.syncToSelection', { sourceId, targetIds, includeMasks }),
    syncToSequence: (sourceId: number, seqId: number, includeMasks: boolean) =>
      rpc.request<{ updated: number }>('edit.syncToSequence', { sourceId, seqId, includeMasks }),
  },
  vcopy: {
    create: (id: number, name?: string) => rpc.request<{ vcopyId: number }>('vcopy.create', { id, name }),
    list: (id: number) => rpc.request<VCopyInfo[]>('vcopy.list', { id }),
  },
  presets: {
    list: () => rpc.request<PresetInfo[]>('presets.list'),
    apply: (presetId: number, targetIds: number[]) => rpc.request<{ applied: number }>('presets.apply', { presetId, targetIds }),
  },
  raw: {
    decode: (id: number, scale: 'full' | 'half', purpose: 'preview' | 'export') =>
      rpc.request<{ path: string; width: number; height: number; source: 'libraw' | 'embedded-jpeg'; decodeMsP50?: number }>('raw.decode', { id, scale, purpose }),
    getProxy: (id: number) => rpc.request<{ path: string; width: number; height: number }>('raw.getProxy', { id }),
    selfTest: (sample?: number, ids?: number[]) =>
      rpc.request<RawSelfTestResult>('raw.selfTest', { sample, ids }),
  },
  analysis: {
    histogram: (id: number, edited: boolean) => rpc.request<Histogram>('analysis.histogram', { id, edited }),
    getExposedRange: (id: number) => rpc.request<{ underexposedPct: number; overexposedPct: number }>('analysis.getExposedRange', { id }),
  },
  importApi: {
    scanSource: (source: { kind: 'volume' | 'folder'; path: string }) => rpc.request<{ jobId: number }>('import.scanSource', { source }),
    checkTarget: (targetPath: string, requiredBytes: number) => rpc.request<CheckTargetResult>('import.checkTarget', { targetPath, requiredBytes }),
    start: (opts: Record<string, unknown>) => rpc.request<{ jobId: number }>('import.start', opts),
    cancel: (jobId: number) => rpc.request<Record<string, never>>('import.cancel', { jobId }),
    resolveMissingFolder: (path: string, action: 'relocate' | 'orphan' | 'backfill', newPath?: string) =>
      rpc.request<{ affected: number }>('import.resolveMissingFolder', { path, action, newPath }),
    listRecentSources: () => rpc.request<string[]>('import.listRecentSources'),
  },
  health: {
    getStatus: () => rpc.request<HealthStatus>('health.getStatus'),
    scanFull: () => rpc.request<{ jobId: number }>('health.scanFull'),
    listIssues: (kind: 'missing' | 'corrupt', page: Page) => rpc.request<{ total: number; items: HealthIssueItem[] }>('health.listIssues', { kind, page }),
    resolveIssue: (issueId: number, action: 'relocate' | 'orphan' | 'backfill' | 'restore', newPath?: string) =>
      rpc.request<{ resolved: number }>('health.resolveIssue', { issueId, action, newPath }),
    restoreFromBackup: (photoId: number) => rpc.request<{ jobId: number }>('health.restoreFromBackup', { photoId }),
    verifyPhotos: (ids?: number[]) => rpc.request<{ jobId: number }>('health.verifyPhotos', ids ? { ids } : {}),
    getBackupConfig: () => rpc.request<BackupConfig>('health.getBackupConfig'),
  },
  exportApi: {
    createJob: (photoIds: number[], spec: ExportSpec) => rpc.request<{ jobId: number }>('export.createJob', { photoIds, spec }),
    listJobs: (state?: 'queued' | 'running' | 'done' | 'failed' | 'cancelled') => rpc.request<JobInfo[]>('export.listJobs', state ? { state } : {}),
    cancelJob: (jobId: number) => rpc.request<Record<string, never>>('export.cancelJob', { jobId }),
    retryJob: (jobId: number) => rpc.request<Record<string, never>>('export.retryJob', { jobId }),
    listPresets: () => rpc.request<ExportPreset[]>('export.listPresets'),
    savePreset: (name: string, spec: ExportSpec) => rpc.request<{ id: number }>('export.savePreset', { name, spec }),
  },
  mock: {
    status: () => rpc.request<{ generated: boolean; libraryRoot: string; dbPhotoCount: number; assetFormat?: string; lastSelfTest?: unknown }>('mock.status'),
    generate: () => rpc.request<{ jobId: number }>('mock.generate'),
    simulateExternalEdit: (id: number) => rpc.request<{ ok: boolean }>('mock.debug.simulateExternalEdit', { id }),
    simulateCorruption: (id: number) => rpc.request<{ issueId: number }>('mock.debug.simulateCorruption', { id }),
  },
  // M3-03：PSD 自检 / 外来判定（与导出同一解析口径）
  psd: {
    inspect: (path: string) => rpc.request<Record<string, unknown>>('psd.inspect', { path }),
  },
  map: {
    getLocations: (zoom: number, viewport?: { minLat: number; maxLat: number; minLng: number; maxLng: number }) =>
      rpc.request<MapLocation[]>('map.getLocations', { viewport, zoom }),
    getLocationDetail: (locationId: number) => rpc.request<LocationDetail>('map.getLocationDetail', { locationId }),
    setPhotoLocation: (ids: number[], lat: number, lng: number, altitude?: number, locationName?: string) =>
      rpc.request<LocationWriteResult>('map.setPhotoLocation', { ids, lat, lng, altitude, locationName }),
    clearPhotoLocation: (ids: number[]) => rpc.request<LocationWriteResult>('map.clearPhotoLocation', { ids }),
    listUnlocated: (page: Page) => rpc.request<QueryResult>('map.listUnlocated', page),
    toggleFavorite: (locationId: number, favorite: boolean) => rpc.request<Record<string, never>>('map.toggleFavorite', { locationId, favorite }),
  },
  hdr: {
    detectBracketGroups: () => rpc.request<BracketGroup[]>('hdr.detectBracketGroups', {}),
    composite: (groupId: number, options: { deghost: boolean; outputFormat: 'tiff' | 'dng'; autoAlign: boolean }) =>
      rpc.request<{ jobId: number }>('hdr.composite', { groupId, options }),
  },
  stack: {
    detectStackGroups: () => rpc.request<StackGroup[]>('stack.detectStackGroups', {}),
    composite: (groupId: number, options: { autoAlign: boolean; outputFormat: 'tiff' | 'dng'; smoothness: number }) =>
      rpc.request<{ jobId: number }>('stack.composite', { groupId, options }),
  },
  denoise: {
    apply: (photoId: number, strength: number, luminanceMask: boolean) =>
      rpc.request<{ jobId: number }>('denoise.apply', { photoId, strength, luminanceMask }),
    preview: (photoId: number, strength: number, rect: { x: number; y: number; w: number; h: number }) =>
      rpc.request<{ path: string }>('denoise.preview', { photoId, strength, rect }),
  },
  timelapse: {
    listSequences: () => rpc.request<TimelapseSequenceSummary[]>('timelapse.listSequences'),
    getSequence: (seqId: number) => rpc.request<TimelapseSequenceDetail>('timelapse.getSequence', { seqId }),
    analyzeFlicker: (seqId: number) => rpc.request<{ jobId: number }>('timelapse.analyzeFlicker', { seqId }),
    applyDeflicker: (seqId: number, strength: number) => rpc.request<{ jobId: number }>('timelapse.applyDeflicker', { seqId, strength }),
    setExposureRamp: (seqId: number, keyframes: { frameNo: number; ev: number; temperatureK: number }[]) =>
      rpc.request<Record<string, never>>('timelapse.setExposureRamp', { seqId, keyframes }),
    previewFrame: (seqId: number, frameNo: number, size: 320 | 640 | 1080) =>
      rpc.request<{ path: string }>('timelapse.previewFrame', { seqId, frameNo, size }),
    render: (seqId: number, resolution: '1080p' | '4K' | '6K', fps: 25 | 30 | 60, codec: 'h264' | 'h265' | 'prores', colorAdjust?: TimelapseColorAdjust) =>
      rpc.request<{ jobId: number }>('timelapse.render', { seqId, resolution, fps, codec, ...(colorAdjust ? { colorAdjust } : {}) }),
    /** Phase B：取单帧 R/G/B + 亮度 256 桶直方图（替代客户端 canvas 读像素）。 */
    frameHistogram: (seqId: number, frameNo: number, size?: number) =>
      rpc.request<TimelapseFrameHistogram>('timelapse.frameHistogram', { seqId, frameNo, size }),
    getRenderJobs: () => rpc.request<JobInfo[]>('timelapse.getRenderJobs'),
  },
  gallery: {
    publish: (albumId: number, password: string, title: string, customerName: string, expiryDays = 30) =>
      rpc.request<PublishResult>('gallery.publish', { albumId, password, expiryDays, title, customerName }),
    revoke: (albumId: number) => rpc.request<Record<string, never>>('gallery.revoke', { albumId }),
    listPublished: () => rpc.request<PublishedGallery[]>('gallery.listPublished'),
    importFeedback: (albumId: number, filePath: string) =>
      rpc.request<{ favorites: number; completionRate: number }>('gallery.importFeedback', { albumId, filePath }),
    getFeedback: (albumId: number) =>
      rpc.request<{ favoritesCount: number; completionRate: number; perPhoto: { photoId: number; favorited: boolean }[] }>('gallery.getFeedback', { albumId }),
  },
};
