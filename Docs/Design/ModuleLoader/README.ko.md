# 모듈 로더 설계

기준일: 2026-11-23, 개정 11. 모듈의 수명(시작·종료)과 적재를 맡는 모듈 로더를 정한다. UE 5.8 `IModuleInterface`·`FModuleManager`·로딩 단계(`ELoadingPhase`)에 해당한다.

[빌드 타깃 설계](../BuildTargets/03-Design/03.08-LinkType.ko.md) 3.8절은 Editor 타깃을 UE와 같이 모듈식(`Modular`)으로 링크하기로 정했다(BD17). 그리고 모듈 로더를 별도 스펙으로 넘기며 네 가지 계약을 요구했다(BD30).
1. 같은 소스가 `Monolithic`·`Modular`에서 같은 순서·결과로 동작한다.
2. 모듈 시작은 의존 순서, 종료는 역순이다. 리플렉션 타입 등록은 모듈 시작에 묶는다.
3. Rocks Studio는 프로젝트 기술자와 플러그인 활성 조건을 읽어 게임·플러그인 모듈을 실행 중에 적재한다.
4. 적재 실패는 원래 OS 오류 코드를 보존한 진단으로 끝낸다.

이 문서가 그 스펙이다. 설계가 애매한 곳은 UE 방식을 따르고, Rocks 원칙(전역 생성자 금지, `Result` 기반 오류, 명시적 소유)과 부딪히는 곳만 다르게 정한다(7절).

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`, commit `f45729c0b6f0`)이고, 경로는 `Engine/Source/` 기준이다. UE 이름은 대응표와 근거에만 적는다. hot reload·Live Coding(코드 다시 불러오기)은 범위 밖이다(9.2절).

상태: **설계 초안**(결정 MD1–MD23, 모두 권장안). 구현 단계는 ML0–ML6(6절)이며 착수 시점은 사용자가 정한다. ML0 구현·병합(개정 2에 구현 세부). ML2는 ML1(Launch 병합 대기) 전에 하는 연결을 3.10절(개정 3)에 정했고, [리플렉션 생성 소스](../ReflectionGeneratedSources.ko.md) GS1 병합 뒤 그 구조 위에서 구현했다(2026-11-20 — ML1 뒤로 넘긴 것은 3.10절 끝). ML3(`DynamicModules`)은 개정 4에서 UE 근거로 순환 규칙을 고쳐(3.8절) 구현했다. ML1(단계 실행·묶음·Launch 통합)은 crash-launch 병합 뒤 개정 5에서 3.11절로 정해 구현했고, 그 후속(호스트 서비스·서비스를 쓰는 `Startup` 등록)은 개정 6 3.12절이다. ML0–ML4는 Rocks Studio(BS5) 전에, ML5는 BS4·BS5와 함께 한다. Modular 적재(ML4)는 [빌드 타깃](../BuildTargets/README.ko.md) BS8 뒤다. 초안을 서브에이전트 1개로 교차 검증해 반영했다(10절).

## 목차

- [1. 현재 상태](01-CurrentState.ko.md)
- [2. 용어 대응](02-Terminology.ko.md)
- [3. 설계](03-Design/README.ko.md)
- [4. 결정 요약](04-DecisionSummary.ko.md)
- [5. 결정 사항](05-Decisions.ko.md)
- [6. 단계](06-Stages.ko.md)
- [7. UE와 다르게 정한 점](07-DifferencesFromUnreal.ko.md)
- [8. 다른 문서 개정](08-OtherDocRevisions.ko.md)
- [9. 범위 밖과 채택하지 않은 UE 요소](09-OutOfScope.ko.md)
- [10. 개정 이력](10-Revisions.ko.md)
