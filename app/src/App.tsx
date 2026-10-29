/**
 * LUMINA App 根组件：握手引导、模块路由、全局顶栏、事件泵。
 */

import { lazy, Suspense, useCallback, useEffect, useRef, useState } from 'react';
import { ThemeProvider, useTheme } from './theme/ThemeProvider';
import { api } from './api/engine';
import { errText, fileUrl } from './api/rpc-client';
import { useJobStore } from './store/jobStore';
import { startEventPump } from './store/events';
import { useLibrary } from './store/libraryStore';
import { useDebounced } from './api/hooks';
import { useUi, type AppView, type BootStage } from './store/uiStore';
import { ToastHost, Icon, ThemeToggle, SUBBAR_SLOT_ID } from './components/shell';
import { Button } from './components/primitives';
import { MenuBar } from './components/MenuBar';
import { WindowControls } from './components/WindowControls';
import ErrorBoundary from './components/ErrorBoundary';
import BootProgress from './components/BootProgress';
import { formatCount } from './utils/format';

// 代码分割（v1.3.0 性能优化）：12 个页面模块改为 React.lazy 动态加载，
// 首屏只解析「公共代码 + 当前视图」，其余模块在用户导航到对应视图时才按需拉取。
// 改造前整个 App 被 Vite 打进单一初始 chunk（index-*.js ≈ 612KB）；改造后初始 chunk
// 只剩公共依赖，各页面各自成 chunk，首屏解析/求值量显著下降。
const LibraryPage = lazy(() => import('./modules/library/LibraryPage'));
const DevelopPage = lazy(() => import('./modules/develop/DevelopPage'));
const IngestWizard = lazy(() => import('./modules/ingest/IngestWizard'));
const HealthPage = lazy(() => import('./modules/health/HealthPage'));
const CullingPage = lazy(() => import('./modules/culling/CullingPage'));
const MapPage = lazy(() => import('./modules/map/MapPage'));
const ExportPage = lazy(() => import('./modules/export/ExportPage'));
const TimelapsePage = lazy(() => import('./modules/timelapse/TimelapsePage'));
const LandscapePage = lazy(() => import('./modules/landscape/LandscapePage'));
const PortfolioPage = lazy(() => import('./modules/portfolio/PortfolioPage'));
const GalleryPage = lazy(() => import('./modules/gallery/GalleryPage'));
const SlideshowPage = lazy(() => import('./modules/slideshow/SlideshowPage'));

/**
 * v1.2.0 外壳信息架构（UI 迭代 ④）：顶部模块切换对齐 LR 七大模块 tab 形态
 * （图库 / 修改照片 / 地图 / 书籍 / 幻灯片 / 打印 / Web）。
 * LUMINA 定位：综合图像处理软件 —— 风光摄影是"特色模块"（修改照片 ▸ 风光，带特色徽标），
 * 不是唯一身份。映射：
 *   图库     = library(默认) / ingest / culling / health
 *   修改照片 = develop(默认) / landscape(风光·特色) / timelapse
 *   地图     = map
 *   书籍     = 本阶段未实现 → 显式占位，不许静默空白
 *   幻灯片   = slideshow（v1.2.3 补齐：全屏放映，序列取自图库当前结果/多选）
 *   打印     = LUMINA 交付中心（export / portfolio / gallery），LR 中打印/导出同属输出模块
 *   Web      = 本阶段未实现 → 显式占位
 */
type ModuleKey = 'library' | 'develop' | 'map' | 'book' | 'slideshow' | 'print' | 'web';

const MODULES: { key: ModuleKey; label: string; views: AppView[]; defaultView: AppView | null }[] = [
  { key: 'library', label: '图库', views: ['library', 'ingest', 'culling', 'health'], defaultView: 'library' },
  { key: 'develop', label: '修改照片', views: ['develop', 'landscape', 'timelapse'], defaultView: 'develop' },
  { key: 'map', label: '地图', views: ['map'], defaultView: 'map' },
  { key: 'book', label: '书籍', views: [], defaultView: null },
  { key: 'slideshow', label: '幻灯片', views: ['slideshow'], defaultView: 'slideshow' },
  { key: 'print', label: '打印', views: ['export', 'portfolio', 'gallery'], defaultView: 'export' },
  { key: 'web', label: 'Web', views: [], defaultView: null },
];

