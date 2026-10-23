import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import './theme/tokens.css';
import './index.css';

ReactDOM.createRoot(document.getElementById('root') as HTMLElement).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);

// 冒烟探针：确认渲染进程 console 回传主进程 stdout 的通道可用
if (window.lumina?.isSmoke) {
  console.log('[smoke-boot] renderer mounted, lumina bridge =', typeof window.lumina.request);
}
