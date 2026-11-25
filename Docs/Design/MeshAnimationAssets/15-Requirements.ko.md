## 15. 다른 문서에 반영할 요구

| ID | 대상 | 요구 | 상태 |
|---|---|---|---|
| MR1 | [렌더 기반](../RenderBase.ko.md) 10절 | 정적 메시 정점 입력 타입: stream 집합(위치 Float3, tangent 기저 Byte4Snorm×2 또는 Short4Snorm×2, UV 쌍 Half4/Float4 × 1–4, 색 프로필별 선택). 고정밀 형식은 퍼뮤테이션 축이 아니라 stream 형식 차이로 처리할지 정한다 | 반영(RenderBase 개정 29, 10.4절 끝) |
| MR2 | 렌더 기반 10절, [렌더러](../Renderer.ko.md) RS12 | skinned 정점 입력: 영향 4/8개, 번호 8/16비트, 가중치 8/16비트, 섹션 본 palette 크기 한도 | 반영·결정(RenderBase 개정 30 결정 R18·R19: 섹션 분할 없음·LOD 통합 bone map·256 기준 번호 폭·typed buffer 본 행렬, 고정밀은 stream 형식 차이) |
| MR3 | [텍스처 스트리밍](../TextureStreaming.ko.md) | 메시 LOD 스트리밍(StreamableRenderAsset 공유): LOD별 bulk 묶음, 상주 LOD 규칙 | 반영·결정(TextureStreaming 개정 6 결정 T9·4.9절·TS5) |
| MR4 | [머티리얼 시스템](../MaterialSystem.ko.md) 8.4절 | 메시 정점 입력 타입 이름(정적·skinned)을 사용 대상 집합에 등록. morph 사용 여부 표시 | 반영(MaterialSystem 개정 6, 8.4절) |
| MR5 | [물리 어댑터](../PhysicsJoltAdapter.ko.md) | `SimpleCollision` 형상 → Jolt 형상 대응, `CollisionComplexity`, 볼록 형상 쿠킹 | 반영(PhysicsJoltAdapter 개정 1, 4절·J20·J21) |
| MR6 | [엔진 구현 계획](../EngineImplementationPlan.ko.md) 13.1·15절 | 이 문서를 가리키는 연결(개정 1에서 반영) | 반영(개정 1) |
| MR7 | 렌더 기반(GraphicsTypes) | `IndexFormat`을 Graphics에서 GraphicsTypes로 옮긴다(직렬화하는 쪽이 장치 없는 모듈을 쓰게) | 반영·결정(RenderBase 개정 30 결정 R20 — 고정 번호표 UInt16 = 1·UInt32 = 2, 이동은 MA2 착수 전) |
| MR8 | Core 리플렉션 미러 | `FrameRate`·`Box<double>`·`Rotator<double>`·`Transform`·`LinearColor`의 Binds 미러가 있는지 확인하고 없으면 더한다(필드 타입으로 쓴다) | 확인함: `Rotator<double>`·`LinearColor`·`Transform` 미러는 있다(CoreTypeBindings/CoreValueTypes.h). `FrameRate`는 MA6a에서 더했다(7.5절 3). `Box<double>`는 MA2b에서 더한다 |
| MR9 | [에셋 시스템](../AssetSystem/12-Textures/12.4-Settings.ko.md) 12.4.5절·TargetProfile | 메시 LOD 그룹 표(screenSize·minLod 기본값)를 텍스처 LOD 그룹과 나란히 둔다 | 반영(AssetSystem 개정 10, 12.4.5절 — UE `FStaticMeshLODGroup` 멤버 대응) |
| MR10 | 에셋 시스템 3·4.6절 | 모듈 그래프에 8절 다섯 모듈 추가, 11절 진단 코드 등록 | 반영(AssetSystem 개정 10, 3·4.6절) |
