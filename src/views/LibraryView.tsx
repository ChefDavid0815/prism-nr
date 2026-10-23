import { useEffect, useMemo, useRef, useState } from 'react';
import { LEGACY_PRESET_NAMES, NET_SIZES, pluginById, PRESETS, resolveValues, tierLocked, tierOf } from '../../electron/shared/catalog';
import type { GameAnalysis, GameEntry, InstallPlan, InstallProgress, InstallRecord, PresetDef, PresetId, Route, SystemInfo } from '../../electron/shared/types';
import { api } from '../api';
import { Barcode } from '../components/Barcode';
import { DataField } from '../components/DataField';
import { RollingNumber } from '../components/RollingNumber';
import { TickRange } from '../components/TickRange';
import { fingerprint, routeTone, TONE_COLOR } from '../lib/fingerprint';

export const ROUTE_LABEL: Record<Route, string> = {
  'prism-core': 'Prism Core',
  'native-hook': '原生挂钩',
  optiscaler: '前置 OptiScaler',
  feeder: '输入补全',
  blocked: '已阻止',
  unknown: '未识别',
};

const STORE_LABEL: Record<string, string> = { steam: 'STEAM', epic: 'EPIC', gog: 'GOG', xbox: 'XBOX', manual: '手动' };

interface Props {
  games: GameEntry[];
  analyses: Record<string, GameAnalysis>;
  scanning: boolean;
  system: SystemInfo | null;
  installs: InstallRecord[];
  onApplied: () => void;
  onRolledBack: () => void;
  onTuned: () => void;
  onRemoveManual: (id: string) => Promise<void>;
  onAddManual: () => void;
}

/** Prism Core is the only route: a game either runs it, or is blocked (anti-cheat, no DX12 DLSS). */
export function coreRoute(a: GameAnalysis): Route {
  if (a.recommendedRoute === 'blocked' || a.recommendedRoute === 'unknown') return a.recommendedRoute;
  return a.routes.includes('prism-core') ? 'prism-core' : 'blocked';
}

function coreReason(a: GameAnalysis): string {
  return coreRoute(a) === 'blocked' && a.recommendedRoute !== 'blocked' ? '这个游戏用不了 Prism Core：它需要 DX12、64 位并且自带 DLSS 的游戏。' : a.routeReason;
}

function groupOf(a: GameAnalysis | undefined): 0 | 1 | 2 {
  if (!a) return 2;
  const r = coreRoute(a);
  return r === 'prism-core' ? 0 : r === 'blocked' ? 1 : 2;
}

const GROUPS = ['可以直接启用', '暂时用不了', '分析中或未识别'];

