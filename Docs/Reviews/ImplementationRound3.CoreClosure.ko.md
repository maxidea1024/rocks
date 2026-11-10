# 구현 교차 검토 3차 — IC01–IC04 수정 확인

검토일: 2026-11-10. 범위는 이전 `ImplementationRound2.CoreReview.ko.md`의 IC01–IC04, 해당 회귀 테스트 및 직접 영향을 받는 소유권·진단 경계다. 수정 코드를 새로 읽고 기존 반례를 다시 대입하였다. 이번 검토에서 production 소스와 테스트 소스는 수정하지 않았다.

## 1. 판정과 실행 증거

**IC01–IC04 모두 해결 확인. 이 범위의 미해결 지적은 0건이다.** 새 수정으로 인한 직접 회귀는 발견하지 않았다.

Windows 실행 근거는 `out/windows-clang-debug/foundation-report.json`과 `out/windows-clang-debug/Testing/Temporary/LastTest.log`를 직접 확인하였다. 2026-11-10 14:46 KST 실행 기록이며 seed 7351, repeat 1, Foundation 51개 테스트, 실패 0이다. CTest의 foundation, harness_negative_controls, runtime_stl_audit, scaffold_transaction 4개 항목도 모두 통과로 기록되어 있다. 실행 바이너리의 마지막 수정 시각은 14:46:05이며 이 검토자는 해당 로그를 근거로 판정하였다. 이번 closure 과정에서 같은 테스트를 다시 실행한 것은 아니다.

| 환경·확인 | 상태 |
|---|---|
| Windows Clang Debug · Foundation 51개 | PASS — 저장된 실제 JSON/CTest 로그 확인 |
| Windows 자식 프로세스 실패 경로·병렬 집계 | PASS — harness_negative_controls 실제 로그 확인 |
| macOS native 실행·macOS ASan | NOT RUN — 이번 검토에 실행 증거 없음 |
| Windows sanitizer | NOT RUN — 위 Debug 로그만으로 sanitizer 통과를 주장하지 않음 |
| 전체 R2·병렬 Task Graph·동적 DLL unload | 범위 밖 — 초기 subset의 통과와 구분 |

## 2. IC01 — Function·ModuleLease 재진입 대입 누수

**코드:** `Engine/Core/Public/Core/Function.h:26–33`, `Engine/Core/Public/Core/Lifetime/ModuleLease.h:49–59`.

Function은 현재 상태를 previous에 분리한 뒤 incoming을 target에 설치한다. previous가 파괴될 때만 old callable destructor를 실행하며 그 이후 target을 다시 덮어쓰지 않는다. ModuleLease도 같은 순서로 old lease를 이전하고 incoming을 먼저 게시한다.

이제 A의 destructor/release가 prebuilt C를 target에 설치하면, 재진입 assignment가 당시 target의 B를 정리하고 C를 유지한다. 바깥 assignment 복귀 시 C를 덮는 후속 쓰기가 없다. A/B/C 모두 독립 owner에서 정확히 한 번 정리된다. self move는 기존 동일 주소 분기로 무효 연산을 유지한다.

**회귀:** `Tests/Foundation/ReentryTests.cpp:64–109`는 준비된 C를 destructor/release에서 이동하고, C가 최종 선택되는지·A/B가 이미 반환되었는지·최종 reset 후 live allocation/reference가 0인지 확인한다. JSON에는 아래 두 사례가 각각 PASS로 기록되어 있다.

- Function_ReentryDuringOldCaptureDestructionKeepsNewOwner
- ModuleLease_ReentrantOldReleaseDoesNotLoseReplacement

**판정: 해결.** 이 결과를 객체가 이미 파괴되는 중에 임의로 자기 자신을 부활시키거나, 실행 중 callable을 아무 규약 없이 파괴해도 안전하다는 일반 보장으로 확대하지 않는다.

## 3. IC02 — assertion의 CRT stderr lock 교착

**코드:** `Engine/Core/Private/Diagnostics.cpp:5–32`.

