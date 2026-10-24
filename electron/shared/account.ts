/**
 * Prism NR accounts (1.5, experimental): optional sign-in with an email, a phone number (any
 * country) or a username, a password, and a code sent to the email to prove it is real. The server
 * is the prism-nr Supabase project; this file holds what both sides of the app share. The key below
 * is the project's publishable key: it is public by design (row-level security and the edge
 * functions decide what anyone may do), and never the service key.
 */

export const ACCOUNT_URL = 'https://zqcqvjjxxwiurlqwppwb.supabase.co';
/**
 * The same server through a mirror on Vercel's edge (account.chefzc.dev, Hong Kong first for users
 * in mainland China): a reverse proxy, not a copy, so there is one set of accounts. The app uses
 * whichever answers first and moves to the other when one stops answering.
 */
export const ACCOUNT_MIRROR = 'https://account.chefzc.dev';
export const ACCOUNT_BASES = [ACCOUNT_URL, ACCOUNT_MIRROR];
export const ACCOUNT_KEY = 'sb_publishable_VYoxuPnb8Y81Ee3Cus0GMg_c_Drcb3z';

/** owner (站长) is above every administrator. */
export type AccountRole = 'user' | 'admin' | 'owner';
/** The nameplate an account shows: none, Basic, Pro, or Ultra. */
export type AccountPlan = 'free' | 'basic' | 'pro' | 'ultra';

/** What other people see of an account: its face, its nameplate and its marks. */
export interface AccountBadges {
  plan: AccountPlan;
  founder: boolean;
  contributor: boolean;
  role: AccountRole;
}

export interface AccountProfile extends AccountBadges {
  id: string;
  username: string;
  email: string;
  phone: string | null;
  bio: string;
  /** A small image as a data URL, or null for the monogram. */
  avatar: string | null;
  createdAt: string;
}

export const isStaff = (role: AccountRole | undefined) => role === 'admin' || role === 'owner';
export const BIO_MAX = 160;
/** The largest avatar the server takes (its data URL's length). */
export const AVATAR_MAX = 60000;

export interface AccountState {
  signedIn: boolean;
  profile?: AccountProfile;
}

/** A sign-in or sign-up that needs the email's code first. */
export interface AccountPending {
  needsCode: true;
  /** The address the code went to, or a masked form when the user signed in with a phone or name. */
  email: string | null;
  hint?: string;
}

export interface AdminUser {
  id: string;
  username: string;
  email: string;
  phone: string | null;
  role: AccountRole;
  confirmed: boolean;
  banned: boolean;
  created_at: string;
  last_sign_in_at: string | null;
  plan: AccountPlan;
  founder: boolean;
  contributor: boolean;
  avatar: string | null;
  bio: string;
}

export interface AdminStats {
  total: number;
  confirmed: number;
  admins: number;
  banned: number;
  new_7d: number;
  active_7d: number;
  new_today: number;
}

/** A look shared in the community (the grade_shares table). */
export interface CommunityShare {
  id: string;
  title: string;
  description: string;
  game_name: string;
  game_exe: string | null;
  steam_app_id: string | null;
  code: string;
  imports: number;
  created_at: string;
  author: string;
  own: boolean;
  /** One of Prism NR's own looks. */
  official: boolean;
  author_id: string;
  author_avatar: string | null;
  author_plan: AccountPlan;
  author_founder: boolean;
  author_contributor: boolean;
}
export interface CommunityQuery {
  q?: string;
  /** Only shares for this game (Steam app id, or executable name). */
  steam?: string | null;
  exe?: string | null;
  mine?: boolean;
  official?: boolean;
  /** Every look of one game (a CommunityGame's key). */
  gkey?: string;
  page?: number;
}

/** A game that has looks in the community, with its Chinese and English names. */
export interface CommunityGame {
  /** Its Steam app id, or n: and its lower-cased name. */
  key: string;
  steam_app_id: string | null;
  name_en: string;
  name_zh: string;
  game_exe: string | null;
  shares: number;
  official: number;
  imports: number;
  latest: string;
}

