// NAKA Title Designer Pro - fill / stroke surface pass: bevel shading and lit materials in 2D.
// The bevel height comes from the distance to the layer's edge, found with a jump flood over the
// pixels outside the layer. Compiled at build time with fxc, see CMakeLists.txt.

#include "tdp_light.hlsli"

cbuffer Surface : register(b0)
{
	float4 view;    // output width, height, camera distance, surroundings (0 soft, 1 studio)
	float4 bevel;   // width px, height px, shape (0 none, 1 flat, 2 round, 3 cove), alpha threshold
	float4 light;   // direction towards the light, intensity
	float4 camera;  // camera position, ambient
	float4 mat;     // metallic, roughness, brushed, lit (1) / flat colour (0)
	float4 jump;    // jump flood step px; blur direction (yz)
	float4 rot[3];  // turn of the 3D layer behind this one (rows), identity in 2D
	float4 bump;    // bump height px for white, bump map present (1)
};

float3 Turn(float3 n) { return normalize(float3(dot(rot[0].xyz, n), dot(rot[1].xyz, n), dot(rot[2].xyz, n))); }

Texture2D colorTex : register(t0);  // the layer, premultiplied
Texture2D seedTex : register(t1);   // nearest outside pixel (xy), x < -1e4 = none yet
Texture2D bumpTex : register(t2);   // bump map (premultiplied texture drawn in output pixels), when bump.y > 0

float4 VSFull(uint id : SV_VertexID) : SV_Position
{
	const float2 t = float2((id << 1) & 2, id & 2);
	return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSSeed(float4 pos : SV_Position) : SV_Target
{
	const float a = colorTex.Load(int3(pos.xy, 0)).a;
	return a < bevel.w ? float4(pos.xy, 0, 0) : float4(-1e5, -1e5, 0, 0);
}

float4 PSJump(float4 pos : SV_Position) : SV_Target
{
	const float2 p = pos.xy;
	float2 best = seedTex.Load(int3(p, 0)).xy;
	float bestD = best.x < -1e4 ? 1e10 : dot(p - best, p - best);
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x) {
			const float2 q = p + float2(x, y) * jump.x;
			if (q.x < 0 || q.y < 0 || q.x >= view.x || q.y >= view.y) continue;
			const float2 s = seedTex.Load(int3(q, 0)).xy;
			if (s.x < -1e4) continue;
			const float d = dot(p - s, p - s);
			if (d < bestD) { bestD = d; best = s; }
		}
	return float4(best, 0, 0);
}

// Bevel height from the distance to the nearest outside pixel (seedTex), 0 outside the layer.
float4 PSHeight(float4 pos : SV_Position) : SV_Target
{
	const float2 p = pos.xy;
	if (colorTex.Load(int3(p, 0)).a < bevel.w) return float4(0, 0, 0, 0);
	const float2 s = seedTex.Load(int3(p, 0)).xy;
	const float dist = s.x < -1e4 ? 1e5 : length(p - s);
	const float t = saturate((dist - 0.5) / max(bevel.x, 1e-3));
	float h;
	if (bevel.z < 1.5) h = t;                          // flat
	else if (bevel.z < 2.5) h = sin(t * PI * 0.5);     // round
	else h = 1 - cos(t * PI * 0.5);                    // cove
	return float4(h * bevel.y, 0, 0, 0);
}

// Binomial blur of the height along jump.yz, which hides the pixel steps of the distance.
float4 PSBlur(float4 pos : SV_Position) : SV_Target
{
	const float w[7] = { 1, 6, 15, 20, 15, 6, 1 };
	float h = 0;
	for (int i = 0; i < 7; ++i) h += w[i] * seedTex.Load(int3(pos.xy + jump.yz * (i - 3), 0)).x;
	return float4(h / 64, 0, 0, 0);
}

float Hash(float2 p)
{
	return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float HeightAt(float2 p) { return seedTex.Load(int3(p, 0)).x; } // the height pass output is bound as t1

// Brightness of the bump map (transparent = dark).
float BumpAt(float2 p)
{
	const float3 c = bumpTex.Load(int3(clamp(p, float2(0, 0), view.xy - 1), 0)).rgb;
	return dot(c, float3(0.299, 0.587, 0.114));
}

float4 PSShade(float4 pos : SV_Position) : SV_Target
{
	const float2 p = pos.xy;
	const float4 c = colorTex.Load(int3(p, 0));
	if (c.a <= 0.0) return float4(0, 0, 0, 0);
	float3 base = ToLinear(c.rgb / c.a);

	// Surface normal from the height (Sobel, which also smooths the pixel steps of the distance).
	float hx = 0, hy = 0;
	if (bevel.z > 0.5) {
		const float a = HeightAt(p + float2(-1, -1)), b = HeightAt(p + float2(0, -1)), cc = HeightAt(p + float2(1, -1));
		const float d = HeightAt(p + float2(-1, 0)), f = HeightAt(p + float2(1, 0));
		const float g = HeightAt(p + float2(-1, 1)), h = HeightAt(p + float2(0, 1)), i = HeightAt(p + float2(1, 1));
		hx = ((cc + 2 * f + i) - (a + 2 * d + g)) / 8;
		hy = ((g + 2 * h + i) - (a + 2 * b + cc)) / 8;
	}
	if (bump.y > 0.5) {
		const float a = BumpAt(p + float2(-1, -1)), b = BumpAt(p + float2(0, -1)), cc = BumpAt(p + float2(1, -1));
		const float d = BumpAt(p + float2(-1, 0)), f = BumpAt(p + float2(1, 0));
		const float g = BumpAt(p + float2(-1, 1)), h = BumpAt(p + float2(0, 1)), i = BumpAt(p + float2(1, 1));
		hx += bump.x * ((cc + 2 * f + i) - (a + 2 * d + g)) / 8;
		hy += bump.x * ((g + 2 * h + i) - (a + 2 * b + cc)) / 8;
	}
	const float3 N = Turn(float3(-hx, -hy, -1));
	const float3 N0 = Turn(float3(0, 0, -1));
	const float3 P = float3(p, 0);
	const float3 V = normalize(camera.xyz - P);
	const float3 L = light.xyz;

	float rough = mat.y;
	if (mat.z > 0.5) {
		// Hairline: fine horizontal streaks of varying brightness.
		const float2 cell = float2(floor(p.x / 180.0), p.y);
		const float n = lerp(Hash(float2(cell.x, floor(cell.y))), Hash(float2(cell.x + 1, floor(cell.y))), frac(p.x / 180.0));
		base *= lerp(0.8, 1.12, n);
		rough = saturate(rough + 0.08);
	}

	float3 col;
	if (mat.w > 0.5) {
		col = Shade(base, N, V, L, light.w, camera.w, mat.x, rough, view.w);
	} else {
		// Flat colour: only the change the bevel brings to plain shading.
		const float3 grey = float3(0.5, 0.5, 0.5);
		const float3 lit = Shade(grey, N, V, L, light.w, camera.w, 0, 0.4, view.w);
		const float3 flat = Shade(grey, N0, V, L, light.w, camera.w, 0, 0.4, view.w);
		col = base * lit / max(flat, 0.05);
	}
	return float4(ToSrgb(col) * c.a, c.a);
}
