/**
 * Mock 缩略图/预览产物生成 —— 程序化 SVG 风光占位图，落盘到 {home}/cache/。
 * 协议约定：二进制一律落盘传绝对路径（ipc-protocol.md §1），mock 遵守同一约定；
 * SVG 由渲染进程经 lumina-file:// 协议加载。
 */

import { mkdirSync, writeFileSync, existsSync } from 'node:fs';
import path from 'node:path';
import { mulberry32 } from './data';

const SKY_PALETTES = [
  ['#1a2a4a', '#4a6b9a', '#c9a86a'], // 蓝调晨光
  ['#2b1f3a', '#8a5a6a', '#e8b06a'], // 暮色
  ['#0e1a2e', '#33507a', '#a8c4e0'], // 冷调雪景
  ['#1f2a1e', '#5a7a5a', '#d9c98a'], // 青绿山岚
  ['#141a24', '#2e4a5e', '#e0784a'], // 日落火烧云
  ['#101820', '#24405a', '#88a8c8'], // 蓝色时刻
];

interface SceneSpec {
  w: number;
  h: number;
  seed: number;
  night?: boolean;
  city?: boolean;
}

/** 生成一张程序化风光/建筑 SVG 字符串 */
export function renderSceneSvg(spec: SceneSpec): string {
  const { w, h, seed } = spec;
  const rng = mulberry32(seed);
  const palette = SKY_PALETTES[Math.floor(rng() * SKY_PALETTES.length)];
  const sunX = w * (0.15 + rng() * 0.7);
  const sunY = h * (0.18 + rng() * 0.2);
  const sunR = w * (0.02 + rng() * 0.03);

  // 三层山体/建筑剪影
  const layer = (baseY: number, amp: number, color: string, jag: number): string => {
    const pts: string[] = [`0,${h}`, `0,${baseY}`];
    const steps = 8 + Math.floor(rng() * 6);
    for (let i = 0; i <= steps; i++) {
      const x = (w * i) / steps;
      const y = baseY - Math.abs(Math.sin(i * jag + rng() * 0.5)) * amp * (0.4 + rng() * 0.6);
      pts.push(`${x.toFixed(0)},${y.toFixed(0)}`);
    }
    pts.push(`${w},${baseY}`, `${w},${h}`);
    return `<polygon points="${pts.join(' ')}" fill="${color}"/>`;
  };

  const buildingLayer = (baseY: number, color: string): string => {
    let s = '';
    let x = 0;
    while (x < w) {
      const bw = w * (0.04 + rng() * 0.06);
      const bh = h * (0.12 + rng() * 0.28);
      s += `<rect x="${x.toFixed(0)}" y="${(baseY - bh).toFixed(0)}" width="${bw.toFixed(0)}" height="${bh.toFixed(0)}" fill="${color}"/>`;
      // 窗灯
      const lit = Math.floor(rng() * 12);
      for (let i = 0; i < lit; i++) {
        const wx = x + bw * (0.1 + rng() * 0.8);
        const wy = baseY - bh + bh * rng() * 0.9;
        s += `<rect x="${wx.toFixed(0)}" y="${wy.toFixed(0)}" width="${(bw * 0.08).toFixed(0)}" height="${(bw * 0.08).toFixed(0)}" fill="#e8c26a" opacity="${(0.4 + rng() * 0.5).toFixed(2)}"/>`;
      }
      x += bw + w * 0.012;
    }
    return s;
  };

  const isCity = spec.city ?? rng() < 0.25;
  const body = isCity
    ? buildingLayer(h * 0.82, '#181c22') + buildingLayer(h * 0.92, '#101318')
    : layer(h * 0.62, h * 0.2, '#2a3140', 1.2) + layer(h * 0.74, h * 0.14, '#1e232e', 1.8) + layer(h * 0.85, h * 0.08, '#14181f', 2.4);

  const stars = spec.night
    ? Array.from({ length: 40 }, () => {
        const x = rng() * w;
        const y = rng() * h * 0.5;
        return `<circle cx="${x.toFixed(0)}" cy="${y.toFixed(0)}" r="1" fill="#ffffff" opacity="${(0.2 + rng() * 0.6).toFixed(2)}"/>`;
      }).join('')
    : '';

  return `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">
<defs>
<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="${palette[0]}"/>
<stop offset="0.55" stop-color="${palette[1]}"/>
<stop offset="1" stop-color="${palette[2]}"/>
</linearGradient>
<radialGradient id="glow" cx="${(sunX / w).toFixed(2)}" cy="${(sunY / h).toFixed(2)}" r="0.5">
<stop offset="0" stop-color="${palette[2]}" stop-opacity="0.9"/>
<stop offset="1" stop-color="${palette[2]}" stop-opacity="0"/>
</radialGradient>
</defs>
<rect width="${w}" height="${h}" fill="url(#sky)"/>
${stars}
<circle cx="${sunX.toFixed(0)}" cy="${sunY.toFixed(0)}" r="${sunR.toFixed(0)}" fill="${palette[2]}" opacity="0.95"/>
<rect width="${w}" height="${h}" fill="url(#glow)" opacity="0.5"/>
${body}
</svg>`;
}

