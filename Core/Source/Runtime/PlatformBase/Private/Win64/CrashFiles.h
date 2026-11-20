#pragma once

#include "../Common/CrashReportingInternal.h"

#include <Windows.h>

#include <DbgHelp.h>
#include <processsnapshot.h>

// Windows 크래시 수집기의 파일 쪽이다(Docs/Design/CrashHandlingAndLaunch.ko.md 11·14절). 설치 때
// 경로·모듈 정보·DbgHelp를 준비하고, 크래시 때 크래시 폴더·CrashInfo.json·덤프를 쓴다. 쓰는 쪽은
// 도우미 프로세스(snapshot 덤프)이고, 도우미가 없으면 부모의 크래시 처리 스레드(프로세스 안
// 덤프)다. 두 쪽 모두 정적 버퍼만 쓴다(Win32 파일·경로 API 자체는 heap을 쓸 수 있다).
namespace rocks::platform::detail {

// 크래시 폴더 경로 칸의 UTF-16 단위 수다(뿌리 1024 + 폴더 이름 + 여유).
inline constexpr usize CrashFolderUnits = 1024 + CrashFolderNameBytes + 32;
// 덤프 사용자 stream 버퍼(형식 이름 줄 + 예비 CrashInfo.json)와 덤프 설명(CommentStreamA) 크기다.
inline constexpr usize CrashCommentBytes = 512;
// 크래시 폴더 안 덤프 파일 이름이다(기본 덤프와 Detailed·Full 추가 덤프).
inline constexpr wchar_t CrashDumpFileName[] = L"Minidump.dmp";
inline constexpr wchar_t CrashDetailDumpFileName[] = L"Minidump.detail.dmp";
inline constexpr char CrashDumpFileText[] = "Minidump.dmp";
inline constexpr char CrashDetailDumpFileText[] = "Minidump.detail.dmp";

// --- 설치(부모) ---

// 출력 폴더(없으면 "Saved/Crashes")와 임시 폴더 fallback을 절대 경로로 굳히고, 주 모듈 PE
// CodeView 정보를 읽고, writeDumps면 dbghelp.dll을 System32에서 연다. 출력 폴더 경로를 만들 수
// 없으면(변환 실패·길이 초과) false다.
bool PrepareCrashFiles(const char* outputDirectory, bool writeDumps) noexcept;

// SetCrashOutputDirectory의 본체다. 실패하면 이전 경로를 유지한다.
bool ResolveCrashOutputDirectory(const char* directory) noexcept;

void ReleaseCrashFiles() noexcept;

// 프로세스 안 덤프를 쓸 수 있는가(DbgHelp 준비됨)다.
bool CanWriteInProcessDump() noexcept;

// process를 덤프용으로 캡처한다(PssCaptureSnapshot, VA clone·스레드 문맥, Detailed·Full이면 핸들
// 정보). VA clone이 job을 빠지게 PSS_CREATE_BREAKAWAY_OPTIONAL을 먼저 청하고, 이 OS가 그 조합을
// 받지 않으면(ERROR_INVALID_PARAMETER) 플래그 없이 다시 캡처한다. 성공이면 ERROR_SUCCESS이고
// snapshot은 PssFreeSnapshot(GetCurrentProcess(), snapshot)으로 푼다. 도우미(부모 캡처)와 프로세스
// 안 덤프(자기 캡처 — 살아 있는 자기 프로세스를 덤프하면 dbghelp가 마감 스레드까지 suspend한다,
// 설계 11.5절)가 같이 쓴다.
DWORD CaptureCrashSnapshot(HANDLE process, CrashDumpDetail detail, HPSS& snapshot) noexcept;

// 시험용: 덤프 callback마다 callback 종류로 부른다(nullptr이면 끈다). 덤프의 특정 단계에서 다른
// 스레드가 lock을 잡게 맞추는 시험이 쓴다(TestSupport). 설치 전에 한 번 건다.
using CrashDumpCallbackHook = void (*)(ULONG callbackType) noexcept;
void SetCrashDumpCallbackHookForTest(CrashDumpCallbackHook hook) noexcept;

// 설치 때 굳힌 크래시 폴더 경로다(UTF-16과 진단용 UTF-8).
const wchar_t* CrashPrimaryFolder() noexcept;
const wchar_t* CrashFallbackFolder() noexcept;
const char* CrashPrimaryFolderText() noexcept;
const char* CrashFallbackFolderText() noexcept;

// --- 쓰기(부모 크래시 처리 스레드와 도우미가 같이 쓴다) ---

// dbghelp.dll을 System32에서 열고 MiniDumpWriteDump를 찾는다. 이미 열었으면 true다.
bool LoadCrashDumpWriter() noexcept;

// 두 폴더 모두 실패했을 때 stderr에 경로와 Win32 오류를 한 줄씩 남긴다.
void WriteCrashFolderFailure(const char* primaryText, DWORD primaryError, const char* fallbackText,
                             DWORD fallbackError) noexcept;

// folder 안 CrashInfo.json을 쓴다. final이면 임시 파일에 쓴 뒤 바꿔 넣는다(임시 파일을 쓰지 못하면
// 예비본을 덮어쓰지 않고 오류를 돌려준다. 바꿔 넣기만 실패하면 직접 쓰고 임시 파일을 지운다).
DWORD WriteCrashMetadataFile(const wchar_t* folder, const char* bytes, usize count,
                             bool final) noexcept;

// 쓰는 중인 크래시 폴더다. primary로 시작하고, 폴더나 파일을 만들 수 없으면 fallback으로 한 번
// 옮긴다. current가 nullptr이면 두 곳 모두 실패했다(오류 코드가 남는다).
struct CrashReportFolder {
    const wchar_t* primary = nullptr;
    const wchar_t* fallback = nullptr;
    const wchar_t* current = nullptr;
    bool fallbackUsed = false;
    bool wroteFile = false;
    DWORD primaryError = ERROR_SUCCESS;
    DWORD fallbackError = ERROR_SUCCESS;
};

// primary 폴더를 만들고 예비 CrashInfo.json을 쓴다. 폴더나 파일이 실패하면 fallback에서 한 번 더
// 한다.
void OpenCrashReportFolder(CrashReportFolder& report, const char* preliminary,
                           usize bytes) noexcept;

// primary에서 파일을 만들 수 없을 때(덤프 파일 등) fallback으로 옮기고 예비본을 다시 쓴다. 이미
// 옮겼거나 fallback도 실패하면 false다(실패면 current가 nullptr이 된다).
bool MoveCrashReportToFallback(CrashReportFolder& report, DWORD primaryError,
                               const char* preliminary, usize bytes) noexcept;

// 덤프 대상이다. 프로세스 안 덤프는 자기 프로세스와 정적 사본 포인터(ClientPointers FALSE), 도우미
// 덤프는 snapshot 핸들과 부모 주소(ClientPointers TRUE)다.
struct CrashDumpSource {
    HANDLE process = nullptr;
    DWORD processId = 0;
    bool snapshot = false;
    DWORD failingThreadId = 0;
    EXCEPTION_POINTERS* pointers = nullptr;
    bool clientPointers = false;
    const CrashMemoryRange* ranges = nullptr;
    usize rangeCount = 0;
};

struct CrashDumpWrite {
    const char* status; // Written·Failed·Cancelled
    DWORD error;
    bool fileError; // 덤프 파일을 만들지 못했다(fallback 폴더로 옮길 수 있다)
};

// 삭제 예약 상태로 연 덤프 파일이다(OpenCrashDumpFile). 도우미는 캡처 끝을 알리기 전에 열어 크래시
// 폴더를 확정한다(그 뒤 폴더가 바뀌지 않는다).
struct CrashDumpFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool pendingDelete = false;
    DWORD error = ERROR_SUCCESS;
};

