/**
 * 修图会话 store：快速/专业共用同一参数模型（PRD §6.1 —— 禁止两套参数状态）。
 * 滑块拖动 → 本地乐观更新（WebGL 即时渲染）+ edit.setParams(commitHistory=false)；
 * 松手 → commitHistory=true（引擎侧 500ms 防抖落盘 sidecar）。
 */

import { create } from 'zustand';
import { api } from '../api/engine';
import { errText } from '../api/rpc-client';
import type { AdjustmentLayer, EditParams, HslBand, MaskLayer } from '../api/protocol';
import type { Hsl } from '../api/protocol';
import { DEFAULT_EDIT_PARAMS } from '../api/protocol';
import { MAX_MASK_SLOTS } from '../modules/develop/webgl/maskField';

export type DevelopMode = 'fast' | 'pro';
/**
 * XMP 落盘指示灯（状态机与「真实落盘」严格对齐，全部凭引擎返回值，不自行判断）：
 *  - 'idle'       参数未修改
 *  - 'debouncing' 引擎已接受参数（setParams/undo/redo 成功），但落盘还在防抖窗口内 → 只能报「待落盘」
 *  - 'written'    仅 endSession{flush:true} 返回 flushed && sidecarMtime !== null 时才成立
 *  - 'error'      最近一次写库/落盘失败（界面不得停在假成功态）
 */
export type XmpStatus = 'idle' | 'debouncing' | 'written' | 'error';
export type GuidesMode = 'off' | 'thirds' | 'center' | 'grid';

/** G9 文档 Tab 上限：每个 Tab 都要引擎建会话并保留一份解码底图，10 个是内存/心智双上限 */
export const MAX_DEVELOP_TABS = 10;

/** 把 id 并进 Tab 列表：已在列表里则**原地不动**（切 Tab 时顺序不能跳），
 *  新 id 追加到末尾；按上限挤掉最早打开的（keep 里的 id 永不被挤）。 */
function pushTab(tabs: number[], id: number, keep: number[] = []): number[] {
  if (tabs.includes(id)) return tabs;
  const out = [...tabs, id];
  while (out.length > MAX_DEVELOP_TABS) {
    const victim = out.find((t) => !keep.includes(t));
    if (victim === undefined) break;
    out.splice(out.indexOf(victim), 1);
  }
  return out;
}

/** 历史步骤容量：只保留**最近** 200 条（早于 200 步的步骤回放不了，但最新的一定在） */
const HISTORY_LIMIT = 200;

/**
 * 打开文档的并发序号（模块级，不进 store——它只是竞态令牌，不参与渲染）。
 *
 * 背景：openPhoto 内部有两次 await（endSession → beginSession/getBase）。两次快速切图时，
 * 后发请求的结果可能先到，先发请求的结果后到并覆盖 store，于是出现
 * `photoId=B / sessionId=A`（滑块读 B 的参数、写 A 的会话）；catch 分支不清 sessionId 时，
 * 之后所有 applyPatch 都会因 `!sessionId` 静默返回（滑块点了没反应）。
 * 每次发起打开（openPhoto/openPath/closePhoto）都自增此序号，await 之后比对：
 * 只有「最新一次」的响应可以写回 store，过期的响应一律丢弃并自行清理它创建的会话。
 */
let openSeq = 0;

/** 历史快照条目（UI 侧参数快照，供专业模式点击跳转回放） */
export interface HistoryEntry {
  label: string;
  params: EditParams;
  time: number;
}

/** 调整图层混合模式（协议 blendMode 为开放枚举，此处为 UI 白名单） */
export const BLEND_MODES: { value: string; label: string }[] = [
  { value: 'normal', label: '正常' },
  { value: 'multiply', label: '变暗 · 正片叠底' },
  { value: 'screen', label: '变亮 · 滤色' },
  { value: 'overlay', label: '叠加' },
  { value: 'softLight', label: '柔光' },
  { value: 'hardLight', label: '强光' },
  { value: 'lighten', label: '变亮' },
  { value: 'darken', label: '变暗' },
  { value: 'luminosity', label: '明度' },
  { value: 'color', label: '颜色' },
];

export function blendLabel(v: string): string {
  return BLEND_MODES.find((b) => b.value === v)?.label ?? v;
}

// ---------- 错误原文判据（业务边界 vs 真失败） ----------

/**
 * 引擎错误**原文**（用于区分「同一个错误码下的不同业务情形」）。
 *
 * 为什么不用 `errText(err)`：它返回「码 → 中文文案」映射
 * （`api/protocol.ts:702-728`，-32602 恒为「参数校验失败」、-32603 恒为「引擎内部错误」），
 * 会把原文里的 `nothing to undo` / `session not found` 整段丢掉 —— 这两个都报 -32602，
 * 文案完全一样，但一个是「正常边界，不该提示」，一个是「会话没了，要提示」。
 * 为什么也不用 code：同理，-32602 同时承载多种业务边界，码本身区分不出来。
 *
 * （B8 之后 IPC 边界已经能保住 code 与原文，见 `api/rpc-client.ts` 的
 *  `RpcErrorEnvelope` 注释；但「按原文判边界」这条策略仍然成立且更精确。）
 */
