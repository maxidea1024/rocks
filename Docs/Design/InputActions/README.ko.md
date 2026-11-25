# 입력 액션 계층 설계(CR21)

기준일: 2026-11-25, 개정 4(IA2 착수 전 보충·설계 리뷰 반영 — 객체와 평가기 데이터 사본의 경계 2.7절, 바인딩의 액션 참조는 Guid, `UpdateContext`, 액션 표 결정 순서, 액션 상태 짝짓기, 주입 액션 데이터, 키 이름 저장, PlayerController의 lease, IA2 세 단계, IAD10·IAD11). 개정 3(IA1 구현 리뷰 반영 — float 계산 예외·Enter와 숫자패드 Enter·DeadZone 임계 검증·FOV 문맥·chord 대상 표·RepeatedTap 시계). 개정 2(UE 원본 대조 리뷰 반영 — 값 변환·패드 원시 축·시간 dt·키 이벤트·정렬·chord·전달 순서·slot·커브·동적 진동·자원·오류). [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR21의 설계다.

범위:
- 입력 액션·매핑 컨텍스트·리바인딩·햅틱 출력(게임패드 진동)이다.
- UE 5.8 Enhanced Input 플러그인과 force feedback(`UForceFeedbackEffect`·`APlayerController::ClientPlayForceFeedback`)에 해당한다.

원칙:
- **구조는 UE 5.8과 같다.** 사용자 결정: 애매하면 UE 방식을 따른다.
- **이름은 Rocks 이름이다.** UE 이름은 1절 대응표와 근거에만 적는다.
- 상세 계획 [CoreObjectInputDetailedPlan](../CoreObjectInputDetailedPlan.ko.md) 21절에서 이 설계와 맞는 부분을 따른다.
  - 안정 ID와 표시 이름을 분리한다.
  - phase 이름은 Started·Ongoing·Triggered·Completed·Canceled다.
  - 시간 기준은 Tick이다.
  - 같은 우선순위의 순서는 안정 순서로 정한다.
- UE와 다른 일반화(processor 등록, 컴파일된 profile, 임의 함수 그래프)는 들이지 않는다(10절 IAD1).

UE 근거 경로 약어:
- `EI/` = `Engine/Plugins/EnhancedInput/Source/EnhancedInput/`
- `RT/` = `Engine/Source/Runtime/`

UE 버전은 5.8.4다.

## 목차

1. [이름과 모듈](01-NamesModules.ko.md)
2. [데이터 모델](02-DataModel.ko.md)
3. [모디파이어·트리거](03-ModifiersTriggers.ko.md)
4. [평가](04-Evaluation.ko.md)
5. [게임플레이 연결·리바인딩](05-GameplayRebinding.ko.md)
6. [햅틱](06-Haptics.ko.md)
7. [단계·결정·시험](07-StagesDecisions.ko.md)
