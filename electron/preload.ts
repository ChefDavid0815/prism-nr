import { contextBridge, ipcRenderer } from 'electron';
import type { InstallProgress, PrismApi } from './shared/types';

async function call<T>(channel: string, ...args: unknown[]): Promise<T> {
  const r = (await ipcRenderer.invoke(channel, ...args)) as { ok: boolean; value?: T; error?: string };
  if (!r.ok) throw new Error(r.error ?? '未知错误');
  return r.value as T;
}

const api: PrismApi = {
  getSystem: () => call('system'),
  scanGames: () => call('scan'),
  analyzeGame: (id) => call('analyze', id),
  addManualGame: () => call('addManual'),
  removeManualGame: (id) => call('removeManual', id),
  coreUpgrades: () => call('coreUpgrades'),
  getCatalog: () => call('catalog'),
  listComponents: () => call('components'),
  importComponent: (id) => call('importComponent', id),
  locateNvidiaDlls: () => call('nvidiaDlls'),
  planInstall: (req) => call('plan', req),
  runInstall: (plan) => call('install', plan),
  onProgress: (cb) => {
    const listener = (_e: unknown, p: InstallProgress) => cb(p);
    ipcRenderer.on('progress', listener);
    return () => ipcRenderer.removeListener('progress', listener);
  },
  listInstalls: () => call('installs'),
  rollback: (id) => call('rollback', id),
  updateCoreConfig: (id, values) => call('updateCoreConfig', id, values),
  diagnostics: () => call('diagnostics'),
  exportDiagnostics: () => call('exportDiagnostics'),
  getSettings: () => call('settings'),
  setMotion: (m) => call('setMotion', m),
  openPath: (p) => call('openPath', p),
  openExternal: (url) => call('openExternal', url),
  account: {
    state: () => call('account.state'),
    signUp: (args) => call('account.signUp', args),
    verify: (email, code) => call('account.verify', email, code),
    resend: (email) => call('account.resend', email),
    signIn: (identifier, password) => call('account.signIn', identifier, password),
    signOut: () => call('account.signOut'),
    updateProfile: (patch) => call('account.updateProfile', patch),
    admin: (action) => call('account.admin', action),
    packs: () => call('account.packs'),
    redeem: (code) => call('account.redeem', code),
  },
  community: {
    list: (query) => call('community.list', query),
    share: (args) => call('community.share', args),
    remove: (id, asAdmin) => call('community.remove', id, asAdmin),
    imported: (id) => call('community.imported', id),
  },
};

contextBridge.exposeInMainWorld('prism', api);