AssertFailed의 fprintf/fflush 의존이 제거되었다. 2,048-byte stack buffer, 제한 길이 append, uint32 line의 10자리 이내 정수 변환을 사용하고 마지막에 Platform::Fatal을 호출한다. buffer의 마지막 null 공간을 확보하며 일반 logger·scheduler·동적 allocation을 호출하지 않는다.

**회귀:** `Tests/Foundation/FailureControls.cpp:66–88`는 main thread에서 stderr stream lock을 보유한 상태로 assertion worker를 실행한다. `Tests/Integration/test_harness.py:27–29`는 자식 프로세스의 10초 timeout, 비정상 종료 및 invariant 진단 출력을 검사한다. Windows fixture는 자동화 중 CRT abort 대화상자를 피하도록 abort 동작을 설정한다. LastTest.log에는 해당 locked-stderr 검증의 성공 메시지와 CTest PASS가 있다.

**판정: 해결.** 이 테스트는 지적한 CRT FILE lock 의존을 검사한다. 모든 OS 출력 장치·crash 상황에서 진단이 반드시 완전하게 저장된다는 보장은 아니다.

## 4. IC03 — native worker failure counter 경합

**코드:** `Tests/Foundation/Main.cpp:12,20–23,82–84`.

RecordFailure가 Platform Mutex의 LockGuard 아래에서 failure counter와 진단 출력을 갱신한다. plain uint64에 대한 여러 worker의 동시 쓰기 경로가 제거되었다. negative control은 모든 native worker를 join한 뒤 집계를 읽는다.

**회귀:** `Tests/Foundation/FailureControls.cpp:15–18,44–64`에서 4개 worker가 각각 50번 실패를 기록한다. Main은 정확히 200건인 경우에만 예상 실패 exit 1을 반환하고, Python은 exit 1과 stderr 실패 기록 200개를 함께 확인한다(`test_harness.py:24–26`). 저장된 harness_negative_controls 로그가 PASS다.

**판정: 해결.** 일반 테스트도 자체 worker를 종료·join한 후 반환해야 한다. 테스트가 반환한 뒤에도 비동기 worker를 방치하는 수명 오류를 이 mutex가 자동 복구하지는 않는다.

## 5. IC04 — ProfileScope 이름 잘림 정보 유실

**코드:** `Engine/Core/Private/Diagnostics/ProfileScope.cpp:8–22`, `Engine/Core/Private/Diagnostics/BootstrapLog.cpp:9–11,35–36`.

ProfileScope가 원본 이름의 추가 문자가 존재하는지 nameTruncated_로 보존하고 begin/end 모두에 전달한다. LogBuffer는 자체 복사 중 잘림과 alreadyTruncated를 OR하므로, 이미 잘린 이름을 전달해도 false로 되돌리지 않는다. source 길이 119에서는 false, 120 이상에서는 true가 되는 경계도 소스의 반복 조건과 마지막 검사로 확인하였다.

**회귀:** `Tests/Foundation/ReentryTests.cpp:112–125`가 199-byte 이름의 begin/end truncated flag를 확인한다. JSON의 Diagnostics_ProfilePreservesNameTruncationFlag 및 기존 Diagnostics_ProfileAndTruncation은 PASS다. 119/120 경계는 이번 실행에서 별도 이름의 테스트 사례로 집계하지 않았으며 소스 검토로 확인한 범위다.

**판정: 해결.** 잘린 이름의 완전한 identity 복원이나 전문 profiler viewer 완성을 의미하지 않는다.

## 6. 종료 범위

이 보고서는 네 지적의 수정 및 직접 회귀만 판정한다. Span의 Derived 배열 변환 수정은 별도 build 담당 검토의 대상이며 여기서 중복 closure하지 않았다. 추가 기능 범위나 초기 Foundation 완료 기준을 확대하지 않았다.

후속 구현에서는 현재 테스트와 실패 제어를 회귀군에 유지한다. macOS·sanitizer·더 넓은 동시성/모듈 수명 검증은 실제 실행 근거가 생기기 전까지 NOT RUN으로 남긴다.