// folder\name을 만들고(같은 이름이 있으면 덮는다) 다 쓸 때까지 삭제 예약 상태로 둔다. 실패하면
// handle이 INVALID_HANDLE_VALUE이고 error에 원인이 있다.
CrashDumpFile OpenCrashDumpFile(const wchar_t* folder, const wchar_t* name) noexcept;

// 열어 둔 파일을 쓰지 않고 닫는다(삭제 예약이면 파일이 사라진다).
void CloseCrashDumpFile(CrashDumpFile& file) noexcept;

// OpenCrashDumpFile로 연 파일에 덤프를 쓰고 닫는다. 결과는 WriteCrashDumpFile과 같다.
CrashDumpWrite WriteOpenedCrashDumpFile(CrashDumpFile& file, const wchar_t* folder,
                                        const wchar_t* name, MINIDUMP_TYPE type,
                                        const CrashDumpSource& source, const char* record,
                                        usize recordBytes, const char* comment,
                                        uint64 deadlineNanoseconds) noexcept;

// folder\name에 덤프를 쓴다. record(RocksCrashRecord/1 stream 내용)·comment는 덤프 사용자
// stream이다. deadlineNanoseconds(MonotonicNanoseconds 기준)를 넘으면 CancelCallback으로 취소한다.
// 다 쓰기 전에는 파일을 삭제 예약 상태로 두므로 취소·실패·쓰는 프로세스의 중도 종료 때 열 수 없는
// 파일이 남지 않는다.
CrashDumpWrite WriteCrashDumpFile(const wchar_t* folder, const wchar_t* name, MINIDUMP_TYPE type,
                                  const CrashDumpSource& source, const char* record,
                                  usize recordBytes, const char* comment,
                                  uint64 deadlineNanoseconds) noexcept;

