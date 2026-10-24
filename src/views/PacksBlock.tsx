import { useCallback, useEffect, useState } from 'react';
import { PACK_SOURCE, isRedeemCode, normRedeemCode, showRedeemCode, type AccountState, type Pack } from '../../electron/shared/account';
import { api } from '../api';

const day = (iso: string) => new Date(iso).toLocaleDateString('zh-CN', { year: 'numeric', month: '2-digit', day: '2-digit' });

/**
 * Plugin packs on the plugins page: a redeem code field, and every pack on offer with whether this
 * account owns it. A code ties its pack to the account, so redeeming needs one; browsing does not.
 * How a pack is used comes later; for now owning it is recorded on the server.
 */
export function PacksBlock({ account, onSignIn }: { account: AccountState | null; onSignIn: () => void }) {
  const [packs, setPacks] = useState<Pack[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [raw, setRaw] = useState('');
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState<string | null>(null);
  const [fresh, setFresh] = useState<string | null>(null);
  const signedIn = Boolean(account?.signedIn);
  const code = normRedeemCode(raw).slice(0, 16);

  const load = useCallback(async () => {
    setLoading(true);
    try {
      setPacks(await api.account.packs());
      setError(null);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void load();
  }, [load, signedIn]);

  const redeem = async () => {
    setBusy(true);
    setError(null);
    setNote(null);
    try {
      const r = await api.account.redeem(code);
      setNote(r.already ? `你的账号已经有「${r.name}」了，这个兑换码没有被用掉，可以留给别人。` : `已领取「${r.name}」，它已经绑定到你的账号。`);
      if (!r.already) setFresh(r.pack);
      setRaw('');
      await load();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <section className="block block-wide packs">
      <h2 className="block-title">插件包</h2>
      <p className="block-text">插件包是额外的内容合集。拿到兑换码以后在这里输入，插件包就会绑定到你的账号，换台电脑登录也还在。</p>

      <form className="redeem" onSubmit={(e) => { e.preventDefault(); if (signedIn && isRedeemCode(code)) void redeem(); }}>
        <label className="acct-field redeem-field">
          <span className="acct-label">兑换码</span>
          <input
            className="mono redeem-input"
            value={showRedeemCode(code)}
            onChange={(e) => { setRaw(e.target.value); setError(null); setNote(null); }}
            placeholder="XXXX-XXXX-XXXX-XXXX"
            spellCheck={false}
            autoComplete="off"
            disabled={!signedIn}
            aria-describedby="redeem-ticks"
          />
          <span id="redeem-ticks" className="redeem-ticks" aria-label={`已输入 ${code.length} / 16 位`}>
            {Array.from({ length: 16 }, (_, i) => <i key={i} className={i < code.length ? 'is-on' : ''} />)}
          </span>
        </label>
        {signedIn ? (
          <button className="btn btn-primary redeem-go" type="submit" disabled={busy || !isRedeemCode(code)}>{busy ? '正在兑换' : '兑换'}</button>
        ) : (
          <button className="btn btn-line redeem-go" type="button" onClick={onSignIn}>登录后兑换</button>
        )}
      </form>
      {!signedIn && <p className="block-note">兑换码会把插件包绑定到账号上，所以要先登录。</p>}
      {error && <p className="foot-error" role="alert">{error}</p>}
      {note && <p className="block-note redeem-note" role="status">{note}</p>}

      <div className="packs-table" role="table" aria-label="插件包">
        <div className="packs-row packs-head" role="row">
          <span role="columnheader" aria-hidden="true" />
          <span role="columnheader">名称</span>
          <span role="columnheader">内容</span>
          <span role="columnheader">状态</span>
        </div>
        {packs.map((p) => (
          <div key={p.id} className={`packs-row ${p.owned ? 'is-owned' : ''} ${fresh === p.id ? 'is-fresh' : ''}`} role="row">
            <i role="cell" className="packs-mark" aria-hidden="true" />
            <span role="cell" className="prow-name">{p.name}</span>
            <span role="cell" className="prow-sum">{p.description || '内容之后公布。'}</span>
            <span role="cell" className="packs-state">
              {p.owned ? (
                <>
                  <strong>已拥有</strong>
                  <span className="mono dim">{PACK_SOURCE[p.source ?? ''] ?? ''}{p.owned_at ? `，${day(p.owned_at)}` : ''}</span>
                </>
              ) : (
                <span className="dim">未拥有</span>
              )}
            </span>
          </div>
        ))}
        {!loading && packs.length === 0 && !error && <p className="block-note admin-empty">还没有上架的插件包。有了以后会出现在这里。</p>}
      </div>
    </section>
  );
}
