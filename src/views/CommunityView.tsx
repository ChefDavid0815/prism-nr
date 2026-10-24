import { useCallback, useEffect, useMemo, useState } from 'react';
import type { AccountState, CommunityShare } from '../../electron/shared/account';
import { GRADE, gradeFormat, gradeText, gradeValueKey, GRADE_SLOTS, type GradeValues } from '../../electron/shared/grade';
import { decodeGradeCode, matchGame } from '../../electron/shared/gradecode';
import type { GameAnalysis, GameEntry, InstallRecord } from '../../electron/shared/types';
import { api } from '../api';
import { TONE_COLOR } from '../lib/fingerprint';

type Filter = 'all' | 'mine-games' | 'shared';

interface Props {
  account: AccountState | null;
  games: GameEntry[];
  analyses: Record<string, GameAnalysis>;
  installs: InstallRecord[];
  onImported: () => void;
}

const when = (iso: string) => new Date(iso).toLocaleDateString('zh-CN', { year: 'numeric', month: '2-digit', day: '2-digit' });

/**
 * The community: custom looks people shared as grade codes. Each one names its game, which is found
 * in this library by itself (Steam id, executable, name); one press writes the look into that game's
 * custom look. Anyone may browse and import; sharing needs an account.
 */
export function CommunityView({ account, games, analyses, installs, onImported }: Props) {
  const [filter, setFilter] = useState<Filter>('all');
  const [q, setQ] = useState('');
  const [shares, setShares] = useState<CommunityShare[]>([]);
  const [total, setTotal] = useState(0);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [open, setOpen] = useState<string | null>(null);
  const [paste, setPaste] = useState('');
  const isAdmin = account?.signedIn && account.profile?.role === 'admin';

  const load = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      if (filter === 'mine-games') {
        // One query per library game with a Steam id or an executable, merged newest first.
        const keys = games.map((g) => ({ steam: g.steamAppId ?? null, exe: analyses[g.id]?.exe?.replace(/^.*[\\/]/, '') ?? null })).filter((k) => k.steam || k.exe);
        const lists = await Promise.all(keys.slice(0, 40).map((k) => api.community.list({ q, steam: k.steam, exe: k.exe })));
        const seen = new Set<string>();
        const all = lists.flatMap((l) => l.shares).filter((s) => (seen.has(s.id) ? false : (seen.add(s.id), true))).sort((a, b) => b.created_at.localeCompare(a.created_at));
        setShares(all);
        setTotal(all.length);
      } else {
        const r = await api.community.list({ q, mine: filter === 'shared' });
        setShares(r.shares);
        setTotal(r.total);
      }
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setLoading(false);
    }
  }, [filter, q, games, analyses]);

  useEffect(() => {
    const t = setTimeout(() => void load(), q ? 300 : 0);
    return () => clearTimeout(t);
  }, [load, q]);

  const pasted = useMemo(() => {
    if (!paste.trim()) return null;
    try {
      return { code: decodeGradeCode(paste), error: null as string | null };
    } catch (e) {
      return { code: null, error: (e as Error).message };
    }
  }, [paste]);

  return (
    <div className="page">
      <header className="page-head">
        <h1>社区</h1>
        <p>大家分享的自定义调色。每一条都标明属于哪个游戏，会自动对上你游戏库里的那个游戏，一键导入。</p>
      </header>

      <section className="block block-wide">
        <h2 className="block-title">导入调色码</h2>
        <div className="community-paste">
          <input className="admin-search" placeholder="粘贴调色码（PRISM1. 开头）" value={paste} onChange={(e) => setPaste(e.target.value)} aria-label="调色码" />
        </div>
        {pasted?.error && <p className="foot-error" role="alert">{pasted.error}</p>}
        {pasted?.code && (
          <Importer
            grade={pasted.code.grade}
            gameName={pasted.code.game.name}
            target={matchGame(pasted.code.game, games, analyses)}
            installs={installs}
            onDone={() => { onImported(); }}
          />
        )}
      </section>

      <section className="block block-wide">
        <div className="admin-bar">
          <div className="segmented segmented-3 community-filter" role="radiogroup" aria-label="范围">
            {([['all', '全部'], ['mine-games', '我的游戏'], ['shared', '我分享的']] as [Filter, string][]).map(([id, label]) => (
              <button key={id} role="radio" aria-checked={filter === id} className={filter === id ? 'is-on' : ''} disabled={id === 'shared' && !account?.signedIn} onClick={() => setFilter(id)}>{label}</button>
            ))}
          </div>
          <input className="admin-search" placeholder="搜索名字、游戏或作者" value={q} onChange={(e) => setQ(e.target.value)} aria-label="搜索分享" />
          <span className="dim mono">{loading ? '加载中' : `${total} 套`}</span>
        </div>
        {error && <p className="foot-error" role="alert">{error}</p>}
        <div className="community-list">
          {shares.map((s) => {
            let grade: GradeValues | null = null;
            try {
              grade = decodeGradeCode(s.code).grade;
            } catch {
              grade = null;
            }
            const target = matchGame({ name: s.game_name, exe: s.game_exe ?? undefined, steamAppId: s.steam_app_id ?? undefined }, games, analyses);
            const enabled = target && installs.some((r) => r.gameId === target.id && r.status === 'active' && r.route === 'prism-core');
            return (
              <div key={s.id} className={`community-item ${open === s.id ? 'is-open' : ''}`}>
                <button className="community-row" aria-expanded={open === s.id} onClick={() => setOpen(open === s.id ? null : s.id)}>
                  <i className="community-mark" style={{ background: enabled ? TONE_COLOR.ready : target ? TONE_COLOR.caution : '#5c5c5c' }} aria-hidden="true" />
                  <span className="community-title">
                    <strong>{s.title}</strong>
                    <span className="dim">{s.game_name}</span>
                  </span>
                  {grade && <Fingerprint grade={grade} />}
                  <span className="community-meta mono">
                    <span>{s.author}</span>
                    <span>{when(s.created_at)}</span>
                    <span>{s.imports} 次导入</span>
                  </span>
                </button>
                {open === s.id && grade && (
                  <div className="community-detail">
                    {s.description && <p className="block-text">{s.description}</p>}
                    <Changed grade={grade} />
                    <Importer grade={grade} gameName={s.game_name} target={target} installs={installs} shareId={s.id} onDone={() => { onImported(); void load(); }} />
                    <div className="acct-row">
                      <CopyButton text={s.code} />
                      {(s.own || isAdmin) && <DeleteShare id={s.id} asAdmin={!s.own && Boolean(isAdmin)} onDone={() => void load()} />}
                    </div>
                  </div>
                )}
              </div>
            );
          })}
          {!loading && shares.length === 0 && (
            <p className="block-note admin-empty">
              {filter === 'mine-games' ? '你游戏库里的游戏还没有人分享调色。' : filter === 'shared' ? '你还没有分享过调色：在调色台里点"分享到社区"。' : q ? '没有匹配的分享。' : '还没有人分享。在调色台里调好以后点"分享到社区"，做第一个。'}
            </p>
          )}
        </div>
      </section>
    </div>
  );
}

