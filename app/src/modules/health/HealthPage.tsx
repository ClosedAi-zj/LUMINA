/**
 * 存储健康（帧 12）：四状态大数字行（已校验/全库 · 缺失 · NAS 双备份 · MD5）
 * + 异常清单三操作（重定位/孤儿/回填）+ 隔离视图（从 NAS 备份恢复）+ 显式告知承诺区（R3 零静默）。
 */

import { useCallback, useEffect, useState } from 'react';
import { api } from '../../api/engine';
import { errText } from '../../api/rpc-client';
import { useJobStore } from '../../store/jobStore';
import { useJobProgress, useRpcQuery } from '../../api/hooks';
import { Button, Chip } from '../../components/primitives';
import { Dialog, Icon } from '../../components/shell';
import { formatCount, formatRelative } from '../../utils/format';
import type { HealthIssueItem, HealthStatus } from '../../api/protocol';

export default function HealthPage() {
  const [status, setStatus] = useState<HealthStatus | null>(null);
  const [tab, setTab] = useState<'missing' | 'corrupt'>('missing');
  const [issues, setIssues] = useState<HealthIssueItem[]>([]);
  const [total, setTotal] = useState(0);
  const [relocateTarget, setRelocateTarget] = useState<HealthIssueItem | null>(null);
  const [newPath, setNewPath] = useState('');
  const healthIssueCount = useJobStore((s) => s.healthIssueCount);
  const scanJob = useJobProgress(null);
  void scanJob;

  const loadStatus = useCallback(async () => {
    try {
      const s = await api.health.getStatus();
      setStatus(s);
    } catch (err) {
      useJobStore.getState().toast(`健康状态加载失败：${errText(err)}`, 'error');
    }
  }, []);

  const loadIssues = useCallback(async () => {
    try {
      const res = await api.health.listIssues(tab, { offset: 0, limit: 100 });
      setIssues(res.items);
      setTotal(res.total);
    } catch (err) {
      useJobStore.getState().toast(`异常清单加载失败：${errText(err)}`, 'error');
    }
  }, [tab]);

  useEffect(() => {
    void loadStatus();
  }, [loadStatus]);
  useEffect(() => {
    void loadIssues();
  }, [loadIssues]);
  // 5s 轮询状态（实时监测事件的补充）
  useEffect(() => {
    const t = setInterval(() => void loadStatus(), 5000);
    return () => clearInterval(t);
  }, [loadStatus]);

  const resolve = async (issue: HealthIssueItem, action: 'relocate' | 'orphan' | 'backfill' | 'restore', path?: string) => {
    try {
      await api.health.resolveIssue(issue.issueId, action, path);
      useJobStore.getState().toast(
        action === 'relocate' ? '已重定位' : action === 'orphan' ? '已标记孤儿（保留缺失标记，可随时重定位）' : action === 'backfill' ? '已用衍生件回填' : '已从备份恢复',
        'success',
      );
      await Promise.all([loadIssues(), loadStatus()]);
      useJobStore.getState().clearHealthIssues();
    } catch (err) {
      useJobStore.getState().toast(`处理失败：${errText(err)}`, 'error');
    }
  };

  return (
    <div className="h-full overflow-y-auto p-5" style={{ background: 'var(--Lumina-Tokens-canvas)' }}>
      <div className="flex items-center gap-3 mb-5">
        <h2 className="text-base font-semibold">存储健康</h2>
        {status?.abnormal && <span className="w-2.5 h-2.5 rounded-full" style={{ background: 'var(--Lumina-Tokens-danger)' }} />}
        <div className="flex-1" />
        <Button
          onClick={async () => {
            const { jobId } = await api.health.scanFull();
            useJobStore.getState().toast(`全库扫描已启动（任务 ${jobId}）`, 'info');
          }}
        >
          立即全库扫描
        </Button>
        <Button
          onClick={async () => {
            const { jobId } = await api.health.verifyPhotos();
            useJobStore.getState().toast(`批量 MD5 校验已启动（任务 ${jobId}）`, 'info');
          }}
        >
          批量 MD5 校验
        </Button>
      </div>

      {/* 四状态大数字行 */}
      {status && (
        <div className="grid grid-cols-4 gap-3 mb-5">
          <BigStat
            label="已校验 / 全库"
            value={`${formatCount(status.verified)}`}
            sub={`全库 ${formatCount(status.total)}`}
            ok={status.verified === status.total}
          />
          <BigStat
            label="缺失文件（实时监测）"
            value={formatCount(status.missing)}
            sub="ReadDirectoryChangesW + 启动兜底扫描"
            ok={status.missing === 0}
          />
          <BigStat
            label="NAS 双备份"
            value={status.backup.state === 'ok' ? '正常' : status.backup.state === 'warning' ? '预警' : '未配置'}
            sub={`${status.backup.target} · ${formatRelative(status.backup.lastSuccessAt)}`}
            ok={status.backup.state === 'ok'}
          />
          <BigStat
            label="MD5 校验"
            value={`${status.md5.percent >= 100 ? '100' : status.md5.percent.toFixed(1).replace(/\.0$/, '')}%`}
            sub={status.md5.running ? '后台进行中…' : '后台完成'}
            ok={status.md5.percent >= 97}
          />
        </div>
      )}

      {/* 显式告知承诺区（R3） */}
      <div className="rounded-lg border border-line p-4 mb-5" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <div className="flex items-center gap-2 mb-1.5">
          <Icon name="warning" size={14} className="text-accent" />
          <span className="text-xs text-txt font-medium">对比 LR · 静默缺失 423 张的惨剧不再发生</span>
        </div>
        <div className="text-2xs text-txt3 leading-5">
          任何缺失 / 校验失败 / 不可读都会在此显式呈现（红点 + 清单 + toast），绝不静默吞错。文件系统实时监测由引擎自动开启；异常清单每一条都有处理出口。
          {healthIssueCount > 0 && <span className="text-accent">（本次会话新收到 {healthIssueCount} 条异常事件）</span>}
        </div>
      </div>

      {/* 异常清单 */}
      <div className="rounded-lg border border-line overflow-hidden" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <div className="flex items-center gap-2 px-3 h-11 border-b border-line">
          <Chip label={`缺失（${status?.missing ?? '…'}）`} active={tab === 'missing'} onClick={() => setTab('missing')} />
          <Chip label={`校验失败 · 隔离（${status?.corrupt ?? 0}）`} active={tab === 'corrupt'} onClick={() => setTab('corrupt')} />
          <div className="flex-1" />
          <span className="text-2xs text-txt3 num">共 {formatCount(total)} 条</span>
        </div>
        <div className="max-h-[420px] overflow-y-auto">
          {issues.length === 0 && (
            <div className="py-10 text-center text-2xs text-txt3">{tab === 'corrupt' ? '隔离视图为空 · 未发现校验失败文件' : '无缺失文件 · 实时监测中'}</div>
          )}
          {issues.map((issue) => (
            <div key={issue.issueId} className="flex items-center gap-3 px-3 h-12 border-b border-line last:border-b-0 hover:bg-hover text-xs">
              <span className="w-1.5 h-1.5 rounded-full shrink-0" style={{ background: issue.kind === 'corrupt' ? 'var(--Lumina-Tokens-danger)' : 'var(--Lumina-Tokens-accent)' }} />
              <div className="flex-1 min-w-0">
                <div className="num text-2xs text-txt2 truncate">{issue.path}</div>
                <div className="text-2xs text-txt3">
                  {issue.kind === 'corrupt' ? 'MD5 不符 · 已隔离' : '最后已知路径如上'} · 检测于 {formatRelative(issue.detectedAt)}
                </div>
              </div>
              {issue.kind === 'missing' ? (
                <div className="flex items-center gap-1.5 shrink-0">
                  <Button
                    size="sm"
                    onClick={() => {
                      setRelocateTarget(issue);
                      setNewPath(issue.path.replace(/[^/]+$/, '') /* 默认同目录 */);
                    }}
                  >
                    重定位
                  </Button>
                  <Button size="sm" onClick={() => void resolve(issue, 'orphan')}>
                    标记孤儿
                  </Button>
                  <Button size="sm" onClick={() => void resolve(issue, 'backfill')}>
                    衍生件回填
                  </Button>
                </div>
              ) : (
                <div className="flex items-center gap-1.5 shrink-0">
                  <Button
                    size="sm"
                    variant="primary"
                    onClick={async () => {
                      try {
                        const { jobId } = await api.health.restoreFromBackup(issue.photoId);
                        useJobStore.getState().toast(`正在从 NAS 备份恢复（任务 ${jobId}）…`, 'info');
                      } catch (err) {
                        useJobStore.getState().toast(`恢复失败：${errText(err)}`, 'error');
                      }
                    }}
                  >
                    从 NAS 备份恢复
                  </Button>
                </div>
              )}
            </div>
          ))}
        </div>
      </div>

      {/* 重定位对话框 */}
      <Dialog open={relocateTarget !== null} title="重定位文件" onClose={() => setRelocateTarget(null)} width={480}>
        <div className="text-2xs text-txt3 mb-1">原路径</div>
        <div className="num text-2xs text-txt2 mb-3 break-all">{relocateTarget?.path}</div>
        <div className="text-2xs text-txt3 mb-1">新路径</div>
        <input
          value={newPath}
          onChange={(e) => setNewPath(e.target.value)}
          className="w-full h-8 px-2.5 rounded-md border border-line bg-card text-xs num focus:outline-none focus:border-accent"
        />
        <div className="flex justify-end gap-2 mt-4">
          <Button onClick={() => setRelocateTarget(null)}>取消</Button>
          <Button
            variant="primary"
            onClick={() => {
              if (relocateTarget && newPath) void resolve(relocateTarget, 'relocate', newPath);
              setRelocateTarget(null);
            }}
          >
            确认重定位
          </Button>
        </div>
      </Dialog>
    </div>
  );
}

function BigStat({ label, value, sub, ok }: { label: string; value: string; sub: string; ok: boolean }) {
  return (
    <div className="rounded-lg border border-line p-4" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center gap-1.5 text-2xs text-txt3 mb-1.5">
        {!ok && <span className="w-1.5 h-1.5 rounded-full" style={{ background: 'var(--Lumina-Tokens-danger)' }} />}
        {label}
      </div>
      <div className="num text-2xl font-semibold" style={{ color: ok ? 'var(--Lumina-Tokens-textPrimary)' : 'var(--Lumina-Tokens-danger)' }}>
        {value}
      </div>
      <div className="text-2xs text-txt3 mt-1 truncate">{sub}</div>
    </div>
  );
}
