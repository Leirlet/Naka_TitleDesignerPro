// NAKA Title Designer Pro - lighting shared by the 3D layer and the fill / stroke surface pass.
// World space = output pixels: x right, y down, z away from the viewer.

static const float PI = 3.14159265;

float3 ToLinear(float3 c) { return pow(saturate(c), 2.2); }
float3 ToSrgb(float3 c) { return pow(saturate(c), 1.0 / 2.2); }

float Band(float x, float centre, float width)
{
	const float d = (x - centre) / width;
	return exp(-d * d);
}

// Soft surroundings (the first Phase 3 look): dark floor, bright ceiling, a soft box where the light is.
float3 EnvSoft(float3 r, float3 L, float rough)
{
	const float up = -r.y;
	float3 env = lerp(float3(0.04, 0.04, 0.05), float3(0.85, 0.87, 0.9), smoothstep(-0.35, 0.75, up));
	env += pow(saturate(dot(r, L)), lerp(80.0, 3.0, rough)) * lerp(4.0, 0.5, rough);
	return lerp(env, float3(0.35, 0.35, 0.37), rough * 0.7);
}

// High-contrast studio for metal: big overhead light, bright horizon line over a dark floor band,
// tall strip lights left and right, a key light where the light is. Rough surfaces widen every light.
float3 EnvStudio(float3 r, float3 L, float rough)
{
	const float up = -r.y, side = r.x, toward = -r.z;
	const float w = rough * rough;
	float c = 0.02;
	c += 0.3 * smoothstep(-0.05, 0.7, up);
	c += 2.4 * Band(up, 0.85, 0.14 + w);
	c += 1.6 * Band(up, 0.04, 0.035 + w);
	c *= 1 - 0.85 * Band(up, -0.16, 0.1 + w);
	c += 2.0 * Band(abs(side), 0.8, 0.06 + w) * smoothstep(-0.45, 0.0, up);
	// Reflectors around the camera: what faces that look at the viewer see.
	c += saturate(toward) * (0.3 + 0.35 * smoothstep(-0.25, 0.25, up));
	float3 env = c * float3(1.0, 0.99, 0.97);
	env += pow(saturate(dot(r, L)), lerp(150.0, 4.0, rough)) * lerp(6.0, 0.6, rough);
	return lerp(env, float3(0.3, 0.3, 0.31), w * 0.6);
}

// The surroundings are laid out for the default light (upper left, 225°). Turn them around the view
// axis with the light so its direction also moves the reflections on surfaces facing the viewer.
float3 Environment(float3 r, float3 L, float rough, float style)
{
	const float len = length(L.xy);
	if (len > 1e-3) {
		const float2 d = L.xy / len, d0 = float2(-0.70710678, -0.70710678);
		const float c = dot(d0, d), s = d.x * d0.y - d.y * d0.x; // turn from d to d0, applied to r
		r.xy = float2(c * r.x - s * r.y, s * r.x + c * r.y);
		L.xy = d0 * len;
	}
	return style > 0.5 ? EnvStudio(r, L, rough) : EnvSoft(r, L, rough);
}

// Direct light (GGX) + ambient + reflected surroundings. base = linear colour.
float3 Shade(float3 base, float3 N, float3 V, float3 L, float intensity, float ambient, float metal, float rough, float style)
{
	rough = clamp(rough, 0.04, 1.0);
	const float3 H = normalize(L + V);
	const float a2 = rough * rough * rough * rough;
	const float NL = saturate(dot(N, L)), NV = saturate(dot(N, V)) + 1e-4, NH = saturate(dot(N, H)), VH = saturate(dot(V, H));
	const float3 F0 = lerp(float3(0.04, 0.04, 0.04), base, metal);
	const float3 F = F0 + (1 - F0) * pow(1 - VH, 5);
	const float d = NH * NH * (a2 - 1) + 1;
	const float D = a2 / (PI * d * d);
	const float k = (rough + 1) * (rough + 1) / 8;
	const float G = (NV / (NV * (1 - k) + k)) * (NL / (NL * (1 - k) + k));
	const float3 spec = D * G * F / (4 * NV * max(NL, 1e-4));

	const float3 diffuse = base * (1 - metal);
	float3 c = (diffuse * (1 - F) + spec * PI) * NL * intensity;
	c += diffuse * ambient;
	const float3 Fr = F0 + (max(float3(1 - rough, 1 - rough, 1 - rough), F0) - F0) * pow(1 - NV, 5);
	c += Environment(reflect(-V, N), L, rough, style) * Fr * lerp(0.35, 1.0, metal);
	return c;
}
