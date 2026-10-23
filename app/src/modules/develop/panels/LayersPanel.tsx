/**
 * 调整图层栈（设计稿 10 左栏）：新建/删除/重命名/上下移动 · 眼睛显隐 ·
 * 混合模式 · 不透明度 · 锁定（UI 本地）· 图层内曝光/对比快速滑块。
 * 数据存 params.adjustmentStack（协议 §4 AdjustmentLayer[]），经 edit.setParams 落 sidecar。
 */

import { useState } from 'react';
import { BLEND_MODES, useDevelop } from '../../../store/developStore';
import { Slider } from '../../../components/primitives';
import type { AdjustmentLayer } from '../../../api/protocol';

export default function LayersPanel() {
  const params = useDevelop((s) => s.params);
  const selectedLayerId = useDevelop((s) => s.selectedLayerId);
  const lockedLayerIds = useDevelop((s) => s.lockedLayerIds);
  const [renamingId, setRenamingId] = useState<string | null>(null);
  const [renameText, setRenameText] = useState('');

  if (!params) return null;
  const stack = params.adjustmentStack ?? [];
  const selected = stack.find((l) => l.id === selectedLayerId) ?? null;

  return (
    <div className="p-3 border-b border-line">
      <div className="flex items-center mb-2">
        <span className="text-2xs text-txt3 flex-1">图层 · 调整栈</span>
        <button
          className="text-2xs text-accent hover:opacity-80"
          onClick={() => void useDevelop.getState().addLayer('adjustment', `调整图层 ${stack.length + 1}`)}
        >
          ＋ 新建调整图层
        </button>
      </div>

      {stack.length === 0 && <div className="text-2xs text-txt3 py-1.5 leading-4">无调整图层 · 全局参数直接作用于底片，新建图层可叠加局部风格</div>}

      <div className="flex flex-col gap-px max-h-52 overflow-y-auto">
        {stack
          .slice()
          .reverse()
          .map((layer) => (
            <LayerRow
              key={layer.id}
              layer={layer}
              selected={layer.id === selectedLayerId}
              locked={lockedLayerIds.includes(layer.id)}
              renaming={renamingId === layer.id}
              renameText={renameText}
              setRenameText={setRenameText}
              startRename={() => {
                setRenamingId(layer.id);
                setRenameText(layer.name);
              }}
              commitRename={() => {
                void useDevelop.getState().renameLayer(layer.id, renameText);
                setRenamingId(null);
              }}
            />
          ))}
      </div>

      {selected && (
        <div className="mt-2 rounded-md border border-line p-2">
          <div className="flex items-center gap-1.5 mb-1.5">
            <span className="text-2xs text-txt2 flex-1 truncate">
              图层参数 · {selected.name}
            </span>
            <span className="num text-2xs text-txt3">{selected.kind}</span>
          </div>
          <LayerParamSlider layerId={selected.id} label="曝光" paramKey="exposure" min={-5} max={5} step={0.01} value={(selected.params.exposure as number) ?? 0} locked={lockedLayerIds.includes(selected.id)} />
          <LayerParamSlider layerId={selected.id} label="对比" paramKey="contrast" min={-100} max={100} step={1} value={(selected.params.contrast as number) ?? 0} locked={lockedLayerIds.includes(selected.id)} />
          <LayerParamSlider layerId={selected.id} label="饱和" paramKey="saturation" min={-100} max={100} step={1} value={(selected.params.saturation as number) ?? 0} locked={lockedLayerIds.includes(selected.id)} />
        </div>
      )}
    </div>
  );
}

