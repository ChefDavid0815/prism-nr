import { useEffect, useState } from 'react';
import { CATEGORY_LABEL, PLUGINS } from '../../electron/shared/catalog';
import type { ComponentInfo, PluginCategory } from '../../electron/shared/types';
import { api } from '../api';

const ORIGIN_LABEL = { prism: 'Prism 原生', community: '社区组件', nvidia: 'NVIDIA' };
const CATS: (PluginCategory | 'all')[] = ['all', 'core', 'performance', 'quality', 'deai', 'stability'];

// Which component a community plugin installs from.
const COMPONENT_OF: Record<string, string> = { 'nr-renodx': 'renodx-dlss5', 'nr-dfc': 'dfc', feeder: 'feeder', 'motion-lumenite': 'lumenite', 'nr-optiscaler': 'optiscaler-presr', mgpu: 'mgpu' };
const MANUAL = new Set(['dfc', 'lumenite', 'mgpu']);

export function PluginsView() {
  const [cat, setCat] = useState<PluginCategory | 'all'>('all');
  const [open, setOpen] = useState<string | null>('prism-guard');
  const [components, setComponents] = useState<ComponentInfo[]>([]);
  const [msg, setMsg] = useState<string | null>(null);

  const refresh = () => api.listComponents().then(setComponents).catch(() => {});
  useEffect(() => {
    void refresh();
  }, []);

  const list = PLUGINS.filter((p) => cat === 'all' || p.category === cat);

  const importFor = async (componentId: string) => {
    try {
      const c = await api.importComponent(componentId);
      if (c) {
        setMsg(`已导入 ${c.name}`);
        void refresh();
      }
    } catch (e) {
      setMsg((e as Error).message);
    }
  };

  return (
    <div className="page">
      <header className="page-head">
        <h1>插件</h1>
        <p>Prism 把每个插件的用途、开销和适用显卡写清楚。一键启用时会按游戏和方案自动挑选，这里可以查看细节。</p>
      </header>
      <div className="tabs" role="tablist" aria-label="插件分类">
        {CATS.map((c) => (
          <button key={c} role="tab" aria-selected={cat === c} className={cat === c ? 'is-on' : ''} onClick={() => setCat(c)}>
            {c === 'all' ? '全部' : CATEGORY_LABEL[c]}
            <span className="mono">{String(PLUGINS.filter((p) => c === 'all' || p.category === c).length).padStart(2, '0')}</span>
          </button>
        ))}
      </div>
      {msg && <p className="banner" role="status">{msg}</p>}

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
          const compId = COMPONENT_OF[p.id];
          const have = compId ? components.some((c) => c.id === compId) : true;
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
                        {compId && (
                          <p className="comp-state">
                            <i className={have ? 'dot-ready' : MANUAL.has(compId) ? 'dot-caution' : 'dot-info'} />
                            {have ? '已在组件仓库' : MANUAL.has(compId) ? '需要导入一次' : '启用时自动下载'}
                            {!have && MANUAL.has(compId) && (
                              <button className="btn btn-line btn-small" onClick={() => void importFor(compId)}>
                                导入
                              </button>
                            )}
                          </p>
                        )}
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
