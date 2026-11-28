# 렌더러(Renderer) 설계

기준일: 2026-11-28, 개정 18(초안 — 21장 교차 리뷰 반영). 개정 17(초안 — 21.10절 TBDR: prepass 선택·메모리 없는 attachment RQ24). 개정 16(초안 — 21장 RS0a 설계: RendererShaders·pass 타입·위치 전용 depth·traits 거르기·정적 메시 정점 입력·GPU Scene 항목 10 float4·격자 2^21 cm·View·Scene RS0 멤버). 개정 15(초안 — 17절 RS0을 RS0a 렌더 단독·RS0b 머티리얼 사용으로 나누고 머티리얼 최소 선행을 적음, 머티리얼 세션 질문 2026-11-28). 개정 14(2026-11-28). UE 5.8 Renderer 모듈이 맡는 일 가운데 **렌더 경로**(Forward+·Deferred)와 그 위에 올라가는 그림자·투명·하늘·안개·후처리의 구조, GI 두 축과 PathTracing 모드의 연결점을 Rocks에 세우는 설계 스펙이다. 범위는 다음과 같다.
- 장면 표현과 게임 → 렌더 전달, view와 가시성
- 공통 frame graph, Forward+와 Deferred가 갈라지는 곳
- GBuffer layout, light grid, 조명 평가, 그림자, 투명·하늘·안개, 후처리·업스케일 연결
- GI 두 축과 PathTracing 모드의 경계(방식 내부는 각 방식의 스펙이 정한다)
- backend 지원표, 렌더러가 머티리얼에 요구하는 계약, 렌더 그래프·셰이더 타입 사용 방식, 성능 예산과 계측
- 단계·완료 기준, 선행 작업에 반영할 요구

**근거 표기.** UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론) 원본이다. 경로는 `Engine/Source/Runtime/` 기준이며 다음 약어를 쓴다.

| 약어 | 경로 |
|---|---|
| `R/` | `Renderer/Private/` |
| `RPub/` | `Renderer/Public/` |
| `DSR` | `Renderer/Private/DeferredShadingRenderer.cpp` |
| `RC/` | `RenderCore/` |
| `RU` | `RenderCore/Private/RenderUtils.cpp` |
| `EN/` | `Engine/` |
| `SH/` | `Engine/Shaders/Private/`(셰이더 원문, `Engine/Source` 밖) |

`Editor/`로 시작하는 경로는 `Engine/Source/Editor/` 기준이다. Rocks 경로는 저장소 루트 기준이고 `44bb1d2`(형식 정의 JSON은 `origin/feature/engine-runtime` `6eeba23`)에서 확인했다. 확인하지 못한 사실은 그 자리에 **미확인**으로 적고 19.2절에 모은다.

**상태.** 이 문서는 **설계**다. 정식 렌더러는 화면 마일스톤 다음 단계다([렌더 기반](../RenderBase/18-ScreenMilestone.ko.md) 18.1절 "범위 밖" 첫 항목). 상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음).

**용어 방침.** UE 구조는 따르되 이름은 Rocks 이름을 쓴다. UE 타입·함수·설정 변수 이름은 용어 대응(2절)과 "UE 5.8 방식" 표·근거 열에만 적는다. GBuffer·Forward+·clustered·light grid·HZB·CSM·shadow atlas·PCF·TAA·tone mapping처럼 업계에서 함께 쓰는 말은 그대로 쓴다. 계획·RenderBase에 이미 있는 Rocks 이름(`RenderPath`·`ViewDesc`·`RenderInstanceId`·`StaticLighting`·`DynamicGiMethod`·`RenderMode`·`Upscaler`·`RayScene`·`GpuScope`·`FramePacket`·`RenderHandle`)은 그대로 쓴다.

**UE 레거시 제외.** UE가 하위 호환이나 제거 예정으로 남긴 경로는 근거로 삼지 않는다(2026-11-16 사용자 지시). 이 문서에 해당하는 것: clustered deferred shading(제거 예정, "low utility and thus use", `R/ClusteredDeferredShadingPass.cpp:36–46`), SSGI(5.8부터 deprecated, `R/IndirectLightRendering.cpp:599`, `EN/Classes/Engine/EngineTypes.h:463`), 레거시 RT 반투명(`EN/Classes/Engine/Scene.h:116`), 독립 RT 반사(선언만 있고 정의 없음, `R/DeferredShadingRenderer.h:1010, 1018`). 다만 UE가 "Legacy"라고 이름 붙인 GBuffer layout 함수(`RC/Private/GBufferInfo.cpp:247`)는 5.8 새 프로젝트 기본값(Substrate Blendable GBuffer)이 그대로 쓰는 현재 경로이므로 근거로 쓴다(7.1절).

## 목차

- [1. 결정 요약](01-Decisions.ko.md)
- [2. 용어 대응](02-Terms.ko.md)
- [3. 모듈 배치](03-Modules.ko.md)
- [4. 장면 표현과 게임 → 렌더 전달](04-SceneRepresentation.ko.md)
- [5. view와 visibility](05-ViewVisibility.ko.md)
- [6. 공통 frame graph](06-FrameGraph.ko.md)
- [7. Deferred](07-Deferred.ko.md)
- [8. Forward+](08-ForwardPlus.ko.md)
- [9. 그림자](09-Shadows.ko.md)
- [10. 투명·하늘·안개](10-TranslucencySkyFog.ko.md)
- [11. 후처리와 업스케일 연결점](11-PostProcess.ko.md)
- [12. GI 두 축과 PathTracing 모드의 연결점](12-GlobalIllumination.ko.md)
- [13. backend 지원표](13-BackendSupport.ko.md)
- [14. 렌더러가 머티리얼에 요구하는 계약](14-MaterialContract.ko.md)
- [15. 렌더 그래프·파라미터 구조체·셰이더 타입 사용 방식](15-GraphParametersShaderTypes.ko.md)
- [16. 성능 예산과 계측](16-PerformanceBudget.ko.md)
- [17. 단계와 완료 기준](17-Stages.ko.md)
- [18. 선행 작업에 반영할 요구](18-Requirements.ko.md)
- [19. 결정 필요 사항과 미확인 목록](19-OpenDecisions.ko.md)
- [20. 개정 이력](20-Revisions.ko.md)
- [21. RS0a 설계](21-RS0aDesign.ko.md)
