import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import path from 'node:path';

// LUMINA 渲染进程构建配置。
// 产物输出到 ../build/app（与架构文档 §2 约定一致）；
// Electron 主进程产物由 esbuild 单独输出到 ../build/app/electron，因此这里 emptyOutDir=false 防止互删。
export default defineConfig({
  plugins: [react()],
  base: './',
  resolve: {
    alias: {
      '@': path.resolve(__dirname, 'src'),
    },
  },
  build: {
    outDir: '../build/app',
    emptyOutDir: false,
    sourcemap: false,
    chunkSizeWarningLimit: 1500,
  },
  server: {
    port: 5173,
    strictPort: true,
  },
});
