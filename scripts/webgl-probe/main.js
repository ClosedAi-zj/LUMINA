// WebGL 可用性探针：对比「安全GL开关组合」与「默认硬件加速」
// 用法: electron main.js safe|hw
const { app, BrowserWindow } = require('electron');

const mode = process.argv[process.argv.length - 1];

if (mode === 'safe') {
  app.disableHardwareAcceleration();
  app.commandLine.appendSwitch('disable-gpu');
  app.commandLine.appendSwitch('disable-gpu-compositing');
  app.commandLine.appendSwitch('in-process-gpu');
  app.commandLine.appendSwitch('use-angle', 'swiftshader');
  app.commandLine.appendSwitch('use-gl', 'angle');
  app.commandLine.appendSwitch('disable-gpu-process-crash-limit');
}

app.whenReady().then(async () => {
  const win = new BrowserWindow({ show: false, width: 400, height: 300 });
  try {
    await win.loadFile(require('path').join(__dirname, 'test.html'));
    const r = await win.webContents.executeJavaScript(`(() => {
      const c = document.getElementById('c');
      let gl = null, err = null;
      try { gl = c.getContext('webgl') || c.getContext('experimental-webgl'); } catch (e) { err = String(e); }
      if (!gl) return { mode: '${mode}', webgl: false, err };
      const dbg = gl.getExtension('WEBGL_debug_renderer_info');
      return {
        mode: '${mode}', webgl: true,
        vendor: gl.getParameter(gl.VENDOR),
        renderer: gl.getParameter(gl.RENDERER),
        dbgRenderer: dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : null,
        maxTex: gl.getParameter(gl.MAX_TEXTURE_SIZE),
      };
    })()`);
    console.log('WEBGL_PROBE ' + JSON.stringify(r));
  } catch (e) {
    console.log('WEBGL_PROBE {"mode":"' + mode + '","error":"' + String(e).replace(/"/g, "'") + '"}');
  }
  app.exit(0);
});
