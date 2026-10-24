/**
 * The account session, in the main process: sign up, the email's code, sign in (email, phone or
 * username), sign out, the profile, and the administrator's actions. Talks to the prism-nr Supabase
 * project over HTTPS (Auth, REST and its two edge functions). The session is kept on this PC only,
 * encrypted with Electron's safeStorage (the user's Windows credentials), and refreshed before it
 * expires.
 */
import * as fs from 'node:fs';
import * as path from 'node:path';
import { safeStorage } from 'electron';
import {
  ACCOUNT_BASES, ACCOUNT_KEY, AVATAR_MAX, BIO_MAX, USERNAME_HELP, USERNAME_RULE, isRedeemCode, normPhone, normRedeemCode,
  type AccountPending, type AccountProfile, type AccountState, type AdminAction, type CommunityQuery, type CommunityShare,
  type CommunityGame, type Pack, type RedeemResult,
} from '../shared/account';
import { decodeGradeCode } from '../shared/gradecode';
import { log } from './util';

interface Session {
  access_token: string;
  refresh_token: string;
  expires_at: number;  // seconds since the epoch
  user: { id: string; email: string };
}

type Json = Record<string, any>;

export class AccountService {
  private session: Session | null = null;
  /** The server address in use (the direct one or the mirror), picked by whichever answers first. */
  private base: Promise<string> | null = null;
  private profile: AccountProfile | null = null;
  private readonly file: string;

  constructor(userData: string) {
    this.file = path.join(userData, 'account.bin');
    try {
      if (fs.existsSync(this.file) && safeStorage.isEncryptionAvailable()) this.session = JSON.parse(safeStorage.decryptString(fs.readFileSync(this.file)));
    } catch {
      this.session = null;
    }
  }

  private save(): void {
    try {
      if (!this.session) fs.rmSync(this.file, { force: true });
      else if (safeStorage.isEncryptionAvailable()) fs.writeFileSync(this.file, safeStorage.encryptString(JSON.stringify(this.session)));
    } catch (e) {
      log(`account: session not saved: ${(e as Error).message}`);
    }
  }

  /** Both addresses at once: the first to answer the health check is used. */
  private pick(): Promise<string> {
    if (!this.base) {
      this.base = new Promise<string>((resolve) => {
        let left = ACCOUNT_BASES.length;
        for (const b of ACCOUNT_BASES)
          fetch(`${b}/auth/v1/health`, { headers: { apikey: ACCOUNT_KEY }, signal: AbortSignal.timeout(6000) })
            .then((r) => { if (r.ok) resolve(b); else if (--left === 0) resolve(ACCOUNT_BASES[0]); })
            .catch(() => { if (--left === 0) resolve(ACCOUNT_BASES[0]); });
      }).then((b) => { log(`account: using ${b}`); return b; });
    }
    return this.base;
  }

  /** A request to the account server (path from /auth, /rest or /functions); on a network failure, once more through the other address. */
  private async request(pathAndQuery: string, init: RequestInit & { json?: unknown; token?: string } = {}): Promise<{ status: number; data: Json }> {
    const headers: Record<string, string> = { apikey: ACCOUNT_KEY, 'Content-Type': 'application/json', ...(init.headers as Record<string, string>) };
    if (init.token) headers.Authorization = `Bearer ${init.token}`;
    const send = (base: string) => fetch(base + pathAndQuery, { ...init, headers, body: init.json !== undefined ? JSON.stringify(init.json) : init.body, signal: AbortSignal.timeout(12000) });
    let r: Response;
    const first = await this.pick();
    try {
      r = await send(first);
    } catch {
      const other = ACCOUNT_BASES.find((b) => b !== first)!;
      try {
        r = await send(other);
        this.base = Promise.resolve(other);
        log(`account: ${first} stopped answering; using ${other}`);
      } catch {
        throw new Error('连不上账号服务器，请检查网络后再试。');
      }
    }
    const text = await r.text();
    let data: Json = {};
    try {
      data = text ? JSON.parse(text) : {};
    } catch {
      data = { message: text };
    }
    return { status: r.status, data };
  }

  private async fn(name: string, body: unknown, token?: string): Promise<Json> {
    const { status, data } = await this.request(`/functions/v1/${name}`, { method: 'POST', json: body, token });
    if (status >= 400 && data.error !== 'unconfirmed') throw new Error(String(data.message ?? `请求失败（${status}）`));
    return { ...data, status };
  }

  private adopt(s: Json): void {
    this.session = {
      access_token: s.access_token,
      refresh_token: s.refresh_token,
      expires_at: s.expires_at ?? Math.floor(Date.now() / 1000) + Number(s.expires_in ?? 3600),
      user: { id: s.user?.id, email: s.user?.email },
    };
    this.save();
  }

