# 작업 진행 보고와 취소 설계

기준일: 2026-11-27, 개정 6(PG5의 렌더 세션 몫 완료 — 셰이더 컴파일 관리자·`shader-compile`·Development 공급자). 이전 개정 5(PG1–PG4 완료 — 시작 화면 세션, 사용자 결정 2026-11-27). 이전: 2026-11-23, 개정 1(초안, 교차 검증 1차 반영). 사용자 지시("시스템 전반에 걸쳐 에디터에서 프로그레스 보여주는 체계가 있어야 함. 셰이더만의 문제가 아님", "스펙만 잡아놓고 다른 세션에서 작업")로 쓴 설계다. 이 문서를 쓴 렌더 세션은 구현하지 않는다. 결정은 권장안으로 확정했다(사용자 기준 "결정사항들은 권장안대로", 애매하면 UE 5.8 방식).

**범위.** 오래 걸리는 작업(임포트·쿠킹·에셋 너겟 일괄 요청·셰이더 컴파일·텍스처/오디오/메시 빌드·언리얼 교환·Sparks 컴파일·저장)이 진행과 취소를 한 가지 방식으로 알리게 한다. 받는 쪽은 나중에 만들 에디터(진행 창·알림·상태 표시줄), 명령줄 도구(사람이 읽는 줄), 자동화(기계 판독 줄), trace다. 이 문서는 데이터·스레드·프로세스 경계 계약과 API를 정한다. 에디터 화면 모양은 계약의 소비자로만 적는다(UiToolkit·에디터 단계에서 그린다).

**착수 시점.** 고정 구현 순서(에셋 A4–A6 → 화면 마일스톤)를 바꾸지 않는다. 단계(PG — 13절)의 착수와 담당 세션은 사용자가 정한다. 지금 진행 중인 작업은 12절 "지금 지킬 것"으로 이 계약과 어긋나지 않게 한다.

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)다. 경로는 `Engine/Source/` 기준이다. 레거시·deprecated API(`BeginSlowTask` 등)는 근거로 쓰지 않았다.

| 약자 | 파일 |
|---|---|
| `ST` / `STC` | `Runtime/Core/Public/Misc/SlowTask.h` / `Runtime/Core/Private/Misc/SlowTask.cpp` |
| `SST` | `Runtime/Core/Public/Misc/ScopedSlowTask.h` |
| `STS` / `STSC` | `Runtime/Core/Public/Misc/SlowTaskStack.h` / `Runtime/Core/Private/Misc/SlowTaskStack.cpp` |
| `FC` / `FCC` | `Runtime/Core/Public/Misc/FeedbackContext.h` / `Runtime/Core/Private/Misc/FeedbackContext.cpp` |
| `ATN` / `CATN` | `Runtime/Core/Public/Misc/AsyncTaskNotification.h` / `Runtime/Core/Public/Misc/CoreAsyncTaskNotificationImpl.h` |
| `NM` | `Runtime/Slate/Public/Framework/Notifications/NotificationManager.h` |
| `IACM` / `ACM` / `ACMC` | `Runtime/Engine/Public/IAssetCompilingManager.h` / `Runtime/Engine/Public/AssetCompilingManager.h` / `Runtime/Engine/Private/AssetCompilingManager.cpp` |
| `ACH` | `Runtime/Engine/Public/AsyncCompilationHelpers.h` |
| `SCM` | `Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp` |

## 목차

- [1. UE 5.8의 구조](01-UnrealStructure.ko.md)
- [2. 요구](02-Requirements.ko.md)
- [3. 구조 개요](03-Overview.ko.md)
- [4. 취소 표지(`CancelToken`)를 Core로](04-CancelToken.ko.md)
- [5. 작업](05-Tasks.ko.md)
- [6. 바인딩·구간·계수](06-BindingScopeCounter.ko.md)
- [7. 중계(`ProgressHub`)](07-Hub.ko.md)
- [8. 대기 작업 등록소(`PendingWorkRegistry`, Engine 모듈)](08-PendingWork.ko.md)
- [9. 받는 쪽](09-Consumers.ko.md)
- [10. 프로세스 경계](10-ProcessBoundary.ko.md)
- [11. 한도 상수](11-Limits.ko.md)
- [12. 적용 대상과 지금 지킬 것](12-Adoption.ko.md)
- [13. 단계](13-Stages.ko.md)
- [14. 시험](14-Tests.ko.md)
- [15. 한계와 후속](15-Limitations.ko.md)
- [16. 버린 대안](16-Alternatives.ko.md)
- [17. 대응표](17-UnrealMapping.ko.md)
- [개정 이력](18-Revisions.ko.md)
