/**
 * 引擎事件 → store 统一路由（App 挂载时启动一次）。
 * 独立成文件以避免 store 间循环依赖。
 */

import { rpc } from '../api/rpc-client';
import { useJobStore } from './jobStore';
import { useLibrary } from './libraryStore';
import type { FsChangedEvent, HealthIssueEvent, ThumbReadyEvent } from '../api/protocol';

export function startEventPump(): void {
  rpc.start();
  rpc.on('event.job.progress', (params) => useJobStore.getState().onJobProgress(params as never));
  rpc.on('event.job.completed', (params) => useJobStore.getState().onJobCompleted(params as never));
  rpc.on('event.import.fileResult', (params) => useJobStore.getState().onImportFileResult(params as never));
  rpc.on('event.engine.log', (params) => useJobStore.getState().onEngineLog(params as never));
  rpc.on('event.thumb.ready', (params) => {
    const e = params as ThumbReadyEvent;
    useLibrary.getState().applyThumbReady(e.photoId, e.path);
  });
  rpc.on('event.fs.changed', (params) => {
    const e = params as FsChangedEvent;
    // 红线 R3：任何异常不得静默 —— 红点 + toast + 刷新元数据
    useJobStore.getState().bumpHealthIssues();
    useJobStore.getState().toast(`文件系统变化：${e.change === 'missing' ? '文件缺失' : e.change === 'restored' ? '文件恢复' : '文件修改'} · ${e.path}`, 'warn', 8000);
    void useLibrary.getState().loadMeta();
  });
  rpc.on('event.health.issue', (params) => {
    const e = params as HealthIssueEvent;
    useJobStore.getState().bumpHealthIssues();
    useJobStore
      .getState()
      .toast(`${e.kind === 'corrupt' ? '校验失败（MD5 不符），已进隔离视图' : '文件缺失'} · photoId=${e.photoId}`, 'error', 8000);
  });
}
