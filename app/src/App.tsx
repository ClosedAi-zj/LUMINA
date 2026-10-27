/**
 * LUMINA App 根组件：握手引导、模块路由、全局顶栏、事件泵。
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import { ThemeProvider, useTheme } from './theme/ThemeProvider';
import { api } from './api/engine';
import { errText, fileUrl } from './api/rpc-client';
import { useJobStore } from './store/jobStore';
import { startEventPump } from './store/events';
import { useLibrary } from './store/libraryStore';
import { useUi, type AppView, type BootStage } from './store/uiStore';
import { ToastHost, Icon, ThemeToggle } from './components/shell';
import { Button } from './components/primitives';
import { WindowControls } from './components/WindowControls';
import ErrorBoundary from './components/ErrorBoundary';
import BootProgress from './components/BootProgress';
import { formatCount } from './utils/format';
import LibraryPage from './modules/library/LibraryPage';
import DevelopPage from './modules/develop/DevelopPage';
import IngestWizard from './modules/ingest/IngestWizard';
import HealthPage from './modules/health/HealthPage';
import CullingPage from './modules/culling/CullingPage';
import MapPage from './modules/map/MapPage';
import ExportPage from './modules/export/ExportPage';
import TimelapsePage from './modules/timelapse/TimelapsePage';
import LandscapePage from './modules/landscape/LandscapePage';
import PortfolioPage from './modules/portfolio/PortfolioPage';
import GalleryPage from './modules/gallery/GalleryPage';

/**
 * v1.2.0 外壳信息架构（UI 迭代 ④）：顶部模块切换对齐 LR 七大模块 tab 形态
 * （图库 / 修改照片 / 地图 / 书籍 / 幻灯片 / 打印 / Web）。
 * LUMINA 定位：综合图像处理软件 —— 风光摄影是"特色模块"（修改照片 ▸ 风光，带特色徽标），
 * 不是唯一身份。映射：
 *   图库     = library(默认) / ingest / culling / health
 *   修改照片 = develop(默认) / landscape(风光·特色) / timelapse
 *   地图     = map
 *   书籍     = 本阶段未实现 → 显式占位，不许静默空白
 *   幻灯片   = 本阶段未实现 → 显式占位
 *   打印     = LUMINA 交付中心（export / portfolio / gallery），LR 中打印/导出同属输出模块
 *   Web      = 本阶段未实现 → 显式占位
 */
type ModuleKey = 'library' | 'develop' | 'map' | 'book' | 'slideshow' | 'print' | 'web';

const MODULES: { key: ModuleKey; label: string; views: AppView[]; defaultView: AppView | null }[] = [
  { key: 'library', label: '图库', views: ['library', 'ingest', 'culling', 'health'], defaultView: 'library' },
  { key: 'develop', label: '修改照片', views: ['develop', 'landscape', 'timelapse'], defaultView: 'develop' },
  { key: 'map', label: '地图', views: ['map'], defaultView: 'map' },
  { key: 'book', label: '书籍', views: [], defaultView: null },
  { key: 'slideshow', label: '幻灯片', views: [], defaultView: null },
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
};

function moduleOf(view: AppView): ModuleKey {
  const hit = MODULES.find((m) => m.views.includes(view));
  return hit?.key ?? 'library';
}

