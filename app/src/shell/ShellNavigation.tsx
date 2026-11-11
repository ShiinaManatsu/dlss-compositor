import {useId} from 'react';
import type {ReactNode} from 'react';
import {Link} from 'react-aria-components/Link';
import {Tooltip, TooltipTrigger} from '@react-spectrum/s2/Tooltip';
import {style, focusRing} from '@react-spectrum/s2/style' with {type: 'macro'};
import {AnimatedPanelToggle} from './AnimatedPanelToggle';

export type NavigationItem = {id: string; label: string; href: string; icon: ReactNode; isDisabled?: boolean; children?: NavigationItem[]};

const routeStyle = style({
  ...focusRing(), display: 'grid', alignItems: 'center', width: 'full', minWidth: 0,
  minHeight: 40, boxSizing: 'border-box', position: 'relative', borderRadius: 'default',
  font: 'ui', fontWeight: {default: 'normal', isActive: 'bold'},
  color: {default: 'neutral', isDisabled: 'disabled'}, textDecoration: 'none', userSelect: 'none',
  backgroundColor: {default: 'transparent', isHovered: 'gray-100', isPressed: 'gray-200'}
});

/** A route rail, not a shrunken SideNav tree. Provider supplies router and locale. */
export function ShellNavigation({items, selectedRoute, isExpanded, onExpandedChange,
  label = 'Primary navigation', expandLabel = 'Expand navigation', collapseLabel = 'Collapse navigation'}: {
  items: NavigationItem[]; selectedRoute: string; isExpanded: boolean;
  onExpandedChange: (value: boolean) => void; label?: string; expandLabel?: string; collapseLabel?: string;
}) {
  const id = useId();
  const containsRoute = (item: NavigationItem): boolean => item.href === selectedRoute || !!item.children?.some(containsRoute);
  const renderItem = (item: NavigationItem, depth = 0): ReactNode => {
    const current = item.href === selectedRoute;
    const active = current || (!isExpanded && containsRoute(item));
    return <li key={item.id} data-navigation-group>
      <TooltipTrigger placement="end">
        <Link href={item.href} isDisabled={item.isDisabled} aria-label={item.label} aria-current={current ? 'page' : undefined}
          data-navigation-route data-active={active} data-depth={depth > 0 ? 'nested' : 'root'}
          className={state => routeStyle({...state, isActive: active})}>
          <span data-navigation-indicator aria-hidden="true" className={style({
            borderRadius: 'full', backgroundColor: {default: 'transparent', isActive: {default: 'neutral', forcedColors: 'Highlight'}}
          })({isActive: active})} />
          <span data-navigation-icon aria-hidden="true">{item.icon}</span>
          <span data-navigation-label>{item.label}</span>
        </Link>
        <Tooltip>{item.label}</Tooltip>
      </TooltipTrigger>
      {isExpanded && item.children?.length ? <ul data-navigation-list>{item.children.map(child => renderItem(child, depth + 1))}</ul> : null}
    </li>;
  };
  return <nav aria-label={label} data-shell-navigation data-expanded={isExpanded}
    className={style({backgroundColor: 'base'})}>
    <div id={id} data-navigation-top><ul data-navigation-list>{items.map(item => renderItem(item))}</ul></div>
    <div data-navigation-bottom><AnimatedPanelToggle isExpanded={isExpanded}
      onExpandedChange={onExpandedChange} controls={id} expandLabel={expandLabel} collapseLabel={collapseLabel} /></div>
  </nav>;
}
