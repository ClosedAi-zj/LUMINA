/**
 * 左侧栏（阶段4 01-3）：文件夹树 + 智能收藏夹 + 时间线，目录树下方补
 * 「收藏集」（albums kind=collection）与「关键词」两个分组。
 * 零静默红线：引擎无关键词聚合接口（keywords 只在 PhotoDetail 单照片级），
 * 关键词组显示空态「暂无」，不伪造数据。
 */

import { useEffect, useState } from 'react';
import { useLibrary } from '../../store/libraryStore';
import { useUi } from '../../store/uiStore';
import { usePortfolio } from '../portfolio/store/portfolioStore';
import { api } from '../../api/engine';
import { errText } from '../../api/rpc-client';
import { useJobStore } from '../../store/jobStore';
import { formatCount, formatMonth } from '../../utils/format';
import { Icon } from '../../components/shell';
import type { AlbumInfo, FolderNode, SmartCollection } from '../../api/protocol';

export default function SidebarFolderTree() {
  const folderTree = useLibrary((s) => s.folderTree);
  const expandedRoots = useLibrary((s) => s.expandedRoots);
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
            collapsed={!expandedRoots.includes(root.rootId)}
            onToggle={() => toggleRootCollapse(root.rootId)}
            activeFolderIds={filter.folderIds ?? []}
            activeRootIds={filter.rootIds ?? []}
          />
        ))}
      </div>

      {/* 阶段4 01-3：目录树正下方补「收藏集」「关键词」两分组 */}
      <Section title="收藏集" />
      <CollectionsList />
      <Section title="关键词" />
      <KeywordsList />

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

/** 收藏集（albums kind=collection）：点击跳转作品集工作区并选中该集合 */
function CollectionsList() {
  const [albums, setAlbums] = useState<AlbumInfo[]>([]);
  const [err, setErr] = useState<string | null>(null);
  useEffect(() => {
    api.albums
      .list()
      .then((list) => {
        setAlbums(list.filter((a) => a.kind === 'collection'));
        setErr(null);
      })
      .catch((e) => setErr(errText(e)));
  }, []);
  return (
    <div className="px-1.5 flex flex-col gap-px">
      {err && <div className="px-2 py-1 text-2xs" style={{ color: 'var(--Lumina-Tokens-danger)' }}>{err}</div>}
      {!err && albums.length === 0 && <div className="px-2 py-1 text-2xs text-txt3">暂无</div>}
      {albums.map((a) => (
        <button
          key={`coll-${a.id}`}
          onClick={() => {
            usePortfolio.getState().select(a.id);
            useUi.getState().navigate('portfolio');
          }}
          className="flex items-center gap-2 h-7 px-2 rounded-md text-xs text-txt2 hover:bg-hover hover:text-txt text-left"
          title="在作品集工作区打开"
        >
          <Icon name="folder" size={12} className="text-txt3" />
          <span className="truncate flex-1">{a.name}</span>
          <span className="num text-2xs text-txt3">{a.itemCount}</span>
        </button>
      ))}
    </div>
  );
}

/**
 * 关键词分组：引擎无关键词聚合接口（keywords 仅在 PhotoDetail 单照片级返回），
 * 按零静默红线显示空态「暂无」，不静默造假数据；聚合接口交付后在此接入。
 */
function KeywordsList() {
  return <div className="px-2 py-1 text-2xs text-txt3">暂无</div>;
}

function TimelineStrip() {  const timeline = useLibrary((s) => s.timeline);
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
