export const IPC_CHANNELS = {
  DIALOG_OPEN_DIRECTORY: 'dialog:openDirectory',
  DIALOG_OPEN_FILE: 'dialog:openFile',
  PROCESS_START: 'process:start',
  PROCESS_STOP: 'process:stop',
  PROCESS_PROGRESS: 'process:progress',
  PROCESS_ERROR: 'process:error',
  PROCESS_COMPLETE: 'process:complete',
  SETTINGS_GET: 'settings:get',
  SETTINGS_SAVE: 'settings:save',
} as const;
