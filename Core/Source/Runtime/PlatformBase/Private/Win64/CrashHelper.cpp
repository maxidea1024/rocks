#include "CrashHelper.h"

#include <Base/Compiler.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/CrashReporting.h>
#include <Platform/Fatal.h>
#include <Platform/Win64/WideString.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <processsnapshot.h>
#include <werapi.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::platform::detail {

namespace {

// 교환 블록의 상태다. 부모(W)와 도우미가 원자 연산으로 옮긴다.
enum CrashHelperState : uint32 {
    HelperStarting = 0,
    HelperReady = 1,      // 도우미: 준비됨
    CrashRequested = 2,   // 부모: 크래시 기록을 썼다
    HelperCaptured = 3,   // 도우미: 캡처 끝(부모는 이제 끝나도 된다)
    RequestWithdrawn = 4, // 부모: 캡처 마감을 넘겨 요청을 거뒀다(프로세스 안 fallback)
    HelperDumpDone = 5,   // 도우미: 최종 CrashInfo.json까지 썼다
    ParentShutdown = 6,   // 부모: 정상 종료
};

// 도우미가 고른 크래시 폴더다.
enum CrashFolderSlot : uint32 {
    FolderUnknown = 0,
    FolderPrimary = 1,
    FolderFallback = 2,
    FolderNone = 3,
};

constexpr uint32 BlockMagic = 0x484B5252; // "RRKH"
constexpr usize BlockBytes = 64 * 1024;
constexpr uint32 ShutdownWaitMilliseconds = 2000;
constexpr DWORD ParentExitedByControlC = 0xC000013A; // STATUS_CONTROL_C_EXIT
constexpr DWORD NtStatusSeverityMask = 0xF0000000;
constexpr DWORD NtStatusErrorSeverity = 0xC0000000;
constexpr usize HandleCount = 7;
constexpr usize CommandLineUnits = CrashFolderUnits + 512;
constexpr usize MaxDecimalDigits = 20;
constexpr usize AttributeListBytes = 256;
constexpr uint64 HelperDecimalBase = 10;
constexpr int HelperExitSuccess = 0;
constexpr int HelperExitBadArguments = 2;
constexpr int HelperExitBadBlock = 3;
constexpr int HelperExitWaitFailed = 4;
// 명령줄의 인자 위치다: <exe> --rocks-crash-helper <version> <handles...>
constexpr int VersionArgument = 2;
constexpr int FirstHandleArgument = 3;
constexpr int HelperArgumentCount = FirstHandleArgument + static_cast<int>(HandleCount);
// 부모 결과 대기 상한이다(콜백 예산 + 메타데이터 몫 + 여유, 각 예산은 MaxCrashBudgetMilliseconds
// 이하).
constexpr uint32 MaxHelperParentWaitMilliseconds =
    2 * MaxCrashBudgetMilliseconds + ReportMarginMilliseconds;
// 도우미 인자 한 칸의 최대 길이다(표지·버전·핸들 10진수).
constexpr usize MaxHelperArgumentBytes = 32;

struct WireRange {
    uint64 address;
    uint64 bytes;
};

// 공유 메모리 교환 블록이다(설계 11.3절). 같은 실행 파일의 두 프로세스가 쓰므로 배치가 같다.
struct CrashHelperBlock {
    uint32 magic;
    uint32 version;
    uint32 size;
    uint32 parentPid;
    uint32 helperPid;
    uint32 state;
    // 고정 정보(부모, 설치·폴더 변경 때)
    uint32 dumpDetail;
    uint32 standardDeadlineMilliseconds;
    uint32 detailDeadlineMilliseconds;
    uint32 parentWaitMilliseconds;
    // 주 폴더는 칸 두 개와 index다(SetCrashOutputDirectory가 쓰지 않는 칸을 채운 뒤 바꿔 끼운다).
    uint32 primaryFolderIndex;
    wchar_t primaryFolders[2][CrashFolderUnits];
    wchar_t fallbackFolder[CrashFolderUnits];
    // 크래시 요청(부모 W)
    uint32 failingThreadId;
    uint64 exceptionPointers;
    WireRange ranges[CrashRecordRangeCount];
    CrashRecordSnapshot record;
    // 도우미 결과
    uint32 folderSlot;
    uint32 primaryError;
    uint32 fallbackError;
    uint32 captureError;
    // 캡처한 VA clone이 job 안에 남았는가다(1이면 부모는 기본 덤프가 끝날 때까지 job을 닫지
    // 않는다).
    uint32 cloneInJob;
    // 부모 결과(W, 종료 콜백 뒤)
    uint32 parentResultReady;
    uint32 reporterFailed;
    uint64 callbackMilliseconds;
    char callbackStatus[CrashStatusBytes];
    // 요청 뒤(종료 콜백 동안) 겹쳐 실패해 멈춘 스레드다.
    uint32 stoppedTotal;
    uint32 stoppedCount;
    uint64 stoppedIds[MaxStoppedCrashThreads];
};

static_assert(sizeof(CrashHelperBlock) <= BlockBytes, "crash helper block must fit its section");

// 부모 쪽 상태다(설치·제거 때 쓰고 크래시 처리 스레드가 읽는다).
constinit CrashHelperBlock* block = nullptr;
constinit HANDLE section = nullptr;
constinit HANDLE requestEvent = nullptr;
constinit HANDLE captureDoneEvent = nullptr;
constinit HANDLE parentDoneEvent = nullptr;
constinit HANDLE standardDumpEvent = nullptr;
constinit HANDLE shutdownEvent = nullptr;
constinit HANDLE parentForHelper = nullptr;
constinit HANDLE helperProcess = nullptr;
constinit uint32 helperPid = 0;
constinit DWORD helperStartError = ERROR_SUCCESS;
constinit bool helperRequested = false;
constinit bool helperInJob = false;
constinit DWORD helperPathError = ERROR_SUCCESS;
constinit uint32 captureTimeoutMilliseconds = 0;
constinit wchar_t helperPath[CrashFolderUnits] = {};
constinit wchar_t commandLine[CommandLineUnits] = {};
alignas(16) constinit unsigned char attributeStorage[AttributeListBytes] = {};
// 크래시 처리 스레드가 콜백 결과를 읽어 올 사본이다.
constinit CrashRecordSnapshot parentScratch = {};

// 시험 전용 gate다(캡처와 CAS 사이). 시험 실행 파일이 도우미 진입 전에 건다.
constinit CrashHelperCaptureHook captureHook = nullptr;
// 시험 전용 고장 난 도우미 본체다. 걸려 있으면 도우미 루프 대신 부른다.
constinit CrashHelperOverride helperOverride = nullptr;

// 도우미 쪽 정적 저장소다(도우미는 단일 스레드다). 교환 블록은 믿지 않고 검사한 사본만 쓴다.
constinit CrashRecordSnapshot helperRecord = {};
constinit wchar_t helperPrimaryFolder[CrashFolderUnits] = {};
constinit wchar_t helperFallbackFolder[CrashFolderUnits] = {};
constinit uint32 helperFailingThreadId = 0;
constinit uint64 helperExceptionPointers = 0;
constinit CrashDumpDetail helperDumpDetail = CrashDumpDetail::Standard;
constinit uint32 helperStandardDeadline = 0;
constinit uint32 helperDetailDeadline = 0;
constinit uint32 helperParentWait = 0;
constinit CrashMemoryRange helperRanges[CrashRecordRangeCount] = {};
constinit char helperStream[CrashMetadataBytes + 64] = {};
constinit char helperComment[CrashCommentBytes] = {};

template <usize Capacity>
void CopyText(char (&destination)[Capacity], const char* source) noexcept {
    usize length = 0;
    for (; source[length] != '\0' && length + 1 < Capacity; ++length) {
        destination[length] = source[length];
    }

    destination[length] = '\0';
}

void CopyWide(wchar_t* destination, const wchar_t* source) noexcept {
    usize length = 0;
    for (; source[length] != L'\0' && length + 1 < CrashFolderUnits; ++length) {
        destination[length] = source[length];
    }

    destination[length] = L'\0';
}

bool HelperAppendWide(usize& length, const wchar_t* text) noexcept {
    for (usize index = 0; text[index] != L'\0'; ++index) {
        if (length + 1 >= CommandLineUnits) {
            return false;
        }

        commandLine[length++] = text[index];
    }

    commandLine[length] = L'\0';
    return true;
}

bool AppendNumber(usize& length, uint64 value) noexcept {
    wchar_t digits[MaxDecimalDigits + 1];
    usize count = 0;
    do {
        digits[count++] = static_cast<wchar_t>(L'0' + value % HelperDecimalBase);
        value /= HelperDecimalBase;
    } while (value != 0);

    wchar_t text[MaxDecimalDigits + 1];
    for (usize index = 0; index < count; ++index) {
        text[index] = digits[count - 1 - index];
    }

    text[count] = L'\0';
    return HelperAppendWide(length, L" ") && HelperAppendWide(length, text);
}

bool ParseNumber(const char* text, uint64& value) noexcept {
    value = 0;
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    for (usize index = 0; text[index] != '\0'; ++index) {
        if (text[index] < '0' || text[index] > '9' || index >= MaxDecimalDigits) {
            return false;
        }

        value = value * HelperDecimalBase + static_cast<uint64>(text[index] - '0');
    }

    return true;
}

bool HelperTextEquals(const char* left, const char* right) noexcept {
    usize index = 0;
    for (; left[index] != '\0' && left[index] == right[index]; ++index) {
    }

    return left[index] == right[index];
}

void CloseIfOpen(HANDLE& handle) noexcept {
    if (handle != nullptr) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

void ReleaseParentHandles() noexcept {
    if (block != nullptr) {
        (void)UnmapViewOfFile(block);
        block = nullptr;
    }

    CloseIfOpen(section);
    CloseIfOpen(requestEvent);
    CloseIfOpen(captureDoneEvent);
    CloseIfOpen(parentDoneEvent);
    CloseIfOpen(standardDumpEvent);
    CloseIfOpen(shutdownEvent);
    CloseIfOpen(parentForHelper);
    CloseIfOpen(helperProcess);
    helperPid = 0;
}

uint32 DetailDeadline(const CrashReportingOptions& options) noexcept {
    if (options.helperDumpDeadlineMilliseconds != 0) {
        return options.helperDumpDeadlineMilliseconds;
    }

    return options.dumpDetail == CrashDumpDetail::Full
               ? CrashReportingOptions::DefaultHelperFullDumpDeadlineMilliseconds
               : CrashReportingOptions::DefaultHelperDetailedDumpDeadlineMilliseconds;
}

bool ResolveHelperPath(const CrashReportingOptions& options) noexcept {
    if (options.helperExecutable == nullptr) {
        const DWORD units = GetModuleFileNameW(nullptr, helperPath, CrashFolderUnits);
        return units != 0 && units < CrashFolderUnits;
    }

    usize bytes = 0;
    while (options.helperExecutable[bytes] != '\0') {
        ++bytes;
    }

    return static_cast<bool>(windows::ConvertUtf8ToWide(options.helperExecutable, bytes, helperPath,
                                                        CrashFolderUnits, windows::TextFit::Exact));
}

// 도우미가 쓰는 부모 핸들 권한이다(시제품 확인 최소 조합). 핸들 캡처(Detailed·Full)만
// PROCESS_DUP_HANDLE이 더 든다.
DWORD ParentAccess(CrashDumpDetail detail) noexcept {
    const DWORD base = PROCESS_CREATE_PROCESS | PROCESS_QUERY_INFORMATION | SYNCHRONIZE;
    return detail == CrashDumpDetail::Standard ? base : base | PROCESS_DUP_HANDLE;
}

// 부모의 교환 블록 view 보호를 바꾼다. 평소에는 읽기 전용으로 두고(엉뚱한 쓰기가 도우미 입력을
// 망가뜨리지 않는다) 크래시 요청·폴더 변경·정상 종료 때만 쓰기를 연다.
void ProtectBlock(DWORD protection) noexcept {
    DWORD previous = 0;
    if (block != nullptr) {
        (void)VirtualProtect(block, sizeof(CrashHelperBlock), protection, &previous);
    }
}

// 핸들은 상속하지 않게 만든다. 도우미를 만드는 CreateProcessW 바로 앞뒤에서만 상속을 켠다.
DWORD CreateHelperObjects(const CrashReportingOptions& options) noexcept {
    section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                 static_cast<DWORD>(BlockBytes), nullptr);
    if (section == nullptr) {
        return GetLastError();
    }

    block = static_cast<CrashHelperBlock*>(
        MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CrashHelperBlock)));
    requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    captureDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    parentDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    standardDumpEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (block == nullptr || requestEvent == nullptr || captureDoneEvent == nullptr ||
        parentDoneEvent == nullptr || standardDumpEvent == nullptr || shutdownEvent == nullptr) {
        return GetLastError();
    }

    // 도우미는 OpenProcess를 하지 않는다. 필요한 권한만 담은 자기 핸들을 넘긴다.
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                         &parentForHelper, ParentAccess(options.dumpDetail), FALSE, 0)) {
        return GetLastError();
    }

    return ERROR_SUCCESS;
}

