// 단위 벡터 인코딩이다(Docs/Design/EngineShaders 4.2절 A02·7.3절): 팔면체(octahedral)·반구 팔면체
// 사상, 격자 양자화(정밀 버전), 2성분 법선 z 복원. 근거: Cigolle, Donow, Evangelakos, Mara, McGuire,
// Meyer, "A Survey of Efficient Representations for Independent Unit Vectors", JCGT 3(2) 2014(3절
// oct·hemi-oct, 4절 정밀 양자화 — 가장 가까운 격자점 탐색).
#pragma once

#include "/Engine/Private/Math/ScalarMath.rsh"

// 팔면체 사상 정의역 [-1, 1]²에서 아래 반구를 접는다(|p| 대칭, 부호는 SignNotZero).
float2 OctahedralWrap(float2 p) {
    return (1.0 - abs(p.yx)) * SignNotZero(p);
}

// 단위 벡터 → [-1, 1]² 팔면체 좌표다. 0 벡터는 정의하지 않는다.
float2 EncodeOctahedral(float3 n) {
    const float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    return n.z >= 0.0 ? p : OctahedralWrap(p);
}

// [-1, 1]² 팔면체 좌표 → 단위 벡터다.
float3 DecodeOctahedral(float2 e) {
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = OctahedralWrap(n.xy);
    }

    return normalize(n);
}

// 위 반구(z ≥ 0) 단위 벡터 → [-1, 1]² 반구 팔면체 좌표다(정사각형을 45° 돌려 전부 쓴다).
float2 EncodeHemiOctahedral(float3 n) {
    const float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + n.z));
    return float2(p.x + p.y, p.x - p.y);
}

float3 DecodeHemiOctahedral(float2 e) {
    const float2 p = float2(e.x + e.y, e.x - e.y) * 0.5;
    return normalize(float3(p, 1.0 - abs(p.x) - abs(p.y)));
}

// 격자 부호 → [-1, 1] 좌표다.
float2 GridToOctahedral(uint2 code, uint bits) {
    const float maxCode = (float)((1u << bits) - 1u);
    return (float2)code * (2.0 / maxCode) - 1.0;
}

// 단위 벡터를 성분당 bits bit 팔면체 격자로 양자화한다(정밀 버전 — Cigolle 2014 4절: 내림·올림 네
// 격자점 가운데 복호화한 방향이 n과 가장 가까운 것). 최대 각 오차는 단순 반올림보다 작다.
uint2 QuantizeOctahedral(float3 n, uint bits) {
    const float maxCode = (float)((1u << bits) - 1u);
    const float2 continuous = saturate(EncodeOctahedral(n) * 0.5 + 0.5) * maxCode;
    const uint2 base = (uint2)floor(continuous);
    const uint maxIndex = (1u << bits) - 1u;
    uint2 best = base;
    float bestDistance = 8.0;
    for (uint corner = 0u; corner < 4u; ++corner) {
        const uint2 candidate = min(base + uint2(corner & 1u, corner >> 1), uint2(maxIndex, maxIndex));
        // cos 각 대신 거리 제곱으로 고른다: 고운 격자에서 cos 차이는 float 1 근처 ulp보다 작다.
        const float3 difference = DecodeOctahedral(GridToOctahedral(candidate, bits)) - n;
        const float distance = dot(difference, difference);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }

    return best;
}

// QuantizeOctahedral 부호 → 단위 벡터다.
float3 DequantizeOctahedral(uint2 code, uint bits) {
    return DecodeOctahedral(GridToOctahedral(code, bits));
}

// 2성분 법선(x, y — tangent 공간, z ≥ 0)에서 z를 복원한다. |xy| > 1이면 z = 0이다.
float3 ReconstructNormalZ(float2 xy) {
    return float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
}