export function LibraryView({ games, analyses, scanning, system, installs, onApplied, onRolledBack, onTuned, onRemoveManual, onAddManual }: Props) {
  const [selected, setSelected] = useState<string | null>(null);
  const [hovered, setHovered] = useState<string | null>(null);
  const [progress, setProgress] = useState<InstallProgress | null>(null);
  const [plan, setPlan] = useState<InstallPlan | null>(null);

  useEffect(() => {
    if ((!selected || !games.some((g) => g.id === selected)) && games.length) {
      const firstReady = games.find((g) => groupOf(analyses[g.id]) === 0) ?? games[0];
      setSelected(firstReady.id);
    }
  }, [games, analyses, selected]);

  const groups = useMemo(() => {
    const out: GameEntry[][] = [[], [], []];
    for (const g of games) out[groupOf(analyses[g.id])].push(g);
    return out;
  }, [games, analyses]);

  const game = games.find((g) => g.id === selected) ?? null;
  const analysis = selected ? analyses[selected] : undefined;
  const bars = useMemo(() => (game ? fingerprint(game, analysis) : []), [game, analysis]);
  const isCore = plan?.route === 'prism-core';
  const intensity = plan ? Number(plan.values[isCore ? 'prism-core.intensity' : 'nr-renodx.intensity'] ?? 30) / 100 : 0.3;
  const fieldIntensity = Math.min(1, intensity);
  const modelRes = plan ? Number(plan.values['nr-optiscaler.modelResolution'] ?? 100) : 100;

  const fieldLabels = {
    left: [
      `API   ${analysis?.apis.map((x) => x.toUpperCase()).join(' ') || '—'}`,
      `BIT   ${analysis?.bitness ?? '—'}`,
      `DLSS  ${analysis?.nativeDlss.version ?? (analysis ? '无' : '—')}`,
      `ROUTE ${analysis ? ROUTE_LABEL[plan?.route ?? coreRoute(analysis)] : '—'}`,
    ],
    right: [
      `NR    ${Math.round(intensity * 100)}%`,
      `NET   1/${plan?.values['prism-core.cadence'] ?? 2}`,
      `PASS  ${plan?.values['prism-core.passes'] ?? 2}`,
      `SEED  ${game ? game.id.slice(0, 6).toUpperCase() : '—'}`,
    ],
  };

  return (
    <div className="library">
      <section className="library-main">
        <DataField bars={bars} intensity={fieldIntensity} busy={scanning || (progress != null && progress.stage !== 'done' && progress.stage !== 'error')} progress={progress && progress.stage !== 'done' && progress.stage !== 'error' ? progress.fraction : null} seed={game ? parseInt(game.id.slice(0, 8), 16) || 7 : 7} labels={fieldLabels} />

        <div className="index" role="listbox" aria-label="游戏" aria-activedescendant={selected ? `row-${selected}` : undefined}>
          <div className="index-head" aria-hidden="true">
            <span>游戏</span>
            <span>平台</span>
            <span>API</span>
            <span>路线</span>
            <span>数据指纹</span>
          </div>
          {!games.length && !scanning && (
            <div className="empty">
              <p>没有找到游戏。Prism 会扫描 Steam、Epic、GOG 和 Xbox 的安装目录。</p>
              <button className="btn btn-line" onClick={onAddManual}>
                手动添加游戏目录
              </button>
            </div>
          )}
          {!games.length && scanning && <div className="empty"><p>正在扫描游戏库</p></div>}
          {groups.map((list, gi) =>
            list.length ? (
              <div className="index-group" key={gi}>
                <div className="index-group-head">
                  <span>{GROUPS[gi]}</span>
                  <span className="mono">{String(list.length).padStart(2, '0')}</span>
                </div>
                {list.map((g) => {
                  const a = analyses[g.id];
                  const active = installs.some((r) => r.gameId === g.id && r.status === 'active');
                  const isSel = g.id === selected;
                  const tone = a?.antiCheatOptIn ? 'caution' : routeTone(a ? coreRoute(a) : undefined);
                  return (
                    <div
                      key={g.id}
                      id={`row-${g.id}`}
                      role="option"
                      aria-selected={isSel}
                      tabIndex={0}
                      className={`row ${isSel ? 'is-selected' : ''}`}
                      onClick={() => setSelected(g.id)}
                      onKeyDown={(e) => {
                        if (e.key === 'Enter' || e.key === ' ') {
                          e.preventDefault();
                          setSelected(g.id);
                        }
                      }}
                      onMouseEnter={() => setHovered(g.id)}
                      onMouseLeave={() => setHovered(null)}
                    >
                      <span className="row-name">
                        {g.name}
                        {active && <span className="row-badge">已启用</span>}
                      </span>
                      <span className="mono dim">{STORE_LABEL[g.store]}</span>
                      <span className="mono">{a ? a.apis.map((x) => x.toUpperCase()).join(' ') || '—' : '···'}</span>
                      <span className="row-route">
                        <i style={{ background: TONE_COLOR[tone] }} />
                        {a ? ROUTE_LABEL[coreRoute(a)] : '分析中'}
                      </span>
                      <Barcode bars={fingerprint(g, a)} inverted={isSel} hover={hovered === g.id} label={`${g.name} 的数据指纹`} />
                    </div>
                  );
                })}
              </div>
            ) : null,
          )}
          <button className="index-add" onClick={onAddManual}>
            手动添加游戏目录
          </button>
        </div>
      </section>

      {game && (
        <Detail
          key={game.id}
          game={game}
          analysis={analysis}
          system={system}
          record={installs.find((r) => r.gameId === game.id && r.status === 'active') ?? null}
          progress={progress?.gameId === game.id ? progress : null}
          onProgress={setProgress}
          onPlan={setPlan}
          onApplied={onApplied}
          onRolledBack={onRolledBack}
          onTuned={onTuned}
          onRemoveManual={onRemoveManual}
        />
      )}
    </div>
  );
}

// ---------------------------------------------------------------------------

const COST_WEIGHT: Record<string, number> = { high: 14, mid: 8, low: 3, tiny: 1 };
function costOf(pluginId: string): number {
  const c = pluginById(pluginId)?.cost ?? '';
  if (c.startsWith('高')) return COST_WEIGHT.high;
  if (c.startsWith('中')) return COST_WEIGHT.mid;
  if (c.startsWith('极低')) return COST_WEIGHT.tiny;
  if (c.startsWith('低')) return COST_WEIGHT.low;
  return 0;
}

