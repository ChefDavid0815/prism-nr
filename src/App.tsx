import { useCallback, useEffect, useMemo, useState } from 'react';
import type { GameAnalysis, GameEntry, InstallRecord, SystemInfo } from '../electron/shared/types';
import { PLUGINS } from '../electron/shared/catalog';
import { api, isPreview } from './api';
import { PrismMark } from './components/PrismMark';
import { RollingNumber } from './components/RollingNumber';
import { Sweep } from './components/Sweep';
import { MotionContext, type MotionLevel } from './lib/motion';
import { DiagnosticsView } from './views/DiagnosticsView';
import { HistoryView } from './views/HistoryView';
import { LibraryView } from './views/LibraryView';
import { PluginsView } from './views/PluginsView';
import { SettingsView } from './views/SettingsView';

type View = 'library' | 'plugins' | 'history' | 'diagnostics' | 'settings';

const TIER_LABEL: Record<string, string> = { flagship: '旗舰', high: '高端', mainstream: '主流', entry: '入门', legacy: '上代', unsupported: '不支持' };

export function App() {
  const [view, setView] = useState<View>('library');
  const [system, setSystem] = useState<SystemInfo | null>(null);
  const [games, setGames] = useState<GameEntry[]>([]);
  const [analyses, setAnalyses] = useState<Record<string, GameAnalysis>>({});
  const [scanning, setScanning] = useState(false);
  const [installs, setInstalls] = useState<InstallRecord[]>([]);
  const [motion, setMotionState] = useState<MotionLevel>('max');
  const [sweep, setSweep] = useState(0);
  const [error, setError] = useState<string | null>(null);

  const refreshInstalls = useCallback(() => api.listInstalls().then(setInstalls).catch(() => {}), []);

  const scan = useCallback(async () => {
    setScanning(true);
    setError(null);
    try {
      const list = await api.scanGames();
      setGames(list);
      // Analyse one at a time so rows light up as results arrive.
      for (const g of list) {
        const a = await api.analyzeGame(g.id).catch(() => null);
        if (a) setAnalyses((prev) => ({ ...prev, [g.id]: a }));
      }
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setScanning(false);
    }
  }, []);

  useEffect(() => {
    api.getSystem().then(setSystem).catch((e) => setError((e as Error).message));
    api.getSettings().then((s) => setMotionState(s.motion)).catch(() => {});
    refreshInstalls();
    void scan();
  }, [scan, refreshInstalls]);

  useEffect(() => {
    document.documentElement.dataset.motion = motion;
  }, [motion]);

  const setMotion = (m: MotionLevel) => {
    setMotionState(m);
    void api.setMotion(m);
  };

  const activeCount = installs.filter((r) => r.status === 'active').length;
  const gpu = system?.primary;
  const readyCount = useMemo(() => Object.values(analyses).filter((a) => a.recommendedRoute !== 'blocked' && a.recommendedRoute !== 'unknown').length, [analyses]);

  const nav: { id: View; label: string; count?: string }[] = [
    { id: 'library', label: '游戏库', count: String(games.length).padStart(2, '0') },
    { id: 'plugins', label: '插件', count: String(PLUGINS.length).padStart(2, '0') },
    { id: 'history', label: '还原记录', count: String(activeCount).padStart(2, '0') },
    { id: 'diagnostics', label: '诊断' },
    { id: 'settings', label: '设置' },
  ];

  const onApplied = useCallback(() => {
    if (motion !== 'off') setSweep(Date.now());
    void refreshInstalls();
  }, [motion, refreshInstalls]);

  return (
    <MotionContext.Provider value={motion}>
      <div className="shell">
        <header className="topbar">
          <div className="brand">
            <PrismMark />
            <span className="brand-name">Prism NR</span>
          </div>
          <dl className="readout">
            <div>
              <dt>GPU</dt>
              <dd>{gpu ? gpu.name.replace(/^NVIDIA\s+GeForce\s+/i, '') : '检测中'}</dd>
            </div>
            <div>
              <dt>档位</dt>
              <dd>{gpu ? TIER_LABEL[gpu.tier] : '—'}</dd>
            </div>
            <div>
              <dt>VRAM</dt>
              <dd>{gpu?.vramMB ? <RollingNumber value={`${(gpu.vramMB / 1024).toFixed(0)} GB`} /> : '—'}</dd>
            </div>
            <div>
              <dt>驱动</dt>
              <dd>{gpu?.driver ? <RollingNumber value={gpu.driver} /> : '—'}</dd>
            </div>
            <div>
              <dt>神经渲染</dt>
              <dd className={`nr-state nr-${gpu?.nrSupport ?? 'none'}`}>{gpu ? { official: '官方支持', experimental: '实验性', none: '不可用' }[gpu.nrSupport] : '—'}</dd>
            </div>
            <div>
              <dt>可启用</dt>
              <dd>
                <RollingNumber value={`${readyCount}/${games.length}`} />
              </dd>
            </div>
          </dl>
          <button className="btn btn-line" onClick={() => void scan()} disabled={scanning}>
            {scanning ? '扫描中' : '重新扫描'}
          </button>
        </header>

        <nav className="rail" aria-label="主导航">
          {nav.map((n) => (
            <button key={n.id} className={`rail-item ${view === n.id ? 'is-active' : ''}`} onClick={() => setView(n.id)} aria-current={view === n.id ? 'page' : undefined}>
              <span>{n.label}</span>
              {n.count && <span className="rail-count">{n.count}</span>}
            </button>
          ))}
          <div className="rail-foot">
            {isPreview && <p className="rail-note">预览模式：数据为示例</p>}
            <p>v{__APP_VERSION__}</p>
          </div>
        </nav>

        <main className="stage" key={view}>
          {error && (
            <div className="banner banner-blocked" role="alert">
              {error}
            </div>
          )}
          {view === 'library' && <LibraryView games={games} analyses={analyses} scanning={scanning} system={system} installs={installs} onApplied={onApplied} onRolledBack={refreshInstalls} onTuned={refreshInstalls} onAddManual={async () => { const g = await api.addManualGame(); if (g) await scan(); }} />}
          {view === 'plugins' && <PluginsView />}
          {view === 'history' && <HistoryView installs={installs} onChange={refreshInstalls} />}
          {view === 'diagnostics' && <DiagnosticsView />}
          {view === 'settings' && <SettingsView motion={motion} onMotion={setMotion} />}
        </main>
        <Sweep run={sweep} onDone={() => setSweep(0)} />
      </div>
    </MotionContext.Provider>
  );
}
