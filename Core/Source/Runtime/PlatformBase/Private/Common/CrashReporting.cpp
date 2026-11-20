#include "CrashReportingInternal.h"

#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Process.h>
#include <Platform/SecureRandom.h>
#include <Platform/Thread.h>

namespace rocks::platform {

namespace {

// 종료 콜백 칸의 상태다.
enum CallbackSlotState : uint32 {
    SlotEmpty = 0,
    SlotReserved = 1,
    SlotRegistered = 2,
    SlotRunning = 3,
};

// 칸 상태와 세대를 한 word에 묶는다(상위 32 bit 세대, 하위 32 bit 상태). CAS 하나로 둘을 함께
// 확인해 해제가 다른 등록의 칸을 풀지 않게 한다(ABA 방지).
struct CallbackSlot {
    TerminationCallback callback;
    void* user;
    uint64 word;
};

constexpr uint32 GenerationShift = 32;
constexpr uint64 StateMask = NumericLimits<uint32>::Max();

constexpr uint64 MakeSlotWord(uint32 generation, uint32 state) noexcept {
    return (uint64{generation} << GenerationShift) | state;
}

constexpr uint32 SlotGeneration(uint64 word) noexcept {
    return static_cast<uint32>(word >> GenerationShift);
}

constexpr uint32 SlotStateOf(uint64 word) noexcept {
    return static_cast<uint32>(word & StateMask);
}

constexpr uint32 ParkSliceMilliseconds = 60000;
constexpr char HexDigits[] = "0123456789ABCDEF";
constexpr uint32 HexDigitsPerValue = 8;
constexpr uint32 BitsPerHexDigit = 4;
constexpr uint32 HexDigitMask = 0xF;
constexpr uint32 DecimalBase = 10;
constexpr usize MaxDecimalDigits = 10;
constexpr uint32 BitsPerByte = 8;
constexpr usize CrashIdBytes = 16;
constexpr usize CrashIdTextBytes = detail::CrashIdTextBytes;
static_assert(CrashIdTextBytes == CrashIdBytes * 2 + 1,
              "crash id text holds two hex digits per byte");
constexpr usize ThreadNameBytes = MaxThreadNameBytes + 1;
constexpr usize UtcCompactBytes = 24;
constexpr usize ExecutablePathBytes = 4096;
constexpr char DefaultProgramName[] = "Rocks";
constexpr uint32 AnnotationReadAttempts = 64;
constexpr usize AnnotationWords = MaxCrashAnnotationBytes / sizeof(uint64);
constexpr usize AnnotationSlotCount = static_cast<usize>(CrashAnnotation::Count);

// 연관 파일 칸 하나다. 쓰는 쪽 하나가 sequence를 홀수로 올리고 내용을 word 단위 원자 저장으로 쓴
// 뒤 짝수로 올린다(seqlock). 읽는 쪽은 앞뒤 sequence가 같고 짝수일 때만 받아들인다.
struct AnnotationSlot {
    uint32 sequence;
    uint32 bytes;
    uint64 words[AnnotationWords];
};

// 정적 저장소다. 설치·제거는 시작·종료 단계에서만 하고, 크래시 경로는 원자 연산만 쓴다.
constinit CallbackSlot callbackSlots[MaxTerminationCallbacks] = {};
constinit TerminationCallbackResult callbackResults[MaxTerminationCallbacks] = {};
constinit CrashReportingOptions options{};
constinit uint32 installed = 0;
constinit uint64 crashOwner = detail::NoCrashOwner;
constinit uint32 ownerExitCode = 0;
constinit uint64 ownerDeadline = 0;
constinit uint64 reporterThreadId = 0;
constinit uint32 reporterFailed = 0;
constinit uint32 reportCompleted = 0;
constinit uint64 stoppedThreadIds[MaxStoppedCrashThreads] = {};
constinit uint32 stoppedThreadCount = 0;
constinit uint32 stoppedThreadTotal = 0;
// 실행 단계는 한 워드(단계 이름 번호와 phase)로 둔다. 다른 스레드의 크래시가 "새 이름 + 옛 phase"를
// 읽지 않게 한다(LC6 리뷰). 이름은 정적 문자열이므로 표에 한 번만 등록하고 번호로 가리킨다. 표가
// 차면 이름 없이 phase만 남긴다.
constexpr uint32 StagePhaseBits = 8;
constexpr uint32 StagePhaseMask = (1u << StagePhaseBits) - 1;
static_assert(MaxRunStageNames <= (NumericLimits<uint32>::Max() >> StagePhaseBits),
              "The run stage word must hold every stage name slot");
constinit detail::RunStageNameTable stageNames;
// (이름 번호 + 1) << StagePhaseBits | phase다. 이름 번호 칸이 0이면 이름이 없다.
constinit uint32 runStage = static_cast<uint32>(RunPhase::Startup);
constinit uint32 reporterStage = static_cast<uint32>(detail::ReporterStage::Idle);
constinit AnnotationSlot annotations[AnnotationSlotCount] = {};

// 설치 때 고정하는 식별 정보다(설치 때만 쓴다).
constinit char programName[CrashReportingOptions::MaxProgramNameBytes + 1] = {};
constinit char executableName[detail::CrashExecutableNameBytes] = {};
constinit char crashId[CrashIdTextBytes] = {};
constinit char folderName[detail::CrashFolderNameBytes] = {};
constinit char identity[CrashReportingOptions::MaxIdentityBytes + 1] = {};
constinit usize identityBytes = 0;
constinit uint32 processId = 0;
constinit uint32 argumentCount = CrashReportingOptions::UnknownArgumentCount;
constinit int64 processStartTicks = 0;

// 종료 콜백 요약이다(크래시 처리 스레드만 쓴다).
constinit const char* callbackStatus = nullptr;
constinit uint64 callbackMilliseconds = 0;

// 크래시 메타데이터 서식 버퍼다(크래시 처리 스레드만 쓴다).
constinit char metadataBuffer[detail::CrashMetadataBytes] = {};

// 크래시 기록(소유자만 쓴다).
constinit TerminationReason crashReason = TerminationReason::Exception;
constinit uint64 failingThreadId = 0;
constinit char crashMessage[detail::MaxCrashMessageBytes] = {};
constinit usize crashMessageBytes = 0;
constinit const char* crashFile = nullptr;
constinit uint32 crashLine = 0;
constinit const void* crashReturnAddress = nullptr;
constinit bool crashHasException = false;
constinit detail::CrashCodeKind crashCodeKind = detail::CrashCodeKind::Exception;
constinit uint32 crashCode = 0;
constinit uint64 crashAddress = 0;
constinit detail::FaultAccess crashAccess = detail::FaultAccess::None;
constinit uint64 crashAccessAddress = 0;
constinit char failingThreadName[ThreadNameBytes] = {};
constinit bool hasFailingThreadName = false;

bool IsProgramNameCharacter(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-';
}

// text 길이가 limit 이하면 길이를 length에 두고 true다(NUL까지 limit + 1 byte만 본다).
bool MeasureBounded(const char* text, usize limit, usize& length) noexcept {
    length = 0;
    while (text[length] != '\0') {
        if (length == limit) {
            return false;
        }

        ++length;
    }

    return true;
}

usize CopyText(char* destination, usize capacity, const char* source, usize bytes) noexcept {
    usize length = 0;
    for (; length < bytes && length + 1 < capacity; ++length) {
        destination[length] = source[length];
    }

    destination[length] = '\0';
    return length;
}

usize AppendText(char* destination, usize capacity, usize length, const char* source) noexcept {
    for (usize index = 0; source[index] != '\0' && length + 1 < capacity; ++index) {
        destination[length++] = source[index];
    }

    destination[length] = '\0';
    return length;
}

// 실행 파일 경로에서 파일 이름만 남긴다(사용자 이름이 든 폴더 경로를 메타데이터에 남기지 않는다).
void CaptureExecutableName() noexcept {
    static char path[ExecutablePathBytes];
    usize bytes = 0;
    executableName[0] = '\0';
    if (!GetExecutablePath(path, sizeof(path), bytes)) {
        return;
    }

    usize start = 0;
    for (usize index = 0; index < bytes; ++index) {
        if (path[index] == '/' || path[index] == '\\') {
            start = index + 1;
        }
    }

    (void)CopyText(executableName, sizeof(executableName), path + start, bytes - start);
}

void CaptureCrashId() noexcept {
    uint8 random[CrashIdBytes] = {};
    if (!TryFillSecureRandom(random, sizeof(random))) {
        // 엔트로피가 없으면 pid와 시작 시각으로 대신한다(프로세스 사이 충돌만 피하면 된다).
        const uint64 parts[2] = {uint64{processId}, static_cast<uint64>(processStartTicks)};
        for (usize index = 0; index < CrashIdBytes; ++index) {
            random[index] = static_cast<uint8>(parts[index / sizeof(uint64)] >>
                                               ((index % sizeof(uint64)) * BitsPerByte));
        }
    }

    for (usize index = 0; index < CrashIdBytes; ++index) {
        crashId[index * 2] = HexDigits[random[index] >> BitsPerHexDigit];
        crashId[index * 2 + 1] = HexDigits[random[index] & HexDigitMask];
    }

    crashId[CrashIdTextBytes - 1] = '\0';
}

const char* CallbackResultName(TerminationCallbackResult result) noexcept {
    switch (result) {
    case TerminationCallbackResult::Completed:
        return "Completed";
    case TerminationCallbackResult::Skipped:
        return "Skipped";
    case TerminationCallbackResult::TimedOut:
        return "TimedOut";
    }

    return "Completed";
}

// 결과의 나쁨 순서다(전체 상태는 가장 나쁜 결과를 남긴다).
uint32 CallbackResultRank(TerminationCallbackResult result) noexcept {
    switch (result) {
    case TerminationCallbackResult::Completed:
        return 1;
    case TerminationCallbackResult::Skipped:
        return 2;
    case TerminationCallbackResult::TimedOut:
        return 3;
    }

    return 1;
}

} // namespace

bool SetCrashAnnotation(CrashAnnotation slot, const char* text, usize bytes) noexcept {
    const auto index = static_cast<usize>(slot);
    if (index >= AnnotationSlotCount || bytes > MaxCrashAnnotationBytes ||
        (text == nullptr && bytes != 0)) {
        return false;
    }

    AnnotationSlot& target = annotations[index];
    const uint32 sequence = AtomicLoad<MemoryOrder::Relaxed>(&target.sequence);
    AtomicStore<MemoryOrder::Relaxed>(&target.sequence, sequence + 1);
    // 홀수 sequence가 내용보다 먼저 보이게 한다(읽는 쪽이 쓰는 도중을 알아챈다).
    AtomicThreadFence<MemoryOrder::Release>();
    for (usize word = 0; word < AnnotationWords; ++word) {
        uint64 value = 0;
        for (usize offset = 0; offset < sizeof(uint64); ++offset) {
            const usize position = word * sizeof(uint64) + offset;
            if (position < bytes) {
                value |= uint64{static_cast<uint8>(text[position])} << (offset * BitsPerByte);
            }
        }

        AtomicStore<MemoryOrder::Relaxed>(&target.words[word], value);
    }

    AtomicStore<MemoryOrder::Relaxed>(&target.bytes, static_cast<uint32>(bytes));
    AtomicStore<MemoryOrder::Release>(&target.sequence, sequence + 2);
    return true;
}

bool RegisterTerminationCallback(TerminationCallback callback, void* user,
                                 TerminationCallbackHandle& handle) noexcept {
    if (callback == nullptr) {
        return false;
    }

    for (uint32 index = 0; index < MaxTerminationCallbacks; ++index) {
        CallbackSlot& slot = callbackSlots[index];
        uint64 word = AtomicLoad<MemoryOrder::Acquire>(&slot.word);
        if (SlotStateOf(word) != SlotEmpty ||
            !AtomicCompareExchangeStrong<MemoryOrder::Acquire, MemoryOrder::Relaxed>(
                &slot.word, word, MakeSlotWord(SlotGeneration(word), SlotReserved))) {
            continue;
        }

        slot.callback = callback;
        slot.user = user;
        const uint32 generation = SlotGeneration(word) + 1u;
        AtomicStore<MemoryOrder::Release>(&slot.word, MakeSlotWord(generation, SlotRegistered));
        handle.slot = index;
        handle.generation = generation;
        return true;
    }

    return false;
}

void UnregisterTerminationCallback(TerminationCallbackHandle& handle) noexcept {
    if (handle.slot >= MaxTerminationCallbacks) {
        return;
    }

    CallbackSlot& slot = callbackSlots[handle.slot];
    uint64 expected = MakeSlotWord(handle.generation, SlotRegistered);
    if (!AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
            &slot.word, expected, MakeSlotWord(handle.generation, SlotEmpty)) &&
        expected == MakeSlotWord(handle.generation, SlotRunning)) {
        // 크래시 처리가 이 콜백을 부르고 있다(또는 콜백 안에서 처리 스레드가 실패해 돌아오지
        // 않는다). user 객체를 파괴하지 않도록 돌아오지 않는다.
        detail::ParkCurrentThread();
    }