void FillBlock(const CrashReportingOptions& options) noexcept {
    block->magic = BlockMagic;
    block->version = CrashHelperProtocolVersion;
    block->size = static_cast<uint32>(sizeof(CrashHelperBlock));
    block->parentPid = GetCurrentProcessId();
    block->dumpDetail = static_cast<uint32>(options.dumpDetail);
    block->standardDeadlineMilliseconds =
        CrashReportingOptions::DefaultHelperStandardDumpDeadlineMilliseconds;
    block->detailDeadlineMilliseconds = DetailDeadline(options);
    // 부모는 캡처 끝 뒤 종료 콜백과 여유만큼 더 산다. 도우미는 그만큼 콜백 결과를 기다린다.
    block->parentWaitMilliseconds = options.callbackBudgetMilliseconds +
                                    options.metadataTimeoutMilliseconds + ReportMarginMilliseconds;
    block->primaryFolderIndex = 0;
    CopyWide(block->primaryFolders[0], CrashPrimaryFolder());
    CopyWide(block->fallbackFolder, CrashFallbackFolder());
    // 설치 때 기록이다. 보고 없는 종료 기록(UnreportedExit)이 식별 정보를 여기서 읽는다.
    CaptureCrashRecord(block->record);
    AtomicStore<MemoryOrder::Release>(&block->state, uint32{HelperStarting});
}

