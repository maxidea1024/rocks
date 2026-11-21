#include <SparksVm/Vm.h>

#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>

// Sparks VM 실행 문맥의 수명·frame·값 버퍼 관리다(Docs/Design/SparksVm.ko.md 2절, LC5a·LC5b).
// dispatch 루프는 VmDispatch.cpp, TOSTR 표기는 VmFormat.cpp다.

namespace rocks::sparks {

namespace {

// stack을 늘릴 때의 최소 배수다(호출마다 재할당하지 않게).
constexpr usize VmStackGrowthFactor = 2;

// 버퍼 내용의 정렬 단위(slot 크기)다.
constexpr uint64 VmPayloadAlignment = 8;

} // namespace

const char* VmFaultKindName(VmFaultKind kind) noexcept {
    switch (kind) {
    case VmFaultKind::None:
        return "None";
    case VmFaultKind::IndexOutOfRange:
        return "IndexOutOfRange";
    case VmFaultKind::IntegerOverflow:
        return "IntegerOverflow";
    case VmFaultKind::DivideByZero:
        return "DivideByZero";
    case VmFaultKind::NonFinite:
        return "NonFinite";
    case VmFaultKind::BudgetExceeded:
        return "BudgetExceeded";
    case VmFaultKind::CascadeLimit:
        return "CascadeLimit";
    case VmFaultKind::AssertFailed:
        return "AssertFailed";
    case VmFaultKind::ContractViolation:
        return "ContractViolation";
    case VmFaultKind::Unreachable:
        return "Unreachable";
    case VmFaultKind::HookTryFailed:
        return "HookTryFailed";
    }

    return "?";
}

VmExecution::VmExecution(Allocator& allocator) noexcept
    : allocator_(&allocator), scalars_(allocator), refs_(allocator), buffers_(allocator),
      frames_(allocator), returned_(allocator), returnedBuffers_(allocator) {
}

VmExecution::~VmExecution() noexcept {
    Unwind();
    ReleaseReturned();
}

Result<VmCallResult> VmExecution::Call(const BytecodeModule& module, uint32 function,
                                       Span<const uint64> scalars, Span<const uint64> refs,
                                       Span<VmBuffer* const> buffers,
                                       const VmLimits& limits) noexcept {
    if (function >= module.functions.Size()) {
        return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
    }

    const BytecodeFunction& entry = module.functions[function];
    const uint32 scalarBank = static_cast<uint32>(BytecodeBank::Scalar);
    const uint32 refBank = static_cast<uint32>(BytecodeBank::Ref);
    const uint32 bufBank = static_cast<uint32>(BytecodeBank::Buf);
    if (scalars.Size() != entry.parameters[scalarBank] ||
        refs.Size() != entry.parameters[refBank] || buffers.Size() != entry.parameters[bufBank]) {
        return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
    }

    ReleaseReturned();
    Unwind();
    returned_.Clear();
    module_ = &module;
    scalars_.Clear();
    refs_.Clear();
    buffers_.Clear();
    const uint32 base[BytecodeBankCount] = {};
    if (auto pushed = PushFrame(function, base); !pushed) {
        Unwind();
        return Result<VmCallResult>::Failure(pushed.Error());
    }

    for (usize index = 0; index < scalars.Size(); ++index) {
        scalars_[index] = scalars[index];
    }

    for (usize index = 0; index < refs.Size(); ++index) {
        refs_[index] = refs[index];
    }

    for (usize index = 0; index < buffers.Size(); ++index) {
        Retain(buffers[index]);
        buffers_[index] = buffers[index];
    }

    // 실수 연산은 기대 환경(nearest-even, FTZ·DAZ 끔)에서 한다 — 드라이버·미들웨어가 스레드 환경을
    // 바꿔 놓아도 서버·클라이언트·재시뮬레이션이 같은 결과다(SL 12.5절).
    struct RunJob {
        VmExecution* self;
        const VmLimits* limits;
        Result<VmCallResult> result;
    };
    RunJob job{this, &limits, Result<VmCallResult>::Failure(ErrorCode::InvalidArgument)};
    platform::RunWithDeterministicFloatingPoint(
        [](void* context) noexcept {
            RunJob& run = *static_cast<RunJob*>(context);
            run.result = run.self->Run(*run.limits);
        },
        &job);
    return static_cast<Result<VmCallResult>&&>(job.result);
}

Result<void> VmExecution::PushFrame(uint32 function,
                                    const uint32 (&base)[BytecodeBankCount]) noexcept {
    // 부르는 함수의 frame은 창 base에서 시작한다. 매개변수(창의 앞)는 부르는 쪽이 두었고, 나머지
    // 레지스터는 0·null로 채운다(V2 — 초기화 전 읽기도 정해진 값을 본다). Buf slot에 남은 값(앞선
    // 호출의 창)은 놓는다.
    const BytecodeFunction& entry = module_->functions[function];
    Array<uint64>* stacks[] = {&scalars_, &refs_};
    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
        const usize begin = usize(base[bank]) + entry.parameters[bank];
        const usize end = usize(base[bank]) + entry.frame[bank];
        const bool buffers = bank == static_cast<uint32>(BytecodeBank::Buf);
        const usize size = buffers ? buffers_.Size() : stacks[bank]->Size();
        const usize capacity = buffers ? buffers_.Capacity() : stacks[bank]->Capacity();
        if (end > capacity) {
            const usize grown =
                capacity * VmStackGrowthFactor > end ? capacity * VmStackGrowthFactor : end;
            auto reserved = buffers ? buffers_.TryReserve(grown) : stacks[bank]->TryReserve(grown);
            if (!reserved) {
                return reserved;
            }
        }

        if (end > size) {
            // 늘린 자리는 0·null로 값 초기화된다.
            auto resized = buffers ? buffers_.TryResize(end) : stacks[bank]->TryResize(end);
            if (!resized) {
                return resized;
            }
        }

        if (buffers) {
            ReleaseRange(begin, end);
            continue;
        }

        for (usize at = begin; at < end; ++at) {
            (*stacks[bank])[at] = 0;
        }
    }

