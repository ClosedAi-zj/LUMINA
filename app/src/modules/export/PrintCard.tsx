/**
 * M4-04 打印通道卡（无打印机环境的 PDF 分支）。
 *
 * 行为：按当前修图参数渲染一张长边 3000 的位图 → 交给主进程按纸张尺寸排版并 printToPDF。
 * 状态机：idle → render（渲染）→ layout（排版输出）→ done/failed，每态可见，失败给原文原因。
 * 原片只读：只读解码 + 只写 PDF，不改任何 sidecar/EXIF。
 */
import { useState } from 'react';
import { api } from '../../api/engine';
import { errText } from '../../api/rpc-client';
import { useDevelop } from '../../store/developStore';
import { useJobStore } from '../../store/jobStore';
import { Button, Segmented, Toggle } from '../../components/primitives';

type Stage = 'idle' | 'render' | 'layout' | 'done' | 'failed';

export default function PrintCard() {
  const photoId = useDevelop((s) => s.photoId);
  const params = useDevelop((s) => s.params);
  const [pageSize, setPageSize] = useState<'A4' | 'A3' | 'Letter'>('A4');
  const [landscape, setLandscape] = useState(false);
  const [margin, setMargin] = useState('10');
  const [stage, setStage] = useState<Stage>('idle');
  const [result, setResult] = useState<{ path: string; bytes: number } | null>(null);
  const [failure, setFailure] = useState('');

  const run = async () => {
    if (!photoId) {
      useJobStore.getState().toast('先在素材库打开一张照片，再输出打印 PDF', 'error', 5000);
      return;
    }
    setFailure('');
    setResult(null);
    try {
      setStage('render');
      const rendered = await api.develop.render({ photoId, params: params ?? undefined, longEdge: 3000 });
      setStage('layout');
      const r = await window.lumina.printPdf({
        imagePath: rendered.path,
        pageSize,
        landscape,
        marginMm: Number(margin),
        dpi: 300,
      });
      setResult({ path: r.path, bytes: r.bytes });
      setStage('done');
    } catch (e) {
      setFailure(errText(e));
      setStage('failed');
    }
  };

  const busy = stage === 'render' || stage === 'layout';

  return (
    <div className="rounded-lg border border-line p-3 flex flex-col gap-2.5" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center gap-2">
        <span className="text-xs font-medium">打印 / PDF 输出</span>
        <div className="flex-1" />
        <span className="text-2xs px-1.5 py-0.5 rounded-sm" style={{ background: 'var(--Lumina-Tokens-accentSoft)', color: 'var(--Lumina-Tokens-accent)' }}>v1.2.0</span>
      </div>

      <div className="flex items-center gap-3">
        <span className="text-2xs text-txt2 w-16 shrink-0">纸张</span>
        <Segmented
          value={pageSize}
          onChange={(v) => setPageSize(v as 'A4' | 'A3' | 'Letter')}
          options={[
            { value: 'A4', label: 'A4' },
            { value: 'A3', label: 'A3' },
            { value: 'Letter', label: 'Letter' },
          ]}
        />
        <Segmented
          value={margin}
          onChange={setMargin}
          options={[
            { value: '0', label: '无边距' },
            { value: '10', label: '10mm' },
            { value: '20', label: '20mm' },
          ]}
        />
      </div>

      <div className="flex items-center gap-3">
        <span className="text-2xs text-txt2 w-16 shrink-0">方向</span>
        <Toggle checked={landscape} onChange={setLandscape} label="横向" />
        <span className="text-2xs text-txt3">无打印机时输出 PDF；页面尺寸与边距按上方设置写入元数据</span>
      </div>

      <div className="flex items-center gap-3">
        <Button variant="primary" size="md" disabled={busy || !photoId} onClick={() => void run()}>
          {stage === 'render' ? '渲染中…' : stage === 'layout' ? '排版输出中…' : '输出打印 PDF'}
        </Button>
        <span className="text-2xs text-txt3">
          {stage === 'done' ? '已完成' : stage === 'failed' ? '失败' : photoId ? '按当前修图参数输出' : '需先在素材库打开一张照片'}
        </span>
      </div>

      {stage === 'done' && result && (
        <div className="flex flex-col gap-1.5">
          <div className="text-2xs text-txt3 break-all num">{result.path}（{(result.bytes / 1024).toFixed(0)} KB）</div>
          <div>
            <Button size="sm" variant="ghost" onClick={() => void window.lumina.showInFolder(result.path)}>
              在文件夹中显示
            </Button>
          </div>
        </div>
      )}
      {stage === 'failed' && (
        <div className="text-2xs" style={{ color: 'var(--Lumina-Tokens-danger)' }}>打印输出失败：{failure}</div>
      )}
    </div>
  );
}