const VIEW_LABEL: Record<AppView, string> = {
  library: '图库',
  develop: '修改照片',
  ingest: '导入',
  health: '存储健康',
  culling: '精选',
  map: '地图',
  timelapse: '延时',
  landscape: '风光',
  export: '交付',
  portfolio: '作品集',
  gallery: '画廊',
  slideshow: '幻灯片',
};

function moduleOf(view: AppView): ModuleKey {
  const hit = MODULES.find((m) => m.views.includes(view));
  return hit?.key ?? 'library';
}

/** WP-B3：Mock 兜底横幅 —— 引擎没起来时不许装作一切正常（明示 + 恢复入口） */
function MockBanner() {
  const engineStatus = useJobStore((s) => s.engineStatus);
  const [retrying, setRetrying] = useState(false);
  if (engineStatus !== 'mock') return null;
  const retry = async () => {
    setRetrying(true);
    try {
      const res = await window.lumina.engineRetry();
      if (res.mode === 'engine') {
        useJobStore.getState().toast('引擎已连接，正在重新加载工作台…', 'success', 4000);
        setTimeout(() => window.location.reload(), 600);
        return;
      }
      useJobStore.getState().toast(`引擎仍未启动：${res.error ?? '未知原因'}（继续使用演示数据）`, 'error', 8000);
    } catch (err) {
      useJobStore.getState().toast(`重试失败：${errText(err)}`, 'error', 6000);
    } finally {
      setRetrying(false);
    }
  };
  return (
    <div
      className="flex items-center gap-2 px-3 h-8 text-2xs shrink-0"
      style={{ background: 'var(--Lumina-Tokens-accentSoft)', borderBottom: '1px solid var(--Lumina-Tokens-border)' }}
      role="alert"
    >
      <span className="text-accent font-medium">演示模式</span>
      <span className="text-txt2">图像引擎未启动，当前为内置演示数据；修图/导出等写库操作不会作用于真实照片。</span>
      <div className="flex-1" />
      <button className="h-5 px-2 rounded-sm border border-line text-2xs text-txt2 hover:text-txt hover:bg-hover disabled:opacity-50" onClick={() => void retry()} disabled={retrying}>
        {retrying ? '重试中…' : '重试连接引擎'}
      </button>
    </div>
  );
}

/**
 * 标题栏 = macOS 三段式（v1.4.0 UI 重构）
 *
 *   左：产品标识 + PS 式自绘菜单（收成紧凑文字，功能与快捷键 100% 保留）
 *   中：分段导航（macOS SegmentedControl 形态：模块整体收进一枚圆角胶囊，选中项白底浮起）
 *   右：搜索 / 导出 / 主题 + Windows 风格窗口三键（贴最右缘）
 *
 * 模块内子视图（导入/精选/健康、风光/延时、交付/作品集/画廊）单列一条 32px 子导航，
 * 避免把视图胶囊塞进标题栏挤占分段导航的居中位。
 */
