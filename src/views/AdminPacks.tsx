import { useCallback, useEffect, useState } from 'react';
import { PACK_SOURCE, showRedeemCode, type AdminCode, type AdminPack, type AdminUserPack } from '../../electron/shared/account';
import { api } from '../api';
import { TONE_COLOR } from '../lib/fingerprint';

const day = (iso: string | null) => (iso ? new Date(iso).toLocaleDateString('zh-CN', { year: 'numeric', month: '2-digit', day: '2-digit' }) : '长期');

type CodeState = 'left' | 'used' | 'expired' | 'disabled';
const codeState = (c: AdminCode): CodeState =>
  c.disabled ? 'disabled' : c.expires_at && Date.parse(c.expires_at) < Date.now() ? 'expired' : c.uses >= c.max_uses ? 'used' : 'left';
const CODE_LABEL: Record<CodeState, string> = { left: '可用', used: '已用完', expired: '已过期', disabled: '已停用' };
const CODE_TONE: Record<CodeState, string> = { left: TONE_COLOR.ready, used: '#5c5c5c', expired: TONE_COLOR.caution, disabled: TONE_COLOR.blocked };

async function copy(text: string, set: (s: string) => void, done: string) {
  await navigator.clipboard.writeText(text);
  set(done);
}

/**
 * Plugin packs on the admin page: every pack with how many own it and how many of its codes are
 * left; a new pack; and per pack its name, description and whether it is on offer, making a batch
 * of codes (shown once in full to copy and hand out), and every code with its uses and a switch.
 */
