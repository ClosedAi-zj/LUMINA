/**
 * LUMINA App 根组件：握手引导、模块路由、全局顶栏、事件泵。
 */

import { useCallback, useEffect, useState } from 'react';
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

const NAV: { view: AppView; label: string }[] = [
  { view: 'library', label: '图库' },
  { view: 'develop', label: '修图' },
  { view: 'culling', label: '精选' },
  { view: 'map', label: '地图' },
  { view: 'timelapse', label: '延时' },
  { view: 'landscape', label: '风光' },
  { view: 'ingest', label: '导入' },
  { view: 'portfolio', label: '作品集' },
  { view: 'gallery', label: '画廊' },
  { view: 'export', label: '交付' },
  { view: 'health', label: '存储健康' },
];

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

function TopBar() {  const view = useUi((s) => s.view);
  const navigate = useUi((s) => s.navigate);
  const engineStatus = useJobStore((s) => s.engineStatus);
  const engineVersion = useJobStore((s) => s.engineVersion);
  const healthIssueCount = useJobStore((s) => s.healthIssueCount);
  const stats = useLibrary((s) => s.stats);
  const { theme } = useTheme();

  return (
    <header
      className="flex items-center h-11 px-3 gap-4 border-b border-line shrink-0"
      style={{ background: 'var(--Lumina-Tokens-browserBar)' }}
      data-theme-surface={theme}
    >
      <div className="flex items-center gap-2">
        <span className="w-5 h-5 rounded-md bg-accent flex items-center justify-center text-onaccent text-xs font-bold">L</span>
        <span className="text-sm font-semibold tracking-wide">Lumina</span>
        <span className="text-2xs text-txt3">摄影工作台</span>
      </div>

      <nav className="flex items-center gap-1">
        {NAV.map((n) => (
          <button
            key={n.view}
            onClick={() => navigate(n.view)}
            className={`relative h-7 px-3 rounded-md text-xs transition-colors ${
              view === n.view ? 'bg-accent text-onaccent font-medium' : 'text-txt2 hover:text-txt hover:bg-hover'
            }`}
          >
            {n.label}
            {n.view === 'health' && healthIssueCount > 0 && (
              <span className="absolute -top-1 -right-1 w-2.5 h-2.5 rounded-full" style={{ background: 'var(--Lumina-Tokens-danger)' }} />
            )}
          </button>
        ))}
      </nav>

      <div className="flex-1" />

      <div className="flex items-center gap-2 text-2xs text-txt3">
        <span className="inline-flex items-center gap-1.5">
          <span
            className="w-1.5 h-1.5 rounded-full"
            style={{ background: engineStatus === 'starting' ? 'var(--Lumina-Tokens-textMuted)' : engineStatus === 'mock' ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-success)' }}
          />
          {engineStatus === 'starting' ? '引擎连接中…' : engineStatus === 'mock' ? `Mock 引擎 · ${engineVersion || '1.0.0'}` : `引擎 · ${engineVersion || '1.0.0'}`}
        </span>
        {stats && <span className="num">已同步 · {formatCount(stats.totalPhotos)} 张</span>}
      </div>
      <ThemeToggle />
    </header>
  );
}

function Shell() {
  const view = useUi((s) => s.view);
  const [bootError, setBootError] = useState<string | null>(null);
  const [stage, setStage] = useState<BootStage>('handshake');
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

        // 空库：引导生成模拟图库（引擎/mock 均支持 mock.generate）
        const stats = useLibrary.getState().stats;
        if (stats && stats.totalPhotos === 0 && init.capabilities?.mock) {
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

  // 全局快捷键：Ctrl+F 聚焦搜索（PRD §6.3）
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'f') {
        e.preventDefault();
        useUi.getState().requestSearchFocus();
        if (useUi.getState().view !== 'library') navigate('library');
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [navigate]);

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
      <TopBar />
      <MockBanner />
      <main className="flex-1 min-h-0">
        <ErrorBoundary scope={NAV.find((n) => n.view === view)?.label ?? view}>
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
        </ErrorBoundary>
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
