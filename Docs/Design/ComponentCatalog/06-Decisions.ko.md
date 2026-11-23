# 엔진 컴포넌트 도입 — 6. 결정(권장안)

[목차](README.ko.md)

## 6. 결정(권장안)

| ID | 항목 | 권장안 | 근거 |
|---|---|---|---|
| CD1 | BSP와 brush | `UModelComponent`·`UBrushComponent` 제외. volume 형상은 `VolumeShape`(상자·구·캡슐·볼록체)로, 볼록체는 메시 에셋 단순 충돌(MA 4.6절)에서 가져온다. 트리거처럼 물리 이벤트가 필요한 영역은 충돌 형상 컴포넌트(CM3)를 쓴다 | 사용자 지시. 오디오 리버브 존이 이미 같은 형상을 정했다(AU 3.15.2절) |
| CD2 | HISM | 별도 클래스 없이 `InstancedStaticMeshComponent`로 흡수 | 2.3절. 인스턴스 컬링은 렌더러 몫 |
| CD3 | 앱 수명·플랫폼 이벤트 래퍼 | 컴포넌트 없이 엔진 이벤트로 노출 | Blueprint가 없다(EIP 20.4절) |
| CD4 | Stereo layer | VR P3 프로필 문서로 미룸 | EIP 14절 |
| CD5 | Cascade vector field·pawn noise | 제외 | 2.2절 |
| CD6 | 이름 | 업계 공용 이름(StaticMesh·PointLight·Decal·SkyAtmosphere 등)은 그대로 쓰고 UE 고유 이름은 바꾼다: `ExponentialHeightFog` → `HeightFog`, `LightmassPortal` → `LightBakePortal`(Lightmass는 UE 정적 조명 베이커 이름이다. 베이크 전용이라는 뜻을 남기고 계획 14.10절의 Bake 어휘에 맞춘다. `LightPortal`은 런타임 광원처럼 읽히고, `LightmapPortal`은 프로브 bake도 쓰는 범위를 좁히며, V-Ray식 `SkyPortal`은 베이크 전용임이 드러나지 않아 버렸다 — 사용자 지적 2026-11-23), `WorldPartitionStreamingSource` → `StreamingSource`, `InterpToMovement` → `PathMover`, `RuntimeVirtualTexture` → `VirtualTextureVolume`, `LODSync` → `LodSync`, `WindDirectionalSource` → `WindSource`. `Component`(UE `UActorComponent`)는 이미 정한 이름 | Rocks 이름 규칙(UE 구조, Rocks 이름) |
| CD7 | 입력 컴포넌트 | Enhanced Input 구조(액션·매핑 컨텍스트) 하나. 옛 축·액션 이름 바인딩 없음 | 3.8절, 하위호환 없음 원칙 |
| CD8 | 활성(Activate) | UE의 Activate·Deactivate·`bAutoActivate`는 Rocks `enabled`·`OnEnable`·`OnDisable`로 대응(EIP 20.2절 Activate→Enable). 별도 활성 상태를 두지 않는다. "등록됐지만 재생 안 함" 같은 상태는 컴포넌트 속성(예: 오디오 자동 재생)으로 | 상태 하나를 줄인다. UE 대응은 계획 20.2절이 이미 정했다 |
| CD9 | AssetUserData | 들이지 않는다. 필요한 확장 데이터는 리플렉션 필드나 별도 컴포넌트로 | 플러그인용 임의 부착 데이터로, 타입 규칙(CR6)과 겹친다. 요구가 생기면 다시 본다 |
| CD10 | Mobility | UE와 같은 셋(Static·Stationary·Movable). Static 이동은 거부(진단), 정적 조명·캐시 그리기 항목과 연동 | UE와 같게(애매하면 UE 방식) |
| CD11 | Tick 단계(CR8a) | 단계를 더하지 않는다. UE `TG_PrePhysics` → PrePhysics(컴포넌트 기본도 PrePhysics — UE 컴포넌트 기본 `TG_DuringPhysics`는 대응 단계가 없고 관찰 결과가 같다), `TG_StartPhysics`·`TG_EndPhysics` → Physics phase(엔진 전용), `TG_PostPhysics` → PostPhysics, 카메라 갱신 + `TG_PostUpdateWork` → LateUpdate(시작에 엔진 카메라 갱신), end-of-frame 갱신 → PreRender. 옛 PhysicsSync·AnimationFinalize는 Physics phase 끝의 엔진 단계 둘(물리 결과 반영 → skeletal mesh 물리 마무리)이다. 상세 대응표는 ER 8절 | UE 그룹 순서를 그대로 지키면서 Rocks 물리가 Physics phase에서 동기로 돈다는 점만 다르다. "물리와 병렬" 등록 종류는 비동기 물리·병렬 Tick 때 다시 본다(사용자 지시로 권장안 확정 2026-11-23) |
| CD12 | 디버그·편집 표시(CR23) | UE와 같은 세 경로: 디버그 그리기 빌드(Debug·Development·에디터)는 디버그 전용 수집 → 후처리 합성 PC9b, 에디터 view의 편집 표시는 PC9a, Shipping은 base pass 불투명 단순 요소. 자리는 tone map·spatial AA 뒤, spatial upscale 앞. 깊이 우선 그룹 World·Foreground. 렌더러 단계 RS13(선행 RS0, RS6에서 정식 자리) — 에디터 뷰포트 단계를 기다리지 않는다 | UE `UE_ENABLE_DEBUG_DRAWING`·`EPass` 순서(RN 4.2·11.2절 근거). 개발 빌드 디버그 선은 에디터 전에도 필요하다(물리·AI 디버깅). 사용자 지시로 권장안 확정 2026-11-23 |
