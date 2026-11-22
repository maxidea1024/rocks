#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Vector2.h>
#include <Core/Math/Vector3.h>
#include <Core/Math/Vector4.h>
#include <Core/Text/StringView.h>
#include <Mesh/MeshBuildSettings.h>
#include <MeshSource/MeshSource.h>

// 정적 메시 LOD 빌드다(Docs/Design/MeshAnimationAssets.ko.md 5.2·5.4절, UE FStaticMeshBuilder
// 대응).
namespace rocks {

// 두 위치가 같다고 보는 축별 거리다(UE THRESH_POINTS_ARE_SAME = 0.00002 — UnrealMathUtility.h).
// removeDegenerates가 켜져 있으면 퇴화 삼각형 판정·겹치는 코너·렌더 정점 용접의 위치 비교가 이 값을
// 쓰고, 꺼져 있으면 0(정확히 같은 위치만)이다. 비교는 성분마다 |a − b| ≤ 임계값이다.
inline constexpr float MeshPointsSameThreshold = 0.00002f;

// 렌더 정점 용접에서 두 법선·tangent·쌍법선이 같다고 보는 축별 거리다(UE THRESH_NORMALS_ARE_SAME —
// MeshBuild.h NormalsEqual).
inline constexpr float MeshNormalsSameThreshold = 0.00002f;

// 렌더 정점 용접에서 두 UV가 같다고 보는 축별 거리다(UE THRESH_UVS_ARE_SAME = 1/1024 — MeshBuild.h
// UVsEqual).
inline constexpr float MeshUvsSameThreshold = 1.0f / 1024.0f;

// MikkTSpace가 받는 최대 삼각형 수다(원본이 면 번호를 int에 넣고 2비트 밀어 쓴다 — MakeIndex).
inline constexpr uint32 MeshMikkTSpaceMaxTriangles = 0x1FFFFFFFu;

// 9.3절 정적 메시 너겟 키의 두 버전이다. MikkTSpace Rocks.patch의 결과 bit가 바뀌면 앞의 것을, 위
// 세 임계값 상수가 바뀌면 뒤의 것을 올린다(MA2b 쿠킹 처리기가 키에 넣는다).
inline constexpr uint32 MeshMikkTSpacePatchVersion = 1;
inline constexpr uint32 MeshBuildThresholdVersion = 1;

// 섹션 하나의 섹션 정보 덮어쓰기다(4.4절 MeshSectionInfo의 구현 필드, UE FMeshSectionInfo).
struct MeshBuildSectionInfo {
    // 쓸 머티리얼 슬롯 번호다. MeshNoIndex면 슬롯 대응(4.3절) 결과를 쓴다.
    uint32 materialIndex = MeshNoIndex;
    bool castShadow = true;
};

// LOD 하나의 빌드 입력이다. 포인터·span이 가리키는 것은 BuildStaticMeshLod가 끝날 때까지 살아
// 있어야 한다.
struct StaticMeshBuildInput {
    // 원본 메시다(필수). 빌드는 읽기만 하고 사본에 값 정리를 적용한다(5.2절).
    const MeshSource* source = nullptr;
    MeshBuildSettings settings;
    // 머티리얼 슬롯마다의 importedSlotName이다(4.3절 — 슬롯 번호 = 배열 번호). 다각형 묶음의
    // MaterialSlotName과 Name 비교 규칙(ASCII 대소문자 무시)으로 맞추고, 빈 문자열은 None이다.
    Span<const StringView> slotNames{};
    // 섹션(= 다각형 묶음) 번호마다의 섹션 정보다. 짧으면 나머지 섹션은 기본값이다.
    Span<const MeshBuildSectionInfo> sectionInfo{};
    // bounds 확장(UE StaticMesh PositiveBoundsExtension·NegativeBoundsExtension). 둘 다 0이면
    // 상자와 구를 그대로 둔다.
    Vec3d positiveBoundsExtension{};
    Vec3d negativeBoundsExtension{};
    // 빌드 중 임시 배열의 할당기다. null이면 결과의 할당기다.
    Allocator* scratch = nullptr;
};

// 섹션 하나다(다각형 묶음 하나 = 섹션 하나, 묶음 순서). 빈 묶음도 삼각형 0인 섹션으로 남는다(UE와
// 같다).
struct MeshBuildSection {
    uint32 materialIndex = 0;
    // indices 안 시작 위치와 삼각형 수다(빈 섹션은 0, 0).
    uint32 firstIndex = 0;
    uint32 triangleCount = 0;
    // 이 섹션 index가 가리키는 정점 번호의 최소·최대다(빈 섹션은 0, 0).
    uint32 minVertex = 0;
    uint32 maxVertex = 0;
    bool castShadow = true;
};

// 상자 + 구 bounds다(UE FBoxSphereBounds의 상자와 반지름, 구 중심은 상자 중심). 렌더 정점이 없으면
// 모두 0이다.
struct MeshBuildBounds {
    Vec3d boxMin{};
    Vec3d boxMax{};
    double sphereRadius = 0.0;
};

// LOD 하나의 메모리 빌드 결과다(쿠킹 형식 패킹 전 — 5.3절 패킹은 MA2b). 정점 속성은 정점 번호로
// 맞춘 배열이다.
//
// 수명: 할당기는 이 객체보다 오래 살아야 한다. 스레드: 동기화하지 않는다.
struct StaticMeshBuildResult {
    explicit StaticMeshBuildResult(Allocator& allocator) noexcept
        : positions(allocator), normals(allocator), tangents(allocator), binormalSigns(allocator),
          texCoords(allocator), colors(allocator), indices(allocator), sections(allocator),
          warnings(allocator) {
    }

