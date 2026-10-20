/*
 * PR-12 Prism Stabilize
 * Removes frame-to-frame flicker and detail shimmer introduced by neural rendering.
 * History is clipped to the current pixel's neighbourhood (variance clipping), so
 * moving content does not ghost. With PRISM_MOTION_VECTORS=1 the history is
 * reprojected with a shared texMotionVectors texture (iMMERSE / Launchpad layout).
 * MIT License, Prism NR.
 */

#include "Prism_Common.fxh"

#ifndef PRISM_MOTION_VECTORS
	#define PRISM_MOTION_VECTORS 0
#endif

uniform float StabilizeAmount <
	ui_type = "slider"; ui_min = 0.0; ui_max = 0.9; ui_step = 0.01;
	ui_label = "Stabilize amount";
	ui_tooltip = "Weight of the previous frames. Higher is steadier; too high can smear motion.";
> = 0.55;

uniform float ClipTightness <
	ui_type = "slider"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.01;
	ui_label = "Neighbourhood clip";
	ui_tooltip = "Lower clips history harder (less ghosting, less smoothing).";
> = 1.0;

texture PrismHistoryTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler PrismHistory { Texture = PrismHistoryTex; };

#if PRISM_MOTION_VECTORS
texture texMotionVectors { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
sampler sMotionVectors { Texture = texMotionVectors; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };
#endif

float4 PS_PrismStabilize(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float2 p = PRISM_PIXEL;
	float4 curRaw = tex2D(PrismBackBuffer, uv);
	float3 cur = PrismToYCoCg(PrismEncode(curRaw.rgb));

	// Neighbourhood statistics in YCoCg.
	float3 m1 = cur;
	float3 m2 = cur * cur;
	[unroll]
	for (int y = -1; y <= 1; y++)
	{
		[unroll]
		for (int x = -1; x <= 1; x++)
		{
			if (x == 0 && y == 0)
				continue;
			float3 s = PrismToYCoCg(PrismEncode(tex2D(PrismBackBuffer, uv + float2(x, y) * p).rgb));
			m1 += s;
			m2 += s * s;
		}
	}
	float3 mean = m1 / 9.0;
	float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
	float3 boxMin = mean - sigma * ClipTightness;
	float3 boxMax = mean + sigma * ClipTightness;

	float2 prevUv = uv;
#if PRISM_MOTION_VECTORS
	prevUv += tex2D(sMotionVectors, uv).xy;
#endif
	float validHistory = all(prevUv >= 0.0) && all(prevUv <= 1.0) ? 1.0 : 0.0;

	float3 hist = PrismToYCoCg(PrismEncode(tex2D(PrismHistory, prevUv).rgb));
	float3 clipped = clamp(hist, boxMin, boxMax);

	// How far the history had to move tells us it was stale; reduce its weight there.
	float clipDistance = length(hist - clipped);
	float weight = StabilizeAmount * validHistory * saturate(1.0 - clipDistance * 12.0);

	float3 result = lerp(cur, clipped, weight);
	return float4(PrismDecode(saturate(PrismFromYCoCg(result))), curRaw.a);
}

float4 PS_PrismStoreHistory(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return tex2D(PrismBackBuffer, uv);
}

technique PrismStabilize <
	ui_label = "PR-12 Prism Stabilize";
	ui_tooltip = "Removes flicker and shimmer from neural rendering. Place after the neural rendering effect.";
>
{
	pass Stabilize
	{
		VertexShader = PrismVS;
		PixelShader = PS_PrismStabilize;
	}
	pass StoreHistory
	{
		VertexShader = PrismVS;
		PixelShader = PS_PrismStoreHistory;
		RenderTarget = PrismHistoryTex;
	}
}
