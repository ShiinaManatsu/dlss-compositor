import {useEffect, useState} from 'react';
import {ActionButton, Button, Text, ToggleButton, ToggleButtonGroup} from '@react-spectrum/s2';
import Add from '@react-spectrum/s2/icons/Add';
import Brush from '@react-spectrum/s2/icons/Brush';
import Export from '@react-spectrum/s2/icons/Export';
import Filmstrip from '@react-spectrum/s2/icons/Filmstrip';
import Settings from '@react-spectrum/s2/icons/Settings';
import ViewList from '@react-spectrum/s2/icons/ViewList';
import {useApp, type Route} from './appState';

const RAIL_KEY = 'dc-rail-collapsed';

function SidebarIcon() {
  return (
    <svg viewBox="0 0 20 20" width="20" height="20" aria-hidden="true" focusable="false">
      <path
        fill="currentColor"
        d="M15.75 18H4.25C3.01 18 2 16.99 2 15.75V4.25C2 3.01 3.01 2 4.25 2H15.75C16.99 2 18 3.01 18 4.25V15.75C18 16.99 16.99 18 15.75 18ZM4.25 3.5C3.84 3.5 3.5 3.84 3.5 4.25V15.75C3.5 16.16 3.84 16.5 4.25 16.5H15.75C16.16 16.5 16.5 16.16 16.5 15.75V4.25C16.5 3.84 16.16 3.5 15.75 3.5H4.25Z"
      />
      <rect x="5" y="5" width="4" height="10" rx="0.5" fill="currentColor" />
    </svg>
  );
}

export function Rail() {
  const {route, setRoute, hasProject, newProject} = useApp();
  const [collapsed, setCollapsed] = useState(() => {
    try {
      return window.localStorage.getItem(RAIL_KEY) === '1';
    } catch {
      return false;
    }
  });

  // Screenshot harness hook: --rail=collapsed|expanded forces the state.
  useEffect(() => {
    void window.dlss.launchFlags().then((launch) => {
      if (launch.rail === 'collapsed') setCollapsed(true);
      else if (launch.rail === 'expanded') setCollapsed(false);
    });
  }, []);

  function toggleCollapsed() {
    setCollapsed((prev) => {
      const next = !prev;
      try {
        window.localStorage.setItem(RAIL_KEY, next ? '1' : '0');
      } catch {
        /* ignore */
      }
      return next;
    });
  }

  return (
    <nav className={`chrome rail${collapsed ? ' is-collapsed' : ''}`} aria-label="主导航">
      <Button variant="accent" aria-label="新建" onPress={() => void newProject()}>
        <Add />
        {collapsed ? null : <Text>新建</Text>}
      </Button>
      <ToggleButtonGroup
        aria-label="页面"
        orientation="vertical"
        selectionMode="single"
        isQuiet
        density="compact"
        disallowEmptySelection
        selectedKeys={new Set([route])}
        onSelectionChange={(keys) => {
          const next = [...keys][0];
          if (next != null) setRoute(String(next) as Route);
        }}>
        <ToggleButton id="project">
          <Filmstrip />
          {collapsed ? null : <Text>项目</Text>}
        </ToggleButton>
        <ToggleButton id="export" isDisabled={!hasProject}>
          <Export />
          {collapsed ? null : <Text>导出</Text>}
        </ToggleButton>
        <ToggleButton id="jobs" isDisabled={!hasProject}>
          <ViewList />
          {collapsed ? null : <Text>Jobs</Text>}
        </ToggleButton>
        <ToggleButton id="nrtool">
          <Brush />
          {collapsed ? null : <Text>NR / FG</Text>}
        </ToggleButton>
        <ToggleButton id="settings">
          <Settings />
          {collapsed ? null : <Text>设置</Text>}
        </ToggleButton>
      </ToggleButtonGroup>
      <div className="rail-spacer" />
      <ActionButton isQuiet aria-label="Toggle sidebar" onPress={toggleCollapsed}>
        <div slot="icon">
          <SidebarIcon />
        </div>
      </ActionButton>
    </nav>
  );
}
