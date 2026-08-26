import type {ReactNode} from 'react';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};

export function TitleBar({
  title,
  children,
  maximized,
  onMinimize,
  onMaximize,
  onClose
}: {
  title: string;
  children?: ReactNode;
  maximized: boolean;
  onMinimize: () => void;
  onMaximize: () => void;
  onClose: () => void;
}) {
  return (
    <header className="chrome titlebar">
      <div className="titlebar-drag">
        <span className={style({font: 'title-xs'})}>{title}</span>
      </div>
      <div className="titlebar-actions">{children}</div>
      <div className="window-controls">
        <button type="button" className="window-btn" aria-label="最小化" onClick={onMinimize}>
          <span className="window-icon window-icon-min" />
        </button>
        <button
          type="button"
          className="window-btn"
          aria-label={maximized ? '还原' : '最大化'}
          onClick={onMaximize}>
          <span className={maximized ? 'window-icon window-icon-restore' : 'window-icon window-icon-max'} />
        </button>
        <button type="button" className="window-btn window-btn-close" aria-label="关闭" onClick={onClose}>
          <span className="window-icon window-icon-close" />
        </button>
      </div>
    </header>
  );
}
