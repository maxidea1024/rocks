#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <SparksVm/Bytecode.h>

// Sparks VM의 실행 문맥이다(Docs/Design/SparksVm.ko.md 2–5절, LC5a). verifier를 통과한 모듈의
// 함수 하나를 부르고, 결과(반환 값·fault·실행 끝)와 쓴 명령어 수를 돌려준다. 실행 문맥은 bank마다
// 레지스터 stack 하나와 frame stack을 두며, 호출은 Lua 방식 register window다(SparksBytecode
// 4.1절).
//
// 언제 쓰나: 실행 모델(LC6)이 handler·함수를 돌릴 때, 시험·`sparks run`이 최상위 함수를 부를 때.
//
// 사용 예:
//   VmExecution execution(allocator);
//   const uint64 arguments[] = {2, 3};
//   auto called = execution.Call(module, function, Span<const uint64>(arguments, 2), {});
//   if (called && called.Value().outcome == VmOutcome::Returned) {
//       const uint64 sum = execution.ReturnedSlots()[0];
//   }
//
// fault(숫자 검사·예산 — SL 8.3절)는 Result 실패가 아니라 VmCallResult의 결과다. Result 실패는
// 호출자 오류(함수 번호·인자 수)와 할당 실패뿐이다.
//
// 수명·스레드: module은 Call 동안 살아 있어야 한다. 한 실행 문맥은 한 스레드에서 쓴다. 다 만든
// 모듈은 여러 실행 문맥이 함께 읽어도 된다.

namespace rocks::sparks {

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
// 않는다), callDepth는 frame 수 상한이다.
struct VmLimits {
    uint64 instructions = DefaultInstructionBudget;
    uint32 callDepth = MaxCallDepth;
};

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
    VmExecution(const VmExecution&) = delete;
    VmExecution& operator=(const VmExecution&) = delete;

    // module의 function을 부른다. scalars·refs는 그 bank의 매개변수 slot(함수 표 parameters — this
    // 포함, 선언 순)이다. module은 verifier를 통과해야 한다(ReadBytecodeFile·VerifyBytecode —
    // 통과하지 않은 모듈은 정의되지 않은 동작). 실패: function이 범위 밖이거나 인자 slot 수가
    // 다르거나 Buf 매개변수가 있으면(LC5b 전) InvalidArgument, stack 할당 실패는 OutOfMemory.
    Result<VmCallResult> Call(const BytecodeModule& module, uint32 function,
                              Span<const uint64> scalars, Span<const uint64> refs,
                              const VmLimits& limits = VmLimits()) noexcept;

    // 마지막 Call이 Returned일 때 돌려준 값 slot이다(함수 표 returnBank의 원시 값, returnSlots
    // 개). 다음 Call 전까지 유효하다.
    Span<const uint64> ReturnedSlots() const noexcept {
        return returned_.View();
    }

private:
    struct Frame {
        uint32 function = 0;
        uint32 pc = 0; // 이 frame으로 돌아왔을 때 실행할 명령의 모듈 단어 번호
        uint32 base[BytecodeBankCount] = {};
    };

    Result<void> PushFrame(uint32 function, const uint32 (&base)[BytecodeBankCount]) noexcept;
    Result<VmCallResult> Run(const VmLimits& limits) noexcept;
    Result<VmCallResult> MakeFault(VmFaultKind kind, uint32 word, uint64 steps) noexcept;

    const BytecodeModule* module_ = nullptr;
    // bank별 레지스터 stack이다. Buf는 LC5b의 값 버퍼 전까지 null(0)만 담는다.
    Array<uint64> scalars_;
    Array<uint64> refs_;
    Array<uint64> buffers_;
    Array<Frame> frames_;
    Array<uint64> returned_;
};

} // namespace rocks::sparks
