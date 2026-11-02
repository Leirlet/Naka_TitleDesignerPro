// NAKA Title Designer Pro - 3D layer (extrusion / bevel) shading.
// Compiled at build time with fxc (vs_4_0 / ps_4_0), see CMakeLists.txt.
// World space = output pixels: x right, y down, z away from the viewer.

#include "tdp_light.hlsli"

cbuffer Scene : register(b0)
{
	float4 view;    // output width, height, camera distance, surroundings (0 soft, 1 studio)
	float4 depthK;  // clip z = depthK.x * w + depthK.y
	float4 light;   // direction towards the light, intensity
	float4 camera;  // camera position, ambient
	float4 side1;   // side colour at the front (linear), metallic
	float4 side2;   // side colour at the back (linear), roughness
};

Texture2D frontTex : register(t0); // the layers in front of the 3D layer, premultiplied, output pixels
SamplerState samp : register(s0);

struct VSIn {
	float3 pos : POSITION;
	float3 nrm : NORMAL;
	float2 uv : TEXCOORD0;
	float2 kt : TEXCOORD1; // kind (0 front, 1 bevel, 2 side, 3 back), side gradient 0..1
};

struct PSIn {
	float4 pos : SV_Position;
	float3 world : TEXCOORD0;
	float3 nrm : TEXCOORD1;
	float2 uv : TEXCOORD2;
	float2 kt : TEXCOORD3;
};

PSIn VSMain(VSIn i)
{
	PSIn o;
	// Pinhole camera in front of the frame centre; w = 1 on the text plane.
	const float w = (view.z + i.pos.z) / view.z;
	o.pos = float4((i.pos.x - view.x * 0.5) / (view.x * 0.5), -(i.pos.y - view.y * 0.5) / (view.y * 0.5),
		depthK.x * w + depthK.y, w);
	o.world = i.pos;
	o.nrm = i.nrm;
	o.uv = i.uv;
	o.kt = i.kt;
	return o;
}

float4 PSMain(PSIn i) : SV_Target
{
	const float kind = i.kt.x;
	float3 base;
	if (kind < 1.5) {
		const float4 t = frontTex.Sample(samp, i.uv);
		const bool inked = t.a > 0.004;
		// Front face: the 2D look as it is.
		if (kind < 0.5) return float4(inked ? t.rgb / t.a : ToSrgb(side1.rgb), 1);
		base = inked ? ToLinear(t.rgb / t.a) : side1.rgb;
	} else {
		base = lerp(side1.rgb, side2.rgb, saturate(i.kt.y));
	}

	float3 N = normalize(i.nrm);
	const float3 V = normalize(camera.xyz - i.world);
	if (dot(N, V) < 0) N = -N;
	return float4(ToSrgb(Shade(base, N, V, light.xyz, light.w, camera.w, side1.w, side2.w, view.w)), 1);
}