    handle = TerminationCallbackHandle{};
}

void SetRunPhase(RunPhase phase, const char* name) noexcept {
    const uint32 slot = stageNames.Register(name);
    AtomicStore<MemoryOrder::Release>(&runStage,
                                      (slot << StagePhaseBits) | static_cast<uint32>(phase));
}

RunPhase CurrentRunPhase() noexcept {
    return static_cast<RunPhase>(AtomicLoad<MemoryOrder::Acquire>(&runStage) & StagePhaseMask);
}

const char* CurrentRunStage() noexcept {
    return stageNames.Find(AtomicLoad<MemoryOrder::Acquire>(&runStage) >> StagePhaseBits);
}

bool IsCrashReportingInstalled() noexcept {
    return AtomicLoad<MemoryOrder::Acquire>(&installed) != 0;
}

namespace detail {

uint32 RunStageNameTable::Register(const char* name) noexcept {
    if (name == nullptr) {
        return 0;
    }

    for (uint32 index = 0; index < MaxRunStageNames; ++index) {
        const char* current = AtomicLoad<MemoryOrder::Acquire>(&names[index]);
        if (current == nullptr) {
            // 빈 칸을 이 이름으로 채운다. 지면 current가 먼저 채운 이름이 된다.
            if (AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
                    &names[index], current, name)) {
                return index + 1;
            }
        }

        if (current == name) {
            return index + 1;
        }
    }

