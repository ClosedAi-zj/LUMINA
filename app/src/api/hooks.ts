/**
 * React 绑定：通用 RPC 查询 Hook、防抖值、job 进度订阅。
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import { rpc, RpcError } from './rpc-client';
import { useJobStore } from '../store/jobStore';

export interface RpcQueryState<T> {
  data: T | null;
  error: RpcError | null;
  loading: boolean;
  reload: () => void;
}

/** 简单请求-响应查询（deps 变化即重查；params=null 时不查询） */
export function useRpcQuery<T>(fetcher: () => Promise<T>, deps: unknown[], enabled = true): RpcQueryState<T> {
  const [state, setState] = useState<RpcQueryState<T>>({ data: null, error: null, loading: enabled, reload: () => undefined });
  const fetcherRef = useRef(fetcher);
  fetcherRef.current = fetcher;
  const [tick, setTick] = useState(0);

  useEffect(() => {
    if (!enabled) {
      setState({ data: null, error: null, loading: false, reload: () => setTick((t) => t + 1) });
      return;
    }
    let cancelled = false;
    setState((s) => ({ ...s, loading: true, error: null }));
    fetcherRef
      .current()
      .then((data) => {
        if (!cancelled) setState({ data, error: null, loading: false, reload: () => setTick((t) => t + 1) });
      })
      .catch((err) => {
        const e = err instanceof RpcError ? err : new RpcError(-32603, String(err));
        if (!cancelled) setState((s) => ({ ...s, error: e, loading: false, reload: () => setTick((t) => t + 1) }));
      });
    return () => {
      cancelled = true;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [...deps, tick, enabled]);

  return state;
}

export function useDebounced<T>(value: T, ms: number): T {
  const [debounced, setDebounced] = useState(value);
  useEffect(() => {
    const t = setTimeout(() => setDebounced(value), ms);
    return () => clearTimeout(t);
  }, [value, ms]);
  return debounced;
}

/** 订阅指定 jobId 的进度（事件由 jobStore 分发） */
export function useJobProgress(jobId: number | null) {
  const job = useJobStore((s) => (jobId !== null ? s.jobs[jobId] : undefined));
  return job ?? null;
}

/** 撤销型 toast（破坏性操作 5s 内可撤销，PRD §6.3） */
export function useUndoToast() {
  const addUndoToast = useJobStore((s) => s.addUndoToast);
  return useCallback((text: string, undoFn: () => Promise<unknown> | void) => addUndoToast(text, undoFn), [addUndoToast]);
}
