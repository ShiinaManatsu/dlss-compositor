import {contextBridge, ipcRenderer} from 'electron';

contextBridge.exposeInMainWorld('dlss', {
  probe: () => ipcRenderer.invoke('pipeline:probe'),
  inspect: (blend: string) => ipcRenderer.invoke('pipeline:inspect', blend),
  start: (req: unknown) => ipcRenderer.invoke('pipeline:start', req),
  stop: () => ipcRenderer.invoke('pipeline:stop'),
  nrOpen: (params: {color_source: string; buffer_dir: string}) =>
    ipcRenderer.invoke('nrtool:open', params),
  nrPreview: (frame: number, settings: Record<string, unknown>) =>
    ipcRenderer.invoke('nrtool:preview', frame, settings),
  nrExport: (params: Record<string, unknown>) => ipcRenderer.invoke('nrtool:export', params),
  nrClose: () => ipcRenderer.invoke('nrtool:close'),
  nrStop: () => ipcRenderer.invoke('nrtool:stop'),
  chooseNrVideo: () => ipcRenderer.invoke('dialog:nrVideo'),
  chooseDir: (title: string) => ipcRenderer.invoke('dialog:pickDir', title),
  chooseNrVideoSave: (defaultPath?: string) =>
    ipcRenderer.invoke('dialog:nrVideoSave', defaultPath ?? ''),
  chooseBlend: () => ipcRenderer.invoke('dialog:blend'),
  chooseExportDir: () => ipcRenderer.invoke('dialog:exportDir'),
  chooseDcpOpen: () => ipcRenderer.invoke('dialog:dcpOpen'),
  chooseDcpSave: (defaultPath?: string) => ipcRenderer.invoke('dialog:dcpSave', defaultPath ?? ''),
  loadDcp: (filePath: string) => ipcRenderer.invoke('project:load', filePath),
  saveDcp: (filePath: string, project: unknown) => ipcRenderer.invoke('project:save', filePath, project),
  listRecent: () => ipcRenderer.invoke('project:recent'),
  rememberRecent: (filePath: string) => ipcRenderer.invoke('project:remember', filePath),
  exists: (filePath: string) => ipcRenderer.invoke('fs:exists', filePath),
  openPath: (filePath: string) => ipcRenderer.invoke('shell:openPath', filePath),
  listDir: (dirPath: string) => ipcRenderer.invoke('fs:listDir', dirPath),
  launchFlags: () => ipcRenderer.invoke('app:launchFlags'),
  setTitle: (title: string) => ipcRenderer.invoke('window:setTitle', title),
  windowMinimize: () => ipcRenderer.invoke('window:minimize'),
  windowMaximize: () => ipcRenderer.invoke('window:maximize'),
  windowClose: () => ipcRenderer.invoke('window:close'),
  windowMaximized: () => ipcRenderer.invoke('window:isMaximized'),
  previewDataUrl: (filePath: string) => ipcRenderer.invoke('preview:dataUrl', filePath),
  onMaximized: (callback: (maximized: boolean) => void) => {
    const listener = (_event: unknown, maximized: boolean) => callback(maximized);
    ipcRenderer.on('window:maximized', listener);
    return () => ipcRenderer.removeListener('window:maximized', listener);
  },
  onEvent: (callback: (event: Record<string, unknown>) => void) => {
    const listener = (_event: unknown, payload: Record<string, unknown>) => callback(payload);
    ipcRenderer.on('pipeline:event', listener);
    return () => ipcRenderer.removeListener('pipeline:event', listener);
  },
});