function TopBar({
  module,
  onPickModule,
  searchRef,
}: {
  module: ModuleKey;
  onPickModule: (k: ModuleKey) => void;
  searchRef: { current: HTMLInputElement | null };
}) {
  const view = useUi((s) => s.view);
  const navigate = useUi((s) => s.navigate);
  const engineStatus = useJobStore((s) => s.engineStatus);
  const engineVersion = useJobStore((s) => s.engineVersion);
  const healthIssueCount = useJobStore((s) => s.healthIssueCount);
  const stats = useLibrary((s) => s.stats);
  const searchText = useLibrary((s) => s.searchText);
  const { theme } = useTheme();
  const spec = MODULES.find((m) => m.key === module);

  // 顶栏搜索框接真实搜索（P24）：旧实现没有 value/onChange/onKeyDown，是个假控件。
  // 单一真相源仍是 libraryStore.searchText —— 图库内搜索框与本框双向同步同一值。
  const [q, setQ] = useState(searchText);
  const debouncedQ = useDebounced(q, 250);
  useEffect(() => {
    // 外部写入（图库内搜索框 / 清除筛选）→ 同步显示
    setQ(searchText);
  }, [searchText]);
  useEffect(() => {
    if (debouncedQ === useLibrary.getState().searchText) return;
    useLibrary.getState().setSearchText(debouncedQ);
    void useLibrary.getState().refresh();
  }, [debouncedQ]);
  const submitSearch = () => {
    if (useUi.getState().view !== 'library') navigate('library');
    useUi.getState().requestSearchFocus();
  };

  return (
    <>
      <header
        className="app-drag glass-titlebar relative flex items-center h-11 gap-2 border-b border-line shrink-0 select-none"
        data-theme-surface={theme}
        onDoubleClick={(e) => {
          // 拖拽区双击 = 最大化/还原（Windows 惯例）；点在交互元素上不触发
          if ((e.target as HTMLElement).closest('.app-no-drag, button, input')) return;
          void window.lumina.win.toggleMaximize();
        }}
      >
        {/* 左：产品标识 + 全局菜单 */}
        <div className="app-no-drag flex items-center gap-2 pl-3 shrink-0">
          <span className="w-5 h-5 rounded-smC bg-accent flex items-center justify-center text-onaccent text-xs font-bold">L</span>
          <span className="text-sm font-semibold tracking-wide">Lumina</span>
        </div>
        <div className="app-no-drag shrink-0">
          <MenuBar />
        </div>

        {/* 中：分段导航（绝对居中，macOS SegmentedControl） */}
        <div className="app-no-drag absolute left-1/2 -translate-x-1/2 flex items-center">
          <div
            className="flex items-center gap-0.5 p-0.5 rounded-mdC"
            style={{
              background: 'rgba(0, 0, 0, 0.28)',
              border: '1px solid var(--Lumina-Tokens-border)',
              boxShadow: 'inset 0 1px 2px rgba(0, 0, 0, 0.28)',
            }}
            role="tablist"
            aria-label="模块切换"
          >
            {MODULES.map((m) => {
              const active = module === m.key;
              return (
                <button
                  key={m.key}
                  role="tab"
                  aria-selected={active}
                  onClick={() => onPickModule(m.key)}
                  className={`mac-transition h-7 px-3 rounded-[6px] text-xs whitespace-nowrap ${
                    active ? 'text-txt font-semibold' : 'text-txt2 hover:text-txt'
                  }`}
                  style={
                    active
                      ? {
                          background: 'rgba(255, 255, 255, 0.14)',
                          boxShadow: '0 1px 3px rgba(0, 0, 0, 0.4), inset 0 0.5px 0 rgba(255, 255, 255, 0.22)',
                          color: 'var(--Lumina-Tokens-textStrong)',
                        }
                      : undefined
                  }
                >
                  {m.label}
                </button>
              );
            })}
          </div>
        </div>

        {/* 右：引擎状态 + 搜索 + 导出 + 主题 */}
        <div className="ml-auto app-no-drag flex items-center gap-1.5 shrink-0">
          <span className="flex items-center gap-1.5 text-2xs text-txt3 shrink-0 px-1" title={engineStatus === 'mock' ? '演示模式' : `引擎 ${engineVersion || '1.2.3'}`}>
            <span
              className="w-1.5 h-1.5 rounded-full"
              style={{
                background:
                  engineStatus === 'starting'
                    ? 'var(--Lumina-Tokens-textMuted)'
                    : engineStatus === 'mock'
                      ? 'var(--Lumina-Tokens-warning)'
                      : 'var(--Lumina-Tokens-success)',
              }}
            />
            <span className="hidden xl:inline num">
              {stats ? formatCount(stats.totalPhotos) : ''}
            </span>
          </span>
          <div className="flex items-center h-7 gap-1.5 px-2.5 rounded-pill border border-line bg-card" style={{ width: 170 }}>
            <Icon name="search" size={12} className="text-txt3 shrink-0" />
            <input
              ref={(el) => {
                searchRef.current = el;
              }}
              value={q}
              onChange={(e) => setQ(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') {
                  e.preventDefault();
                  submitSearch();
                } else if (e.key === 'Escape') {
                  setQ('');
                  useLibrary.getState().setSearchText('');
                  void useLibrary.getState().refresh();
                  (e.target as HTMLInputElement).blur();
                }
              }}
              placeholder="搜索照片、文件夹、关键词"
              className="flex-1 min-w-0 bg-transparent text-2xs text-txt placeholder:text-txt3 focus:outline-none"
            />
          </div>
          <Button variant="primary" size="sm" onClick={() => navigate('export')}>
            导出
          </Button>
          <ThemeToggle />
        </div>

        {/* 右缘：Windows 风格窗口三键（满高热区，不参与拖拽） */}
        <WindowControls />
      </header>

      {/* 模块内子视图；无视图的模块给显式提示，不留空白。
          v1.4.1：单一视图的模块（地图）不再白占一行；本行同时是页面上下文控件的宿主
          （右侧 #app-subbar-slot，修图台经 SubBarPortal 注入 快速/专业、XMP、左右栏、同步）。 */}
      {spec && spec.views.length !== 1 && (
        <div
          className="app-no-drag flex items-center h-8 gap-1 px-3 shrink-0 border-b border-line overflow-x-auto scrollbar-none"
          style={{ background: 'var(--Lumina-Tokens-browserBar)' }}
        >
          {spec.views.length === 0 ? (
            <span className="text-2xs text-txt3">
              {spec.label}模块：后续里程碑交付，当前版本暂未开放
            </span>
          ) : (
            spec.views.map((v) => {
              const active = view === v;
              const isLandscape = v === 'landscape';
              return (
                <button
                  key={v}
                  onClick={() => navigate(v)}
                  className={`mac-transition relative h-6 px-2.5 rounded-pill text-2xs shrink-0 inline-flex items-center gap-1 border ${
                    active ? 'font-medium' : 'text-txt2 hover:text-txt hover:bg-hover border-transparent'
                  }`}
                  style={
                    active
                      ? {
                          background: 'var(--Lumina-Tokens-accentSoft)',
                          borderColor: 'color-mix(in srgb, var(--Lumina-Tokens-accent) 42%, transparent)',
                          color: 'var(--Lumina-Tokens-accent)',
                        }
                      : undefined
                  }
                >
                  {VIEW_LABEL[v]}
                  {isLandscape && (
                    <span
                      className="px-1 rounded-sm text-[9px] leading-[14px]"
                      style={{
                        background: 'var(--Lumina-Tokens-brandSoft)',
                        color: 'var(--Lumina-Tokens-brand)',
                      }}
                    >
                      特色
                    </span>
                  )}
                  {v === 'health' && healthIssueCount > 0 && (
                    <span className="w-1.5 h-1.5 rounded-full" style={{ background: 'var(--Lumina-Tokens-danger)' }} />
                  )}
                </button>
              );
            })
          )}
          {/* v1.4.1：页面级上下文控件插槽（修图台经 SubBarPortal 注入）。
              宿主常驻不按需创建：portal 需要节点在挂载时就已存在。 */}
          <div id={SUBBAR_SLOT_ID} className="app-no-drag ml-auto flex items-center gap-1.5 min-w-0" />
        </div>
      )}
    </>
  );
}

