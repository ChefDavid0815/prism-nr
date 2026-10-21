import { useEffect, useMemo, useRef, useState } from 'react';
import { pluginById, PRESETS, resolveValues } from '../../electron/shared/catalog';
import { STYLES } from '../../electron/shared/profiles';
import type { GameAnalysis, GameEntry, InstallPlan, InstallProgress, InstallRecord, PresetId, Route, SystemInfo } from '../../electron/shared/types';
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
  onAddManual: () => void;
}

function groupOf(a: GameAnalysis | undefined): 0 | 1 | 2 | 3 {
  if (!a) return 3;
  if (a.recommendedRoute === 'blocked') return 2;
  if (a.recommendedRoute === 'unknown') return 3;
  return a.recommendedRoute === 'feeder' ? 1 : 0;
}

const GROUPS = ['可以直接启用', '需要补全输入', '已阻止', '分析中或未识别'];

export function LibraryView({ games, analyses, scanning, system, installs, onApplied, onRolledBack, onTuned, onAddManual }: Props) {
  const [selected, setSelected] = useState<string | null>(null);
  const [hovered, setHovered] = useState<string | null>(null);
  const [progress, setProgress] = useState<InstallProgress | null>(null);
  const [plan, setPlan] = useState<InstallPlan | null>(null);

  useEffect(() => {
    if (!selected && games.length) {
      const firstReady = games.find((g) => groupOf(analyses[g.id]) === 0) ?? games[0];
      setSelected(firstReady.id);
    }
  }, [games, analyses, selected]);

  const groups = useMemo(() => {
    const out: GameEntry[][] = [[], [], [], []];
    for (const g of games) out[groupOf(analyses[g.id])].push(g);
    return out;
  }, [games, analyses]);

  const game = games.find((g) => g.id === selected) ?? null;
  const analysis = selected ? analyses[selected] : undefined;
  const bars = useMemo(() => (game ? fingerprint(game, analysis) : []), [game, analysis]);
  const isCore = plan?.route === 'prism-core';
  const intensity = plan ? Number(plan.values[isCore ? 'prism-core.intensity' : 'nr-renodx.intensity'] ?? 30) / 100 : 0.3;
  const modelRes = plan ? Number(plan.values['nr-optiscaler.modelResolution'] ?? 100) : 100;

  const fieldLabels = {
    left: [
      `API   ${analysis?.apis.map((x) => x.toUpperCase()).join(' ') || '—'}`,
      `BIT   ${analysis?.bitness ?? '—'}`,
      `DLSS  ${analysis?.nativeDlss.version ?? (analysis ? '无' : '—')}`,
      `ROUTE ${analysis ? ROUTE_LABEL[plan?.route ?? analysis.recommendedRoute] : '—'}`,
    ],
    right: [
      `NR    ${Math.round(intensity * 100)}%`,
      isCore ? `NET   1/${plan?.values['prism-core.cadence'] ?? 4}` : `MODEL ${plan?.route === 'optiscaler' ? modelRes + '%' : '100%'}`,
      `PLUG  ${plan?.plugins.length ?? 0}`,
      `SEED  ${game ? game.id.slice(0, 6).toUpperCase() : '—'}`,
    ],
  };

  return (
    <div className="library">
      <section className="library-main">
        <DataField bars={bars} intensity={intensity} busy={scanning || (progress != null && progress.stage !== 'done' && progress.stage !== 'error')} progress={progress && progress.stage !== 'done' && progress.stage !== 'error' ? progress.fraction : null} seed={game ? parseInt(game.id.slice(0, 8), 16) || 7 : 7} labels={fieldLabels} />

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
                  const tone = routeTone(a?.recommendedRoute);
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
                        {a ? ROUTE_LABEL[a.recommendedRoute] : '分析中'}
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
    const cadence = Number(plan.values['prism-core.cadence'] ?? 4);
    const async = plan.values['prism-core.async'] !== false && cadence >= 2;
    // From the CONTROL measurements: every frame 13.4 ms, every 2nd 61% of that, every 4th async 37%.
    const weight = ({ 1: 14, 2: 9, 3: 7, 4: 5 } as Record<number, number>)[cadence] + (async || cadence === 1 ? 0 : 1);
    stages.push({ label: 'DLSS', weight: 2 }, { label: cadence === 1 ? '神经渲染 每帧' : `神经渲染 每 ${cadence} 帧`, weight });
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
}

type CoreValues = Record<string, number | boolean | string>;

function Detail({ game, analysis, system, record, progress, onProgress, onPlan, onApplied, onRolledBack, onTuned }: DetailProps) {
  const [preset, setPreset] = useState<PresetId | undefined>(undefined);
  const [style, setStyle] = useState<string | undefined>(undefined);
  const [disabled, setDisabled] = useState<Set<string>>(new Set());
  const [plan, setPlan] = useState<InstallPlan | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [justDone, setJustDone] = useState(false);
  const [nonce, setNonce] = useState(0);
  const basePlugins = useRef<string[] | null>(null);
  // Prism Core settings: start from what is installed, so the panel shows the game's live values.
  const [coreValues, setCoreValues] = useState<CoreValues>(() => (record?.route === 'prism-core' ? pickCore(record.values ?? {}) : {}));

  useEffect(() => {
    if (!analysis) return;
    let alive = true;
    const plugins = basePlugins.current ? basePlugins.current.filter((p) => !disabled.has(p)) : undefined;
    api
      .planInstall({ gameId: game.id, preset, style, plugins, overrides: coreValues })
      .then((p) => {
        if (!alive) return;
        setPlan(p);
        onPlan(p);
        setPreset((cur) => cur ?? p.preset);
        setStyle((cur) => cur ?? p.style);
      })
      .catch((e) => alive && setError((e as Error).message));
    return () => {
      alive = false;
    };
  }, [game.id, analysis, preset, style, disabled, onPlan, nonce, coreValues]);

  // The preset defines the plugin baseline; toggles subtract from it.
  useEffect(() => {
    basePlugins.current = null;
    setDisabled(new Set());
  }, [preset]);
  useEffect(() => {
    if (plan && !basePlugins.current) basePlugins.current = PRESETS.find((p) => p.id === plan.preset)?.plugins ?? plan.plugins;
  }, [plan]);

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

  const coreAvailable = analysis?.routes.includes('prism-core') ?? false;
  const tier = system?.primary?.tier;
  const blocked = Boolean(plan?.blockers.length);
  const tone = routeTone(plan?.route ?? analysis?.recommendedRoute);
  const stages = plan ? pipeline(plan) : [];
  const exeName = analysis?.exe ? analysis.exe.split(/[\\/]/).pop() : '—';
  const optional = plan ? plan.plugins.filter((id) => ['prism', 'nvidia'].includes(pluginById(id)?.origin ?? '') && id !== 'prism-capture') : [];
  const optionalAll = basePlugins.current?.filter((id) => ['prism', 'nvidia'].includes(pluginById(id)?.origin ?? '') && id !== 'prism-capture' && (pluginById(id)?.routes.includes(plan?.route ?? 'native-hook') ?? false)) ?? optional;

  return (
    <aside className="detail" aria-label={`${game.name} 详情`}>
      <div className="detail-scroll">
        <h1 className="detail-title">{game.name}</h1>
        <p className="detail-path mono dim">{game.installDir}</p>

        <dl className="facts">
          <div><dt>主程序</dt><dd className="mono">{exeName}</dd></div>
          <div><dt>API</dt><dd className="mono">{analysis?.apis.map((x) => x.toUpperCase()).join(' ') || '—'}</dd></div>
          <div><dt>位数</dt><dd className="mono">{analysis?.bitness ?? '—'}</dd></div>
          <div><dt>原生 DLSS</dt><dd className="mono">{analysis ? analysis.nativeDlss.version ?? (analysis.nativeDlss.present ? '有' : '无') : '—'}</dd></div>
          <div><dt>帧生成</dt><dd className="mono">{analysis ? (analysis.dlssFrameGen ? '支持' : '无') : '—'}</dd></div>
          <div><dt>反作弊</dt><dd className={analysis?.antiCheat.length ? 'tone-blocked' : 'mono'}>{analysis ? analysis.antiCheat.map((x) => x.name).join('、') || '无' : '—'}</dd></div>
        </dl>

        <section className="block">
          <h2 className="block-title">
            <i className="tone-bar" style={{ background: TONE_COLOR[tone] }} />
            路线：{plan ? ROUTE_LABEL[plan.route] : analysis ? ROUTE_LABEL[analysis.recommendedRoute] : '分析中'}
          </h2>
          <p className="block-text">{analysis?.routeReason ?? '正在读取游戏文件。'}</p>
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

        {plan && !blocked && plan.route === 'prism-core' && (
          <CoreSettings values={{ ...resolveValues('core'), ...coreValues }} live={record?.route === 'prism-core' ? record : null} onChange={setCoreValues} onTuned={onTuned} />
        )}

        {plan && !blocked && (
          <>
            {plan.route !== 'prism-core' && (
            <section className="block">
              <h2 className="block-title">画风</h2>
              <div className="segmented" role="radiogroup" aria-label="画风">
                {STYLES.map((s) => (
                  <button key={s.id} role="radio" aria-checked={style === s.id} className={style === s.id ? 'is-on' : ''} onClick={() => setStyle(s.id)} title={s.summary}>
                    {s.name}
                  </button>
                ))}
              </div>
              <p className="block-text">{STYLES.find((s) => s.id === style)?.summary}</p>
            </section>
            )}

            <section className="block">
              <h2 className="block-title">方案</h2>
              <div className="presets" role="radiogroup" aria-label="方案">
                {PRESETS.map((p) => {
                  const unavailable = p.id === 'core' && !coreAvailable;
                  const fits = !unavailable && tier ? p.forTiers.includes(tier) : false;
                  return (
                    <button key={p.id} role="radio" aria-checked={preset === p.id} disabled={unavailable} className={`preset ${preset === p.id ? 'is-on' : ''}`} onClick={() => setPreset(p.id)}>
                      <span className="preset-name">
                        {p.name}
                        {fits && <span className="preset-fit">适合你的显卡</span>}
                      </span>
                      <span className="preset-gpu mono">{p.gpuLabel}</span>
                      <span className="preset-sum">{unavailable ? '这个游戏用不了：Prism Core 需要 DX12、64 位并且自带 DLSS。' : p.summary}</span>
                    </button>
                  );
                })}
              </div>
              <p className="block-text">{PRESETS.find((p) => p.id === preset)?.expectation}</p>
            </section>

            <section className="block">
              <h2 className="block-title">包含的插件</h2>
              <ul className="plugin-list">
                {plan.plugins
                  .filter((id) => !optionalAll.includes(id))
                  .map((id) => {
                    const p = pluginById(id)!;
                    return (
                      <li key={id} className="plugin-row is-fixed">
                        <span className="mono">{p.code}</span>
                        <span>{p.name}</span>
                        <span className="dim">必需</span>
                      </li>
                    );
                  })}
                {optionalAll.map((id) => {
                  const p = pluginById(id)!;
                  const on = !disabled.has(id);
                  return (
                    <li key={id} className="plugin-row">
                      <label>
                        <input
                          type="checkbox"
                          checked={on}
                          onChange={() =>
                            setDisabled((d) => {
                              const n = new Set(d);
                              if (n.has(id)) n.delete(id);
                              else n.add(id);
                              return n;
                            })
                          }
                        />
                        <span className="mono">{p.code}</span>
                        <span>{p.name}</span>
                        <span className="dim">{p.cost.split('：')[0]}</span>
                      </label>
                    </li>
                  );
                })}
              </ul>
            </section>
          </>
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
        {(justDone || record) && !busy && (
          <div className="ready">
            <p className="ready-title">
              <i style={{ background: TONE_COLOR.ready }} />
              已就绪{record ? `：${PRESETS.find((p) => p.id === record.preset)?.name}` : ''}
            </p>
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
          <button className="btn btn-primary" disabled={!plan || blocked || busy} onClick={() => void install()}>
            {busy ? '正在启用' : record ? '重新应用' : blocked ? '无法启用' : '一键启用'}
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

const CORE_KEYS = ['prism-core.cadence', 'prism-core.async', 'prism-core.intensity', 'prism-core.guard'];

function pickCore(values: CoreValues): CoreValues {
  return Object.fromEntries(Object.entries(values).filter(([k]) => CORE_KEYS.includes(k)));
}

/** What each cadence costs and keeps, from the CONTROL measurements and the offline comparison. */
const CADENCE_NOTE: Record<number, string> = {
  1: '网络每帧跑一次，就是网络的完整答案，成本最高：CONTROL 实测每帧 +13.4 ms。',
  2: '和每帧的答案相比误差约 0.03 档，成本约为每帧的六成：CONTROL 实测 +7.4 ms。',
  3: '介于每 2 帧和每 4 帧之间，还没有单独实测。',
  4: '成本最低：CONTROL 实测每帧 +5.0 ms，误差 0.06–0.09 档。快速运动的人脸细节损失最大。',
};

interface CoreSettingsProps {
  values: CoreValues;
  /** The active Core install, when there is one: changes are written to it right away. */
  live: InstallRecord | null;
  onChange: (v: CoreValues) => void;
  onTuned: () => void;
}

function CoreSettings({ values, live, onChange, onTuned }: CoreSettingsProps) {
  const [status, setStatus] = useState<'idle' | 'writing' | 'written' | 'error'>('idle');
  const [error, setError] = useState<string | null>(null);
  const pending = useRef<ReturnType<typeof setTimeout> | null>(null);
  const cadence = Number(values['prism-core.cadence'] ?? 4);
  const async = values['prism-core.async'] !== false;
  const defaults = resolveValues('core');
  const changed = CORE_KEYS.some((k) => String(values[k]) !== String(defaults[k]));

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
      <h2 className="block-title">Prism Core 设置</h2>

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

      <label className={`core-toggle ${cadence === 1 ? 'is-disabled' : ''}`}>
        <input type="checkbox" checked={async && cadence > 1} disabled={cadence === 1} onChange={() => set({ 'prism-core.async': !async })} />
        <span>异步计算队列</span>
        <span className="dim">{cadence === 1 ? '每帧模式下不生效' : '网络和下一帧并行跑'}</span>
      </label>

      <div className="core-field">
        <TickRange label="改动强度" value={Number(values['prism-core.intensity'] ?? 100)} min={0} max={100} step={5} unit="%" help="保留网络改动的多少。100% 是网络的完整答案，调低画面更接近原画。" onChange={(v) => set({ 'prism-core.intensity': v })} />
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
          <button className="btn btn-line btn-small" onClick={() => set({ ...pickCore(defaults) })}>
            恢复默认
          </button>
        )}
      </div>
    </section>
  );
}