    return 0;
}

const char* RunStageNameTable::Find(uint32 slot) const noexcept {
    return slot != 0 && slot <= MaxRunStageNames
               ? AtomicLoad<MemoryOrder::Acquire>(&names[slot - 1])
               : nullptr;
}

void ResetCrashState(const CrashReportingOptions& value) noexcept {
    options = value;
    // 문자열은 PrepareCrashIdentity가 복사했다. 호출자의 포인터를 들고 있지 않는다.
    options.programName = nullptr;
    options.outputDirectory = nullptr;
    options.identity = nullptr;
    options.helperExecutable = nullptr;
    AtomicStore<MemoryOrder::Relaxed>(&reporterStage, static_cast<uint32>(ReporterStage::Idle));
    callbackStatus = nullptr;
    callbackMilliseconds = 0;
    crashHasException = false;
    hasFailingThreadName = false;
    failingThreadName[0] = '\0';
    AtomicStore<MemoryOrder::Relaxed>(&reportCompleted, 0u);
    AtomicStore<MemoryOrder::Relaxed>(&crashOwner, NoCrashOwner);
    AtomicStore<MemoryOrder::Relaxed>(&ownerExitCode, 0u);
    AtomicStore<MemoryOrder::Relaxed>(&ownerDeadline, uint64{0});
    AtomicStore<MemoryOrder::Relaxed>(&reporterFailed, 0u);
    AtomicStore<MemoryOrder::Relaxed>(&stoppedThreadCount, 0u);
    AtomicStore<MemoryOrder::Relaxed>(&stoppedThreadTotal, 0u);
    crashMessageBytes = 0;
    crashMessage[0] = '\0';
}

