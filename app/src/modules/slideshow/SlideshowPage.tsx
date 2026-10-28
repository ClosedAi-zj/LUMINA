/**
 * 幻灯片放映（补齐 v1.2.0 阶段标记为「占位」的模块）。
 *
 * 设计取舍：
 * - 画面区固定 #08090B：与其它照片观察区（查看器/地图/延时）同一口径，浅色主题下也不翻转。
 * - 低资源：只用 <img> 呈现已缓存的预览，预取前方 2 张；标签页切到后台时暂停计时器，
 *   不做任何滤镜/过渡合成开销（CSS transition 由浏览器合成器处理，不占主线程）。
 * - 零静默：空序列、预览失败、序列被截断（只加载到一部分）都必须显式告知，不许留空画面。
 */

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { fileUrl } from '../../api/rpc-client';
import { Button } from '../../components/primitives';
import { useLibrary } from '../../store/libraryStore';
import { useUi } from '../../store/uiStore';
import type { PhotoSummary } from '../../api/protocol';
import { SLIDE_SIZE, useSlideshow } from './slideshowStore';

const INTERVAL_CHOICES = [2000, 4000, 8000, 15000] as const;
const PRELOAD_AHEAD = 2;
const CONTROLS_HIDE_MS = 2500;

export default function SlideshowPage() {
  const navigate = useUi((s) => s.navigate);
  const pages = useLibrary((s) => s.pages);
  const selectedIds = useLibrary((s) => s.selectedIds);
  const total = useLibrary((s) => s.total);

  const urls = useSlideshow((s) => s.urls);
  const failures = useSlideshow((s) => s.failures);
  const ensure = useSlideshow((s) => s.ensure);
  const retry = useSlideshow((s) => s.retry);

  const [index, setIndex] = useState(0);
  const [playing, setPlaying] = useState(true);
  const [intervalMs, setIntervalMs] = useState<number>(4000);
  const [controlsVisible, setControlsVisible] = useState(true);
  const hideTimer = useRef<number | null>(null);

  /** 放映序列：图库里选了 ≥2 张就只放映选中的；否则放映当前已加载的全部。 */
  const photos = useMemo(() => {
    const all: PhotoSummary[] = [];
    for (const key of Object.keys(pages).map(Number).sort((a, b) => a - b)) {
      all.push(...(pages[key] ?? []));
    }
    const seen = new Set<number>();
    const deduped = all.filter((p) => (seen.has(p.id) ? false : (seen.add(p.id), true)));
    if (selectedIds.length >= 2) {
      const picked = deduped.filter((p) => selectedIds.includes(p.id));
      if (picked.length > 0) return picked;
      // 选中的图不在已加载分页里（比如筛选后被翻走）：退回全序列并交给下面的截断提示说明。
    }
    return deduped;
  }, [pages, selectedIds]);

  const count = photos.length;
  /** 已加载分页可能与筛选总量不一致：必须说明放映范围，避免用户以为放映了全部。 */
  const allLoaded = useMemo(() => {
    const seen = new Set<number>();
    for (const key of Object.keys(pages)) for (const p of pages[Number(key)] ?? []) seen.add(p.id);
    return seen.size;
  }, [pages]);
  const usingSelection = selectedIds.length >= 2 && photos.length !== allLoaded;
  const truncated = !usingSelection && typeof total === 'number' && total > 0 && allLoaded < total;

  // clamp：序列变短后 index 可能越界
  useEffect(() => {
    if (count > 0 && index >= count) setIndex(0);
  }, [count, index]);

  const preloadIds = useMemo(
    () => photos.slice(index, index + 1 + PRELOAD_AHEAD).map((p) => p.id),
    [photos, index],
  );
  useEffect(() => {
    void ensure(preloadIds);
  }, [preloadIds, ensure]);

  const go = useCallback(
    (delta: number) => {
      setIndex((i) => (count === 0 ? 0 : (i + delta + count) % count));
    },
    [count],
  );

  // 自动放映
  useEffect(() => {
    if (!playing || count < 2) return;
    const timer = window.setTimeout(() => go(1), intervalMs);
    return () => window.clearTimeout(timer);
  }, [playing, count, intervalMs, index, go]);

  // 后台标签页暂停：不浪费一次解码与一次渲染
  useEffect(() => {
    const onVisibility = () => {
      if (document.hidden) return;
      if (hideTimer.current !== null) window.clearTimeout(hideTimer.current);
      setControlsVisible(true);
    };
    document.addEventListener('visibilitychange', onVisibility);
    return () => document.removeEventListener('visibilitychange', onVisibility);
  }, []);

  const bumpControls = useCallback(() => {
    setControlsVisible(true);
    if (hideTimer.current !== null) window.clearTimeout(hideTimer.current);
    hideTimer.current = window.setTimeout(() => setControlsVisible(false), CONTROLS_HIDE_MS);
  }, []);

  useEffect(() => {
    const tick = () => {
      if (!document.hidden) setControlsVisible(true);
    };
    window.addEventListener('mousemove', tick);
    return () => window.removeEventListener('mousemove', tick);
  }, []);

  // 键盘优先：放映是全屏场景，鼠标操作会打断沉浸感
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      // 输入框里打字时不抢键（与全站 Dialog/开发的约定一致）
      const el = e.target as HTMLElement | null;
      if (el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable)) return;
      if (e.key === 'Escape') {
        navigate('library');
      } else if (e.key === ' ') {
        e.preventDefault();
        setPlaying((p) => !p);
      } else if (e.key === 'ArrowRight') {
        go(1);
      } else if (e.key === 'ArrowLeft') {
        go(-1);
      }
      bumpControls();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [navigate, go, bumpControls]);

  useEffect(() => {
    return () => {
      if (hideTimer.current !== null) window.clearTimeout(hideTimer.current);
    };
  }, []);

  if (count === 0) {
    return (
      <Shell onExit={() => navigate('library')} controlsVisible>
        <div className="h-full flex items-center justify-center px-8">
          <div className="max-w-md text-center">
            <p className="text-sm text-txt font-medium">没有可放映的照片</p>
            <p className="mt-2 text-xs text-txt3 leading-relaxed">
              先在「图库」查看照片，或在图库里选中至少两张再回到本模块 —— 放映序列取自当前图库的结果；
              选中两张及以上时只放映选中的那些。
            </p>
            <div className="mt-4 flex justify-center">
              <Button variant="primary" size="md" onClick={() => navigate('library')}>
                回到图库
              </Button>
            </div>
          </div>
        </div>
      </Shell>
    );
  }

  const photo = photos[index];
  const url = urls[photo.id];
  const failure = failures[photo.id];

  return (
    <Shell
      onExit={() => navigate('library')}
      controlsVisible={controlsVisible}
      overlay={
        <>
          <div className="flex items-center gap-3 min-w-0">
            <span className="text-xs text-txt truncate">{photo.fileName}</span>
            <span className="text-2xs text-txt3 shrink-0">
              {index + 1} / {count}
            </span>
            {photo.width > 0 && (
              <span className="text-2xs text-txt3 shrink-0 hidden sm:inline">
                {photo.width}×{photo.height}
              </span>
            )}
            {usingSelection && <span className="text-2xs text-txt3 shrink-0">仅放映已选中的 {count} 张</span>}
            {truncated && (
              <span className="text-2xs text-txt3 shrink-0">
                已加载 {allLoaded} / 共 {total} 张 —— 滚动图库加载更多即可纳入放映
              </span>
            )}
          </div>
          <div className="flex items-center gap-1.5 shrink-0">
            <Button size="sm" variant="ghost" onClick={() => go(-1)} aria-label="上一张" title="← 上一张">
              ←
            </Button>
            <Button size="sm" variant={playing ? 'primary' : 'default'} onClick={() => setPlaying((p) => !p)} title="空格 播放/暂停">
              {playing ? '暂停' : '播放'}
            </Button>
            <Button size="sm" variant="ghost" onClick={() => go(1)} aria-label="下一张" title="→ 下一张">
              →
            </Button>
            <select
              className="h-6 text-2xs rounded-md border border-line bg-panel text-txt px-1.5 outline-none"
              value={intervalMs}
              onChange={(e) => setIntervalMs(Number(e.target.value))}
              title="每张停留时间"
            >
              {INTERVAL_CHOICES.map((ms) => (
                <option key={ms} value={ms}>
                  {ms / 1000}s
                </option>
              ))}
            </select>
          </div>
        </>
      }
    >
      {failure && !url ? (
        <FailureNote message={failure} onRetry={() => retry(photo.id)} onSkip={() => go(1)} />
      ) : url ? (
        <img
          key={photo.id}
          src={fileUrl(url)}
          alt={photo.fileName}
          className="max-w-full max-h-full object-contain"
          draggable={false}
        />
      ) : (
        <p className="text-xs text-txt3">正在准备预览（长边 {SLIDE_SIZE}px）…</p>
      )}
    </Shell>
  );
}

