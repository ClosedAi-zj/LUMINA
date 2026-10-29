/**
 * 修图设置会话级剪贴板（LR 语义：修图设置独立于系统剪贴板）。
 *
 * 单独成模块的原因（v1.3.0 性能优化）：MenuBar 等常驻组件需要调用
 * copyDevelopSettings / pasteDevelopSettings，若直接从 DevelopPage 导入，会把整个
 * DevelopPage 组件图（ViewerCanvas + 十余个面板）拽回主 chunk，导致 React.lazy
 * 无法把修图台拆出去。抽出这个零组件依赖的纯逻辑模块后，DevelopPage 才能被干净地
 * 代码分割。
 */
import { useDevelop } from '../../store/developStore';
import { useJobStore } from '../../store/jobStore';
import type { EditParams } from '../../api/protocol';

let settingsClipboard: EditParams | null = null;

/** G1 菜单接线：编辑 ▸ 复制设置 与 Ctrl+C 共用同一会话剪贴板（无会话/无参数时为 no-op） */
export function copyDevelopSettings(): boolean {
  const params = useDevelop.getState().params;
  if (!params) return false;
  settingsClipboard = structuredClone(params);
  return true;
}

/** G1 菜单接线：编辑 ▸ 粘贴设置 与 Ctrl+V 共用同一会话剪贴板 */
export async function pasteDevelopSettings(): Promise<void> {
  if (!settingsClipboard) {
    useJobStore.getState().toast('剪贴板里还没有设置：先在任一照片上 Ctrl+C 复制', 'warn', 3000);
    return;
  }
  await useDevelop.getState().applyPatch(structuredClone(settingsClipboard), true, '粘贴设置');
  useJobStore.getState().toast('已粘贴设置', 'success', 2000);
}