bool CrashStateInstalled() noexcept {
    return IsCrashReportingInstalled();
}

void SetCrashStateInstalled(bool value) noexcept {
    AtomicStore<MemoryOrder::Release>(&installed, value ? 1u : 0u);
}

const CrashReportingOptions& CrashOptions() noexcept {
    return options;
}

CrashClaim ClaimCrash(uint64 threadId) noexcept {
    uint64 expected = NoCrashOwner;
    if (AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
            &crashOwner, expected, threadId)) {
        return CrashClaim::Owner;
    }

    if (expected == threadId) {
        return CrashClaim::Recursive;
    }

    return expected == ShutdownCrashOwner ? CrashClaim::Shutdown : CrashClaim::Concurrent;
}

uint64 CrashOwner() noexcept {
    return AtomicLoad<MemoryOrder::Acquire>(&crashOwner);
}

bool BlockCrashOwnerForShutdown() noexcept {
    uint64 expected = NoCrashOwner;
    return AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
        &crashOwner, expected, ShutdownCrashOwner);
}

void ReleaseCrashOwnerAfterShutdown() noexcept {
    AtomicStore<MemoryOrder::Release>(&crashOwner, NoCrashOwner);
}

void PublishOwnerTermination(uint32 exitCode, uint64 deadlineNanoseconds) noexcept {
    AtomicStore<MemoryOrder::Relaxed>(&ownerExitCode, exitCode);
    AtomicStore<MemoryOrder::Release>(&ownerDeadline, deadlineNanoseconds);
}