interface Stage {
  label: string;
  weight: number;
  prism?: boolean;
}

function pipeline(plan: InstallPlan): Stage[] {
  const has = (id: string) => plan.plugins.includes(id);
  const prismStages: Stage[] = plan.plugins
    .filter((id) => pluginById(id)?.origin === 'prism' && id !== 'prism-capture' && id !== 'prism-compare')
    .map((id) => ({ label: pluginById(id)!.name.replace('Prism ', ''), weight: costOf(id), prism: true }));
  const nrWeight = plan.route === 'optiscaler' ? Math.max(3, Math.round(14 * (Number(plan.values['nr-optiscaler.modelResolution'] ?? 67) / 100) ** 2)) : 14;
  const stages: Stage[] = [{ label: '游戏渲染', weight: 0 }];
  if (plan.route === 'prism-core') {
    const cadence = Number(plan.values['prism-core.cadence'] ?? 2);
    const async = plan.values['prism-core.async'] !== false && cadence >= 2;
    // From the CONTROL measurements: every frame 13.4 ms, every 2nd 61% of that, every 4th async 37%.
    const passes = Math.max(1, Math.min(5, Number(plan.values['prism-core.passes'] ?? 2)));
    const size = NET_SIZES.find((n) => n.value === String(plan.values['prism-core.netSize'] ?? '100')) ?? NET_SIZES[0];
    const reduced = cadence === 1 && plan.values['prism-core.async'] === false;
    // The network's cost per pass is about 0.87 ms plus 1.11 ms per megapixel (nr_bench): at
    // 2560x1600 the base size (62.5%) costs about half of native, 150% about two thirds.
    const weight = Math.round((({ 1: 14, 2: 9, 3: 7, 4: 5 } as Record<number, number>)[cadence] + (async || cadence === 1 ? 0 : 1)) * passes * (reduced ? (size.value === '150' ? 0.65 : 0.5) : 1));
    stages.push({ label: 'DLSS', weight: 2 }, { label: `${cadence === 1 ? '神经渲染 每帧' : `神经渲染 每 ${cadence} 帧`} × ${passes} 遍${reduced ? ` · ${size.label}` : ''}`, weight });
    if (reduced) stages.push({ label: '边缘感知放大', weight: 1, prism: true });
    if (cadence > 1) stages.push({ label: '携带改动', weight: 1, prism: true });
    if (Number(plan.values['prism-core.guard'] ?? 100) > 0) stages.push({ label: '角色守护', weight: 1, prism: true });
    if (has('framegen') && plan.values['framegen.mode'] !== 'off') stages.push({ label: `帧生成 ${plan.values['framegen.mode']}`, weight: 0 });
    stages.push({ label: '输出', weight: 0 });
    return stages;
  }
  if (plan.route === 'optiscaler') stages.push({ label: `神经渲染 ${plan.values['nr-optiscaler.modelResolution']}%`, weight: nrWeight }, { label: 'DLSS 超分', weight: 2 });
  else if (plan.route === 'feeder') stages.push({ label: '参考帧', weight: 1, prism: true }, { label: '运动矢量', weight: 3 }, { label: '神经渲染', weight: nrWeight });
  else stages.push({ label: 'DLSS 超分', weight: 2 }, { label: '神经渲染', weight: nrWeight });
  stages.push(...prismStages);
  if (has('framegen') && plan.values['framegen.mode'] !== 'off') stages.push({ label: `帧生成 ${plan.values['framegen.mode']}`, weight: 0 });
  stages.push({ label: '输出', weight: 0 });
  return stages;
}

interface DetailProps {
  game: GameEntry;
  analysis: GameAnalysis | undefined;
  system: SystemInfo | null;
  record: InstallRecord | null;
  progress: InstallProgress | null;
  onProgress: (p: InstallProgress | null) => void;
  onPlan: (p: InstallPlan | null) => void;
  onApplied: () => void;
  onRolledBack: () => void;
  onTuned: () => void;
  onRemoveManual: (id: string) => Promise<void>;
}

type CoreValues = Record<string, number | boolean | string>;

