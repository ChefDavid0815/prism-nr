import { app, BrowserWindow, dialog, ipcMain, shell } from 'electron';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { setCoreDir, setShaderDir } from './core/installer';
import { setBundledNvidiaDir } from './core/nvidia';
import { setBundledComponentsDir } from './core/components';
import { PrismService } from './core/service';
import { AccountService } from './core/account';
import type { AdminAction, CommunityQuery } from './shared/account';
import { initStore } from './core/store';
import { log } from './core/util';
import type { InstallPlan, PlanRequest } from './shared/types';

const isDev = Boolean(process.env.PRISM_DEV);
let win: BrowserWindow | null = null;
let service: PrismService;

function createWindow(): void {
  win = new BrowserWindow({
    width: 1440,
    height: 900,
    minWidth: 1120,
    minHeight: 720,
    backgroundColor: '#000000',
    title: 'Prism NR',
    autoHideMenuBar: true,
    show: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });
  win.once('ready-to-show', () => win?.show());
  // Keep navigation inside the app; external links open in the system browser.
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https:\/\//.test(url)) void shell.openExternal(url);
    return { action: 'deny' };
  });
  win.webContents.on('will-navigate', (e, url) => {
    if (!url.startsWith('http://localhost:5173') && !url.startsWith('file://')) e.preventDefault();
  });
  if (isDev) void win.loadURL('http://localhost:5173');
  else void win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
}

function handle<T extends unknown[], R>(channel: string, fn: (...args: T) => Promise<R> | R): void {
  ipcMain.handle(channel, async (_e, ...args) => {
    try {
      return { ok: true, value: await fn(...(args as T)) };
    } catch (err) {
      log(`${channel} failed: ${(err as Error).stack ?? err}`);
      return { ok: false, error: (err as Error).message };
    }
  });
}

app.whenReady().then(() => {
  initStore(app.getPath('userData'));
  const shaders = app.isPackaged ? path.join(process.resourcesPath, 'prism-shaders', 'Shaders') : path.join(__dirname, '..', 'plugins', 'prism-shaders', 'Shaders');
  setShaderDir(shaders);
  setBundledComponentsDir(app.isPackaged ? path.join(process.resourcesPath, 'components') : path.join(__dirname, '..', 'vendor-components'));
  setBundledNvidiaDir(app.isPackaged ? path.join(process.resourcesPath, 'nvidia') : path.join(__dirname, '..', 'vendor-nvidia'));
  setCoreDir(app.isPackaged ? path.join(process.resourcesPath, 'prism-core') : path.join(__dirname, '..', 'native', 'prism-core', 'build'));
  service = new PrismService();
  const account = new AccountService(app.getPath('userData'));
  handle('account.state', () => account.state());
  handle('account.signUp', (args: { email: string; password: string; username?: string; phone?: string }) => account.signUp(args));
  handle('account.verify', (email: string, code: string) => account.verify(email, code));
  handle('account.resend', (email: string) => account.resend(email));
  handle('account.signIn', (identifier: string, password: string) => account.signIn(identifier, password));
  handle('account.signOut', () => account.signOut());
  handle('account.updateProfile', (patch: { username?: string; phone?: string | null; bio?: string; avatar?: string | null }) => account.updateProfile(patch));
  handle('account.admin', (action: AdminAction) => account.admin(action));
  handle('account.packs', () => account.packs());
  handle('account.redeem', (code: string) => account.redeem(code));
  handle('community.list', (query: CommunityQuery) => account.communityList(query));
  handle('community.games', (q: string) => account.communityGames(q));
  handle('community.share', (args: { title: string; description: string; code: string }) => account.communityShare(args));
  handle('community.remove', (id: string, asAdmin?: boolean) => account.communityDelete(id, asAdmin));
  handle('community.imported', (id: string) => account.communityImported(id));

  handle('system', () => service.getSystem());
  handle('scan', () => service.scanGames());
  handle('analyze', (id: string, force?: boolean) => service.analyzeGame(id, force));
  handle('addManual', async () => {
    const r = await dialog.showOpenDialog(win!, { title: '选择游戏安装目录', properties: ['openDirectory'] });
    return r.canceled || !r.filePaths[0] ? null : service.addManual(r.filePaths[0]);
  });
  handle('removeManual', (id: string) => service.removeManual(id));
  handle('coreUpgrades', () => service.upgrades);
  handle('catalog', () => service.catalog());
  handle('components', () => service.listComponents());
  handle('importComponent', async (id: string) => {
    const r = await dialog.showOpenDialog(win!, {
      title: '选择组件压缩包或文件',
      properties: ['openFile'],
      filters: [{ name: '组件', extensions: ['zip', '7z', 'rar', 'addon64', 'addon32', 'dll', 'exe'] }],
    });
    return r.canceled || !r.filePaths[0] ? null : service.importComponent(id, r.filePaths[0]);
  });
  handle('nvidiaDlls', () => service.nvidiaDlls(true));
  handle('plan', (req: PlanRequest) => service.plan(req));
  handle('install', (plan: InstallPlan) => service.install(plan, (p) => win?.webContents.send('progress', p)));
  handle('installs', () => service.listInstalls());
  handle('rollback', (id: string) => service.rollback(id));
  handle('updateCoreConfig', (id: string, values: Record<string, number | boolean | string>) => service.updateCoreConfig(id, values));
  handle('diagnostics', () => service.diagnostics());
  handle('exportDiagnostics', async () => {
    const r = await dialog.showSaveDialog(win!, { title: '导出诊断报告', defaultPath: `prism-nr-diagnostics-${Date.now()}.json` });
    if (r.canceled || !r.filePath) return null;
    fs.writeFileSync(r.filePath, JSON.stringify(await service.diagnostics(), null, 2), 'utf8');
    return r.filePath;
  });
  handle('settings', () => service.getSettings());
  handle('setMotion', (m: 'max' | 'standard' | 'off') => service.setMotion(m));
  handle('openPath', (p: string) => shell.openPath(p).then(() => undefined));
  handle('openExternal', (url: string) => (/^https:\/\//.test(url) ? shell.openExternal(url) : undefined));

  createWindow();
});

app.on('window-all-closed', () => app.quit());