/** A plugin pack as a user sees it: the active ones, and any they own. */
export interface Pack {
  id: string;
  name: string;
  description: string;
  owned: boolean;
  owned_at: string | null;
  /** How it was got: a redeem code, an administrator, or (later) a purchase. */
  source: 'code' | 'admin' | 'purchase' | null;
}

export interface RedeemResult {
  pack: string;
  name: string;
  /** The account had this pack already; the code was not used up. */
  already: boolean;
}

export interface AdminPack {
  id: string;
  name: string;
  description: string;
  active: boolean;
  created_at: string;
  owners: number;
  codes: number;
  /** Codes that can still be redeemed: not disabled, not used up, not expired. */
  codes_left: number;
}

export interface AdminCode {
  code: string;
  pack_id: string;
  max_uses: number;
  uses: number;
  expires_at: string | null;
  disabled: boolean;
  note: string;
  created_at: string;
}

export interface AdminUserPack {
  pack_id: string;
  source: 'code' | 'admin' | 'purchase';
  code: string | null;
  created_at: string;
}

export type AdminAction =
  | { action: 'stats' }
  | { action: 'list'; q?: string; page?: number; size?: number }
  | { action: 'update'; id: string; username?: string; phone?: string | null; role?: 'user' | 'admin'; plan?: AccountPlan; founder?: boolean; contributor?: boolean }
  | { action: 'ban'; id: string; banned: boolean }
  | { action: 'delete'; id: string }
  | { action: 'password'; id: string; password: string }
  | { action: 'deleteShare'; id: string }
  | { action: 'setOfficial'; id: string; official: boolean }
  | { action: 'packs' }
  | { action: 'savePack'; pack: string; name: string; description: string; active?: boolean; create?: boolean }
  | { action: 'codes'; pack: string }
  | { action: 'makeCodes'; pack: string; count: number; maxUses: number; days: number | null; note: string }
  | { action: 'setCode'; code: string; disabled: boolean }
  | { action: 'userPacks'; id: string }
  | { action: 'grant'; id: string; pack: string }
  | { action: 'revoke'; id: string; pack: string };

export const USERNAME_RULE = /^[0-9A-Za-z_一-龥]{2,20}$/;
export const USERNAME_HELP = '2–20 个字，可以用中文、字母、数字和下划线';

/** E.164 (+8613800138000) from what people type: 11-digit mainland numbers, 00-prefixed, spaces and dashes. */
export function normPhone(raw: string): string | null {
  const s = raw.replace(/[\s\-()]/g, '');
  if (/^1[3-9]\d{9}$/.test(s)) return '+86' + s;
  if (/^\+[1-9]\d{6,14}$/.test(s)) return s;
  if (/^00[1-9]\d{6,14}$/.test(s)) return '+' + s.slice(2);
  return null;
}

/** A phone number as people read it: +86 138 0013 8000 for mainland numbers, the E.164 form otherwise. */
export function showPhone(e164: string | null | undefined): string {
  if (!e164) return '';
  const m = e164.match(/^\+86(1\d{2})(\d{4})(\d{4})$/);
  return m ? `+86 ${m[1]} ${m[2]} ${m[3]}` : e164;
}

/**
 * Redeem codes: 16 characters of Crockford's base 32 (no I, L, O or U), 80 random bits, shown in
 * groups of four. What people type is read leniently: any case, any separators, O as 0, I and L as 1,
 * and an optional PRISM- in front. The server reads them the same way.
 */
export const REDEEM_ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';

export function normRedeemCode(raw: string): string {
  let c = raw.toUpperCase().replace(/[^0-9A-Z]/g, '').replace(/O/g, '0').replace(/[IL]/g, '1');
  if (c.length === 21 && c.startsWith('PR1SM')) c = c.slice(5);
  return c;
}

export const isRedeemCode = (c: string) => /^[0-9A-HJKMNP-TV-Z]{16}$/.test(c);

export function showRedeemCode(c: string): string {
  return c.replace(/(.{4})(?=.)/g, '$1-');
}

export const PACK_SOURCE: Record<string, string> = { code: '兑换码', admin: '管理员发放', purchase: '购买' };
