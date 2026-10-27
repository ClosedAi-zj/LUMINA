/**
 * 风光专项主页（对照 12 号设计稿上半部分）：风光合成卡片区
 * （HDR v1.0.0 交互 / 全景拼接 v1.1 降级卡 / 焦点堆栈 v1.1 降级卡 / AI 降噪）+
 * 底部版本路线条。存储健康与导出中心已有独立模块，此处不重复。
 */

import { useEffect } from 'react';
import { useLandscape } from './store/landscapeStore';
import { Button } from '../../components/primitives';
import { EmptyState } from '../../components/shell';
import HdrCard from './HdrCard';
import DenoiseCard from './DenoiseCard';

/** v1.1 能力占位卡：按 PRD 版本定位显式标注，不报错 */
function PlannedCard({ icon, title, desc, meta }: { icon: string; title: string; desc: string; meta: string }) {
  return (
    <div className="rounded-lg border border-line p-3 flex flex-col gap-2 opacity-90" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center gap-2">
        <span className="text-sm">{icon}</span>
        <span className="text-xs font-medium">{title}</span>
        <div className="flex-1" />
        <span className="text-2xs px-1.5 py-0.5 rounded-sm border border-line text-txt3">v1.1</span>
      </div>
      <div className="flex items-center gap-2 py-2">
        <div className="flex gap-0.5">
          {[0, 1, 2].map((i) => (
            <span key={i} className="w-8 h-6 rounded-sm" style={{ background: 'var(--Lumina-Tokens-card)', border: '1px solid var(--Lumina-Tokens-border)' }} />
          ))}
          <span className="text-txt3 self-center mx-0.5">→</span>
          <span className="w-10 h-6 rounded-sm" style={{ background: 'var(--Lumina-Tokens-accentSoft)', border: '1px solid var(--Lumina-Tokens-accent)' }} />
        </div>
      </div>
      <div className="text-2xs text-txt2 leading-4">{desc}</div>
      <div className="text-2xs text-txt3">{meta}</div>
      <div className="rounded-md px-2 py-1.5 text-2xs" style={{ background: 'var(--Lumina-Tokens-accentSoft)', color: 'var(--Lumina-Tokens-accent)' }}>
        该能力 v1.1 提供 · 当前版本未开放
      </div>
    </div>
  );
}

const ROADMAP: { version: string; label: string; current?: boolean }[] = [
  { version: 'v1.2.0', label: 'LR 闭环 + PSD 分层导出（本批）', current: true },
  { version: 'v1.1.1', label: '核心闭环' },
  { version: 'v1.1', label: '全景 / 堆栈 / AI 降噪' },
  { version: 'v1.2', label: '星空 / 打印 / 移动端' },
  { version: 'v2.0', label: '插件市场 / 协作新闻' },
];

export default function LandscapePage() {
  const groupsLoading = useLandscape((s) => s.groupsLoading);
  const groups = useLandscape((s) => s.groups);

  useEffect(() => {
    void useLandscape.getState().load();
  }, []);

  return (
    <div className="h-full overflow-y-auto p-5" style={{ background: 'var(--Lumina-Tokens-canvas)' }}>
      <div className="flex items-center gap-3 mb-4">
        <h2 className="text-base font-semibold">风光专项</h2>
        {/* 定位迭代：LUMINA 是综合图像处理软件，风光是"特色模块"而非唯一身份 —— 显式打上特色标签 */}
        <span
          className="text-2xs px-1.5 py-0.5 rounded-sm border"
          style={{
            color: 'var(--Lumina-Tokens-accent)',
            borderColor: 'color-mix(in srgb, var(--Lumina-Tokens-accent) 45%, transparent)',
            background: 'var(--Lumina-Tokens-accentSoft)',
          }}
        >
          特色模块
        </span>
        <span className="text-2xs text-txt3">COMPOSITE · 合成 / 降噪</span>
        <div className="flex-1" />
        {groupsLoading && <span className="text-2xs text-txt3">检测包围组中…</span>}
        {!groupsLoading && <span className="num text-2xs text-txt3">{groups.length} 个包围组</span>}
      </div>

      <div className="grid grid-cols-3 gap-3">
        <HdrCard />
        <PlannedCard icon="⛰" title="全景拼接" desc="柱形 / 球形投影 · 接缝融合 · 自动裁剪" meta="12 组待拼接 · 每组 6 ~ 9 张" />
        <PlannedCard icon="◫" title="焦点堆栈" desc="焦点皮革 · 深度图合成 · 半径蒙版" meta="5 组待堆叠 · 每组 8 ~ 13 张" />
        <DenoiseCard />
      </div>

      {/* 版本路线条 */}
      <div className="mt-6 rounded-lg border border-line px-4 py-3 flex items-center gap-3 flex-wrap max-w-5xl" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <span className="text-2xs text-txt3 shrink-0">版本路线</span>
        {ROADMAP.map((r) => (
          <span
            key={r.version}
            className={`text-2xs px-2.5 h-6 inline-flex items-center gap-1.5 rounded-full border ${
              r.current ? 'border-accent font-medium' : 'border-line text-txt3'
            }`}
            style={r.current ? { background: 'var(--Lumina-Tokens-accentSoft)', color: 'var(--Lumina-Tokens-accent)' } : { background: 'var(--Lumina-Tokens-card)' }}
          >
            <span className="num">{r.version}</span>
            <span>·</span>
            <span>{r.label}</span>
          </span>
        ))}
      </div>

      {groups.length === 0 && !groupsLoading && (
        <div className="mt-6 max-w-5xl">
          <EmptyState title="暂无包围组" desc="导入同机位 ±EV 连拍（≥3 张）后，此处自动列出可合成的 HDR 组。" action={<Button onClick={() => void useLandscape.getState().load()}>重新检测</Button>} />
        </div>
      )}
    </div>
  );
}
