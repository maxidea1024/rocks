#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <SparksVm/Bytecode.h>
#include <SparksVm/VmBuffer.h>
#include <SparksVm/VmHost.h>

// Sparks VM의 실행 문맥이다(Docs/Design/SparksVm.ko.md 2–5절, LC5a·LC5b). verifier를 통과한 모듈의
// 함수 하나를 부르고, 결과(반환 값·fault·실행 끝)와 쓴 명령어 수를 돌려준다. 실행 문맥은 bank마다
// 레지스터 stack 하나와 frame stack을 두며, 호출은 Lua 방식 register window다(SparksBytecode
// 4.1절).
//
// 언제 쓰나: 실행 모델(LC6)이 handler·함수를 돌릴 때, 시험·`sparks run`이 최상위 함수를 부를 때.
//
// 사용 예:
//   VmExecution execution(allocator);
//   const uint64 arguments[] = {2, 3};
//   auto called = execution.Call(module, function, Span<const uint64>(arguments, 2), {}, {});
//   if (called && called.Value().outcome == VmOutcome::Returned) {
//       const uint64 sum = execution.ReturnedSlots()[0];
//   }
//
// fault(숫자 검사·예산 — SL 8.3절)는 Result 실패가 아니라 VmCallResult의 결과다. Result 실패는
// 호출자 오류(함수 번호·인자 수)와 할당 실패뿐이다.
//
// 값 버퍼(LC5b): Buf 값은 실행 문맥이 소유하는 참조계수 버퍼다. ledger(LiveBuffers·LiveBytes)는
// 살아 있는 버퍼를 세며, host가 만든 버퍼와 반환 값을 놓으면 0이어야 한다(SparksVm 2.2절 V7).
//
// 수명·스레드: module은 Call 동안 살아 있어야 한다. 한 실행 문맥은 한 스레드에서 쓴다. 다 만든
// 모듈은 여러 실행 문맥이 함께 읽어도 된다.

namespace rocks::sparks {

namespace detail {
class VmLibrary;
} // namespace detail

// 실행 조각 하나의 기본 명령어 예산이다(SL 9.3절).
inline constexpr uint64 DefaultInstructionBudget = 1000000;

// 호출 깊이 상한이다(frame 수 — SL 9.3절).
inline constexpr uint32 MaxCallDepth = 256;

// fault 종류다(SL 8.3절 표 + VM의 두 자리).
enum class VmFaultKind : uint8 {
    None,
    IndexOutOfRange,
    IntegerOverflow,
    DivideByZero,
    NonFinite,
    BudgetExceeded,
    CascadeLimit,
    AssertFailed,
    ContractViolation,
    // `FAULT 1`: 도달하지 않는 경로(SparksBytecode 3.1a절)다. verifier를 통과한 컴파일러 출력에서는
    // 나오지 않는다 — 나오면 엔진 버그로 보고한다(SL 8.4절 "실행").
    Unreachable,
    // `FAULT 2`: `on Awake`·`on Enable`·`on Start` 안의 `try` 실패(SD43 — 그 hook의 fault).
    HookTryFailed
};

const char* VmFaultKindName(VmFaultKind kind) noexcept;

// 호출 하나의 끝 모양이다.
enum class VmOutcome : uint8 {
    Returned, // RET — 값은 ReturnedSlots
    Faulted,  // fault — VmCallResult::fault
    Ended     // ENDEXEC — Result가 아닌 함수·handler의 `try` 실패(fault가 아니다, SL 8.4절)
};

// fault의 자리다. address는 함수 안 명령 주소, location은 그 명령의 원문 위치(디버그 정보가 없으면
// 0:0)다.
struct VmFault {
    VmFaultKind kind = VmFaultKind::None;
    uint32 function = NoBytecodeIndex;
    uint32 address = NoBytecodeIndex;
    BytecodeLocation location;
};

// 예산이다(SparksVm 4절). instructions는 이 호출이 실행할 수 있는 명령 수(EXTRA 단어는 세지
// 않는다), callDepth는 frame 수 상한, memoryBytes는 이 실행 문맥에 살아 있는 버퍼의 논리 byte
// 상한이다(host가 만든 버퍼 포함 — 넘기는 할당이 BudgetExceeded).
struct VmLimits {
    uint64 instructions = DefaultInstructionBudget;
    uint32 callDepth = MaxCallDepth;
    uint64 memoryBytes = DefaultMemoryBudget;
    // `Random`의 결정적 생성기 씨앗이다(SparksVm 6a절 — instance·session 씨앗은 LC6).
    uint64 randomSeed = 0;
};

// `Log.*` 기록의 수준이다(SparksVm 6a절 — host 로그 연결은 LC5d).
enum class VmLogLevel : uint8 {
    Info,
    Warning,
    Error,
    Debug
};

const char* VmLogLevelName(VmLogLevel level) noexcept;

struct VmCallResult {
    VmOutcome outcome = VmOutcome::Returned;
    VmFault fault;        // Faulted일 때
    uint64 errorCode = 0; // Ended일 때: ENDEXEC.S의 오류 코드(`Error` case 번호 + 1)
    uint64 steps =
        0; // 실행한 명령 수(fault를 낸 명령은 세지만 예산 초과로 막힌 명령은 세지 않는다)
};

class VmExecution final {
public:
    explicit VmExecution(Allocator& allocator) noexcept;
    // 남은 버퍼(반환 값 포함)를 모두 놓는다. host가 아직 놓지 않은 자기 버퍼는 호출자 버그다(Fatal
    // 대신 ledger로 보인다 — 소멸은 실패할 수 없다).
    ~VmExecution() noexcept;
    VmExecution(const VmExecution&) = delete;
    VmExecution& operator=(const VmExecution&) = delete;

