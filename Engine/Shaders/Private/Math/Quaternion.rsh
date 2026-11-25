// quaternion 보조다(Docs/Design/EngineShaders 4.2절 A02·7.3절). 표현은 float4(x, y, z, w)이고 규약은
// UE·Core Quaternion과 같다: 곱 QuatMultiply(a, b)는 b를 먼저 적용하고 a를 다음에 적용하는 회전,
// 벡터 회전 v' = q v q*, 행렬은 행 벡터 규약(mul(v, QuatToMatrix(q)) = QuatRotateVector(q, v)).
// 근거: Hamilton 곱, Shoemake, "Animating Rotation with Quaternion Curves", SIGGRAPH 1985(slerp),
// Shepperd, "Quaternion from Rotation Matrix", J. Guidance and Control 1978(행렬 → quaternion).
// tangent 기저 압축은 Frey, Herzeg, "Spherical Skinning with Dual-Quaternions and QTangents",
// SIGGRAPH 2011 Talks(반사 부호를 w 부호에 담는다).
#pragma once

#include "/Engine/Private/Math/ScalarMath.rsh"

// 반사 부호를 담으려고 |w|를 이 값 이상으로 둔다(snorm16 한 단계 1/32767보다 크다 — 양자화해도
// 부호가 남는다).
static const float TangentFrameMinimumW = 1.0 / 32767.0;
// slerp가 nlerp로 넘어가는 cos 각 임계다(거의 같은 회전 — sin θ로 나누는 오차를 피한다).
static const float QuatSlerpLinearThreshold = 0.9995;

// a · b(Hamilton 곱)다. b를 먼저, a를 나중에 적용한다.
float4 QuatMultiply(float4 a, float4 b) {
    return float4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz));
}

// 켤레(단위 quaternion이면 역)다.
float4 QuatConjugate(float4 q) {
    return float4(-q.xyz, q.w);
}

// 단위 quaternion q로 v를 회전한다: t = 2(q.xyz × v), v' = v + w t + q.xyz × t.
float3 QuatRotateVector(float4 q, float3 v) {
    const float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// 단위 quaternion의 회전 행렬(행 벡터 규약)이다. 행 i는 기저 축 i를 회전한 벡터다.
float3x3 QuatToMatrix(float4 q) {
    const float x2 = q.x + q.x;
    const float y2 = q.y + q.y;
    const float z2 = q.z + q.z;
    const float xx = q.x * x2;
    const float xy = q.x * y2;
    const float xz = q.x * z2;
    const float yy = q.y * y2;
    const float yz = q.y * z2;
    const float zz = q.z * z2;
    const float wx = q.w * x2;
    const float wy = q.w * y2;
    const float wz = q.w * z2;
    return float3x3(1.0 - (yy + zz), xy + wz, xz - wy, xy - wz, 1.0 - (xx + zz), yz + wx, xz + wy,
                    yz - wx, 1.0 - (xx + yy));
}

// 회전 행렬(행 벡터 규약, 정규 직교·행렬식 1)의 단위 quaternion이다(Shepperd — 대각합과 가장 큰
// 대각 원소 가운데 큰 쪽으로 나눠 정확도를 지킨다). w ≥ 0 쪽 부호는 보장하지 않는다(q와 −q는 같은
// 회전이다).
float4 MatrixToQuat(float3x3 m) {
    const float trace = m[0][0] + m[1][1] + m[2][2];
    float4 q;
    if (trace > 0.0) {
        const float s = 0.5 * rsqrt(trace + 1.0);
        q = float4((m[1][2] - m[2][1]) * s, (m[2][0] - m[0][2]) * s, (m[0][1] - m[1][0]) * s, 0.25 / s);
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = 0.5 * rsqrt(1.0 + m[0][0] - m[1][1] - m[2][2]);
        q = float4(0.25 / s, (m[0][1] + m[1][0]) * s, (m[2][0] + m[0][2]) * s, (m[1][2] - m[2][1]) * s);
    } else if (m[1][1] > m[2][2]) {
        const float s = 0.5 * rsqrt(1.0 + m[1][1] - m[0][0] - m[2][2]);
        q = float4((m[0][1] + m[1][0]) * s, 0.25 / s, (m[1][2] + m[2][1]) * s, (m[2][0] - m[0][2]) * s);
    } else {
        const float s = 0.5 * rsqrt(1.0 + m[2][2] - m[0][0] - m[1][1]);
        q = float4((m[2][0] + m[0][2]) * s, (m[1][2] + m[2][1]) * s, 0.25 / s, (m[0][1] - m[1][0]) * s);
    }

    return normalize(q);
}

// a에서 b로 t ∈ [0, 1] 구면 선형 보간한다(짧은 쪽 — dot < 0이면 b를 뒤집는다). 거의 같은 회전이면
// 정규화한 선형 보간이다. 결과는 단위 quaternion이다.
float4 QuatSlerp(float4 a, float4 b, float t) {
    float cosAngle = dot(a, b);
    float4 target = b;
    if (cosAngle < 0.0) {
        cosAngle = -cosAngle;
        target = -b;
    }

    float4 result;
    if (cosAngle > QuatSlerpLinearThreshold) {
        result = normalize(lerp(a, target, t));
    } else {
        const float angle = acos(cosAngle);
        const float inverseSin = 1.0 / sin(angle);
        result = a * (sin((1.0 - t) * angle) * inverseSin) + target * (sin(t * angle) * inverseSin);
    }

    return result;
}

// 정규 직교 tangent 기저(tangent, bitangent, normal)를 quaternion 하나로 압축한다(QTangent). 행이
// (tangent, cross(normal, tangent), normal)인 회전의 quaternion이고, bitangent가 cross(normal,
// tangent)와 반대 방향(거울 UV)이면 w < 0, 아니면 w > 0이다(|w| ≥ TangentFrameMinimumW).
float4 EncodeTangentFrame(float3 tangent, float3 bitangent, float3 normal) {
    const float3 canonicalBitangent = cross(normal, tangent);
    float4 q = MatrixToQuat(float3x3(tangent, canonicalBitangent, normal));
    if (q.w < 0.0) {
        q = -q;
    }

    if (q.w < TangentFrameMinimumW) {
        // 크기를 1로 두고 w만 하한으로 올린다.
        const float xyzScale = sqrt(1.0 - TangentFrameMinimumW * TangentFrameMinimumW);
        q = float4(q.xyz * (xyzScale * rsqrt(max(dot(q.xyz, q.xyz), SmallestSafeSquaredLength))),
                   TangentFrameMinimumW);
    }

    return dot(bitangent, canonicalBitangent) < 0.0 ? -q : q;
}

// EncodeTangentFrame의 역이다. q는 단위 quaternion(양자화 뒤면 정규화해서 넘긴다)이다.
void DecodeTangentFrame(float4 q, out float3 tangent, out float3 bitangent, out float3 normal) {
    const float3x3 m = QuatToMatrix(q);
    tangent = m[0];
    normal = m[2];
    bitangent = (q.w < 0.0 ? -1.0 : 1.0) * cross(normal, tangent);
}
