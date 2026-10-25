/**
 * T1.4 · 三键缓存键（JS 侧，架构 §5.3 / engine plan/cache_key.cpp 分桶口径同步）。
 *
 * 分桶与引擎完全一致：
 *  - kGeom   geometry.* / lens.* / optics.manualDistortion / optics.lensProfileEnable
 *  - kTone   basic 曝光系（exposure/contrast/highlights/shadows/whites/blacks/dehaze/whiteBalance）
 *            + calibration.* + detail NR（colorNR/colorNRDetail/luminanceSmoothing）+ icc.*
 *  - kDisplay 其余（含 detail.sharpness 系、curve/hsl/colorGrade/optics 效果等）
 * 层级：kTone = H(kGeom键 || kTone桶参数)，kDisplay = H(kTone键 || kDisplay桶参数)。
 *
 * 偏差（入档）：引擎哈希为 md5；JS 侧用 FNV-1a 32bit（桶口径一致，哈希算法不跨端
 * 互认——缓存一致性只在各自进程内要求，见 T1.4 回报偏差⑤）。
 */

export type CacheBucket = 'kGeom' | 'kTone' | 'kDisplay';
export const CACHE_BUCKETS: CacheBucket[] = ['kGeom', 'kTone', 'kDisplay'];

function isOneOf(path: string, names: string[]): boolean {
  return names.includes(path);
}

/** 参数路径 → 缓存桶（与 engine cache_key.cpp bucketForParam 逐条对齐） */
export function bucketForParam(path: string): CacheBucket {
  if (
    path.startsWith('geometry.') ||
    path.startsWith('lens.') ||
    isOneOf(path, ['optics.manualDistortion', 'optics.lensProfileEnable'])
  ) {
    return 'kGeom';
  }
  if (
    isOneOf(path, [
      'basic.exposure',
      'basic.contrast',
      'basic.highlights',
      'basic.shadows',
      'basic.whites',
      'basic.blacks',
      'basic.dehaze',
      'basic.whiteBalance',
    ]) ||
    path.startsWith('calibration.') ||
    isOneOf(path, [
      'detail.colorNR',
      'detail.colorNRDetail',
      'detail.luminanceSmoothing',
    ]) ||
    path.startsWith('icc.')
  ) {
    return 'kTone';
  }
  return 'kDisplay';
}

function fnv1a(text: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < text.length; i++) {
    h ^= text.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h.toString(16).padStart(8, '0');
}

/**
 * 层级三键。params：叶路径 → 归一化文本值（与 compilePlan 入参同口径）。
 * 键结构镜像引擎：kGeom = H(kGeom桶)，kTone = H(kGeom键+kTone桶)，kDisplay = H(kTone键+kDisplay桶)。
 */
export function computeCacheKeys(params: [string, string][]): Record<CacheBucket, string> {
  const byBucket: Record<CacheBucket, string[]> = { kGeom: [], kTone: [], kDisplay: [] };
  for (const [path, value] of params) {
    byBucket[bucketForParam(path)].push(`${path}=${value}`);
  }
  // 桶内排序保证键对参数顺序稳定（引擎按插入序，此处保守排序并注明）
  for (const b of CACHE_BUCKETS) byBucket[b].sort();
  const keys: Record<CacheBucket, string> = { kGeom: '', kTone: '', kDisplay: '' };
  keys.kGeom = fnv1a(byBucket.kGeom.join('|'));
  keys.kTone = fnv1a(keys.kGeom + '|' + byBucket.kTone.join('|'));
  keys.kDisplay = fnv1a(keys.kTone + '|' + byBucket.kDisplay.join('|'));
  return keys;
}
