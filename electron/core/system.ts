import * as os from 'node:os';
import type { GpuArch, GpuInfo, GpuTier, GpuVendor, NrSupport, SystemInfo } from '../shared/types';
import { log, psJson, run } from './util';

interface RawGpu {
  name: string;
  driver: string | null;
  vramMB: number | null;
}

/** Classifies a GPU name into architecture, tier and DLSS 5 expectation. */
export function classifyGpu(raw: RawGpu): GpuInfo {
  const name = raw.name.trim();
  const n = name.toUpperCase();
  const vendor: GpuVendor = /NVIDIA|GEFORCE|RTX|GTX|QUADRO/.test(n) ? 'nvidia' : /AMD|RADEON/.test(n) ? 'amd' : /INTEL|ARC/.test(n) ? 'intel' : 'unknown';
  const laptop = /LAPTOP|MOBILE|MAX-Q/.test(n);
  const m = n.match(/RTX\s*(?:PRO\s*)?(\d{2})(\d{2})\s*(TI)?/);
  let arch: GpuArch = 'other';
  let tier: GpuTier = 'unsupported';
  let nrSupport: NrSupport = 'none';
  let expectation = '';

  if (vendor === 'nvidia' && m) {
    const series = Number(m[1]);
    const model = Number(m[2]);
    const ti = Boolean(m[3]);
    arch = series === 50 ? 'blackwell' : series === 40 ? 'ada' : series === 30 ? 'ampere' : series === 20 ? 'turing' : 'other';
    if (arch === 'blackwell') {
      nrSupport = 'official';
      // Laptop parts sit roughly one class below the desktop part with the same number.
      const key = `${model}${ti ? 'ti' : ''}`;
      const desktop: Record<string, GpuTier> = { '90': 'flagship', '80': 'flagship', '70ti': 'high', '70': 'mainstream', '60ti': 'mainstream', '60': 'entry', '50': 'entry' };
      const mobile: Record<string, GpuTier> = { '90': 'flagship', '80': 'high', '70ti': 'mainstream', '70': 'entry', '60': 'entry', '50': 'entry' };
      tier = (laptop ? mobile : desktop)[key] ?? 'entry';
      expectation = {
        flagship: '可以开满模型分辨率，追求最好画质。',
        high: '建议把模型分辨率放在 67% 到 100% 之间。',
        mainstream: '建议用 Pre-SR 路线、67% 模型分辨率，配合帧生成。',
        entry: '建议用 Pre-SR 路线、50% 模型分辨率，配合多帧生成保证流畅。',
        legacy: '',
        unsupported: '',
      }[tier];
    } else if (arch === 'ada') {
      nrSupport = 'experimental';
      tier = 'legacy';
      expectation = 'NVIDIA 已确认会支持 RTX 40，但还没有日期。社区适配版能跑，开销明显更大。';
    } else if (arch === 'ampere' || arch === 'turing') {
      nrSupport = 'experimental';
      tier = 'legacy';
      expectation = '没有官方支持。社区改版能运行，但帧率通常会大幅下降，不推荐。';
    } else {
      expectation = '这张显卡不支持 DLSS 5 神经渲染。';
    }
  } else if (vendor === 'amd' || vendor === 'intel') {
    expectation = 'DLSS 5 神经渲染依赖 NVIDIA RTX 硬件，这张显卡无法运行。Prism 的画质插件仍可单独使用。';
  } else {
    expectation = '没有识别出支持神经渲染的显卡。';
  }

  return { name, vendor, arch, laptop, vramMB: raw.vramMB, driver: raw.driver, tier, nrSupport, expectation };
}

async function queryNvidiaSmi(): Promise<RawGpu[]> {
  const r = await run('nvidia-smi', ['--query-gpu=name,driver_version,memory.total', '--format=csv,noheader,nounits']);
  if (r.code !== 0 || !r.stdout.trim()) return [];
  return r.stdout
    .trim()
    .split(/\r?\n/)
    .map((line) => {
      const [name, driver, mem] = line.split(',').map((s) => s.trim());
      return { name, driver: driver || null, vramMB: Number.isFinite(Number(mem)) ? Number(mem) : null };
    });
}

async function queryCim(): Promise<RawGpu[]> {
  const rows = await psJson<{ Name: string; DriverVersion: string; AdapterRAM: number | null } | { Name: string; DriverVersion: string; AdapterRAM: number | null }[]>(
    'Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM | ConvertTo-Json -Compress',
  );
  if (!rows) return [];
  const list = Array.isArray(rows) ? rows : [rows];
  return list.map((r) => ({ name: r.Name, driver: r.DriverVersion ?? null, vramMB: r.AdapterRAM ? Math.round(r.AdapterRAM / 1048576) : null }));
}

const tierRank: Record<GpuTier, number> = { flagship: 5, high: 4, mainstream: 3, entry: 2, legacy: 1, unsupported: 0 };

export async function detectSystem(): Promise<SystemInfo> {
  let raw = await queryNvidiaSmi();
  const others = (await queryCim()).filter((g) => !/NVIDIA/i.test(g.name));
  if (!raw.length) raw = await queryCim();
  else raw = raw.concat(others);
  const gpus = raw.map(classifyGpu);
  const primary = [...gpus].sort((a, b) => tierRank[b.tier] - tierRank[a.tier])[0] ?? null;

  const release = os.release();
  const build = Number(release.split('.')[2] ?? 0);
  const isWin11 = build >= 22000;
  const notes: string[] = [];
  if (!isWin11) notes.push('建议使用 Windows 11。部分社区组件在 Windows 10 上不稳定。');
  if (primary?.vendor === 'nvidia' && primary.driver) {
    const major = Number(primary.driver.split('.')[0]);
    if (primary.arch === 'blackwell' && major < 610) notes.push(`驱动 ${primary.driver} 偏旧，建议升级到 616 以上。`);
  }
  if (!primary || primary.nrSupport === 'none') notes.push('没有找到可以运行神经渲染的显卡，只有画质类插件可用。');
  const nvidiaCount = gpus.filter((g) => g.arch === 'blackwell').length;
  if (nvidiaCount >= 2) notes.push('检测到两块 RTX 50 显卡，可以尝试双卡分担。');

  log(`system: ${gpus.map((g) => `${g.name} [${g.tier}]`).join(', ')} build ${build}`);
  return { gpus, primary, os: { release, build, isWin11 }, notes };
}