function Detail({ game, analysis, system, record, progress, onProgress, onPlan, onApplied, onRolledBack, onTuned, onRemoveManual }: DetailProps) {
  const live = record?.route === 'prism-core' ? record : null;
  const [preset, setPreset] = useState<PresetId | undefined>(() => (PRESETS.find((p) => p.id === live?.preset)?.id));
  const [plan, setPlan] = useState<InstallPlan | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [justDone, setJustDone] = useState(false);
  // After rolling back a late-loading install: the Steam launch option must go too.
  const [leftover, setLeftover] = useState<string | null>(null);
  const [nonce, setNonce] = useState(0);
  const [tierNonce, setTierNonce] = useState(0);
  // Prism Core settings: start from what is installed, so the panel shows the game's live values.
  const [acceptRisk, setAcceptRisk] = useState(false);
  const [coreValues, setCoreValues] = useState<CoreValues>(() => (live ? pickCore(live.values ?? {}) : {}));

  useEffect(() => {
    if (!analysis) return;
    let alive = true;
    api
      .planInstall({ gameId: game.id, preset, overrides: coreValues, acceptRisk })
      .then((p) => {
        if (!alive) return;
        setPlan(p);
        onPlan(p);
        setPreset((cur) => cur ?? p.preset);
      })
      .catch((e) => alive && setError((e as Error).message));
    return () => {
      alive = false;
    };
  }, [game.id, analysis, preset, onPlan, nonce, coreValues, acceptRisk]);

  // A tier replaces the Core values; on an active install they are written to the game right away.
  const pickTier = (id: PresetId) => {
    const v = pickCore(resolveValues(id));
    setPreset(id);
    setCoreValues(v);
    setTierNonce((n) => n + 1);
    if (live) api.updateCoreConfig(live.id, v).then(onTuned).catch((e) => setError((e as Error).message));
  };

  useEffect(() => api.onProgress((p) => p.gameId === game.id && onProgress(p)), [game.id, onProgress]);

  const install = async () => {
    if (!plan) return;
    setBusy(true);
    setError(null);
    setJustDone(false);
    try {
      await api.runInstall(plan);
      setJustDone(true);
      onApplied();
    } catch (e) {
      setError((e as Error).message);
      onProgress({ gameId: game.id, stage: 'error', message: (e as Error).message, fraction: 0 });
    } finally {
      setBusy(false);
    }
  };

  const rollback = async () => {
    if (!record) return;
    try {
      await api.rollback(record.id);
      setLeftover(record.launchOption ?? null);
      setJustDone(false);
      onProgress(null);
      onRolledBack();
    } catch (e) {
      setError((e as Error).message);
    }
  };

  const importNeed = async (id: string) => {
    const c = await api.importComponent(id).catch((e) => {
      setError((e as Error).message);
      return null;
    });
    if (c) setNonce((n) => n + 1);
  };

  const tier = system?.primary?.tier;
  const blocked = Boolean(plan?.blockers.length);
  const needsConsent = Boolean(plan?.riskConsent && !plan.riskConsent.accepted);
  const tone = routeTone(plan?.route ?? (analysis ? coreRoute(analysis) : undefined));
  const stages = plan ? pipeline(plan) : [];
  const exeName = analysis?.exe ? analysis.exe.split(/[\\/]/).pop() : '—';
  const shownTier = plan ? tierOf(plan.values) : undefined;
  const recordName = record ? (record.route === 'prism-core' ? tierOf(record.values ?? {})?.name ?? '自定义' : LEGACY_PRESET_NAMES[record.preset] ?? record.preset) : '';

  return (
    <aside className="detail" aria-label={`${game.name} 详情`}>
      <div className="detail-scroll">
        <h1 className="detail-title">{game.name}</h1>
        <p className="detail-path mono dim">{game.installDir}</p>
        {game.store === 'manual' && <RemoveManual game={game} installed={Boolean(record)} onRemove={onRemoveManual} onError={setError} />}

        <dl className="facts">
          <div><dt>主程序</dt><dd className="mono">{exeName}</dd></div>
          <div><dt>API</dt><dd className="mono">{analysis?.apis.map((x) => x.toUpperCase()).join(' ') || '—'}</dd></div>
          <div><dt>位数</dt><dd className="mono">{analysis?.bitness ?? '—'}</dd></div>
          <div><dt>原生 DLSS</dt><dd className="mono">{analysis ? analysis.nativeDlss.version ?? (analysis.nativeDlss.present ? '有' : '无') : '—'}</dd></div>
          <div><dt>帧生成</dt><dd className="mono">{analysis ? (analysis.dlssFrameGen ? '支持' : '无') : '—'}</dd></div>
          <div><dt>反作弊</dt><dd className={analysis?.antiCheat.length ? 'tone-blocked' : 'mono'}>{analysis ? analysis.antiCheat.map((x) => x.name).join('、') || '无' : '—'}</dd></div>
        </dl>

        {plan?.riskConsent && !blocked && (
          <section className="block risk">
            <h2 className="block-title">
              <i className="tone-bar" style={{ background: TONE_COLOR.caution }} />
              风险确认
            </h2>
            <p className="block-text">
              这款游戏带有 {plan.riskConsent.names.join('、')}。它可以离线玩、不启动反作弊，所以 Prism 允许你在了解风险后自行启用。
            </p>
            <ul className="notes">
              <li><i style={{ background: TONE_COLOR.caution }} />{plan.riskConsent.how}</li>
              <li><i style={{ background: TONE_COLOR.blocked }} />带着 Prism 进入在线模式，账号可能被封禁。这一后果由你自己承担，Prism 不负责。</li>
            </ul>
            <label className="risk-accept">
              <input type="checkbox" checked={acceptRisk} onChange={(e) => setAcceptRisk(e.target.checked)} />
              <span>我已了解上面的风险，只在离线模式下使用，后果由我自己承担</span>
            </label>
          </section>
        )}

        <section className="block">
          <h2 className="block-title">
            <i className="tone-bar" style={{ background: TONE_COLOR[tone] }} />
            路线：{plan ? ROUTE_LABEL[plan.route] : analysis ? ROUTE_LABEL[coreRoute(analysis)] : '分析中'}
          </h2>
          <p className="block-text">{analysis ? coreReason(analysis) : '正在读取游戏文件。'}</p>
        </section>

        {plan && !blocked && (
          <section className="block">
            <h2 className="block-title">渲染管线</h2>
            <div className="lattice" role="list" aria-label="渲染管线与相对开销">
              {stages.map((s, i) => (
                <div className={`lattice-row ${s.prism ? 'is-prism' : ''}`} role="listitem" key={s.label + i} style={{ animationDelay: `${i * 50}ms` }}>
                  <span className="lattice-label">{s.label}</span>
                  <span className="lattice-bars" aria-hidden="true">
                    {Array.from({ length: s.weight * 2 }, (_, b) => (
                      <i key={b} style={{ animationDelay: `${i * 50 + b * 12}ms` }} />
                    ))}
                    {!s.weight && <em />}
                  </span>
                  <span className="lattice-w mono">{s.weight ? s.weight : '·'}</span>
                </div>
              ))}
            </div>
            <p className="block-note">条数表示相对开销（估算），绿色是 Prism 原生插件。实际开销取决于游戏和分辨率。</p>
          </section>
        )}

        {plan && !blocked && (
          <section className="block">
            <h2 className="block-title">档位</h2>
            <div className="presets" role="radiogroup" aria-label="档位">
              {PRESETS.map((p) => {
                const locked = tierLocked(p, system?.primary?.name);
                const fits = !locked && (p.requiresGpu ? true : tier ? p.forTiers.includes(tier) : false);
                const on = shownTier?.id === p.id;
                return (
                  <button key={p.id} role="radio" aria-checked={on} disabled={Boolean(locked)} className={`preset ${on ? 'is-on' : ''}`} onClick={() => pickTier(p.id)}>
                    <span className="preset-name">
                      {p.name}
                      {fits && <span className="preset-fit">{p.requiresGpu ? '谨慎开启' : '适合你的显卡'}</span>}
                    </span>
                    <span className="preset-gpu mono">{p.gpuLabel}</span>
                    <span className="preset-sum">{locked ?? p.summary}</span>
                  </button>
                );
              })}
            </div>
            <p className="block-text">{shownTier ? shownTier.expectation : '自定义：下面的设置和任何档位都不一样。'}</p>
          </section>
        )}

        {plan && !blocked && (
          <CoreSettings key={tierNonce} values={pickCore(plan.values)} tier={PRESETS.find((p) => p.id === preset) ?? PRESETS[1]} live={record?.route === 'prism-core' ? record : null} onChange={setCoreValues} onTuned={onTuned} />
        )}

        {plan && (plan.profileNotes.length > 0 || plan.warnings.length > 0) && (
          <section className="block">
            <h2 className="block-title">注意</h2>
            <ul className="notes">
              {plan.profileNotes.map((n) => (
                <li key={n}><i style={{ background: TONE_COLOR.info }} />{n}</li>
              ))}
              {plan.warnings.map((n) => (
                <li key={n}><i style={{ background: TONE_COLOR.caution }} />{n}</li>
              ))}
            </ul>
          </section>
        )}

        {plan && blocked && (
          <section className="block">
            <h2 className="block-title"><i className="tone-bar" style={{ background: TONE_COLOR.blocked }} />暂时无法启用</h2>
            <ul className="notes">
              {plan.blockers.map((b) => (
                <li key={b}><i style={{ background: TONE_COLOR.blocked }} />{b}</li>
              ))}
            </ul>
            {plan.needs.filter((n) => n.status === 'manual').map((n) => (
              <button key={n.id} className="btn btn-line btn-block" onClick={() => void importNeed(n.id)}>
                导入 {n.name}
              </button>
            ))}
          </section>
        )}
      </div>

      <footer className="detail-foot">
        {progress && progress.stage !== 'done' && progress.stage !== 'error' && (
          <div className="progress" role="status" aria-live="polite">
            <span>{progress.message}</span>
            <RollingNumber className="mono" value={`${Math.round(progress.fraction * 100)}%`} />
          </div>
        )}
        {error && <p className="foot-error" role="alert">{error}</p>}
        {leftover && !record && (
          <p className="foot-error" role="status">已还原。请到 Steam 里把这个游戏的启动选项清空（原来填的是 {leftover}），否则 Steam 会找不到启动器、游戏打不开。</p>
        )}
        {(justDone || record) && !busy && (
          <div className="ready">
            <p className="ready-title">
              <i style={{ background: TONE_COLOR.ready }} />
              已就绪{record ? `：${recordName}` : ''}
            </p>
            {(plan?.lateLoad?.launchOption ?? record?.launchOption) && <LaunchOption value={(plan?.lateLoad?.launchOption ?? record?.launchOption)!} />}
            {plan?.manualSteps.length ? (
              <ol className="ready-steps">
                {plan.manualSteps.map((s) => (
                  <li key={s}>{s}</li>
                ))}
              </ol>
            ) : (
              <p className="block-text">直接启动游戏即可。</p>
            )}
          </div>
        )}
        <div className="foot-actions">
          <button className="btn btn-primary" disabled={!plan || blocked || needsConsent || busy} onClick={() => void install()}>
            {busy ? '正在启用' : blocked ? '无法启用' : needsConsent ? '先确认风险' : record ? '重新应用' : '一键启用'}
          </button>
          {record && (
            <button className="btn btn-line" onClick={() => void rollback()} disabled={busy}>
              还原
            </button>
          )}
        </div>
      </footer>
    </aside>
  );
}

