#pragma once

#include <Platform/CrashReporting.h>

// 크래시 수집기의 플랫폼 공통 상태다(정적 저장소, 할당 없음). 플랫폼 구현(Win64·Posix)이 OS
// 처리기·크래시 처리 스레드·파일 쓰기를 맡고, 소유권·종료 콜백·크래시 기록·연관 파일 칸·
// CrashInfo.json 서식(CrashMetadata.cpp)은 여기서 공유한다.
namespace rocks::platform::detail {

inline constexpr uint64 NoCrashOwner = 0;
// 정상 종료(RemoveCrashReporting)가 소유권 자리를 막았다는 표지다.
inline constexpr uint64 ShutdownCrashOwner = NumericLimits<uint64>::Max();
inline constexpr usize MaxCrashMessageBytes = 2048;
inline constexpr usize MaxFatalDiagnosticBytes = 4096;
inline constexpr uint64 NanosecondsPerMillisecond = 1000000;
// 크래시한 스레드가 처리 스레드 예산 뒤 더 기다리는 여유다(완료 신호·기록 마무리).
inline constexpr uint32 ReportMarginMilliseconds = 1000;
// 크래시 폴더 이름(<program>-<YYYYMMDDTHHMMSSZ>-<pid>)의 byte 상한(NUL 포함)이다.
inline constexpr usize CrashFolderNameBytes = 128;
// 실행 파일 이름(경로 제외)의 byte 상한(NUL 포함)이다.
inline constexpr usize CrashExecutableNameBytes = 256;
// CrashInfo.json과 덤프 사용자 stream을 서식하는 정적 버퍼 크기다.
inline constexpr usize CrashMetadataBytes = 16 * 1024;
// 덤프 사용자 stream(RocksCrashRecord/1)의 형식 이름과 stream 종류다. stream 내용은 이 이름과
// 줄바꿈 뒤 예비 CrashInfo.json이다. 종류 값은 MINIDUMP_STREAM_TYPE 예약 범위(0xFFFF) 밖의 "RK"
// 값이다.
inline constexpr char CrashRecordStreamName[] = "RocksCrashRecord/1";
inline constexpr uint32 CrashRecordStreamType = 0x524B0001;
// 예산·여유 칸의 상한(10분)이다. 넘으면 설치가 InvalidArgument다(마감 계산 넘침 방지).
inline constexpr uint32 MaxCrashBudgetMilliseconds = 10 * 60 * 1000;

// 단계 이름 표다(SetRunPhase). 칸은 nullptr에서 이름으로 CAS로 한 번만 채우고 비우지 않는다. 같은
// 포인터를 동시에 처음 등록해도 한 칸만 쓴다(CAS에 진 쪽은 이긴 이름을 보고 같으면 그 칸을 쓴다).
// 표가 차면 0(이름 없음)이고 칸을 덮어쓰지 않는다. 프로세스 전역 표 하나를 쓰고, 시험은 지역 표로
// 가득 참·동시 등록을 본다.
struct ROCKS_PLATFORMBASE_API RunStageNameTable {
    const char* names[MaxRunStageNames] = {};

    // name의 (칸 번호 + 1)이다. name이 nullptr이거나 표가 찼으면 0이다.
    uint32 Register(const char* name) noexcept;

