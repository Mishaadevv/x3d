#ifndef ZX_BRDF_HLSLI
#define ZX_BRDF_HLSLI

// Physically based shading: GGX/Trowbridge-Reitz NDF, height-correlated Smith visibility, Schlick Fresnel,
// Lambert diffuse (metallic-roughness model, glTF-compatible).

static const float ZX_PI = 3.14159265359;

float D_GGX(float NoH, float a2)
{
    const float d = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (ZX_PI * d * d + 1e-7);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float a2)
{
    const float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    const float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / (gv + gl + 1e-7);
}

float3 F_Schlick(float3 f0, float VoH)
{
    const float f = pow(1.0 - VoH, 5.0);
    return f + f0 * (1.0 - f);
}

struct SurfaceData
{
    float3 baseColor;
    float metallic;
    float roughness; // perceptual
    float occlusion;
    float3 normal;   // world space
    float3 emissive;
    float alpha;
};

/// Direct light contribution (radiance already multiplied by intensity), returns outgoing radiance.
float3 EvaluateDirectLight(SurfaceData s, float3 V, float3 L, float3 radiance)
{
    const float3 N = s.normal;
    const float3 H = normalize(V + L);
    const float NoL = saturate(dot(N, L));
    if (NoL <= 0.0)
        return 0.0;
    const float NoV = max(dot(N, V), 1e-4);
    const float NoH = saturate(dot(N, H));
    const float VoH = saturate(dot(V, H));
    const float a = max(s.roughness * s.roughness, 0.002);
    const float a2 = a * a;
    const float3 f0 = lerp(0.04, s.baseColor, s.metallic);
    const float3 F = F_Schlick(f0, VoH);
    const float3 specular = D_GGX(NoH, a2) * V_SmithGGXCorrelated(NoV, NoL, a2) * F;
    const float3 diffuse = (1.0 - F) * (1.0 - s.metallic) * s.baseColor / ZX_PI;
    return (diffuse + specular) * radiance * NoL;
}

/// Analytic approximation of the split-sum environment BRDF (Karis, "Physically Based Shading on Mobile").
float3 EnvBRDFApprox(float3 f0, float roughness, float NoV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

#endif // ZX_BRDF_HLSLI
