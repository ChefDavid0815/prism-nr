import type { AccountPlan, AccountRole } from '../../electron/shared/account';

/**
 * An account's marks, drawn from the brand: white light through a prism, out as the four colours.
 * The nameplate (Basic, Pro, Ultra) sits after the name; the founder's seal is the prism itself with
 * the spectrum leaving it; the contributor's mark is three bars of the app's barcode; an official
 * look carries the prism and 官方. Free accounts show no nameplate.
 */

const PLAN_LABEL: Record<Exclude<AccountPlan, 'free'>, string> = { basic: 'BASIC', pro: 'PRO', ultra: 'ULTRA' };
export const PLAN_NAME: Record<AccountPlan, string> = { free: '无', basic: 'Basic', pro: 'Pro', ultra: 'Ultra' };

export function Nameplate({ plan, size = 'sm' }: { plan: AccountPlan; size?: 'sm' | 'lg' }) {
  if (plan === 'free') return null;
  return (
    <span className={`plate plate-${plan} plate-${size}`} title={`${PLAN_NAME[plan]} 用户`}>
      {plan === 'ultra' && <PrismGlyph />}
      <span className="plate-word">{PLAN_LABEL[plan]}</span>
      {plan === 'pro' && <span className="plate-spectrum" aria-hidden="true"><i /><i /><i /><i /></span>}
    </span>
  );
}

/** The prism, small: a triangle with the beam in and the spectrum out. */
function PrismGlyph() {
  return (
    <svg className="plate-glyph" viewBox="0 0 22 14" aria-hidden="true">
      <line x1="0" y1="8.4" x2="7.4" y2="7.2" stroke="currentColor" strokeWidth="1" />
      <path d="M11 1 L17 13 H5 Z" fill="none" stroke="currentColor" strokeWidth="1.2" />
      <line x1="13.4" y1="6.6" x2="22" y2="3.8" stroke="#FF3B30" strokeWidth="1" />
      <line x1="13.7" y1="7.3" x2="22" y2="6.1" stroke="#FFC400" strokeWidth="1" />
      <line x1="14" y1="8" x2="22" y2="8.4" stroke="#3DDC97" strokeWidth="1" />
      <line x1="14.3" y1="8.7" x2="22" y2="10.7" stroke="#4DA3FF" strokeWidth="1" />
    </svg>
  );
}

/** 创始人认证: a solid prism with a tick, the spectrum leaving its face. */
export function FounderSeal({ label = false }: { label?: boolean }) {
  return (
    <span className="seal seal-founder" title="创始人认证">
      <svg viewBox="0 0 26 18" aria-hidden="true">
        <path d="M9.5 1.2 L17.8 16.8 H1.2 Z" fill="#fff" />
        <path d="M5.6 11.2 L8.4 13.8 L13 7.6" fill="none" stroke="#000" strokeWidth="1.6" strokeLinecap="square" />
        <line x1="14.6" y1="8" x2="26" y2="4" stroke="#FF3B30" strokeWidth="1.1" />
        <line x1="15.1" y1="9" x2="26" y2="7.4" stroke="#FFC400" strokeWidth="1.1" />
        <line x1="15.6" y1="10" x2="26" y2="10.8" stroke="#3DDC97" strokeWidth="1.1" />
        <line x1="16.1" y1="11" x2="26" y2="14.2" stroke="#4DA3FF" strokeWidth="1.1" />
      </svg>
      {label && <span>创始人</span>}
    </span>
  );
}

/** 社区贡献者: three rising bars, the community fingerprint in miniature. */
export function ContributorMark({ label = false }: { label?: boolean }) {
  return (
    <span className="seal seal-contrib" title="社区贡献者">
      <svg viewBox="0 0 12 14" aria-hidden="true">
        <rect x="0" y="8" width="2" height="6" />
        <rect x="5" y="4" width="2" height="10" />
        <rect x="10" y="0" width="2" height="14" />
      </svg>
      {label && <span>社区贡献者</span>}
    </span>
  );
}

export function RoleTag({ role }: { role: AccountRole }) {
  if (role === 'user') return null;
  return <span className={`role-tag role-${role}`}>{role === 'owner' ? '站长' : '管理员'}</span>;
}

/** A look by Prism NR itself. */
export function OfficialMark() {
  return (
    <span className="official-mark" title="Prism NR 官方调色">
      <svg viewBox="0 0 14 12" aria-hidden="true"><path d="M7 1 L13 11 H1 Z" fill="none" stroke="currentColor" strokeWidth="1.4" /></svg>
      官方
    </span>
  );
}

/** Every mark an account has, in a fixed order: role, founder, contributor, nameplate. */
export function Marks({ role, founder, contributor, plan, size = 'sm', labels = false }: { role?: AccountRole; founder: boolean; contributor: boolean; plan: AccountPlan; size?: 'sm' | 'lg'; labels?: boolean }) {
  return (
    <span className={`marks marks-${size}`}>
      {role && <RoleTag role={role} />}
      {founder && <FounderSeal label={labels} />}
      {contributor && <ContributorMark label={labels} />}
      <Nameplate plan={plan} size={size} />
    </span>
  );
}

// ---------------------------------------------------------------- the avatar

const TILE = ['#FF3B30', '#FFC400', '#3DDC97', '#4DA3FF'];
function hash(s: string) {
  let h = 0;
  for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0;
  return Math.abs(h);
}

/** The picture, or the name's first character on a tile edged in one of the four colours. Ultra accounts wear the spectrum round it. */
export function Avatar({ name, src, size = 32, plan }: { name: string; src?: string | null; size?: number; plan?: AccountPlan }) {
  const edge = TILE[hash(name) % TILE.length];
  return (
    <span className={`avatar ${plan === 'ultra' ? 'avatar-ultra' : ''}`} style={{ width: size, height: size, fontSize: Math.round(size * 0.44) }} aria-hidden="true">
      {src ? <img src={src} alt="" draggable={false} /> : <span className="avatar-mono" style={plan === 'ultra' ? undefined : { boxShadow: `inset 0 -${Math.max(2, Math.round(size / 16))}px 0 ${edge}` }}>{[...name][0]?.toUpperCase() ?? '?'}</span>}
    </span>
  );
}

/** A picture file as a square avatar: centre-cropped, 160 px, WebP, small enough for the server. */
export async function avatarFromFile(file: File, max: number): Promise<string> {
  if (!/^image\//.test(file.type)) throw new Error('请选择一张图片。');
  const url = URL.createObjectURL(file);
  try {
    const img = await new Promise<HTMLImageElement>((ok, fail) => {
      const i = new Image();
      i.onload = () => ok(i);
      i.onerror = () => fail(new Error('这张图片打不开，请换一张。'));
      i.src = url;
    });
    const side = Math.min(img.naturalWidth, img.naturalHeight);
    const canvas = document.createElement('canvas');
    for (const px of [160, 128, 96]) {
      canvas.width = canvas.height = px;
      const ctx = canvas.getContext('2d')!;
      ctx.imageSmoothingQuality = 'high';
      ctx.drawImage(img, (img.naturalWidth - side) / 2, (img.naturalHeight - side) / 2, side, side, 0, 0, px, px);
      for (const q of [0.9, 0.8, 0.65]) {
        const data = canvas.toDataURL('image/webp', q);
        if (data.startsWith('data:image/webp') && data.length <= max) return data;
      }
    }
    throw new Error('这张图片压缩后还是太大，请换一张。');
  } finally {
    URL.revokeObjectURL(url);
  }
}