/** 未实现模块占位（书籍/幻灯片/Web）：必须显式告知（不许静默空白），沿用精修占位式提示 */
const MODULE_META: Partial<Record<ModuleKey, { title: string; desc: string }>> = {
  book: { title: '书籍', desc: '书籍排版模块将在后续里程碑交付。现阶段请在「图库」整理照片，在「修改照片」完成编辑后经「打印」模块交付。' },
  web: { title: 'Web', desc: 'Web 画廊模块将在后续里程碑交付。现阶段可用「打印」模块导出作品集后自行发布，或用「幻灯片」全屏放映当前图库。' },
};

function ModulePlaceholder({ module }: { module: ModuleKey }) {
  const navigate = useUi((s) => s.navigate);
  const meta = MODULE_META[module];
  return (
    <div className="h-full flex items-center justify-center px-6">
      <div className="max-w-md rounded-lgC border border-line p-6 bg-panel" style={{ boxShadow: 'var(--Lumina-Tokens-shadow1)' }}>
        <div className="text-sm font-medium mb-2">{meta?.title ?? '该模块'}模块</div>
        <div className="text-2xs text-txt3 leading-6">
          {meta?.desc ?? '该模块将在后续里程碑交付，当前版本暂未开放。'}
        </div>
        <div className="mt-4 flex items-center gap-2">
          <Button onClick={() => navigate('develop')}>前往修改照片</Button>
        </div>
      </div>
    </div>
  );
}

