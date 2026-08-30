import type {ReactNode} from 'react';
import {Breadcrumbs, Breadcrumb} from '@react-spectrum/s2';

export function TitleBar({
  crumbs,
  children,
  maximized,
  onMinimize,
  onMaximize,
  onClose
}: {
  crumbs: string[];
  children?: ReactNode;
  maximized: boolean;
  onMinimize: () => void;
  onMaximize: () => void;
  onClose: () => void;
}) {
  return (
    <header className="chrome titlebar">
      <div className="titlebar-brand" aria-hidden="true">
        <span className="brand-tile" />
      </div>
      <div className="titlebar-title">
        <Breadcrumbs aria-label="位置">
          {crumbs.map((label, index) => (
            <Breadcrumb key={`${label}-${index}`}>{label}</Breadcrumb>
          ))}
        </Breadcrumbs>
      </div>
      <div className="titlebar-mid" />
      {children ? (
        <div className="titlebar-toolbar" role="toolbar" aria-orientation="horizontal">
          {children}
        </div>
      ) : null}
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
