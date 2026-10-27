/**
 * 交付中心主页（对照 12 号设计稿「导出与交付中心」）：
 * 左预设列表 + 中导出规格表单（格式/尺寸/色彩空间/水印/命名模板/输出目录/导出对象）+
 * 右任务队列。export.* 接口不可用时整页降级提示，不崩溃（零静默）。
 * 队列 2s 轮询刷新；创建任务需引擎确认，失败显式 toast。
 */

import { useEffect } from 'react';
import { useExport, previewFileName } from './store/exportStore';
import { Button, Segmented, Slider, Toggle } from '../../components/primitives';
import { Icon } from '../../components/shell';
import PresetList from './PresetList';
import WatermarkForm from './WatermarkForm';
import ExportQueue from './ExportQueue';
import SoftProofCard from './SoftProofCard';
import PrintCard from './PrintCard';

const LONG_EDGE_OPTIONS = [1920, 2560, 4096, 7680] as const;

export default function ExportPage() {
  const spec = useExport((s) => s.spec);
  const source = useExport((s) => s.source);
  const sourceCount = useExport((s) => s.sourceCount);
  const creating = useExport((s) => s.creating);

  // 进入页面：预设 + 对象统计 + 队列；挂载期间 2s 轮询队列
  useEffect(() => {
    void useExport.getState().loadPresets();
    void useExport.getState().refreshSourceCount();
    void useExport.getState().refreshJobs();
    const t = setInterval(() => void useExport.getState().refreshJobs(), 2000);
    return () => clearInterval(t);
  }, []);

  const disabledByDegradation = !!useExport((s) => s.jobsError) && !!useExport((s) => s.presetsError);

  return (
    <div className="h-full flex">
      <PresetList />

      {/* 中央规格表单 */}
      <section className="flex-1 min-w-0 overflow-y-auto" style={{ background: 'var(--Lumina-Tokens-canvas)' }}>
        <div className="p-4 flex flex-col gap-4 max-w-2xl">
          <div className="flex items-center gap-2">
            <Icon name="download" size={16} className="text-accent" />
            <h2 className="text-sm font-semibold">导出与交付中心</h2>
            {disabledByDegradation && (
              <span className="text-2xs px-1.5 py-0.5 rounded-sm font-medium" style={{ background: 'var(--Lumina-Tokens-accentSoft)', color: 'var(--Lumina-Tokens-accent)' }}>
                部分接口降级
              </span>
            )}
          </div>

          {/* 格式 + 基本参数 */}
          <div className="rounded-lg border border-line p-3 flex flex-col gap-3" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
            <div className="flex items-center gap-3">
              <span className="text-2xs text-txt2 w-16 shrink-0">格式</span>
              {/* 引擎自研 PSD（M3-01）：带图层或单层合成。PNG/TIFF 为无损，JPG 带质量。 */}
              <Segmented
                value={spec.format}
                onChange={(v) => useExport.getState().patchSpec({ format: v })}
                options={[
                  { value: 'jpg', label: 'JPG' },
                  { value: 'png', label: 'PNG' },
                  { value: 'tiff', label: 'TIFF' },
                  { value: 'psd', label: 'PSD' },
                ]}
              />
              {spec.format === 'psd' && (
                <span className="text-2xs text-txt3">PSD · 节点分层（背景+各调整层+合成层）</span>
              )}
            </div>
            <div className="flex items-center gap-3">
              <span className="text-2xs text-txt2 w-16 shrink-0">长边</span>
              <Segmented
                value={String(spec.longEdge)}
                onChange={(v) => useExport.getState().patchSpec({ longEdge: Number(v) })}
                options={LONG_EDGE_OPTIONS.map((n) => ({ value: String(n), label: `${n}px` }))}
              />
            </div>
            <div className="flex items-center gap-3">
              <span className="text-2xs text-txt2 w-16 shrink-0">色彩空间</span>
              <Segmented
                value={spec.colorSpace}
                onChange={(v) => useExport.getState().patchSpec({ colorSpace: v })}
                options={[
                  { value: 'sRGB', label: 'sRGB' },
                  { value: 'AdobeRGB', label: 'AdobeRGB' },
                ]}
              />
            </div>
            {spec.format === 'jpg' ? (
              <div className="pl-20">
                <Slider
                  label="质量"
                  value={spec.quality}
                  min={40}
                  max={100}
                  defaultValue={90}
                  onChange={(v) => useExport.getState().patchSpec({ quality: v })}
                  format={(v) => `${v}%`}
                />
              </div>
            ) : spec.format === 'psd' ? (
              <div className="flex items-center gap-3 pl-20">
                <span className="text-2xs text-txt3">质量参数仅 JPG 支持</span>
              </div>
            ) : (
              <div className="flex items-center gap-3 pl-20">
                <span className="text-2xs text-txt3">质量参数仅 JPG 支持</span>
              </div>
            )}
            {spec.format === 'psd' && (
              <>
                <div className="flex items-center gap-3">
                  <span className="text-2xs text-txt2 w-16 shrink-0">位深</span>
                  <Segmented
                    value={String(spec.psdDepth)}
                    onChange={(v) => useExport.getState().patchSpec({ psdDepth: Number(v) as 8 | 16 })}
                    options={[
                      { value: '8', label: '8 bit' },
                      { value: '16', label: '16 bit' },
                    ]}
                  />
                  <span className="text-2xs text-txt3">16bit = 8bit 拓宽（引擎工作空间为 8bit 显示）</span>
                </div>
                <div className="flex items-center gap-3">
                  <span className="text-2xs text-txt2 w-16 shrink-0">图层</span>
                  <Toggle checked={spec.psdKeepLayers} onChange={(v) => useExport.getState().patchSpec({ psdKeepLayers: v })} label="保留 PSD 图层" />
                  <span className="text-2xs text-txt3">关闭则仅导出合成单层；节点&gt;20 自动降级单层</span>
                </div>
              </>
            )}
          </div>

          {/* 水印 */}
          <WatermarkForm />

          {/* 命名模板 + 输出目录 */}
          <div className="rounded-lg border border-line p-3 flex flex-col gap-2.5" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
            <span className="text-xs font-medium">命名与输出</span>
            <div>
              <div className="text-2xs text-txt2 mb-1">命名模板</div>
              <input
                value={spec.namingTemplate}
                onChange={(e) => useExport.getState().patchSpec({ namingTemplate: e.target.value })}
                className="w-full h-8 px-2.5 rounded-md border border-line bg-card text-xs num focus:outline-none focus:border-accent"
              />
              <div className="flex items-center gap-1.5 mt-1.5">
                {['{date}', '{seq}', '{name}'].map((ph) => (
                  <button
                    key={ph}
                    onClick={() => useExport.getState().patchSpec({ namingTemplate: `${spec.namingTemplate}${ph}` })}
                    className="num text-2xs px-1.5 h-5 rounded-sm border border-line text-txt3 hover:text-txt hover:bg-hover"
                  >
                    {ph}
                  </button>
                ))}
                <div className="flex-1" />
                <span className="text-2xs text-txt3">示例：</span>
                <span className="num text-2xs text-accent">{previewFileName(spec)}</span>
              </div>
            </div>
            <div>
              <div className="text-2xs text-txt2 mb-1">输出目录</div>
              <input
                value={spec.outputDir}
                onChange={(e) => useExport.getState().patchSpec({ outputDir: e.target.value })}
                className="w-full h-8 px-2.5 rounded-md border border-line bg-card text-xs num focus:outline-none focus:border-accent"
              />
            </div>
          </div>

          {/* 导出对象 + 执行 */}
          <div className="rounded-lg border border-line p-3 flex flex-col gap-3" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
            <div className="flex items-center gap-3">
              <span className="text-2xs text-txt2 w-16 shrink-0">导出对象</span>
              <Segmented
                value={source}
                onChange={(v) => void useExport.getState().setSource(v)}
                options={[
                  { value: 'all', label: '全部照片' },
                  { value: 'picked', label: '仅精选' },
                  { value: 'rated', label: '≥3 星' },
                ]}
              />
              <span className="num text-2xs text-txt3">{sourceCount === null ? '统计中…' : `${sourceCount.toLocaleString('zh-Hans-CN')} 张`}</span>
            </div>
            <div className="flex items-center gap-3">
              <Button variant="primary" size="lg" disabled={creating || sourceCount === 0} onClick={() => void useExport.getState().createJob()}>
                {creating ? '创建中…' : '开始导出'}
              </Button>
              <span className="text-2xs text-txt3">任务进入右侧队列，可随时取消；失败可重试。</span>
            </div>
          </div>

          {/* M4-03：软打样预览（引擎 lcms2 实测 ΔE，不是占位） */}
          <SoftProofCard />
          {/* M4-04：打印 / PDF 输出通道（无打印机环境的交付路径） */}
          <PrintCard />
        </div>
      </section>

      <ExportQueue />
    </div>
  );
}
