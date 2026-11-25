// 행렬 보조다(Docs/Design/EngineShaders 4.2절 A02·7.3절). 규약은 UE와 같다(3.4절, SS 7.2절 6항):
// 행 벡터 p' = p·M(mul(v, M)), 행 우선 저장, 이동은 4행(m[3].xyz). 투영은 reversed-Z 무한 원평면만
// 다룬다(Core MatrixFactory TryMakePerspective(halfFov, w, h, near) — UE FReversedZPerspectiveMatrix).
#pragma once

#include "/Engine/Private/Math/ScalarMath.rsh"

// 위치를 아핀 행렬로 옮긴다(w = 1, 결과 w는 버린다 — 투영 행렬이면 TransformPositionHomogeneous).
float3 TransformPosition(float3 position, float4x4 m) {
    return mul(float4(position, 1.0), m).xyz;
}

// 위치를 동차 좌표로 옮긴다(투영 포함).
float4 TransformPositionHomogeneous(float3 position, float4x4 m) {
    return mul(float4(position, 1.0), m);
}

// 방향을 옮긴다(이동 무시 — 위 3×3).
float3 TransformDirection(float3 direction, float4x4 m) {
    return mul(direction, (float3x3)m);
}

// 3×3 행렬식이다.
float Determinant3x3(float3x3 m) {
    return dot(m[0], cross(m[1], m[2]));
}

// 3×3 역행렬이다(여인수 / 행렬식 — Cramer). 행렬식이 0이면(|det| < SmallestSafeDivisor) 0 행렬이다.
// 오차: 조건수 κ에 비례한다(잘 조건화된 변환 행렬에서 수 ulp × κ).
float3x3 InverseMatrix3x3(float3x3 m) {
    // 열 j가 여인수 행렬의 행 j다: inverse[i][j] = cofactor[j][i] / det.
    const float3 row0 = cross(m[1], m[2]);
    const float3 row1 = cross(m[2], m[0]);
    const float3 row2 = cross(m[0], m[1]);
    const float det = dot(m[0], row0);
    const bool singular = abs(det) < SmallestSafeDivisor;
    const float inverseDet = singular ? 0.0 : 1.0 / det;
    return transpose(float3x3(row0, row1, row2)) * inverseDet;
}

// world 행렬(행 벡터)의 법선 변환 행렬 (M₃⁻¹)ᵀ다: n' = mul(n, NormalMatrixFromWorld(M))를 정규화해
// 쓴다. 비균등 scale·음수 scale에서도 법선이 면에 수직이다. 특이 행렬이면 0 행렬이다.
float3x3 NormalMatrixFromWorld(float4x4 world) {
    return transpose(InverseMatrix3x3((float3x3)world));
}

// 이동(4행)이다.
float3 ExtractTranslation(float4x4 m) {
    return m[3].xyz;
}

// 축별 scale(앞 세 행의 길이 — UE FMatrix::GetScaleVector와 같은 정의, 부호 없음)이다.
float3 ExtractScale(float4x4 m) {
    return float3(length(m[0].xyz), length(m[1].xyz), length(m[2].xyz));
}

// reversed-Z 무한 원평면 원근 투영의 near 거리(cm)다: M[3][2] = near, M[2][2] = 0, M[2][3] = 1.
float ProjectionNearDistance(float4x4 projection) {
    return projection[3][2];
}

// reversed-Z 원근 투영의 반화각 tan(수평, 수직)이다: M[0][0] = 1/tan(h), M[1][1] = 1/tan(v).
float2 ProjectionTanHalfFov(float4x4 projection) {
    return float2(1.0 / projection[0][0], 1.0 / projection[1][1]);
}

// reversed-Z 무한 원평면 깊이(NDC z, near 1 → 무한 0)에서 view 깊이(cm)다: z = near / depth.
// depth = 0(무한)이면 FloatMaxValue다.
float ReversedInfiniteDepthToViewDepth(float deviceDepth, float nearDistance) {
    return deviceDepth > 0.0 ? nearDistance / deviceDepth : FloatMaxValue;
}
