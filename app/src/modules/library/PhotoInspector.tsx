/**
 * 照片检查器（帧 01 右栏）：EXIF 元数据 / GPS / 色标 / 旗标 / 评分 / 关键词。
 * mock 模式额外提供「模拟外部修改」调试入口（capabilities.mock 才显示）。
 */

import { useEffect, useState } from 'react';
import { useLibrary } from '../../store/libraryStore';
import { api } from '../../api/engine';
import { errText } from '../../api/rpc-client';
import { useJobStore } from '../../store/jobStore';
import { fileUrl } from '../../api/rpc-client';
import { formatBytes, formatDateTime, formatCount } from '../../utils/format';
import { Icon } from '../../components/shell';
import { Button } from '../../components/primitives';
import type { PhotoDetail } from '../../api/protocol';

/** 照片色标：存储值为语义键（协议 colorLabel: string），显示色取 tokens.css 的 --Lumina-Tokens-label* */
const COLOR_LABELS: { key: string; css: string; name: string }[] = [
  { key: 'red', css: 'var(--Lumina-Tokens-labelRed)', name: '红' },
  { key: 'yellow', css: 'var(--Lumina-Tokens-labelYellow)', name: '黄' },
  { key: 'green', css: 'var(--Lumina-Tokens-labelGreen)', name: '绿' },
  { key: 'blue', css: 'var(--Lumina-Tokens-labelBlue)', name: '蓝' },
  { key: 'purple', css: 'var(--Lumina-Tokens-labelPurple)', name: '紫' },
  { key: 'pink', css: 'var(--Lumina-Tokens-labelPink)', name: '粉' },
];
const RATING_LABELS = ['无', '★', '★★', '★★★', '★★★★', '★★★★★'];