uint32 OwnerExitCode() noexcept {
    return AtomicLoad<MemoryOrder::Relaxed>(&ownerExitCode);
}

uint64 OwnerDeadline() noexcept {
    return AtomicLoad<MemoryOrder::Acquire>(&ownerDeadline);
}

void RecordStoppedThread(uint64 threadId) noexcept {
    const uint32 index = AtomicFetchAdd<MemoryOrder::Relaxed>(&stoppedThreadTotal, 1u);
    if (index < MaxStoppedCrashThreads) {
        AtomicStore<MemoryOrder::Relaxed>(&stoppedThreadIds[index], threadId);
        // 칸을 채운 뒤 개수를 올린다(읽는 쪽은 개수만큼만 읽는다). 칸 순서와 개수 순서가 다를 수
        // 있어 CAS로 차례를 지킨다.
        uint32 expected = index;
        while (!AtomicCompareExchangeWeak<MemoryOrder::Release, MemoryOrder::Relaxed>(
            &stoppedThreadCount, expected, index + 1)) {
            expected = index;
            YieldThread();
        }
    }
}

void SetReporterThreadId(uint64 threadId) noexcept {
    AtomicStore<MemoryOrder::Release>(&reporterThreadId, threadId);
}

uint64 ReporterThreadId() noexcept {
    return AtomicLoad<MemoryOrder::Acquire>(&reporterThreadId);
}

void MarkReporterFailed() noexcept {
    AtomicStore<MemoryOrder::Release>(&reporterFailed, 1u);
}

bool ReporterFailed() noexcept {
    return AtomicLoad<MemoryOrder::Acquire>(&reporterFailed) != 0;
}

void SetReporterStage(ReporterStage stage) noexcept {
    AtomicStore<MemoryOrder::Release>(&reporterStage, static_cast<uint32>(stage));
}

bool TakeOverFilesAfterReporterFailure() noexcept {
    MarkReporterFailed();
    uint32 expected = static_cast<uint32>(ReporterStage::Callbacks);
    if (!AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
            &reporterStage, expected, static_cast<uint32>(ReporterStage::Files))) {
        return false;
    }

    callbackStatus = "ReporterFailed";
    return true;
}

namespace {

// 옵션 문자열(이름 문법·길이, identity·폴더·도우미 경로 길이)을 검사한다. 상태를 바꾸지 않는다.
bool CheckCrashIdentity(const CrashReportingOptions& value, usize& programBytes,
                        usize& identityLength) noexcept {
    programBytes = 0;
    identityLength = 0;
    if (value.programName == nullptr ||
        !MeasureBounded(value.programName, CrashReportingOptions::MaxProgramNameBytes,
                        programBytes) ||
        programBytes == 0) {
        return false;
    }

    bool onlyDots = true;
    for (usize index = 0; index < programBytes; ++index) {
        if (!IsProgramNameCharacter(value.programName[index])) {
            return false;
        }

        onlyDots = onlyDots && value.programName[index] == '.';
    }

    if (onlyDots || (value.identity != nullptr &&
                     !MeasureBounded(value.identity, CrashReportingOptions::MaxIdentityBytes,
                                     identityLength))) {
        return false;
    }

    usize helperBytes = 0;
    if ((value.outputDirectory != nullptr && !IsValidCrashDirectoryText(value.outputDirectory)) ||
        (value.helperExecutable != nullptr &&
         (!MeasureBounded(value.helperExecutable, CrashReportingOptions::MaxHelperExecutableBytes,
                          helperBytes) ||
          helperBytes == 0))) {
        return false;
    }

    return true;
}

} // namespace

