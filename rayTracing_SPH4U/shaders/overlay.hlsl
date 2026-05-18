Texture2D<float4> Src : register(t0);
Texture2D<float4> Ov : register(t1);
RWTexture2D<float4> Dst : register(u0);

// w, h, center, half-extents, OvStrength, MaxAlphaCap (-1 = no cap; banner uses a cap so it never goes fully solid)
cbuffer OverlayCB : register(b0) {
    float ScreenW;
    float ScreenH;
    float CenterX;
    float CenterY;
    float HalfW;
    float HalfH;
    float OvStrength;
    float MaxAlphaCap;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint2 xy = tid.xy;
    uint w = uint(ScreenW);
    uint h = uint(ScreenH);
    if (xy.x >= w || xy.y >= h) {
        return;
    }

    float4 bg = Src.Load(int3(xy, 0));

    float2 corner0 = float2(CenterX, CenterY) - float2(HalfW, HalfH);
    float2 corner1 = float2(CenterX, CenterY) + float2(HalfW, HalfH);
    float2 p = float2(xy) + float2(0.5f, 0.5f);

    if (p.x < corner0.x || p.y < corner0.y || p.x >= corner1.x || p.y >= corner1.y) {
        Dst[xy] = bg;
        return;
    }

    uint ovW = 1u;
    uint ovH = 1u;
    Ov.GetDimensions(ovW, ovH);

    float2 dims = corner1 - corner0;
    float2 uv = (p - corner0) / max(dims, float2(1e-6f, 1e-6f));
    uv = saturate(uv);

    uint tx = clamp(uint(uv.x * float(ovW)), 0u, max(ovW, 1u) - 1u);
    uint ty = clamp(uint(uv.y * float(ovH)), 0u, max(ovH, 1u) - 1u);

    float4 fg = Ov.Load(int3(int2(tx, ty), 0));

    // Straight-alpha premultiply: transparent texels stay transparent (no white wash from rgb where a=0).
    float srcA = fg.a;
    float alpha = saturate(srcA * OvStrength);
    if (MaxAlphaCap >= 0.0f) {
        alpha = min(alpha, MaxAlphaCap);
    }

    float3 rgba = fg.rgb * alpha;
    Dst[xy] = float4(rgba + bg.rgb * (1.0 - alpha), 1.0);
}
