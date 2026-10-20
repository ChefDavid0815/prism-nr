/*
 * PR-11 Prism Guard
 * Keeps neural rendering an enhancement instead of a replacement:
 *  - limits how far each pixel's luma and chroma may move away from the original,
 *  - re-injects the original's high-frequency detail (counters the airbrushed look),
 *  - stops dark areas from being lifted,
 *  - keeps user-marked HUD rectangles untouched.
 * Needs PR-10 Prism Reference placed before the neural rendering effect.
 * MIT License, Prism NR.
 */

#include "Prism_Common.fxh"

uniform float GuardStrength <
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_category = "Guard";
	ui_label = "Guard strength";
	ui_tooltip = "0 leaves neural rendering untouched, 1 holds every change inside the limits below.";
> = 0.6;

uniform float MaxLumaShift <
	ui_type = "slider"; ui_min = 0.02; ui_max = 0.5; ui_step = 0.005;
	ui_category = "Guard";
	ui_label = "Max brightness change";
> = 0.12;

uniform float MaxChromaShift <
	ui_type = "slider"; ui_min = 0.01; ui_max = 0.3; ui_step = 0.005;
	ui_category = "Guard";
	ui_label = "Max colour change";
> = 0.06;

uniform float DetailTransfer <
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_category = "Detail";
	ui_label = "Original detail";
	ui_tooltip = "Adds the original texture detail back on top of the neural lighting.";
> = 0.35;

uniform float ShadowProtect <
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_category = "Detail";
	ui_label = "Shadow protection";
	ui_tooltip = "Prevents neural rendering from brightening dark areas.";
> = 0.6;

uniform bool HudProtect <
	ui_category = "HUD";
	ui_label = "Protect HUD rectangles";
> = true;

uniform float4 HudRect0 < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.001; ui_category = "HUD"; ui_label = "HUD rect 1 (x, y, w, h)"; > = float4(0.0, 0.0, 0.0, 0.0);
uniform float4 HudRect1 < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.001; ui_category = "HUD"; ui_label = "HUD rect 2 (x, y, w, h)"; > = float4(0.0, 0.0, 0.0, 0.0);
uniform float4 HudRect2 < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.001; ui_category = "HUD"; ui_label = "HUD rect 3 (x, y, w, h)"; > = float4(0.0, 0.0, 0.0, 0.0);
uniform float4 HudRect3 < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.001; ui_category = "HUD"; ui_label = "HUD rect 4 (x, y, w, h)"; > = float4(0.0, 0.0, 0.0, 0.0);

uniform bool ShowDifference <
	ui_category = "Debug";
	ui_label = "Show what neural rendering changed";
> = false;

bool InRect(float2 uv, float4 r)
{
	return r.z > 0.0 && r.w > 0.0 && all(uv >= r.xy) && all(uv <= r.xy + r.zw);
}

// Luma of a 3x3 tent blur, used to split low and high frequencies.
float BlurLuma(sampler s, float2 uv)
{
	float2 p = PRISM_PIXEL;
	float sum = PrismToYCoCg(PrismEncode(tex2D(s, uv).rgb)).x * 4.0;
	sum += PrismToYCoCg(PrismEncode(tex2D(s, uv + float2( p.x, 0.0)).rgb)).x * 2.0;
	sum += PrismToYCoCg(PrismEncode(tex2D(s, uv + float2(-p.x, 0.0)).rgb)).x * 2.0;
	sum += PrismToYCoCg(PrismEncode(tex2D(s, uv + float2(0.0,  p.y)).rgb)).x * 2.0;
	sum += PrismToYCoCg(PrismEncode(tex2D(s, uv + float2(0.0, -p.y)).rgb)).x * 2.0;
	sum += PrismToYCoCg(PrismEncode(tex2D(s, uv + p).rgb)).x;
	sum += PrismToYCoCg(PrismEncode(tex2D(s, uv - p).rgb)).x;
	sum += PrismToYCoCg(PrismEncode(tex2D(s, uv + float2(p.x, -p.y)).rgb)).x;
	sum += PrismToYCoCg(PrismEncode(tex2D(s, uv + float2(-p.x, p.y)).rgb)).x;
	return sum / 16.0;
}

float4 PS_PrismGuard(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float4 curRaw = tex2D(PrismBackBuffer, uv);
	float3 refRaw = tex2D(PrismReference, uv).rgb;

	if (HudProtect && (InRect(uv, HudRect0) || InRect(uv, HudRect1) || InRect(uv, HudRect2) || InRect(uv, HudRect3)))
		return float4(refRaw, curRaw.a);

	float3 cur = PrismToYCoCg(PrismEncode(curRaw.rgb));
	float3 ref = PrismToYCoCg(PrismEncode(refRaw));

	if (ShowDifference)
	{
		float d = abs(cur.x - ref.x) * 6.0 + length(cur.yz - ref.yz) * 10.0;
		return float4(d.xxx, 1.0);
	}

	// Luma: limited change, with extra resistance to brightening shadows.
	float dY = cur.x - ref.x;
	if (dY > 0.0)
	{
		float darkness = smoothstep(0.28, 0.02, ref.x);
		dY *= lerp(1.0, 1.0 - ShadowProtect, darkness);
	}
	float limitedY = ref.x + PrismSoftLimit(dY, MaxLumaShift);

	// Chroma: limited as a 2D vector so hue and saturation stay close to the original.
	float2 limitedC = ref.yz + PrismSoftLimit2(cur.yz - ref.yz, MaxChromaShift);

	float3 guarded = float3(limitedY, limitedC);
	float3 result = lerp(cur, guarded, GuardStrength);

	// Frequency split: keep the neural low frequencies, restore the original's lost detail.
	float refHF = ref.x - BlurLuma(PrismReference, uv);
	float curHF = cur.x - BlurLuma(PrismBackBuffer, uv);
	float lost = refHF - curHF;
	// Only restore where the original had more detail than the neural output.
	float lossMask = saturate((abs(refHF) - abs(curHF)) * 40.0);
	result.x += lost * DetailTransfer * lossMask;

	return float4(PrismDecode(saturate(PrismFromYCoCg(result))), curRaw.a);
}

technique PrismGuard <
	ui_label = "PR-11 Prism Guard";
	ui_tooltip = "Limits how much neural rendering may change each pixel, restores original detail, protects shadows and HUD.\nPlace after the neural rendering effect. Needs PR-10 Prism Reference before it.";
>
{
	pass
	{
		VertexShader = PrismVS;
		PixelShader = PS_PrismGuard;
	}
}
