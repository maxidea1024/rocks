# 렌더러(Renderer) 설계 — 4. 장면 표현과 게임 → 렌더 전달

[목차](README.ko.md)

## 4. 장면 표현과 게임 → 렌더 전달

### 4.1 UE 5.8 방식

| 단계 | UE 5.8 | 근거 |
|---|---|---|
| proxy 생성 | 게임 스레드가 컴포넌트의 `CreateSceneProxy()`로 proxy를 만들고 scene info를 할당한 뒤 render command 하나로 넘긴다 | `R/RendererScene.cpp:1341, 1455, 1471, 1507–1516` |
| 큐 | 렌더 스레드는 추가를 바로 넣지 않고 갱신 큐에 쌓는다 | `R/RendererScene.cpp:1073`, `R/ScenePrimitiveUpdates.h:54` |
| 일괄 적용 | 렌더 시작(`OnRenderBegin`)에서 큐를 프레임 수명 change set으로 옮기고 추가·제거·삭제를 나눠 적용한다. 그 뒤 static mesh 등록 → 그리기 명령 캐시 → GPU Scene 갱신 | `R/SceneRendering.cpp:4378`, `R/RendererScene.cpp:5534–5596, 6446, 6492, 6633–6637, 6792` |
| 변환 갱신 | 변환 명령(world·local bounds, local-to-world)을 큐에 넣는다 | `R/RendererScene.cpp:1572–1579, 1614–1696` |
| 저장 | 장면이 인스턴스별 SoA 배열(변환·proxy·bounds·flags·가시성 ID·occlusion)을 가진다 | `R/ScenePrivate.h:1699–1715` |
| 소유 | 생성 뒤 렌더 스레드 소유. "렌더 스레드는 UObject를 역참조하지 않는다" | `EN/Public/PrimitiveSceneProxy.h:494–495`, 삭제 `R/RendererScene.cpp:6799–6816` |
| 동적 메시 | proxy 가상 함수가 프레임마다 메시 묶음을 낸다 | `EN/Public/PrimitiveSceneProxy.h:504`, `R/SceneVisibility.cpp:4191–4207` |

### 4.2 Rocks 구조

```text
게임 lane(WorldIntegration)                          렌더 lane(Renderer)
컴포넌트 변경 감지 ─▶ SceneChangeList(값) ─┐
카메라 ─▶ ViewDesc 목록 ──────────────────┼─▶ FramePacket ─▶ RenderScene.Apply(변경 목록)
동적 데이터(skin pose·입자) ─▶ RenderBlob ┘                   ─▶ ViewSet 구성 ─▶ 그래프 구성
RenderNotices 처리(해제 완료) ◀──────────────────────────────── 완료 통지
```