    // module의 function을 부른다. scalars·refs·buffers는 그 bank의 매개변수 slot(함수 표 parameters
    // — this 포함, 선언 순)이다. Scalar 인자는 정규형이어야 한다(정수는 그 타입 범위의 부호·0 확장,
    // bool은 0·1, float32는 float 값의 double, 유한한 실수 — 검사는 LC5d host 경계가 매개변수
    // 종류로 한다). 실행은 결정적 부동소수점 환경 안에서
    // 한다(platform::RunWithDeterministicFloatingPoint). buffers는 빌려 준다(호출이 참조를 더하고
    // host의 참조는 그대로다 — null은 null 값). module은 verifier를 통과해야
    // 한다(ReadBytecodeFile·VerifyBytecode — 통과하지 않은 모듈은 정의되지 않은 동작). 이전 호출의
    // 반환 버퍼는 먼저 놓는다. 실패: function이 범위 밖이거나 인자 slot 수가 다르거나 import를 아직
    // 풀지 않았으면(LinkImports) InvalidArgument, stack·버퍼 할당 실패는 OutOfMemory(그때 실행
    // 중이던 버퍼는 모두 놓는다).
    Result<VmCallResult> Call(const VmLinkedModule& linked, uint32 function,
                              Span<const uint64> scalars, Span<const uint64> refs,
                              Span<VmBuffer* const> buffers,
                              const VmLimits& limits = VmLimits()) noexcept;

    // 마지막 Call이 Returned일 때 돌려준 Scalar·Ref 값 slot이다(함수 표 returnBank의 원시 값,
    // returnSlots 개 — Buf 반환이면 비어 있다). 다음 Call 전까지 유효하다.
    Span<const uint64> ReturnedSlots() const noexcept {
        return returned_.View();
    }

    // 마지막 Call이 Returned일 때 돌려준 Buf 값이다(Buf 반환이 아니면 비어 있다). 실행 문맥이 다음
    // Call·ReleaseReturned까지 소유한다.
    Span<VmBuffer* const> ReturnedBuffers() const noexcept {
        return returnedBuffers_.View();
    }

    // 반환된 Buf 값을 놓는다.
    void ReleaseReturned() noexcept;