export function AdminPacks() {
  const [packs, setPacks] = useState<AdminPack[]>([]);
  const [open, setOpen] = useState<string | null>(null);
  const [creating, setCreating] = useState(false);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(async () => {
    setLoading(true);
    try {
      setPacks((await api.account.admin({ action: 'packs' })).packs);
      setError(null);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void load();
  }, [load]);

  return (
    <section className="block block-wide">
      <div className="admin-bar">
        <h2 className="block-title admin-bar-title">插件包与兑换码</h2>
        <span className="dim mono">{loading ? '加载中' : `${packs.length} 个`}</span>
        <button className="btn btn-line btn-small" onClick={() => setCreating(!creating)} aria-expanded={creating}>{creating ? '取消新建' : '新建插件包'}</button>
      </div>
      {creating && <PackForm onDone={(id) => { setCreating(false); setOpen(id); void load(); }} />}
      {error && <p className="foot-error" role="alert">{error}</p>}
      <div className="admin-table" role="table" aria-label="插件包">
        <div className="admin-row packs-admin-row admin-head" role="row">
          <span role="columnheader">名称</span>
          <span role="columnheader">编号</span>
          <span role="columnheader">状态</span>
          <span role="columnheader">拥有</span>
          <span role="columnheader">可用码 / 全部</span>
        </div>
        {packs.map((p) => (
          <div key={p.id} className={`admin-item ${open === p.id ? 'is-open' : ''}`}>
            <button className="admin-row packs-admin-row" role="row" aria-expanded={open === p.id} onClick={() => setOpen(open === p.id ? null : p.id)}>
              <span role="cell" className="admin-name">{p.name}</span>
              <span role="cell" className="mono admin-cut">{p.id}</span>
              <span role="cell" className="admin-state"><i style={{ background: p.active ? TONE_COLOR.ready : '#5c5c5c' }} />{p.active ? '上架' : '下架'}</span>
              <span role="cell" className="mono">{p.owners}</span>
              <span role="cell" className="mono">{p.codes_left} / {p.codes}</span>
            </button>
            {open === p.id && <PackDetail pack={p} onChange={() => void load()} />}
          </div>
        ))}
        {!loading && packs.length === 0 && !error && <p className="block-note admin-empty">还没有插件包。先新建一个，再给它生成兑换码。</p>}
      </div>
    </section>
  );
}

function PackForm({ onDone }: { onDone: (id: string) => void }) {
  const [id, setId] = useState('');
  const [name, setName] = useState('');
  const [description, setDescription] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const create = async () => {
    setBusy(true);
    setError(null);
    try {
      await api.account.admin({ action: 'savePack', create: true, pack: id.trim(), name: name.trim(), description: description.trim() });
      onDone(id.trim());
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  return (
    <form className="admin-editor packs-new" onSubmit={(e) => { e.preventDefault(); void create(); }}>
      <div className="admin-grid">
        <label className="acct-field">
          <span className="acct-label">名字</span>
          <input autoFocus maxLength={40} value={name} onChange={(e) => setName(e.target.value)} placeholder="例如：电影感调色包" />
        </label>
        <label className="acct-field">
          <span className="acct-label">编号（建好后不能改）</span>
          <input className="mono" maxLength={32} value={id} onChange={(e) => setId(e.target.value.toLowerCase())} placeholder="例如：cinema" />
        </label>
        <label className="acct-field packs-wide">
          <span className="acct-label">说明</span>
          <input maxLength={500} value={description} onChange={(e) => setDescription(e.target.value)} placeholder="用户在插件页看到的介绍，可以之后再写" />
        </label>
        <div className="admin-actions">
          <button className="btn btn-line btn-small" type="submit" disabled={busy || !id.trim() || !name.trim()}>{busy ? '正在创建' : '创建插件包'}</button>
        </div>
      </div>
      <p className="block-note">编号只能用小写字母、数字和连字符。新建的插件包直接上架，用户在插件页就能看到。</p>
      {error && <p className="foot-error" role="alert">{error}</p>}
    </form>
  );
}

function PackDetail({ pack, onChange }: { pack: AdminPack; onChange: () => void }) {
  const [name, setName] = useState(pack.name);
  const [description, setDescription] = useState(pack.description);
  const [codes, setCodes] = useState<AdminCode[]>([]);
  const [made, setMade] = useState<string[] | null>(null);
  const [count, setCount] = useState('10');
  const [maxUses, setMaxUses] = useState('1');
  const [days, setDays] = useState('');
  const [note, setNote] = useState('');
  const [all, setAll] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [status, setStatus] = useState<string | null>(null);

  const loadCodes = useCallback(async () => {
    try {
      setCodes((await api.account.admin({ action: 'codes', pack: pack.id })).codes);
    } catch (e) {
      setError((e as Error).message);
    }
  }, [pack.id]);

  useEffect(() => {
    void loadCodes();
  }, [loadCodes]);

  const act = async (fn: () => Promise<unknown>, done: string | null) => {
    setBusy(true);
    setError(null);
    setStatus(null);
    try {
      await fn();
      if (done) setStatus(done);
      onChange();
      await loadCodes();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const save = (active?: boolean) => act(() => api.account.admin({ action: 'savePack', pack: pack.id, name: name.trim(), description: description.trim(), ...(active !== undefined ? { active } : {}) }), active === undefined ? '已保存' : active ? '已上架' : '已下架，已拥有的用户不受影响');
  const make = () => act(async () => {
    const r = await api.account.admin({ action: 'makeCodes', pack: pack.id, count: Number(count), maxUses: Number(maxUses), days: days.trim() ? Number(days) : null, note: note.trim() });
    setMade(r.codes);
  }, null);

  const left = codes.filter((c) => codeState(c) === 'left');
  const shown = all ? codes : codes.slice(0, 50);

  return (
    <div className="admin-editor packs-detail">
      <div className="admin-grid">
        <label className="acct-field">
          <span className="acct-label">名字</span>
          <input maxLength={40} value={name} onChange={(e) => setName(e.target.value)} />
        </label>
        <label className="acct-field">
          <span className="acct-label">编号</span>
          <input className="mono" value={pack.id} readOnly />
        </label>
        <label className="acct-field packs-wide">
          <span className="acct-label">说明</span>
          <input maxLength={500} value={description} onChange={(e) => setDescription(e.target.value)} placeholder="用户在插件页看到的介绍" />
        </label>
        <div className="admin-actions">
          <button className="btn btn-line btn-small" disabled={busy || !name.trim() || (name.trim() === pack.name && description.trim() === pack.description)} onClick={() => void save()}>保存</button>
          <button className="btn btn-line btn-small" disabled={busy} onClick={() => void save(!pack.active)}>{pack.active ? '下架' : '上架'}</button>
        </div>
      </div>

      <h3 className="packs-sub">生成兑换码</h3>
      <form className="packs-make" onSubmit={(e) => { e.preventDefault(); void make(); }}>
        <label className="acct-field">
          <span className="acct-label">数量</span>
          <input className="mono" inputMode="numeric" value={count} onChange={(e) => setCount(e.target.value.replace(/\D/g, ''))} />
        </label>
        <label className="acct-field">
          <span className="acct-label">每个码能用几次</span>
          <input className="mono" inputMode="numeric" value={maxUses} onChange={(e) => setMaxUses(e.target.value.replace(/\D/g, ''))} />
        </label>
        <label className="acct-field">
          <span className="acct-label">有效天数</span>
          <input className="mono" inputMode="numeric" value={days} onChange={(e) => setDays(e.target.value.replace(/\D/g, ''))} placeholder="留空为长期" />
        </label>
        <label className="acct-field packs-make-note">
          <span className="acct-label">备注</span>
          <input maxLength={100} value={note} onChange={(e) => setNote(e.target.value)} placeholder="例如：B站抽奖" />
        </label>
        <button className="btn btn-line btn-small" type="submit" disabled={busy || !Number(count) || !Number(maxUses)}>{busy ? '正在生成' : `生成 ${Number(count) || 0} 个`}</button>
      </form>
      <p className="block-note">一个码能用几次，就能让几个账号各领一份；同一个账号不会重复占用。一次最多 500 个。</p>

      {made && (
        <div className="packs-made" role="region" aria-label="刚生成的兑换码">
          <div className="acct-row">
            <strong>刚生成 {made.length} 个</strong>
            <button className="btn btn-line btn-small" onClick={() => void copy(made.map(showRedeemCode).join('\n'), setStatus, `已复制 ${made.length} 个兑换码`)}>全部复制</button>
            <button className="btn btn-line btn-small" onClick={() => setMade(null)}>收起</button>
          </div>
          <ol className="packs-codes mono">
            {made.map((c) => <li key={c}>{showRedeemCode(c)}</li>)}
          </ol>
        </div>
      )}

      <div className="acct-row packs-codes-bar">
        <h3 className="packs-sub">全部兑换码</h3>
        <span className="dim mono">{codes.length} 个，可用 {left.length} 个</span>
        <button className="btn btn-line btn-small" disabled={!left.length} onClick={() => void copy(left.map((c) => showRedeemCode(c.code)).join('\n'), setStatus, `已复制 ${left.length} 个可用的兑换码`)}>复制可用的码</button>
      </div>
      {status && <p className="block-note" role="status">{status}</p>}
      {error && <p className="foot-error" role="alert">{error}</p>}
      {codes.length > 0 && (
        <div className="codes-table" role="table" aria-label="兑换码">
          <div className="codes-row admin-head" role="row">
            <span role="columnheader">兑换码</span>
            <span role="columnheader">已用</span>
            <span role="columnheader">到期</span>
            <span role="columnheader">备注</span>
            <span role="columnheader">状态</span>
            <span role="columnheader" aria-hidden="true" />
          </div>
          {shown.map((c) => {
            const st = codeState(c);
            return (
              <div key={c.code} className="codes-row" role="row">
                <span role="cell" className="mono">{showRedeemCode(c.code)}</span>
                <span role="cell" className="mono">{c.uses} / {c.max_uses}</span>
                <span role="cell" className="mono">{day(c.expires_at)}</span>
                <span role="cell" className="codes-note">{c.note || '—'}</span>
                <span role="cell" className="admin-state"><i style={{ background: CODE_TONE[st] }} />{CODE_LABEL[st]}</span>
                <span role="cell">
                  <button className="btn btn-line btn-small codes-switch" disabled={busy} onClick={() => void act(() => api.account.admin({ action: 'setCode', code: c.code, disabled: !c.disabled }), c.disabled ? '已重新启用' : '已停用，这个码不能再兑换')}>
                    {c.disabled ? '启用' : '停用'}
                  </button>
                </span>
              </div>
            );
          })}
          {codes.length > shown.length && <button className="btn btn-line btn-small codes-more" onClick={() => setAll(true)}>显示全部 {codes.length} 个</button>}
        </div>
      )}
    </div>
  );
}

/** In an account's editor: the packs it owns, taking one back, and giving one. */
export function UserPacks({ userId }: { userId: string }) {
  const [owned, setOwned] = useState<AdminUserPack[]>([]);
  const [packs, setPacks] = useState<AdminPack[]>([]);
  const [pick, setPick] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(async () => {
    try {
      const [o, p] = await Promise.all([api.account.admin({ action: 'userPacks', id: userId }), api.account.admin({ action: 'packs' })]);
      setOwned(o.packs);
      setPacks(p.packs);
      setError(null);
    } catch (e) {
      setError((e as Error).message);
    }
  }, [userId]);

  useEffect(() => {
    void load();
  }, [load]);

  const act = async (fn: () => Promise<unknown>) => {
    setBusy(true);
    setError(null);
    try {
      await fn();
      setPick('');
      await load();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const nameOf = (id: string) => packs.find((p) => p.id === id)?.name ?? id;
  const free = packs.filter((p) => !owned.some((o) => o.pack_id === p.id));

  return (
    <div className="user-packs">
      <span className="acct-label">插件包</span>
      {owned.length === 0 && <p className="block-note">这个账号还没有插件包。</p>}
      {owned.length > 0 && (
        <ul className="user-packs-list">
          {owned.map((o) => (
            <li key={o.pack_id}>
              <span>{nameOf(o.pack_id)}</span>
              <span className="mono dim">{PACK_SOURCE[o.source]}{o.code ? ` ${showRedeemCode(o.code)}` : ''}，{day(o.created_at)}</span>
              <button className="btn btn-line btn-small" disabled={busy} onClick={() => void act(() => api.account.admin({ action: 'revoke', id: userId, pack: o.pack_id }))}>收回</button>
            </li>
          ))}
        </ul>
      )}
      {free.length > 0 && (
        <div className="acct-row">
          <select className="user-packs-pick" value={pick} onChange={(e) => setPick(e.target.value)} aria-label="要发放的插件包">
            <option value="">选择要发放的插件包</option>
            {free.map((p) => <option key={p.id} value={p.id}>{p.name}{p.active ? '' : '（已下架）'}</option>)}
          </select>
          <button className="btn btn-line btn-small" disabled={busy || !pick} onClick={() => void act(() => api.account.admin({ action: 'grant', id: userId, pack: pick }))}>直接发放</button>
        </div>
      )}
      {error && <p className="foot-error" role="alert">{error}</p>}
    </div>
  );
}
