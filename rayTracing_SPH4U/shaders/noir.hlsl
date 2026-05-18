Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

cbuffer NoirCB : register(b0) {
    float ScreenW;
    float ScreenH;
}

float hash21(float2 p) {
    float h = dot(p, float2(127.1, 311.7));
    return frac(sin(h) * 43758.5453123);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint2 xy = tid.xy;
    uint w = uint(ScreenW);
    uint h = uint(ScreenH);
    if (xy.x >= w || xy.y >= h) {
        return;
    }

    float3 c = Src.Load(int3(xy, 0)).rgb;
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));

    // Preserve chroma so diffuse colour and fresnel shifts stay visible for physics demos.
    float3 chrom = c - lum;
    float chromGain = 0.78;
    float3 neo = lum.xxx + chrom * chromGain;

    // Mild cool matte bias without collapsing luminance structure.
    neo *= float3(0.94, 0.97, 1.06);

    // Gentler curve than before — lifted mids, less crushed blacks.
    neo = saturate((neo - 0.48) * 1.15 + 0.50);
    neo = pow(saturate(neo), float3(0.93, 0.95, 0.97));

    // Lift blacks slightly while keeping highlight separation.
    const float blackLift = 0.048;
    neo = saturate(neo * 0.93 + blackLift);

    // Blend bright/spec-rich pixels back toward the traced colour so lighting stays readable.
    float peak = max(max(c.r, c.g), c.b);
    float hl = saturate((peak - 0.32) / 0.68);
    float hlBlend = saturate(hl * hl);
    float towardSrc = 0.30 + 0.52 * hl;
    neo = lerp(neo, lerp(neo, c, towardSrc), hlBlend);

    float2 uv = ((float2(xy) + 0.5) / float2(w, h)) * 2.0 - 1.0;
    uv.x *= float(w) / float(h);
    float vig = saturate(1.0 - dot(uv, uv) * 0.42);
    vig = vig * vig;
    neo *= 0.52 + 0.48 * vig;

    float gn = hash21(float2(xy) + lum * 1301.7) * 2.0 - 1.0;
    neo += gn * 0.012;

    Dst[xy] = float4(saturate(neo), 1.0);
}