/** 视图按需加载时的轻量占位：不引重依赖，仅一行提示，避免 Suspense 切换时闪烁变重。 */
function PageFallback() {
  return (
    <div className="h-full flex items-center justify-center">
      <span className="text-2xs text-txt3">加载中…</span>
    </div>
  );
}

function Shell() {
  const view = useUi((s) => s.view);
  const [bootError, setBootError] = useState<string | null>(null);
  const [stage, setStage] = useState<BootStage>('handshake');
  /** 未实现模块（书籍/幻灯片/Web）没有对应视图：用本地占位态，任何一次导航都会自动退出占位 */
  const [modulePlaceholder, setModulePlaceholder] = useState<ModuleKey | null>(null);
  const searchRef = useRef<HTMLInputElement | null>(null);
  const module: ModuleKey = modulePlaceholder ?? moduleOf(view);
  // goStage：setState + 立即留痕（避免 React 批处理吞掉短暂中间态的日志）
  const goStage = useCallback((next: BootStage) => {
    setStage(next);
    if (window.lumina?.isSmoke) console.log(`[smoke-scenario] boot-stage: ${next}`);
  }, []);
  const [bootAttempt, setBootAttempt] = useState(0);
  const navigate = useUi((s) => s.navigate);

  useEffect(() => {
    startEventPump();
    // 生命周期 → 崩溃恢复提示
    const offLifecycle = window.lumina.onLifecycle((s) => {
      if (s === 'recovered') useJobStore.getState().notifyRecovered();
    });
    let cancelled = false;
    // 兜底：任何阶段卡住 20s 都收回遮罩，避免永久挡住界面（但不静默：转为顶部 toast）
    const watchdog = setTimeout(() => {
      if (cancelled) return;
      setStage((cur) => {
        if (cur !== 'ready') {
          useJobStore.getState().toast('启动耗时较长，已跳过等待（部分内容稍后自动补全）', 'warn', 6000);
          return 'ready';
        }
        return cur;
      });
    }, 20000);

    (async () => {
      try {
        // —— 第 1 段：引擎握手 ——
        goStage('handshake');
        const info = await window.lumina.getInfo();
        if (cancelled) return;
        if (info.isSmoke) (window as unknown as { __LUMINA_SMOKE__?: boolean }).__LUMINA_SMOKE__ = true;
        // 握手（system.initialize）由主进程 EngineBridge.start() 完成并重放；
        // 渲染层不得重复 initialize（协议 §5.0 单次握手），引擎信息经 lumina:info 透传。
        const init = info.engineInfo as unknown as {
          protocolVersion?: string;
          engineVersion?: string;
          capabilities?: { mock?: boolean; codecs?: string[] };
        };
        const status = info.isMock ? 'mock' : 'engine';
        useJobStore.getState().setEngineStatus(status, init.engineVersion ?? '', (init as unknown as Record<string, unknown>) ?? {});

        // —— 第 2 段：库加载 ——
        goStage('library');
        await useLibrary.getState().refresh();
        if (cancelled) return;

        // —— 第 3 段：缩略图就绪（首屏 24 张 320px 预取，失败不阻断）——
        goStage('thumbs');
        const firstIds = (useLibrary.getState().pages[0] ?? []).slice(0, 24).map((p) => p.id);
        if (firstIds.length > 0) {
          await useLibrary.getState().ensureThumbs(firstIds, 320).catch(() => undefined);
        }
        if (cancelled) return;

        // 空库演示图库：**仅开发/冒烟环境**自动生成。
        // 打包版绝不允许自动灌测试图 —— 用户实测反馈"发布的 exe 存在测试用的图片"
        // 正是这条自动生成 + 开发档案共用导致的。打包版空库应停在空状态等用户导入。
        const stats = useLibrary.getState().stats;
        const allowDemoLibrary = info.isSmoke || !info.isPackaged;
        if (stats && stats.totalPhotos === 0 && init.capabilities?.mock && allowDemoLibrary) {
          const { jobId } = await api.mock.generate();
          useJobStore.getState().toast(`正在生成模拟图库（任务 ${jobId}）…`, 'info', 3000);
        }
        goStage('ready');
      } catch (err) {
        if (cancelled) return;
        const msg = errText(err);
        setBootError(msg);
        setStage((cur) => cur);
        // 失败不静默：toast + 启动遮罩内的可恢复卡片
        useJobStore.getState().toast(`启动失败：${msg}`, 'error', 8000);
      }
    })();
    return () => {
      cancelled = true;
      clearTimeout(watchdog);
      offLifecycle();
    };
  }, [bootAttempt]);

  // 全局快捷键：
  //  - Ctrl/Cmd+K 聚焦顶栏搜索；Ctrl+F 聚焦图库搜索（1.1 既有行为保留）
  //  - PS/LR 惯例单键（batch 1）：D 显影模块 / G 网格视图 / E 放大视图（→精选 review）。
  //    输入框聚焦时不抢键；带 Ctrl/Cmd 的组合键不动（编辑类快捷键归各模块）。
  //    \（前后对比）、1-5/0/P/X（评分/旗标）已由修图台/精选模块内 handler 承接，不重复绑定。
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const mod = e.ctrlKey || e.metaKey;
      const target = e.target as HTMLElement | null;
      const typing = target?.tagName === 'INPUT' || target?.tagName === 'TEXTAREA' || target?.isContentEditable;
      if (mod && e.key.toLowerCase() === 'k') {
        e.preventDefault();
        searchRef.current?.focus();
        return;
      }
      if (mod && e.key.toLowerCase() === 'f') {
        e.preventDefault();
        useUi.getState().requestSearchFocus();
        if (useUi.getState().view !== 'library') navigate('library');
        return;
      }
      if (mod || typing || e.altKey) return;
      const key = e.key.toLowerCase();
      if (key === 'd') {
        e.preventDefault();
        navigate('develop');
      } else if (key === 'g') {
        e.preventDefault();
        navigate('library');
      } else if (key === 'e') {
        // LR 的 E = 放大视图；LUMINA 最接近的单张放大语境是精选工作区（review 模式）
        e.preventDefault();
        navigate('culling');
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [navigate]);

  // 视图切换即退出「未实现模块」占位（占位不是路由，避免用户卡在空页面）
  useEffect(() => {
    setModulePlaceholder(null);
  }, [view]);

  // 冒烟场景：全模块流程驱动（证据经 console → 主进程 stdout）
  useEffect(() => {
    if (!window.lumina?.isSmoke) return;
    const t = setTimeout(() => {
      void import('./smoke').then((m) => m.runSmokeScenario());
    }, 800);
    return () => clearTimeout(t);
  }, []);

  // WP-C1 视觉基线：?baseline=1 时逐帧导航 + 双主题截图（与冒烟互斥）
  useEffect(() => {
    if (new URLSearchParams(window.location.search).get('baseline') !== '1') return;
    const t = setTimeout(() => {
      void import('./baseline').then((m) => m.runBaselineScenario());
    }, 1200);
    return () => clearTimeout(t);
  }, []);

  // v1.2.0 外壳自检：?shellcheck=1（真实点击四工作区 / 二级导航 / 主题 / 菜单）
  useEffect(() => {
    if (new URLSearchParams(window.location.search).get('shellcheck') !== '1') return;
    const t = setTimeout(() => {
      void import('./shellCheck').then((m) => m.runShellCheck());
    }, 800);
    return () => clearTimeout(t);
  }, []);

  if (bootError) {
    return (
      <div className="h-full flex flex-col items-center justify-center gap-3 px-8">
        <Icon name="warning" size={28} className="text-danger" />
        <div className="text-sm text-txt">引擎握手失败，工作台无法启动</div>
        <div className="text-2xs text-txt3 max-w-xl text-center break-all leading-5">{bootError}</div>
        <div className="text-2xs text-txt3">常见原因：引擎进程被安全软件拦截 / 安装目录被移动 / 端口占用。</div>
        <div className="flex items-center gap-2">
          <Button
            onClick={() => {
              setBootError(null);
              goStage('handshake');
              setBootAttempt((n) => n + 1);
            }}
          >
            重试
          </Button>
          <Button onClick={() => window.location.reload()}>重新加载应用</Button>
        </div>
      </div>
    );
  }

  return (
    <div className="h-full flex flex-col relative">
      <TopBar
        module={module}
        onPickModule={(k) => {
          const spec = MODULES.find((m) => m.key === k);
          if (spec?.defaultView) {
            setModulePlaceholder(null);
            navigate(spec.defaultView);
          } else {
            // 书籍/幻灯片/Web：显式占位（不许静默空白，也不自创路由）
            setModulePlaceholder(k);
          }
        }}
        searchRef={searchRef}
      />
      <MockBanner />
      {/* 工作区内容按"面板卡片"承载：外层 8px 间距 + 新圆角 + 面层阴影；
          面板内部圆角/间距属于各模块，按派工约束留到下一阶段再改 */}
      <main className="flex-1 min-h-0 p-2">
        <div
          className="h-full rounded-lgC overflow-hidden border border-line"
          style={{ background: 'var(--Lumina-Tokens-canvas)', boxShadow: 'var(--Lumina-Tokens-shadow1)' }}
        >
          <ErrorBoundary scope={modulePlaceholder ? `${MODULE_META[modulePlaceholder]?.title ?? '模块'}占位` : (VIEW_LABEL[view] ?? view)}>
            {modulePlaceholder ? (
              <ModulePlaceholder module={modulePlaceholder} />
            ) : (
              <Suspense fallback={<PageFallback />}>
                {view === 'library' && <LibraryPage />}
                {view === 'develop' && <DevelopPage />}
                {view === 'ingest' && <IngestWizard />}
                {view === 'health' && <HealthPage />}
                {view === 'culling' && <CullingPage />}
                {view === 'map' && <MapPage />}
                {view === 'export' && <ExportPage />}
                {view === 'timelapse' && <TimelapsePage />}
                {view === 'landscape' && <LandscapePage />}
                {view === 'portfolio' && <PortfolioPage />}
                {view === 'gallery' && <GalleryPage />}
                {view === 'slideshow' && <SlideshowPage />}
              </Suspense>
            )}
          </ErrorBoundary>
        </div>
      </main>
      <ToastHost />
      {stage !== 'ready' && (
        <BootProgress
          stage={stage}
          error={undefined}
          onRetry={() => {
            setBootError(null);
            goStage('handshake');
            setBootAttempt((n) => n + 1);
          }}
          onSkip={() => setStage('ready')}
        />
      )}
    </div>
  );
}

export default function App() {
  return (
    <ThemeProvider>
      <Shell />
    </ThemeProvider>
  );
}