DWORD LaunchHelper() noexcept {
    usize length = 0;
    const HANDLE handles[HandleCount] = {section,          parentForHelper, requestEvent,
                                         captureDoneEvent, parentDoneEvent, standardDumpEvent,
                                         shutdownEvent};
    if (!HelperAppendWide(length, L"\"") || !HelperAppendWide(length, helperPath) ||
        !HelperAppendWide(length, L"\" ")) {
        return ERROR_FILENAME_EXCED_RANGE;
    }

    for (usize index = 0; CrashHelperArgument[index] != '\0'; ++index) {
        const wchar_t text[2] = {static_cast<wchar_t>(CrashHelperArgument[index]), L'\0'};
        (void)HelperAppendWide(length, text);
    }

    (void)AppendNumber(length, CrashHelperProtocolVersion);
    for (HANDLE handle : handles) {
        if (!AppendNumber(length, reinterpret_cast<uptr>(handle))) {
            return ERROR_FILENAME_EXCED_RANGE;
        }
    }

    SIZE_T attributeBytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    if (attributeBytes > sizeof(attributeStorage)) {
        return ERROR_INSUFFICIENT_BUFFER;
    }

    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage);
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
        return GetLastError();
    }

    // 넘길 핸들만 상속한다(표준 입출력도 넘기지 않는다: 부모 stdout을 읽는 harness의 EOF를 막지
    // 않는다). 콘솔은 붙이지 않는다(Ctrl-C가 도우미를 먼저 죽이지 않는다).
    DWORD error = ERROR_SUCCESS;
    HANDLE list[HandleCount];
    for (usize index = 0; index < HandleCount; ++index) {
        list[index] = handles[index];
    }

    // 상속 목록의 핸들은 상속 가능해야 한다. CreateProcessW 바로 앞에서 켜고 바로 뒤에서 끈다(그
    // 사이 다른 스레드가 상속 목록 없이 만든 자식만 물려받을 수 있다. 설치는 시작 단계에서 한다).
    for (HANDLE handle : handles) {
        (void)SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }

    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                                   sizeof(list), nullptr, nullptr)) {
        error = GetLastError();
    } else {
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        // 부모가 job 안에 있으면 도우미는 job에서 빠져나와야 job을 닫을 때(harness의 kill 등) 함께
        // 끝나지 않는다. job이 breakaway를 허락하지 않으면 ERROR_ACCESS_DENIED이고 job 안에 띄운다.
        constexpr DWORD Flags = EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS;
        BOOL created = CreateProcessW(helperPath, commandLine, nullptr, nullptr, TRUE,
                                      Flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr,
                                      &startup.StartupInfo, &process);
        if (!created && GetLastError() == ERROR_ACCESS_DENIED) {
            created = CreateProcessW(helperPath, commandLine, nullptr, nullptr, TRUE, Flags,
                                     nullptr, nullptr, &startup.StartupInfo, &process);
        }

        if (created) {
            CloseHandle(process.hThread);
            helperProcess = process.hProcess;
            helperPid = process.dwProcessId;
            BOOL inJob = FALSE;
            helperInJob = IsProcessInJob(helperProcess, nullptr, &inJob) && inJob;
        } else {
            error = GetLastError();
        }
    }

    DeleteProcThreadAttributeList(attributes);
    for (HANDLE handle : handles) {
        (void)SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
    }

    return error;
}

void HelperWriteText(const char* text) noexcept {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }

    WriteDiagnostic(text, length);
}

// --- 도우미 ---

// 교환 블록의 글자 칸을 NUL 종료로 고친다(부모가 망가뜨렸어도 칸 밖을 읽지 않는다).
template <usize Capacity>
void TerminateText(char (&text)[Capacity]) noexcept {
    text[Capacity - 1] = '\0';
}

template <typename T>
T ClampCount(T value, usize limit) noexcept {
    return value < limit ? value : static_cast<T>(limit);
}