function errRaw(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

/** 引擎侧「会话已不存在」= 同一会话被关两次的良性边界（不是落盘失败） */
function isSessionGone(err: unknown): boolean {
  return /session not found/i.test(errRaw(err));
}

/** 引擎侧「历史栈已空」= 撤销/重做的正常边界（不是写库失败） */
function isHistoryEmpty(err: unknown, dir: 'undo' | 'redo'): boolean {
  return new RegExp(`nothing to ${dir}`, 'i').test(errRaw(err));
}

// ---------- temperature 单位契约（v1.2.0 T1.7 收敛，全应用单点） ----------
// 引擎/oracle 把 basic.temperature 当「0 为中性」的归一化值（basic.cpp TemperatureOp:
// ev = t*0.002，±100 ≈ ±0.2EV，t>0 暖）；开尔文只是 UI/预设/侧车的显示口径。
// Kelvin → 归一化转换只在本文件发生：
//  - applyPatch 入口把 Kelvin 大值归一化后再本地合并 + edit.setParams 落盘（引擎边界收到归一化值）；
//  - 引擎回传（normalizeParams：beginSession / undo / redo / 侧车读回，mock 为 5200–6400K）同样归一化后进 store；
//  - 面板显示用 temperatureToKelvin 还原开尔文（UI 口径仍可开尔文）。
export const WB_KELVIN_MIN = 2000;
export const WB_KELVIN_NEUTRAL = 5500;
export const WB_KELVIN_SPAN = 6500; // 5500K（中性）→ 12000K 映射归一化 0 → +100

export function kelvinToTemperature(k: number): number {
  return ((k - WB_KELVIN_NEUTRAL) / WB_KELVIN_SPAN) * 100;
}
export function temperatureToKelvin(t: number): number {
  return (t / 100) * WB_KELVIN_SPAN + WB_KELVIN_NEUTRAL;
}
/** 双口径归一：绝对色温（|v| ≥ 2000K）→ 归一化；已是归一化口径（|v| ≤ 100）原样返回。
 *  两口径区间天然不重叠（归一化上限 +100），判定无歧义；快照回放/引擎回传已归一化的值不会被二次转换。 */
export function normalizeTemperature(v: number): number {
  return Math.abs(v) >= WB_KELVIN_MIN ? kelvinToTemperature(v) : v;
}

// ---------- T2.12：蒙版语义升级首启标记（跨代理契约：键名 m2MaskSemanticsNotified 不许改） ----------

const MASK_SEMANTICS_NOTICE_KEY = 'm2MaskSemanticsNotified';

function readMaskSemanticsNotified(): boolean {
  try {
    return localStorage.getItem(MASK_SEMANTICS_NOTICE_KEY) === 'true';
  } catch {
    // localStorage 不可用（极少数隐私场景）→ 按未告知处理，横幅每次显示（可见性优先）
    return false;
  }
}

interface DevelopStore {
  photoId: number | null;
  sessionId: number | null;
  params: EditParams | null;
  revision: number;
  baseImageUrl: string | null;
  baseImageReady: boolean;
  beforeParams: EditParams | null;
  mode: DevelopMode;
  xmpStatus: XmpStatus;
  historyLabels: string[];
  canUndo: boolean;
  canRedo: boolean;
  selectedMaskId: string | null;
  maskTool: 'linear' | 'radial' | 'brush' | null;
  syncMessage: string | null;
  loading: boolean;
  /** 协议 v1.0.1：最后一次 endSession{flush:true} 返回的 sidecarMtime（null=未确认落盘，禁示"已写入"） */
  lastFlushMtime: number | null;
  /**
   * 最后一次「引擎已确认接受」的参数（= 写库失败时的回滚原点）。
   *
   * 不能用「本次 patch 前」的快照当原点：连续两次编辑都失败时，第二次的原点是
   * 第一次的**乐观**结果 —— 服务端从未接受过它，于是界面停在 A+B，XMP 里两者都没有。
   * 打开照片/文件时置为初始参数（此时引擎状态 = 初始参数），此后每次 setParams
   * 成功都推进到当时合并出的 params。
   */
  lastCommittedParams: EditParams | null;
  /** T2.12：蒙版渲染语义升级一次性告知 —— 首启标记（localStorage 键 m2MaskSemanticsNotified，falsy=横幅显示） */
  maskSemanticsNotified: boolean;
  /** G9 文档 Tab（会话级多开照片，含当前 photoId；不含未打开的库内照片）。
   *  语义裁定见 docs/v1.2.0/UI-对齐差距评估与改版清单.md §五-D1：
   *  产品是「库 + 单图」模型，Tab = 图库多选后接力修图的已开文档，不是 PS 的画布多文档。
   *  上限 MAX_DEVELOP_TABS，超出时挤掉最早打开的（当前照片永不被挤）。 */
  tabs: number[];

  // ---- 专业模式（与快速模式共享同一 params，以下为 UI 侧状态 + params 扩展读写） ----
  /** 历史快照（与 historyLabels 一一对应，commit 时记录） */
  historySnapshots: HistoryEntry[];
  /** 当前选中调整图层 */
  selectedLayerId: string | null;
  /** 锁定的图层（UI 本地状态，协议无锁定字段，不落盘） */
  lockedLayerIds: string[];
  /** 参考线模式（UI 叠加层，不落盘） */
  guides: GuidesMode;

  openPhoto: (id: number) => Promise<void>;
  closePhoto: () => Promise<void>;
  /** M3-03：显式打开一个 PSD（或图像）文件作为「合并像素层」预览（只读，不入图库会话）。 */
  openPath: (path: string) => Promise<void>;
  /** G9：批量打开（首张成为当前文档，其余进 Tab 列表）——图库多选「接力修图」入口 */
  openTabs: (ids: number[]) => Promise<void>;
  /** G9：关闭某个文档 Tab；关掉当前时切到相邻 Tab，Tab 清空则结束会话 */
  closeTab: (id: number) => Promise<void>;
  setMode: (m: DevelopMode) => void;
  applyPatch: (patch: Partial<EditParams>, commit: boolean, label?: string) => Promise<void>;
  setNumeric: (key: keyof EditParams, value: number, commit: boolean) => Promise<void>;
  setHsl: (band: HslBand, key: keyof Hsl, value: number, commit: boolean) => Promise<void>;
  undo: () => Promise<void>;
  redo: () => Promise<void>;
  reset: () => Promise<void>;
  addMask: (type: MaskLayer['type']) => Promise<void>;
  updateMask: (maskId: string, patch: Partial<MaskLayer>) => Promise<void>;
  /** 蒙版离散字段（amount / opacity 等）拖动：拖动中 commit=false 只本地更新，
   *  松手 commit=true 提交一次并落历史（不再「每帧一次 commit」刷满历史） */
  updateMaskField: (maskId: string, patch: Partial<MaskLayer>, commit: boolean, label?: string) => Promise<void>;
  updateMaskLocal: (maskId: string, key: keyof MaskLayer['local'], value: number, commit: boolean) => Promise<void>;
  removeMask: (maskId: string) => Promise<void>;
  appendStroke: (maskId: string, stroke: NonNullable<MaskLayer['strokes']>[number], commit: boolean) => Promise<void>;
  selectMask: (id: string | null) => void;
  setMaskTool: (t: 'linear' | 'radial' | 'brush' | null) => void;
  applyPreset: (params: EditParams, name: string) => Promise<void>;
  saveSnapshot: (name: string) => Promise<void>;
  syncToSelection: (targetIds: number[]) => Promise<void>;
  syncToSequence: (seqId: number) => Promise<void>;
  toggleBeforeAfter: () => void;
  /** T2.12：点击「知道了」——置 localStorage 标记 + 关横幅（此后不再显示） */
  dismissMaskSemanticsNotice: () => void;

  // ---- 专业模式 actions ----
  /** 点路径设值：'detail.sharpness' / 'geometry.perspective.rotate' / 'exposure' */
  setPath: (path: string, value: number | boolean | string, commit: boolean, label?: string) => Promise<void>;
  /** 历史跳转（参数快照回放） */
  jumpHistory: (index: number) => Promise<void>;
  addLayer: (kind: string, name?: string) => Promise<void>;
  updateLayer: (id: string, patch: Partial<AdjustmentLayer>) => Promise<void>;
  /** 图层离散字段（opacity 等）拖动：拖动中 commit=false，松手 commit=true 提交一次 */
  updateLayerField: (id: string, patch: Partial<AdjustmentLayer>, commit: boolean, label?: string) => Promise<void>;
  /** 图层内参数滑块（图层.params 顶层键） */
  updateLayerParam: (id: string, key: keyof EditParams, value: number, commit: boolean) => Promise<void>;
  removeLayer: (id: string) => Promise<void>;
  moveLayer: (id: string, dir: -1 | 1) => Promise<void>;
  renameLayer: (id: string, name: string) => Promise<void>;
  selectLayer: (id: string | null) => void;
  /** 锁定为 UI 本地状态（协议 AdjustmentLayer 无 locked 字段） */
  toggleLayerLock: (id: string) => void;
  setGuides: (g: GuidesMode) => void;
}

export const useDevelop = create<DevelopStore>((set, get) => ({
  photoId: null,
  sessionId: null,
  params: null,
  revision: 0,
  baseImageUrl: null,
  baseImageReady: false,
  beforeParams: null,
  mode: 'fast',
  xmpStatus: 'idle',
  historyLabels: [],
  canUndo: false,
  canRedo: false,
  selectedMaskId: null,
  maskTool: null,
  syncMessage: null,
  loading: false,
  lastFlushMtime: null,
  lastCommittedParams: null,
  maskSemanticsNotified: readMaskSemanticsNotified(),
  historySnapshots: [],
  selectedLayerId: null,
  lockedLayerIds: [],
  guides: 'off',
  tabs: [],

  openPhoto: async (id) => {
    // 竞态令牌：本次打开的唯一序号；每个 await 之后比对，过期即丢弃（见 openSeq 注释）
    const mySeq = ++openSeq;
    // 切图/关图必须先 endSession{flush:true}（协议 §5.5 / v1.0.1）
    const prev = get();
    if (prev.sessionId !== null) {
      try {
        // 必须 await 返回值：凭 sidecarMtime 确认上一张参数已落盘，不自行判断是否写入
        const end = await api.edit.endSession(prev.sessionId, true);
        if (openSeq !== mySeq) return; // 期间又开始了新的打开：本次整包放弃
        if (end.flushed && end.sidecarMtime !== null) set({ xmpStatus: 'written', lastFlushMtime: end.sidecarMtime });
      } catch (err) {
        if (openSeq !== mySeq) return;
        // 会话已被别处关掉（closeTab / Esc 连发）不是「清理失败」：
        // 弹「其参数可能未写入 XMP」会让用户以为丢了编辑
        if (!isSessionGone(err)) {
          const { useJobStore } = await import('./jobStore');
          useJobStore.getState().toast('切换照片：上一张的会话清理失败，其参数可能未写入 XMP', 'warn', 6000);
        }
      }
    }
    // sessionId 立刻置空：新会话尚未建立前，任何 applyPatch 都必须落到「无会话」的空转分支，
    // 绝不能继续写进上一张的会话（否则参数会串图落盘）。
    // params 同时置 null：面板在加载期不能渲染上一张的参数（否则切图瞬间闪一帧旧值，
    // 且该帧里的拖动会把这些旧值写进新会话）；ViewerCanvas 已有 loading 遮罩，各面板均判空返回。
    set({ loading: true, photoId: id, sessionId: null, params: null, beforeParams: null, lastCommittedParams: null, baseImageReady: false, baseImageUrl: null, selectedMaskId: null, historyLabels: [], canUndo: false, canRedo: false, xmpStatus: 'idle', historySnapshots: [], selectedLayerId: null });
    try {
      const [session, base] = await Promise.all([api.edit.beginSession(id), api.preview.getBase(id)]);
      if (openSeq !== mySeq) {
        // 过期响应：它创建的会话没有任何 UI 持有，主动 endSession 清理（不 flush，
        // 否则会把「用户根本没看到的那张」的默认参数写进 sidecar）
        try {
          await api.edit.endSession(session.sessionId, false);
        } catch {
          /* 清理失败不影响新请求的结果，也不该盖掉新请求的提示 */
        }
        return;
      }
      const normalized = normalizeParams(session.params);
      set((s) => ({
        sessionId: session.sessionId,
        params: normalized,
        revision: session.revision,
        beforeParams: structuredClone(normalized),
        // 回滚原点：此刻引擎状态就是这份初始参数
        lastCommittedParams: structuredClone(normalized),
        baseImageUrl: base.path,
        baseImageReady: true,
        loading: false,
        historyLabels: ['导入原状'],
        historySnapshots: [{ label: '导入原状', params: structuredClone(normalized), time: Date.now() }],
        // G9：打开成功的照片进入文档 Tab（切 Tab 与首次打开走同一条路径）
        tabs: pushTab(s.tabs, id, [id]),
      }));
    } catch (err) {
      if (openSeq !== mySeq) return;
      // 失败必须回滚到「无文档」态：残留 photoId 会让人以为照片已打开，
      // 而 sessionId 已空 → applyPatch 静默返回（滑块点了没反应）
      set({
        loading: false,
        photoId: null,
        sessionId: null,
        params: null,
        beforeParams: null,
        lastCommittedParams: null,
        baseImageUrl: null,
        baseImageReady: false,
        historyLabels: [],
        historySnapshots: [],
        canUndo: false,
        canRedo: false,
        xmpStatus: 'idle',
        selectedLayerId: null,
        selectedMaskId: null,
      });
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`无法打开照片：${errText(err)}`, 'error', 8000);
    }
  },

  // M3-03：显式打开 PSD/图像文件作为「合并像素层」只读预览（不入图库会话、无 sidecar 参数）。
  // develop.render 按 inputPath 解码（PSD 走 decodePsdComposite；图像走通用解码），故无需 photoId。
  openPath: async (path) => {
    // 与 openPhoto 共用同一竞态序号：打开 PSD 合并预览同样会取消在途的图库照片打开
    const mySeq = ++openSeq;
    const prev = get();
    if (prev.sessionId !== null) {
      try {
        const end = await api.edit.endSession(prev.sessionId, true);
        if (openSeq !== mySeq) return;
        if (end.flushed && end.sidecarMtime !== null) set({ xmpStatus: 'written', lastFlushMtime: end.sidecarMtime });
      } catch (err) {
        if (openSeq !== mySeq) return;
        // 同 openPhoto：会话已不存在不是清理失败，不弹「参数可能未写入 XMP」
        if (!isSessionGone(err)) {
          const { useJobStore } = await import('./jobStore');
          useJobStore.getState().toast('切换文件：上一张的会话清理失败，其参数可能未写入 XMP', 'warn', 6000);
        }
      }
    }
    if (openSeq !== mySeq) return;
    set({
      photoId: null,
      sessionId: null,
      params: structuredClone(DEFAULT_EDIT_PARAMS),
      revision: 0,
      baseImageUrl: path,
      baseImageReady: true,
      loading: false,
      beforeParams: null,
      // 合并预览无会话/无 sidecar：任何写库都不会发生，回滚原点即当前展示的默认参数
      lastCommittedParams: structuredClone(DEFAULT_EDIT_PARAMS),
      selectedMaskId: null,
      historyLabels: ['合并预览'],
      historySnapshots: [],
      tabs: [],
      xmpStatus: 'idle',
    });
    // 判来源 → 显式告知（外来 = 非 LUMINA 导出，调整历史不可编辑）
    try {
      const info = await api.psd.inspect(path);
      if (openSeq !== mySeq) return; // 期间已切到别的文档：这条告知已无意义
      const foreign = info.foreign === true;
      const { useJobStore } = await import('./jobStore');
      if (foreign) {
        useJobStore.getState().toast(
          '此 PSD 非 LUMINA 导出，已按合并像素图层打开（调整历史不可编辑）',
          'info',
          6000,
        );
      } else {
        useJobStore.getState().toast(
          '已打开 LUMINA 导出的 PSD（v1.2.0 为合并预览，节点编辑还原将在 v2 提供）',
          'info',
          6000,
        );
      }
    } catch {
      /* inspect 失败（文件非 PSD 等）→ 静默，预览由 develop.render 兜底 */
    }
    if (openSeq !== mySeq) return;
    const { useUi } = await import('./uiStore');
    useUi.getState().navigate('develop');
  },

  closePhoto: async () => {
    // 关会话同样取消在途打开：否则「Esc 返回图库」后旧响应可能把文档又写回来。
    // mySeq 供尾部使用：关图期间用户可能立刻打开了另一张，此时本次的响应必须先作废，
    // 否则会把刚建立的新会话清掉（空白修图台 + 会话泄漏）。
    const mySeq = ++openSeq;
    const { sessionId } = get();
    if (sessionId !== null) {
      try {
        // 协议 v1.0.1：退出修图台必须 await endSession{flush:true} 的返回值，
        // 用返回的 sidecarMtime 熄灭「参数已写入 XMP」指示灯；不得自行判断是否已写入。
        const end = await api.edit.endSession(sessionId, true);
        if (openSeq !== mySeq) return; // 期间已开始新的打开：这条响应已过期
        if (end.flushed && end.sidecarMtime !== null) {
          set({ xmpStatus: 'written', lastFlushMtime: end.sidecarMtime });
        } else {
          set({ lastFlushMtime: null });
        }
      } catch (err) {
        if (openSeq !== mySeq) return; // 过期响应：不再用「上一张」的失败盖住新文档
        // 同一会话被关两次（closeTab / openPhoto 已先 flush 过）不是错误：
        // 必须**只跳过提示**、继续走尾部收尾 —— 旧写法在这里 return 退出整个 closePhoto，
        // 于是引擎侧会话已结束、store 仍认为它有 sessionId，再进修图台后每次拖滑块
        // 都弹「参数写入失败：session not found」。触发路径：长按 Esc（key repeat 连发）。
        if (!isSessionGone(err)) {
          // 零静默红线：落盘失败必须可见（否则用户以为参数已保存）
          const { useJobStore } = await import('./jobStore');
          useJobStore.getState().toast(`参数落盘失败（XMP 未写入）：${errText(err)}`, 'error', 8000);
        }
      }
    }
    if (openSeq !== mySeq) return; // 期间已打开别的文档：不得清空它的会话
    // G9：退出修图台 = 文档全部关闭（Tab 是会话级，不跨进出图库保留）
    set({ photoId: null, sessionId: null, params: null, beforeParams: null, lastCommittedParams: null, baseImageUrl: null, xmpStatus: 'idle', tabs: [] });
  },

  openTabs: async (ids) => {
    const list = [...new Set(ids.filter((id) => Number.isFinite(id) && id > 0))];
    if (list.length === 0) return;
    // 首张成为当前文档（走 openPhoto：会先 flush 上一张并建会话），其余只登记进 Tab 列表
    const [first, ...rest] = list;
    await get().openPhoto(first);
    // 首张没打开成功（openPhoto 已回滚成无文档态）→ 不再登记其余 Tab：
    // 否则会出现「Tab 条排满、画面却是空状态」的假成功
    if (get().photoId !== first) return;
    if (rest.length > 0) {
      set((s) => ({ tabs: rest.reduce((acc, id) => pushTab(acc, id, [s.photoId ?? first]), s.tabs) }));
    }
  },

  closeTab: async (id) => {
    const { tabs, photoId } = get();
    const idx = tabs.indexOf(id);
    if (idx < 0) return;
    const rest = tabs.filter((t) => t !== id);
    if (photoId !== id) {
      set({ tabs: rest });
      return;
    }
    // 关掉的是当前文档 → 切到相邻（优先右侧，其次左侧）；没有下一个则结束会话
    const next = rest[idx] ?? rest[idx - 1] ?? null;
    if (next === null) {
      await get().closePhoto();
      return;
    }
    await get().openPhoto(next);
    // openPhoto 会把 next 追加到末尾；这里恢复原相对位置，避免 Tab 顺序跳动
    set((s) => ({ tabs: rest, photoId: s.photoId }));
  },

  setMode: (m) => {
    // 快速↔专业切换不调引擎（同一 params，仅控件呈现），不重新解码
    set({ mode: m });
  },

  applyPatch: async (patch, commit, label) => {
    const st = get();
    const { sessionId, params } = st;
    if (!sessionId || !params) return;
    // temperature 单位契约：Kelvin 大值（UI/预设/工装）→ 归一化（见 normalizeTemperature），
    // 之后本地合并与 edit.setParams 落盘均为引擎归一化口径；快照回放等已归一化的值原样通过。
    if (typeof patch.temperature === 'number') {
      patch = { ...patch, temperature: normalizeTemperature(patch.temperature) };
    }
    // 本地乐观合并（与引擎 deepMerge 语义一致：对象递归、数组替换）
    const local = deepMergeLocal(params, patch);
    // 回滚原点 = 最后一次「引擎已确认接受」的参数（不是本次 patch 前的乐观值）：
    // 连续两次编辑都失败时，以「本次前」为原点会回滚到 A 的乐观结果，而服务端从未接受过 A。
    // 打开时该值 = 初始参数，因此恒不为 null（兜底 st.params 只防极端时序）。
    const origin = st.lastCommittedParams ?? st.params;
    const rollback = {
      params: origin,
      revision: st.revision,
      historyLabels: st.historyLabels,
      historySnapshots: st.historySnapshots,
      canUndo: st.canUndo,
      canRedo: st.canRedo,
    };
    set({ params: local, revision: st.revision + 1 });
    /** 本次追加的历史条目（失败时要精确撤回它；null = 本次没加 label） */
    let appended: HistoryEntry | null = null;
    if (label) {
      // 历史标签与参数快照同步记录（专业模式可点击跳转回放）。
      // 容量裁剪方向：保留**最近** 200 条（追加后从头部裁），丢掉最老的；
      // 两个数组同一把尺子裁，jumpHistory(index) 与面板行号才继续对得上。
      const entry: HistoryEntry = { label, params: structuredClone(local), time: Date.now() };
      appended = entry;
      set((s) => ({
        historyLabels: [...s.historyLabels, label].slice(-HISTORY_LIMIT),
        historySnapshots: [...s.historySnapshots, entry].slice(-HISTORY_LIMIT),
        canUndo: true,
        canRedo: false,
      }));
    }
    try {
      await api.edit.setParams(sessionId, patch, commit);
      // 指示灯只跟随真实 RPC 结果：RPC **返回之后**才置 debouncing（已提交 · 待落盘），
      // 旧写法在 await 之前置位，请求挂住时灯已宣称「已提交」。
      // 仍然**不能**在此置 written：引擎 edit.setParams 只置 dirty=true 并返回
      // {params, revision}（不回 sidecarMtime），真正写盘在 500ms 防抖 tick 或
      // endSession{flush:true}。提前置 written 会让只读/磁盘满/外部冲突的落盘失败
      // 永久显示成绿，且没有任何事件能把它翻回 error（见本文件 lastFlushMtime 注释）。
      // 引擎已确认接受这份参数：推进回滚原点。
      // 只有「本次仍是最新一次乐观更新」才写回 —— 迟到的成功响应不能覆盖更晚的状态：
      // 若编辑 B 已经失败置了 error，A 的响应晚到绝不能把灯改回 debouncing（红灯被清）。
      const latest = get().revision === rollback.revision + 1;
      if (latest) {
        const next: Partial<DevelopStore> = { lastCommittedParams: local };
        if (commit) next.xmpStatus = 'debouncing';
        set(next);
      }
    } catch (err) {
      // 回滚 + 指示灯转 error（零静默：界面不得停留在假成功态）。
      // 只在「本次乐观更新仍是最新一次」时回滚，避免把拖动中更晚的成功值退回旧值。
      const rolledBack = get().revision === rollback.revision + 1;
      set((s) => {
        // 幽灵步清理：**本次**追加的那条 label/snapshot 必须撤回（引擎从未接受它）。
        // - 回滚分支：rollback 里的两个数组就是追加前的数组本身，整组还原即已覆盖；
        // - 不回滚分支（更晚的编辑已成功）：仍要精确删掉本次这一条 ——
        //   按 HistoryEntry 对象引用定位下标，绝不能用 slice(-1)（会误删并发的更晚条目）。
        let historyLabels = s.historyLabels;
        let historySnapshots = s.historySnapshots;
        if (!rolledBack && appended !== null) {
          const idx = historySnapshots.indexOf(appended);
          if (idx >= 0) {
            historySnapshots = historySnapshots.filter((x) => x !== appended);
            historyLabels = historyLabels.filter((_, i) => i !== idx);
          }
        }
        const touchedHistory = historyLabels !== s.historyLabels;
        return {
          ...(rolledBack ? rollback : {}),
          ...(touchedHistory ? { historyLabels, historySnapshots } : {}),
          xmpStatus: 'error',
          // 历史数组只在真正回滚时还原（不回滚则保留更新后的步骤，避免索引错位）
          canUndo: rolledBack ? rollback.canUndo : s.canUndo,
          canRedo: rolledBack ? rollback.canRedo : s.canRedo,
        };
      });
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`参数写入失败：${errText(err)}`, 'error', 8000);
    }
  },

  setPath: async (path, value, commit, label) => {
    // 点路径 → 嵌套 patch（setPath('detail.sharpness', 40) → { detail: { sharpness: 40 } }）
    const keys = path.split('.');
    let patch: Record<string, unknown> = { [keys[keys.length - 1]]: value };
    for (let i = keys.length - 2; i >= 0; i--) {
      patch = { [keys[i]]: patch };
    }
    await get().applyPatch(patch as Partial<EditParams>, commit, commit ? label ?? `${sliderLabel(keys[keys.length - 1] as keyof EditParams)} ${typeof value === 'number' ? formatEv(value) : String(value)}` : undefined);
  },

  jumpHistory: async (index) => {
    const snap = get().historySnapshots[index];
    if (!snap) return;
    // deepMerge 无删除语义：数组字段必须显式携带（快照里不存在的数组置空），否则后加的图层/蒙版不会被回退
    const patch: Partial<EditParams> = {
      ...snap.params,
      adjustmentStack: snap.params.adjustmentStack ?? [],
      masks: snap.params.masks ?? [],
    };
    await get().applyPatch(patch, true, `回放 · ${snap.label}`);
    useJobStoreToast(`已回放到「${snap.label}」`);
  },

  setNumeric: async (key, value, commit) => {
    // Lightroom 语义（T1.7 裁定）：用户手动动 temperature/tint 时，whiteBalance 自动升为
    // 'custom'（预览管线 gate 在 custom 档；原预设档位值保留为基线不动）。预设/快照回放
    // 走 applyPatch 不经过这里，不会误升级。
    const patch: Partial<EditParams> = { [key]: value } as Partial<EditParams>;
    if ((key === 'temperature' || key === 'tint') && get().params && get().params!.whiteBalance !== 'custom') {
      patch.whiteBalance = 'custom';
    }
    await get().applyPatch(patch, commit, commit ? `${sliderLabel(key)} ${formatEv(value)}` : undefined);
  },

  setHsl: async (band, key, value, commit) => {
    const { params } = get();
    if (!params) return;
    const next = { ...params.hsl, [band]: { ...params.hsl[band], [key]: value } };
    await get().applyPatch({ hsl: next }, commit, commit ? `混色器 · ${band} ${key} ${value}` : undefined);
  },

  undo: async () => {
    const { sessionId, revision: revBefore } = get();
    if (!sessionId) return;
    try {
      const res = await api.edit.undo(sessionId);
      const back = normalizeParams(res.params);
      set((st) => {
        // 先切片再判 canUndo：'导入原状' 不是一步编辑，可撤销步数 = labels.length - 1。
        // 旧写法在**未切片**的数组上写 `> 2`（与这里等价但读起来像 off-by-one，
        // 且把「阈值」和「数组长度」绑死）；现在把不变量写在切片之后，一眼可验。
        const labels = st.historyLabels.slice(0, -1);
        const snaps = st.historySnapshots.slice(0, -1);
        return {
          params: back,
          revision: res.revision,
          historyLabels: labels,
          historySnapshots: snaps,
          canUndo: labels.length > 1,
          canRedo: true,
          // 与 applyPatch 同口径：引擎 undo 同样只置 dirty，落盘在防抖 tick / endSession，
          // 因此这里只能报「已提交 · 待落盘」，不能报「已写入」
          xmpStatus: 'debouncing',
          // undo 的结果就是引擎当前状态；期间若有更晚的 setParams 成功（revision 已前进），
          // 不能用更旧的 back 覆盖它
          lastCommittedParams: st.revision === revBefore ? back : st.lastCommittedParams,
        };
      });
    } catch (err) {
      // 空历史栈 = 正常边界（引擎 `edit.undo` 返回 -32602 "nothing to undo"）：
      // 不转 error、不弹窗。旧写法按 code 判 3004（那是 job 失败码）→ 打开照片后
      // 按 Ctrl+Z 会把顶栏灯刷成红色「参数写入失败」并弹一句「撤销失败」，什么都没写过。
      // 同上：-32602 也是 session not found 的码，所以按原文判，不按码判。
      if (isHistoryEmpty(err, 'undo')) return;
      set({ xmpStatus: 'error' });
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`撤销失败：${errText(err)}`, 'error', 6000);
    }
  },

  redo: async () => {
    const { sessionId, revision: revBefore } = get();
    if (!sessionId) return;
    try {
      const res = await api.edit.redo(sessionId);
      const forward = normalizeParams(res.params);
      set((st) => ({
        params: forward,
        revision: res.revision,
        historyLabels: [...st.historyLabels, '重做'],
        historySnapshots: [...st.historySnapshots, { label: '重做', params: structuredClone(forward), time: Date.now() }],
        canRedo: false,
        canUndo: true,
        // 同 undo：redo 也只置 dirty → 只能说「待落盘」
        xmpStatus: 'debouncing',
        lastCommittedParams: st.revision === revBefore ? forward : st.lastCommittedParams,
      }));
    } catch (err) {
      // 空重做栈 = 正常边界（-32602 "nothing to redo"）：不转 error、不弹窗
      if (isHistoryEmpty(err, 'redo')) return;
      set({ xmpStatus: 'error' });
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`重做失败：${errText(err)}`, 'error', 6000);
    }
  },

  reset: async () => {
    const { params } = get();
    if (!params) return;
    const zeroed = zeroParams(params);
    await get().applyPatch(zeroed, true, '重置');
  },

  addMask: async (type) => {
    const { params } = get();
    if (!params) return;
    // 渲染槽硬上限（maskField.MAX_MASK_SLOTS=32）：所有新建入口（蒙版面板 / 范围蒙版条 /
    // 工具轨 / MCP）都收口在这里，超限只提示不新增 —— 否则第 33 个蒙版会让整页预览失败
    if (params.masks.length >= MAX_MASK_SLOTS) {
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`已达蒙版渲染槽上限 ${MAX_MASK_SLOTS} 个：请先删除不用的蒙版`, 'warn', 6000);
      return;
    }
    const mask = buildMask(type, params.masks.length);
    await get().applyPatch({ masks: [...params.masks, mask] }, true, `新建蒙版 · ${maskName(type)}`);
    set({ selectedMaskId: mask.id });
  },

  updateMask: async (maskId, patch) => {
    const { params } = get();
    if (!params) return;
    const masks = params.masks.map((m) => (m.id === maskId ? { ...m, ...patch } : m));
    await get().applyPatch({ masks }, true);
  },

  updateMaskField: async (maskId, patch, commit, label) => {
    const { params } = get();
    if (!params) return;
    const masks = params.masks.map((m) => (m.id === maskId ? { ...m, ...patch } : m));
    await get().applyPatch({ masks }, commit, commit ? label ?? '蒙版 · 强度' : undefined);
  },

  updateMaskLocal: async (maskId, key, value, commit) => {
    const { params } = get();
    if (!params) return;
    const masks = params.masks.map((m) => (m.id === maskId ? { ...m, local: { ...m.local, [key]: value } } : m));
    await get().applyPatch({ masks }, commit, commit ? `蒙版 · ${maskLocalLabel(key)} ${formatEv(value)}` : undefined);
  },

  removeMask: async (maskId) => {
    const { params } = get();
    if (!params) return;
    const masks = params.masks.filter((m) => m.id !== maskId);
    await get().applyPatch({ masks }, true, '删除蒙版');
    set({ selectedMaskId: null });
  },

  /** 画笔落点（WP-B5）：拖动中 commit=false 不刷历史；抬笔时由调用方以 commit=true 提交一次 */
  appendStroke: async (maskId, stroke, commit) => {
    const { params } = get();
    if (!params) return;
    const masks = params.masks.map((m) =>
      m.id === maskId ? { ...m, strokes: [...(m.strokes ?? []), stroke] } : m,
    );
    await get().applyPatch({ masks }, commit, commit ? '画笔' : undefined);
  },

  selectMask: (id) => set({ selectedMaskId: id }),
  setMaskTool: (t) => set({ maskTool: t }),

  // ---- 专业模式：调整图层栈（params.adjustmentStack，协议 §4 AdjustmentLayer[]） ----

  addLayer: async (kind, name) => {
    const { params } = get();
    if (!params) return;
    const stack = params.adjustmentStack ?? [];
    const layer: AdjustmentLayer = {
      id: `layer_${Date.now().toString(36)}_${stack.length}`,
      name: name ?? `调整图层 ${stack.length + 1}`,
      kind,
      enabled: true,
      blendMode: 'normal',
      opacity: 100,
      params: {},
    };
    await get().applyPatch({ adjustmentStack: [...stack, layer] }, true, `新建图层 · ${layer.name}`);
    set({ selectedLayerId: layer.id });
  },

  updateLayer: async (id, patch) => {
    const { params } = get();
    if (!params) return;
    const stack = (params.adjustmentStack ?? []).map((l) => (l.id === id ? { ...l, ...patch } : l));
    await get().applyPatch({ adjustmentStack: stack }, true);
  },

  updateLayerField: async (id, patch, commit, label) => {
    const { params } = get();
    if (!params) return;
    const stack = (params.adjustmentStack ?? []).map((l) => (l.id === id ? { ...l, ...patch } : l));
    await get().applyPatch({ adjustmentStack: stack }, commit, commit ? label ?? '图层属性' : undefined);
  },

  updateLayerParam: async (id, key, value, commit) => {
    const { params } = get();
    if (!params) return;
    const stack = (params.adjustmentStack ?? []).map((l) => (l.id === id ? { ...l, params: { ...l.params, [key]: value } } : l));
    await get().applyPatch({ adjustmentStack: stack }, commit, commit ? `图层参数 · ${sliderLabel(key)} ${formatEv(value)}` : undefined);
  },

  removeLayer: async (id) => {
    const { params, selectedLayerId } = get();
    if (!params) return;
    const stack = (params.adjustmentStack ?? []).filter((l) => l.id !== id);
    await get().applyPatch({ adjustmentStack: stack }, true, '删除图层');
    if (selectedLayerId === id) set({ selectedLayerId: null });
  },

  moveLayer: async (id, dir) => {
    const { params } = get();
    if (!params) return;
    const stack = [...(params.adjustmentStack ?? [])];
    const i = stack.findIndex((l) => l.id === id);
    const j = i + dir;
    if (i < 0 || j < 0 || j >= stack.length) return;
    [stack[i], stack[j]] = [stack[j], stack[i]];
    await get().applyPatch({ adjustmentStack: stack }, true, dir > 0 ? '图层上移' : '图层下移');
  },

  renameLayer: async (id, name) => {
    const trimmed = name.trim();
    if (!trimmed) return;
    await get().updateLayer(id, { name: trimmed });
  },

  selectLayer: (id) => set({ selectedLayerId: id }),

  toggleLayerLock: (id) =>
    set((s) => ({
      lockedLayerIds: s.lockedLayerIds.includes(id) ? s.lockedLayerIds.filter((x) => x !== id) : [...s.lockedLayerIds, id],
    })),

  setGuides: (g) => set({ guides: g }),

  applyPreset: async (presetParams, name) => {
    const { params } = get();
    if (!params) return;
    // 预设 = 参数集合（协议 §5.5），套用但保留几何与蒙版结构
    const merged: Partial<EditParams> = {
      processVersion: presetParams.processVersion,
      whiteBalance: presetParams.whiteBalance,
      temperature: presetParams.temperature,
      tint: presetParams.tint,
      exposure: presetParams.exposure,
      contrast: presetParams.contrast,
      highlights: presetParams.highlights,
      shadows: presetParams.shadows,
      whites: presetParams.whites,
      blacks: presetParams.blacks,
      clarity: presetParams.clarity,
      texture: presetParams.texture,
      dehaze: presetParams.dehaze,
      vibrance: presetParams.vibrance,
      saturation: presetParams.saturation,
      toneCurveRGB: presetParams.toneCurveRGB,
      hsl: presetParams.hsl,
      colorGrade: presetParams.colorGrade,
    };
    await get().applyPatch(merged, true, `预设 · ${name}`);
  },

  saveSnapshot: async (name) => {
    const { photoId } = get();
    if (photoId === null) return;
    try {
      await api.edit.createSnapshot(photoId, name);
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`快照「${name}」已保存`, 'success');
    } catch (err) {
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`快照保存失败：${errText(err)}`, 'error');
    }
  },

  syncToSelection: async (targetIds) => {
    const { photoId } = get();
    if (photoId === null || targetIds.length === 0) return;
    try {
      const res = await api.edit.syncToSelection(photoId, targetIds, true);
      set({ syncMessage: `${res.updated} 张已同步参数` });
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`已同步参数到所选 ${res.updated} 张`, 'success');
    } catch (err) {
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`同步失败：${errText(err)}`, 'error');
    }
  },

  syncToSequence: async (seqId) => {
    const { photoId } = get();
    if (photoId === null) return;
    try {
      const res = await api.edit.syncToSequence(photoId, seqId, true);
      set({ syncMessage: `${res.updated} 张已同步参数（序列）` });
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`已同步参数到序列 ${seqId}（${res.updated} 张）`, 'success');
    } catch (err) {
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`同步失败：${errText(err)}`, 'error');
    }
  },

  toggleBeforeAfter: () => set((st) => ({ beforeParams: st.beforeParams ? null : st.beforeParams })),

  dismissMaskSemanticsNotice: () => {
    try {
      localStorage.setItem(MASK_SEMANTICS_NOTICE_KEY, 'true');
    } catch {
      // 写失败不阻塞关闭（本次会话仍消失），但下次启动横幅会再次出现 —— 不静默吞告知义务
    }
    set({ maskSemanticsNotified: true });
  },
}));

