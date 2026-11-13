import '@react-spectrum/s2/page.css';
import {useEffect, useState, type ReactNode} from 'react';
import {Button, Text} from '@react-spectrum/s2/Button';
import {Provider} from '@react-spectrum/s2/Provider';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};
import StopProcessing from '@react-spectrum/s2/icons/StopProcessing';
import {AppStateProvider, ROUTE_LABELS, useApp, type Route} from './appState';
import {ThemeCycleButton} from './shell/ThemeCycleButton';
import {NrSessionProvider, useNrSession} from './nrSession';
import {projectTitle} from './dcp';
import {Rail} from './Rail';
import {TitleBar} from './TitleBar';
import {ProjectPage} from './pages/ProjectPage';
import {ExportPage} from './pages/ExportPage';
import {JobsPage} from './pages/JobsPage';
import {NrToolPage} from './pages/NrToolPage';
import {SettingsPage} from './pages/SettingsPage';
import './shell.css';

function Shell() {
  const app = useApp();
  const nr = useNrSession();
  const nrActive = app.route === 'nrtool';
  const [nrVisited, setNrVisited] = useState(false);
  useEffect(() => {
    if (nrActive) setNrVisited(true);
  }, [nrActive]);
  const crumbs =
    app.route === 'project'
      ? ['项目']
      : app.route === 'nrtool'
        ? ['NR / FG']
        : app.hasProject ? [projectTitle(app.dcpPath), ROUTE_LABELS[app.route]] : [ROUTE_LABELS[app.route]];

  let titleAction: ReactNode = null;
  if (nr.exporting) {
    titleAction = (
      <Button variant="secondary" onPress={() => void nr.stopExport()}>
        <StopProcessing />
        <Text>停止</Text>
      </Button>
    );
  } else if (app.route === 'export' && app.hasProject) {
    titleAction = app.busy ? (
      <Button variant="secondary" onPress={() => void app.stopExport()}>
        <StopProcessing />
        <Text>停止</Text>
      </Button>
    ) : (
      <Button
        variant="accent"
        isDisabled={!app.canStart}
        onPress={() => void app.startExport()}>
        <Text>导出</Text>
      </Button>
    );
  } else if (nrActive) {
    titleAction = (
      <Button
        variant="accent"
        isDisabled={nr.opening || !nr.info || !nr.exportTrigger}
        onPress={() => nr.exportTrigger?.()}>
        <Text>导出</Text>
      </Button>
    );
  }

  return (
    <div className={`shell ${style({font: 'body', color: 'neutral', backgroundColor: 'base',
      '--shell-gap': {type: 'width', value: 16},
      '--surface-padding': {type: 'width', value: 20},
      '--control-gap': {type: 'width', value: 8},
      '--section-gap': {type: 'width', value: 12},
      '--title-height': {type: 'height', value: 64},
      '--caption-size': {type: 'width', value: 48},
      '--rail-collapsed': {type: 'width', value: 72},
      '--rail-expanded': {type: 'width', value: 240},
      '--leading-slot': {type: 'width', value: 40},
      '--axis-offset': {type: 'width', value: 4}})}`}>
      <TitleBar
        isLocked={nr.exporting}
        onBack={app.route === 'project' ? undefined : app.navigateBack}
        onParent={() => app.setRoute('project')}
        crumbs={crumbs}
        maximized={app.maximized}
        onMinimize={app.windowMinimize}
        onMaximize={app.windowMaximize}
        onClose={app.windowClose}>
        {titleAction}
        <ThemeCycleButton preference={app.colorScheme} onChange={app.setColorScheme} isDisabled={nr.exporting}
          names={{light: '浅色', dark: '深色', system: '跟随系统'}}
          formatLabel={(current, next) => `主题 ${current} · 切换至${next}`}
          formatTooltip={(_, next) => `切换至${next}`} />
      </TitleBar>
      <div className="shell-body">
        <Rail isLocked={nr.exporting} />
        <main className="page">
          {(nrActive || nrVisited) && (
            <div className="nr-page-host" hidden={!nrActive}>
              <NrToolPage active={nrActive} />
            </div>
          )}
          {app.route === 'project' ? (
            <ProjectPage />
          ) : app.route === 'export' ? (
            <ExportPage />
          ) : app.route === 'jobs' ? (
            <JobsPage />
          ) : app.route === 'settings' ? (
            <SettingsPage />
          ) : null}
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
      background="base"
      router={{navigate: path => {
        const route = path.replace(/^#\//, '');
        if (route in ROUTE_LABELS) app.setRoute(route as Route);
      }}}>
      <Shell />
    </Provider>
  );
}

export default function App() {
  return (
    <NrSessionProvider>
      <AppStateProvider>
        <ThemedShell />
      </AppStateProvider>
    </NrSessionProvider>
  );
}