// 크래시 기록 사본의 길이·글자 칸을 배열 크기 안으로 고친다.
void SanitizeRecord(CrashRecordSnapshot& record) noexcept {
    TerminateText(record.program);
    TerminateText(record.executable);
    TerminateText(record.crashId);
    TerminateText(record.stage);
    TerminateText(record.identity);
    TerminateText(record.moduleJson);
    TerminateText(record.message);
    TerminateText(record.file);
    TerminateText(record.failingThreadName);
    TerminateText(record.callbackStatus);
    record.messageBytes = ClampCount(record.messageBytes, MaxCrashMessageBytes - 1);
    record.identityBytes = ClampCount(record.identityBytes, CrashIdentityBytes - 1);
    for (usize slot = 0; slot < CrashAnnotationCount; ++slot) {
        record.annotationBytes[slot] =
            ClampCount(record.annotationBytes[slot], MaxCrashAnnotationBytes);
    }

    record.stoppedCount = ClampCount(record.stoppedCount, MaxStoppedCrashThreads);
    if (record.dumpDetail > static_cast<uint8>(CrashDumpDetail::Full)) {
        record.dumpDetail = static_cast<uint8>(CrashDumpDetail::Standard);
    }
}

// 경로 칸을 NUL 종료로 복사한다.
void CopyFolder(wchar_t (&destination)[CrashFolderUnits], const wchar_t* source) noexcept {
    for (usize index = 0; index < CrashFolderUnits; ++index) {
        destination[index] = source[index];
    }

    destination[CrashFolderUnits - 1] = L'\0';
}

// 크래시 요청 때 교환 블록에서 쓸 값을 사본으로 옮긴다(블록은 다시 읽지 않는다).
void CopyRequest() noexcept {
    helperRecord = block->record;
    SanitizeRecord(helperRecord);
    const uint32 folder = AtomicLoad<MemoryOrder::Acquire>(&block->primaryFolderIndex) & 1u;
    CopyFolder(helperPrimaryFolder, block->primaryFolders[folder]);
    CopyFolder(helperFallbackFolder, block->fallbackFolder);
    helperFailingThreadId = block->failingThreadId;
    helperExceptionPointers = block->exceptionPointers;
    for (usize index = 0; index < CrashRecordRangeCount; ++index) {
        helperRanges[index].address = reinterpret_cast<const void*>(block->ranges[index].address);
        helperRanges[index].bytes = static_cast<usize>(block->ranges[index].bytes);
    }
}

uint64 DeadlineAfter(uint32 milliseconds) noexcept {
    return MonotonicNanoseconds() + uint64{milliseconds} * NanosecondsPerMillisecond;
}

void PublishFolder(const CrashReportFolder& report) noexcept {
    block->primaryError = report.primaryError;
    block->fallbackError = report.fallbackError;
    AtomicStore<MemoryOrder::Release>(
        &block->folderSlot,
        uint32{report.current == nullptr ? FolderNone
                                         : (report.fallbackUsed ? FolderFallback : FolderPrimary)});
}

