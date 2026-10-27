/**
 * v1.2.0 外壳自检（`LUMINA_SHELLCHECK=1` → `?shellcheck=1`）：
 * 用真实点击验证 LR 七大模块 tab 切换、二级导航、未实现模块占位与深浅主题切换。
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

/** 含徽标的按钮（如「风光+特色」胶囊）textContent 不等于单字，按包含匹配 */
function clickTextContains(text: string): boolean {
  const el = Array.from(document.querySelectorAll('button')).find((b) => (b.textContent ?? '').includes(text));
  if (!el) return false;
  el.click();
  return true;
}

const theme = (): string => document.documentElement.getAttribute('data-theme') ?? 'none';

export async function runShellCheck(): Promise<void> {
  try {
    await sleep(6000); // 等启动（引擎握手 + 库加载）
    log(`boot view=${useUi.getState().view} theme=${theme()} header=${document.querySelectorAll('header').length}`);

    const cases: Array<{ label: string; expect: 'library' | 'develop' | 'map' | 'export' | 'placeholder' }> = [
      { label: '图库', expect: 'library' },
      { label: '修改照片', expect: 'develop' },
      { label: '地图', expect: 'map' },
      { label: '书籍', expect: 'placeholder' },
      { label: '打印', expect: 'export' },
      { label: 'Web', expect: 'placeholder' },
    ];
    for (const c of cases) {
      const clicked = clickText(c.label);
      await sleep(900);
      const view = useUi.getState().view;
      const body = document.body.innerText;
      const placeholderShown = body.includes('后续里程碑') && body.includes(`${c.label}模块`);
      const ok = c.expect === 'placeholder' ? clicked && placeholderShown : clicked && view === c.expect;
      log(`module ${c.label}: clicked=${clicked} view=${view} placeholder=${placeholderShown} → ${ok ? 'OK' : 'FAIL'}`);
    }

    // 二级导航：回到「修改照片」模块，点其中的「风光」（特色模块入口）
    clickText('修改照片');
    await sleep(700);
    const tabClicked = clickTextContains('风光');
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

    // G1 真菜单：点「文件」展开下拉（不直接导航），点「导出…」落到交付视图（export）
    const viewBefore = useUi.getState().view;
    const menuClicked = clickText('文件');
    await sleep(400);
    const dropdownOpen = document.body.innerText.includes('导入照片');
    log(`menu 文件: clicked=${menuClicked} dropdown=${dropdownOpen} view=${viewBefore}`);
    const exportClicked = clickText('导出…');
    await sleep(900);
    const exportView = useUi.getState().view;
    log(`menu 导出…: clicked=${exportClicked} view→${exportView} → ${menuClicked && dropdownOpen && exportClicked && exportView === 'export' ? 'OK' : 'FAIL'}`);

    // 自定义标题栏窗口三键（frame:false）：真实 DOM 点击 → IPC → 主进程 BrowserWindow。
    // 最小化后渲染层仍在运行，用 toggleMaximize 从最小化恢复并同时验证 maximize 链路。
    const winBtn = (label: string): HTMLButtonElement | null =>
      document.querySelector<HTMLButtonElement>(`button[aria-label="${label}"]`);
    const max0 = await window.lumina.win.isMaximized();
    winBtn('最小化')?.click();
    await sleep(800);
    const restored = await window.lumina.win.toggleMaximize(); // minimize → maximize
    await sleep(700);
    winBtn('向下还原')?.click();
    await sleep(700);
    const maxAfterRestore = await window.lumina.win.isMaximized();
    winBtn('最大化')?.click();
    await sleep(700);
    const maxAfterMax = await window.lumina.win.isMaximized();
    winBtn('向下还原')?.click();
    await sleep(700);
    const maxFinal = await window.lumina.win.isMaximized();
    const closePresent = !!winBtn('关闭');
    log(
      `win-controls: max0=${max0} minimize→maximize=${restored} restore→${maxAfterRestore} maximize→${maxAfterMax} restore→${maxFinal} closeBtn=${closePresent} → ${
        !max0 && restored && !maxAfterRestore && maxAfterMax && !maxFinal && closePresent ? 'OK' : 'FAIL'
      }`,
    );

    // PS/LR 惯例单键（batch 1）：D 显影 / G 网格 / E 放大视图（→精选）。合成 keydown 走同一 window keydown 链路。
    const pressKey = (key: string): void => {
      window.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true }));
    };
    pressKey('g');
    await sleep(400);
    const gView = useUi.getState().view;
    pressKey('d');
    await sleep(400);
    const dView = useUi.getState().view;
    pressKey('e');
    await sleep(400);
    const eView = useUi.getState().view;
    log(`hotkeys: G→${gView} D→${dView} E→${eView} → ${gView === 'library' && dView === 'develop' && eView === 'culling' ? 'OK' : 'FAIL'}`);

    log('DONE');
  } catch (err) {
    log(`FAILED ${(err as Error).message}`);
    log('DONE');
  }
}
