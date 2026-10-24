import { useCallback, useEffect, useMemo, useState } from 'react';
import { isStaff, type AccountState, type CommunityGame, type CommunityQuery, type CommunityShare } from '../../electron/shared/account';
import { GRADE, gradeFormat, gradeText, gradeValueKey, GRADE_SLOTS, type GradeValues } from '../../electron/shared/grade';
import { decodeGradeCode, matchGame } from '../../electron/shared/gradecode';
import type { GameAnalysis, GameEntry, InstallRecord } from '../../electron/shared/types';
import { api } from '../api';
import { Avatar, Marks, OfficialMark } from '../components/Badges';
import { TONE_COLOR } from '../lib/fingerprint';

interface Props {
  account: AccountState | null;
  games: GameEntry[];
  analyses: Record<string, GameAnalysis>;
  installs: InstallRecord[];
  onImported: () => void;
  onSignIn: () => void;
}

const when = (iso: string) => new Date(iso).toLocaleDateString('zh-CN', { year: 'numeric', month: '2-digit', day: '2-digit' });

/**
 * The community: custom looks people shared as grade codes. Each one names its game, which is found
 * in this library by itself (Steam id, executable, name); one press writes the look into that game's
 * custom look. The community opens on its games (searched by Chinese or English name or nickname);
 * a game's page has its looks, Prism NR's own first, marked 官方. Signed-in accounts only.
 */
export function CommunityView(props: Props) {
  if (!props.account?.signedIn) return <CommunityGate onSignIn={props.onSignIn} />;
  return <Community {...props} />;
}

function CommunityGate({ onSignIn }: { onSignIn: () => void }) {
  return (
    <div className="page">
      <header className="page-head">
        <h1>社区</h1>
        <p>大家分享的自定义调色，还有 Prism NR 为热门大作调的官方调色。</p>
      </header>
      <section className="block block-wide community-gate">
        <h2 className="block-title">登录后进入社区</h2>
        <p className="block-text">社区里的调色都会自动对上你游戏库里的游戏，一键导入。登录或注册一个账号就能浏览、导入和分享，注册只需要一个邮箱。</p>
        <button className="btn btn-primary community-gate-go" onClick={onSignIn}>登录或注册</button>
      </section>
    </div>
  );
}

type Place = { kind: 'home' } | { kind: 'game'; game: CommunityGame } | { kind: 'mine' };

function Community({ account, games, analyses, installs, onImported }: Props) {
  const [place, setPlace] = useState<Place>({ kind: 'home' });
  const isAdmin = Boolean(account?.signedIn && isStaff(account.profile?.role));
  const lists = { games, analyses, installs, isAdmin, onImported };

  if (place.kind === 'game') return <GamePage game={place.game} onBack={() => setPlace({ kind: 'home' })} {...lists} />;
  return <Home tab={place.kind} onTab={(t) => setPlace({ kind: t })} onGame={(g) => setPlace({ kind: 'game', game: g })} {...lists} />;
}

interface ListProps {
  games: GameEntry[];
  analyses: Record<string, GameAnalysis>;
  installs: InstallRecord[];
  isAdmin: boolean;
  onImported: () => void;
}

/** A community game as the library knows it: which library game it is, and whether Prism is on there. */
function inLibrary(g: CommunityGame, games: GameEntry[], analyses: Record<string, GameAnalysis>, installs: InstallRecord[]) {
  const target = matchGame({ name: g.name_en, exe: g.game_exe ?? undefined, steamAppId: g.steam_app_id ?? undefined }, games, analyses);
  const enabled = Boolean(target && installs.some((r) => r.gameId === target.id && r.status === 'active' && r.route === 'prism-core'));
  return { target, enabled };
}

