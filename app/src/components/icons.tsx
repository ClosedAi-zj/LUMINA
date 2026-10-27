/**
 * 统一图标入口（v1.3.0-m3 PS/LR 对齐）：lucide-react 线性图标集。
 *
 * 选型说明：npm 安装 lucide-react@1.62.0 成功（react 18 兼容），故走官方图标库而非手绘 SVG。
 * 全部图标统一 24 viewBox / strokeWidth 1.5 / currentColor，与 shell.tsx 既有 Icon（1.5px 线性）
 * 同一视觉语言；develop 模块（工具轨 / 选项栏 / dock 标签 / PanelCard）统一经此文件取图标，
 * 保证 stroke 家族一致。颜色一律继承 currentColor（令牌文字色），不在此处发明新色。
 */

import {
  // 工具轨
  Crop,
  Brush,
  Slash,
  CircleDot,
  Sun,
  Palette,
  Hand,
  ZoomIn,
  Pipette,
  // dock 分组
  SlidersHorizontal,
  Spline,
  Droplet,
  Aperture,
  Frame,
  Sparkles,
  Grid3x3,
  // 常用 UI
  History,
  Layers,
  BookOpen,
  Camera,
  ChevronDown,
  RotateCcw,
  Copy,
  Star,
  X,
  Plus,
  Eye,
  EyeOff,
  Trash2,
  Maximize,
  Lasso,
  ChartColumn,
  type LucideIcon,
} from 'lucide-react';

/** 名称 → lucide 组件映射（develop 模块统一取图标口） */
export const ICONS: Record<string, LucideIcon> = {
  // 工具轨
  crop: Crop,
  brush: Brush,
  'mask-linear': Slash,
  'mask-radial': CircleDot,
  'mask-luminance': Sun,
  'mask-color': Palette,
  hand: Hand,
  zoom: ZoomIn,
  wb: Pipette,
  // dock 分组
  basic: SlidersHorizontal,
  curve: Spline,
  color: Droplet,
  detail: Aperture,
  lens: Aperture,
  geometry: Frame,
  effects: Sparkles,
  calibration: Grid3x3,
  // 常用 UI（左栏卡片 / 面板行）
  history: History,
  layers: Layers,
  preset: BookOpen,
  snapshot: Camera,
  navigator: Maximize,
  mask: Lasso,
  histogram: ChartColumn,
  chevron: ChevronDown,
  reset: RotateCcw,
  copy: Copy,
  star: Star,
  x: X,
  plus: Plus,
  eye: Eye,
  'eye-off': EyeOff,
  trash: Trash2,
};

export function LuIcon({ name, size = 16, className = '', strokeWidth = 1.5 }: { name: string; size?: number; className?: string; strokeWidth?: number }) {
  const Cmp = ICONS[name];
  if (!Cmp) return null;
  return <Cmp size={size} strokeWidth={strokeWidth} className={className} aria-hidden />;
}
