import type {ReactNode} from 'react';
import {Breadcrumbs, Breadcrumb} from '@react-spectrum/s2/Breadcrumbs';
import {ActionButton} from '@react-spectrum/s2/ActionButton';
import ChevronLeft from '@react-spectrum/s2/icons/ChevronLeft';
import {Image} from '@react-spectrum/s2/Image';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};
import icon from '../assets/icon.png';

export function TitleBar({
  crumbs,
  children,
  isLocked,
  maximized,
  onMinimize,
  onMaximize,
  onClose,
  onBack,
  onParent
}: {
  crumbs: string[];
  children?: ReactNode;
  isLocked?: boolean;
  maximized: boolean;
  onMinimize: () => void;
  onMaximize: () => void;
  onClose: () => void;
  onBack?: () => void;
  onParent: () => void;
}) {
  return (
    <header className="chrome titlebar">
      <div className="titlebar-brand">
        {onBack ? <ActionButton aria-label="返回" isDisabled={isLocked} onPress={onBack}><ChevronLeft /></ActionButton>
          : <Image src={icon} alt="DLSS Compositor" styles={style({width: 40, height: 40, backgroundColor: 'transparent'})} />}
      </div>
      <div className="titlebar-title">
        {onBack ? <Breadcrumbs aria-label="位置" isDisabled={isLocked} onAction={onParent}>
          {crumbs.map((label, index) => (
            <Breadcrumb id={index} key={`${label}-${index}`}>{label}</Breadcrumb>
          ))}
        </Breadcrumbs> : null}
      </div>
      <div className="titlebar-mid" />
      {children ? (
        <div className="titlebar-toolbar" role="toolbar" aria-label="应用工具" aria-orientation="horizontal">
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
        <button type="button" className="window-btn window-btn-close" aria-label="关闭" disabled={isLocked} onClick={onClose}>
          <span className="window-icon window-icon-close" />
        </button>
      </div>
    </header>
  );
}
