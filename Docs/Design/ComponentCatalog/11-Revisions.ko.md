# 엔진 컴포넌트 도입 — 11. 개정 기록

[목차](README.ko.md)

## 11. 개정 기록

- 개정 6(2026-11-23): 9절 플러그인 컴포넌트(3부) 추가(사용자 지시) — `Engine/Plugins` 139개 플러그인의 클래스 418개를 상속 추적으로 전수 분류(필수 31·필수(실험 기반) 25·대체 28·제외 10·후속 154·편집기 26·범위 밖 124·아님 20), 선행 CR40–CR54, 묶음 CM22–CM31(+ CM20에 `ProceduralMeshComponent`), 결정 CD19–CD24. CD13(색인만)을 CD19로 대체하고 8.9절 색인을 지움. 같은 추적으로 2부 재대조 — 빠진 deprecated 4종(`UAtmosphericFogComponent`·`ULandscapeMeshCollisionComponent_DEPRECATED`·GeometryCollection ISM pool 2종)을 8.3절 제외에 더함(2부 제외 4 → 8). 2부 이름을 CD24 규칙으로 맞춤(`Hlod…`, `AiPerception…`, 내비 디버그 표시 `…Draw`). 9.9절 다른 문서 개정 반영(EIP 15.1·16.1·21·24.1·33.2절, AU 3.15.2·9.1절). 리뷰어 1명 교차검증 반영: namespace 한정 부모로 빠진 26종(MeshPartition modifier·MeshPartitionWater·PCG 어댑터·시험 1), EIP 인용 절 번호 정정(24.3절 Live Link, 16.2·16.3·33.2절 rope·vehicle), CR43·CR44 근거, `UWaterTerrainComponent` 역할(지형 → 물 정보 텍스처), DaySequence·InstancedActors의 CD20 판정 사유, MetaHumanSDK·LidarPointCloud를 범위 밖에서 후속으로, `UDirectMeshControlComponent`(실험)를 편집기에서 후속으로, `UMoverCVDSimDataComponent`를 대체에서 범위 밖(Chaos 디버거)으로, CR48에 PlayerState, CM30·CM31 선행 정리, ISM pool 정의 줄, MotoSynth 의존 행 분리. 확인하지 못한 것·개정 기록을 10·11절로 옮김.
- 개정 5(2026-11-23): 8.8절 다른 문서 개정 반영(EIP 16.1·19·20.5·21절).
- 개정 4(2026-11-23): 8절 폴더 밖 컴포넌트(2부) 추가(사용자 지시) — Engine 다른 폴더·Runtime 다른 모듈 전수 분류(필수 56·대체 2(`USynthComponent`·`UMediaSoundComponent` — AU 9.1절)·제외 4·후속 38·편집기 9·아님 13), 선행 CR27–CR39, 묶음 CM12–CM21, 결정 CD13–CD18, 플러그인 색인. `PathMoverComponent`를 CM5에서 CM12로. 리뷰어 1명 교차검증 반영: 빠진 `UMediaSoundComponent`(대체)·`UMockDataMeshTrackerComponent`(XR 후속), 절차 식생은 UE 실험 기능이라 후속, AI 컨트롤러 선행 CR39, CR29 정본을 EIP 20.5절 `CameraDirector`로, CM12·CM13에 CM3, `LandscapeMeshProxyComponent`를 CM19로, 플러그인 색인의 모델링 도구·Dataflow·단계 표기 정정. 확인하지 못한 것·개정 기록을 9·10절로 옮김.
- 개정 3(2026-11-23): 남아 있던 두 항목을 권장안으로 확정(사용자 지시) — CD11 Tick 단계 UE 대응(ER 8절 대응표, EIP 20.2절), CD12 디버그·편집 표시(RN 4.2절 세 경로, 11.2절 PC9a·PC9b, 17절 RS13). CR8a·CR23 상태를 결정으로.
- 개정 2(2026-11-23): 7절 다른 문서 개정 반영. CR23 정본을 RenderBase에서 Renderer 4.2절로 정정. `LightmassPortal` 대응 이름을 `LightPortal`에서 `LightBakePortal`로(사용자 지적 — 베이크 전용 의미, CD6).
- 개정 1(2026-11-23): 리뷰어 1명 교차검증 반영 — CM1을 CM1a(정적 메시, RS0)·CM1b(광원, RS1·RS3)로 나누고 화면 마일스톤 범위를 CM0+카메라로 정정, 게임 프레임워크 선행 CR26 추가(입력·햅틱), 볼륨 안개·구름을 CM11로, CR5 근거(객체 단위 EditorOnly 미활성), 3절과 4.6절 선행 불일치·ID 오기 정정, PawnNoiseEmitter·HISM·InputComponent 인용 정정, 1.2절 Runtime 다른 모듈(UMG·Foliage·Landscape·AIModule).
- 개정 0(2026-11-23): 첫 초안. 분류(필수 58·대체 3·제외 4·후속 프로필 1), 선행 CR1–CR25, 묶음 CM0–CM11, 결정 CD1–CD10 권장안.