    // host의 문자열 값을 만든다(참조 하나 — host가 Release로 놓는다). ledger가 센다. 예산은 보지
    // 않는다(host 입력이다). 실패: 단일 버퍼 상한을 넘으면 CapacityExceeded, 할당 실패는
    // OutOfMemory.
    Result<VmBuffer*> NewString(StringView text) noexcept;

    // host의 컨테이너 값(Array·Map·Set)을 만든다(참조 하나 — host가 Release로 놓는다). marker는
    // 원소 배치(EncodeContainerMarker — Array·Set은 값 자리, Map은 값과 key), elements는 원소마다
    // slot 묶음(Map은 key slot 뒤 값 slot)을 이은 것이다. Buf slot의 버퍼는 빌려 준다(컨테이너가
    // 참조를 더한다 — null은 null 값). ledger가 세고 예산은 보지 않는다(host 입력이다). 실패:
    // 종류가 컨테이너가 아니거나 배치·slot 수가 맞지 않거나 Map·Set에 같은 key가 둘이면
    // InvalidArgument, 단일 버퍼 상한을 넘으면 CapacityExceeded, 할당 실패는 OutOfMemory. 원소 값의
    // 정규형은 Call의 Scalar 인자와 같은 호출자 책임이다.
    Result<VmBuffer*> NewContainer(VmBufferKind kind, uint32 marker,
                                   Span<const uint64> elements) noexcept;

    // 참조 하나를 더한다(host가 버퍼를 보관할 때 — 6.2절). null이면 아무것도 하지 않는다.
    void Retain(VmBuffer* buffer) noexcept {
        if (buffer != nullptr) {
            ++buffer->references;
        }
    }

    // 참조 하나를 놓는다(0이면 해제 — 원소의 버퍼도). null이면 아무것도 하지 않는다.
    void Release(VmBuffer* buffer) noexcept;

    // 마지막 Call의 `Log.*` 기록이다(다음 Call 전까지 유효 — 할당 실패한 기록은 빠진다).
    uint32 LogCount() const noexcept {
        return static_cast<uint32>(logs_.Size());
    }

    VmLogLevel LogLevel(uint32 index) const noexcept {
        return logs_[index].level;
    }

    StringView LogMessage(uint32 index) const noexcept {
        return StringView(logText_.Data() + logs_[index].offset, logs_[index].length);
    }

    // 마지막 Call에서 할당 실패로 빠진 `Log.*` 기록 수다(로그는 실행 결과를 바꾸지 않으므로 fault가
    // 아니다 — 기록이 온전해야 하는 host·시험이 본다).
    uint32 DroppedLogs() const noexcept {
        return droppedLogs_;
    }

    // ledger: 살아 있는 버퍼 수와 논리 byte다.
    uint64 LiveBuffers() const noexcept {
        return liveBuffers_;
    }

    uint64 LiveBytes() const noexcept {
        return liveBytes_;
    }

private:
    friend class detail::VmLibrary;

    struct LogEntry {
        VmLogLevel level = VmLogLevel::Info;
        uint32 offset = 0;
        uint32 length = 0;
    };

    struct Frame {
        const VmLinkedModule* link = nullptr; // 이 frame 함수의 모듈(6.5절)
        uint32 function = 0;
        uint32 pc = 0; // 이 frame으로 돌아왔을 때 실행할 명령의 모듈 단어 번호
        uint32 base[BytecodeBankCount] = {};
    };

    // 새 버퍼 할당의 결과다: 버퍼, 또는 예산·상한 fault(BudgetExceeded), 또는 할당자 실패.
    struct Allocation {
        VmBuffer* buffer = nullptr;
        VmFaultKind fault = VmFaultKind::None;
        ErrorCode error = ErrorCode::None;
    };

    // 지금 frame의 bank별 레지스터 시작이다(컨테이너 명령이 쓴다).
    struct Banks {
        uint64* s = nullptr;
        uint64* r = nullptr;
        VmBuffer** b = nullptr;
    };

