// Presents the fitted image: one full-screen triangle. Inside the image rectangle the
// premultiplied RGBA8 pixels are composited over a checkerboard; outside it is background.

#define PRESENT_RS "RootFlags(0), RootConstants(num32BitConstants=10, b0), SRV(t0, visibility=SHADER_VISIBILITY_PIXEL)"

ByteAddressBuffer img : register(t0);
cbuffer Present : register(b0) {
    int imgX, imgY;          // image origin in the back buffer (pixels; may be negative)
    uint imgW, imgH;         // 0 x 0: no image
    uint pitch;              // image row pitch, bytes
    uint checker;            // checker square size, pixels
    uint background;         // 0xAABBGGRR
    uint checkerLight, checkerDark;
    uint pad;
};

float4 unpack(uint c) { return float4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24) / 255.0f; }

[RootSignature(PRESENT_RS)]
float4 vs_main(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

[RootSignature(PRESENT_RS)]
float4 ps_main(float4 pos : SV_Position) : SV_Target {
    const int2 p = int2(pos.xy) - int2(imgX, imgY);
    if (p.x < 0 || p.y < 0 || uint(p.x) >= imgW || uint(p.y) >= imgH) return unpack(background);
    const float4 c = unpack(img.Load(uint(p.y) * pitch + uint(p.x) * 4u));
    const bool odd = ((uint(p.x) / checker + uint(p.y) / checker) & 1u) != 0;
    const float4 bg = unpack(odd ? checkerDark : checkerLight);
    return float4(c.rgb + (1.0f - c.a) * bg.rgb, 1.0f);
}
