/**
 * 左侧导出预设列表：点击套用（格式徽标 + 摘要），底部保存当前设置为预设。
 * 引擎侧 export.listPresets 不可用时显示明确降级提示（零静默）。
 */

import { useState } from 'react';
import { useExport } from './store/exportStore';
import { Button, Spinner } from '../../components/primitives';
import { Dialog } from '../../components/shell';

const FORMAT_BADGE: Record<string, string> = { jpg: 'JPG', tiff: 'TIFF', png: 'PNG', psd: 'PSD' };

export default function PresetList() {
  const presets = useExport((s) => s.presets);
  const presetsLoading = useExport((s) => s.presetsLoading);
  const presetsError = useExport((s) => s.presetsError);
  const selectedPresetId = useExport((s) => s.selectedPresetId);
  const [saveOpen, setSaveOpen] = useState(false);
  const [presetName, setPresetName] = useState('');

  return (
    <aside className="shrink-0 w-56 border-r border-line flex flex-col overflow-hidden" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center h-9 px-3 border-b border-line shrink-0">
        <span className="text-2xs text-txt2 font-medium">导出预设</span>
        <div className="flex-1" />
        <span className="num text-2xs text-txt3">{presets.length}</span>
      </div>
      <div className="flex-1 min-h-0 overflow-y-auto p-2 flex flex-col gap-1.5">
        {presetsLoading && (
          <div className="flex items-center justify-center py-8">
            <Spinner size={16} />
          </div>
        )}
        {!presetsLoading && presetsError && (
          <div className="rounded-md border p-2.5 text-2xs leading-5" style={{ borderColor: 'var(--Lumina-Tokens-danger)', color: 'var(--Lumina-Tokens-textSecondary)' }}>
            <span className="font-medium" style={{ color: 'var(--Lumina-Tokens-danger)' }}>
              预设接口不可用
            </span>
            <br />
            {presetsError}
            <br />
            <button className="mt-1.5 text-accent hover:opacity-80" onClick={() => void useExport.getState().loadPresets()}>
              重试
            </button>
          </div>
        )}
        {!presetsLoading && !presetsError && presets.length === 0 && (
          <div className="py-8 text-center text-2xs text-txt3">暂无预设 · 调好参数后保存</div>
        )}
        {presets.map((p) => {
          const active = p.id === selectedPresetId;
          return (
            <button
              key={p.id}
              onClick={() => useExport.getState().applyPreset(p.id)}
              className={`text-left rounded-md border p-2 transition-colors ${active ? 'border-accent' : 'border-line hover:border-txt3'}`}
              style={{ background: active ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
            >
              <div className="flex items-center gap-1.5">
                <span
                  className="text-2xs leading-none px-1 py-0.5 rounded-sm num font-bold"
                  style={{
                    background: p.spec.format === 'psd' ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-hover)',
                    color: p.spec.format === 'psd' ? 'var(--Lumina-Tokens-onAccent)' : 'var(--Lumina-Tokens-textSecondary)',
                  }}
                >
                  {FORMAT_BADGE[p.spec.format] ?? p.spec.format.toUpperCase()}
                </span>
                <span className="text-2xs text-txt truncate flex-1">{p.name}</span>
              </div>
              <div className="num text-2xs text-txt3 mt-1">
                长边 {p.spec.longEdge === 0 ? '原尺寸' : p.spec.longEdge}px · {p.spec.colorSpace}
                {p.spec.format === 'jpg' && ` · Q${p.spec.quality}`}
              </div>
            </button>
          );
        })}
      </div>
      <div className="shrink-0 border-t border-line p-2">
        <Button className="w-full" size="sm" onClick={() => setSaveOpen(true)}>
          保存当前设置为预设
        </Button>
      </div>

      <Dialog open={saveOpen} title="保存为导出预设" onClose={() => setSaveOpen(false)}>
        <input
          autoFocus
          value={presetName}
          onChange={(e) => setPresetName(e.target.value)}
          placeholder="预设名称，如：客户交付 · 长边 4096"
          className="w-full h-8 px-2.5 rounded-md border border-line bg-card text-xs focus:outline-none focus:border-accent"
        />
        <div className="flex justify-end gap-2 mt-4">
          <Button onClick={() => setSaveOpen(false)}>取消</Button>
          <Button
            variant="primary"
            disabled={!presetName.trim() || useExport.getState().savingPreset}
            onClick={() => {
              void useExport.getState().savePreset(presetName);
              setSaveOpen(false);
              setPresetName('');
            }}
          >
            保存
          </Button>
        </div>
      </Dialog>
    </aside>
  );
}