/** 放映外壳：画面区固定 #08090B（照片观察区不随主题翻转），控制台悬浮且可自动隐藏。 */
function Shell({
  children,
  overlay,
  onExit,
  controlsVisible,
}: {
  children: React.ReactNode;
  overlay?: React.ReactNode;
  onExit: () => void;
  controlsVisible: boolean;
}) {
  return (
    <div
      className="relative h-full w-full overflow-hidden"
      style={{ background: '#08090B' }}
    >
      <div className="absolute inset-0 flex items-center justify-center p-6">{children}</div>

      <div
        className={`absolute inset-x-0 top-0 flex items-center justify-between gap-4 px-4 py-3 transition-opacity duration-200 ${
          controlsVisible ? 'opacity-100' : 'opacity-0 pointer-events-none'
        }`}
        style={{ background: 'linear-gradient(180deg, rgba(8,9,11,0.82), rgba(8,9,11,0))' }}
      >
        {overlay ?? <span />}
        <Button size="sm" variant="ghost" onClick={onExit} className="ml-2 shrink-0" title="Esc 退出放映">
          退出
        </Button>
      </div>
    </div>
  );
}

function FailureNote({ message, onRetry, onSkip }: { message: string; onRetry: () => void; onSkip: () => void }) {
  return (
    <div className="max-w-md rounded-lgC border border-line bg-panel px-5 py-4">
      <p className="text-sm text-txt font-medium">这张的预览没能生成</p>
      <p className="mt-2 text-xs text-txt3 leading-relaxed">{message}</p>
      <div className="mt-4 flex gap-2">
        <Button size="sm" variant="primary" onClick={onRetry}>
          重试
        </Button>
        <Button size="sm" variant="ghost" onClick={onSkip}>
          跳过这张
        </Button>
      </div>
    </div>
  );
}
