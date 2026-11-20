#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Vector2.h>
#include <Core/Math/Vector3.h>
#include <Core/Math/Color.h>
#include <MeshSource/MeshSourceValidation.h>
#include <MeshSource/MeshTopologyIndex.h>
#include <MeshTools/StaticMeshBuilder.h>

// 정적·스킨 메시 빌드의 모듈 내부 공용
// 부분이다(Docs/Design/MeshAnimationAssets.ko.md 5.2·5.4·6.5절). 벡터 도우미는 UE TVector의 같은
// 이름 함수와 같은 식·같은 임계값으로 둔다(연산 순서가 결과 bit를 정하므로 Core Vector3 함수 대신
// 여기서 식을 고정한다). 제곱근은 IEEE 정확 반올림 명령(math::Sqrt)이라 결정적이다.
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

// 5.2절 1–5·8–10·12단계의 단계 함수다(정적 빌드와 스킨 빌드가 함께 쓴다 — 6.5절 15, 구현은
// MeshLodBuild.cpp).

// 사본이 담는 코너 속성 수다(Normal·Tangent·BinormalSign·Color·TexCoord).
inline constexpr uint32 CopiedCornerAttributes = 5;

enum CopiedSlot : uint32 {
    NormalSlot,
    TangentSlot,
    SignSlot,
    ColorSlot,
    TexCoordSlot
};

// 원본 사본의 속성 번호와 원본 번호의 대응이다(없으면 MeshNoAttribute). weights는 스킨 빌드가 담는
// 정점 가중치 속성(기본 SkinWeights 먼저, 그다음 SkinWeights-<profile>을 원본 순서로)이다.
struct WorkAttributes {
    explicit WorkAttributes(Allocator& allocator) noexcept
        : weightWork(allocator), weightSource(allocator) {
    }

    uint32 work[CopiedCornerAttributes] = {MeshNoAttribute, MeshNoAttribute, MeshNoAttribute,
                                           MeshNoAttribute, MeshNoAttribute};
    uint32 source[CopiedCornerAttributes] = {MeshNoAttribute, MeshNoAttribute, MeshNoAttribute,
                                             MeshNoAttribute, MeshNoAttribute};
    Array<uint32> weightWork;
    Array<uint32> weightSource;

    // 사본 속성 번호의 원본 번호다(대응이 없으면 그대로).
    uint32 ToSource(uint32 attribute) const noexcept;
};

// 경고 하나를 만든다(code·issue·요소·위치·수).
MeshDiagnostic MakeWarning(AssetDiagnosticCode code, MeshIssue issue, MeshElement element,
                           uint32 attribute, uint32 index, uint64 count) noexcept;

// 원본의 위상과 빌드가 쓰는 코너 속성만 담은 사본을 만든다(5.4절 3). copyWeights면 정점
// SkinWeights·SkinWeights-<profile>도 담는다(6.5절 5). 오류: 할당 실패.
Result<void> CopyBuildMesh(const MeshSource& source, bool copyWeights, MeshSource& work,
                           WorkAttributes& attributes) noexcept;

// 사본에 값 정리(SanitizeMeshSource)를 하고 경고를 원본 속성 번호로 바꿔 out 뒤에 붙인다. 실패
// 진단(failure — 늘 있다)의 속성 번호도 원본 번호로 바꾼다. options.scratch는 null이 아니어야 한다.
// 오류: 값 정리 실패(가중치 합 0), 할당 실패.
Result<void> SanitizeWorkMesh(MeshSource& work, const WorkAttributes& attributes,
                              const MeshValidateOptions& options, Array<MeshDiagnostic>& out,
                              MeshDiagnostic* failure) noexcept;

// 5.2절 3·5단계 결과다. basis의 span은 work의 코너 속성 값을 가리킨다.
struct LodBases {
    explicit LodBases(Allocator& allocator) noexcept : triangles(allocator), topology(allocator) {
    }

    CornerBasis basis;
    TriangleBases triangles;
    MeshTopologyIndex topology;
    // 변마다의 IsHard 값(원본 속성 — 없으면 비어 있다).
    Span<const uint8> hardness;
    CornerBasisOptions options;
    bool hasUv = false;
};

