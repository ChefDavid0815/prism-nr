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

export function SettingsView({ motion, onMotion }: { motion: MotionLevel; onMotion: (m: MotionLevel) => void }) {
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
