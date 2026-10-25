/**
 * v1.2.0 外壳自检（`LUMINA_SHELLCHECK=1` → `?shellcheck=1`）：
 * 用真实点击验证四工作区切换、二级导航、精修占位提示与深浅主题切换。
 * 证据经 console → 主进程 stdout（`[renderer][log] [shellcheck] ...`），走完打 DONE 让主进程退出。
 */

import { useUi } from './store/uiStore';

const log = (msg: string): void => console.log(`[shellcheck] ${msg}`);
const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

/** 顶栏里找不到 testid，按可见文本点（同时校验按钮文案本身没被改坏） */
function clickText(text: string): boolean {
  const el = Array.from(document.querySelectorAll('button')).find((b) => (b.textContent ?? '').trim() === text);
  if (!el) return false;
  el.click();
  return true;
}

const theme = (): string => document.documentElement.getAttribute('data-theme') ?? 'none';

export async function runShellCheck(): Promise<void> {
  try {
    await sleep(6000); // 等启动（引擎握手 + 库加载）
    log(`boot view=${useUi.getState().view} theme=${theme()} header=${document.querySelectorAll('header').length}`);

    const cases: Array<{ label: string; expect: 'library' | 'develop' | 'retouch' | 'export' }> = [
      { label: '库', expect: 'library' },
      { label: '修图', expect: 'develop' },
      { label: '精修', expect: 'retouch' },
      { label: '输出', expect: 'export' },
    ];
    for (const c of cases) {
      const clicked = clickText(c.label);
      await sleep(900);
      const view = useUi.getState().view;
      const body = document.body.innerText;
      const retouchShown = body.includes('精修工作区') && body.includes('NodeGraph');
      const ok = c.expect === 'retouch' ? clicked && retouchShown : clicked && view === c.expect;
      log(`workspace ${c.label}: clicked=${clicked} view=${view} retouchPlaceholder=${retouchShown} → ${ok ? 'OK' : 'FAIL'}`);
    }

    // 二级导航：回到「修图」工作区，点其中的「风光」
    clickText('修图');
    await sleep(700);
    const tabClicked = clickText('风光');
    await sleep(700);
    log(`subnav 风光: clicked=${tabClicked} view=${useUi.getState().view} → ${tabClicked && useUi.getState().view === 'landscape' ? 'OK' : 'FAIL'}`);

    // 主题：切一次再切回，读 data-theme 与顶栏计算样式；两个主题各留一张截图供"不漏色"目视核对
    const before = theme();
    const toggle = document.querySelector<HTMLButtonElement>('button[title="切换到浅色主题"], button[title="切换到深色主题"]');
    if (theme() !== 'Dark') {
      toggle?.click();
      await sleep(500);
    }
    log('shot dark');
    await sleep(1400);
    const after = theme();
    const headerBg = getComputedStyle(document.querySelector('header') as HTMLElement).backgroundColor;
    // 注意：ThemeToggle 的文案显示"当前主题"（Dark 时写"深色"），不能按文案点目标主题，
    // 必须按 title（"切换到浅色主题"）定位
    document.querySelector<HTMLButtonElement>('button[title="切换到浅色主题"]')?.click();
    await sleep(600);
    log('shot light');
    await sleep(1400);
    const darkSeen = after === 'Dark';
    const lightSeen = theme() === 'Light';
    log(`theme: ${before}→${after}→${theme()} headerBg=${headerBg} → ${darkSeen && lightSeen ? 'OK' : 'FAIL'}`);

    // 菜单项只提示不导航：点「文件」后 view 必须不变
    const viewBefore = useUi.getState().view;
    const menuClicked = clickText('文件');
    await sleep(500);
    log(`menu 文件: clicked=${menuClicked} view ${viewBefore}→${useUi.getState().view} → ${menuClicked && viewBefore === useUi.getState().view ? 'OK' : 'FAIL'}`);

    log('DONE');
  } catch (err) {
    log(`FAILED ${(err as Error).message}`);
    log('DONE');
  }
}