/** Every control's distance from neutral as a bar: what the look changes, at a glance. */
function Fingerprint({ grade }: { grade: GradeValues }) {
  return (
    <span className="community-print" aria-label="调色指纹：每根竖条是一项参数，越高改得越多">
      {GRADE.map((g, i) => {
        const span = g.unit === 'hue' ? 180 : Math.max(Math.abs(g.max - g.def), Math.abs(g.min - g.def));
        let d = g.unit === 'hue' ? Math.min(Math.abs(grade[i] - g.def), 360 - Math.abs(grade[i] - g.def)) : Math.abs(grade[i] - g.def);
        if (g.unit === 'hue' && grade[GRADE.findIndex((x) => x.key === g.key.replace('Hue', 'Amount'))] === 0) d = 0;  // a wheel's hue without amount changes nothing
        const h = Math.min(1, d / span);
        return <i key={g.key} style={{ height: `${Math.max(1, Math.round(h * 22))}px`, opacity: h > 0 ? 1 : 0.35 }} />;
      })}
    </span>
  );
}

function Changed({ grade }: { grade: GradeValues }) {
  const changed = GRADE.map((g, i) => ({ g, i })).filter(({ g, i }) => Math.abs(grade[i] - g.def) > 1e-4);
  if (!changed.length) return <p className="block-note">这套调色和中性一样。</p>;
  return (
    <ul className="community-changed">
      {changed.map(({ g, i }) => (
        <li key={g.key}><span>{g.label}</span><span className="mono">{gradeText(i, grade[i])}</span></li>
      ))}
    </ul>
  );
}