function Home({ tab, onTab, onGame, games, analyses, installs, isAdmin, onImported }: ListProps & { tab: 'home' | 'mine'; onTab: (t: 'home' | 'mine') => void; onGame: (g: CommunityGame) => void }) {
  const [q, setQ] = useState('');
  const [found, setFound] = useState<CommunityGame[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [paste, setPaste] = useState('');

  useEffect(() => {
    if (tab !== 'home') return;
    setLoading(true);
    const t = setTimeout(() => {
      api.community.games(q)
        .then((r) => { setFound(r.games); setError(null); })
        .catch((e) => setError((e as Error).message))
        .finally(() => setLoading(false));
    }, q ? 250 : 0);
    return () => clearTimeout(t);
  }, [q, tab]);

  const pasted = useMemo(() => {
    if (!paste.trim()) return null;
    try {
      return { code: decodeGradeCode(paste), error: null as string | null };
    } catch (e) {
      return { code: null, error: (e as Error).message };
    }
  }, [paste]);

  // Games in this library first, then the rest, each in the server's order (official looks, imports).
  const rows = found.map((g) => ({ g, ...inLibrary(g, games, analyses, installs) }));
  const mine = rows.filter((x) => x.target);
  const rest = rows.filter((x) => !x.target);

  return (
    <div className="page">
      <header className="page-head">
        <h1>社区</h1>
        <p>按游戏找调色：搜中文名、英文名或者常用简称都可以。每个游戏里有官方调色和大家分享的调色，一键导入到你游戏库里的那个游戏。</p>
      </header>

      <section className="block block-wide">
        <div className="admin-bar community-bar">
          <div className="segmented segmented-2 community-tabs" role="tablist" aria-label="社区">
            <button role="tab" aria-selected={tab === 'home'} className={tab === 'home' ? 'is-on' : ''} onClick={() => onTab('home')}>游戏</button>
            <button role="tab" aria-selected={tab === 'mine'} className={tab === 'mine' ? 'is-on' : ''} onClick={() => onTab('mine')}>我分享的</button>
          </div>
          {tab === 'home' && (
            <input className="admin-search community-search" autoFocus placeholder="搜索游戏，比如 地平线6、赛博朋克、Witcher、rdr2" value={q} onChange={(e) => setQ(e.target.value)} aria-label="搜索游戏" />
          )}
          {tab === 'home' && <span className="dim mono">{loading ? '搜索中' : `${found.length} 个游戏`}</span>}
        </div>
        {error && <p className="foot-error" role="alert">{error}</p>}

        {tab === 'home' ? (
          <>
            {mine.length > 0 && <GameGroup title="你游戏库里的" rows={mine} onGame={onGame} />}
            {rest.length > 0 && <GameGroup title={mine.length ? '其他游戏' : '游戏'} rows={rest} onGame={onGame} />}
            {!loading && !error && found.length === 0 && (
              <p className="block-note admin-empty">{q ? `没有找到"${q.trim()}"。换个叫法试试：中文名、英文名或者简称；也可能这个游戏还没有人分享调色。` : '还没有任何调色。'}</p>
            )}
          </>
        ) : (
          <ShareList query={{ mine: true }} showGame empty={'你还没有分享过调色：在调色台里点"分享到社区"。'} games={games} analyses={analyses} installs={installs} isAdmin={isAdmin} onImported={onImported} />
        )}
      </section>

      <section className="block block-wide">
        <h2 className="block-title">导入调色码</h2>
        <p className="block-text">别人直接发给你的调色码，粘贴到这里。</p>
        <div className="community-paste">
          <input className="admin-search" placeholder="粘贴调色码（PRISM1. 开头）" value={paste} onChange={(e) => setPaste(e.target.value)} aria-label="调色码" />
        </div>
        {pasted?.error && <p className="foot-error" role="alert">{pasted.error}</p>}
        {pasted?.code && (
          <Importer grade={pasted.code.grade} gameName={pasted.code.game.name} target={matchGame(pasted.code.game, games, analyses)} installs={installs} onDone={() => { onImported(); }} />
        )}
      </section>
    </div>
  );
}

function GameGroup({ title, rows, onGame }: { title: string; rows: { g: CommunityGame; target: GameEntry | null; enabled: boolean }[]; onGame: (g: CommunityGame) => void }) {
  return (
    <div className="cgames">
      <h3 className="cgames-title">{title}<span className="mono">{String(rows.length).padStart(2, '0')}</span></h3>
      {rows.map(({ g, target, enabled }) => (
        <button key={g.key} className="cgame" onClick={() => onGame(g)}>
          <i className="community-mark" style={{ background: enabled ? TONE_COLOR.ready : target ? TONE_COLOR.caution : '#5c5c5c' }} aria-hidden="true" />
          <span className="cgame-name">
            <strong>{g.name_zh || g.name_en}</strong>
            {g.name_zh && <span className="dim">{g.name_en}</span>}
          </span>
          <span className="cgame-count">
            {g.official > 0 && <OfficialMark />}
            <span className="mono">{g.shares} 套调色</span>
          </span>
          <span className="cgame-imports mono dim">{g.imports} 次导入</span>
        </button>
      ))}
    </div>
  );
}

function GamePage({ game, onBack, games, analyses, installs, isAdmin, onImported }: ListProps & { game: CommunityGame; onBack: () => void }) {
  const { target, enabled } = inLibrary(game, games, analyses, installs);
  return (
    <div className="page">
      <header className="page-head cgame-head">
        <button className="btn btn-line btn-small cgame-back" onClick={onBack}>返回社区</button>
        <h1>{game.name_zh || game.name_en}</h1>
        <p>
          {game.name_zh && <span className="mono">{game.name_en}　</span>}
          {enabled ? `你游戏库里的 ${target!.name} 已启用 Prism Core，选好调色点"一键导入"即可。` : target ? `你游戏库里有这个游戏（${target.name}），先在游戏库里一键启用 Prism Core，再回来导入。` : '你的游戏库里没有这个游戏，可以先复制调色码，装好游戏后再导入。'}
        </p>
      </header>
      <section className="block block-wide">
        <ShareList query={{ gkey: game.key }} empty="这个游戏还没有调色。" games={games} analyses={analyses} installs={installs} isAdmin={isAdmin} onImported={onImported} />
      </section>
    </div>
  );
}

/** Looks, official first: each row opens to what it changes, the import, the code and the switches. */
function ShareList({ query, showGame, empty, games, analyses, installs, isAdmin, onImported }: ListProps & { query: CommunityQuery; showGame?: boolean; empty: string }) {
  const [shares, setShares] = useState<CommunityShare[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [open, setOpen] = useState<string | null>(null);
  const key = JSON.stringify(query);

  const load = useCallback(async () => {
    setLoading(true);
    try {
      setShares((await api.community.list(JSON.parse(key) as CommunityQuery)).shares);
      setError(null);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setLoading(false);
    }
  }, [key]);

  useEffect(() => {
    void load();
  }, [load]);

  return (
    <>
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
            <div key={s.id} className={`community-item ${open === s.id ? 'is-open' : ''} ${s.official ? 'is-official' : ''}`}>
              <button className="community-row" aria-expanded={open === s.id} onClick={() => setOpen(open === s.id ? null : s.id)}>
                <i className="community-mark" style={{ background: enabled ? TONE_COLOR.ready : target ? TONE_COLOR.caution : '#5c5c5c' }} aria-hidden="true" />
                <span className="community-title">
                  <strong>{s.official && <OfficialMark />}{s.title}</strong>
                  <span className="dim">{showGame ? s.game_name : s.description || ' '}</span>
                </span>
                {grade && <Fingerprint grade={grade} />}
                <span className="community-meta mono">
                  <span className="community-author">
                    <Avatar name={s.author} src={s.author_avatar} size={18} plan={s.author_plan} />
                    <span className="community-author-name">{s.author}</span>
                    <Marks founder={s.author_founder} contributor={s.author_contributor} plan={s.author_plan} />
                  </span>
                  <span className="community-when">{when(s.created_at)}  {s.imports} 次导入</span>
                </span>
              </button>
              {open === s.id && grade && (
                <div className="community-detail">
                  {s.description && <p className="block-text">{s.description}</p>}
                  <Changed grade={grade} />
                  <Importer grade={grade} gameName={s.game_name} target={target} installs={installs} shareId={s.id} onDone={() => { onImported(); void load(); }} />
                  <div className="acct-row">
                    <CopyButton text={s.code} />
                    {isAdmin && <OfficialSwitch id={s.id} official={s.official} onDone={() => void load()} />}
                    {(s.own || isAdmin) && <DeleteShare id={s.id} asAdmin={!s.own && isAdmin} onDone={() => void load()} />}
                  </div>
                </div>
              )}
            </div>
          );
        })}
        {!loading && !error && shares.length === 0 && <p className="block-note admin-empty">{empty}</p>}
      </div>
    </>
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


/** Staff only: mark a share as one of Prism NR's own looks, or take the mark off. */
function OfficialSwitch({ id, official, onDone }: { id: string; official: boolean; onDone: () => void }) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  return (
    <>
      <button className="btn btn-line btn-small" disabled={busy} onClick={() => {
        setBusy(true);
        api.account.admin({ action: 'setOfficial', id, official: !official }).then(onDone).catch((e) => setError((e as Error).message)).finally(() => setBusy(false));
      }}>
        {official ? '取消官方' : '设为官方'}
      </button>
      {error && <span className="foot-error">{error}</span>}
    </>
  );
}
