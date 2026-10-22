import { useState } from 'react';
import { LEGACY_PRESET_NAMES, tierOf } from '../../electron/shared/catalog';
import type { InstallRecord } from '../../electron/shared/types';
import { api } from '../api';
import { ROUTE_LABEL } from './LibraryView';

const STATUS_LABEL = { active: '生效中', 'rolled-back': '已还原', superseded: '已被替换' };

function when(iso?: string): string {
  if (!iso) return '—';
  const d = new Date(iso);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')} ${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}

export function HistoryView({ installs, onChange }: { installs: InstallRecord[]; onChange: () => void }) {
  const [error, setError] = useState<string | null>(null);

  const rollback = async (id: string) => {
    setError(null);
    try {
      await api.rollback(id);
      onChange();
    } catch (e) {
      setError((e as Error).message);
    }
  };

  return (
    <div className="page">
      <header className="page-head">
        <h1>还原记录</h1>
        <p>每次启用都会备份被覆盖的文件。还原会把游戏目录恢复到安装前的样子，记录本身会保留。</p>
      </header>
      {error && <p className="banner banner-blocked" role="alert">{error}</p>}
      {!installs.length ? (
        <div className="empty">
          <p>还没有安装记录。在游戏库里选一个游戏，点“一键启用”。</p>
        </div>
      ) : (
        <div className="htable" role="table" aria-label="安装记录">
          <div className="htable-head" role="row">
            <span role="columnheader">游戏</span>
            <span role="columnheader">档位</span>
            <span role="columnheader">路线</span>
            <span role="columnheader">文件</span>
            <span role="columnheader">安装时间</span>
            <span role="columnheader">状态</span>
            <span role="columnheader" />
          </div>
          {installs.map((r) => (
            <div key={r.id} role="row" className={`hrow status-${r.status}`}>
              <span role="cell">{r.gameName}</span>
              <span role="cell">{r.route === 'prism-core' ? tierOf(r.values ?? {})?.name ?? '自定义' : LEGACY_PRESET_NAMES[r.preset] ?? r.preset}</span>
              <span role="cell">{ROUTE_LABEL[r.route]}</span>
              <span role="cell" className="mono">
                {r.files.length} / 备份 {r.files.filter((f) => f.backup).length}
              </span>
              <span role="cell" className="mono dim">{when(r.installedAt)}</span>
              <span role="cell">
                <span className={`stamp stamp-${r.status}`}>{STATUS_LABEL[r.status]}</span>
                {r.rolledBackAt && <span className="mono dim"> {when(r.rolledBackAt)}</span>}
              </span>
              <span role="cell">
                {r.status === 'active' && (
                  <button className="btn btn-line btn-small" onClick={() => void rollback(r.id)}>
                    还原
                  </button>
                )}
              </span>
              {r.rollbackNotes?.length ? (
                <ul className="hrow-notes">
                  {r.rollbackNotes.map((n) => (
                    <li key={n}>{n}</li>
                  ))}
                </ul>
              ) : null}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