    // Register가 돌려준 값의 이름이다. 0이나 범위 밖이면 nullptr다.
    const char* Find(uint32 slot) const noexcept;
};

// Fatal·FatalAt이 보고 경로로 넘기는 발생 정보다. 크래시한 스레드의 stack에 있고, 보고가 끝날
// 때까지 그 프레임이 풀리지 않는다.
struct FatalOrigin {
    TerminationReason reason;
    const char* message;
    usize messageBytes;
    const char* file;
    uint32 line;
    const void* returnAddress;
};

enum class CrashClaim : uint8 {
    Owner,      // 처음 들어왔다. 보고한다
    Recursive,  // 소유자 스레드가 다시 들어왔다(처리기 안 실패)
    Concurrent, // 다른 스레드가 이미 보고 중이다
    Shutdown,   // 정상 종료가 자리를 막았다
};

// 설치 상태와 옵션이다(설치·제거 때만 쓴다).
void ResetCrashState(const CrashReportingOptions& options) noexcept;
bool CrashStateInstalled() noexcept;
void SetCrashStateInstalled(bool installed) noexcept;
const CrashReportingOptions& CrashOptions() noexcept;

// 소유권. 원자 CAS 하나가 프로세스 전체의 첫 보고를 정한다.
CrashClaim ClaimCrash(uint64 threadId) noexcept;
uint64 CrashOwner() noexcept;
// 정상 종료가 소유권 자리를 막는다. 이미 보고 중이면 false다.
bool BlockCrashOwnerForShutdown() noexcept;
void ReleaseCrashOwnerAfterShutdown() noexcept;

// 소유자가 정한 종료 코드와 마감(MonotonicNanoseconds)이다. 겹친 스레드가 읽는다.
void PublishOwnerTermination(uint32 exitCode, uint64 deadlineNanoseconds) noexcept;
uint32 OwnerExitCode() noexcept;
uint64 OwnerDeadline() noexcept;

// 겹쳐 실패한 스레드를 칸에 남긴다(처음 MaxStoppedCrashThreads개).
void RecordStoppedThread(uint64 threadId) noexcept;

// 크래시 처리 스레드 id와 그 실패 표시다.
void SetReporterThreadId(uint64 threadId) noexcept;
uint64 ReporterThreadId() noexcept;
void MarkReporterFailed() noexcept;
bool ReporterFailed() noexcept;

// 크래시 처리 스레드가 종료 콜백을 모두 마쳤다는 표시다. 내장 모드에서 호스트가 실행을 이어 가면
// 제거(RemoveCrashReporting)가 이 표시를 보고 기다리지 않는다.
void MarkReportCompleted() noexcept;
bool ReportCompleted() noexcept;

// 옵션 범위 검사다(예산 1–MaxCrashBudgetMilliseconds, 여유 0–MaxCrashBudgetMilliseconds).
bool ValidateCrashOptions(const CrashReportingOptions& options) noexcept;

// 옵션 문자열 검사다(이름 문법·길이 상한). PrepareCrashIdentity와 같은 규칙이고 상태를 바꾸지
// 않는다.
bool ValidateCrashIdentity(const CrashReportingOptions& options) noexcept;

// 크래시 처리 스레드의 진행 단계다. 처리 스레드가 종료 콜백 안에서 실패하면 남은 콜백만 포기하고
// 파일 단계(메타데이터·덤프)를 이어 간다.
enum class ReporterStage : uint32 {
    Idle,
    Callbacks,
    Files,
};
void SetReporterStage(ReporterStage stage) noexcept;
// 처리 스레드가 실패했을 때 부른다. 콜백 단계였으면 파일 단계로 옮기고(콜백 결과는 ReporterFailed)
// true다. 아니면(파일 단계에서 실패) false다. 어느 쪽이든 reporterFailed를 남긴다.
bool TakeOverFilesAfterReporterFailure() noexcept;

// 설치 때 옵션 문자열을 검사·복사하고 크래시 id·시작 시각·폴더 이름·실행 파일 이름을 만든다.
// 잘못된 이름·길이 초과면 false다(상태는 바뀌지 않는다).
bool PrepareCrashIdentity(const CrashReportingOptions& options) noexcept;
const char* CrashFolderName() noexcept;
const char* CrashProgramName() noexcept;
// 출력 폴더 인자(UTF-8) 검사다(비어 있지 않고 상한 안).
bool IsValidCrashDirectoryText(const char* directory) noexcept;

// 소유자가 기록한다. message는 복사한다.
void RecordCrashOrigin(TerminationReason reason, uint64 failingThreadId, const char* message,
                       usize messageBytes, const char* file, uint32 line,
                       const void* returnAddress) noexcept;

// 접근 위반의 접근 종류다.
enum class FaultAccess : uint8 {
    None,
    Read,
    Write,
    Execute,
};

// 크래시 코드의 종류다(Windows 예외 코드, POSIX 신호 번호).
enum class CrashCodeKind : uint8 {
    Exception,
    Signal,
};

// 소유자가 기록하는 예외·신호 정보다.
void RecordCrashException(CrashCodeKind kind, uint32 code, uint64 address, FaultAccess access,
                          uint64 accessAddress) noexcept;
// 소유자 스레드가 자기 platform::Thread 이름(없으면 nullptr)을 복사한다.
void RecordCrashThreadName(const char* name) noexcept;
uint64 CrashFailingThreadId() noexcept;

// 지금 스레드가 platform::Thread로 시작했으면 그 이름(빈 문자열 가능), 아니면 nullptr다.
// thread_local 상태를 읽기만 한다(할당·lock 없음). Common/Thread.cpp에 있다.
const char* CurrentPlatformThreadName() noexcept;

// 덤프 결과다(CrashInfo.json "dump"·"detailDump"). status가 nullptr이면 null로 적는다.
struct CrashDumpOutcome {
    const char* status = nullptr; // Pending·Written·Failed·Cancelled·Unsupported·Skipped
    const char* writer = nullptr; // InProcess 등
    const char* detail = nullptr; // Standard·Detailed·Full
    const char* file = nullptr;   // 크래시 폴더 안 파일 이름
    uint32 error = 0;             // OS 오류 코드(없으면 0)
    const char* reason = nullptr; // Unsupported·Skipped 사유
};

inline constexpr usize MaxCrashLimitations = 4;

// 덤프 도우미의 상태다(CrashInfo.json "helper"). status가 nullptr이면 도우미를 쓰지 않은 것이고
// null로 적는다.
struct CrashHelperOutcome {
    const char* status = nullptr; // Running·StartFailed·ExitedEarly·ExitedDuringCapture·
                                  // CaptureTimedOut·CaptureFailed
    uint32 error = 0;             // OS 오류 코드(없으면 0)
    bool hasExitCode = false;
    uint32 exitCode = 0;
    bool inJob = false; // 도우미가 부모의 job 안에 있다(breakaway 실패)
};

// CrashInfo.json 한 판을 만드는 입력이다. 크래시 기록·설치 정보는 CrashRecordSnapshot에서 읽는다.
struct CrashMetadataInput {
    bool final = false;
    bool fallbackDirectory = false;
    bool hasCrashUtc = false;
    int64 crashUtcTicks = 0;
    // 보고 없는 비정상 종료 기록이다(도우미가 부모의 종료 코드만 안다). 크래시 칸은 null이다.
    bool unreportedExit = false;
    uint32 exitCode = 0;
    CrashDumpOutcome dump;
    CrashDumpOutcome detailDump;
    CrashHelperOutcome helper;
    const char* limitations[MaxCrashLimitations] = {};
    usize limitationCount = 0;
};

struct CrashRecordSnapshot;

// CrashInfo.json을 buffer에 서식하고 길이(NUL 제외)를 돌려준다. 할당·lock이 없다(크래시 처리
// 스레드·도우미가 부른다). 버퍼가 모자라면 문자열 칸을 잘라 incompleteReasons에
// "metadataTruncated"를 남긴다. 결과는 늘 올바른 JSON이다.
ROCKS_PLATFORMBASE_API usize FormatCrashMetadata(char* buffer, usize capacity,
                                                 const CrashRecordSnapshot& record,
                                                 const CrashMetadataInput& input) noexcept;

// 고정 버퍼 JSON 쓰기 도구다(할당 없음). 문자열은 escape하고, 잘못된 UTF-8은 U+FFFD로 바꾼다.
// 문자열이 남은 자리(reserve 제외)를 넘으면 잘라 닫고 Truncated()가 참이 된다. 구조 기호·숫자는
// reserve 자리에 쓰므로 호출자가 reserve보다 작은 고정 부분만 쓰면 결과가 늘 닫힌다.
class ROCKS_PLATFORMBASE_API CrashJsonWriter {
public:
    CrashJsonWriter(char* buffer, usize capacity, usize reserve) noexcept;

