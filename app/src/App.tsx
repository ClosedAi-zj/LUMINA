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
 * v1.2.0 外壳信息架构：四个工作区 → 十一个视图（沿用 1.1 的模块，不改模块内部）。
 * 映射由 team-lead 指定，不要自创：
 *   库   = library(默认) / ingest / culling / map / health
 *   修图 = develop(默认) / landscape / timelapse
 *   精修 = 本阶段无实现（v1.2.0 M1 NodeGraph 交付）→ 显式占位，不许静默空白
 *   输出 = export(默认) / portfolio / gallery
 */
type WorkspaceKey = 'library' | 'develop' | 'retouch' | 'output';

const WORKSPACES: { key: WorkspaceKey; label: string; defaultView: AppView | null; views: AppView[] }[] = [
  { key: 'library', label: '库', defaultView: 'library', views: ['library', 'ingest', 'culling', 'map', 'health'] },
  { key: 'develop', label: '修图', defaultView: 'develop', views: ['develop', 'landscape', 'timelapse'] },
  { key: 'retouch', label: '精修', defaultView: null, views: [] },
  { key: 'output', label: '输出', defaultView: 'export', views: ['export', 'portfolio', 'gallery'] },
];

const VIEW_LABEL: Record<AppView, string> = {
  library: '图库',
  develop: '修图台',
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

function workspaceOf(view: AppView): WorkspaceKey {
  const hit = WORKSPACES.find((w) => w.views.includes(view));
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

function TopBar({
  workspace,
  onPickWorkspace,
  searchRef,
}: {
  workspace: WorkspaceKey;
  onPickWorkspace: (k: WorkspaceKey) => void;
  searchRef: { current: HTMLInputElement | null };
}) {
  const navigate = useUi((s) => s.navigate);
  const engineStatus = useJobStore((s) => s.engineStatus);
  const engineVersion = useJobStore((s) => s.engineVersion);
  const stats = useLibrary((s) => s.stats);
  const { theme } = useTheme();
  const toast = useJobStore((s) => s.toast);

  return (
    <header
      className="glass-m2 flex items-center h-11 px-2 gap-3 border-b border-line shrink-0"
      data-theme-surface={theme}
    >
      {/* 左：标识 + 菜单栏 */}
      <div className="flex items-center gap-2 shrink-0">
        <span className="w-5 h-5 rounded-smC bg-accent flex items-center justify-center text-onaccent text-xs font-bold">L</span>
        <span className="text-sm font-semibold tracking-wide">Lumina</span>
      </div>
      <nav className="flex items-center gap-0.5 shrink-0">
        {MENUS.map((m) => (
          <button
            key={m}
            onClick={() => toast(`${m}菜单即将开放（v1.2.0 后续里程碑）`, 'info', 2000)}
            className="hover-accent8 h-7 px-2 rounded-mdC text-xs text-txt2 hover:text-txt transition-colors"
          >
            {m}
          </button>
        ))}
      </nav>

      {/* 中：当前文档名占位 + 引擎状态（沿用 1.1 的状态提示，不回退） */}
      <div className="flex-1 min-w-0 flex items-center justify-center gap-3">
        <span className="text-xs text-txt2 truncate">未打开照片</span>
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
          {engineStatus === 'starting' ? '引擎连接中…' : engineStatus === 'mock' ? `Mock 引擎 · ${engineVersion || '1.1.1'}` : `引擎 · ${engineVersion || '1.1.1'}`}
          {stats && <span className="num">· {formatCount(stats.totalPhotos)} 张</span>}
        </span>
      </div>

      {/* 右：工作区四按钮 + 搜索 + 导出 + 主题 */}
      <nav className="flex items-center gap-1 shrink-0">
        {WORKSPACES.map((w) => {
          const active = workspace === w.key;
          return (
            <button
              key={w.key}
              onClick={() => onPickWorkspace(w.key)}
              className={`relative h-7 px-2.5 rounded-xs text-xs transition-colors ${
                active ? 'text-accent font-medium' : 'text-txt2 hover:text-txt hover:bg-hover'
              }`}
            >
              {w.label}
              {active && <span className="absolute left-1.5 right-1.5 bottom-0.5 h-0.5 rounded-pill bg-accent" />}
            </button>
          );
        })}
      </nav>

      <div className="flex items-center h-7 gap-1.5 px-2.5 rounded-pill border border-line bg-card shrink-0" style={{ width: 220 }}>
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

      <Button variant="primary" size="sm" onClick={() => navigate('export')} className="shrink-0">
        导出
      </Button>
      <ThemeToggle />
    </header>
  );
}

/** 二级导航：当前工作区下的视图（胶囊 tab，走 v1.2.0 圆角令牌） */
function SubNav({ workspace }: { workspace: WorkspaceKey }) {
  const view = useUi((s) => s.view);
  const navigate = useUi((s) => s.navigate);
  const healthIssueCount = useJobStore((s) => s.healthIssueCount);
  const spec = WORKSPACES.find((w) => w.key === workspace);
  if (!spec) return null;
  if (spec.views.length === 0) {
    return (
      <div className="glass-m1 flex items-center h-9 px-4 border-b border-line shrink-0 text-2xs text-txt3">
        精修工作区：v1.2.0 M1（NodeGraph）交付，当前版本此工作区为空
      </div>
    );
  }
  return (
    <nav className="glass-m1 flex items-center gap-1.5 h-9 px-3 border-b border-line shrink-0">
      {spec.views.map((v) => {
        const active = view === v;
        return (
          <button
            key={v}
            onClick={() => navigate(v)}
            className={`relative h-6 px-2.5 rounded-pill text-2xs transition-colors ${
              active ? 'bg-accent text-onaccent font-medium' : 'text-txt2 hover:text-txt hover:bg-hover'
            }`}
          >
            {VIEW_LABEL[v]}
            {v === 'health' && healthIssueCount > 0 && (
              <span className="absolute -top-0.5 -right-0.5 w-2 h-2 rounded-full" style={{ background: 'var(--Lumina-Tokens-danger)' }} />
            )}
          </button>
        );
      })}
    </nav>
  );
}

/** 精修工作区占位：本阶段无实现，必须显式告知（不许静默空白） */
function RetouchPlaceholder() {
  const navigate = useUi((s) => s.navigate);
  return (
    <div className="h-full flex items-center justify-center px-6">
      <div className="max-w-md rounded-lgC border border-line p-6 bg-panel" style={{ boxShadow: 'var(--Lumina-Tokens-shadow1)' }}>
        <div className="text-sm font-medium mb-2">精修工作区</div>
        <div className="text-2xs text-txt3 leading-6">
          精修工作区将在 <span className="text-accent">v1.2.0 M1（NodeGraph）</span> 交付。
          现阶段请使用「修图」工作区完成参数调整、局部蒙版与画笔精修。
        </div>
        <div className="mt-4 flex items-center gap-2">
          <Button onClick={() => navigate('develop')}>前往修图工作区</Button>
        </div>
      </div>
    </div>
  );
}

function Shell() {
  const view = useUi((s) => s.view);
  const [bootError, setBootError] = useState<string | null>(null);
  const [stage, setStage] = useState<BootStage>('handshake');
  /** 精修工作区没有对应视图：用本地占位态，任何一次导航都会自动退出占位 */
  const [retouch, setRetouch] = useState(false);
  const searchRef = useRef<HTMLInputElement | null>(null);
  const workspace: WorkspaceKey = retouch ? 'retouch' : workspaceOf(view);
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

  // 全局快捷键：Ctrl/Cmd+K 聚焦顶栏搜索；Ctrl+F 聚焦图库搜索（1.1 既有行为保留）
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const mod = e.ctrlKey || e.metaKey;
      if (mod && e.key.toLowerCase() === 'k') {
        e.preventDefault();
        searchRef.current?.focus();
        return;
      }
      if (mod && e.key.toLowerCase() === 'f') {
        e.preventDefault();
        useUi.getState().requestSearchFocus();
        if (useUi.getState().view !== 'library') navigate('library');
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [navigate]);

  // 视图切换即退出「精修」占位（占位不是路由，避免用户卡在空页面）
  useEffect(() => {
    setRetouch(false);
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
        workspace={workspace}
        onPickWorkspace={(k) => {
          const spec = WORKSPACES.find((w) => w.key === k);
          setRetouch(k === 'retouch');
          if (spec?.defaultView) navigate(spec.defaultView);
        }}
        searchRef={searchRef}
      />
      <SubNav workspace={workspace} />
      <MockBanner />
      {/* 工作区内容按"面板卡片"承载：外层 8px 间距 + 新圆角 + 面层阴影；
          面板内部圆角/间距属于各模块，按派工约束留到下一阶段再改 */}
      <main className="flex-1 min-h-0 p-2">
        <div
          className="h-full rounded-lgC overflow-hidden border border-line"
          style={{ background: 'var(--Lumina-Tokens-canvas)', boxShadow: 'var(--Lumina-Tokens-shadow1)' }}
        >
          <ErrorBoundary scope={retouch ? '精修' : (VIEW_LABEL[view] ?? view)}>
            {retouch ? (
              <RetouchPlaceholder />
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
