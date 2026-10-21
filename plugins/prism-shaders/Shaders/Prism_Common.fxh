/*
 * Prism NR - shared definitions for the Prism shader pack.
 * MIT License. https://github.com/ (Prism NR project)
 *
 * Textures declared here are shared between every Prism effect file: ReShade
 * shares a texture across effects when its name and description are identical.
 */

#pragma once

// Self-contained: no dependency on ReShade.fxh, so the pack works next to any
// other shader collection without version clashes.

#ifndef BUFFER_COLOR_SPACE
	#define BUFFER_COLOR_SPACE 1
#endif

#define PRISM_PIXEL float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT)

texture PrismBackBufferTex : COLOR;
sampler PrismBackBuffer { Texture = PrismBackBufferTex; };

// Full-screen triangle.
void PrismVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// Frame copied by Prism_Capture before neural rendering (Feeder route) or before
// the Prism chain (native routes).
texture PrismReferenceTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler PrismReference { Texture = PrismReferenceTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };

// ---------------------------------------------------------------------------
// Working space
// SDR and HDR10 PQ are already perceptual in [0,1]. scRGB is linear and can go
// far above 1, so it is compressed with x/(1+x) before any perceptual limit.
// ---------------------------------------------------------------------------
float3 PrismEncode(float3 c)
{
#if BUFFER_COLOR_SPACE == 2
	c = max(c, 0.0);
	return c / (1.0 + c);
#else
	return saturate(c);
#endif
}

float3 PrismDecode(float3 e)
{
#if BUFFER_COLOR_SPACE == 2
	e = min(e, 0.9995);
	return e / (1.0 - e);
#else
	return e;
#endif
}

float3 PrismToYCoCg(float3 c)
{
	return float3(
		dot(c, float3(0.25, 0.5, 0.25)),
		dot(c, float3(0.5, 0.0, -0.5)),
		dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 PrismFromYCoCg(float3 y)
{
	return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z);
}

float PrismLuma(float3 c)
{
	return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Smooth limiter: behaves like identity for small d and approaches +-lim.
float PrismSoftLimit(float d, float lim)
{
	float x = clamp(d / max(lim, 1e-5), -8.0, 8.0);
	float e = exp(2.0 * x);
	return lim * (e - 1.0) / (e + 1.0);
}

float2 PrismSoftLimit2(float2 d, float lim)
{
	float len = length(d);
	float scale = len < 1e-6 ? 1.0 : PrismSoftLimit(len, lim) / max(len, 1e-6);
	return d * scale;
}

float PrismHash(float2 p)
{
	float3 p3 = frac(float3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return frac((p3.x + p3.y) * p3.z);
}
