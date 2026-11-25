// 3D 벡터 보조다(Docs/Design/EngineShaders 4.2절 A02·7.3절): 정규 직교 기저, tangent Gram–Schmidt,
// 반사·굴절, 두 단위 벡터 사이 각. 좌표계와 무관한 식이다(UE 왼손 Z-up에서도 그대로 — 3.4절).
#pragma once

#include "/Engine/Private/Math/ScalarMath.rsh"

// 단위 법선 n에서 (tangent, bitangent, n)이 정규 직교이고 cross(tangent, bitangent) = n인 기저를
// 만든다. Duff, Burgess, Christensen, Hery, Kensler, Liani, Villemin, "Building an Orthonormal Basis,
// Revisited", JCGT 6(1) 2017, Listing 3(부호 분기 — n.z = -1 근처에서도 정확). 부호는 n.z의 부호
// bit(copysign과 같다 — -0은 음수)로 고른다. 오차: n이 단위 벡터면 ‖BᵀB − I‖ ≤ 수 ulp.
void BuildOrthonormalBasis(float3 n, out float3 tangent, out float3 bitangent) {
    const float sign = (asuint(n.z) >> 31) != 0u ? -1.0 : 1.0;
    const float a = -1.0 / (sign + n.z);
    const float b = n.x * n.y * a;
    tangent = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    bitangent = float3(b, sign + n.y * n.y * a, -n.y);
}

// GramSchmidtTangent가 평행으로 보는 상대 길이 제곱 상한이다(남은 성분이 원래 길이의 1e-4 미만 — 각
// 0.006° 안. 그 아래는 반올림 잡음이 방향을 정한다).
static const float GramSchmidtParallelThreshold = 1.0e-8;

// tangent를 단위 법선 normal에 직교하게 만들고 정규화한다(Gram–Schmidt). tangent가 normal과 거의
// 평행하거나(남은 길이 제곱 < GramSchmidtParallelThreshold × 원래 길이 제곱) 0이면
// BuildOrthonormalBasis의 tangent를 쓴다.
float3 GramSchmidtTangent(float3 tangent, float3 normal) {
    const float3 projected = tangent - normal * dot(normal, tangent);
    const float projectedSquared = dot(projected, projected);
    float3 basisTangent = float3(1.0, 0.0, 0.0);
    float3 basisBitangent = float3(0.0, 1.0, 0.0);
    BuildOrthonormalBasis(normal, basisTangent, basisBitangent);
    const bool parallel = !(projectedSquared > GramSchmidtParallelThreshold * dot(tangent, tangent));
    return parallel ? basisTangent : projected * rsqrt(parallel ? 1.0 : projectedSquared);
}

// incident(표면을 향하는 방향)를 단위 법선 normal에 대해 반사한다: i − 2(n·i)n.
float3 ReflectDirection(float3 incident, float3 normal) {
    return incident - 2.0 * dot(normal, incident) * normal;
}

// incident(단위, 표면을 향함)를 단위 법선 normal(incident 쪽)에서 굴절시킨다. eta = 들어오는 쪽
// 굴절률 / 나가는 쪽 굴절률이다(Snell, GLSL refract와 같은 식). 전반사면 totalInternalReflection이
// 참이고 0 벡터를 돌려준다. 결과는 단위 벡터다.
float3 RefractDirection(float3 incident, float3 normal, float eta, out bool totalInternalReflection) {
    const float cosIncident = dot(normal, incident);
    const float k = 1.0 - eta * eta * (1.0 - cosIncident * cosIncident);
    totalInternalReflection = k < 0.0;
    const float3 refracted = eta * incident - (eta * cosIncident + sqrt(max(k, 0.0))) * normal;
    return totalInternalReflection ? float3(0.0, 0.0, 0.0) : refracted;
}

// 두 단위 벡터 사이 각(라디안, [0, π])이다. acos(dot)은 거의 평행할 때 오차가 크므로
// 2·atan2(‖a − b‖, ‖a + b‖)를 쓴다(Kahan, "How Futile are Mindless Assessments of Roundoff", 2006).
float AngleBetweenUnitVectors(float3 a, float3 b) {
    return 2.0 * atan2(length(a - b), length(a + b));
}
