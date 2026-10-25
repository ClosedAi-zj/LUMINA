/**
 * 作品集主页（对照 03 号设计稿）：左画册列表 + 中画册详情/挑选排序 +
 * 右栏「水印设置 / 导出规格」两卡片（阶段4 03-7，样式沿用 PanelCard hairline 语言）。
 */

import { useEffect } from 'react';
import { usePortfolio } from './store/portfolioStore';
import { useExport } from '../export/store/exportStore';
import { Button } from '../../components/primitives';
import { EmptyState } from '../../components/shell';
import { PanelCard, ParamToggle, ParamSlider, ParamSegmented } from '../develop/panels/panelKit';
import AlbumList from './AlbumList';
import AlbumDetail from './AlbumDetail';

/** 引擎支持的 5 个水印锚点（protocol ExportSpec.watermark.position，kebab-case）；
 *  九宫格其余 4 个中点锚点引擎暂不支持，渲染为禁用格并标注（零静默，不伪造可点项） */
const WM_GRID: { key: string; label: string; supported: boolean }[] = [
  { key: 'top-left', label: '↖', supported: true },
  { key: 'top-center', label: '↑', supported: false },
  { key: 'top-right', label: '↗', supported: true },
  { key: 'middle-left', label: '←', supported: false },
  { key: 'center', label: '·', supported: true },
  { key: 'middle-right', label: '→', supported: false },
  { key: 'bottom-left', label: '↙', supported: true },
  { key: 'bottom-center', label: '↓', supported: false },
  { key: 'bottom-right', label: '↘', supported: true },
];

/** 右栏（03-7）：水印设置 + 导出规格。写同一个 exportStore.spec —— 与交付中心真实联动，非演示态 */
function OutputSidebar() {
  const spec = useExport((s) => s.spec);
  return (
    <aside className="w-64 shrink-0 border-l border-line overflow-y-auto p-1.5 flex flex-col gap-1.5" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <PanelCard title="水印设置" defaultOpen hint="随导出生效">
        <div className="px-2 pb-1 flex flex-col gap-1.5">
          <ParamToggle label="启用水印" checked={spec.watermark.enabled} onChange={(v) => useExport.getState().patchWatermark({ enabled: v })} />
          {spec.watermark.enabled && (
            <>
              <input
                value={spec.watermark.text ?? ''}
                onChange={(e) => useExport.getState().patchWatermark({ text: e.target.value })}
                placeholder="水印文字"
                className="h-7 px-2 rounded-mdC border border-line bg-card text-xs focus:outline-none focus:border-accent"
              />
              <div>
                <div className="text-2xs text-txt3 mb-1">位置（九宫格）</div>
                <div className="grid grid-cols-3 gap-1 w-24">
                  {WM_GRID.map((p) => (
                    <button
                      key={p.key}
                      disabled={!p.supported}
                      title={p.supported ? p.key : '引擎暂不支持该锚点'}
                      onClick={() => useExport.getState().patchWatermark({ position: p.key })}
                      className={`h-6 rounded-sm border text-2xs transition-colors ${
                        spec.watermark.position === p.key
                          ? 'border-accent text-accent'
                          : p.supported
                            ? 'border-line text-txt3 hover:text-txt2 hover:bg-hover'
                            : 'border-line text-txt3 opacity-40 cursor-not-allowed'
                      }`}
                      style={{ background: spec.watermark.position === p.key ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
                    >
                      {p.label}
                    </button>
                  ))}
                </div>
              </div>
            </>
          )}
        </div>
      </PanelCard>
      <PanelCard title="导出规格" defaultOpen hint="随导出生效">
        <div className="px-2 pb-1 flex flex-col gap-1.5">
          <ParamSegmented
            value={spec.format}
            options={[
              { value: 'jpg', label: 'JPG' },
              { value: 'tiff', label: 'TIFF' },
              { value: 'png', label: 'PNG' },
              { value: 'psd', label: 'PSD' },
            ]}
            onChange={(v) => useExport.getState().patchSpec({ format: v })}
          />
          <ParamSlider
            label="长边"
            value={spec.longEdge}
            min={1024}
            max={8192}
            step={256}
            defaultValue={4096}
            format={(v) => `${v}px`}
            onChange={(v) => useExport.getState().patchSpec({ longEdge: v })}
            onCommit={(v) => useExport.getState().patchSpec({ longEdge: v })}
          />
          <ParamSlider
            label="质量"
            value={spec.quality}
            min={50}
            max={100}
            defaultValue={90}
            format={(v) => `${v}%`}
            onChange={(v) => useExport.getState().patchSpec({ quality: v })}
            onCommit={(v) => useExport.getState().patchSpec({ quality: v })}
          />
        </div>
      </PanelCard>
    </aside>
  );
}

export default function PortfolioPage() {
  const loadError = usePortfolio((s) => s.loadError);
  const loading = usePortfolio((s) => s.loading);
  const albums = usePortfolio((s) => s.albums);

  useEffect(() => {
    void usePortfolio.getState().load();
  }, []);

  if (loadError && albums.length === 0 && !loading) {
    return (
      <div className="h-full flex items-center justify-center">
        <EmptyState
          icon={<span className="text-danger">⚠</span>}
          title="画册加载失败"
          desc={loadError}
          action={<Button onClick={() => void usePortfolio.getState().load()}>重试</Button>}
        />
      </div>
    );
  }

  return (
    <div className="h-full flex">
      <AlbumList />
      <AlbumDetail />
      <OutputSidebar />
    </div>
  );
}