// ---------- 帮助 ----------

/**
 * 完整 EditParams 缺省形状：真引擎 beginSession 只回传子集（实测 15 个键，
 * 缺 hsl/colorGrade/calibration/detail/optics/geometry/toneCurve 等），
 * UI 归一化补齐后再进 store，专业模式 7 标签面板才能直接读写；
 * 引擎侧 edit.setParams 接受任意参数键，首次设置即随 sidecar 持久化。
 */
const DEFAULT_PARAMS_SHAPE: EditParams = {
  processVersion: '15.4',
  whiteBalance: 'asShot',
  temperature: 5500,
  tint: 0,
  exposure: 0,
  contrast: 0,
  highlights: 0,
  shadows: 0,
  whites: 0,
  blacks: 0,
  clarity: 0,
  texture: 0,
  dehaze: 0,
  vibrance: 0,
  saturation: 0,
  toneCurveRGB: [
    { x: 0, y: 0 },
    { x: 255, y: 255 },
  ],
  hsl: {
    red: { hue: 0, sat: 0, lum: 0 },
    orange: { hue: 0, sat: 0, lum: 0 },
    yellow: { hue: 0, sat: 0, lum: 0 },
    green: { hue: 0, sat: 0, lum: 0 },
    aqua: { hue: 0, sat: 0, lum: 0 },
    blue: { hue: 0, sat: 0, lum: 0 },
    purple: { hue: 0, sat: 0, lum: 0 },
    magenta: { hue: 0, sat: 0, lum: 0 },
  },
  colorGrade: { shadowHue: 0, shadowSat: 0, midtoneHue: 0, midtoneSat: 0, highlightHue: 0, highlightSat: 0, blend: 50, balance: 0 },
  calibration: { shadowTint: 0, redHue: 0, redSat: 0, greenHue: 0, greenSat: 0, blueHue: 0, blueSat: 0 },
  detail: { sharpness: 25, sharpenRadius: 1, sharpenDetail: 25, sharpenMasking: 0, luminanceSmoothing: 0, colorNR: 25, colorNRDetail: 50 },
  optics: { lensProfileEnable: true, manualDistortion: 0, chromaticAberration: 0, vignetteAmount: 0, vignetteMidpoint: 50, vignetteFeather: 50, grainAmount: 0, grainFrequency: 50, grainSize: 25 },
  geometry: { hasCrop: false, perspective: { vertical: 0, horizontal: 0, rotate: 0, scale: 1, aspect: 0, x: 0, y: 0 } },
  masks: [],
};

