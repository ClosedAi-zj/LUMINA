import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import './theme/tokens.css';
import './index.css';
import { initMcpExecutor } from './mcp/executor';

initMcpExecutor(); // v1.3.0-m3：MCP 渲染层执行器（控制端口 → store 真实动作）

ReactDOM.createRoot(document.getElementById('root') as HTMLElement).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);

// 冒烟探针：确认渲染进程 console 回传主进程 stdout 的通道可用
if (window.lumina?.isSmoke) {
  console.log('[smoke-boot] renderer mounted, lumina bridge =', typeof window.lumina.request);
}
