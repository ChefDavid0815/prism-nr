/*
 * PR-10 Prism Reference
 * Copies the current frame into PrismReferenceTex. Place it before the neural
 * rendering effect (Feeder route) or first in the chain (native routes).
 * MIT License, Prism NR.
 */

#include "Prism_Common.fxh"

float4 PS_PrismCapture(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return tex2D(PrismBackBuffer, uv);
}

technique PrismCapture <
	ui_label = "PR-10 Prism Reference";
	ui_tooltip = "Saves the frame before neural rendering so Prism Guard and Prism Compare have an original to work against.\nKeep it above the neural rendering effect.";
>
{
	pass
	{
		VertexShader = PrismVS;
		PixelShader = PS_PrismCapture;
		RenderTarget = PrismReferenceTex;
	}
}
