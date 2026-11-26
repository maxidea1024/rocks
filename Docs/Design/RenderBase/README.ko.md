# 렌더 기반(RenderBase) 설계

기준일: 2026-11-26, 개정 53(초안). UE 5.8 RenderCore 모듈이 맡는 일을 Rocks에 세우는 구현 스펙이다. 범위는 다음과 같다.
- 렌더 lane과 프레임 전달, 장치 스레드 모델, 장치 자원 수명, 렌더 그래프
- 셰이더 런타임 객체(셰이더 집합·파라미터 바인딩·상수 블록·파이프라인 캐시)
- 정점 입력 타입, 공용 자원·그리기 유틸리티, GPU 계측
- 이것들을 올리기 전에 Core·Graphics가 갖춰야 할 **선행 작업**

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론) 원본이다. 경로는 `Engine/Source/Runtime/` 기준이며 `RenderCore/` 아래 파일은 `RC/`로 줄여 적는다. 셰이더 원문은 `Engine/Shaders/`, 플러그인은 `Engine/Plugins/` 기준이다. Rocks 경로는 저장소 루트 기준이고 HEAD `79f966a`에서 확인했다(초안 기준 `1638a7f`와 인용 파일 내용이 같다).

[셰이더 시스템](../ShaderSystem/README.ko.md)(개정 8, 구현 전)과 겹치는 부분은 그 문서의 결정을 따른다. RenderCore 런타임 요구 때문에 그 문서를 고쳐야 하는 곳은 9.9절에 모았다. [엔진 구현 계획](../EngineImplementationPlan.ko.md) 3·8·9.6·11절, [엔진 런타임](../EngineRuntime/README.ko.md) 6·10–14절과 충돌하는 부분은 이 문서 기준으로 정리하고 그 문서들도 함께 고친다(15절).

상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음). 이 문서는 **설계**다. 화면 마일스톤(18.1절)에서 구현하며, 미리 해서 유리한 Core 선행 작업만 먼저 착수했다(16절 결정 R10). 다만 결정 R12(샘플러 위치)와 SX13(비교 샘플러)은 A0-G에 걸리므로 **A0-G 착수 전**에 정해 AssetSystem에 반영해야 한다(6절의 기한 가운데 가장 이르다).

**용어 방침.** UE 구조는 따르되 이름은 Rocks 이름을 쓴다. UE의 모듈·타입·함수·매크로 이름은 대응표(2절)와 "UE 5.8 방식" 표·근거 열에만 적는다. Rocks 규칙을 적는 본문에는 UE 이름 대신 근거 `파일:줄`만 단다. pass·barrier·pipeline·descriptor·render target·swap chain·fence·SRV·UAV처럼 그래픽 API와 업계에서 함께 쓰는 말은 그대로 쓴다. 계획 문서에 이미 있는 Rocks 이름(`RenderGraph`·`RenderResourceLease`·`Retire`·`GpuFencePoint`·`DeviceCaps`·`BuiltinId`·`BuiltinReady`)도 그대로 쓴다.

**UE 레거시 제외.** Rocks는 새로 만드는 엔진이므로 UE가 하위 호환을 위해 남긴 코드(deprecated API·옛 경로·이전 버전 형식 대응·전환기 이중 경로)는 조사·설계 대상이 아니다(2026-11-16 사용자 지시). UE의 현재 기본 경로만 근거로 삼고 UE 버전 사이의 변경 이력은 적지 않는다.

## 목차

- [1. 결정 요약](01-Decisions.ko.md)
- [2. 용어 대응](02-Terms.ko.md)
- [3. UE 5.8 RenderCore 구성과 Rocks 배치](03-UeLayout.ko.md)
- [4. 현재 코드베이스와 선행 작업](04-Prerequisites.ko.md)
- [5. 렌더 lane](05-RenderLane.ko.md)
- [6. 장치 자원 수명](06-ResourceLifetime.ko.md)
- [7. 렌더 그래프](07-RenderGraph.ko.md)
- [8. 렌더 그래프 단계와 완료 기준](08-RenderGraphStages.ko.md)
- [9. 셰이더 런타임](09-ShaderRuntime.ko.md)
- [10. 정점 입력 타입](10-VertexInput.ko.md)
- [11. 공용 자원·그리기 유틸리티](11-CommonResources.ko.md)
- [12. 콘솔 설정(CP4·CP8)](12-ConsoleSettings.ko.md)
- [13. GPU 계측(RB7)](13-GpuTrace.ko.md)
- [14. 후속·범위 밖](14-OutOfScope.ko.md)
- [15. 다른 문서 개정](15-OtherDocRevisions.ko.md)
- [16. 결정 필요 사항](16-OpenDecisions.ko.md)
- [17. 측정](17-Measurements.ko.md)
- [18. 화면 마일스톤과 구현 순서](18-ScreenMilestone.ko.md)
- [19. 개정 이력](19-Revisions.ko.md)
