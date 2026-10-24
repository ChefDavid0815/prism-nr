import { useCallback, useEffect, useState } from 'react';
import { showPhone, type AdminStats, type AdminUser } from '../../electron/shared/account';
import { api } from '../api';
import { TONE_COLOR } from '../lib/fingerprint';
import { AdminPacks, UserPacks } from './AdminPacks';

const when = (iso: string | null) => (iso ? new Date(iso).toLocaleString('zh-CN', { month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit' }) : '—');

/**
 * The administrator's page: how many accounts there are and how they are doing, every account with
 * a search, and per account a new username, phone or role, disabling, a new password, or deletion.
 * Plugin packs and their redeem codes below that. Everything goes through the admin edge function,
 * which checks the role server-side.
 */
export function AdminView({ selfId }: { selfId: string }) {
  const [stats, setStats] = useState<AdminStats | null>(null);
  const [users, setUsers] = useState<AdminUser[]>([]);
  const [total, setTotal] = useState(0);
  const [q, setQ] = useState('');
  const [open, setOpen] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);

  const load = useCallback(async (query = q) => {
    setLoading(true);
    setError(null);
    try {
      const [s, l] = await Promise.all([api.account.admin({ action: 'stats' }), api.account.admin({ action: 'list', q: query, size: 200 })]);
      setStats(s.stats);
      setUsers(l.users);
      setTotal(l.total);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setLoading(false);
    }
  }, [q]);

  useEffect(() => {
    void load('');
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    const t = setTimeout(() => void load(q), 300);
    return () => clearTimeout(t);
  }, [q]); // eslint-disable-line react-hooks/exhaustive-deps

  const figures: [string, number | undefined][] = [
    ['总账号', stats?.total],
    ['已验证邮箱', stats?.confirmed],
    ['今日新增', stats?.new_today],
    ['7 天新增', stats?.new_7d],
    ['7 天活跃', stats?.active_7d],
    ['管理员', stats?.admins],
    ['已停用', stats?.banned],
  ];

  return (
    <div className="page">
      <header className="page-head">
        <h1>管理</h1>
        <p>所有 Prism NR 账号。只有管理员能看到这一页，每个操作都在服务器上再核对一次身份。</p>
      </header>
      <section className="block block-wide">
        <dl className="readout admin-figures">
          {figures.map(([label, v]) => (
            <div key={label}>
              <dt>{label}</dt>
              <dd className="mono">{v ?? '—'}</dd>
            </div>
          ))}
        </dl>
      </section>
      <section className="block block-wide">
        <div className="admin-bar">
          <input className="admin-search" placeholder="搜索用户名、邮箱或手机号" value={q} onChange={(e) => setQ(e.target.value)} aria-label="搜索账号" />
          <span className="dim mono">{loading ? '加载中' : `${total} 个`}</span>
          <button className="btn btn-line btn-small" onClick={() => void load()} disabled={loading}>刷新</button>
        </div>
        {error && <p className="foot-error" role="alert">{error}</p>}
        <div className="admin-table" role="table" aria-label="账号">
          <div className="admin-row admin-head" role="row">
            <span role="columnheader">用户名</span>
            <span role="columnheader">邮箱</span>
            <span role="columnheader">手机号</span>
            <span role="columnheader">注册</span>
            <span role="columnheader">最近登录</span>
            <span role="columnheader">状态</span>
          </div>
          {users.map((u) => (
            <div key={u.id} className={`admin-item ${open === u.id ? 'is-open' : ''}`}>
              <button className="admin-row" role="row" aria-expanded={open === u.id} onClick={() => setOpen(open === u.id ? null : u.id)}>
                <span role="cell" className="admin-name">{u.username}{u.role === 'admin' && <em>管理员</em>}{u.id === selfId && <em>你</em>}</span>
                <span role="cell" className="admin-cut">{u.email}</span>
                <span role="cell" className="mono">{showPhone(u.phone) || '—'}</span>
                <span role="cell" className="mono">{when(u.created_at)}</span>
                <span role="cell" className="mono">{when(u.last_sign_in_at)}</span>
                <span role="cell" className="admin-state">
                  <i style={{ background: u.banned ? TONE_COLOR.blocked : u.confirmed ? TONE_COLOR.ready : TONE_COLOR.caution }} />
                  {u.banned ? '已停用' : u.confirmed ? '正常' : '未验证'}
                </span>
              </button>
              {open === u.id && <Editor user={u} self={u.id === selfId} onDone={() => void load()} />}
            </div>
          ))}
          {!loading && users.length === 0 && <p className="block-note admin-empty">{q ? '没有匹配的账号。' : '还没有账号。'}</p>}
        </div>
      </section>
      <AdminPacks />
    </div>
  );
}

function Editor({ user, self, onDone }: { user: AdminUser; self: boolean; onDone: () => void }) {
  const [username, setUsername] = useState(user.username);
  const [phone, setPhone] = useState(showPhone(user.phone));
  const [password, setPassword] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [note, setNote] = useState<string | null>(null);
  const [armed, setArmed] = useState(false);

  const act = async (fn: () => Promise<unknown>, done: string, reload = true) => {
    setBusy(true);
    setError(null);
    setNote(null);
    try {
      await fn();
      setNote(done);
      if (reload) onDone();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const save = () => act(() => api.account.admin({ action: 'update', id: user.id, ...(username.trim() !== user.username ? { username } : {}), ...(phone.trim() !== showPhone(user.phone) ? { phone: phone.trim() || null } : {}) }), '已保存');

  return (
    <div className="admin-editor">
      <div className="admin-grid">
        <label className="acct-field">
          <span className="acct-label">用户名</span>
          <input value={username} onChange={(e) => setUsername(e.target.value)} />
        </label>
        <label className="acct-field">
          <span className="acct-label">手机号</span>
          <input value={phone} onChange={(e) => setPhone(e.target.value)} placeholder="留空即解除绑定" />
        </label>
        <div className="admin-actions">
          <button className="btn btn-line btn-small" disabled={busy || (username.trim() === user.username && phone.trim() === showPhone(user.phone))} onClick={() => void save()}>保存资料</button>
          <button className="btn btn-line btn-small" disabled={busy || self} onClick={() => void act(() => api.account.admin({ action: 'update', id: user.id, role: user.role === 'admin' ? 'user' : 'admin' }), user.role === 'admin' ? '已取消管理员' : '已设为管理员')}>
            {user.role === 'admin' ? '取消管理员' : '设为管理员'}
          </button>
          <button className="btn btn-line btn-small" disabled={busy || self} onClick={() => void act(() => api.account.admin({ action: 'ban', id: user.id, banned: !user.banned }), user.banned ? '已恢复' : '已停用')}>
            {user.banned ? '恢复账号' : '停用账号'}
          </button>
        </div>
        <label className="acct-field">
          <span className="acct-label">新密码</span>
          <input type="password" autoComplete="new-password" value={password} onChange={(e) => setPassword(e.target.value)} placeholder="至少 8 位" />
        </label>
        <div className="admin-actions">
          <button className="btn btn-line btn-small" disabled={busy || password.length < 8} onClick={() => void act(async () => { await api.account.admin({ action: 'password', id: user.id, password }); setPassword(''); }, '已设置新密码', false)}>设置新密码</button>
          <button className={`btn btn-line btn-small ${armed ? 'admin-danger' : ''}`} disabled={busy || self}
            onClick={() => { if (!armed) { setArmed(true); setTimeout(() => setArmed(false), 3000); return; } void act(() => api.account.admin({ action: 'delete', id: user.id }), '已删除'); }}>
            {armed ? '再点一次删除' : '删除账号'}
          </button>
        </div>
      </div>
      <UserPacks userId={user.id} />
      {self && <p className="block-note">这是你自己的账号：不能停用、删除或取消管理员。</p>}
      {error && <p className="foot-error" role="alert">{error}</p>}
      {note && <p className="block-note" role="status">{note}</p>}
    </div>
  );
}