bool PrepareCrashIdentity(const CrashReportingOptions& value) noexcept {
    usize programBytes = 0;
    usize identityLength = 0;
    if (!CheckCrashIdentity(value, programBytes, identityLength)) {
        return false;
    }

    (void)CopyText(programName, sizeof(programName), value.programName, programBytes);
    identityBytes = value.identity != nullptr
                        ? CopyText(identity, sizeof(identity), value.identity, identityLength)
                        : 0;
    processId = CurrentProcessId();
    argumentCount = value.argumentCount;
    if (!ProcessStartUtcTicks(processStartTicks)) {
        processStartTicks = 0;
    }

    CaptureExecutableName();
    CaptureCrashId();
    char stamp[UtcCompactBytes] = {};
    (void)FormatUtcTicks(processStartTicks, true, stamp, sizeof(stamp));
    char pid[MaxDecimalDigits + 1];
    (void)FormatCodeMessage(pid, sizeof(pid), "", processId, false);
    usize length = CopyText(folderName, sizeof(folderName), programName, programBytes);
    length = AppendText(folderName, sizeof(folderName), length, "-");
    length = AppendText(folderName, sizeof(folderName), length, stamp);
    length = AppendText(folderName, sizeof(folderName), length, "-");
    (void)AppendText(folderName, sizeof(folderName), length, pid);
    return true;
}

const char* CrashFolderName() noexcept {
    return folderName;
}

const char* CrashProgramName() noexcept {
    return programName[0] != '\0' ? programName : DefaultProgramName;
}

bool IsValidCrashDirectoryText(const char* directory) noexcept {
    usize bytes = 0;
    return directory != nullptr &&
           MeasureBounded(directory, CrashReportingOptions::MaxOutputDirectoryBytes, bytes) &&
           bytes != 0;
}

void RecordCrashException(CrashCodeKind kind, uint32 code, uint64 address, FaultAccess access,
                          uint64 accessAddress) noexcept {
    crashCodeKind = kind;
    crashCode = code;
    crashAddress = address;
    crashAccess = access;
    crashAccessAddress = accessAddress;
    crashHasException = true;
}

void RecordCrashThreadName(const char* name) noexcept {
    hasFailingThreadName = name != nullptr;
    usize length = 0;
    if (name != nullptr) {
        for (; name[length] != '\0' && length + 1 < ThreadNameBytes; ++length) {
            failingThreadName[length] = name[length];
        }
    }

    failingThreadName[length] = '\0';
}

uint64 CrashFailingThreadId() noexcept {
    return failingThreadId;
}

namespace {

// source를 NUL 종료로 destination 칸에 복사한다(넘치면 자른다). source가 nullptr이면 빈 문자열이다.
template <usize Capacity>
usize CopyField(char (&destination)[Capacity], const char* source, usize bytes) noexcept {
    usize length = 0;
    if (source != nullptr) {
        for (; length < bytes && length + 1 < Capacity && source[length] != '\0'; ++length) {
            destination[length] = source[length];
        }
    }

    destination[length] = '\0';
    return length;
}

constexpr usize Unbounded = NumericLimits<usize>::Max();

} // namespace

void CaptureCrashRecord(CrashRecordSnapshot& record) noexcept {
    (void)CopyField(record.program, CrashProgramName(), Unbounded);
    record.hasExecutable = executableName[0] != '\0';
    (void)CopyField(record.executable, executableName, Unbounded);
    (void)CopyField(record.crashId, crashId, Unbounded);
    // 단계 이름과 phase는 같은 한 번의 읽기에서 꺼낸다.
    const uint32 stageWord = AtomicLoad<MemoryOrder::Acquire>(&runStage);
    const char* stage = stageNames.Find(stageWord >> StagePhaseBits);
    record.phase = static_cast<uint8>(stageWord & StagePhaseMask);
    record.hasStage = stage != nullptr;
    (void)CopyField(record.stage, stage, Unbounded);
    record.identityBytes = static_cast<uint32>(CopyField(record.identity, identity, identityBytes));
    const char* module = CrashModuleJson();
    record.hasModule = module != nullptr;
    (void)CopyField(record.moduleJson, module, Unbounded);
    record.messageBytes =
        static_cast<uint32>(CopyField(record.message, crashMessage, crashMessageBytes));
    record.hasFile = crashFile != nullptr;
    (void)CopyField(record.file, crashFile, Unbounded);
    record.hasThreadName = hasFailingThreadName;
    (void)CopyField(record.failingThreadName, failingThreadName, Unbounded);
    (void)CopyField(record.callbackStatus, callbackStatus, Unbounded);
    record.annotationTorn = false;
    for (usize slot = 0; slot < CrashAnnotationCount; ++slot) {
        usize bytes = 0;
        if (!ReadCrashAnnotation(static_cast<CrashAnnotation>(slot), record.annotations[slot],
                                 MaxCrashAnnotationBytes, bytes)) {
            record.annotationTorn = true;
            bytes = 0;
        }

        record.annotationBytes[slot] = static_cast<uint32>(bytes);
    }

    record.stoppedTotal = AtomicLoad<MemoryOrder::Relaxed>(&stoppedThreadTotal);
    record.stoppedCount = AtomicLoad<MemoryOrder::Acquire>(&stoppedThreadCount);
    for (uint32 index = 0; index < MaxStoppedCrashThreads; ++index) {
        record.stoppedIds[index] = AtomicLoad<MemoryOrder::Relaxed>(&stoppedThreadIds[index]);
    }

    record.processStartTicks = static_cast<uint64>(processStartTicks);
    record.argumentCount = argumentCount;
    record.address = crashAddress;
    record.accessAddress = crashAccessAddress;
    record.failingThreadId = failingThreadId;
    record.reporterThreadId = ReporterThreadId();
    record.returnAddress = reinterpret_cast<uptr>(crashReturnAddress);
    record.callbackMilliseconds = callbackMilliseconds;
    record.processId = processId;
    record.code = crashCode;
    record.line = crashLine;
    record.dumpDetail = static_cast<uint8>(options.dumpDetail);
    record.reason = static_cast<uint8>(crashReason);
    record.codeKind = static_cast<uint8>(crashCodeKind);
    record.access = static_cast<uint8>(crashAccess);
    record.hasException = crashHasException;
    record.reporterFailed = ReporterFailed();
}

