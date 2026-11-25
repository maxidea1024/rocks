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
| `SkeletonMismatch` | 오류 | 본 병합 때 부모 사슬 불일치 |
| `UnknownBoneTrack` | 경고 | 스켈레톤에 없는 본의 트랙 |
| `InvalidClipData` | 오류 | 클립 상한·frame rate 위반 |
| `EventPropertiesDropped` | 경고 | 이벤트 종류별 속성을 버림 |