// 크래시 요청을 처리한다(설계 11.4절). 순서가 중요하다.
// 1. 요청이 아직 유효한지 본다(부모가 거뒀으면 아무것도 쓰지 않는다).
// 2. 부모를 캡처한다(snapshot). 부모는 이 동안만 멈춰 있으면 된다.
// 3. CAS(CrashRequested → Captured)로 보고 소유를 정한다. 지면(부모가 마감을 넘겨 거둠) snapshot을
//    버리고 파일을 하나도 쓰지 않는다(부모가 프로세스 안에서 쓴 최종본을 덮지 않는다).
// 4. 폴더·예비 CrashInfo.json을 쓰고 기본 덤프 파일을 열어 폴더를 확정한다(파일을 만들 수 없으면
//    임시 폴더로 옮긴다). 그 뒤 폴더는 바뀌지 않는다.
// 5. 캡처 끝을 알린다(부모는 확정된 폴더 칸을 읽는다).
// 6. 기본 덤프, 추가 덤프, 부모 결과 대기, 최종 CrashInfo.json.
void HandleRequest(HANDLE parent) noexcept {
    if (AtomicLoad<MemoryOrder::Acquire>(&block->state) != CrashRequested) {
        return;
    }

    CopyRequest();
    const CrashDumpDetail detail = helperDumpDetail;
    HPSS snapshot = nullptr;
    // VA clone 프로세스는 job이 허락하면 job에서 빠지게 먼저 청한다(job을 닫아도 덤프 원본이
    // 남는다). 이 OS가 그 플래그 조합을 받지 않으면(ERROR_INVALID_PARAMETER, Windows 11 26200에서
    // 확인) 플래그 없이 다시 캡처한다(CaptureCrashSnapshot). 그때 job 안 부모는 기본 덤프가 끝날
    // 때까지 job을 닫지 않는다(FinishCrashHelperReport).
    const DWORD captured = CaptureCrashSnapshot(parent, detail, snapshot);
    if (captured != ERROR_SUCCESS) {
        // 부모가 프로세스 안 fallback으로 쓴다. 이 도우미는 더 쓰지 않는다.
        block->captureError = captured;
        SetEvent(captureDoneEvent);
        SetEvent(standardDumpEvent);
        return;
    }

    if (captureHook != nullptr) {
        captureHook(parent);
    }

    uint32 expected = CrashRequested;
    if (!AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
            &block->state, expected, uint32{HelperCaptured})) {
        // 부모가 마감을 넘겨 요청을 거뒀다. 덤프가 둘 생기지 않게 버린다.
        (void)PssFreeSnapshot(GetCurrentProcess(), snapshot);
        return;
    }

    // VA clone이 job 안에 남았는지 본다(breakaway를 허락하는 job이어도 이 OS가 clone의 breakaway
    // 플래그를 받지 않으면 clone은 부모의 job에 남는다). 알 수 없으면 부모가 job 안인지로 정한다.
    // 부모는 캡처 끝 신호 뒤 이 값을 읽는다.
    BOOL cloneInJob = FALSE;
    PSS_VA_CLONE_INFORMATION clone{};
    if (PssQuerySnapshot(snapshot, PSS_QUERY_VA_CLONE_INFORMATION, &clone, sizeof(clone)) !=
            ERROR_SUCCESS ||
        clone.VaCloneHandle == nullptr ||
        !IsProcessInJob(clone.VaCloneHandle, nullptr, &cloneInJob)) {
        cloneInJob = FALSE;
        (void)IsProcessInJob(parent, nullptr, &cloneInJob);
    }

    block->cloneInJob = cloneInJob ? 1u : 0u;
    CrashMetadataInput input;
    input.hasCrashUtc = TryUtcTicks(input.crashUtcTicks);
    input.helper.status = "Running";
    BOOL inJob = FALSE;
    input.helper.inJob = IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) && inJob;
    input.dump = {"Pending", "Helper", "Standard", CrashDumpFileText, 0, nullptr};
    usize jsonOffset = 0;
    const usize recordBytes = FormatCrashRecordStream(helperStream, sizeof(helperStream),
                                                      helperRecord, input, jsonOffset);
    CrashReportFolder report;
    report.primary = helperPrimaryFolder;
    report.fallback = helperFallbackFolder;
    OpenCrashReportFolder(report, helperStream + jsonOffset, recordBytes - jsonOffset);
    CrashDumpFile dumpFile;
    if (report.current != nullptr) {
        dumpFile = OpenCrashDumpFile(report.current, CrashDumpFileName);
        if (dumpFile.handle == INVALID_HANDLE_VALUE &&
            MoveCrashReportToFallback(report, dumpFile.error, helperStream + jsonOffset,
                                      recordBytes - jsonOffset)) {
            dumpFile = OpenCrashDumpFile(report.current, CrashDumpFileName);
        }
    }

    PublishFolder(report);
    SetEvent(captureDoneEvent);
    if (report.current != nullptr) {
        FormatCrashComment(helperComment, sizeof(helperComment), helperRecord);
        CrashDumpSource source;
        source.process = reinterpret_cast<HANDLE>(snapshot);
        source.processId = GetProcessId(parent);
        source.snapshot = true;
        source.failingThreadId = helperFailingThreadId;
        source.pointers = reinterpret_cast<EXCEPTION_POINTERS*>(helperExceptionPointers);
        source.clientPointers = true;
        source.ranges = helperRanges;
        source.rangeCount = CrashRecordRangeCount;
        // 빠른 Standard 덤프를 먼저 확보하고, 큰 덤프는 같은 snapshot에서 별도 파일로 쓴다. 마감은
        // 종류마다 따로다(부모 종료와 무관하다).
        CrashDumpWrite standard{"Failed", dumpFile.error, true};
        if (dumpFile.handle != INVALID_HANDLE_VALUE) {
            standard = WriteOpenedCrashDumpFile(
                dumpFile, report.current, CrashDumpFileName, StandardCrashDumpType(), source,
                helperStream, recordBytes, helperComment, DeadlineAfter(helperStandardDeadline));
        }

        input.dump.status = standard.status;
        input.dump.error = standard.error;
        input.dump.file = standard.error == ERROR_SUCCESS ? CrashDumpFileText : nullptr;
        // 부모가 job 안 도우미를 기다리는 지점이다(기본 덤프가 끝났다).
        SetEvent(standardDumpEvent);
        if (detail != CrashDumpDetail::Standard) {
            input.detailDump = {"Skipped", "Helper", CrashDumpDetailName(detail),
                                nullptr,   0,        "standard dump was not written"};
            if (standard.error == ERROR_SUCCESS && report.current != nullptr) {
                const CrashDumpWrite extra = WriteCrashDumpFile(
                    report.current, CrashDetailDumpFileName, DetailCrashDumpType(detail), source,
                    helperStream, recordBytes, helperComment, DeadlineAfter(helperDetailDeadline));
                input.detailDump.status = extra.status;
                input.detailDump.error = extra.error;
                input.detailDump.file =
                    extra.error == ERROR_SUCCESS ? CrashDetailDumpFileText : nullptr;
                input.detailDump.reason = nullptr;
            }
        }
    }

    SetEvent(standardDumpEvent);
    (void)PssFreeSnapshot(GetCurrentProcess(), snapshot);
    // 부모의 종료 콜백 결과를 기다린다. 부모가 먼저 끝나면 결과는 알 수 없다.
    const HANDLE waits[] = {parentDoneEvent, parent};
    const DWORD which = WaitForMultipleObjects(2, waits, FALSE, helperParentWait);
    if (which == WAIT_OBJECT_0 &&
        AtomicLoad<MemoryOrder::Acquire>(&block->parentResultReady) != 0) {
        char status[CrashStatusBytes];
        for (usize index = 0; index < CrashStatusBytes; ++index) {
            status[index] = block->callbackStatus[index];
        }

        status[CrashStatusBytes - 1] = '\0';
        CopyText(helperRecord.callbackStatus, status);
        helperRecord.callbackMilliseconds = block->callbackMilliseconds;
        helperRecord.reporterFailed = block->reporterFailed != 0;
        helperRecord.stoppedTotal = block->stoppedTotal;
        helperRecord.stoppedCount = ClampCount(block->stoppedCount, MaxStoppedCrashThreads);
        for (usize index = 0; index < MaxStoppedCrashThreads; ++index) {
            helperRecord.stoppedIds[index] = block->stoppedIds[index];
        }

    } else {
        CopyText(helperRecord.callbackStatus, "Unknown");
    }

    if (report.current != nullptr) {
        input.final = true;
        input.fallbackDirectory = report.fallbackUsed;
        const usize finalBytes =
            FormatCrashMetadata(helperStream, sizeof(helperStream), helperRecord, input);
        (void)WriteCrashMetadataFile(report.current, helperStream, finalBytes, true);
    }

    AtomicStore<MemoryOrder::Release>(&block->state, uint32{HelperDumpDone});
}