    StaticMeshBuildResult(StaticMeshBuildResult&&) noexcept = default;
    StaticMeshBuildResult& operator=(StaticMeshBuildResult&&) noexcept = default;
    StaticMeshBuildResult(const StaticMeshBuildResult&) = delete;
    StaticMeshBuildResult& operator=(const StaticMeshBuildResult&) = delete;

    uint32 VertexCount() const noexcept {
        return static_cast<uint32>(positions.Size());
    }

    // buildScale을 적용한 위치다.
    Array<Vec3> positions;
    // 단위 법선(TangentZ)과 tangent(TangentX)다. 계산할 수 없었던 코너(모든 삼각형이 퇴화 등)는 0
    // 벡터다.
    Array<Vec3> normals;
    Array<Vec3> tangents;
    // 쌍법선 부호다: (tangent, 쌍법선, 법선) 기저의 행렬식이 음수면 −1, 아니면 +1(UE
    // GetBasisDeterminantSign). 쌍법선은 cross(법선, tangent) × 부호다.
    Array<float> binormalSigns;
    // UV 채널 수(원본 TexCoord 채널 수, 0–MeshMaxTexCoords)와 값이다. 채널 c의 정점 v는
    // texCoords[c × VertexCount() + v]다.
    uint32 texCoordChannels = 0;
    Array<Vec2> texCoords;
    // 선형 float 색이다(원본에 Color 속성이 없으면 비어 있다). 8비트 sRGB 부호화는 쿠킹
    // 패킹(MA2b)이 한다.
    Array<Vec4> colors;
    Array<uint32> indices;
    Array<MeshBuildSection> sections;
    MeshBuildBounds bounds;
    // 경고다(값 정리 NonFiniteAttribute·UvOutOfRange, UnsupportedSetting, NegativeBuildScale,
    // MaterialSlotFallback — 이 순서로 붙는다).
    Array<MeshDiagnostic> warnings;
};

// 원본 메시 LOD 하나를 렌더 정점·index·섹션·bounds로 빌드한다(5.2절 1–5·8–10·12단계. 6·7단계 보존
// 설정은 기본값이 아니면 UnsupportedSetting 경고, 11단계는 원본 순서 유지, 13단계 패킹은 MA2b).
//
// 순서: 원본 검증(ValidateMeshSource) → 설정 검사(buildScale, 보존 설정 경고, 음수 성분 경고) →
// 빌드에 쓰는 위상·코너 속성만 담은 사본에 값 정리(SanitizeMeshSource) → 삼각형 법선·tangent →
// 겹치는 코너 → 코너 법선·tangent(UE ComputeTangentsAndNormals와 같은 정점 단위 묶음, 재계산
// 플래그가 꺼져 있어도 0 벡터인 코너는 계산, tangent 재계산은 MikkTSpace 또는 UV별 평균) → 삼각형
// 순서로 렌더 정점 구성 (퇴화 삼각형 제외, buildScale, 겹치는 앞 코너와 용접) → 묶음 순서 섹션(슬롯
// 대응) → bounds. 모든 부동소수점 계산은 RunWithDeterministicFloatingPoint 안에서 하고 초월 함수는
// Core 결정적 수학만 쓴다. 같은 입력이면 결과 bit가 같다. 스레드를 쓰지 않는다.
//
// 사용 예:
//   StaticMeshBuildInput input;
//   input.source = &mesh;
//   const StringView slots[] = {"Body", "Glass"};
//   input.slotNames = slots;
//   StaticMeshBuildResult built(allocator);
//   MeshDiagnostic failure;
//   if (auto result = BuildStaticMeshLod(input, built, &failure); !result) {
//       Report(AssetDiagnosticName(failure.code), MeshIssueName(failure.issue));
//   }
//
// 오류: source가 null이면 InvalidArgument(진단 없음). 원본 검증 실패는 ValidateMeshSource의 진단
// (InvalidMeshData·TooManyInfluences)과 그 오류다. buildScale 성분이 0·비유한이거나 역수가
// 비유한이면, 또는 크기를 곱한 위치가 비유한이면 InvalidSetting(issue InvalidBuildScale, index = 축
// 0–2)과 InvalidArgument다. 삼각형 수가 렌더 index 수(uint32)나 MeshMikkTSpaceMaxTriangles를 넘으면
// CapacityExceeded(진단 없음), 할당 실패는 그 오류다. 실패하면 out은 그대로다.
//
// 인자: out은 성공하면 통째로 바뀐다(경고 포함). failure는 실패하면 채운다(성공하면 건드리지
// 않는다). 스레드: input.source를 읽기만 한다(그 메시를 고치는 스레드가 없어야 한다).
ROCKS_MESHTOOLS_API Result<void> BuildStaticMeshLod(const StaticMeshBuildInput& input,
                                                    StaticMeshBuildResult& out,
                                                    MeshDiagnostic* failure = nullptr) noexcept;

} // namespace rocks