// ---------------------------------------------------------------------------

/** The Steam launch option of a late-loading (Denuvo) install, ready to copy. */
function LaunchOption({ value }: { value: string }) {
  const [copied, setCopied] = useState(false);
  const copy = () => {
    void navigator.clipboard.writeText(value).then(() => {
      setCopied(true);
      setTimeout(() => setCopied(false), 1600);
    });
  };
  return (
    <div className="launch-option">
      <span className="launch-option-label">Steam 启动选项</span>
      <code className="launch-option-value">{value}</code>
      <button className="btn btn-line btn-small" onClick={copy}>{copied ? '已复制' : '复制'}</button>
    </div>
  );
}

// ---------------------------------------------------------------------------

const CORE_KEYS = ['prism-core.cadence', 'prism-core.passes', 'prism-core.netSize', 'prism-core.async', 'prism-core.intensity', 'prism-core.guard', 'prism-core.look'];
/** The look is its own choice: tiers neither set nor reset it. */
const TIER_KEYS = CORE_KEYS.filter((k) => k !== 'prism-core.look');

const LOOKS: { value: string; label: string; note: string }[] = [
  { value: '0', label: '默认', note: '网络原样的色彩。' },
  { value: '1', label: '高还原', note: '保留原画的颜色，只取网络的光影和质感；强改动被柔和压住，皮肤上更克制。AI 味最低，适合原画党。' },
  { value: '2', label: '高饱和', note: '颜色更鲜更干净：越灰的颜色加得越多，灰和白保持中性，皮肤只跟一小部分。适合二次元和卡通风格。' },
  { value: '3', label: '低饱和', note: '收住颜色，白色微冷，暗部微青，像美剧和电影的质感，皮肤保留自然血色。适合科技和现代题材。' },
  { value: '4', label: '浓烈', note: '深对比加浓郁色彩，暗部偏青、高光微暖。真正的重度渲染。' },
];