  /** A valid access token, refreshed when it is within a minute of expiring. */
  private async token(): Promise<string> {
    if (!this.session) throw new Error('请先登录。');
    if (this.session.expires_at - 60 < Date.now() / 1000) {
      const { status, data } = await this.request(`/auth/v1/token?grant_type=refresh_token`, { method: 'POST', json: { refresh_token: this.session.refresh_token } });
      if (status >= 400) {
        this.session = null;
        this.profile = null;
        this.save();
        throw new Error('登录已过期，请重新登录。');
      }
      this.adopt(data);
    }
    return this.session!.access_token;
  }

  private async loadProfile(): Promise<AccountProfile | null> {
    if (!this.session) return null;
    const token = await this.token();
    const { status, data } = await this.request(`/rest/v1/profiles?id=eq.${this.session.user.id}&select=*`, { token });
    const row = Array.isArray(data) ? (data as Json[])[0] : null;
    if (status >= 400 || !row) return null;
    this.profile = {
      id: row.id, username: row.username, phone: row.phone, role: row.role, createdAt: row.created_at, email: this.session.user.email,
      bio: row.bio ?? '', avatar: row.avatar ?? null, plan: row.plan ?? 'free', founder: Boolean(row.founder), contributor: Boolean(row.contributor),
    };
    return this.profile;
  }

  async state(): Promise<AccountState> {
    if (!this.session) return { signedIn: false };
    try {
      const profile = this.profile ?? (await this.loadProfile());
      return profile ? { signedIn: true, profile } : { signedIn: false };
    } catch (e) {
      // Offline: still signed in on this PC, the profile as last seen.
      return this.profile ? { signedIn: true, profile: this.profile } : { signedIn: false };
    }
  }

  async signUp(args: { email: string; password: string; username?: string; phone?: string }): Promise<AccountPending> {
    const r = await this.fn('account', { action: 'signup', ...args });
    return { needsCode: true, email: r.email };
  }

  /** The code from the email (6 to 10 digits, as the project's OTP length says); signs in on success. */
  async verify(email: string, code: string): Promise<AccountState> {
    const token = code.replace(/\s/g, '');
    if (!/^\d{6,10}$/.test(token)) throw new Error('验证码是邮件里的一串数字（6 到 10 位）。');
    let r = await this.request(`/auth/v1/verify`, { method: 'POST', json: { type: 'email', email, token } });
    if (r.status >= 400) r = await this.request(`/auth/v1/verify`, { method: 'POST', json: { type: 'signup', email, token } });
    if (r.status >= 400) throw new Error('验证码不对或已过期，请重新发送。');
    this.adopt(r.data);
    this.profile = null;
    return this.state();
  }

  async resend(email: string): Promise<void> {
    await this.fn('account', { action: 'resend', email });
  }

  /** Signs in; a pending result means the email still needs its code (or the link in the email). */
  async signIn(identifier: string, password: string): Promise<AccountState | AccountPending> {
    const r = await this.fn('account', { action: 'signin', identifier, password });
    if (r.error === 'unconfirmed') return { needsCode: true, email: r.email ?? null, hint: r.hint };
    this.adopt(r.session);
    this.profile = null;
    return this.state();
  }

  async signOut(): Promise<void> {
    if (this.session) await this.request(`/auth/v1/logout`, { method: 'POST', token: this.session.access_token }).catch(() => undefined);
    this.session = null;
    this.profile = null;
    this.save();
  }

  async updateProfile(patch: { username?: string; phone?: string | null; bio?: string; avatar?: string | null }): Promise<AccountState> {
    const body: Json = {};
    if (patch.bio !== undefined) {
      const b = patch.bio.trim();
      if ([...b].length > BIO_MAX) throw new Error(`简介最多 ${BIO_MAX} 个字。`);
      body.bio = b;
    }
    if (patch.avatar !== undefined) {
      if (patch.avatar && (!/^data:image\/(webp|png|jpeg);base64,/.test(patch.avatar) || patch.avatar.length > AVATAR_MAX)) throw new Error('头像图片太大或格式不对，请换一张。');
      body.avatar = patch.avatar;
    }
    if (patch.username !== undefined) {
      const u = patch.username.trim();
      if (!USERNAME_RULE.test(u)) throw new Error(`用户名要 ${USERNAME_HELP}。`);
      body.username = u;
    }
    if (patch.phone !== undefined) {
      const raw = (patch.phone ?? '').trim();
      const p = raw ? normPhone(raw) : null;
      if (raw && !p) throw new Error('手机号格式不对：国内直接填 11 位，其他国家请带 + 和国家区号。');
      body.phone = p;
    }
    const token = await this.token();
    const { status, data } = await this.request(`/rest/v1/profiles?id=eq.${this.session!.user.id}`, { method: 'PATCH', json: body, token, headers: { Prefer: 'return=minimal' } });
    if (status >= 400) {
      if (String(data.code) === '23505') throw new Error(String(data.message ?? '').includes('phone') ? '这个手机号已被其他账号使用。' : '这个用户名已经有人用了。');
      throw new Error(String(data.message ?? `保存失败（${status}）`));
    }
    this.profile = null;
    return this.state();
  }

  // ------------------------------------------------------------ the community (signed-in accounts only)