bool ReadCrashAnnotation(CrashAnnotation slot, char* buffer, usize capacity,
                         usize& bytes) noexcept {
    bytes = 0;
    const auto index = static_cast<usize>(slot);
    if (index >= AnnotationSlotCount || capacity < MaxCrashAnnotationBytes) {
        return false;
    }

    const AnnotationSlot& source = annotations[index];
    for (uint32 attempt = 0; attempt < AnnotationReadAttempts; ++attempt) {
        const uint32 before = AtomicLoad<MemoryOrder::Acquire>(&source.sequence);
        if ((before & 1u) != 0) {
            YieldThread();
            continue;
        }

        const uint32 length = AtomicLoad<MemoryOrder::Relaxed>(&source.bytes);
        for (usize word = 0; word < AnnotationWords; ++word) {
            const uint64 value = AtomicLoad<MemoryOrder::Relaxed>(&source.words[word]);
            for (usize offset = 0; offset < sizeof(uint64); ++offset) {
                buffer[word * sizeof(uint64) + offset] =
                    static_cast<char>(static_cast<uint8>(value >> (offset * BitsPerByte)));
            }
        }

        // 내용 읽기가 뒤 sequence 읽기보다 늦게 보이지 않게 한다.
        AtomicThreadFence<MemoryOrder::Acquire>();
        if (AtomicLoad<MemoryOrder::Relaxed>(&source.sequence) == before &&
            length <= MaxCrashAnnotationBytes) {
            bytes = length;
            return true;
        }
    }

    return false;
}

void CrashRecordRanges(CrashMemoryRange (&ranges)[CrashRecordRangeCount]) noexcept {
    ranges[0] = {crashMessage, sizeof(crashMessage)};
    ranges[1] = {metadataBuffer, sizeof(metadataBuffer)};
    ranges[2] = {annotations, sizeof(annotations)};
    ranges[3] = {stoppedThreadIds, sizeof(stoppedThreadIds)};
}

char* CrashMetadataBuffer() noexcept {
    return metadataBuffer;
}

void MarkReportCompleted() noexcept {
    AtomicStore<MemoryOrder::Release>(&reportCompleted, 1u);
}

bool ReportCompleted() noexcept {
    return AtomicLoad<MemoryOrder::Acquire>(&reportCompleted) != 0;
}

bool ValidateCrashIdentity(const CrashReportingOptions& value) noexcept {
    usize programBytes = 0;
    usize identityLength = 0;
    return CheckCrashIdentity(value, programBytes, identityLength);
}

bool ValidateCrashOptions(const CrashReportingOptions& value) noexcept {
    const auto inBudget = [](uint32 milliseconds) noexcept {
        return milliseconds != 0 && milliseconds <= MaxCrashBudgetMilliseconds;
    };
    return inBudget(value.callbackBudgetMilliseconds) &&
           inBudget(value.inProcessDumpTimeoutMilliseconds) &&
           inBudget(value.metadataTimeoutMilliseconds) &&
           inBudget(value.captureTimeoutMilliseconds) &&
           value.helperDumpDeadlineMilliseconds <= MaxCrashBudgetMilliseconds &&
           static_cast<uint32>(value.helper) <= static_cast<uint32>(CrashHelperPolicy::Deferred) &&
           value.concurrentCrashGraceMilliseconds <= MaxCrashBudgetMilliseconds &&
           static_cast<uint32>(value.dumpDetail) <= static_cast<uint32>(CrashDumpDetail::Full);
}