function pickCore(values: CoreValues): CoreValues {
  return Object.fromEntries(Object.entries(values).filter(([k]) => CORE_KEYS.includes(k)));
}

/** What each cadence costs and keeps, from the CONTROL measurements and the offline comparison. */
const CADENCE_NOTE: Record<number, string> = {
  1: '网络每帧跑一次，就是网络的完整答案，没有携带误差，开销最高（RTX 5090 Laptop、2560×1600、310.9 模型约 5 ms）。',
  2: '运动中最清楚的异步档：离线实测，和每 4 帧相比运动区域误差低约三成、闪烁低约四成，网络开销约为每帧的一半。',
  3: '介于每 2 帧和每 4 帧之间。',
  4: '最省。快速运动时改动最旧，鬼影最明显，适合静态画面多的游戏。',
};

const PASSES_NOTE: Record<number, string> = {
  1: '网络跑一遍，改动最克制，开销最低。',
  2: '推荐。第二遍在第一遍的结果上再跑一次，光影和质感的变化更明显，网络开销是 1 遍的两倍。',
  3: '游乐场：网络开销是 1 遍的三倍，画面容易过度。只建议尝鲜，不建议实际游玩。',
  4: '游乐场：网络开销是 1 遍的四倍，画面通常已经过度。只建议尝鲜，不建议实际游玩。',
  5: '游乐场：网络开销是 1 遍的五倍，纯粹看看网络能走多远。不建议实际游玩。',
};