- **변경 목록.** `SceneChangeList`는 추가(`RenderInstanceDesc`), 제거(ID), 변환 갱신(local-to-world, bounds, 이전 프레임 변환 보존 표시), 재질 교체(section → 머티리얼 `RenderHandle`), 표시 플래그, 광원 추가·제거·갱신을 담는다. 프레임 packet의 가변 길이 목록이므로 프레임 arena에 둔다(RenderBase 5.2절). UE의 "큐 → 프레임 시작 일괄 적용" 구조는 같고, 게임이 만든 C++ 객체를 넘기지 않는 점이 다르다.
- **ID.** `RenderInstanceId`·`RenderLightId`는 게임 lane이 예약하는 slot+generation이다(RenderBase `RenderHandle`과 같은 방식). 계획 11.4절에 따라 ID domain에 SessionId·WorldEpoch를 넣어 EditorWorld·Tryout을 격리한다.
- **`RenderScene`.** 렌더 lane 소유 SoA 표: 변환(현재·이전), world bounds, 메시 `RenderHandle`과 section별 머티리얼, flags(그림자 caster, 정적·이동, velocity 필요, 정적 조명 수신), 정적 조명 데이터 참조, 공간 인덱스 항목(SpatialIndex, 계획 3.1). 적용 순서는 제거 → 추가 → 갱신이고 같은 프레임 안 같은 ID 명령은 목록 순서대로다.
- **인스턴스 종류.** 가상 함수 proxy 대신 렌더러가 정한 **인스턴스 종류**(정적 메시, skinned 메시, 입자 sprite 등)마다 렌더러 쪽 처리기를 등록한다. 종류별 프레임 데이터(skin pose·입자 버퍼)는 `RenderBlob`으로 넘긴다. 1차 종류는 정적 메시 하나이고(17절 RS0) skinned 메시가 RS12다.
- **그리기 항목 캐시.** 정적 인스턴스는 추가될 때 pass별 `MeshDrawItem`을 만들어 캐시한다(UE `R/PrimitiveSceneInfo.cpp:583`, 사용 `R/SceneVisibility.cpp:1269–1305`). 재질·메시·셰이더 집합 epoch가 바뀌면 그 항목만 다시 만든다.
- **이전 프레임 변환.** velocity·TAA는 이전 변환이 필요하다(계획 12.2절). `RenderScene`이 직전 적용 값을 보존하고, 이번 프레임에 변환 갱신이 없으면 이전 = 현재로 둔다. teleport 표시가 있으면 이전 = 현재로 리셋한다.
- **텍스처 사용 기록.** 렌더러는 그린 그리기 항목이 쓴 텍스처의 사용 기록마다 "마지막으로 화면에 그린 프레임 번호"를 `RenderNotices`로 돌려준다(TextureStreaming 4.1절·결정 T3). 상주 계획이 보임 신호로 쓴다(RQ21).
- **해제.** 인스턴스 제거는 렌더 lane에서 즉시 표에서 빼고, 그 인스턴스가 쓰던 GPU Scene 영역은 프레임 slot GPU 완료 뒤 재사용한다(계획 13.1절 "frame overlap 중 같은 buffer 영역을 덮어쓰지 않는다").
- **디버그·편집 표시(2026-11-23 결정, 권장안 — [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR23·CD12, 단계 RS13).** 선·점·삼각형·아이콘 sprite를 받는 **단순 요소 수집**(UE `FSimpleElementCollector`·`FBatchedElements`)을 view마다 둔다. `LineBatchComponent`·`ArrowComponent`·`BillboardComponent`·`DrawFrustumComponent` 등은 인스턴스 종류 "디버그 선 묶음"·"아이콘 sprite"로 등록하고 프레임 데이터(선 정점 목록)는 `RenderBlob`으로 넘긴다. UE는 line batch proxy가 프레임마다 동적 메시 요소를 낸다(`Engine/Private/Components/LineBatchComponent.cpp:60`). Rocks는 가상 함수 proxy 대신 종류별 처리기다. 그리는 곳은 UE와 같다(UE 경로는 `Engine/Source/Runtime/` 기준):
  - **디버그 그리기 빌드**(Debug·Development와 에디터 — UE `UE_ENABLE_DEBUG_DRAWING`, `Engine/Public/EngineDefines.h:15`): line batch를 디버그 전용 수집에 넣고(`Engine/Private/Components/LineBatchComponent.cpp:70–75`) 후처리 PC9b에서 합성한다(UE `DebugPrimitive` pass — `Renderer/Private/PostProcess/PostProcessCompositeDebugPrimitives.cpp:140, 165`).
  - **편집 표시**(에디터 view만 — gizmo·선택·EditorOnly 시각화 컴포넌트): PC9a에서 합성한다(UE `EditorPrimitive` pass — `Renderer/Private/PostProcess/PostProcessCompositeEditorPrimitives.cpp:76–127`).
  - **Shipping**: 디버그 전용 수집이 없으므로 같은 선이 일반 수집으로 가서 base pass에서 불투명 단순 요소로 그려지고(UE `Renderer/Private/BasePassRendering.cpp:1211`) depth prepass에도 쓰인다(`Renderer/Private/DepthRendering.cpp:454–455`). 처리기를 빼지 않는다.
  - **깊이 우선 그룹** 둘: World(장면 깊이로 가려진다)·Foreground(늘 위) — UE `SDPG_World`·`SDPG_Foreground`.
