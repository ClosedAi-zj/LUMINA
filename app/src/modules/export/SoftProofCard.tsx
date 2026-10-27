/**
 * M4-03 软打样预览卡（导出与交付中心内）。
 *
 * 真实行为：调用引擎 softproof.preview → 引擎用 lcms2 把当前照片按目标 profile 打样一次，
 * 落盘 PNG 并回传 ΔE00 统计（打样后 vs 打样前）。这里只负责选择目标/意图与呈现结果，
 * 不伪造任何数值 —— ΔE 全部来自引擎实测。
 *
 * 诚实边界：不带 ICC 时目标为内置 AdobeRGB（演示/自检用）；真实印刷需要用户提供打印机
 * profile 文件。PRD 里"与 Photoshop 软打样比对"的验收不在此处冒充（见 m4-icc-roundtrip.json）。
 */
import { useState } from 'react';
import { api } from '../../api/engine';
import { errText, fileUrl } from '../../api/rpc-client';
import { useDevelop } from '../../store/developStore';
import { useJobStore } from '../../store/jobStore';
import { Button, Segmented, Toggle } from '../../components/primitives';

type DeltaE = { mean: number; median: number; p95: number; max: number };

export default function SoftProofCard() {
  const photoId = useDevelop((s) => s.photoId);
  const params = useDevelop((s) => s.params);
  const [targetProfile, setTargetProfile] = useState('');
  const [intent, setIntent] = useState('1');
  const [simulatePaper, setSimulatePaper] = useState(true);
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState<{ path: string; target: string; deltaE: DeltaE; width: number; height: number } | null>(null);

  const run = async () => {
    if (!photoId) {
      useJobStore.getState().toast('先在素材库打开一张照片，再生成打样预览', 'error', 5000);
      return;
    }
    setBusy(true);
    try {
      const r = await api.develop.softProofPreview({
        photoId,
        params: params ?? undefined,
        longEdge: 1400,
        targetProfile: targetProfile || undefined,
        intent: Number(intent),
        simulatePaper,
      });
      setResult({ path: r.path, target: r.target, deltaE: r.deltaE, width: r.width, height: r.height });
    } catch (e) {
      useJobStore.getState().toast(`打样预览失败：${errText(e)}`, 'error', 8000);
    } finally {
      setBusy(false);
    }
  };

  const pickTarget = async () => {
    const p = await window.lumina.pickFile({ title: '选择打样目标 ICC', extensions: ['icc', 'icm'] });
    if (p) setTargetProfile(p);
  };

  return (
    <div className="rounded-lg border border-line p-3 flex flex-col gap-2.5" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center gap-2">
        <span className="text-xs font-medium">打样预览（软打样）</span>
        <div className="flex-1" />
        <span className="text-2xs px-1.5 py-0.5 rounded-sm" style={{ background: 'var(--Lumina-Tokens-accentSoft)', color: 'var(--Lumina-Tokens-accent)' }}>v1.2.0</span>
      </div>

      <div className="flex items-center gap-3">
        <span className="text-2xs text-txt2 w-16 shrink-0">目标</span>
        <span className="text-2xs text-txt3 truncate flex-1" title={targetProfile || '内置 AdobeRGB（演示目标）'}>
          {targetProfile ? targetProfile.split(/[\\/]/).pop() : '内置 AdobeRGB（演示目标）'}
        </span>
        <Button size="sm" variant="ghost" onClick={() => void pickTarget()}>选择 ICC…</Button>
        {targetProfile && (
          <Button size="sm" variant="ghost" onClick={() => setTargetProfile('')}>清除</Button>
        )}
      </div>

      <div className="flex items-center gap-3">
        <span className="text-2xs text-txt2 w-16 shrink-0">意图</span>
        <Segmented
          value={intent}
          onChange={setIntent}
          options={[
            { value: '1', label: '相对色度' },
            { value: '0', label: '感知' },
            { value: '3', label: '绝对色度' },
            { value: '2', label: '饱和度' },
          ]}
        />
      </div>

      <div className="flex items-center gap-3">
        <span className="text-2xs text-txt2 w-16 shrink-0">纸白</span>
        <Toggle checked={simulatePaper} onChange={setSimulatePaper} label="模拟纸白" />
        <span className="text-2xs text-txt3">关闭时只模拟油墨叠印，不压纸白</span>
      </div>

      <div className="flex items-center gap-3">
        <Button variant="primary" size="md" disabled={busy || !photoId} onClick={() => void run()}>
          {busy ? '打样中…' : '生成打样预览'}
        </Button>
        <span className="text-2xs text-txt3">
          {photoId ? '按当前修图参数实时打样' : '需先在素材库打开一张照片'}
        </span>
      </div>

      {result && (
        <div className="flex flex-col gap-2">
          <div className="rounded-md overflow-hidden border border-line" style={{ background: 'var(--Lumina-Tokens-viewer)' }}>
            <img src={fileUrl(result.path)} alt="软打样预览" className="w-full max-h-64 object-contain" />
          </div>
          <div className="flex items-center gap-3 flex-wrap num text-2xs text-txt3">
            <span>目标 {result.target}</span>
            <span>尺寸 {result.width}×{result.height}</span>
            <span>ΔE00 中位 <b className="text-txt2">{result.deltaE.median.toFixed(2)}</b></span>
            <span>95 分位 <b className="text-txt2">{result.deltaE.p95.toFixed(2)}</b></span>
            <span>最大 <b className="text-txt2">{result.deltaE.max.toFixed(2)}</b></span>
          </div>
          <div className="text-2xs text-txt3 leading-4">
            ΔE00 = 打样后 vs 打样前（同一 sRGB Lab 口径），衡量这版打样把画面推了多远；印刷准确度
            取决于目标 ICC 是否匹配你的纸墨组合。
          </div>
        </div>
      )}
    </div>
  );
}
