/*
 * PR-14 Prism Tone
 * Keeps black level, highlights and saturation in check after neural rendering.
 * Works on luma and scales RGB by the luma ratio, so hue is preserved.
 * MIT License, Prism NR.
 */

#include "Prism_Common.fxh"

uniform float BlackAnchor <
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_label = "Black anchor";
	ui_tooltip = "Pushes lifted, greyish shadows back down.";
> = 0.5;

uniform float BlackThreshold <
	ui_type = "slider"; ui_min = 0.02; ui_max = 0.2; ui_step = 0.005;
	ui_label = "Black threshold";
> = 0.08;

uniform float HighlightRolloff <
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_label = "Highlight rolloff";
> = 0.35;

uniform float SaturationGuard <
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_label = "Saturation guard";
	ui_tooltip = "Pulls only over-saturated colours back toward neutral.";
> = 0.4;

float4 PS_PrismTone(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float4 raw = tex2D(PrismBackBuffer, uv);
	float3 c = PrismEncode(raw.rgb);
	float y = PrismLuma(c);

	// Toe: below the threshold, bend luma toward a deeper curve.
	float t = BlackThreshold;
	float toe = (y < t) ? lerp(y, y * y / t, BlackAnchor) : y;

	// Shoulder: compress above the knee.
	float knee = lerp(1.0, 0.7, HighlightRolloff);
	float shoulder = toe;
	if (toe > knee)
	{
		float over = toe - knee;
		float range = 1.0 - knee;
		shoulder = knee + over / (1.0 + over / max(range, 1e-4));
	}
	c *= (y > 1e-5) ? shoulder / y : 1.0;

	// Saturation guard: compress chroma above 0.7 saturation.
	float mx = max(c.r, max(c.g, c.b));
	float mn = min(c.r, min(c.g, c.b));
	float sat = (mx > 1e-4) ? (mx - mn) / mx : 0.0;
	float excess = saturate((sat - 0.7) / 0.3);
	float yc = PrismLuma(c);
	c = lerp(c, yc + (c - yc) * 0.82, excess * SaturationGuard);

	return float4(PrismDecode(saturate(c)), raw.a);
}

technique PrismTone <
	ui_label = "PR-14 Prism Tone";
	ui_tooltip = "Anchors black level, rolls off highlights and guards saturation after neural rendering.";
>
{
	pass
	{
		VertexShader = PrismVS;
		PixelShader = PS_PrismTone;
	}
}