/** 引擎返回的会话参数 → 补齐缺省形状（引擎值优先，数组以引擎为准） */
/** 引擎回传参数归一化：真引擎只回 14~17 个键（缺 toneCurveRGB/hsl 等），
 *  直接塞进 store 会让读取这些键的组件（WebGL 曲线 LUT 等）拿到 undefined。
 *  undo/redo/快照恢复等所有"引擎参数 → store"的入口都必须过这一层。 */
export function normalizeParams(p: EditParams): EditParams {
  const merged = deepMergeLocal(structuredClone(DEFAULT_PARAMS_SHAPE), p as unknown as Partial<EditParams>);
  // temperature 单位契约：引擎/侧车回传为 Kelvin 口径（mock 5200–6400K / LR 2000–12000K）→
  // 归一化后进 store；undo/redo 回传的已是归一化值（|t|≤100），原样通过。
  if (typeof merged.temperature === 'number') merged.temperature = normalizeTemperature(merged.temperature);
  // T2.5 v3 读入兜底：v2 存量 sidecar / 引擎回传的 masks 可能缺 combine（→replace）与
  // amount（→100，引擎写端 ==100 省键）；逐项补齐后再进 store，消费端拿到的 MaskLayer 恒完整。
  if (Array.isArray(merged.masks)) {
    merged.masks = merged.masks.map((m) => ({
      ...m,
      combine: m.combine ?? 'replace',
      amount: m.amount ?? 100,
    }));
  }
  return merged;
}

