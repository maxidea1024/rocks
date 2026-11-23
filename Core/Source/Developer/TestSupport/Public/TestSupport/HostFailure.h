#pragma once

#include <Base/Types.h>
#include <stdio.h>

namespace rocks::test {

// CRT stream lock 재진입 검사용 호스트 경계다. 엔진의 일반 로그/Thread API가 아니다.
// lock과 unlock은 같은 테스트 스레드에서 호출한다.
// 쓰기 전용 binary 보고서 파일을 연다. 실패는 nullptr이며 caller가 fclose한다.
FILE* OpenReportFile(const char* path) noexcept;

// 의도적으로 실패하는 자식 probe가 실패 직전에 부른다. 실패·assert·abort가 사람 입력을 기다리는
// OS·CRT 대화상자를 띄우지 않고 stderr 진단과 종료 코드로 끝나게 한다(platform::
// SuppressSystemErrorDialogs와 같다). 시험 실행기는 Launch TestRunner로 시작해 이미 꺼져 있다.
// probe가 실행기 밖에서 쓰여도 같게 동작하도록 실패 지점에서 한 번 더 부른다.
void PrepareFatalTestProcess() noexcept;
void LockStandardErrorForTest() noexcept;
void UnlockStandardErrorForTest() noexcept;

// Rocks platform::Thread를 거치지 않는 OS 스레드를 하나 띄운다(크래시 수집 시험의 "래퍼 밖
// 스레드"). 분리(detach)된 채 돌고 join하지 않는다. 만들지 못하면 false다.
bool StartUnmanagedThread(void (*entry)(void* context) noexcept, void* context) noexcept;

// 크래시 수집 시험의 CRT·heap 치명 경로 주입이다(Windows UCRT·NT heap). 정상이면 반환하지 않는다.
// 이 플랫폼에 그 경로가 없으면 아무것도 하지 않고 false다(호출자는 "지원 안 함"으로 끝낸다).
// - PassInvalidParameter: 범위 밖 파일 번호로 _close를 불러 invalid parameter 처리기로 보낸다.
// - CallPureVirtual: 기반 클래스 생성 중 순수 가상 함수를 불러 purecall 처리기로 보낸다.
// - CorruptHeap: 사설 heap 블록 머리를 덮고 해제해 STATUS_HEAP_CORRUPTION을 낸다.
bool PassInvalidParameter() noexcept;
bool CallPureVirtual() noexcept;
bool CorruptHeap() noexcept;
// 사용자 예외 코드 0xE0001234를 RaiseException으로 낸다(ASan이 치명으로 보지 않는 예외의 보고
// 시험).
bool RaiseUserException() noexcept;
// 프로세스 기본 heap(GetProcessHeap)의 블록 머리를 덮고 해제한다. 크래시한 스레드가 프로세스 heap
// lock 상태를 망가뜨린 채 보고 경로로 들어오는 경우다(프로세스 안 파일 API가 막힐 수 있다).
bool CorruptProcessHeap() noexcept;

// 크래시 수집 시험의 처리기 우회 종료 주입이다(Windows). 정상이면 반환하지 않는다. 다른 플랫폼은
// false다.
// - FailFast: __fastfail(SEH 처리기를 거치지 않는다, 종료 코드 STATUS_STACK_BUFFER_OVERRUN).
// - ExitWithControlCStatus: Ctrl-C로 끝난 프로세스와 같은 종료 코드
//   (STATUS_CONTROL_C_EXIT)로 끝낸다.
bool FailFast() noexcept;
bool ExitWithControlCStatus() noexcept;

// 크래시 수집 시험의 덤프 도우미 장치(설계 17절, Windows)를 건다. 시험 환경 변수
// ROCKS_CRASH_PROBE_HELPER의 값으로 고르고, 없으면 아무것도 하지 않고 false다. 시험 실행기가 정적
// 초기화에서 부른다(Launch 진입점이 다른 무엇보다 먼저 도우미 모드로 분기하므로). 도우미로 띄워진
// 프로세스에서만 효과가 있다. 값:
// - "hang": 고장 난 도우미. 캡처 신호를 보내지 않고 부모가 끝날 때까지 기다린다(부모의 캡처 마감
//   시험)
// - "exit-on-request": 고장 난 도우미. 크래시 요청을 받으면 캡처 없이 끝난다(중도 종료 시험)
// - "exit-now": 고장 난 도우미. 바로 끝난다(크래시 전에 끝난 도우미 시험)
// - "gate-parent-first": 실제 도우미에 gate를 건다. 도우미가 부모를 캡처한 뒤 소유 CAS 전에 부모가
//   끝날 때까지 기다린다. 부모는 캡처 마감을 넘겨 요청을 거두고 프로세스 안에서 쓴다. 도우미는
//   CAS에 져서 아무것도 쓰지 않는다.
// 모르는 값이면 고장 난 도우미가 종료 코드 2로 끝난다.
bool InstallCrashHelperTestMode() noexcept;

// 이 프로세스를 KILL_ON_JOB_CLOSE job에 넣는다(중첩 job). allowBreakaway면 JOB_OBJECT_LIMIT_
// BREAKAWAY_OK를 더한다(자식은 빠져나갈 수 있다). job 핸들은 일부러 닫지 않는다. 프로세스가 끝나면
// 핸들이 닫히고 job 안 프로세스가 함께 끝난다. harness가 job을 닫아 자식을 정리하는 경우의
// 시험용이다. Windows만이고 다른 플랫폼은 false다.
bool EnterKillOnCloseJob(bool allowBreakaway) noexcept;

// 이 프로세스를 breakaway를 허락하지 않는 job에 넣는다(제한 없음, KILL_ON_JOB_CLOSE 없음, 중첩
// job). 그 안에서 만든 job이 breakaway를 허락해도 CREATE_BREAKAWAY_FROM_JOB은 실패한다(바깥 job이
// 막는 CI 러너 조건을 재현한다 — Docs/Design/CrashHandlingAndLaunch.ko.md 17절). job 핸들은 일부러
// 닫지 않는다. Windows만이고 다른 플랫폼은 false다.
bool EnterBreakawayBlockingJob() noexcept;

// 프로세스 안 덤프가 스레드 목록 단계에 들어오면(IncludeThreadCallback) 다른 스레드가 프로세스 heap
// lock(HeapLock)을 잡고 영원히 멈추게 한다(gate로 순서를 맞춘다). 덤프가 다른 스레드를 멈추는 순간
// heap lock이 쥐어져 있는 장면을 결정적으로
// 만든다(Docs/Design/CrashHandlingAndLaunch.ko.md 11.5절). 크래시 수집 설치 뒤, 크래시 전에 부른다.
// Windows만이고 다른 플랫폼은 false다.
bool HoldProcessHeapDuringDump() noexcept;

// 처리되지 않은 예외 filter 교환 확인(platform::CheckUnhandledCrashFilter) 시험용이다(Windows).
// Replace는 시험 filter로 바꾸고 바꾸기 전 filter를 기억한다. IsCurrent는 시험 filter가 지금
// filter인지 교환으로 확인한다(확인 뒤 그대로 둔다). Restore는 기억한 filter로 되돌린다. 다른
// 플랫폼은 false·아무 일도 하지 않는다.
bool ReplaceUnhandledFilterForTest() noexcept;
bool TestUnhandledFilterIsCurrent() noexcept;
void RestoreUnhandledFilterForTest() noexcept;

} // namespace rocks::test
