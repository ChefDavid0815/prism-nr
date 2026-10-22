// Prism Core scheduling plan: from a GPU, an output resolution and a target frame rate to the
// NETS working scale and whether the focus window fits. See docs/architecture/prism-1.1.md.
// Keep this file free of Node or DOM imports.

/** Where a cost figure comes from. Measured figures replace estimates as Core reports them. */
export type CostSource = 'measured' | 'published' | 'estimated';

/**
 * Network cost of one evaluation: fixedMs + msPerMP x megapixels. Prism's NR bench measured the
 * fixed part directly (RTX 5090 Laptop: 0.90 ms + 1.11 ms/MP with model 310.9.16; 1.45 + 1.96 with
 * the August model), so a smaller
 * working size never saves as much as its pixel count suggests.
 */
export interface NrCost {
  msPerMP: number;
  fixedMs: number;
  source: CostSource;
  note: string;
}

// Model 310.9.16 against the August model on Prism's NR bench (RTX 5090 Laptop, 720p to 4K): 0.56x.
const NEW_MODEL = 0.56;
// TechSpot's 1440p and 4K figures (previous model, desktop cards) give slope and intercept.
const PUBLISHED: Record<string, [number, number]> = { '5090': [0.935, 0.15], '5070': [2.59, 0.64], '5060': [4.54, 0.14] };
// Prism's own measurement (nr_bench, model 310.9.16): used as measured, no model factor.
const MEASURED_LAPTOP: Record<string, [number, number]> = { '5090': [1.109, 0.896] };
// For the rest, the slope scales with the inverse of tensor throughput (NVIDIA's AI TOPS);
// K is fitted to the three published cards (5090 3184, 5070 2668, 5060 2824). The fixed part is
// taken as 0.35 of the slope, between the desktop intercepts and the laptop measurement.
const K = 2900;
const FIXED_PER_SLOPE = 0.35;
const TOPS_DESKTOP: Record<string, number> = {
  '5090': 3352, '5080': 1801, '5070ti': 1406, '5070': 988, '5060ti': 759, '5060': 614, '5050': 421,
};
const TOPS_LAPTOP: Record<string, number> = {
  '5090': 1824, '5080': 1334, '5070ti': 992, '5070': 798, '5060': 572, '5050': 440,
};

/** Network cost for an RTX 50 GPU name, or null when the GPU has no official NR support. */
export function nrCostFor(name: string, laptop: boolean): NrCost | null {
  const m = name.toUpperCase().match(/RTX\s*50(\d0)\s*(TI)?/);
  if (!m) return null;
  const key = `50${m[1]}${m[2] ? 'ti' : ''}`;
  const known = laptop ? MEASURED_LAPTOP[key] : PUBLISHED[key];
  if (known) {
    return {
      msPerMP: laptop ? known[0] : known[0] * NEW_MODEL,
      fixedMs: laptop ? known[1] : known[1] * NEW_MODEL,
      source: laptop ? 'measured' : 'published',
      note: laptop ? 'Prism NR 测试台实测（310.9.16 模型）' : 'TechSpot 实测（旧模型）按测试台实测的新模型提速换算',
    };
  }
  const tops = (laptop ? TOPS_LAPTOP : TOPS_DESKTOP)[key];
  if (!tops) return null;
  const slope = (K / tops) * NEW_MODEL;
  return { msPerMP: slope, fixedMs: slope * FIXED_PER_SLOPE, source: 'estimated', note: '按张量算力从实测卡推算' };
}

export interface PlanInput {
  cost: NrCost;
  outputWidth: number;
  outputHeight: number;
  targetFps: number;
  /** Frame time of the game without neural rendering, if known. */
  baseFrameMs?: number;
  /** Frame generation multiplier; the budget applies to rendered frames. */
  frameGen?: number;
}

export interface NrPlan {
  /** NETS working scale per axis; 1 = native. */
  scale: number;
  focusWindow: { width: number; height: number } | null;
  /** Network plus Prism's own passes, per rendered frame. */
  estimatedMs: number;
  budgetMs: number;
  fits: boolean;
  label: string;
}

export const NETS_TIERS = [1, 0.67, 0.5, 0.35] as const;
const TIER_LABEL: Record<number, string> = { 1: '原生', 0.67: '画质', 0.5: '均衡', 0.35: '流畅' };

/** Prism's own passes (encode, reprojection, accumulation, composite), scaled from ~0.6 ms at 1440p on an RTX 5060. */
function composeMs(cost: NrCost, outMP: number): number {
  return outMP * 0.16 * (cost.msPerMP / 2.3);
}

/** Focus window: about a seventh of the frame at native resolution, 16:9. */
export function focusWindowFor(width: number, height: number): { width: number; height: number } {
  const w = Math.round((width * 0.375) / 8) * 8;
  return { width: w, height: Math.round((w * 9) / 16 / 8) * 8 };
}

/**
 * Picks the best quality that fits the frame budget: the largest NETS scale, then the focus
 * window if there is room left. Without a known base frame time, NR may take 40% of the frame.
 */
export function planNr(input: PlanInput): NrPlan {
  const { cost, outputWidth: w, outputHeight: h } = input;
  const outMP = (w * h) / 1e6;
  const frameMs = 1000 / (input.targetFps / Math.max(1, input.frameGen ?? 1));
  const budgetMs = input.baseFrameMs !== undefined ? Math.max(0, frameMs - input.baseFrameMs) : frameMs * 0.4;
  const fw = focusWindowFor(w, h);
  const fwMs = cost.fixedMs + ((fw.width * fw.height) / 1e6) * cost.msPerMP;
  const compose = composeMs(cost, outMP);

  for (const scale of NETS_TIERS) {
    const netsMs = cost.fixedMs + outMP * scale * scale * cost.msPerMP + (scale < 1 ? compose : 0);
    if (netsMs > budgetMs) continue;
    const withWindow = scale < 1 && netsMs + fwMs <= budgetMs;
    return {
      scale,
      focusWindow: withWindow ? fw : null,
      estimatedMs: +(netsMs + (withWindow ? fwMs : 0)).toFixed(2),
      budgetMs: +budgetMs.toFixed(2),
      fits: true,
      label: TIER_LABEL[scale] + (withWindow ? ' + 焦点窗口' : ''),
    };
  }
  const last = NETS_TIERS[NETS_TIERS.length - 1];
  return {
    scale: last,
    focusWindow: null,
    estimatedMs: +(cost.fixedMs + outMP * last * last * cost.msPerMP + compose).toFixed(2),
    budgetMs: +budgetMs.toFixed(2),
    fits: false,
    label: TIER_LABEL[last],
  };
}