    Result<void> PushFrame(const VmLinkedModule& linked, uint32 function,
                           const uint32 (&base)[BytecodeBankCount]) noexcept;
    Result<VmCallResult> Run(const VmLimits& limits) noexcept;
    Result<VmCallResult> MakeFault(VmFaultKind kind, uint32 word, uint64 steps) noexcept;
    // 실행 중 Buf stack을 모두 놓고 frame을 비운다(fault·실행 끝·할당 실패).
    void Unwind() noexcept;
    // Buf stack [begin, end)를 놓고 null로 둔다.
    void ReleaseRange(usize begin, usize end) noexcept;
    // kind 버퍼를 elements개 원소(원소 하나 elementBytes byte)로 만든다. budget이 0이 아니면 예산을
    // 본다.
    Allocation Allocate(VmBufferKind kind, uint64 elements, uint64 elementBytes,
                        uint64 budget) noexcept;
    // 내용 payload byte의 버퍼다(원소 수 상한은 호출자가 본다). count·capacity는 0이다.
    Allocation AllocateBytes(VmBufferKind kind, uint64 payload, uint64 budget) noexcept;
    // 원소를 놓지 않고 버퍼 자체만 해제한다(원소의 참조를 다른 버퍼로 옮긴 뒤).
    void FreeShell(VmBuffer* buffer) noexcept;

    // 컨테이너 명령(NEWA·NEWSET·NEWM·APPEND·PUT·GETE·SETE·LEN·GETKEY·GETVAL — VmContainers.cpp)을
    // 실행한다. 반환: fault 종류(None이면 성공). 할당자 실패는 error.
    VmFaultKind Container(Opcode op, uint32 word, uint32 marker, const Banks& banks, uint64 budget,
                          ErrorCode& error) noexcept;
    // 레지스터 slot의 컨테이너가 이 실행만 쥔 버퍼이고 원소 needed개 자리가 있게 한다(COW 복제·
    // 증가). 빈 컨테이너는 marker의 배치를 적는다.
    VmFaultKind Reserve(VmBuffer*& slot, uint64 needed, uint32 marker, uint64 budget,
                        ErrorCode& error) noexcept;
    // struct·Buf Result 명령(NEWS·GETSF·SETSF·OK·ERR — VmStructs.cpp)을 실행한다.
    VmFaultKind Structured(Opcode op, uint32 word, uint32 extra, const Banks& banks, uint64 budget,
                           ErrorCode& error) noexcept;
    // 언어 함수 참조 ref를 창 window로 부른다(VmLibrary.cpp — SparksVm 6a절).
    VmFaultKind CallLanguage(uint32 ref, const Banks& window, uint64 budget,
                             ErrorCode& error) noexcept;
    // host 명령(GETF·SETF·GETCTX·CALLN·NEWLIT — VmHostCalls.cpp, SparksVm 6.2절)을 실행한다.
    VmFaultKind CallHost(Opcode op, uint32 word, uint32 extra, const Banks& banks,
                         ErrorCode& error) noexcept;
    // handle이 0이 아니고 host가 살아 있다고 답하는지다(SL 4.1절).
    bool Alive(uint64 handle) noexcept;
    // 오류 코드 code의 Buf Result(값 없음)를 만든다.
    Allocation MakeErrorResult(uint64 code, uint64 budget) noexcept;

    Allocator* allocator_;
    const BytecodeModule* module_ = nullptr;
    const VmLinkedModule* link_ = nullptr;
    // bank별 레지스터 stack이다. Buf slot은 null이 아니면 참조 하나를 가진다.
    Array<uint64> scalars_;
    Array<uint64> refs_;
    Array<VmBuffer*> buffers_;
    Array<Frame> frames_;
    Array<uint64> returned_;
    Array<VmBuffer*> returnedBuffers_;
    uint64 liveBuffers_ = 0;
    uint64 liveBytes_ = 0;
    // 언어 함수 묶음(참조 번호별 — VmLibrary가 배치를 정한다)과 그 모듈, Random·로그 상태.
    RandomStream random_{0};
    Array<LogEntry> logs_;
    uint32 droppedLogs_ = 0;
    Array<char> logText_;
};

} // namespace rocks::sparks
