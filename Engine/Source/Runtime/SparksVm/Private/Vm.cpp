#include <SparksVm/Vm.h>

#include <Core/Math/Matrix4.h>
#include <Core/Math/Quaternion.h>

#include "VmArithmetic.h"

// Sparks VM 실행 문맥과 dispatch 루프다(Docs/Design/SparksVm.ko.md 2–5절, LC5a). switch 기반 단일
// 루프이며 verifier를 통과한 모듈만 실행하므로 피연산자 범위를 다시 검사하지 않는다.

namespace rocks::sparks {

namespace {

using detail::VmBits;
using detail::VmDouble;

// `FAULT Ax`의 hook `try` 실패 번호다(SparksBytecode 3.1a절 — 1은 도달하지 않는 경로, 그 밖도
// Unreachable로 본다).
constexpr uint32 VmFaultHookTryCode = 2;

// 합성 곱 묶음의 slot 수다.
constexpr uint32 VmQuatSlots = 4;
constexpr uint32 VmMat4Slots = 16;

// stack을 늘릴 때의 최소 배수다(호출마다 재할당하지 않게).
constexpr usize VmStackGrowthFactor = 2;

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
    : scalars_(allocator), refs_(allocator), buffers_(allocator), frames_(allocator),
      returned_(allocator) {
}

Result<VmCallResult> VmExecution::Call(const BytecodeModule& module, uint32 function,
                                       Span<const uint64> scalars, Span<const uint64> refs,
                                       const VmLimits& limits) noexcept {
    if (function >= module.functions.Size()) {
        return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
    }

    const BytecodeFunction& entry = module.functions[function];
    const uint32 scalarBank = static_cast<uint32>(BytecodeBank::Scalar);
    const uint32 refBank = static_cast<uint32>(BytecodeBank::Ref);
    const uint32 bufBank = static_cast<uint32>(BytecodeBank::Buf);
    if (scalars.Size() != entry.parameters[scalarBank] ||
        refs.Size() != entry.parameters[refBank] || entry.parameters[bufBank] != 0) {
        return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
    }

    module_ = &module;
    frames_.Clear();
    returned_.Clear();
    scalars_.Clear();
    refs_.Clear();
    buffers_.Clear();
    const uint32 base[BytecodeBankCount] = {};
    if (auto pushed = PushFrame(function, base); !pushed) {
        return Result<VmCallResult>::Failure(pushed.Error());
    }

    for (usize index = 0; index < scalars.Size(); ++index) {
        scalars_[index] = scalars[index];
    }

    for (usize index = 0; index < refs.Size(); ++index) {
        refs_[index] = refs[index];
    }

    return Run(limits);
}

Result<void> VmExecution::PushFrame(uint32 function,
                                    const uint32 (&base)[BytecodeBankCount]) noexcept {
    // 부르는 함수의 frame은 창 base에서 시작한다. 매개변수(창의 앞)는 부르는 쪽이 두었고, 나머지
    // 레지스터는 0·null로 채운다(V2 — 초기화 전 읽기도 정해진 값을 본다).
    const BytecodeFunction& entry = module_->functions[function];
    Array<uint64>* stacks[BytecodeBankCount] = {&scalars_, &refs_, &buffers_};
    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
        Array<uint64>& stack = *stacks[bank];
        const usize end = usize(base[bank]) + entry.frame[bank];
        if (end > stack.Capacity()) {
            const usize grown = stack.Capacity() * VmStackGrowthFactor;
            if (auto reserved = stack.TryReserve(grown > end ? grown : end); !reserved) {
                return reserved;
            }
        }

        if (end > stack.Size()) {
            if (auto resized = stack.TryResize(end); !resized) {
                return resized;
            }
        }

        for (usize at = usize(base[bank]) + entry.parameters[bank]; at < end; ++at) {
            stack[at] = 0;
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

    frames_.Clear();
    return Result<VmCallResult>::Success(result);
}

Result<VmCallResult> VmExecution::Run(const VmLimits& limits) noexcept {
    const BytecodeModule& module = *module_;
    const uint32* const words = module.words.Data();
    uint64 steps = 0;
    uint32 pc = frames_[frames_.Size() - 1].pc;
    uint64* s = nullptr;
    uint64* r = nullptr;
    uint64* b = nullptr;
    // frame이 바뀌거나 stack이 다시 할당되면 bank 포인터를 다시 잡는다.
    auto reload = [&]() noexcept {
        const Frame& frame = frames_[frames_.Size() - 1];
        s = scalars_.Data() + frame.base[0];
        r = refs_.Data() + frame.base[1];
        b = buffers_.Data() + frame.base[2];
    };
    // 지금 frame을 내리고 bank의 0번부터 count개를 돌려준다. 마지막 frame이면 호출 결과다.
    auto leave = [&](uint32 bank, uint32 count, VmCallResult& out) noexcept -> Result<bool> {
        const uint64* values = bank == 0 ? s : bank == 1 ? r : b;
        if (frames_.Size() == 1) {
            if (auto resized = returned_.TryResize(count); !resized) {
                return Result<bool>::Failure(resized.Error());
            }

            for (uint32 index = 0; index < count; ++index) {
                returned_[index] = values[index];
            }

            frames_.Clear();
            out.outcome = VmOutcome::Returned;
            out.steps = steps;
            return Result<bool>::Success(true);
        }

        static_cast<void>(frames_.TryResize(frames_.Size() - 1)); // 축소는 실패하지 않는다
        pc = frames_[frames_.Size() - 1].pc;
        reload();
        return Result<bool>::Success(false);
    };
    reload();
    for (;;) {
        const uint32 at = pc;
        if (steps >= limits.instructions) {
            return MakeFault(VmFaultKind::BudgetExceeded, at, steps);
        }

        ++steps;
        const uint32 word = words[pc++];
        const Opcode op = DecodeOpcode(word);
        const uint32 a = DecodeA(word);
        const uint32 bb = DecodeB(word);
        const uint32 c = DecodeC(word);
        VmFaultKind fault = VmFaultKind::None;
        if (op >= Opcode::AddI8 && op <= Opcode::ModU64) {
            fault =
                detail::VmIntegerOperation(uint32(op) - uint32(Opcode::AddI8), s[bb], s[c], s[a]);
            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            continue;
        }

        if (op >= Opcode::AddF32 && op <= Opcode::NegF64) {
            fault =
                detail::VmFloatOperation(uint32(op) - uint32(Opcode::AddF32), s[bb], s[c], s[a]);
            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            continue;
        }

        switch (op) {
        case Opcode::MovS:
            s[a] = s[bb];
            break;
        case Opcode::MovR:
            r[a] = r[bb];
            break;
        case Opcode::MovB:
            b[a] = b[bb];
            break;
        case Opcode::LoadI:
            s[a] = static_cast<uint64>(static_cast<int64>(DecodeSBx(word)));
            break;
        case Opcode::LoadK:
            s[a] = module.scalars[DecodeBx(word)];
            break;
        case Opcode::LoadNullR:
            r[a] = 0;
            break;
        case Opcode::LoadNullB:
            b[a] = 0;
            break;
        case Opcode::NegI8:
        case Opcode::NegI16:
        case Opcode::NegI32:
        case Opcode::NegI64:
            fault = detail::VmIntegerNegation(uint32(op) - uint32(Opcode::NegI8), s[bb], s[a]);
            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            break;
        case Opcode::MulQ: {
            // A는 B·C와 겹치지 않는다(early clobber — SparksBytecode 4.2절).
            const Quaternion<double> left(VmDouble(s[bb]), VmDouble(s[bb + 1]), VmDouble(s[bb + 2]),
                                          VmDouble(s[bb + 3]));
            const Quaternion<double> right(VmDouble(s[c]), VmDouble(s[c + 1]), VmDouble(s[c + 2]),
                                           VmDouble(s[c + 3]));
            const Quaternion<double> product = left * right;
            const double values[VmQuatSlots] = {product.x, product.y, product.z, product.w};
            for (uint32 slot = 0; slot < VmQuatSlots; ++slot) {
                if (!math::IsFinite(values[slot])) {
                    return MakeFault(VmFaultKind::NonFinite, at, steps);
                }
            }

            for (uint32 slot = 0; slot < VmQuatSlots; ++slot) {
                s[a + slot] = VmBits(values[slot]);
            }

            break;
        }
        case Opcode::MulM: {
            Matrix4<double> left;
            Matrix4<double> right;
            for (uint32 slot = 0; slot < VmMat4Slots; ++slot) {
                left.m[slot] = VmDouble(s[bb + slot]);
                right.m[slot] = VmDouble(s[c + slot]);
            }

            const Matrix4<double> product = left * right;
            for (uint32 slot = 0; slot < VmMat4Slots; ++slot) {
                if (!math::IsFinite(product.m[slot])) {
                    return MakeFault(VmFaultKind::NonFinite, at, steps);
                }
            }

            for (uint32 slot = 0; slot < VmMat4Slots; ++slot) {
                s[a + slot] = VmBits(product.m[slot]);
            }

            break;
        }
        case Opcode::Not:
            s[a] = s[bb] == 0 ? 1 : 0;
            break;
        case Opcode::EqI:
            s[a] = s[bb] == s[c] ? 1 : 0;
            break;
        case Opcode::NeI:
            s[a] = s[bb] != s[c] ? 1 : 0;
            break;
        case Opcode::LtI:
            s[a] = static_cast<int64>(s[bb]) < static_cast<int64>(s[c]) ? 1 : 0;
            break;
        case Opcode::LeI:
            s[a] = static_cast<int64>(s[bb]) <= static_cast<int64>(s[c]) ? 1 : 0;
            break;
        case Opcode::LtU:
            s[a] = s[bb] < s[c] ? 1 : 0;
            break;
        case Opcode::LeU:
            s[a] = s[bb] <= s[c] ? 1 : 0;
            break;
        case Opcode::EqF:
            s[a] = VmDouble(s[bb]) == VmDouble(s[c]) ? 1 : 0;
            break;
        case Opcode::NeF:
            s[a] = VmDouble(s[bb]) != VmDouble(s[c]) ? 1 : 0;
            break;
        case Opcode::LtF:
            s[a] = VmDouble(s[bb]) < VmDouble(s[c]) ? 1 : 0;
            break;
        case Opcode::LeF:
            s[a] = VmDouble(s[bb]) <= VmDouble(s[c]) ? 1 : 0;
            break;
        case Opcode::EqR:
            s[a] = r[bb] == r[c] ? 1 : 0;
            break;
        case Opcode::NeR:
            s[a] = r[bb] != r[c] ? 1 : 0;
            break;
        case Opcode::EqN:
        case Opcode::NeN:
        case Opcode::EqNF:
        case Opcode::NeNF: {
            // 묶음 비교: .F는 성분마다 실수 비교(-0.0 == 0.0, NaN != NaN), 그 밖은 비트 비교다.
            const uint32 count = DecodeAx(words[pc++]);
            const bool floats = op == Opcode::EqNF || op == Opcode::NeNF;
            bool equal = true;
            for (uint32 slot = 0; slot < count && equal; ++slot) {
                equal = floats ? VmDouble(s[bb + slot]) == VmDouble(s[c + slot])
                               : s[bb + slot] == s[c + slot];
            }

            s[a] = (op == Opcode::EqN || op == Opcode::EqNF) == equal ? 1 : 0;
            break;
        }
        case Opcode::Conv:
            fault = detail::VmConvert(c, s[bb], s[a]);
            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            break;
        case Opcode::Jmp:
            pc = static_cast<uint32>(static_cast<int32>(pc) + DecodeSBx(word));
            break;
        case Opcode::Brt:
            if (s[a] != 0) {
                pc = static_cast<uint32>(static_cast<int32>(pc) + DecodeSBx(word));
            }

            break;
        case Opcode::Brf:
            if (s[a] == 0) {
                pc = static_cast<uint32>(static_cast<int32>(pc) + DecodeSBx(word));
            }

            break;
        case Opcode::Switch: {
            // case 값은 int64 원문 값이고 레지스터는 그 타입 범위의 확장값이라 비트로 맞춘다.
            const BytecodeSwitchTable& table = module.switches[DecodeBx(word)];
            const uint32 first = module.functions[frames_[frames_.Size() - 1].function].firstWord;
            uint32 target = table.defaultTarget;
            for (uint32 index = table.firstCase; index < table.firstCase + table.caseCount;
                 ++index) {
                if (static_cast<uint64>(module.cases[index].value) == s[a]) {
                    target = module.cases[index].target;
                    break;
                }
            }

            pc = first + target;
            break;
        }
        case Opcode::Ret: {
            // bank C의 A부터 B개를 0번부터로 옮긴다(앞에서부터 — 대상이 원본보다 앞이다).
            uint64* values = c == 0 ? s : c == 1 ? r : b;
            for (uint32 index = 0; index < bb; ++index) {
                values[index] = values[a + index];
            }

            VmCallResult result;
            auto left = leave(c, bb, result);
            if (!left) {
                return Result<VmCallResult>::Failure(left.Error());
            }

            if (left.Value()) {
                return Result<VmCallResult>::Success(result);
            }

            break;
        }
        case Opcode::RetErrS: {
            // Scalar Result A..A+B의 오류 코드를 이 함수의 Result 모양(함수 표)으로 돌려준다.
            const BytecodeFunction& current =
                module.functions[frames_[frames_.Size() - 1].function];
            if (current.returnBank != static_cast<uint8>(BytecodeBank::Scalar)) {
                return MakeFault(VmFaultKind::ContractViolation, at, steps); // Buf Result — LC5b
            }

            const uint64 code = s[a];
            s[0] = code;
            for (uint32 slot = 1; slot < current.returnSlots; ++slot) {
                s[slot] = 0;
            }

            VmCallResult result;
            auto left = leave(0, current.returnSlots, result);
            if (!left) {
                return Result<VmCallResult>::Failure(left.Error());
            }

            if (left.Value()) {
                return Result<VmCallResult>::Success(result);
            }

            break;
        }
        case Opcode::EndExecS: {
            // Result가 아닌 함수·handler의 `try` 실패: 실행 전체를 끝낸다(fault가 아니다).
            VmCallResult result;
            result.outcome = VmOutcome::Ended;
            result.errorCode = s[a];
            result.steps = steps;
            frames_.Clear();
            return Result<VmCallResult>::Success(result);
        }
        case Opcode::IsSomeR:
            s[a] = r[bb] != 0 ? 1 : 0;
            break;
        case Opcode::IsSomeB:
            s[a] = b[bb] != 0 ? 1 : 0;
            break;
        case Opcode::TryS: {
            // 성공(코드 0)이면 값 C − 1 slot을 A로, 실패면 EXTRA 거리(EXTRA 다음 단어 기준)로 간다.
            const int32 distance = DecodeSAx(words[pc++]);
            if (s[bb] == 0) {
                for (uint32 slot = 0; slot + 1 < c; ++slot) {
                    s[a + slot] = s[bb + 1 + slot];
                }
            } else {
                pc = static_cast<uint32>(static_cast<int32>(pc) + distance);
            }

            break;
        }
        case Opcode::Call: {
            const BytecodeRef& ref = module.refs[DecodeBx(word)];
            if (ref.kind != BytecodeRefKind::Function) {
                // 다른 모듈·가상 호출은 LC5d·LC6(V5).
                return MakeFault(VmFaultKind::ContractViolation, at, steps);
            }

            if (frames_.Size() >= limits.callDepth) {
                return MakeFault(VmFaultKind::BudgetExceeded, at, steps);
            }

            Frame& caller = frames_[frames_.Size() - 1];
            const BytecodeFunction& current = module.functions[caller.function];
            uint32 base[BytecodeBankCount] = {};
            for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                base[bank] = caller.base[bank] + current.registers[bank];
            }

            caller.pc = pc;
            if (auto pushed = PushFrame(ref.detail, base); !pushed) {
                return Result<VmCallResult>::Failure(pushed.Error());
            }

            pc = frames_[frames_.Size() - 1].pc;
            reload();
            break;
        }
        case Opcode::Fault:
            return MakeFault(DecodeAx(word) == VmFaultHookTryCode ? VmFaultKind::HookTryFailed
                                                                  : VmFaultKind::Unreachable,
                             at, steps);
        case Opcode::Extra:
        case Opcode::Count:
            // verifier를 통과한 모듈에서는 나오지 않는다.
            return MakeFault(VmFaultKind::Unreachable, at, steps);
        default:
            // 아직 구현하지 않은 명령(V5 — LC5b·LC5c·LC5d·LC6).
            return MakeFault(VmFaultKind::ContractViolation, at, steps);
        }
    }
}

} // namespace rocks::sparks
