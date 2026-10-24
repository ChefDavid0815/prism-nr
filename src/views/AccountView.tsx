import { useEffect, useRef, useState } from 'react';
import { showPhone, USERNAME_HELP, type AccountPending, type AccountState } from '../../electron/shared/account';
import { api } from '../api';

type Mode = 'signin' | 'signup';

/**
 * The account page (1.5, experimental): sign in with an email, a phone number or a username; sign
 * up with an email (a code goes to it) and optionally a username and a phone; then the profile,
 * where the username and phone can change. Signing in is optional: Prism works the same without.
 */
export function AccountView({ account, onChange }: { account: AccountState | null; onChange: (s: AccountState) => void }) {
  if (!account) return <div className="page"><header className="page-head"><h1>账号</h1><p>正在连接账号服务器</p></header></div>;
  return account.signedIn && account.profile ? <Profile account={account} onChange={onChange} /> : <SignIn onChange={onChange} />;
}

function SignIn({ onChange }: { onChange: (s: AccountState) => void }) {
  const [mode, setMode] = useState<Mode>('signin');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [identifier, setIdentifier] = useState('');
  const [password, setPassword] = useState('');
  const [email, setEmail] = useState('');
  const [username, setUsername] = useState('');
  const [phone, setPhone] = useState('');
  const [pending, setPending] = useState<AccountPending | null>(null);

  const run = async (fn: () => Promise<void>) => {
    setBusy(true);
    setError(null);
    try {
      await fn();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const signIn = () => run(async () => {
    const r = await api.account.signIn(identifier, password);
    if ('needsCode' in r) setPending(r);
    else onChange(r);
  });
  const signUp = () => run(async () => {
    const r = await api.account.signUp({ email, password, username: username.trim() || undefined, phone: phone.trim() || undefined });
    setIdentifier(r.email ?? email);
    setPending(r);
  });

  if (pending) return <Verify pending={pending} identifier={mode === 'signup' ? email : identifier} password={password} onChange={onChange} onBack={() => setPending(null)} />;

  return (
    <div className="page">
      <header className="page-head">
        <h1>账号</h1>
        <p>登录是可选的：不登录也能完整使用 Prism。登录后有自己的档案。</p>
      </header>
      <section className="block block-wide acct">
        <div className="segmented segmented-2 acct-mode" role="tablist" aria-label="登录或注册">
          <button role="tab" aria-selected={mode === 'signin'} className={mode === 'signin' ? 'is-on' : ''} onClick={() => { setMode('signin'); setError(null); }}>登录</button>
          <button role="tab" aria-selected={mode === 'signup'} className={mode === 'signup' ? 'is-on' : ''} onClick={() => { setMode('signup'); setError(null); }}>注册</button>
        </div>
        <form className="acct-form" onSubmit={(e) => { e.preventDefault(); void (mode === 'signin' ? signIn() : signUp()); }}>
          {mode === 'signin' ? (
            <>
              <Field label="账号" hint="邮箱、手机号或用户名">
                <input autoFocus autoComplete="username" value={identifier} onChange={(e) => setIdentifier(e.target.value)} />
              </Field>
              <Field label="密码">
                <input type="password" autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} />
              </Field>
            </>
          ) : (
            <>
              <Field label="邮箱" hint="验证码会发到这里，证明是真实用户">
                <input autoFocus type="email" autoComplete="email" value={email} onChange={(e) => setEmail(e.target.value)} />
              </Field>
              <Field label="密码" hint="至少 8 位">
                <input type="password" autoComplete="new-password" value={password} onChange={(e) => setPassword(e.target.value)} />
              </Field>
              <Field label="用户名" hint={`选填，${USERNAME_HELP}；以后也能改`}>
                <input autoComplete="nickname" value={username} onChange={(e) => setUsername(e.target.value)} />
              </Field>
              <Field label="手机号" hint="选填，可以用来登录。国内直接填 11 位，其他国家带 + 和区号，例如 +1 415 555 0123">
                <input type="tel" autoComplete="tel" value={phone} onChange={(e) => setPhone(e.target.value)} />
              </Field>
            </>
          )}
          {error && <p className="foot-error" role="alert">{error}</p>}
          <button className="btn btn-primary acct-submit" type="submit" disabled={busy || (mode === 'signin' ? !identifier || !password : !email || !password)}>
            {busy ? '请稍候' : mode === 'signin' ? '登录' : '注册并发送验证码'}
          </button>
        </form>
      </section>
    </div>
  );
}

function Verify({ pending, identifier, password, onChange, onBack }: { pending: AccountPending; identifier: string; password: string; onChange: (s: AccountState) => void; onBack: () => void }) {
  const [email, setEmail] = useState(pending.email ?? '');
  const [code, setCode] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [note, setNote] = useState<string | null>(null);
  const [cooldown, setCooldown] = useState(60);
  const timer = useRef<ReturnType<typeof setInterval> | null>(null);

  useEffect(() => {
    timer.current = setInterval(() => setCooldown((c) => Math.max(0, c - 1)), 1000);
    return () => { if (timer.current) clearInterval(timer.current); };
  }, []);

  const run = async (fn: () => Promise<void>) => {
    setBusy(true);
    setError(null);
    setNote(null);
    try {
      await fn();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const verify = () => run(async () => onChange(await api.account.verify(email.trim().toLowerCase(), code)));
  const resend = () => run(async () => {
    await api.account.resend(email.trim().toLowerCase());
    setCooldown(60);
    setNote('已重新发送，请查收邮件（也看看垃圾邮件箱）。');
  });
  // The link in the email verifies too: then the password works.
  const linkUsed = () => run(async () => {
    const r = await api.account.signIn(identifier || email, password);
    if ('needsCode' in r) throw new Error('还没有验证成功：请点邮件里的链接，或者填写邮件里的验证码。');
    onChange(r);
  });

  return (
    <div className="page">
      <header className="page-head">
        <h1>验证邮箱</h1>
        <p>{pending.email ? `验证码已发到 ${pending.email}` : `这个账号的邮箱（${pending.hint ?? '已隐藏'}）还没验证，请填写完整邮箱。`}</p>
      </header>
      <section className="block block-wide acct">
        <form className="acct-form" onSubmit={(e) => { e.preventDefault(); void verify(); }}>
          {!pending.email && (
            <Field label="邮箱">
              <input type="email" autoComplete="email" value={email} onChange={(e) => setEmail(e.target.value)} />
            </Field>
          )}
          <Field label="验证码" hint="邮件里的一串数字">
            <input className="acct-code mono" autoFocus inputMode="numeric" maxLength={10} autoComplete="one-time-code" value={code} onChange={(e) => setCode(e.target.value.replace(/\D/g, ''))} />
          </Field>
          {error && <p className="foot-error" role="alert">{error}</p>}
          {note && <p className="block-note" role="status">{note}</p>}
          <button className="btn btn-primary acct-submit" type="submit" disabled={busy || code.length < 6 || !email}>{busy ? '请稍候' : '验证并登录'}</button>
          <div className="acct-row">
            <button type="button" className="btn btn-line btn-small" disabled={busy || cooldown > 0 || !email} onClick={() => void resend()}>
              {cooldown > 0 ? `${cooldown} 秒后可重新发送` : '重新发送验证码'}
            </button>
            {password && <button type="button" className="btn btn-line btn-small" disabled={busy} onClick={() => void linkUsed()}>我已点邮件里的链接</button>}
            <button type="button" className="btn btn-line btn-small" disabled={busy} onClick={onBack}>返回</button>
          </div>
        </form>
      </section>
    </div>
  );
}

function Profile({ account, onChange }: { account: AccountState; onChange: (s: AccountState) => void }) {
  const p = account.profile!;
  const [username, setUsername] = useState(p.username);
  const [phone, setPhone] = useState(showPhone(p.phone));
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [saved, setSaved] = useState(false);
  const dirty = username.trim() !== p.username || phone.trim() !== showPhone(p.phone);

  const save = async () => {
    setBusy(true);
    setError(null);
    setSaved(false);
    try {
      const patch: { username?: string; phone?: string | null } = {};
      if (username.trim() !== p.username) patch.username = username;
      if (phone.trim() !== showPhone(p.phone)) patch.phone = phone.trim() || null;
      const s = await api.account.updateProfile(patch);
      onChange(s);
      setUsername(s.profile?.username ?? username);
      setPhone(showPhone(s.profile?.phone));
      setSaved(true);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const signOut = async () => {
    await api.account.signOut();
    onChange({ signedIn: false });
  };

  return (
    <div className="page">
      <header className="page-head">
        <h1>{p.username}</h1>
        <p>{p.role === 'admin' ? '管理员 · ' : ''}{new Date(p.createdAt).toLocaleDateString('zh-CN')} 加入</p>
      </header>
      <section className="block block-wide acct">
        <h2 className="block-title">档案</h2>
        <form className="acct-form" onSubmit={(e) => { e.preventDefault(); void save(); }}>
          <Field label="用户名" hint={USERNAME_HELP}>
            <input value={username} onChange={(e) => { setUsername(e.target.value); setSaved(false); }} />
          </Field>
          <Field label="手机号" hint="可以用来登录；国内直接填 11 位，其他国家带 + 和区号。留空即解除绑定">
            <input type="tel" value={phone} onChange={(e) => { setPhone(e.target.value); setSaved(false); }} />
          </Field>
          <Field label="邮箱" hint="登录和验证用，暂不支持修改">
            <input value={p.email} readOnly />
          </Field>
          {error && <p className="foot-error" role="alert">{error}</p>}
          <div className="acct-row">
            <button className="btn btn-line" type="submit" disabled={busy || !dirty}>{busy ? '正在保存' : '保存'}</button>
            {saved && <span className="block-note" role="status">已保存</span>}
          </div>
        </form>
      </section>
      <section className="block block-wide acct">
        <h2 className="block-title">登录</h2>
        <p className="block-text">登录状态只保存在这台电脑上，用 Windows 账户加密。</p>
        <button className="btn btn-line" onClick={() => void signOut()}>退出登录</button>
      </section>
    </div>
  );
}

function Field({ label, hint, children }: { label: string; hint?: string; children: React.ReactNode }) {
  return (
    <label className="acct-field">
      <span className="acct-label">{label}</span>
      {children}
      {hint && <span className="acct-hint">{hint}</span>}
    </label>
  );
}