    void Raw(const char* text) noexcept;
    void Raw(const char* text, usize bytes) noexcept;
    // 따옴표로 감싼 가변 문자열이다. text가 nullptr이면 null이다. escape한 내용이 maxBytes나 남은
    // 자리(reserve 제외)를 넘으면 글자 경계에서 잘라 닫고 Truncated()가 참이 된다.
    void String(const char* text, usize maxBytes = NumericLimits<usize>::Max()) noexcept;
    void String(const char* text, usize bytes, usize maxBytes) noexcept;
    // 따옴표로 감싼 고정 문자열이다(escape가 필요 없는 짧은 ASCII: 열거 이름·크래시 id·프로그램
    // 이름). reserve 자리에도 쓴다(가변 칸이 잘려도 빠지지 않는다). nullptr이면 null이다.
    void Constant(const char* text) noexcept;
    // 호출자가 칸을 버렸음을 남긴다(Truncated()가 참이 된다).
    void MarkTruncated() noexcept {
        truncated_ = true;
    }

    void Unsigned(uint64 value) noexcept;
    // "0x" 뒤 digits자리 대문자 16진수 문자열(따옴표 포함)이다.
    void Hex(uint64 value, uint32 digits) noexcept;
    void Bool(bool value) noexcept;
    // ,"key": 형태로 키를 쓴다(첫 키 앞에는 쉼표가 없다). key는 escape가 필요 없는 ASCII다.
    void Key(const char* key) noexcept;
    void BeginObject() noexcept;
    void EndObject() noexcept;
    void BeginArray() noexcept;
    void EndArray() noexcept;
    // 배열 원소 앞 쉼표다.
    void Element() noexcept;