function Importer({ grade, gameName, target, installs, shareId, onDone }: { grade: GradeValues; gameName: string; target: GameEntry | null; installs: InstallRecord[]; shareId?: string; onDone: () => void }) {
  const record = target ? installs.find((r) => r.gameId === target.id && r.status === 'active' && r.route === 'prism-core') : undefined;
  const [slot, setSlot] = useState(0);
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const run = async () => {
    if (!record) return;
    setBusy(true);
    setError(null);
    setNote(null);
    try {
      await api.updateCoreConfig(record.id, { [gradeValueKey(slot)]: gradeFormat(grade), 'prism-core.gradeSlot': String(slot), 'prism-core.look': '5' });
      if (shareId) await api.community.imported(shareId);
      setNote(`已导入到 ${target!.name} 的方案 ${slot + 1}，色彩风格已切到自定义。游戏运行中约 1 秒生效。`);
      onDone();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  return (
    <div className="community-import">
      <p className="block-text">
        {target ? (record ? `属于 ${gameName}，对应你游戏库里的 ${target.name}。` : `属于 ${gameName}：你游戏库里有这个游戏，但还没启用 Prism Core。先在游戏库里一键启用，再回来导入。`) : `属于 ${gameName}：你的游戏库里没有找到这个游戏。可以复制调色码，装好游戏后再导入。`}
      </p>
      {record && (
        <div className="acct-row">
          <div className="segmented segmented-3 community-slots" role="radiogroup" aria-label="导入到哪个方案">
            {Array.from({ length: GRADE_SLOTS }, (_, i) => (
              <button key={i} role="radio" aria-checked={slot === i} className={slot === i ? 'is-on' : ''} onClick={() => setSlot(i)}>方案 {i + 1}</button>
            ))}
          </div>
          <button className="btn btn-line" disabled={busy} onClick={() => void run()}>{busy ? '正在导入' : '一键导入'}</button>
        </div>
      )}
      {note && <p className="block-note" role="status">{note}</p>}
      {error && <p className="foot-error" role="alert">{error}</p>}
    </div>
  );
}

function CopyButton({ text }: { text: string }) {
  const [copied, setCopied] = useState(false);
  return (
    <button className="btn btn-line btn-small" onClick={() => void navigator.clipboard.writeText(text).then(() => { setCopied(true); setTimeout(() => setCopied(false), 1600); })}>
      {copied ? '已复制' : '复制调色码'}
    </button>
  );
}

function DeleteShare({ id, asAdmin, onDone }: { id: string; asAdmin: boolean; onDone: () => void }) {
  const [armed, setArmed] = useState(false);
  const [error, setError] = useState<string | null>(null);
  return (
    <>
      <button className={`btn btn-line btn-small ${armed ? 'admin-danger' : ''}`} onClick={() => {
        if (!armed) { setArmed(true); setTimeout(() => setArmed(false), 3000); return; }
        api.community.remove(id, asAdmin).then(onDone).catch((e) => setError((e as Error).message));
      }}>
        {armed ? '再点一次删除' : asAdmin ? '删除（管理员）' : '删除我的分享'}
      </button>
      {error && <span className="foot-error">{error}</span>}
    </>
  );
}

