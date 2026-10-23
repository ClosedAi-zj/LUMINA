/**
 * lumina-file:// 自定义协议 —— 渲染进程加载引擎落盘的二进制产物（缩略图/预览/导出产物）。
 * 协议约定二进制一律不进 JSON，引擎只传绝对路径（ipc-protocol.md §1）；
 * 渲染进程不直接持有 Node fs，故由主进程提供受控的只读文件服务。
 * URL 形态：lumina-file://D/LUMINA/build/runtime/cache/thumbs/1/1_320.svg
 * （Windows 盘符作为 host，路径作为 pathname；仅允许白名单扩展名）。
 */

import { protocol, net } from 'electron';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const ALLOWED_EXT = new Set(['.jpg', '.jpeg', '.png', '.svg', '.webp', '.tif', '.tiff', '.dng', '.mp4', '.mov', '.psd', '.html', '.css', '.js', '.json']);

/** 必须在 app ready 之前注册（standard 方案才能被 <img>/fetch 使用） */
export function registerFileScheme(): void {
  protocol.registerSchemesAsPrivileged([
    {
      scheme: 'lumina-file',
      privileges: { standard: true, secure: true, supportFetchAPI: true, stream: true, corsEnabled: true },
    },
  ]);
}

export function registerFileProtocol(): void {
  protocol.handle('lumina-file', (request) => {
    let raw: string;
    try {
      const url = new URL(request.url);
      // host = 盘符（如 D），pathname = /LUMINA/build/... → 还原为 D:/LUMINA/build/...
      raw = decodeURIComponent(url.hostname + url.pathname).replace(/^([a-zA-Z])(?=\/)/, '$1:');
    } catch {
      raw = request.url.replace(/^lumina-file:\/\//, '');
    }
    const p = path.normalize(raw);
    const ext = path.extname(p).toLowerCase();
    if (!ALLOWED_EXT.has(ext)) {
      return new Response('forbidden extension', { status: 403 });
    }
    return net.fetch(pathToFileURL(p).toString()).catch(
      () =>
        new Response('not found', {
          status: 404,
        }),
    );
  });
}