function LayerRow({
  layer,
  selected,
  locked,
  renaming,
  renameText,
  setRenameText,
  startRename,
  commitRename,
}: {
  layer: AdjustmentLayer;
  selected: boolean;
  locked: boolean;
  renaming: boolean;
  renameText: string;
  setRenameText: (v: string) => void;
  startRename: () => void;
  commitRename: () => void;
}) {
  return (
    <div
      className={`rounded-sm px-1.5 py-1 cursor-pointer transition-colors ${selected ? 'bg-[var(--Lumina-Tokens-accentSoft)]' : 'hover:bg-hover'}`}
      onClick={() => useDevelop.getState().selectLayer(layer.id)}
    >
      <div className="flex items-center gap-1.5">
        {/* 眼睛显隐 */}
        <button
          className={`text-2xs w-4 ${layer.enabled ? 'text-txt2' : 'text-txt3'}`}
          title={layer.enabled ? '隐藏图层' : '显示图层'}
          onClick={(e) => {
            e.stopPropagation();
            void useDevelop.getState().updateLayer(layer.id, { enabled: !layer.enabled });
          }}
        >
          {layer.enabled ? '👁' : '—'}
        </button>
        {/* 名称（双击重命名） */}
        {renaming ? (
          <input
            autoFocus
            value={renameText}
            onChange={(e) => setRenameText(e.target.value)}
            onClick={(e) => e.stopPropagation()}
            onBlur={commitRename}
            onKeyDown={(e) => {
              if (e.key === 'Enter') commitRename();
              if (e.key === 'Escape') setRenameText('');
            }}
            className="flex-1 min-w-0 h-5 px-1 rounded-sm border border-accent bg-card text-2xs focus:outline-none"
          />
        ) : (
          <span
            className={`flex-1 min-w-0 truncate text-2xs ${layer.enabled ? 'text-txt2' : 'text-txt3 line-through'}`}
            onDoubleClick={(e) => {
              e.stopPropagation();
              if (!locked) startRename();
            }}
            title={locked ? '已锁定' : '双击重命名'}
          >
            {layer.name}
          </span>
        )}
        {/* 锁定（UI 本地） */}
        <button
          className={`text-2xs w-4 ${locked ? 'text-accent' : 'text-txt3 hover:text-txt2'}`}
          title={locked ? '解除锁定' : '锁定图层'}
          onClick={(e) => {
            e.stopPropagation();
            useDevelop.getState().toggleLayerLock(layer.id);
          }}
        >
          {locked ? '🔒' : '🔓'}
        </button>
        {/* 上下移动 */}
        <button
          className="text-2xs text-txt3 hover:text-txt w-3.5"
          title="上移（图层栈顶层优先）"
          onClick={(e) => {
            e.stopPropagation();
            void useDevelop.getState().moveLayer(layer.id, 1);
          }}
        >
          ↑
        </button>
        <button
          className="text-2xs text-txt3 hover:text-txt w-3.5"
          title="下移"
          onClick={(e) => {
            e.stopPropagation();
            void useDevelop.getState().moveLayer(layer.id, -1);
          }}
        >
          ↓
        </button>
        {/* 删除 */}
        <button
          className="text-2xs text-txt3 hover:text-danger w-4 text-right"
          title="删除图层"
          onClick={(e) => {
            e.stopPropagation();
            void useDevelop.getState().removeLayer(layer.id);
          }}
        >
          ✕
        </button>
      </div>
      <div className="flex items-center gap-1.5 mt-0.5 pl-5">
        {/* 混合模式 */}
        <select
          value={layer.blendMode}
          onClick={(e) => e.stopPropagation()}
          onChange={(e) => void useDevelop.getState().updateLayer(layer.id, { blendMode: e.target.value })}
          className="h-5 min-w-0 flex-1 rounded-sm border border-line bg-card text-2xs text-txt3 focus:outline-none focus:border-accent"
          title="混合模式"
        >
          {BLEND_MODES.map((b) => (
            <option key={b.value} value={b.value}>
              {b.label}
            </option>
          ))}
        </select>
        <span className="num text-2xs text-txt3 shrink-0">{Math.round(layer.opacity)}%</span>
      </div>
      {/* 不透明度 */}
      <div className="pl-5 pr-1" onClick={(e) => e.stopPropagation()}>
        <input
          type="range"
          min={0}
          max={100}
          value={layer.opacity}
          onChange={(e) => void useDevelop.getState().updateLayer(layer.id, { opacity: Number(e.target.value) })}
          className="w-full h-0.5 appearance-none rounded-full cursor-pointer accent-accent mt-1"
          style={{
            background: `linear-gradient(to right, var(--Lumina-Tokens-accent) 0%, var(--Lumina-Tokens-accent) ${layer.opacity}%, var(--Lumina-Tokens-border) ${layer.opacity}%, var(--Lumina-Tokens-border) 100%)`,
          }}
        />
      </div>
    </div>
  );
}

function LayerParamSlider({
  layerId,
  label,
  paramKey,
  min,
  max,
  step,
  value,
  locked,
}: {
  layerId: string;
  label: string;
  paramKey: string;
  min: number;
  max: number;
  step: number;
  value: number;
  locked: boolean;
}) {
  return (
    <div className={locked ? 'opacity-40 pointer-events-none' : ''}>
      <Slider
        label={label}
        value={value}
        min={min}
        max={max}
        step={step}
        defaultValue={0}
        onChange={(v) => void useDevelop.getState().updateLayerParam(layerId, paramKey as never, v, false)}
        onCommit={(v) => void useDevelop.getState().updateLayerParam(layerId, paramKey as never, v, true)}
      />
    </div>
  );
}
