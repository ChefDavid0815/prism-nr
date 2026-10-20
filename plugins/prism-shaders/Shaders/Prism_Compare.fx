/*
 * PR-15 Prism Compare
 * Split view: left of the line is the PR-10 reference, right is the final image.
 * The divider carries a thin barcode so it stays visible on any content.
 * MIT License, Prism NR.
 */

#include "Prism_Common.fxh"

uniform float SplitPosition <
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.001;
	ui_label = "Split position";
> = 0.5;

uniform bool SwapSides <
	ui_label = "Swap sides";
> = false;

float4 PS_PrismCompare(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float4 final = tex2D(PrismBackBuffer, uv);
	float3 original = tex2D(PrismReference, uv).rgb;
	bool left = uv.x < SplitPosition;
	if (SwapSides)
		left = !left;
	float4 color = left ? float4(original, final.a) : final;

	float px = abs(uv.x - SplitPosition) * BUFFER_WIDTH;
	if (px < 1.0)
		color.rgb = 1.0;
	else if (px < 4.0)
	{
		// Barcode strip beside the divider.
		float bar = step(0.5, PrismHash(float2(floor(pos.y / 3.0), 7.0)));
		color.rgb = bar;
	}
	return color;
}

technique PrismCompare <
	ui_label = "PR-15 Prism Compare";
	ui_tooltip = "Left: reference before neural rendering. Right: final image. Keep it last in the list.";
	enabled = false;
>
{
	pass
	{
		VertexShader = PrismVS;
		PixelShader = PS_PrismCompare;
	}
}
