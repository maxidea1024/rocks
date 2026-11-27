## 11. 진단 코드

AssetSystem 4.6절 진단 표에 등록한다(요구 MR10).

| 코드 | 수준 | 상황 |
|---|---|---|
| `InvalidMeshData` | 오류 | 위상·속성 불변식 위반, 위치 비유한값 |
| `NonFiniteAttribute` | 경고 | UV·색 비유한값을 고침 |
| `UvOutOfRange` | 경고 | UV 절댓값이 `MeshUvRangeWarning` 초과 |
| `NonNormalizedWeights` | 경고 | 스킨 가중치 합이 `BoneWeightTotal`이 아님 |
| `TooManyInfluences` | 오류 | 정점 영향이 `MeshMaxSourceInfluences` 초과(원본 상한) |
| `InfluencesTrimmed` | 정보 | 쿠킹 상한으로 영향을 버림 |
| `MissingInfluences` | 경고 | 스킨 메시 정점에 영향이 없어 루트 본에 붙임(6.5절 7 — MA5a에서 더함) |
| `MaterialSlotFallback` | 경고 | 다각형 묶음 이름과 맞는 슬롯 없음 |
| `NegativeBuildScale` | 경고 | buildScale 음수 성분 홀수 개(winding 반전 없음) |
| `UnsupportedSetting` | 경고 | 보존 상태 설정이 기본값이 아님 |
| `UnsupportedCollisionShape` | 경고 | level set 등 받지 않는 충돌 형상 |
| `InvalidCollisionData` | 오류 | 단순 충돌 값·볼록 형상 bulk 규칙 위반(4.8절 — MA3a에서 더함) |
| `InvalidSkeletonData` | 오류 | 본 계층·스켈레톤 불변식 위반(6.4절 — MA4a에서 더함) |
| `SkeletonMismatch` | 오류 | 본 병합 때 부모 사슬 불일치, 스킨 메시 임포트에서 고칠 수 없는 묶음 구성원 스켈레톤에 메시 본이 모자람(세부 `MissingSkeletonBones` — 6.7.3절 3) |
| `UnknownBoneTrack` | 경고 | 스켈레톤에 없는 본의 트랙 |
| `InvalidClipData` | 오류 | 클립 상한·frame rate 위반 |
| `EventPropertiesDropped` | 경고 | 이벤트 종류별 속성을 버림 |

### 11.1 손상 데이터 오류(`CorruptData` — AssetSystem 19장)

2026-11-27(에셋 세션 공지 420f68b8 — 담당 세션이 옮긴다). 진단 코드는 그대로이고 오류 값만 19.1절 규칙으로 가른다.

- `CorruptData` — 저장된 데이터(원본 bulk·쿠킹 데이터·너겟)를 읽거나 검사하다 규칙을 어긴 경우: 메시 원본 bulk 배치와 읽은 원본의 모델 검증(`MeshSource::Load` — `CorruptPackage`, 한도 초과는 `CapacityExceeded`), 볼록 충돌 bulk(`LoadConvexGeometry` — 한도 초과는 그대로), 클립 원본 bulk(`LoadClipSource`), 쿠킹 데이터 검증(`ValidateStaticMeshRenderData`·`ValidateStaticMeshIndices`·`ValidateSkeletalMeshRenderData`·`ValidateSkinnedRenderLodBulk`·`ValidateStaticMesh`·`ValidateSkeletalMesh`·`LoadClipPlayback`·`ValidateCookedAnimationClip` — 쿠커가 만든 데이터를 소비자가 부르는 검사라 위반은 손상이다, PostLoad·리플렉션 검증 hook이 없다), 정적·스킨 메시 너겟 머리 읽기(`ReadStaticMeshNuggetHeader`·`ReadSkeletalMeshNuggetHeader` — 쿠킹 처리기는 적중 거절로 다시 만든다, 동작 같음. 이름 pool 상태 오류는 그대로 전한다).
- `InvalidArgument` — 원본·편집 모델 검증(`ValidateMeshSource`·`ValidateSkeleton`·`ValidateClipSource`·`ValidateSimpleCollision` — 호출자가 만든 모델 검사), 빌드·패킹·너겟 머리 쓰기의 인자와 한도, 임포트 명세(사람이 준 요청), 쿠킹 verify의 너겟 불일치(`NuggetMismatch` — 손상이 아니라 결정성 문제, 텍스처 처리기와 같다).