interface CoreSettingsProps {
  values: CoreValues;
  /** The tier "恢复档位" goes back to. */
  tier: PresetDef;
  /** The active Core install, when there is one: changes are written to it right away. */
  live: InstallRecord | null;
  onChange: (v: CoreValues) => void;
  onTuned: () => void;
}

function CoreSettings({ values, tier, live, onChange, onTuned }: CoreSettingsProps) {
  const [status, setStatus] = useState<'idle' | 'writing' | 'written' | 'error'>('idle');
  const [error, setError] = useState<string | null>(null);
  const pending = useRef<ReturnType<typeof setTimeout> | null>(null);
  const cadence = Number(values['prism-core.cadence'] ?? 2);
  const passes = Number(values['prism-core.passes'] ?? 2);
  const netSize = String(values['prism-core.netSize'] ?? '100');
  const async = values['prism-core.async'] !== false;
  const defaults = resolveValues(tier.id);
  const look = String(values['prism-core.look'] ?? '0');
  const changed = TIER_KEYS.some((k) => String(values[k]) !== String(defaults[k]));

  useEffect(() => () => {
    if (pending.current) clearTimeout(pending.current);
  }, []);

  const set = (patch: CoreValues) => {
    const next = pickCore({ ...values, ...patch });
    onChange(next);
    if (!live) return;
    // Sliders fire on every step: write once the hand stops.
    if (pending.current) clearTimeout(pending.current);
    setStatus('writing');
    pending.current = setTimeout(() => {
      api
        .updateCoreConfig(live.id, next)
        .then(() => {
          setStatus('written');
          setError(null);
          onTuned();
        })
        .catch((e) => {
          setStatus('error');
          setError((e as Error).message);
        });
    }, 350);
  };

  return (
    <section className="block">
      <h2 className="block-title">微调</h2>

      <div className="core-field">
        <span className="core-field-label" id="core-look-label">色彩风格</span>
        <div className="segmented" role="radiogroup" aria-labelledby="core-look-label">
          {LOOKS.map((l) => (
            <button key={l.value} role="radio" aria-checked={look === l.value} className={look === l.value ? 'is-on' : ''} onClick={() => set({ 'prism-core.look': l.value })}>
              {l.label}
            </button>
          ))}
        </div>
        <p className="block-note">{(LOOKS.find((l) => l.value === look) ?? LOOKS[0]).note}</p>
      </div>

      <div className="core-field">
        <span className="core-field-label" id="core-cadence-label">网络节奏</span>
        <div className="segmented segmented-4" role="radiogroup" aria-labelledby="core-cadence-label">
          {[1, 2, 3, 4].map((n) => (
            <button key={n} role="radio" aria-checked={cadence === n} className={cadence === n ? 'is-on' : ''} onClick={() => set({ 'prism-core.cadence': String(n) })}>
              {n === 1 ? '每帧' : `每 ${n} 帧`}
            </button>
          ))}
        </div>
        <p className="block-note">{CADENCE_NOTE[cadence]}</p>
      </div>

      <div className="core-field">
        <span className="core-field-label" id="core-passes-label">网络遍数</span>
        <div className="segmented" role="radiogroup" aria-labelledby="core-passes-label">
          {[1, 2, 3, 4, 5].map((n) => (
            <button key={n} role="radio" aria-checked={passes === n} className={`${passes === n ? 'is-on' : ''} ${n >= 3 ? 'is-playground' : ''}`} onClick={() => set({ 'prism-core.passes': String(n) })}>
              {n} 遍
            </button>
          ))}
        </div>
        <p className={`block-note ${passes >= 3 ? 'tone-caution-text' : ''}`}>{PASSES_NOTE[passes]}</p>
      </div>

      <div className="core-field">
        <span className="core-field-label" id="core-scale-label">网络分辨率</span>
        <div className="segmented segmented-2" role="radiogroup" aria-labelledby="core-scale-label">
          {NET_SIZES.map((n) => (
            <button key={n.value} role="radio" aria-checked={netSize === n.value} className={netSize === n.value ? 'is-on' : ''} onClick={() => set({ 'prism-core.netSize': n.value })}>
              {n.label}
            </button>
          ))}
        </div>
        <p className="block-note">
          {cadence !== 1
            ? '只在每帧跑网络时生效；隔帧时网络始终用原生分辨率。'
            : netSize === '150'
              ? '比旗舰基准多算一半像素，细节更足，开销更高。改动按原生画面的边缘放大回来，没有拖影。'
              : '旗舰的基准分辨率：改动按原生画面的边缘放大回来，帧率和细节最均衡，没有拖影。'}
        </p>
      </div>

      <label className={`core-toggle ${cadence === 1 ? 'is-disabled' : ''}`}>
        <input type="checkbox" checked={async && cadence > 1} disabled={cadence === 1} onChange={() => set({ 'prism-core.async': !async })} />
        <span>异步计算队列</span>
        <span className="dim">{cadence === 1 ? '每帧模式下不生效' : '网络和下一帧并行跑'}</span>
      </label>

      <div className="core-field">
        <TickRange label="改动强度" value={Number(values['prism-core.intensity'] ?? 200)} min={0} max={300} step={5} unit="%" help="100% 是网络的完整答案；调低更接近原画，调高会把网络的改动再放大，最高 300%。档位默认都是 100%（网络原样的答案），250% 以上容易过度。" onChange={(v) => set({ 'prism-core.intensity': v })} />
        <TickRange label="角色守护" value={Number(values['prism-core.guard'] ?? 100)} min={0} max={100} step={5} unit="%" help="压住肤色和高饱和颜色上的色度改动。0% 等于不干预。" onChange={(v) => set({ 'prism-core.guard': v })} />
      </div>

      <div className="core-foot">
        <p className={`block-note ${status === 'error' ? 'tone-blocked' : ''}`} role="status" aria-live="polite">
          {!live
            ? '启用时写入游戏目录的 prism-core.ini。启用以后再调，游戏运行中约 1 秒内生效。'
            : status === 'writing'
              ? '正在写入 prism-core.ini'
              : status === 'written'
                ? '已写入 prism-core.ini，游戏运行中约 1 秒内生效。'
                : status === 'error'
                  ? `写入失败：${error}`
                  : '这些设置已经写在游戏目录里。改动会直接写入，游戏运行中约 1 秒内生效。'}
        </p>
        {changed && (
          <button className="btn btn-line btn-small" onClick={() => set({ ...pickCore(defaults), 'prism-core.look': look })}>
            恢复{tier.name}档
          </button>
        )}
      </div>
    </section>
  );
}