// 부모가 요청·정상 종료 알림 없이 끝났다. 처리기가 돌지 않은 NTSTATUS 오류 종료(fast-fail, 보장
// 없는 stack overflow 등)만 기록한다. 정상 exit·_Exit(임의 코드), Ctrl-C(STATUS_CONTROL_C_EXIT),
// harness kill(0xFFFFFFFF)·작업 관리자(1)는 기록하지 않는다(설계 D28).
void HandleParentExit(HANDLE parent) noexcept {
    const uint32 state = AtomicLoad<MemoryOrder::Acquire>(&block->state);
    DWORD code = 0;
    if ((state != HelperStarting && state != HelperReady) || !GetExitCodeProcess(parent, &code) ||
        (code & NtStatusSeverityMask) != NtStatusErrorSeverity || code == ParentExitedByControlC) {
        return;
    }

    helperRecord = block->record;
    SanitizeRecord(helperRecord);
    CopyFolder(
        helperPrimaryFolder,
        block->primaryFolders[AtomicLoad<MemoryOrder::Acquire>(&block->primaryFolderIndex) & 1u]);
    CopyFolder(helperFallbackFolder, block->fallbackFolder);
    CrashMetadataInput input;
    input.final = true;
    input.unreportedExit = true;
    input.exitCode = code;
    input.hasCrashUtc = TryUtcTicks(input.crashUtcTicks);
    input.helper.status = "Running";
    const usize bytes =
        FormatCrashMetadata(helperStream, sizeof(helperStream), helperRecord, input);
    CrashReportFolder report;
    report.primary = helperPrimaryFolder;
    report.fallback = helperFallbackFolder;
    OpenCrashReportFolder(report, helperStream, bytes);
    if (report.fallbackUsed && report.current != nullptr) {
        // 임시 폴더에 썼으면 그 표시로 다시 쓴다.
        input.fallbackDirectory = true;
        const usize fallbackBytes =
            FormatCrashMetadata(helperStream, sizeof(helperStream), helperRecord, input);
        (void)WriteCrashMetadataFile(report.current, helperStream, fallbackBytes, true);
    }
}

int RunHelper(const HANDLE (&handles)[HandleCount]) noexcept {
    section = handles[0];
    const HANDLE parent = handles[1];
    requestEvent = handles[2];
    captureDoneEvent = handles[3];
    parentDoneEvent = handles[4];
    standardDumpEvent = handles[5];
    shutdownEvent = handles[6];
    // 도우미 자신의 실패가 대화상자로 멈추지 않게 한다(부모가 프로세스 종료를 보고 fallback한다).
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    (void)WerSetFlags(WER_FAULT_REPORTING_NO_UI);
    block = static_cast<CrashHelperBlock*>(
        MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CrashHelperBlock)));
    // 블록은 믿지 않는다. 머리·부모 pid·덤프 종류·마감을 검사하고 고정 값은 사본으로 옮긴다.
    if (block == nullptr || block->magic != BlockMagic ||
        block->version != CrashHelperProtocolVersion ||
        block->size != static_cast<uint32>(sizeof(CrashHelperBlock)) ||
        GetProcessId(parent) != block->parentPid ||
        block->dumpDetail > static_cast<uint32>(CrashDumpDetail::Full) ||
        block->standardDeadlineMilliseconds == 0 ||
        block->standardDeadlineMilliseconds > MaxCrashBudgetMilliseconds ||
        block->detailDeadlineMilliseconds == 0 ||
        block->detailDeadlineMilliseconds > MaxCrashBudgetMilliseconds ||
        block->parentWaitMilliseconds > MaxHelperParentWaitMilliseconds) {
        return HelperExitBadBlock;
    }

    helperDumpDetail = static_cast<CrashDumpDetail>(block->dumpDetail);
    helperStandardDeadline = block->standardDeadlineMilliseconds;
    helperDetailDeadline = block->detailDeadlineMilliseconds;
    helperParentWait = block->parentWaitMilliseconds;
    (void)LoadCrashDumpWriter();
    uint32 expected = HelperStarting;
    (void)AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
        &block->state, expected, uint32{HelperReady});
    // 부모 핸들도 기다리므로 부모가 끝나면 도우미도 끝난다(남지 않는다).
    const HANDLE waits[] = {requestEvent, shutdownEvent, parent};
    switch (WaitForMultipleObjects(3, waits, FALSE, INFINITE)) {
    case WAIT_OBJECT_0:
        HandleRequest(parent);
        return HelperExitSuccess;
    case WAIT_OBJECT_0 + 1:
        return HelperExitSuccess;
    case WAIT_OBJECT_0 + 2:
        HandleParentExit(parent);
        return HelperExitSuccess;
    default:
        return HelperExitWaitFailed;
    }
}

void LaunchPreparedHelper() noexcept {
    const CrashReportingOptions& options = CrashOptions();
    helperStartError = helperPathError;
    if (helperStartError == ERROR_SUCCESS) {
        helperStartError = CreateHelperObjects(options);
    }

    if (helperStartError == ERROR_SUCCESS) {
        FillBlock(options);
        helperStartError = LaunchHelper();
    }

    if (helperStartError != ERROR_SUCCESS) {
        // 시작 실패는 설치 실패가 아니다. 크래시 때 프로세스 안 fallback이 결과에 남긴다.
        ReleaseParentHandles();
    } else {
        block->helperPid = helperPid;
        ProtectBlock(PAGE_READONLY);
    }

    // 크래시 처리 스레드가 보는 표시는 시작 시도를 마친 뒤 켠다.
    helperRequested = true;
}

} // namespace

void PrepareCrashHelper(const CrashReportingOptions& options) noexcept {
    helperRequested = false;
    helperInJob = false;
    helperStartError = ERROR_SUCCESS;
    captureTimeoutMilliseconds = options.captureTimeoutMilliseconds;
    if (options.helper == CrashHelperPolicy::InProcess || options.embedded ||
        ROCKS_ADDRESS_SANITIZER) {
        return;
    }

    // 경로는 설치 때 굳힌다(Deferred도 설치 때의 옵션을 쓴다).
    helperPathError =
        ResolveHelperPath(options) ? ERROR_SUCCESS : static_cast<DWORD>(ERROR_FILENAME_EXCED_RANGE);
    if (options.helper == CrashHelperPolicy::Preferred) {
        LaunchPreparedHelper();
    }
}

CrashHelperStart StartDeferredCrashHelper() noexcept {
    const CrashReportingOptions& options = CrashOptions();
    if (helperRequested || options.helper != CrashHelperPolicy::Deferred || options.embedded ||
        ROCKS_ADDRESS_SANITIZER) {
        return CrashHelperStart::NotApplicable;
    }

    LaunchPreparedHelper();
    return helperProcess != nullptr ? CrashHelperStart::Started : CrashHelperStart::Failed;
}

