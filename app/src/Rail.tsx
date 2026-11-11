import {useEffect, useState} from 'react';
import Brush from '@react-spectrum/s2/icons/Brush';
import Export from '@react-spectrum/s2/icons/Export';
import Filmstrip from '@react-spectrum/s2/icons/Filmstrip';
import Settings from '@react-spectrum/s2/icons/Settings';
import ViewList from '@react-spectrum/s2/icons/ViewList';
import {useApp} from './appState';
import {ShellNavigation} from './shell/ShellNavigation';

const RAIL_KEY = 'dc-rail-collapsed';

export function Rail() {
  const {route, hasProject} = useApp();
  const [expanded, setExpanded] = useState(() => {
    try { return localStorage.getItem(RAIL_KEY) === '0'; } catch { return false; }
  });
  useEffect(() => {
    void window.dlss.launchFlags().then(launch => {
      if (launch.rail) setExpanded(launch.rail === 'expanded');
    });
  }, []);
  return <div className={`rail${expanded ? '' : ' is-collapsed'}`}>
    <ShellNavigation label="主导航" selectedRoute={`#/${route}`} isExpanded={expanded}
      expandLabel="展开导航" collapseLabel="收起导航"
      onExpandedChange={value => {
        setExpanded(value);
        try { localStorage.setItem(RAIL_KEY, value ? '0' : '1'); } catch { /* unavailable storage */ }
      }}
      items={[
        {id: 'project', href: '#/project', label: '项目', icon: <Filmstrip />},
        {id: 'export', href: '#/export', label: '导出', icon: <Export />, isDisabled: !hasProject},
        {id: 'jobs', href: '#/jobs', label: 'Jobs', icon: <ViewList />, isDisabled: !hasProject},
        {id: 'nrtool', href: '#/nrtool', label: 'NR / FG', icon: <Brush />},
        {id: 'settings', href: '#/settings', label: '设置', icon: <Settings />}
      ]} />
  </div>;
}