    bool Truncated() const noexcept {
        return truncated_;
    }

    usize Length() const noexcept {
        return length_;
    }

    // 남은 문자열 자리(reserve 제외)다.
    usize Remaining() const noexcept;

private:
    void Put(char value) noexcept;
    bool PutEscaped(const char* text, usize bytes, usize limit) noexcept;

    char* buffer_;
    usize capacity_;
    usize reserve_;
    usize length_ = 0;
    bool truncated_ = false;
    bool first_ = true;
};

// UtcTicks 값(0001-01-01부터 100ns)을 "YYYY-MM-DDTHH:MM:SSZ"(compact면 "YYYYMMDDTHHMMSSZ")로 쓴다.
// buffer는 21 byte 이상이어야 한다. 결과 길이를 돌려준다.
ROCKS_PLATFORMBASE_API usize FormatUtcTicks(int64 ticks, bool compact, char* buffer,
                                            usize capacity) noexcept;

// 예외 코드·신호 번호를 고정 버퍼에 서식한다(할당·CRT 없음). 결과 길이를 돌려준다.
usize FormatCodeMessage(char* buffer, usize capacity, const char* prefix, uint32 value,
                        bool hexadecimal) noexcept;

// 크래시 처리 스레드가 부른다. 등록 역순으로 콜백을 부르고 결과(전체 상태·걸린 시간)를 남긴다.
void RunTerminationCallbacks(uint64 deadlineNanoseconds) noexcept;

// 크래시 처리 스레드가 메타데이터·덤프 단계에서 남기는 정적 메모리 범위다(덤프에 넣는다).
struct CrashMemoryRange {
    const void* address;
    usize bytes;
};
inline constexpr usize CrashRecordRangeCount = 4;
void CrashRecordRanges(CrashMemoryRange (&ranges)[CrashRecordRangeCount]) noexcept;

// 크래시 처리 스레드가 쓰는 정적 메타데이터 버퍼다(CrashMetadataBytes).
char* CrashMetadataBuffer() noexcept;

inline constexpr usize CrashProgramBytes = CrashReportingOptions::MaxProgramNameBytes + 1;
inline constexpr usize CrashIdTextBytes = 33;
inline constexpr usize CrashStageBytes = 128;
inline constexpr usize CrashIdentityBytes = CrashReportingOptions::MaxIdentityBytes + 1;
inline constexpr usize CrashModuleJsonBytes = 768;
inline constexpr usize CrashFileNameBytes = 260;
inline constexpr usize CrashThreadNameBytes = 64;
inline constexpr usize CrashStatusBytes = 24;
inline constexpr usize CrashAnnotationCount = static_cast<usize>(CrashAnnotation::Count);

// CrashInfo.json을 만드는 데 필요한 크래시 기록 전부를 담은 고정 크기 사본이다(포인터 없음).
// 크래시 처리 스레드가 정적 저장소에서 복사하고(할당 없음), 도우미 교환 블록으로도 그대로 넘긴다.
// 문자열 칸은 NUL로 끝난다.
struct CrashRecordSnapshot {
    char program[CrashProgramBytes];
    char executable[CrashExecutableNameBytes];
    char crashId[CrashIdTextBytes];
    char stage[CrashStageBytes];
    char identity[CrashIdentityBytes];
    char moduleJson[CrashModuleJsonBytes];
    char message[MaxCrashMessageBytes];
    char file[CrashFileNameBytes];
    char failingThreadName[CrashThreadNameBytes];
    char callbackStatus[CrashStatusBytes];
    char annotations[CrashAnnotationCount][MaxCrashAnnotationBytes];
    uint32 annotationBytes[CrashAnnotationCount];
    uint64 stoppedIds[MaxStoppedCrashThreads];
    uint64 processStartTicks;
    uint64 address;
    uint64 accessAddress;
    uint64 failingThreadId;
    uint64 reporterThreadId;
    uint64 returnAddress;
    uint64 callbackMilliseconds;
    uint32 processId;
    uint32 argumentCount; // CrashReportingOptions::UnknownArgumentCount면 모른다
    uint32 messageBytes;
    uint32 identityBytes;
    uint32 code;
    uint32 line;
    uint32 stoppedTotal;
    uint32 stoppedCount;
    uint8 dumpDetail;
    uint8 reason;
    uint8 phase;
    uint8 codeKind;
    uint8 access;
    bool hasStage;
    bool hasException;
    bool hasFile;
    bool hasThreadName;
    bool hasModule;
    bool hasExecutable;
    bool reporterFailed;
    bool annotationTorn;
};

// 지금 크래시 기록을 record로 복사한다(할당·lock 없음, 연관 파일 칸은 seqlock으로 읽는다).
void CaptureCrashRecord(CrashRecordSnapshot& record) noexcept;

inline constexpr usize PeGuidTextBytes = 37;

// PE 이미지의 디버거 심볼 일치 정보다(CodeView RSDS 레코드와 실행 파일 저장소 키).
struct PeCodeView {
    char guid[PeGuidTextBytes] = {}; // 레지스트리 형식 대문자
    uint32 age = 0;
    const char* pdbName = nullptr; // 이미지 안을 가리킨다(경로를 뺀 파일 이름, NUL 종료 아님)
    usize pdbNameBytes = 0;
    uint32 timeDateStamp = 0;
    uint32 sizeOfImage = 0;
    bool hasCodeView = false;
};

// 메모리에 올라간 PE32+ 이미지 머리를 headerBytes 안에서 읽어 SizeOfImage를 돌려준다. 머리가
// 잘못됐거나 범위 밖이면 0이다.
ROCKS_PLATFORMBASE_API usize PeImageSize(const uint8* image, usize headerBytes) noexcept;

// 메모리에 올라간 PE32+ 이미지(RVA 배치, available byte)에서 CodeView 정보를 읽는다. 머리·디버그
// 디렉터리·레코드가 하나라도 available 밖이거나 형식이 틀리면 false다(module을 null로 둔다).
// CodeView가 없는 올바른 이미지는 true이고 hasCodeView가 거짓이다. 할당이 없다.
ROCKS_PLATFORMBASE_API bool ParsePeCodeView(const uint8* image, usize available,
                                            PeCodeView& out) noexcept;

// 플랫폼 모듈 정보(Windows PE CodeView) JSON 객체다. 없으면 nullptr다.
const char* CrashModuleJson() noexcept;

// 연관 파일 칸을 buffer(MaxCrashAnnotationBytes 이상)로 복사한다. 쓰는 도중이라 몇 번 다시 읽어도
// 일관되지 않으면 false다. 빈 칸은 true이고 bytes가 0이다.
ROCKS_PLATFORMBASE_API bool ReadCrashAnnotation(CrashAnnotation slot, char* buffer, usize capacity,
                                                usize& bytes) noexcept;

// 프로세스 시작 UTC(UtcTicks 표현)다. 플랫폼 구현(Windows는 프로세스 생성 시각, POSIX는 설치
// 시각)이 채운다. 실패하면 false다.
bool ProcessStartUtcTicks(int64& ticks) noexcept;

// 메타데이터 "build.platform" 값이다(Windows·MacOS·Android). 플랫폼 구현이 정한다.
const char* CrashPlatformName() noexcept;

// 끝내지 않고 영원히 기다린다(종료는 다른 스레드가 한다).
[[noreturn]] void ParkCurrentThread() noexcept;

// 비상 출력 한 줄(끝에 줄바꿈)이다.
void WriteDiagnosticLine(const char* text) noexcept;

} // namespace rocks::platform::detail