void UpdateCrashHelperFolders() noexcept {
    if (block != nullptr) {
        ProtectBlock(PAGE_READWRITE);
        const uint32 next = (block->primaryFolderIndex & 1u) ^ 1u;
        CopyWide(block->primaryFolders[next], CrashPrimaryFolder());
        AtomicStore<MemoryOrder::Release>(&block->primaryFolderIndex, next);
        // 그 사이 다른 스레드가 크래시 보고를 시작했으면 W가 블록에 쓰는 중일 수 있다. 소유자가
        // 있으면 읽기 전용으로 되돌리지 않는다. 확인과 보호 변경 사이에 크래시가 시작될 수 있으므로
        // 되돌린 뒤 다시 보고 소유자가 생겼으면 쓰기로 되연다. 그래도 그 짧은 틈에 W가 블록에 쓰면
        // W가 실패할 수 있다(이 함수는 다른 스레드가 크래시를 낼 수 없는 부트 단계 전용이라 그 틈은
        // 계약 밖이다).
        if (CrashOwner() == NoCrashOwner) {
            ProtectBlock(PAGE_READONLY);
            if (CrashOwner() != NoCrashOwner) {
                ProtectBlock(PAGE_READWRITE);
            }
        }
    }
}

bool CrashHelperStarted() noexcept {
    return helperProcess != nullptr;
}

// 부모가 지금 job 안인가다. 도우미를 띄운 뒤에 job에 들어갈 수 있으므로(harness가 Process.Start 뒤
// AssignProcessToJobObject) 크래시 때 다시 읽는다(시스템 호출 하나, heap 없음).
namespace {

bool ParentInJobNow() noexcept {
    BOOL inJob = FALSE;
    return IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) && inJob;
}

} // namespace

uint32 CrashHelperJobWaitMilliseconds() noexcept {
    // 예산은 보수적으로 잡는다. 부모가 job 안이면 clone이 job에 남을 수 있다(실제 대기는
    // FinishCrashHelperReport가 도우미가 알린 값으로 정한다).
    return helperProcess != nullptr && (helperInJob || ParentInJobNow())
               ? CrashReportingOptions::DefaultHelperStandardDumpDeadlineMilliseconds +
                     CrashOptions().metadataTimeoutMilliseconds
               : 0u;
}

void SetCrashHelperCaptureHook(CrashHelperCaptureHook hook) noexcept {
    captureHook = hook;
}

void SetCrashHelperOverride(CrashHelperOverride body) noexcept {
    helperOverride = body;
}

CrashHelperCapture RequestCrashHelperCapture(EXCEPTION_POINTERS* pointers,
                                             CrashHelperOutcome& outcome) noexcept {
    outcome = CrashHelperOutcome{};
    outcome.inJob = helperInJob;
    if (!helperRequested) {
        return CrashHelperCapture::Unavailable;
    }

    if (helperProcess == nullptr) {
        outcome.status = "StartFailed";
        outcome.error = helperStartError;
        return CrashHelperCapture::Unavailable;
    }

    DWORD exitCode = 0;
    if (WaitForSingleObject(helperProcess, 0) == WAIT_OBJECT_0) {
        outcome.status = "ExitedEarly";
        outcome.hasExitCode = GetExitCodeProcess(helperProcess, &exitCode) != FALSE;
        outcome.exitCode = exitCode;
        return CrashHelperCapture::Unavailable;
    }

    // 기록 사본과 부모 주소(덤프가 snapshot 안에서 읽는다)를 넘긴다. 원본 예외 포인터는 넘기지
    // 않는다(정적 사본만, 설계 10.4절). 평소 읽기 전용인 view를 이제 쓰기로 연다.
    ProtectBlock(PAGE_READWRITE);
    CaptureCrashRecord(block->record);
    block->failingThreadId = static_cast<uint32>(block->record.failingThreadId);
    block->exceptionPointers = reinterpret_cast<uptr>(pointers);
    CrashMemoryRange ranges[CrashRecordRangeCount];
    CrashRecordRanges(ranges);
    for (usize index = 0; index < CrashRecordRangeCount; ++index) {
        block->ranges[index] = {reinterpret_cast<uptr>(ranges[index].address), ranges[index].bytes};
    }

    block->captureError = ERROR_SUCCESS;
    AtomicStore<MemoryOrder::Release>(&block->state, uint32{CrashRequested});
    SetEvent(requestEvent);
    const HANDLE waits[] = {captureDoneEvent, helperProcess};
    const DWORD which = WaitForMultipleObjects(2, waits, FALSE, captureTimeoutMilliseconds);
    if (which == WAIT_OBJECT_0) {
        if (block->captureError != ERROR_SUCCESS) {
            outcome.status = "CaptureFailed";
            outcome.error = block->captureError;
            return CrashHelperCapture::Unavailable;
        }

        outcome.status = "Running";
        return CrashHelperCapture::Captured;
    }

    if (which == WAIT_OBJECT_0 + 1) {
        outcome.status = "ExitedDuringCapture";
        outcome.hasExitCode = GetExitCodeProcess(helperProcess, &exitCode) != FALSE;
        outcome.exitCode = exitCode;
        return CrashHelperCapture::Unavailable;
    }

    // 캡처 마감을 넘겼다. 요청을 거둔다. 그 사이 도우미가 캡처를 끝냈으면 그대로 쓴다.
    uint32 expected = CrashRequested;
    if (!AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
            &block->state, expected, uint32{RequestWithdrawn}) &&
        expected == HelperCaptured) {
        outcome.status = "Running";
        return CrashHelperCapture::Captured;
    }

    outcome.status = "CaptureTimedOut";
    return CrashHelperCapture::Unavailable;
}

