import { useEffect, useState } from 'react';
import type { SystemInfo } from '../../electron/shared/types';
import type { MotionLevel } from '../lib/motion';
import { api } from '../api';

const MOTION: { id: MotionLevel; name: string; text: string }[] = [
  { id: 'max', name: '极致', text: '数据场持续流动，光标会点亮经过的竖条，启用时整屏条码扫描并反相一次。' },
  { id: 'standard', name: '标准', text: '数据场缓慢流动，保留过场，去掉光标互动。' },
  { id: 'off', name: '关', text: '界面静止，只保留必要的状态变化。' },
];

const CREDITS = [
  { name: 'MinHook（Prism Core 的函数挂钩）', url: 'https://github.com/TsudaKageyu/minhook', license: 'BSD-2-Clause' },
  { name: 'RenoDX DLSS 5（Prism 早期版本的参考实现）', url: 'https://github.com/RankFTW/rhi-repo', license: '见项目仓库' },
];

export function SettingsView({ motion, onMotion, system }: { motion: MotionLevel; onMotion: (m: MotionLevel) => void; system: SystemInfo | null }) {
  return (
    <div className="page">
      <header className="page-head">
        <h1>设置</h1>
      </header>
      <section className="block block-wide">
        <h2 className="block-title">动效强度</h2>
        <div className="presets presets-row" role="radiogroup" aria-label="动效强度">
          {MOTION.map((m) => (
            <button key={m.id} role="radio" aria-checked={motion === m.id} className={`preset ${motion === m.id ? 'is-on' : ''}`} onClick={() => onMotion(m.id)}>
              <span className="preset-name">{m.name}</span>
              <span className="preset-sum">{m.text}</span>
            </button>
          ))}
        </div>
        <p className="block-note">任何强度下，大面积闪烁都不会超过每秒 3 次。</p>
      </section>
      <GpuBlock system={system} />
      <section className="block block-wide">
        <h2 className="block-title">关于</h2>
        <p className="block-text">Prism NR 是开源免费软件（MIT）。它不是 NVIDIA 官方产品，与 NVIDIA 没有关联。DLSS 是 NVIDIA 的商标。</p>
        <p className="block-text">Prism 依赖以下开源项目，并遵守它们各自的许可证：</p>
        <ul className="credits">
          {CREDITS.map((c) => (
            <li key={c.name}>
              <a href={c.url} onClick={(e) => { e.preventDefault(); void api.openExternal(c.url); }}>{c.name}</a>
              <span className="mono dim">{c.license}</span>
            </li>
          ))}
        </ul>
      </section>
    </div>
  );
}

/**
 * Two or more cards: which one runs the network, and whether a second RTX card runs it while the
 * game renders on the first (Offload=, through the frame bridge). Saved at once and written into
 * every enabled game; a running game picks it up within a few seconds.
 */
function GpuBlock({ system }: { system: SystemInfo | null }) {
  const [gpu, setGpu] = useState<{ nrGpu: string; offload: boolean } | null>(null);
  const [note, setNote] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    api.getSettings().then((s) => setGpu(s.gpu)).catch(() => setGpu({ nrGpu: '', offload: false }));
  }, []);

  const gpus = system?.gpus ?? [];
  const rtx = gpus.filter((g) => g.vendor === 'nvidia' && g.nrSupport !== 'none');
  const save = async (next: { nrGpu: string; offload: boolean }) => {
    setGpu(next);
    setError(null);
    try {
      const n = await api.setGpu(next);
      setNote(n ? `已写入 ${n} 个已启用的游戏，游戏运行中几秒内生效。` : '已保存，之后启用的游戏都会用这个设置。');
    } catch (e) {
      setError((e as Error).message);
    }
  };
  if (!gpu) return null;

  return (
    <section className="block block-wide">
      <h2 className="block-title">显卡</h2>
      <p className="block-text">
        {gpus.length > 1
          ? `这台电脑有 ${gpus.length} 块显卡。启用游戏时，Prism NR 会在 Windows 图形设置里把游戏设为高性能 GPU，保证游戏跑在 RTX 卡上；还原时恢复原来的设置。`
          : '这台电脑只有一块显卡，不需要设置。装了第二块显卡以后，这里可以选择用哪块卡跑神经渲染。'}
      </p>

      <h3 className="gpu-sub">神经渲染用哪块卡</h3>
      <div className="presets" role="radiogroup" aria-label="神经渲染用哪块卡">
        <button role="radio" aria-checked={!gpu.nrGpu} className={`preset ${!gpu.nrGpu ? 'is-on' : ''}`} onClick={() => void save({ ...gpu, nrGpu: '' })}>
          <span className="preset-name">自动</span>
          <span className="preset-sum">用显存最大的 RTX 卡；打开副卡运行时，优先用游戏所在的卡以外的另一块 RTX 卡。</span>
        </button>
        {gpus.map((g) => {
          const usable = g.vendor === 'nvidia' && g.nrSupport !== 'none';
          const on = gpu.nrGpu === g.name;
          return (
            <button key={g.name} role="radio" aria-checked={on} disabled={!usable} className={`preset ${on ? 'is-on' : ''}`} onClick={() => void save({ ...gpu, nrGpu: g.name })}>
              <span className="preset-name">{g.name}</span>
              <span className="preset-gpu mono">{g.vramMB ? (g.vramMB >= 1024 ? `${Math.round(g.vramMB / 1024)} GB 显存` : `${g.vramMB} MB 显存`) : '共享内存'}{g.driver ? `  驱动 ${g.driver}` : ''}</span>
              <span className="preset-sum">{usable ? g.expectation : '不能运行神经渲染（需要 NVIDIA RTX 显卡）。'}</span>
            </button>
          );
        })}
      </div>

      <h3 className="gpu-sub">副卡运行（实验性）</h3>
      <div className="presets presets-row" role="radiogroup" aria-label="副卡运行">
        <button role="radio" aria-checked={!gpu.offload} className={`preset ${!gpu.offload ? 'is-on' : ''}`} onClick={() => void save({ ...gpu, offload: false })}>
          <span className="preset-name">关</span>
          <span className="preset-sum">神经渲染和游戏在同一块卡上，延迟最低。</span>
        </button>
        <button role="radio" aria-checked={gpu.offload} disabled={rtx.length < 2 && !gpu.offload} className={`preset ${gpu.offload ? 'is-on' : ''}`} onClick={() => void save({ ...gpu, offload: true })}>
          <span className="preset-name">开</span>
          <span className="preset-sum">游戏在主卡渲染，神经渲染放到第二块 RTX 卡上跑，主卡的帧率损失更小。每帧画面要在两块卡之间经内存搬运，会增加一点延迟。</span>
        </button>
      </div>
      <p className="block-note">
        {rtx.length < 2 ? `副卡运行需要两块 RTX 显卡，这台电脑有 ${rtx.length} 块。` : '打开后，已启用的 DX12 游戏改走副卡；DX9 到 DX11 的游戏本来就由辅助进程处理，会自动用上面选中的卡。'}
      </p>
      {note && <p className="block-note" role="status">{note}</p>}
      {error && <p className="foot-error" role="alert">{error}</p>}
    </section>
  );
}
