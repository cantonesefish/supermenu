// SuperMenu liquid glass. Adapted from Atrium Desktop's glass.hlsl (MIT).
// Plain HLSL 5.0, compiled at runtime with D3DCompile.
//
// The surface samples a live desktop ROI (mip pyramid), bends it near a G3
// (continuous-curvature) rim, tints and lights it, and outputs premultiplied
// alpha so DirectComposition can blend it over the desktop.
//
// Pad0 is used as grain/noise and Pad1 as vibrancy (saturation), keeping the
// 400-byte cbuffer layout identical to the port brief.

cbuffer GlassParams : register(b0)
{
    float2 Size;
    float2 RoiOrigin;
    float2 RoiSize;
    float Radius;
    float Edge;
    float Refract;
    float Dispersion;
    float BlurLod;
    float TintMix;
    float4 Tint;
    float Highlight;
    float Shadow;
    float2 LightDir;
    float Inset;
    float BlurMix;
    float Fade;
    float Noise;
    float4 Shape;
    float4 Ripple;
    float4 PartRect[6];
    float4 PartRadii[6];
    float PartCount;
    float Melt;
    float PartMix;
    float Vibrancy;
    float4 Tail;
    float4 Wave;
    float4 FromArea;
    float2 FromAreaOffset;
    float2 Pad2;
};

Texture2D Bg : register(t0);
SamplerState Linear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// ---- continuous-curvature rounded rectangle --------------------------------

float2 cornerProfile(float2 b, float r)
{
    r = clamp(r, 0, min(b.x, b.y));
    float reach = min(r * 1.6, min(b.x, b.y));
    float v = 1 - 0.2928932188134525 * r / max(reach, 1e-12);
    float v2 = v * v;
    float a = b.x == b.y && r == b.x ? 0 : max(0.02, (2 * v2 * v2 - v2) / max(1 - v2, 1e-12));
    return float2(reach, a);
}

// Exact implicit-edge field: used for coverage/anti-aliasing.
float cornerEdge(float2 q, float2 profile)
{
    float r = profile.x, a = profile.y;
    if (r <= 0 || min(q.x, q.y) <= 0)
        return max(q.x, q.y) - r;
    float2 v = q / r, v2 = v * v;
    float2 den = a + (1 - a) * v2;
    float2 derivative = 2 * v * v2 * (2 * a + (1 - a) * v2) / (den * den);
    return (dot(v2 * v2 / den, 1) - 1) * r / max(length(derivative), 1e-12);
}

// Approximate metric distance: only used for refraction/normals/shadow.
float sdCorner(float2 q, float r, float a)
{
    if (min(q.x, q.y) <= 0)
        return max(q.x, q.y) - r;
    if (r <= 0)
        return length(q);
    float n = length(q);
    float2 v, derivative;
    [unroll] for (int i = 0; i < 2; ++i)
    {
        v = q / n;
        float2 v2 = v * v;
        float2 denominator = a + (1 - a) * v2;
        float2 f = v2 * v2 / denominator;
        derivative = 2 * v * v2 * (2 * a + (1 - a) * v2) / (denominator * denominator);
        n *= 1 + (f.x + f.y - 1) / dot(v, derivative);
    }
    return (n - r) * dot(v, derivative) / length(derivative);
}

float sdRoundRect(float2 p, float2 b, float r, bool edge)
{
    float2 profile = cornerProfile(b, r);
    float2 q = abs(p) - b + profile.x;
    return edge ? cornerEdge(q, profile) : sdCorner(q, profile.x, profile.y);
}

float ShapeDistance(float2 px, bool edge)
{
    if (PartCount > 0.5)
    {
        // Containerless panels: union of up to six floating glass islands.
        float best = 1e9;
        [unroll]
        for (int i = 0; i < 6; ++i)
        {
            if (i >= (int)PartCount) break;
            float4 rect = PartRect[i];
            float2 c = px - (rect.xy + rect.zw) * 0.5;
            float2 b = max((rect.zw - rect.xy) * 0.5, 1);
            float r = min(PartRadii[i].x, min(b.x, b.y));
            best = min(best, sdRoundRect(c, b, r, edge));
        }
        return best;
    }
    float2 c = px - (Shape.xy + Shape.zw) * 0.5;
    float2 b = max((Shape.zw - Shape.xy) * 0.5, 1);
    float r = min(Radius, min(b.x, b.y));
    return sdRoundRect(c, b, r, edge);
}

float Dist(float2 px) { return ShapeDistance(px, false); }

// ---- backdrop ---------------------------------------------------------------

