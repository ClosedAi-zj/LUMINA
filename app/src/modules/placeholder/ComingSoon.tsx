/** 第二阶段模块占位（明确标注未接入，不留假按钮） */

import { useUi } from '../../store/uiStore';
import { EmptyState } from '../../components/shell';
import { Button } from '../../components/primitives';

const TITLES: Record<string, { title: string; desc: string }> = {
  culling: { title: '精选模式（第二阶段）', desc: '键盘流 X/P/U/0-5/Enter/F/Caps、自动晋级规则与批量操作将在第二阶段接入。' },
  map: { title: '地图模块（第二阶段）', desc: '程序化暗色底图、地点聚合与 GPS 反写将在第二阶段接入。' },
  timelapse: { title: '延时工作台（第二阶段）', desc: '去闪分析、Holy Grail 关键帧与渲染队列将在第二阶段接入。' },
  landscape: { title: '风光专项（第二阶段）', desc: 'HDR 包围合成与经典降噪将在第二阶段接入。' },
  export: { title: '导出交付中心（第二阶段）', desc: '四格式导出、水印与命名模板将在第二阶段接入。' },
  portfolio: { title: '作品集（第二阶段）', desc: '画册管理与拖拽排序将在第二阶段接入。' },
  gallery: { title: 'Web 画廊（第二阶段）', desc: '密码门禁发布与收藏回传将在第二阶段接入。' },
};

export default function ComingSoon({ view }: { view: string }) {
  const navigate = useUi((s) => s.navigate);
  const info = TITLES[view] ?? { title: '模块建设中', desc: '该模块将在后续阶段接入。' };
  return (
    <div className="h-full">
      <EmptyState
        icon="◔"
        title={info.title}
        desc={info.desc}
        action={
          <Button variant="primary" onClick={() => navigate('library')}>
            返回图库
          </Button>
        }
      />
    </div>
  );
}