export default function PhotoInspector() {
  const currentPhotoId = useLibrary((s) => s.currentPhotoId);
  // 只订阅「当前这张」的缩略图：直接订阅整个 thumbUrls 会让每一批缩略图到达都触发
  // 一次 getPhotoDetail 重拉（P23）。selector 返回原始字符串，引用稳定、不会无限重渲。
  const thumbUrl = useLibrary((s) => (s.currentPhotoId !== null ? s.thumbUrls[s.currentPhotoId] : undefined));
  const [detail, setDetail] = useState<PhotoDetail | null>(null);
  const engineInfo = useJobStore((s) => s.engineInfo);
  const isMock = (engineInfo as { capabilities?: { mock?: boolean } })?.capabilities?.mock === true;

  useEffect(() => {
    if (currentPhotoId === null) {
      setDetail(null);
      return;
    }
    let cancelled = false;
    api.library
      .getPhotoDetail(currentPhotoId)
      .then((d) => !cancelled && setDetail(d))
      .catch((err) => {
        if (!cancelled) {
          setDetail(null);
          useJobStore.getState().toast(`照片详情加载失败：${errText(err)}`, 'error', 6000);
        }
      });
    return () => {
      cancelled = true;
    };
    // 依赖里不再含 thumbUrls：换图才重拉详情，缩略图到货不重拉
  }, [currentPhotoId]);

  if (!detail) {
    return <div className="p-3 text-2xs text-txt3">选中照片后在此查看元数据、评分与旗标。</div>;
  }

  const update = async (patch: Parameters<typeof api.library.updatePhotos>[1]) => {
    try {
      await api.library.updatePhotos([detail.id], patch);
      const d = await api.library.getPhotoDetail(detail.id);
      setDetail(d);
      await useLibrary.getState().refresh();
    } catch (err) {
      useJobStore.getState().toast(`更新失败：${errText(err)}`, 'error');
    }
  };

  const src = detail.thumbPath ? fileUrl(detail.thumbPath) : thumbUrl ? fileUrl(thumbUrl) : null;

  return (
    <div className="p-3 flex flex-col gap-3 text-xs">
      <div className="rounded-md overflow-hidden border border-line" style={{ background: 'var(--Lumina-Tokens-viewer)', height: 130 }}>
        {src ? <img src={src} alt={detail.fileName} className="w-full h-full object-cover" draggable={false} /> : <div className="w-full h-full" />}
      </div>

      <div>
        <div className="text-sm font-medium break-all">{detail.fileName}</div>
        <div className="text-2xs text-txt3 break-all mt-0.5">{detail.filePath}</div>
      </div>

      <Row label="相机" value={detail.camera} />
      <Row label="镜头" value={detail.lens} />
      <Row label="焦距" value={detail.focalLength} />
      <Row label="光圈 · 快门" value={`${detail.aperture} · ${detail.shutter}`} />
      <Row label="ISO" value={String(detail.iso)} />
      <Row label="拍摄时间" value={formatDateTime(detail.capturedAt)} />
      <Row label="大小" value={formatBytes(detail.fileSize)} />
      <Row label="尺寸" value={`${detail.width} × ${detail.height}`} />
      <Row label="健康" value={healthText(detail.health)} />

      {detail.hasGps && detail.gpsLat !== null && detail.gpsLng !== null && (
        <div className="rounded-md border border-line p-2">
          <div className="flex items-center gap-1 text-2xs text-txt2 mb-1">
            <Icon name="pin" size={11} className="text-accent" /> GPS（来自 sidecar exif:）
          </div>
          <div className="num text-2xs text-txt3">
            {detail.gpsLat.toFixed(5)}°N · {detail.gpsLng.toFixed(5)}°E
          </div>
        </div>
      )}

      {/* 评分 */}
      <div>
        <div className="text-2xs text-txt3 mb-1">评分</div>
        <div className="flex items-center gap-1">
          {RATING_LABELS.map((label, v) => (
            <button
              key={v}
              onClick={() => void update({ rating: v })}
              className={`h-6 w-6 rounded-sm text-2xs ${detail.rating === v ? 'bg-[var(--Lumina-Tokens-accentSoft)] text-accent' : 'text-txt3 hover:bg-hover hover:text-txt'}`}
              title={`${v} 星`}
            >
              {v === 0 ? '–' : '★'}
            </button>
          ))}
        </div>
      </div>

      {/* 旗标 */}
      <div>
        <div className="text-2xs text-txt3 mb-1">旗标</div>
        <div className="flex items-center gap-1.5">
          <Button size="sm" variant={detail.pick === 'pick' ? 'primary' : 'default'} onClick={() => void update({ pick: detail.pick === 'pick' ? 'none' : 'pick' })}>
            置优 P
          </Button>
          <Button size="sm" variant={detail.pick === 'reject' ? 'danger' : 'default'} onClick={() => void update({ pick: detail.pick === 'reject' ? 'none' : 'reject' })}>
            淘汰 X
          </Button>
        </div>
      </div>

      {/* 色标 */}
      <div>
        <div className="text-2xs text-txt3 mb-1">色标</div>
        <div className="flex items-center gap-1.5">
          <button className={`w-5 h-5 rounded-full border ${detail.colorLabel === null ? 'border-accent' : 'border-line'}`} onClick={() => void update({ colorLabel: null })} title="无" />
          {COLOR_LABELS.map((c) => (
            <button
              key={c.key}
              className={`w-5 h-5 rounded-full border ${detail.colorLabel === c.key ? 'border-accent' : 'border-transparent'}`}
              style={{ background: c.css }}
              onClick={() => void update({ colorLabel: c.key })}
              aria-label={`色标 ${c.name}`}
            />
          ))}
        </div>
      </div>

      {/* 蒙版统计 */}
      {detail.hasEdits && <Row label="编辑" value={`已调整（sidecar）· ${formatCount(detail.keywords.length)} 关键词`} />}

      {/* Mock 调试入口（仅 mock 模式可见；触发 1005 三选一流程用） */}
      {isMock && (
        <div className="rounded-md border border-dashed border-line p-2">
          <div className="text-2xs text-txt3 mb-1.5">Mock 调试（非协议功能）</div>
          <div className="flex flex-col gap-1.5">
            <Button
              size="sm"
              onClick={async () => {
                await api.mock.simulateExternalEdit(detail.id);
                useJobStore.getState().toast('已模拟外部修改 sidecar，下次 xmp.write 将返回 1005', 'warn');
              }}
            >
              模拟外部修改（1005）
            </Button>
            <Button
              size="sm"
              onClick={async () => {
                await api.mock.simulateCorruption(detail.id);
                useJobStore.getState().toast('已模拟 MD5 校验失败（4002），进入隔离视图', 'warn');
              }}
            >
              模拟校验失败（4002）
            </Button>
          </div>
        </div>
      )}
    </div>
  );
}

function Row({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex items-start justify-between gap-2">
      <span className="text-2xs text-txt3 shrink-0">{label}</span>
      <span className="text-2xs text-txt2 text-right break-all">{value}</span>
    </div>
  );
}

function healthText(h: PhotoDetail['health']): string {
  switch (h) {
    case 'ok':
      return '正常';
    case 'missing':
      return '⚠ 文件缺失';
    case 'corrupt':
      return '⚠ 校验失败';
    default:
      return '未检查';
  }
}
