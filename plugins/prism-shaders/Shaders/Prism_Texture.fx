/*
 * PR-13 Prism Texture
 * Brings back micro-detail and grain on over-smoothed neural output.
 *  - Contrast-adaptive sharpening on luma, clamped to the local min/max (no halos).
 *  - Luma-weighted grain: none in deep shadows and highlights, most in mid-tones.
 * MIT License, Prism NR.
 */

#include "Prism_Common.fxh"

uniform float DetailAmount <
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_label = "Micro detail";
	ui_tooltip = "Lower-resolution neural rendering looks soft; raise this to compensate.";
> = 0.25;

uniform float GrainAmount <
	ui_type = "slider"; ui_min = 0.0; ui_max = 0.5; ui_step = 0.005;
	ui_label = "Grain";
	ui_tooltip = "A little grain removes much of the plastic look.";
> = 0.08;

uniform float GrainSize <
	ui_type = "slider"; ui_min = 1.0; ui_max = 3.0; ui_step = 0.1;
	ui_label = "Grain size";
> = 1.4;

uniform int PrismFrame < source = "framecount"; >;

float4 PS_PrismTexture(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float2 p = PRISM_PIXEL;
	float4 raw = tex2D(PrismBackBuffer, uv);
	float3 c = PrismEncode(raw.rgb);

	float3 n = PrismEncode(tex2D(PrismBackBuffer, uv + float2(0.0, -p.y)).rgb);
	float3 s = PrismEncode(tex2D(PrismBackBuffer, uv + float2(0.0,  p.y)).rgb);
	float3 e = PrismEncode(tex2D(PrismBackBuffer, uv + float2( p.x, 0.0)).rgb);
	float3 w = PrismEncode(tex2D(PrismBackBuffer, uv + float2(-p.x, 0.0)).rgb);

	float lc = PrismLuma(c);
	float ln = PrismLuma(n), ls = PrismLuma(s), le = PrismLuma(e), lw = PrismLuma(w);
	float mn = min(lc, min(min(ln, ls), min(le, lw)));
	float mx = max(lc, max(max(ln, ls), max(le, lw)));

	// CAS-style amount: strong in flat-ish texture, weak near hard edges.
	float headroom = min(mn, 1.0 - mx);
	float amp = sqrt(saturate(headroom / max(mx, 1e-4)));
	float k = -amp * lerp(0.0, 0.2, DetailAmount);
	float sharpened = (lc + k * (ln + ls + le + lw)) / (1.0 + 4.0 * k);
	sharpened = clamp(sharpened, mn, mx);
	c *= (lc > 1e-4) ? sharpened / lc : 1.0;

	// Grain on a coarser grid, re-seeded every frame.
	float2 cell = floor(pos.xy / GrainSize);
	float g = PrismHash(cell + float2(PrismFrame % 977, PrismFrame % 613)) - 0.5;
	float mid = 4.0 * lc * (1.0 - lc);
	c += g * GrainAmount * 0.25 * mid;

	return float4(PrismDecode(saturate(c)), raw.a);
}

technique PrismTexture <
	ui_label = "PR-13 Prism Texture";
	ui_tooltip = "Restores micro detail and adds fine grain to remove the waxy look of neural rendering.";
>
{
	pass
	{
		VertexShader = PrismVS;
		PixelShader = PS_PrismTexture;
	}
}