float3 bg(float2 px, float lod)
{
    return Bg.SampleLevel(Linear, (px + RoiOrigin) / RoiSize, lod).rgb;
}

float3 blurred(float2 px)
{
    float s = exp2(BlurLod) * 0.75;
    float3 c = bg(px, BlurLod) * 0.4;
    c += bg(px + float2(s, 0), BlurLod) * 0.15;
    c += bg(px - float2(s, 0), BlurLod) * 0.15;
    c += bg(px + float2(0, s), BlurLod) * 0.15;
    c += bg(px - float2(0, s), BlurLod) * 0.15;
    return c;
}

float3 sharpBackdrop(float2 px, float2 off)
{
    float3 c;
    c.r = bg(px + off * (1 + Dispersion), 0).r;
    c.g = bg(px + off, 0).g;
    c.b = bg(px + off * (1 - Dispersion), 0).b;
    return c;
}

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// ---- pixel shader -----------------------------------------------------------

float4 PS(VSOut i) : SV_Target
{
    float2 px = i.uv * Size;
    float d = Dist(px);

    // Soft drop shadow outside the shape.
    float shadowA = 0;
    if (d >= 0)
    {
        float ds = Dist(px - float2(0, Inset * 0.25));
        shadowA = Shadow * exp(-max(ds, 0) / max(Inset * 0.35, 1));
    }

    float edgeDistance = ShapeDistance(px, true);
    float aa = max(fwidth(edgeDistance), 1e-3);
    float cov = d < -2 ? 1 : saturate(0.5 - edgeDistance / aa);
    float keep = saturate(1 - Fade);
    shadowA *= keep;
    if (cov <= 0)
        return float4(0, 0, 0, shadowA);

    // Normal of the metric distance field (for lensing and lighting).
    float e = 1.0;
    float2 g = float2(Dist(px + float2(e, 0)) - Dist(px - float2(e, 0)),
                     Dist(px + float2(0, e)) - Dist(px - float2(0, e)));
    float gl = length(g);
    float2 n = gl > 1e-5 ? g / gl : float2(0, 0);

    float depth = max(-d, 0);
    float t = saturate(1 - depth / max(Edge, 1));
    float profile = 1 - sqrt(saturate(1 - t * t));
    float2 off = -n * Refract * profile;

    // Press ripple (optional; unused by the menu bar, kept for panels).
    float ring = 0;
    if (Ripple.w > 0)
    {
        float2 dv = px - Ripple.xy;
        float dl = length(dv);
        float w = max(Ripple.z * 0.30, 6);
        float x = (dl - Ripple.z) / w;
        ring = exp(-x * x) * Ripple.w;
        float2 dir = dl > 1e-3 ? dv / dl : float2(0, 0);
        off += dir * ring * (6 + Refract * 0.35);
        off -= dv * 0.06 * Ripple.w * saturate(1 - dl / max(Ripple.z, 1));
    }

    float3 col;
    if (BlurMix <= 0)
        col = sharpBackdrop(px, off);
    else if (BlurMix >= 1)
        col = blurred(px + off);
    else
        col = lerp(sharpBackdrop(px, off), blurred(px + off), BlurMix);

    // Vibrancy, like macOS material: keep colour while lifting it a little.
    float luma = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = lerp(luma.xxx, col, max(0.0, Vibrancy));
    col *= (1.0 + 0.04 * Highlight);

    col = lerp(col, Tint.rgb, TintMix);

    // Rim: a crisp specular line facing the light, a soft inner glow, a gentle
    // vertical sheen. LightDir points towards the light.
    float lit = saturate(dot(n, LightDir));
    float back = saturate(dot(n, -LightDir));
    float rimLine = 1 - smoothstep(0.0, 1.6, max(depth - 0.0, 0));
    float glow = saturate(1 - depth / max(Edge * 1.6, 1));
    col += Highlight * rimLine * (0.22 + 0.78 * lit + 0.35 * back);
    col += Highlight * glow * glow * lit * 0.16;

    float sheen = saturate(1 - (px.y - Shape.y) / max(Shape.w - Shape.y, 1));
    col += Highlight * 0.045 * sheen * sheen;

    // A very light shade in the thickest part of the lens.
    float band = smoothstep(0.0, Edge * 0.5, depth) * (1 - smoothstep(Edge * 0.5, Edge * 1.3, depth));
    col *= 1 - 0.05 * band * (1 - lit);
    col += ring * 0.10 * (0.4 + Highlight);

    float grain = (hash12(px * 0.75) - 0.5) * Noise;
    col += grain;

    float4 glass = float4(saturate(col) * cov, cov) * keep;
    return glass + (1 - cov) * float4(0, 0, 0, shadowA);
}