  async communityList(query: CommunityQuery): Promise<{ shares: CommunityShare[]; total: number }> {
    const size = 50;
    if (!this.session) throw new Error('社区需要先登录。');
    const token = await this.token();
    const { status, data } = await this.request('/rest/v1/rpc/community_list', {
      method: 'POST',
      token,
      json: { q: query.q?.trim() || null, steam: query.steam || null, exe: query.exe || null, mine: Boolean(query.mine), official_only: Boolean(query.official), gkey: query.gkey || null, lim: size, off: (query.page ?? 0) * size },
    });
    if (status >= 400) throw new Error(String((data as Json).message ?? `社区加载失败（${status}）`));
    const rows = (Array.isArray(data) ? data : []) as (CommunityShare & { total: number })[];
    return { shares: rows.map(({ total: _t, ...r }) => r), total: rows.length ? Number(rows[0].total) : 0 };
  }

  /** The games that have looks; a search finds Chinese and English names and nicknames. */
  async communityGames(q: string): Promise<{ games: CommunityGame[]; total: number }> {
    if (!this.session) throw new Error('社区需要先登录。');
    const token = await this.token();
    const { status, data } = await this.request('/rest/v1/rpc/community_games', { method: 'POST', token, json: { q: q.trim() || null, lim: 200, off: 0 } });
    if (status >= 400) throw new Error(String((data as Json).message ?? `社区加载失败（${status}）`));
    const rows = (Array.isArray(data) ? data : []) as (CommunityGame & { total: number })[];
    return { games: rows.map(({ total: _t, ...g }) => ({ ...g, shares: Number(g.shares), official: Number(g.official), imports: Number(g.imports) })), total: rows.length ? Number(rows[0].total) : 0 };
  }

  async communityShare(args: { title: string; description: string; code: string }): Promise<void> {
    const title = args.title.trim();
    if (!title) throw new Error('给这套调色起个名字。');
    if (title.length > 40) throw new Error('名字最多 40 个字。');
    if (args.description.length > 300) throw new Error('说明最多 300 个字。');
    const { game } = decodeGradeCode(args.code);
    const token = await this.token();
    const { status, data } = await this.request('/rest/v1/grade_shares', {
      method: 'POST',
      token,
      headers: { Prefer: 'return=minimal' },
      json: { title, description: args.description.trim(), game_name: game.name, game_exe: game.exe ?? null, steam_app_id: game.steamAppId ?? null, code: args.code.trim() },
    });
    if (status >= 400) throw new Error(String(data.message ?? `分享失败（${status}）`));
  }

  /** The author's own share; an administrator's goes through the admin function. */
  async communityDelete(id: string, asAdmin = false): Promise<void> {
    if (asAdmin) {
      await this.admin({ action: 'deleteShare', id });
      return;
    }
    const token = await this.token();
    const { status, data } = await this.request(`/rest/v1/grade_shares?id=eq.${encodeURIComponent(id)}`, { method: 'DELETE', token });
    if (status >= 400) throw new Error(String(data.message ?? `删除失败（${status}）`));
  }

  async communityImported(id: string): Promise<void> {
    const token = await this.token().catch(() => undefined);
    if (token) await this.request('/rest/v1/rpc/community_imported', { method: 'POST', token, json: { share: id } }).catch(() => undefined);
  }

  // ------------------------------------------------------------ plugin packs (redeem codes now; purchases later)

  /** The active packs, and whether this account owns each (signed out: none owned). */
  async packs(): Promise<Pack[]> {
    const token = this.session ? await this.token().catch(() => undefined) : undefined;
    const { status, data } = await this.request('/rest/v1/rpc/pack_list', { method: 'POST', token, json: {} });
    if (status >= 400) throw new Error(String((data as Json).message ?? `插件包加载失败（${status}）`));
    return (Array.isArray(data) ? data : []) as Pack[];
  }

  async redeem(raw: string): Promise<RedeemResult> {
    const code = normRedeemCode(raw);
    if (!isRedeemCode(code)) throw new Error('兑换码是 16 位字母和数字，形如 ABCD-EFGH-JKMN-PQRS，请检查有没有输错或漏掉。');
    const token = await this.token();
    const { status, data } = await this.request('/rest/v1/rpc/redeem_code', { method: 'POST', token, json: { code } });
    if (status >= 400) throw new Error(String((data as Json).message ?? `兑换失败（${status}）`));
    const r = data as Json;
    const why: Record<string, string> = {
      auth: '请先登录。',
      limit: '输错的次数太多，请一小时后再试。',
      invalid: '没有这个兑换码，请检查有没有输错。',
      disabled: '这个兑换码已经停用了。',
      expired: '这个兑换码已经过期了。',
      used: '这个兑换码已经被用完了。',
    };
    if (r.error) throw new Error(why[r.error] ?? `兑换失败（${r.error}）`);
    return { pack: String(r.pack), name: String(r.name ?? r.pack), already: Boolean(r.already) };
  }

  async admin(action: AdminAction): Promise<Json> {
    return this.fn('admin', action, await this.token());
  }
}
