/**
 * 作品集主页（对照 03 号设计稿）：左画册列表 + 中画册详情/挑选排序。
 */

import { useEffect } from 'react';
import { usePortfolio } from './store/portfolioStore';
import { Button } from '../../components/primitives';
import { EmptyState } from '../../components/shell';
import AlbumList from './AlbumList';
import AlbumDetail from './AlbumDetail';

export default function PortfolioPage() {
  const loadError = usePortfolio((s) => s.loadError);
  const loading = usePortfolio((s) => s.loading);
  const albums = usePortfolio((s) => s.albums);

  useEffect(() => {
    void usePortfolio.getState().load();
  }, []);

  if (loadError && albums.length === 0 && !loading) {
    return (
      <div className="h-full flex items-center justify-center">
        <EmptyState
          icon={<span className="text-danger">⚠</span>}
          title="画册加载失败"
          desc={loadError}
          action={<Button onClick={() => void usePortfolio.getState().load()}>重试</Button>}
        />
      </div>
    );
  }

  return (
    <div className="h-full flex">
      <AlbumList />
      <AlbumDetail />
    </div>
  );
}