function deepMergeLocal<T>(base: T, patch: Partial<T> | Record<string, unknown> | undefined): T {
  if (patch === undefined || patch === null) return base;
  if (Array.isArray(base) || Array.isArray(patch)) return patch as T;
  if (typeof base === 'object' && base !== null && typeof patch === 'object') {
    const out: Record<string, unknown> = { ...(base as Record<string, unknown>) };
    for (const [k, v] of Object.entries(patch as Record<string, unknown>)) {
      // undefined = patch 未携带该键（如预设缺 hsl），语义是"不修改"而非"删除"——
      // 曾直接 out[k]=v 把对象组整体置 undefined，ProPanel 读 params.hsl.red 崩掉整页
      if (v === undefined) continue;
      const bv = out[k];
      if (bv !== null && typeof bv === 'object' && !Array.isArray(bv) && v !== null && typeof v === 'object' && !Array.isArray(v)) {
        out[k] = deepMergeLocal(bv, v as Record<string, unknown>);
      } else {
        out[k] = v;
      }
    }
    return out as T;
  }
  return (patch as T) ?? base;
}

function zeroParams(p: EditParams): EditParams {
  return {
    ...p,
    whiteBalance: 'asShot',
    temperature: 5500,
    tint: 0,
    exposure: 0,
    contrast: 0,
    highlights: 0,
    shadows: 0,
    whites: 0,
    blacks: 0,
    clarity: 0,
    texture: 0,
    dehaze: 0,
    vibrance: 0,
    saturation: 0,
    toneCurveRGB: [
      { x: 0, y: 0 },
      { x: 255, y: 255 },
    ],
    masks: [],
  };
}

