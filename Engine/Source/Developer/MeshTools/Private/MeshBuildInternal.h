#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Vector2.h>
#include <Core/Math/Vector3.h>
#include <MeshSource/MeshTopologyIndex.h>
#include <MeshTools/StaticMeshBuilder.h>

// 정적 메시 빌드의 모듈 내부 공용 부분이다(Docs/Design/MeshAnimationAssets.ko.md 5.2·5.4절). 벡터
// 도우미는 UE TVector의 같은 이름 함수와 같은 식·같은 임계값으로 둔다(연산 순서가 결과 bit를
// 정하므로 Core Vector3 함수 대신 여기서 식을 고정한다). 제곱근은 IEEE 정확 반올림
// 명령(math::Sqrt)이라 결정적이다.
namespace rocks::mesh_build {

// UE 수치 상수(UnrealMathUtility.h): SMALL_NUMBER·KINDA_SMALL_NUMBER·DELTA·MIN_flt.
inline constexpr float UeSmallNumber = 1.e-8f;
inline constexpr float UeKindaSmallNumber = 1.e-4f;
inline constexpr float UeDelta = 0.00001f;
inline constexpr float UeMinFloat = 1.175494351e-38f;

// 코너 각 계산에서 내적을 자르는 범위다(UE FMath::Acos가 [−1, 1]로 자른다).
inline constexpr float CosineLimit = 1.0f;

// "렌더 정점 없음"(퇴화로 빠진 삼각형의 wedge) 표지다.
inline constexpr uint32 NoRenderVertex = MeshNoIndex;

// 원본 다각형 묶음에 MaterialSlotName이 없을 때의 이름(None)이다.
inline constexpr StringView NoSlotName{};

inline float Dot3(const Vec3& a, const Vec3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline float SquareSum(const Vec3& value) noexcept {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

inline bool IsFiniteVector(const Vec3& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

inline bool ContainsNaN(const Vec3& value) noexcept {
    return math::IsNaN(value.x) || math::IsNaN(value.y) || math::IsNaN(value.z);
}

// UE IsNearlyZero(Tolerance): 모든 성분이 |c| ≤ tolerance.
inline bool IsNearlyZero(const Vec3& value, float tolerance) noexcept {
    return math::Abs(value.x) <= tolerance && math::Abs(value.y) <= tolerance &&
           math::Abs(value.z) <= tolerance;
}

// UE PointsEqual·FVector3f::Equals: 모든 성분이 |a − b| ≤ threshold.
inline bool PointsEqual(const Vec3& a, const Vec3& b, float threshold) noexcept {
    return math::Abs(a.x - b.x) <= threshold && math::Abs(a.y - b.y) <= threshold &&
           math::Abs(a.z - b.z) <= threshold;
}

inline bool UvsEqual(const Vec2& a, const Vec2& b) noexcept {
    return math::Abs(a.x - b.x) <= MeshUvsSameThreshold &&
           math::Abs(a.y - b.y) <= MeshUvsSameThreshold;
}

// UE GetSafeNormal(Tolerance): 제곱합이 정확히 1이면 그대로, tolerance 미만이면 0 벡터, 아니면
// 1/√(제곱합)을 곱한다.
inline Vec3 SafeNormal(const Vec3& value, float tolerance = UeSmallNumber) noexcept {
    const float sum = SquareSum(value);
    if (sum == 1.0f) {
        return value;
    }

    if (sum < tolerance) {
        return Vec3(0.0f, 0.0f, 0.0f);
    }

    const float scale = math::InvSqrt(sum);
    return Vec3(value.x * scale, value.y * scale, value.z * scale);
}

// UE Normalize(Tolerance): 제곱합이 tolerance보다 크면 단위 길이로 바꾸고 true, 아니면 그대로 두고
// false.
inline bool NormalizeInPlace(Vec3& value, float tolerance = UeSmallNumber) noexcept {
    const float sum = SquareSum(value);
    if (sum > tolerance) {
        const float scale = math::InvSqrt(sum);
        value.x *= scale;
        value.y *= scale;
        value.z *= scale;
        return true;
    }

    return false;
}

// 비유한 성분이 있으면 0 벡터로 바꾼다. 중간 넘침이 낸 NaN의 bit(호스트마다 다른 기본 NaN)가 결과에
// 남지 않게 한다.
inline Vec3 FiniteOrZero(const Vec3& value) noexcept {
    return IsFiniteVector(value) ? value : Vec3(0.0f, 0.0f, 0.0f);
}

// 행이 x·y·z인 3×3 행렬식의 부호다(UE FMatrix(X, Y, Z, 0).Determinant() < 0 → −1, 아니면 +1).
inline float BasisDeterminantSign(const Vec3& x, const Vec3& y, const Vec3& z) noexcept {
    return Dot3(x, Cross(y, z)) < 0.0f ? -1.0f : 1.0f;
}

// 빌드 중의 코너 기저 배열이다(작업 메시의 코너 속성 값 — Vec3는 성분 3개씩, 부호는 1개씩). uv0은
// TexCoord 채널 0(성분 2개씩)이고 TexCoord가 없으면 비어 있다.
struct CornerBasis {
    Span<float> normals;
    Span<float> tangents;
    Span<float> signs;
    Span<const float> uv0;

    Vec3 Normal(uint32 corner) const noexcept {
        return Vec3(normals[3 * corner], normals[3 * corner + 1], normals[3 * corner + 2]);
    }

    Vec3 Tangent(uint32 corner) const noexcept {
        return Vec3(tangents[3 * corner], tangents[3 * corner + 1], tangents[3 * corner + 2]);
    }

    Vec2 Uv(uint32 corner) const noexcept {
        return uv0.IsEmpty() ? Vec2(0.0f, 0.0f) : Vec2(uv0[2 * corner], uv0[2 * corner + 1]);
    }

    void SetNormal(uint32 corner, const Vec3& value) noexcept {
        normals[3 * corner] = value.x;
        normals[3 * corner + 1] = value.y;
        normals[3 * corner + 2] = value.z;
    }

    void SetTangent(uint32 corner, const Vec3& value) noexcept {
        tangents[3 * corner] = value.x;
        tangents[3 * corner + 1] = value.y;
        tangents[3 * corner + 2] = value.z;
    }
};

// 삼각형마다의 법선·tangent·쌍법선이다(5.2절 3단계, UE 삼각형 속성 Normal·Tangent·Binormal).
struct TriangleBases {
    explicit TriangleBases(Allocator& allocator) noexcept
        : normals(allocator), tangents(allocator), binormals(allocator) {
    }

    Array<Vec3> normals;
    Array<Vec3> tangents;
    Array<Vec3> binormals;
};

// 5.2절 3단계: 삼각형 법선·tangent·쌍법선(UE ComputeTriangleTangentsAndNormals — UV 채널 0이 있으면
// UV 경로, 없으면 Duff·Frisvad 경로). threshold는 퇴화 판정 위치 임계값이다. 삼각형을 지우지
// 않는다. 오류: 할당 실패.
Result<void> ComputeTriangleBases(const MeshSource& mesh, const CornerBasis& basis, float threshold,
                                  TriangleBases& out) noexcept;

// 겹치는 wedge 표다(5.2절 4단계, UE FOverlappingCorners). wedge w = 3 × 삼각형 + 꼭짓점 칸이고, 행
// w는 w와 위치가 threshold 안에서 같은 다른 wedge를 오름차순으로 담는다(CSR).
struct OverlappingWedges {
    explicit OverlappingWedges(Allocator& allocator) noexcept
        : starts(allocator), items(allocator) {
    }

    Span<const uint32> Row(uint32 wedge) const noexcept {
        return Span<const uint32>(items.Data() + starts[wedge], starts[wedge + 1] - starts[wedge]);
    }

    Array<usize> starts;
    Array<uint32> items;
};

// 겹치는 wedge 표를 만든다(UE FindOverlappingCorners와 같은 z 정렬 훑기 — 같은 z 값은 wedge 번호
// 순). 오류: 할당 실패.
Result<void> FindOverlappingWedges(const MeshSource& mesh, float threshold, Allocator& scratch,
                                   OverlappingWedges& out) noexcept;

// 코너 법선·tangent 계산 설정이다(UE EComputeNTBsFlags).
struct CornerBasisOptions {
    bool forceNormals = false;
    bool forceTangents = false;
    bool mikkTSpace = false;
    bool weighted = false;
};

// 5.2절 5단계: 코너 법선·tangent·쌍법선 부호(UE ComputeTangentsAndNormals). 정점마다 그 정점을 쓰는
// 삼각형을 soft 변(hard가 아니거나 변 목록에 없는 다각형 안 대각선)으로 이은 묶음으로 나눠
// 평균한다. 모든 코너가 유효한 법선·tangent를 가진 정점은 건너뛴다(재계산 플래그가 꺼져 있어도 0
// 벡터 코너는 계산된다). hardness는 변마다의 IsHard 값(없으면 비어 있다)이다. 오류: 할당 실패.
Result<void> ComputeCornerBases(const MeshSource& mesh, const MeshTopologyIndex& topology,
                                Span<const uint8> hardness, const TriangleBases& triangles,
                                const CornerBasisOptions& options, Allocator& scratch,
                                CornerBasis& basis) noexcept;

// MikkTSpace로 코너 tangent와 쌍법선 부호를 다시 만든다(UE ComputeMikktTangents — 부호는 MikkTSpace
// 부호의 반대). 삼각형이 있고 uv0이 있어야 한다. 한 코너를 여러 삼각형이 쓰면 마지막 삼각형의 값이
// 남는다(UE와 같다). 오류: MikkTSpace 내부 할당 실패는 그 할당기 오류다.
Result<void> ComputeMikkTSpaceTangents(const MeshSource& mesh, Allocator& scratch,
                                       CornerBasis& basis) noexcept;

} // namespace rocks::mesh_build