void FinishCrashHelperReport() noexcept {
    CaptureCrashRecord(parentScratch);
    CopyText(block->callbackStatus, parentScratch.callbackStatus);
    block->callbackMilliseconds = parentScratch.callbackMilliseconds;
    block->reporterFailed = parentScratch.reporterFailed ? 1u : 0u;
    block->stoppedTotal = parentScratch.stoppedTotal;
    block->stoppedCount = parentScratch.stoppedCount;
    for (usize index = 0; index < MaxStoppedCrashThreads; ++index) {
        block->stoppedIds[index] = parentScratch.stoppedIds[index];
    }

    AtomicStore<MemoryOrder::Release>(&block->parentResultReady, 1u);
    SetEvent(parentDoneEvent);
    // 캡처 마감 뒤 CAS로 알게 된 늦은 캡처는 도우미가 아직 폴더를 확정하지 않았을 수 있다. 폴더
    // 칸과 clone job 표시는 캡처 끝 신호 뒤에 읽는다(메타데이터 몫 안에서 기다린다, 예산에 들어
    // 있다).
    (void)WaitForSingleObject(captureDoneEvent, CrashOptions().metadataTimeoutMilliseconds);
    const bool cloneInJob = AtomicLoad<MemoryOrder::Acquire>(&block->cloneInJob) != 0;
    // 도우미나 VA clone이 job 안이면 부모가 끝나 job이 닫힐 때 함께 끝난다. 기본 덤프가 끝날
    // 때까지(도우미 종료 포함, 마감 안) 기다린다.
    if ((helperInJob || cloneInJob) && helperProcess != nullptr) {
        const HANDLE waits[] = {standardDumpEvent, helperProcess};
        (void)WaitForMultipleObjects(2, waits, FALSE, CrashHelperJobWaitMilliseconds());
        // 추가 덤프가 없으면 최종 CrashInfo.json까지 쓰고 도우미가 끝난다. 그것도 메타데이터 몫
        // 안에서 기다린다(추가 덤프는 기다리지 않는다, 기본 덤프와 예비본은 이미 있다).
        if (helperInJob && CrashOptions().dumpDetail == CrashDumpDetail::Standard) {
            (void)WaitForSingleObject(helperProcess, CrashOptions().metadataTimeoutMilliseconds);
        }
    }

    switch (AtomicLoad<MemoryOrder::Acquire>(&block->folderSlot)) {
    case FolderPrimary:
        HelperWriteText("Rocks crash report: ");
        WriteDiagnosticLine(CrashPrimaryFolderText());
        break;
    case FolderFallback:
        HelperWriteText("Rocks crash report: ");
        WriteDiagnosticLine(CrashFallbackFolderText());
        break;
    case FolderNone:
        WriteCrashFolderFailure(CrashPrimaryFolderText(), block->primaryError,
                                CrashFallbackFolderText(), block->fallbackError);
        break;
    default:
        break;
    }
}

void StopCrashHelper() noexcept {
    if (helperProcess != nullptr) {
        // 정상 종료는 어떤 종료 코드든 기록하지 않는다. Shutdown을 먼저 쓰고 깨운다.
        ProtectBlock(PAGE_READWRITE);
        AtomicStore<MemoryOrder::Release>(&block->state, uint32{ParentShutdown});
        SetEvent(shutdownEvent);
        if (WaitForSingleObject(helperProcess, ShutdownWaitMilliseconds) != WAIT_OBJECT_0) {
            (void)TerminateProcess(helperProcess, 1);
            (void)WaitForSingleObject(helperProcess, ShutdownWaitMilliseconds);
        }
    }

    ReleaseParentHandles();
    helperRequested = false;
    helperInJob = false;
}

} // namespace rocks::platform::detail

namespace rocks::platform {

uint32 CrashHelperProcessId() noexcept {
    return detail::helperPid;
}

bool RunCrashHelperIfRequested(int argc, char** argv, int& exitCode) noexcept {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr ||
        !detail::HelperTextEquals(argv[1], detail::CrashHelperArgument)) {
        return false;
    }

    uint64 version = 0;
    HANDLE handles[detail::HandleCount] = {};
    bool valid = argc == detail::HelperArgumentCount &&
                 detail::ParseNumber(argv[detail::VersionArgument], version) &&
                 version == detail::CrashHelperProtocolVersion;
    for (usize index = 0; valid && index < detail::HandleCount; ++index) {
        uint64 value = 0;
        valid = detail::ParseNumber(argv[detail::FirstHandleArgument + static_cast<int>(index)],
                                    value) &&
                value != 0;
        handles[index] = reinterpret_cast<HANDLE>(static_cast<uptr>(value));
    }

    if (valid && detail::helperOverride != nullptr) {
        // 시험 전용 고장 난 도우미다(부모 프로세스·요청 이벤트 핸들만 받는다).
        exitCode = detail::helperOverride(handles[1], handles[2]);
        return true;
    }

    exitCode = valid ? detail::RunHelper(handles) : detail::HelperExitBadArguments;
    return true;
}

bool RunCrashHelperIfRequested(int argc, wchar_t** argv, int& exitCode) noexcept {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
        return false;
    }

    // 도우미 인자는 ASCII다. 표지를 먼저 확인하고, 아니면 다른 인자는 보지 않는다.
    const char* marker = detail::CrashHelperArgument;
    usize index = 0;
    for (; marker[index] != '\0' && argv[1][index] == static_cast<wchar_t>(marker[index]);
         ++index) {
    }

    if (marker[index] != '\0' || argv[1][index] != L'\0') {
        return false;
    }

    char text[detail::HelperArgumentCount][detail::MaxHelperArgumentBytes + 1] = {};
    char* narrow[detail::HelperArgumentCount] = {};
    if (argc != detail::HelperArgumentCount) {
        exitCode = detail::HelperExitBadArguments;
        return true;
    }

    for (int argument = 0; argument < argc; ++argument) {
        narrow[argument] = text[argument];
        // 실행 파일 경로(0번)는 쓰지 않는다.
        if (argument == 0 || argv[argument] == nullptr) {
            continue;
        }

        usize length = 0;
        for (; argv[argument][length] != L'\0'; ++length) {
            const wchar_t unit = argv[argument][length];
            if (length >= detail::MaxHelperArgumentBytes || unit > 0x7F) {
                exitCode = detail::HelperExitBadArguments;
                return true;
            }

            text[argument][length] = static_cast<char>(unit);
        }
    }

    return RunCrashHelperIfRequested(argc, narrow, exitCode);
}

CrashHelperStart StartCrashHelper() noexcept {
    if (!detail::CrashStateInstalled()) {
        return CrashHelperStart::NotApplicable;
    }

    return detail::StartDeferredCrashHelper();
}

} // namespace rocks::platform