export class MockAssetStore {
  readonly homeDir: string;
  constructor(homeDir: string) {
    this.homeDir = homeDir;
    for (const sub of ['cache/thumbs', 'cache/previews', 'exports', 'gallery']) {
      mkdirSync(path.join(homeDir, ...sub.split('/')), { recursive: true });
    }
  }

  private toDisk(rel: string, content: string): string {
    const abs = path.join(this.homeDir, ...rel.split('/'));
    const dir = path.dirname(abs);
    if (!existsSync(dir)) mkdirSync(dir, { recursive: true });
    if (!existsSync(abs)) writeFileSync(abs, content, 'utf8');
    // 协议：路径一律绝对路径正斜杠
    return abs.replace(/\\/g, '/');
  }

  /** 缩略图（320/640） */
  thumb(photoId: number, size: 320 | 640, seed: number, city: boolean): string {
    const w = size;
    const h = Math.round((size * 2) / 3);
    const rel = `cache/thumbs/${photoId % 256}/${photoId}_${size}.svg`;
    return this.toDisk(rel, renderSceneSvg({ w, h, seed: seed * 7 + photoId, city }));
  }

  /** 修图台底图 */
  preview(photoId: number, seed: number, city: boolean, width = 1920, height = 1280): string {
    const rel = `cache/previews/${photoId % 256}/${photoId}_base.svg`;
    return this.toDisk(rel, renderSceneSvg({ w: width, h: height, seed: seed * 7 + photoId, city }));
  }

  /** 降噪局部对比小样 */
  denoiseSample(photoId: number, seed: number): string {
    const rel = `cache/previews/${photoId % 256}/${photoId}_denoise.svg`;
    return this.toDisk(rel, renderSceneSvg({ w: 800, h: 533, seed: seed * 13 + photoId, night: true }));
  }

  /** 导出产物占位 */
  exportResult(photoId: number, ext: string, seed: number): string {
    const rel = `exports/lumina_export_${photoId}_${seed}.${ext === 'jpg' ? 'svg' : ext}`;
    const content = ext === 'jpg' || ext === 'png' ? renderSceneSvg({ w: 1600, h: 1067, seed: seed + photoId }) : renderSceneSvg({ w: 1600, h: 1067, seed: seed + photoId });
    return this.toDisk(rel, content);
  }

  /** 延时序列帧预览 */
  framePreview(seqId: number, frameNo: number, seed: number): string {
    const rel = `cache/previews/seq/${seqId}/frame_${frameNo}.svg`;
    return this.toDisk(rel, renderSceneSvg({ w: 1280, h: 853, seed: seed + seqId * 1000 + frameNo, night: frameNo > 300 }));
  }
}