void RecordCrashOrigin(TerminationReason reason, uint64 threadId, const char* message,
                       usize messageBytes, const char* file, uint32 line,
                       const void* returnAddress) noexcept {
    crashReason = reason;
    failingThreadId = threadId;
    usize length = 0;
    if (message != nullptr) {
        for (; length < messageBytes && length + 1 < MaxCrashMessageBytes; ++length) {
            crashMessage[length] = message[length];
        }
    }

    crashMessage[length] = '\0';
    crashMessageBytes = length;
    crashFile = file;
    crashLine = line;
    crashReturnAddress = returnAddress;
}

usize FormatCodeMessage(char* buffer, usize capacity, const char* prefix, uint32 value,
                        bool hexadecimal) noexcept {
    if (capacity == 0) {
        return 0;
    }

    usize length = 0;
    for (; prefix[length] != '\0' && length + 1 < capacity; ++length) {
        buffer[length] = prefix[length];
    }

    if (hexadecimal) {
        for (uint32 index = 0; index < HexDigitsPerValue && length + 1 < capacity; ++index) {
            const uint32 shift = (HexDigitsPerValue - 1 - index) * BitsPerHexDigit;
            buffer[length++] = HexDigits[(value >> shift) & HexDigitMask];
        }
    } else {
        char digits[MaxDecimalDigits];
        usize count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % DecimalBase);
            value /= DecimalBase;
        } while (value != 0 && count < MaxDecimalDigits);

        while (count > 0 && length + 1 < capacity) {
            buffer[length++] = digits[--count];
        }
    }

    buffer[length] = '\0';
    return length;
}

void RunTerminationCallbacks(uint64 deadlineNanoseconds) noexcept {
    SetReporterStage(ReporterStage::Callbacks);
    const uint64 start = MonotonicNanoseconds();
    uint32 worst = 0;
    callbackStatus = "None";
    const TerminationContext context{crashReason,         failingThreadId, stoppedThreadIds,
                                     &stoppedThreadCount, crashMessage,    crashMessageBytes,
                                     deadlineNanoseconds};
    for (uint32 index = MaxTerminationCallbacks; index-- > 0;) {
        CallbackSlot& slot = callbackSlots[index];
        uint64 word = AtomicLoad<MemoryOrder::Acquire>(&slot.word);
        const uint32 generation = SlotGeneration(word);
        if (SlotStateOf(word) != SlotRegistered ||
            !AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease, MemoryOrder::Acquire>(
                &slot.word, word, MakeSlotWord(generation, SlotRunning))) {
            continue;
        }

        const TerminationCallbackResult result = MonotonicNanoseconds() >= deadlineNanoseconds
                                                     ? TerminationCallbackResult::Skipped
                                                     : slot.callback(context, slot.user);
        callbackResults[index] = result;
        if (CallbackResultRank(result) > worst) {
            worst = CallbackResultRank(result);
            callbackStatus = CallbackResultName(result);
        }

        // 콜백이 돌아왔으므로 user 객체를 더 쓰지 않는다. 등록 상태로 돌려 해제가 기다리지 않게
        // 한다 (내장 모드에서 호스트가 실행을 이어 가는 경우).
        AtomicStore<MemoryOrder::Release>(&slot.word, MakeSlotWord(generation, SlotRegistered));
    }

    callbackMilliseconds = (MonotonicNanoseconds() - start) / NanosecondsPerMillisecond;
    SetReporterStage(ReporterStage::Files);
}

[[noreturn]] void ParkCurrentThread() noexcept {
    for (;;) {
        SleepFor(ParkSliceMilliseconds);
    }
}

void WriteDiagnosticLine(const char* text) noexcept {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }

    WriteDiagnostic(text, length);
    WriteDiagnostic("\n", 1);
}

} // namespace detail

bool ValidateCrashReportingOptions(const CrashReportingOptions& options) noexcept {
    return detail::ValidateCrashOptions(options) && detail::ValidateCrashIdentity(options);
}

} // namespace rocks::platform
