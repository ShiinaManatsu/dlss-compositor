import { contextBridge } from 'electron'

// Context bridge stub — IPC bindings will be added in Task 10
contextBridge.exposeInMainWorld('api', {})