// 5.2절 3·5단계: 삼각형 기저(위치 임계값 threshold) → 위상 조회 → 코너 법선·tangent(설정의 재계산·
// 가중·MikkTSpace 플래그). 오류: 할당 실패, MikkTSpace 할당 실패.
Result<void> ComputeLodBases(const MeshSource& source, MeshSource& work,
                             const WorkAttributes& attributes, const MeshBuildSettings& settings,
                             float threshold, Allocator& scratch, LodBases& out) noexcept;

// 렌더 정점 용접 규칙이다(5.4절 4·10, 6.5절 6·8).
struct WeldOptions {
    // 퇴화 삼각형 판정의 위치 임계값이다(두 꼭짓점이 이 안에서 같으면 뺀다).
    float degenerateThreshold = 0.0f;
    // 용접 비교의 위치·법선(tangent·쌍법선 포함)·UV 임계값이다.
    float positionThreshold = 0.0f;
    float normalThreshold = MeshNormalsSameThreshold;
    float uvThreshold = MeshUvsSameThreshold;
    // 용접 뒤 세 렌더 정점 번호가 서로 다르지 않은 삼각형도 뺀다(UE 스킨 빌드).
    bool dropCollapsedTriangles = false;
    // 위 항목이 모두 같을 때 더 비교한다(두 코너 번호 — a는 지금 코너, b는 후보 렌더 정점을 만든
    // 코너). null이면 비교하지 않는다.
    bool (*sameExtra)(const void* context, uint32 a, uint32 b) noexcept = nullptr;
    const void* context = nullptr;
};

// 렌더 정점을 만드는 중의 보조 배열이다(UE FMeshBuildVertexData). 빌드가 끝난 뒤 스킨 빌드가
// wedgeVertices·vertexCorners·triangleKept를 읽는다.
struct VertexBuffers {
    explicit VertexBuffers(Allocator& allocator) noexcept
        : tangentY(allocator), colorKeys(allocator), uvs(allocator), wedgeVertices(allocator),
          vertexCorners(allocator), triangleKept(allocator) {
    }

    // 용접 비교에 쓰는 쌍법선과 8비트 sRGB 색(UE는 FColor로 비교한다)이다.
    Array<Vec3> tangentY;
    Array<Color> colorKeys;
    // 정점 우선 UV(정점마다 채널 수만큼) — 끝에 채널 우선으로 옮긴다.
    Array<Vec2> uvs;
    // wedge마다의 렌더 정점 번호(퇴화로 빠진 삼각형은 NoRenderVertex).
    Array<uint32> wedgeVertices;
    // 렌더 정점마다 그것을 만든 코너 번호다.
    Array<uint32> vertexCorners;
    // 삼각형마다 index에 남는지(1) 빠졌는지(0)다.
    Array<uint8> triangleKept;
};

// 법선 하나를 렌더 법선으로 바꾼다(UE ScaleStaticMeshVertex의 법선: 크기로 나눈 뒤 정규화, 비유한은
// 0 벡터). 렌더 정점 구성과 morph 법선 변화량(6.5절 13)이 같은 식을 쓴다.
Vec3 RenderNormal(const Vec3& normal, const Vec3& scale) noexcept;

// 5.2절 8·9단계: 삼각형 순서로 코너를 렌더 정점으로 옮긴다(UE BuildVertexBuffer·
// BuildSkeletalMeshChunks). 오류: 할당 실패.
Result<void> BuildRenderVertices(const MeshSource& work, const CornerBasis& basis,
                                 const WorkAttributes& attributes,
                                 const OverlappingWedges& overlaps, const Vec3& scale,
                                 const WeldOptions& weld, StaticMeshBuildResult& result,
                                 VertexBuffers& buffers) noexcept;

// 5.2절 10단계: 묶음 순서 섹션과 index, 슬롯 대응·MaterialSlotFallback 경고. 오류: 할당 실패.
Result<void> BuildSections(const MeshSource& source, Span<const StringView> slotNames,
                           Span<const MeshBuildSectionInfo> sectionInfo,
                           const VertexBuffers& buffers, Allocator& scratch,
                           StaticMeshBuildResult& result) noexcept;

// 5.2절 12단계: 렌더 정점 위치의 상자·구 bounds와 확장.
MeshBuildBounds ComputeBounds(Span<const Vec3> positions, const Vec3d& positiveExtension,
                              const Vec3d& negativeExtension) noexcept;

} // namespace rocks::mesh_build