let maskSeq = 1;
export function buildMask(type: MaskLayer['type'], _index: number): MaskLayer {
  const id = `mask_${Date.now().toString(36)}_${maskSeq++}`;
  const base: MaskLayer = {
    id,
    type,
    enabled: true,
    opacity: 100,
    amount: 0,
    // v3：新蒙版显式声明组合方式（首条/单条 = replace 语义）；布尔组合 UI 在 T2.8
    combine: 'replace',
    local: { exposure: 0, contrast: 0, highlights: 0, shadows: 0, temperature: 0, tint: 0, saturation: 0, clarity: 0, dehaze: 0 },
  };
  if (type === 'linear') {
    base.geometry = { x1: 0.5, y1: 0, x2: 0.5, y2: 1 };
    base.amount = -30;
  } else if (type === 'radial') {
    base.geometry = { cx: 0.5, cy: 0.5, radius: 0.35, ratio: 1 };
    base.amount = -20;
  } else if (type === 'brush') {
    base.strokes = [{ x: 0.5, y: 0.5, radius: 0.08, flow: 80, density: 100, erase: false }];
    // T2.8 裁决：brush 新蒙版缺省 amount=100（画完即全效，LR/PS 行为）——
    // 引擎 v3 语义 w = f · opacity · amount，缺省 0 会导致「画笔无效果」；
    // 与协议 v3 读端 `amount ?? 100` 兜底口径一致（工厂产出即完整）。
    base.amount = 100;
  } else if (type === 'luminanceRange') {
    // 范围蒙版（协议 §4 MaskLayer.range：low/high 0..1 + samples 采样点）
    base.range = { low: 0.25, high: 0.85, samples: [] };
    base.amount = 20;
  } else if (type === 'colorRange') {
    base.range = { low: 0.2, high: 0.9, samples: [32, 74, 120] };
    base.amount = 15;
  } else if (type === 'depth') {
    base.range = { low: 0.4, high: 1, samples: [] };
    base.amount = 10;
  }
  return base;
}