/** 顶栏菜单：本阶段只有视觉与 hover（整项填色），点击给"即将开放"提示 */
const MENUS = ['文件', '编辑', '图像', '图层', '选择', '滤镜', '视图', '窗口', '帮助'];

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
 * 顶栏 = 自定义标题栏 + 工具栏一体化（UI 改版 batch 1；UI 迭代 batch 2 升级 LR 七模块 tab）。
 *
 * 替代原先「原生标题栏 + TopBar + SubNav」三层结构，合并为一条 40px 深色 chrome：
 *   左：产品标识 + PS 式全局菜单占位 + LR 七大模块 tab（图库/修改照片/地图/书籍/幻灯片/打印/Web，
 *       选中态 = 亮底 + 2px accent 下缘线，LR 模块拾取器形态）
 *   中：当前模块上下文（模块内视图胶囊，风光带"特色"徽标）+ 引擎状态
 *   右：搜索 / 导出 / 主题 / 窗口三键（min·max·close）
 * 空白区域 `-webkit-app-region: drag` 可拖拽窗口，双击最大化/还原；交互子元素 app-no-drag。
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
  const { theme } = useTheme();
  const toast = useJobStore((s) => s.toast);

  return (
    <header
      className="app-drag flex items-center h-10 gap-2 border-b border-line shrink-0 select-none"
      style={{ background: 'var(--Lumina-Tokens-browserBar)' }}
      data-theme-surface={theme}
      onDoubleClick={(e) => {
        // 拖拽区双击 = 最大化/还原（LR/Windows 惯例）；点在交互元素上不触发
        if ((e.target as HTMLElement).closest('.app-no-drag, button, input')) return;
        void window.lumina.win.toggleMaximize();
      }}
    >
      {/* 左：产品标识（LR 身份牌） */}
      <div className="app-no-drag flex items-center gap-2 pl-3 shrink-0">
        <span className="w-5 h-5 rounded-smC bg-accent flex items-center justify-center text-onaccent text-xs font-bold">L</span>
        <span className="text-sm font-semibold tracking-wide">Lumina</span>
      </div>

      {/* 左：PS 式全局菜单（视觉与 hover，点击给"即将开放"提示，与 1.1 行为一致） */}
      <nav className="app-no-drag flex items-center gap-0.5 shrink-0">
        {MENUS.map((m) => (
          <button
            key={m}
            onClick={() => toast(`${m}菜单即将开放（v1.2.0 后续里程碑）`, 'info', 2000)}
            className="hover-accent8 h-7 px-1.5 rounded-mdC text-2xs text-txt2 hover:text-txt transition-colors"
          >
            {m}
          </button>
        ))}
      </nav>

      {/* 左：LR 七大模块 tab（UI 迭代 ④：模块拾取器形态 —— 相连 tab + 细分隔线，
          选中态 = hover 底色 + 2px accent 下缘线，替代原四工作区切换器） */}
      <nav className="app-no-drag flex items-center shrink-0" aria-label="模块切换">
        {MODULES.map((m, i) => {
          const active = module === m.key;
          return (
            <button
              key={m.key}
              onClick={() => onPickModule(m.key)}
              className={`relative h-7 px-3 text-xs transition-colors ${i > 0 ? 'border-l border-line' : ''} ${
                active ? 'text-txt font-semibold' : 'text-txt2 hover:text-txt'
              }`}
              style={
                active
                  ? { background: 'var(--Lumina-Tokens-hover)', boxShadow: 'inset 0 -2px 0 var(--Lumina-Tokens-accent)' }
                  : undefined
              }
            >
              {m.label}
            </button>
          );
        })}
      </nav>

      {/* 中：当前模块上下文 = 模块内视图胶囊（风光带"特色"徽标）+ 引擎状态 */}
      <div className="flex-1 min-w-0 flex items-center justify-center gap-3 px-2">
        <nav className="app-no-drag flex items-center gap-1 min-w-0 overflow-hidden">
          {(() => {
            const spec = MODULES.find((m) => m.key === module);
            if (!spec || spec.views.length === 0) {
              return (
                <span className="text-2xs text-txt3 truncate">
                  {spec?.label ?? '该模块'}模块：后续里程碑交付，当前版本暂未开放
                </span>
              );
            }
            return spec.views.map((v) => {
              const active = view === v;
              const isLandscape = v === 'landscape';
              return (
                <button
                  key={v}
                  onClick={() => navigate(v)}
                  className={`relative h-6 px-2.5 rounded-pill text-2xs transition-colors shrink-0 inline-flex items-center gap-1 ${
                    active ? 'bg-accent text-onaccent font-medium' : 'text-txt2 hover:text-txt hover:bg-hover'
                  }`}
                >
                  {VIEW_LABEL[v]}
                  {/* 定位迭代：风光 = 特色模块入口，显式高亮而非全站喊风光 */}
                  {isLandscape && (
                    <span
                      className="px-1 rounded-sm text-[9px] leading-[14px]"
                      style={{
                        background: active ? 'rgba(0,0,0,0.18)' : 'var(--Lumina-Tokens-accentSoft)',
                        color: active ? 'var(--Lumina-Tokens-onAccent)' : 'var(--Lumina-Tokens-accent)',
                      }}
                    >
                      特色
                    </span>
                  )}
                  {v === 'health' && healthIssueCount > 0 && (
                    <span className="absolute -top-0.5 -right-0.5 w-2 h-2 rounded-full" style={{ background: 'var(--Lumina-Tokens-danger)' }} />
                  )}
                </button>
              );
            });
          })()}
        </nav>
        <span className="flex items-center gap-1.5 text-2xs text-txt3 shrink-0">
          <span
            className="w-1.5 h-1.5 rounded-full"
            style={{
              background:
                engineStatus === 'starting'
                  ? 'var(--Lumina-Tokens-textMuted)'
                  : engineStatus === 'mock'
                    ? 'var(--Lumina-Tokens-accent)'
                    : 'var(--Lumina-Tokens-success)',
            }}
          />
          <span className="hidden xl:inline">
            {engineStatus === 'starting' ? '引擎连接中…' : engineStatus === 'mock' ? `Mock · ${engineVersion || '1.1.1'}` : `引擎 · ${engineVersion || '1.1.1'}`}
            {stats && <span className="num"> · {formatCount(stats.totalPhotos)} 张</span>}
          </span>
        </span>
      </div>

      {/* 右：搜索 / 导出 / 主题（交互元素一律 no-drag） */}
      <div className="app-no-drag flex items-center gap-1.5 shrink-0">
        <div className="flex items-center h-7 gap-1.5 px-2.5 rounded-pill border border-line bg-card" style={{ width: 180 }}>
          <Icon name="search" size={12} className="text-txt3 shrink-0" />
          <input
            ref={(el) => {
              searchRef.current = el;
            }}
            placeholder="搜索照片、文件夹、关键词"
            className="flex-1 min-w-0 bg-transparent text-2xs text-txt placeholder:text-txt3 focus:outline-none"
          />
          <span className="text-[10px] text-txt3 shrink-0">⌘K</span>
        </div>

        <Button variant="primary" size="sm" onClick={() => navigate('export')}>
          导出
        </Button>
        <ThemeToggle />
      </div>

      {/* 右缘：窗口三键（满高热区，不参与拖拽） */}
      <WindowControls />
    </header>
  );
}

/** 未实现模块占位（书籍/幻灯片/Web）：必须显式告知（不许静默空白），沿用精修占位式提示 */
const MODULE_META: Partial<Record<ModuleKey, { title: string; desc: string }>> = {
  book: { title: '书籍', desc: '书籍排版模块将在后续里程碑交付。现阶段请在「图库」整理照片，在「修改照片」完成编辑后经「打印」模块交付。' },
  slideshow: { title: '幻灯片', desc: '幻灯片放映模块将在后续里程碑交付。现阶段可先在「图库」用星级/旗标精选出放映序列。' },
  web: { title: 'Web', desc: 'Web 画廊模块将在后续里程碑交付。现阶段可用「打印」模块导出作品集后自行发布。' },
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
              <>
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
              </>
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
