import '@react-spectrum/s2/page.css';
import type {ReactNode} from 'react';
import {Button, Provider, Text} from '@react-spectrum/s2';
import StopProcessing from '@react-spectrum/s2/icons/StopProcessing';
import {AppStateProvider, ROUTE_LABELS, useApp} from './appState';
import {projectTitle} from './dcp';
import {Rail} from './Rail';
import {TitleBar} from './TitleBar';
import {ProjectPage} from './pages/ProjectPage';
import {ExportPage} from './pages/ExportPage';
import {JobsPage} from './pages/JobsPage';
import {SettingsPage} from './pages/SettingsPage';
import './shell.css';

function Shell() {
  const app = useApp();
  const crumbs =
    app.route === 'project' ? ['项目'] : [projectTitle(app.dcpPath), ROUTE_LABELS[app.route]];

  let startAction: ReactNode = null;
  if (app.route === 'export' && app.hasProject) {
    startAction = app.busy ? (
      <Button variant="secondary" size="S" onPress={() => void app.stopExport()}>
        <StopProcessing />
        <Text>停止</Text>
      </Button>
    ) : (
      <Button
        variant="accent"
        size="S"
        isDisabled={!app.canStart}
        onPress={() => void app.startExport()}>
        <Text>发起任务</Text>
      </Button>
    );
  }

  return (
    <div className="shell">
      <TitleBar
        crumbs={crumbs}
        maximized={app.maximized}
        onMinimize={app.windowMinimize}
        onMaximize={app.windowMaximize}
        onClose={app.windowClose}>
        {startAction}
      </TitleBar>
      <div className="shell-body">
        <Rail />
        <main className="page">
          {app.route === 'project' ? (
            <ProjectPage />
          ) : app.route === 'export' ? (
            <ExportPage />
          ) : app.route === 'jobs' ? (
            <JobsPage />
          ) : (
            <SettingsPage />
          )}
        </main>
      </div>
    </div>
  );
}

function ThemedShell() {
  const app = useApp();
  return (
    <Provider
      colorScheme={app.colorScheme === 'system' ? undefined : app.colorScheme}
      locale="zh-CN"
      background="base">
      <Shell />
    </Provider>
  );
}

export default function App() {
  return (
    <AppStateProvider>
      <ThemedShell />
    </AppStateProvider>
  );
}