export function maskName(type: MaskLayer['type']): string {
  switch (type) {
    case 'linear':
      return '渐变';
    case 'radial':
      return '径向';
    case 'brush':
      return '画笔';
    case 'aiSky':
      return 'AI 天空';
    case 'aiSubject':
      return 'AI 主体';
    case 'luminanceRange':
      return '亮度范围';
    case 'colorRange':
      return '颜色范围';
    case 'depth':
      return '深度范围';
    default:
      return type;
  }
}

function maskLocalLabel(key: keyof MaskLayer['local']): string {
  const map: Record<string, string> = {
    exposure: '曝光',
    contrast: '对比',
    highlights: '高光',
    shadows: '阴影',
    temperature: '色温',
    tint: '色调',
    saturation: '饱和',
    clarity: '清晰',
    dehaze: '去雾',
  };
  return map[key] ?? key;
}

export function sliderLabel(key: keyof EditParams): string {
  const map: Record<string, string> = {
    exposure: '曝光',
    contrast: '对比',
    highlights: '高光',
    shadows: '阴影',
    whites: '白色',
    blacks: '黑色',
    clarity: '清晰度',
    texture: '纹理',
    dehaze: '去雾',
    vibrance: '自然饱和',
    saturation: '饱和度',
    temperature: '色温',
    tint: '色调',
  };
  return map[key as string] ?? String(key);
}

function formatEv(v: number): string {
  return `${v > 0 ? '+' : ''}${Math.round(v * 100) / 100}`;
}

/** 组件层轻量提示（动态导入避免 store ↔ 组件循环依赖） */
function useJobStoreToast(text: string): void {
  void import('./jobStore').then(({ useJobStore }) => useJobStore.getState().toast(text, 'info', 2500));
}
