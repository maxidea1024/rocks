#pragma once

#include "CrashFiles.h"

// Windows 덤프 도우미다(Docs/Design/CrashHandlingAndLaunch.ko.md 11절). 설치 때 같은 실행 파일을
// 도우미 모드로 띄운다. 넘기는 핸들(교환 블록 section, 부모 프로세스, 이벤트 다섯)은 상속
// 목록으로만 넘기고 생성 직후 상속 표시를 끈다. 크래시 때 부모의 크래시 처리 스레드가 기록을 교환
// 블록에 쓰고 요청하면, 도우미가 폴더·예비 CrashInfo.json을 쓰고 부모를 PssCaptureSnapshot(VA
// clone)으로 캡처한 뒤 "캡처 끝"을 알린다. 덤프는 snapshot에서 쓴다(부모 종료와 무관하게 종류별
// 마감까지). 부모가 요청 없이 NTSTATUS 오류 코드로 끝나면 UnreportedExit를 기록한다.
namespace rocks::platform::detail {

inline constexpr uint32 CrashHelperProtocolVersion = 2;
inline constexpr char CrashHelperArgument[] = "--rocks-crash-helper";

// 부모가 크래시 때 도우미의 캡처를 요청한 결과다.
enum class CrashHelperCapture : uint8 {
    Captured,    // 도우미가 캡처했다. 파일은 도우미가 쓴다
    Unavailable, // 도우미가 없거나 쓸 수 없다. 부모가 프로세스 안에서 쓴다(outcome에 사유)
};

// --- 부모 ---

// 설치 때 부른다. 도우미 실행 파일 경로를 굳히고, Preferred면 바로
// 띄운다(내장·sanitizer·InProcess는 아무것도 하지 않는다). 실패는 설치 실패가 아니다. 크래시 때
// 프로세스 안 fallback이 쓰이고 결과에 "StartFailed"가 남는다. 시작 때 도우미 준비를 기다리지
// 않는다.
void PrepareCrashHelper(const CrashReportingOptions& options) noexcept;

// Deferred로 설치한 도우미를 띄운다(StartCrashHelper의 본체).
CrashHelperStart StartDeferredCrashHelper() noexcept;

// 도우미가 부모의 job을 빠져나오지 못했을 때 크래시 처리 스레드가 기본 덤프를 더 기다리는 시간이다.
// 아니면 0이다. 부모 쪽 총 대기 예산에 더한다.
uint32 CrashHelperJobWaitMilliseconds() noexcept;

// 시험 전용: 도우미가 부모를 캡처한 뒤 소유 CAS 전에 부르는 함수다. 시험 실행 파일이 도우미 진입
// (RunCrashHelperIfRequested) 전에 건다. 캡처와 부모의 요청 거두기 사이의 순서를 결정적으로 만든다.
using CrashHelperCaptureHook = void (*)(HANDLE parent) noexcept;
ROCKS_PLATFORMBASE_API void SetCrashHelperCaptureHook(CrashHelperCaptureHook hook) noexcept;

// 시험 전용: 도우미 인자가 올바를 때 도우미 루프 대신 부르는 고장 난 도우미 본체다(부모 프로세스·
// 크래시 요청 이벤트 핸들을 받고 반환값이 도우미 종료 코드다). 시험 실행 파일이 도우미 진입 전에
// 건다(정적 초기화). 제품 코드에는 빈 칸만 있다.
using CrashHelperOverride = int (*)(HANDLE parent, HANDLE request) noexcept;
ROCKS_PLATFORMBASE_API void SetCrashHelperOverride(CrashHelperOverride body) noexcept;

// SetCrashOutputDirectory 뒤 교환 블록의 폴더 경로를 고친다.
void UpdateCrashHelperFolders() noexcept;

// 도우미 프로세스가 있는가(시작 성공)다.
bool CrashHelperStarted() noexcept;

// 크래시 처리 스레드가 부른다(heap 없음). 기록을 교환 블록에 쓰고 요청한 뒤 캡처 끝·도우미 종료·
// 마감(captureTimeoutMilliseconds)을 기다린다. 마감을 넘으면 요청을 거둔다(도우미는 그 뒤 덤프를
// 쓰지 않는다). outcome에 도우미 상태를 남긴다.
CrashHelperCapture RequestCrashHelperCapture(EXCEPTION_POINTERS* pointers,
                                             CrashHelperOutcome& outcome) noexcept;

// 크래시 처리 스레드가 종료 콜백 뒤 부른다. 콜백 결과를 교환 블록에 쓰고 도우미에 알리고, 도우미가
// 고른 크래시 폴더를 stderr에 한 줄 남긴다.
void FinishCrashHelperReport() noexcept;

// 정상 종료 때 부른다. 도우미에 Shutdown을 알리고 최대 2초 기다린 뒤(넘으면 끝낸다) 핸들을 닫는다.
void StopCrashHelper() noexcept;

} // namespace rocks::platform::detail