    Frame frame;
    frame.function = function;
    frame.pc = entry.firstWord;
    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
        frame.base[bank] = base[bank];
    }

    return frames_.TryPushBack(frame);
}

Result<VmCallResult> VmExecution::MakeFault(VmFaultKind kind, uint32 word, uint64 steps) noexcept {
    // fault는 실행 문맥 전체를 끝낸다(V3). 자리는 지금 frame의 함수 안 주소다.
    VmCallResult result;
    result.outcome = VmOutcome::Faulted;
    result.steps = steps;
    result.fault.kind = kind;
    const uint32 function = frames_[frames_.Size() - 1].function;
    result.fault.function = function;
    result.fault.address = word - module_->functions[function].firstWord;
    if (word < module_->locations.Size()) {
        result.fault.location = module_->locations[word];
    }

    Unwind();
    return Result<VmCallResult>::Success(result);
}

void VmExecution::Unwind() noexcept {
    ReleaseRange(0, buffers_.Size());
    frames_.Clear();
}

void VmExecution::ReleaseRange(usize begin, usize end) noexcept {
    for (usize at = begin; at < end; ++at) {
        VmBuffer* buffer = buffers_[at];
        buffers_[at] = nullptr;
        Release(buffer);
    }
}

void VmExecution::ReleaseReturned() noexcept {
    for (VmBuffer* buffer : returnedBuffers_) {
        Release(buffer);
    }

    returnedBuffers_.Clear();
}

Result<VmBuffer*> VmExecution::NewString(StringView text) noexcept {
    const Allocation allocation = Allocate(VmBufferKind::String, text.ByteCount(), 1, 0);
    if (allocation.error != ErrorCode::None) {
        return Result<VmBuffer*>::Failure(allocation.error);
    }

    if (allocation.fault != VmFaultKind::None) {
        return Result<VmBuffer*>::Failure(ErrorCode::CapacityExceeded);
    }

    platform::MemoryCopy(allocation.buffer->Bytes(), text.Data(), text.ByteCount());
    return Result<VmBuffer*>::Success(allocation.buffer);
}

VmExecution::Allocation VmExecution::Allocate(VmBufferKind kind, uint64 elements,
                                              uint64 elementBytes, uint64 budget) noexcept {
    // 단일 버퍼 상한(원소 수·byte)과 실행 문맥의 메모리 예산을 먼저 본다(SparksVm 2.2절).
    Allocation result;
    if (elements > MaxBufferElements || elementBytes > MaxBufferBytes ||
        elements * elementBytes > MaxBufferBytes) {
        result.fault = VmFaultKind::BudgetExceeded;
        return result;
    }

    const uint64 payload = (elements * elementBytes + VmPayloadAlignment - 1) / VmPayloadAlignment *
                           VmPayloadAlignment;
    const uint64 bytes = sizeof(VmBuffer) + payload;
    if (budget != 0 && liveBytes_ + bytes > budget) {
        result.fault = VmFaultKind::BudgetExceeded;
        return result;
    }

    auto memory = allocator_->Allocate(static_cast<usize>(bytes), alignof(VmBuffer));
    if (!memory) {
        result.error = memory.Error();
        return result;
    }

    VmBuffer* buffer = ::new (memory.Value()) VmBuffer();
    buffer->references = 1;
    buffer->kind = kind;
    buffer->count = static_cast<uint32>(elements);
    buffer->capacity = static_cast<uint32>(elements);
    buffer->bytes = bytes;
    ++liveBuffers_;
    liveBytes_ += bytes;
    result.buffer = buffer;
    return result;
}

void VmExecution::Release(VmBuffer* buffer) noexcept {
    // 0이 된 버퍼를 `next`로 이은 대기 목록에 두고 하나씩 해제한다(할당 없이 — 해제는 실패하지
    // 않는다, 원소 버퍼의 깊이에 재귀하지 않는다).
    if (buffer == nullptr || --buffer->references != 0) {
        return;
    }

    buffer->next = nullptr;
    VmBuffer* pending = buffer;
    while (pending != nullptr) {
        VmBuffer* current = pending;
        pending = current->next;
        // 원소 버퍼는 LC5b2(컨테이너)·LC5b3(struct·Result)에서 더한다 — 문자열은 원소가 없다.
        --liveBuffers_;
        liveBytes_ -= current->bytes;
        current->~VmBuffer();
        static_cast<void>(allocator_->Deallocate(current));
    }
}

} // namespace rocks::sparks
