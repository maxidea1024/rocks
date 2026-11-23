# Sparks — Rocks 게임플레이 언어

정적 타입 · C 계열 문법 · 레지스터 VM · 비프로그래머 대상의 방어적 설계

**설계 기준일 2026.11.20 · 초안 1.5 · 상태: 설계 제안 / 미구현 · 성능 미측정**

이 문서는 [전체 계획](../EngineImplementationPlan.ko.md) §23이 요구하는 Rocks 게임플레이 언어 Sparks를 정의한다. 프로그래머가 아닌 사용자도 쓸 수 있도록 Go처럼 단순하게, 실수할 여지가 적게, 메모리·리소스 누수가 생길 수 없는 구조로 설계한다. 기준은 엔진 구현 상태가 아니라 전체 계획 §23과 이 언어가 연결되는 전체 계획 §5·§6·§8·§9·§10·§16·§17·§20·§21·§24·§29의 계약이다. 이 문서의 문법·이름·API는 설계 제안이며 구현·측정 결과가 아니다. 예시의 엔진 API 이름(`AddMovement`, `World.Spawn` 등)은 바인딩 규칙을 보이기 위한 예시이며 실제 이름은 각 모듈의 C++ 선언에서 생성된다. 아직 엔진에 없는 게임 타입(`Character`, `Collider`·`SphereCollider`, `SoundAsset`, `Sequence`, `DataAsset`, `ScriptableComponent`, `WidgetController`, `PlaySession`)은 자리표시 이름이며 해당 모듈이 들어올 때 확정한다(§17.2).

초안 1.3은 구현 착수 전 스펙 완비를 위한 개정이다. 엔진 런타임(Actor·World 코드)·Prefab 개정 3·리플렉션 B9·주변 시스템 스펙(로깅·CVar·입력·UI·콘텐츠 서명)과 이름·시점을 맞추고, 사용자 핵심 요구 3가지 — **핫 리로드 필수(§12.4), 오류 방어(§8.4), 선택형 native AOT(§12.5, Unity IL2CPP와 같은 backend 선택)** — 를 규격으로 확정하였다. 미정 값(전달 수 상한·예산·interface·CU9)을 정하고 표준 API(부록 B)를 추가하였다. 구현 순서는 초안 1.5의 §18을 따른다.

초안 1.5(2026.11.20)는 구현 순서를 바꾼 개정이다. 엔진 요소와 무관한 언어 코어(LC0–LC9)를 먼저 구현하고 엔진 연동(EI1–EI9)은 엔진이 준비되는 대로 더한다(§18, SD51–SD53). 엔진·도구 코드에 필요한 사항은 [Sparks 엔진 연동 요구 목록](../SparksEngineIntegration.ko.md)으로 분리하였다.

초안 1.4(2026.11.20)는 레벨마다 하나인 연결 스크립트(UE Level Blueprint 대응)를 기반 타입 `LevelScript`로 더한 개정이다. 정본은 [레벨 스크립트](../LevelScript.ko.md)(결정 LD1–LD20)이고, 이 문서에는 언어 쪽 요지(기반 타입·참조 형태·수명 순서·엔진 Actor 물리 이벤트·편집기 명령·진단 코드)만 두며 결정은 SD46–SD50으로 요약한다.

| 항목 | 내용 |
|---|---|
| 기준 문서 | 전체 계획 §23 · [엔진 런타임](../EngineRuntime/README.ko.md)(이하 ER) 2·6–8절과 World 코드 · [Prefab](../Prefab.ko.md) 개정 3 · [레벨 스크립트](../LevelScript.ko.md) 개정 1 · [런타임 클래스 메타데이터](../RuntimeClassMetadata.ko.md)(이하 RCM) · [리플렉션 B9](../ReflectionExtensions.ko.md)(이하 RX) · [입력 상세안](../CoreObjectInputDetailedPlan.ko.md) · [UI 툴킷](../UiToolkit.ko.md) CU1–CU9 · [로깅](../LoggingSystem.ko.md) · [CVar](../CVarSystem.ko.md) · [콘텐츠 서명](../ContentSigning.ko.md) · [Clang 생성 Reflection](../../Implementation/ReflectionMetadata.ko.md)(구현 상태) |
| Unity·Verse 비교 기준 | Unity 수명 hook 체계(종류·호출 순서·의미). Verse는 The Book of Verse[R19] 기준이며 언어 설계의 참고 대상이다. Rocks의 Unreal 기준은 UE 5.8(5.8.4, commit `f45729c0b6f0`) |
| Rocks 상태 | Sparks 언어·VM·컴파일러 미구현. Core C5(`NamePool::TryFind`)는 구현됨 |
| 식별자 | 도입 단계 L0–L4, 언어 코어 마일스톤 LC0–LC9, 엔진 연동 단계 EI1–EI9(요구 항목 SE-*), Core 변경 C1–C5, 리플렉션 확장 CU9, Gate G1–G6, 결정 SD1–SD50, 진단 코드 S0100–S1199, 부록 A·B, 참고 자료 R1–R31 |

## 장 목차

- [0. 핵심 결정](00-KeyDecisions.ko.md)
- [1. 요구와 설계 원칙](01-Requirements.ko.md)
- [2. 예시와 표기 반영](02-Examples.ko.md)
- [3. 문법](03-Grammar/README.ko.md)
- [4. 타입·참조·값](04-TypesAndReferences.ko.md)
- [5. 수명 hook과 이벤트](05-LifecycleAndEvents.ko.md)
- [6. 시간과 async](06-TimeAndAsync.ko.md)
- [7. 유지 자원과 비동기 로딩](07-HeldResourcesAndLoading.ko.md)
- [8. 오류 처리](08-ErrorHandling.ko.md)
- [9. 소유·해제와 누수 방지](09-OwnershipAndLeaks.ko.md)
- [10. 엔진 바인딩](10-EngineBinding.ko.md)
- [11. VM](11-Vm.ko.md)
- [12. 컴파일·핫 리로드·eval·backend](12-CompileAndReload.ko.md)
- [13. 언어 서버와 편집기](13-LanguageServer.ko.md)
- [14. 네트워크](14-Network.ko.md)
- [15. 저장·데이터·편집 연동](15-SaveAndData.ko.md)
- [16. Verse 검토와 차용](16-VerseReview.ko.md)
- [17. 스펙 개정 대상](17-SpecRevisions.ko.md)
- [18. 도입 단계](18-Phases.ko.md)
- [19. 결정 사항](19-Decisions.ko.md)
- [20. 교차 검증 기록](20-CrossValidation.ko.md)
- [21. 참고 자료](21-References.ko.md)
