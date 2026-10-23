/**
 * 左侧栏：文件夹树（5 根目录可折叠 + 计数 + 缺失红点）+ 智能收藏夹 + 时间线（按月柱状）。
 */

import { useEffect, useState } from 'react';
import { useLibrary } from '../../store/libraryStore';
import { api } from '../../api/engine';
import { errText } from '../../api/rpc-client';
import { useJobStore } from '../../store/jobStore';
import { formatCount, formatMonth } from '../../utils/format';
import { Icon } from '../../components/shell';
import type { FolderNode, SmartCollection } from '../../api/protocol';

export default function SidebarFolderTree() {
  const folderTree = useLibrary((s) => s.folderTree);
  const collapsedRoots = useLibrary((s) => s.collapsedRoots);
  const toggleRootCollapse = useLibrary((s) => s.toggleRootCollapse);
  const filter = useLibrary((s) => s.filter);
  const [collections, setCollections] = useState<SmartCollection[]>([]);
  const activeSmartId = useLibrary((s) => s.activeSmartId);

  useEffect(() => {
    api.library
      .listSmartCollections()
      .then(setCollections)
      .catch((err) => useJobStore.getState().toast(`智能收藏夹加载失败：${errText(err)}`, 'error'));
  }, []);

  return (
    <div className="flex-1 min-h-0 overflow-y-auto py-2">
      <Section title="目录" />
      <div className="px-1.5">
        {folderTree.map((root) => (
          <RootNode
            key={root.rootId}
            node={root}
            collapsed={collapsedRoots.includes(root.rootId)}
            onToggle={() => toggleRootCollapse(root.rootId)}
            activeFolderIds={filter.folderIds ?? []}
            activeRootIds={filter.rootIds ?? []}
          />
        ))}
      </div>

      <Section title="收藏夹" />
      <div className="px-1.5 flex flex-col gap-px">
        {collections.map((c) => (
          <button
            key={c.id}
            onClick={() => useLibrary.getState().setActiveSmart(c.id, c.filter)}
            className={`flex items-center gap-2 h-7 px-2 rounded-md text-xs text-left ${
              activeSmartId === c.id ? 'bg-[var(--Lumina-Tokens-accentSoft)] text-accent' : 'text-txt2 hover:bg-hover hover:text-txt'
            }`}
          >
            <Icon name="star" size={12} className={activeSmartId === c.id ? 'text-accent' : 'text-txt3'} />
            {c.name}
          </button>
        ))}
      </div>

      <Section title="智能收藏夹" />
      <SmartList />

      <Section title="时间线" />
      <TimelineStrip />
    </div>
  );
}

function Section({ title }: { title: string }) {
  return <div className="px-3 pt-3 pb-1 text-2xs text-txt3">{title}</div>;
}

function RootNode({ node, collapsed, onToggle, activeFolderIds, activeRootIds }: { node: FolderNode; collapsed: boolean; onToggle: () => void; activeFolderIds: number[]; activeRootIds: number[] }) {
  const rootActive = (filter: { rootIds?: number[] }) => (filter.rootIds ?? []).includes(node.rootId);
  void rootActive;
  return (
    <div>
      <button
        onClick={onToggle}
        className="w-full flex items-center gap-1.5 h-7 px-2 rounded-md text-xs text-txt2 hover:bg-hover hover:text-txt text-left"
      >
        <span className={`transition-transform text-txt3 ${collapsed ? '' : 'rotate-90'}`}>▸</span>
        <Icon name="folder" size={13} />
        <span className="truncate flex-1">{node.name}</span>
        <span className="num text-2xs text-txt3">{formatCount(node.count)}</span>
        {node.missing > 0 && <span className="w-1.5 h-1.5 rounded-full" style={{ background: 'var(--Lumina-Tokens-danger)' }} />}
      </button>
      {!collapsed && (
        <div className="ml-4">
          {node.children.map((child) => {
            const active = activeFolderIds.includes(child.id) || activeRootIds.includes(node.rootId);
            return (
              <button
                key={child.id}
                onClick={() =>
                  useLibrary.getState().setFilter({
                    ...useLibrary.getState().filter,
                    folderIds: [child.id],
                    rootIds: undefined,
                  })
                }
                className={`w-full flex items-center gap-1.5 h-7 px-2 rounded-md text-xs text-left ${
                  active ? 'bg-[var(--Lumina-Tokens-accentSoft)] text-accent' : 'text-txt2 hover:bg-hover hover:text-txt'
                }`}
              >
                <Icon name="folder" size={12} className={active ? 'text-accent' : 'text-txt3'} />
                <span className="truncate flex-1">{child.name}</span>
                <span className="num text-2xs text-txt3">{formatCount(child.count)}</span>
                {child.missing > 0 && <span className="w-1.5 h-1.5 rounded-full" style={{ background: 'var(--Lumina-Tokens-danger)' }} />}
              </button>
            );
          })}
        </div>
      )}
    </div>
  );
}

function SmartList() {
  const [smart, setSmart] = useState<SmartCollection[]>([]);
  useEffect(() => {
    api.library
      .listSmartCollections()
      .then(setSmart)
      .catch(() => undefined);
  }, []);
  return (
    <div className="px-1.5 flex flex-col gap-px">
      {smart.slice(0, 3).map((c) => (
        <button
          key={`smart-${c.id}`}
          onClick={() => useLibrary.getState().setActiveSmart(c.id, c.filter)}
          className="flex items-center gap-2 h-7 px-2 rounded-md text-xs text-txt2 hover:bg-hover hover:text-txt text-left"
        >
          <span className="text-txt3">◎</span>
          {c.name}
        </button>
      ))}
      <button
        onClick={() =>
          useLibrary.getState().setFilter({ ...useLibrary.getState().filter, edited: true })
        }
        className="flex items-center gap-2 h-7 px-2 rounded-md text-xs text-txt2 hover:bg-hover hover:text-txt text-left"
      >
        <span className="text-txt3">＋</span>
        按当前筛选新建
      </button>
    </div>
  );
}

function TimelineStrip() {
  const timeline = useLibrary((s) => s.timeline);
  const max = Math.max(1, ...timeline.map((t) => t.count));
  return (
    <div className="px-3 pb-4">
      <div className="flex items-end gap-1 h-14">
        {timeline.map((t) => {
          const currentMonth = t.month === new Date().toISOString().slice(0, 7);
          return (
            <button
              key={t.month}
              title={`${formatMonth(t.month)} · ${t.count} 张`}
              onClick={() => {
                const [y, m] = t.month.split('-').map(Number);
                const from = Date.UTC(y, m - 1, 1);
                const to = Date.UTC(y, m, 1) - 1;
                useLibrary.getState().setFilter({ ...useLibrary.getState().filter, dateFrom: from, dateTo: to });
              }}
              className="flex-1 flex flex-col items-center justify-end h-full group"
            >
              <span className="num text-2xs text-txt3 mb-0.5 opacity-0 group-hover:opacity-100">{t.count}</span>
              <div
                className="w-full rounded-t-sm transition-colors"
                style={{
                  height: `${Math.max(6, (t.count / max) * 80)}%`,
                  background: currentMonth ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-hover)',
                }}
              />
            </button>
          );
        })}
        {timeline.length === 0 && <span className="text-2xs text-txt3">暂无数据</span>}
      </div>
      <div className="flex justify-between mt-1 text-2xs text-txt3">
        <span>{timeline[0]?.month ?? ''}</span>
        <span>{timeline[timeline.length - 1]?.month ?? ''}</span>
      </div>
    </div>
  );
}
