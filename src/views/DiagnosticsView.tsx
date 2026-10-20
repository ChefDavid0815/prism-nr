import { useEffect, useState } from 'react';
import type { DiagnosticsReport, NvidiaDll } from '../../electron/shared/types';
import { api } from '../api';

export function DiagnosticsView() {
  const [report, setReport] = useState<DiagnosticsReport | null>(null);
  const [dlls, setDlls] = useState<NvidiaDll[] | null>(null);
  const [searching, setSearching] = useState(false);
  const [msg, setMsg] = useState<string | null>(null);

  useEffect(() => {
    api.diagnostics().then(setReport).catch((e) => setMsg((e as Error).message));
  }, []);

  const findDlls = async () => {
    setSearching(true);
    try {
      setDlls(await api.locateNvidiaDlls());
    } catch (e) {
      setMsg((e as Error).message);
    } finally {
      setSearching(false);
    }
  };

  const exportReport = async () => {
    const p = await api.exportDiagnostics().catch((e) => {
      setMsg((e as Error).message);
      return null;
    });
    if (p) setMsg(`已导出到 ${p}`);
  };

  const sys = report?.system;
  return (
    <div className="page">
      <header className="page-head">
        <h1>诊断</h1>
        <p>出问题时先看这里。导出的报告不含任何账号信息，可以直接发给社区求助。</p>
      </header>
      {msg && <p className="banner" role="status">{msg}</p>}

      <section className="diag-grid">
        <div className="diag-block">
          <h2>显卡</h2>
          {sys?.gpus.map((g) => (
            <dl className="facts facts-wide" key={g.name}>
              <div><dt>名称</dt><dd>{g.name}</dd></div>
              <div><dt>架构</dt><dd className="mono">{g.arch}</dd></div>
              <div><dt>显存</dt><dd className="mono">{g.vramMB ? `${g.vramMB} MB` : '—'}</dd></div>
              <div><dt>驱动</dt><dd className="mono">{g.driver ?? '—'}</dd></div>
              <div className="span2"><dt>预期</dt><dd>{g.expectation}</dd></div>
            </dl>
          ))}
          {sys?.notes.map((n) => (
            <p key={n} className="tone-caution-text">{n}</p>
          ))}
          <p className="dim mono">Windows build {sys?.os.build ?? '—'}</p>
        </div>

        <div className="diag-block">
          <h2>NVIDIA 运行库</h2>
          <p className="block-text">Prism 不下载也不分发 NVIDIA 文件，只使用本机已有并经过签名校验的副本。</p>
          <button className="btn btn-line" onClick={() => void findDlls()} disabled={searching}>
            {searching ? '搜索中' : '搜索本机'}
          </button>
          {dlls && (
            <div className="dll-list">
              {!dlls.length && <p className="dim">没有找到。</p>}
              {dlls.map((d) => (
                <div className="dll" key={d.path}>
                  <span className="mono">{d.file}</span>
                  <span className="mono">{d.version ?? '—'}</span>
                  <span className={d.signed ? 'tone-ready-text' : 'tone-caution-text'}>{d.signed ? '已签名' : '未签名'}</span>
                  <span className="dim dll-path">{d.path}</span>
                </div>
              ))}
            </div>
          )}
        </div>
      </section>

      <section className="diag-block">
        <div className="diag-head">
          <h2>日志</h2>
          <button className="btn btn-line btn-small" onClick={() => void exportReport()}>
            导出诊断报告
          </button>
        </div>
        <pre className="log">{report?.log.slice(-200).join('\n') || '暂无日志'}</pre>
      </section>
    </div>
  );
}