MINIDUMP_TYPE StandardCrashDumpType() noexcept;
// Detailed·Full 추가 덤프의 종류다.
MINIDUMP_TYPE DetailCrashDumpType(CrashDumpDetail detail) noexcept;
const char* CrashDumpDetailName(CrashDumpDetail detail) noexcept;

// buffer에 "RocksCrashRecord/1\n" + 예비 CrashInfo.json을 쓰고 전체 길이를 돌려준다. jsonOffset에
// JSON 시작 위치를 둔다(CrashInfo.json 파일 내용).
usize FormatCrashRecordStream(char* buffer, usize capacity, const CrashRecordSnapshot& record,
                              const CrashMetadataInput& input, usize& jsonOffset) noexcept;

// 덤프 설명 한 줄(NUL 종료)이다.
void FormatCrashComment(char* buffer, usize capacity, const CrashRecordSnapshot& record) noexcept;

// --- 프로세스 안 흐름(부모 크래시 처리 스레드) ---

// 프로세스 안 보고의 설정이다.
struct CrashInProcessReport {
    // 덤프를 쓰려 하는가(설치 때 정한 값: 내장·sanitizer가 아님). DbgHelp를 열지 못했으면 덤프는
    // Failed("dbghelp.dll unavailable")로 남는다.
    bool writeDump = false;
    // 덤프를 쓰지 않는 사유(내장·sanitizer)다. limitations에도 남는다.
    const char* skippedReason = nullptr;
    uint32 dumpTimeoutMilliseconds = 0;
    uint32 metadataMilliseconds = 0;
    // 크래시한 스레드가 프로세스를 끝내는 시각(MonotonicNanoseconds)이다. 덤프 마감은 여기서
    // 메타데이터 몫과 여유를 뺀 시각을 넘지 않는다. 0이면 묶지 않는다.
    uint64 ownerDeadlineNanoseconds = 0;
};

// 폴더 → 예비 CrashInfo.json → 덤프 → 최종 CrashInfo.json 순서로 쓴다. pointers는 정적 사본이다.
// helper는 도우미를 쓰려다 실패한 결과다(쓰지 않았으면 status nullptr).
void WriteCrashFiles(EXCEPTION_POINTERS* pointers, const CrashInProcessReport& request,
                     const CrashHelperOutcome& helper) noexcept;

} // namespace rocks::platform::detail