// ---------------------------------------------------------------------------

interface RemoveManualProps {
  game: GameEntry;
  installed: boolean;
  onRemove: (id: string) => Promise<void>;
  onError: (message: string) => void;
}

/** Takes a manually added folder off the list; the first press arms it for three seconds. */
function RemoveManual({ game, installed, onRemove, onError }: RemoveManualProps) {
  const [armed, setArmed] = useState(false);
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    if (!armed) return;
    const t = setTimeout(() => setArmed(false), 3000);
    return () => clearTimeout(t);
  }, [armed]);
  const press = async () => {
    if (!armed) return setArmed(true);
    setBusy(true);
    try {
      await onRemove(game.id);
    } catch (e) {
      onError((e as Error).message);
      setArmed(false);
    } finally {
      setBusy(false);
    }
  };
  return (
    <div className="manual-remove">
      <button className={`btn btn-line btn-small ${armed ? 'is-armed' : ''}`} disabled={installed || busy} onClick={() => void press()}>
        {armed ? '确认移除' : '从列表移除'}
      </button>
      <p className="block-note">{installed ? '这个目录里还有 Prism 的安装，先点下方的"还原"，再移除。' : armed ? '再点一次确认。只从 Prism 的列表里拿掉，不会删除任何文件。' : '手动添加的目录。移除只是从列表里拿掉，不会删除任何文件。'}</p>
    </div>
  );
}
