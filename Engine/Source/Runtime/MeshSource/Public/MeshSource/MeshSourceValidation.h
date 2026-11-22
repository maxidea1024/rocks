#pragma once

#include <MeshSource/MeshSource.h>

// 원본 메시 검증과 값 정리다(Docs/Design/MeshAnimationAssets.ko.md 3.1·3.3·3.5·3.7절).
namespace rocks {

struct MeshValidateOptions {
    // 메시 본 계층의 본 수다. MeshUnknownBoneCount면 boneIndex 범위를 보지 않는다(원본 메시는 본
    // 계층을 모른다 — 스킨 메시 빌드가 넘긴다).
    uint32 boneCount = MeshUnknownBoneCount;
    // 검증 중 임시 배열(변 표·다각형별 삼각형 표·코너 표식)의 할당기다. null이면 메시의
    // allocator다.
    Allocator* scratch = nullptr;
};

// mesh가 내용 불변식을 지키는지 본다(3.1절 위상, 3.5절 속성). 통과한 메시는 빌드·위상 조회가
// 범위 밖을 읽지 않는다.
//
// 보는 것(순서대로, 첫 위반에서 멈춘다): 위치 유한값 → 코너의 정점 번호 → 변(정점 번호·같은 정점·
// 작은 번호 먼저·같은 쌍 중복) → 다각형(묶음 번호·둘레 코너 수 ≥ MeshMinPolygonCorners·코너 번호·
// 둘레를 잇는 변이 있음, 같은 정점 사이는 건너뜀) → 삼각형(코너·다각형 번호, 다각형마다 개수 =
// 둘레 − 2, 세 코너가 그 다각형 둘레에 있음) → 속성((element, 이름) 유일 — 정점 Position 포함, 엔진
// 이름 속성 형태, Bool 0·1, Name 값 내용 규칙, 영향 개수 ≤ MeshMaxSourceInfluences·같은 본
// 없음·boneIndex < boneCount).
//
// 오류: 위반은 failure->code InvalidMeshData(영향 개수만 TooManyInfluences), ErrorCode
// InvalidArgument이고 issue·element·attribute·index가 위치다. 임시 배열 할당 실패는 진단 코드 없이
// 그 오류다. mesh는 바뀌지 않는다.
//
// 사용 예:
//   MeshDiagnostic failure;
//   if (auto valid = ValidateMeshSource(mesh, {}, &failure); !valid) {
//       Report(AssetDiagnosticName(failure.code), MeshIssueName(failure.issue), failure.index);
//   }
//
// 스레드: mesh를 읽기만 한다(같은 mesh를 고치는 스레드가 없어야 한다).
ROCKS_MESHSOURCE_API Result<void> ValidateMeshSource(const MeshSource& mesh,
                                                     const MeshValidateOptions& options = {},
                                                     MeshDiagnostic* failure = nullptr) noexcept;

// 임포트·편집 결과를 저장하기 전의 값 정리다(3.3절 — UE ValidateAndFixData 대응, 3.2절 스킨 가중치
// 정규화). 먼저 ValidateMeshSource를 부른다.
//
// 고치는 것: 코너 Normal·Tangent의 비유한값·길이 0은 0 벡터(경고 없음), TexCoord 비유한 성분은
// 0(NonFiniteAttribute 경고 — 고친 값 수), 그다음 |성분| > MeshUvRangeWarning인 UV 값 수를 셈
// (UvOutOfRange 경고, 값은 그대로), Color 비유한값은 (1, 1, 1, 1)(NonFiniteAttribute), 정점 영향의
// 가중치 합이 BoneWeightTotal이 아니면 NormalizeBoneWeights(NonNormalizedWeights — 정규화한 정점
// 수). 경고는 속성마다 하나이고 warnings 뒤에 붙는다(issue로 종류를 가른다).
//
// 오류: 검증 실패는 ValidateMeshSource와 같다. 영향이 있는데 가중치 합이 0인 정점은 InvalidMeshData
// (issue ZeroWeightSum)다. 경고 배열 할당 실패는 그 오류다. 실패하면 mesh와 warnings는
// 그대로다(값을 고치기 전에 모든 실패를 먼저 확인한다).
ROCKS_MESHSOURCE_API Result<void> SanitizeMeshSource(MeshSource& mesh,
                                                     const MeshValidateOptions& options,
                                                     Array<MeshDiagnostic>& warnings,
                                                     MeshDiagnostic* failure = nullptr) noexcept;

// influences의 가중치를 합이 BoneWeightTotal이 되게 정규화한다(3.2절): 각 가중치를 w × Total / 합
// 으로 내림한 뒤 반올림 잔차(Total − 새 합, 영향 수 미만)를 원래 가중치가 가장 큰 영향에 더한다.
// 같으면 boneIndex가 작은 쪽이다. 순서는 바꾸지 않는다. 비어 있거나 합이 이미 Total이면 그대로다.
//
// 반환: 값을 바꿨으면 true다. 영향이 있는데 합이 0이면 바꾸지 않고 false다(호출자가 오류로 다룬다
// — SanitizeMeshSource는 ZeroWeightSum으로 거부한다).
ROCKS_MESHSOURCE_API bool NormalizeBoneWeights(Span<BoneInfluence> influences) noexcept;

} // namespace rocks
