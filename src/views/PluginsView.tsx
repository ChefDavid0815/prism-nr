import { useState } from 'react';
import type { AccountState } from '../../electron/shared/account';
import { PLUGINS, PRESETS } from '../../electron/shared/catalog';
import { api } from '../api';
import { PacksBlock } from './PacksBlock';

const ORIGIN_LABEL = { prism: 'Prism 原生', community: '社区组件', nvidia: 'NVIDIA' };

export function PluginsView({ account, onSignIn }: { account: AccountState | null; onSignIn: () => void }) {
  const [open, setOpen] = useState<string | null>('nr-prism-core');
  // Prism Core is the only route since 1.2; the add-on plugins are gone from the app.
  const list = PLUGINS.filter((p) => p.routes.includes('prism-core'));

  return (
    <div className="page">
      <header className="page-head">
        <h1>插件</h1>
        <p>Prism NR 现在只用自己的 Prism Core：不需要 ReShade，也不用下载任何社区组件。档位、遍数和强度在游戏详情里调。</p>
      </header>

      <PacksBlock account={account} onSignIn={onSignIn} />

      <div className="ptable ptable-tiers" role="table" aria-label="Prism Core 档位">
        <div className="ptable-head" role="row">
          <span role="columnheader">档位</span>
          <span role="columnheader">节奏 · 遍数 · 强度</span>
          <span role="columnheader">说明</span>
          <span role="columnheader">适合</span>
        </div>
        {PRESETS.map((p) => (
          <div key={p.id} className="prow" role="row">
            <div className="prow-main">
              <span role="cell" className="prow-name">{p.name}</span>
              <span role="cell" className="mono">{p.gpuLabel}</span>
              <span role="cell" className="prow-sum">{p.summary}</span>
              <span role="cell" className="mono dim">{p.requiresGpu ? '仅 RTX 5090，谨慎开启' : p.forTiers.includes('flagship') ? 'RTX 5080 / 5090' : p.forTiers.includes('high') ? 'RTX 5070 Ti / 5080' : 'RTX 5060 / 5070'}</span>
            </div>
          </div>
        ))}
      </div>

      <div className="ptable" role="table" aria-label="插件列表">
        <div className="ptable-head" role="row">
          <span role="columnheader">编号</span>
          <span role="columnheader">名称</span>
          <span role="columnheader">用途</span>
          <span role="columnheader">开销</span>
          <span role="columnheader">来源</span>
        </div>
        {list.map((p) => {
          const isOpen = open === p.id;
          return (
            <div key={p.id} className={`prow ${isOpen ? 'is-open' : ''}`} role="row">
              <button className="prow-main" onClick={() => setOpen(isOpen ? null : p.id)} aria-expanded={isOpen}>
                <span className="mono" role="cell">{p.code}</span>
                <span role="cell" className="prow-name">{p.name}</span>
                <span role="cell" className="prow-sum">{p.summary}</span>
                <span role="cell" className="mono dim">{p.cost.split(/[：（]/)[0]}</span>
                <span role="cell" className={`origin origin-${p.origin}`}>{ORIGIN_LABEL[p.origin]}</span>
              </button>
              {isOpen && (
                <div className="prow-detail">
                  <div>
                    <h3>它做什么</h3>
                    <ul>
                      {p.does.map((d) => (
                        <li key={d}>{d}</li>
                      ))}
                    </ul>
                  </div>
                  <div>
                    <h3>开销与显卡</h3>
                    <p>{p.cost}</p>
                    <p className="dim">{p.gpu}</p>
                    {p.risk && (
                      <>
                        <h3>风险</h3>
                        <p className="tone-caution-text">{p.risk}</p>
                      </>
                    )}
                  </div>
                  <div>
                    {p.params.length > 0 && (
                      <>
                        <h3>参数</h3>
                        <dl className="params">
                          {p.params.map((x) => (
                            <div key={x.key}>
                              <dt>{x.label}</dt>
                              <dd className="mono">
                                {String(x.default)}
                                {x.unit ?? ''}
                              </dd>
                              <dd className="dim">{x.help}</dd>
                            </div>
                          ))}
                        </dl>
                      </>
                    )}
                    {p.source && (
                      <>
                        <h3>来源</h3>
                        <p>
                          {p.source.url ? (
                            <a href={p.source.url} onClick={(e) => { e.preventDefault(); void api.openExternal(p.source!.url!); }}>
                              {p.source.name}
                            </a>
                          ) : (
                            p.source.name
                          )}
                          {p.source.license ? `（${p.source.license}）` : ''}
                        </p>
                        {p.source.note && <p className="dim">{p.source.note}</p>}
                      </>
                    )}
                  </div>
                </div>
              )}
            </div>
          );
        })}
      </div>
    </div>
  );
}
