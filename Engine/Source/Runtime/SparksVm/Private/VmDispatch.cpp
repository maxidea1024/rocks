#include <Core/Math/Matrix4.h>
#include <Core/Math/Quaternion.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Memory.h>
#include <SparksVm/Vm.h>

#include "VmArithmetic.h"
#include "VmFormat.h"

// Sparks VM dispatch 루프다(Docs/Design/SparksVm.ko.md 2–5절, LC5a·LC5b1). switch 기반 단일
// 루프이며 verifier를 통과한 모듈만 실행하므로 피연산자 범위를 다시 검사하지 않는다. Buf slot은
// null이 아니면 참조 하나를 가진다: 복사는 참조 +1, 덮어쓰기는 이전 값 −1이다.

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

// RET·RETERR가 한 번에 돌려주는 slot 수의 상한(함수 표 returnSlots — 5비트 표지)이다.
constexpr uint32 VmMaxReturnSlots = 32;

// TOSTR 표기의 inline 크기다(mat4 16성분도 inline에 든다).
constexpr usize VmFormatInlineBytes = 1024;

// 문자열 비교(byte 순, null은 null끼리만 같고 어떤 문자열보다 작다)다.
int VmCompareStrings(const VmBuffer* left, const VmBuffer* right) noexcept {
    if (left == nullptr || right == nullptr) {
        return (left == nullptr ? 0 : 1) - (right == nullptr ? 0 : 1);
    }

    return StringView::CompareBytes(VmStringOf(left), VmStringOf(right));
}

} // namespace

Result<VmCallResult> VmExecution::Run(const VmLimits& limits) noexcept {
    const BytecodeModule& module = *module_;
    const uint32* const words = module.words.Data();
    uint64 steps = 0;
    uint32 pc = frames_[frames_.Size() - 1].pc;
    uint64* s = nullptr;
    uint64* r = nullptr;
    VmBuffer** b = nullptr;
    // frame이 바뀌거나 stack이 다시 할당되면 bank 포인터를 다시 잡는다.
    auto reload = [&]() noexcept {
        const Frame& frame = frames_[frames_.Size() - 1];
        s = scalars_.Data() + frame.base[0];
        r = refs_.Data() + frame.base[1];
        b = buffers_.Data() + frame.base[2];
    };
    // Buf 레지스터 at에 value를 둔다(이전 값을 놓는다 — value의 참조는 넘겨받는다).
    auto put = [&](uint32 at, VmBuffer* value) noexcept {
        VmBuffer* previous = b[at];
        b[at] = value;
        Release(previous);
    };
    // 지금 frame을 내리고 bank의 0번부터 count개를 돌려준다. 마지막 frame이면 호출 결과다.
    auto leave = [&](uint32 bank, uint32 count, VmCallResult& out) noexcept -> Result<bool> {
        const Frame& frame = frames_[frames_.Size() - 1];
        const usize bufferBase = frame.base[2];
        const usize frameEnd = bufferBase + module.functions[frame.function].frame[2];
        const bool buffers = bank == static_cast<uint32>(BytecodeBank::Buf);
        if (frames_.Size() == 1) {
            if (buffers) {
                if (auto resized = returnedBuffers_.TryResize(count); !resized) {
                    return Result<bool>::Failure(resized.Error());
                }

                for (uint32 index = 0; index < count; ++index) {
                    returnedBuffers_[index] = b[index]; // 참조를 넘긴다
                    b[index] = nullptr;
                }
            } else {
                const uint64* values = bank == 0 ? s : r;
                if (auto resized = returned_.TryResize(count); !resized) {
                    return Result<bool>::Failure(resized.Error());
                }

                for (uint32 index = 0; index < count; ++index) {
                    returned_[index] = values[index];
                }
            }

            Unwind();
            out.outcome = VmOutcome::Returned;
            out.steps = steps;
            return Result<bool>::Success(true);
        }

        // 반환 값(Buf면 0..count) 밖의 이 frame Buf 레지스터를 놓는다. 반환 값은 부르는 쪽 창에
        // 남아 그 slot이 참조를 가진다.
        ReleaseRange(bufferBase + (buffers ? count : 0), frameEnd);
        static_cast<void>(frames_.TryResize(frames_.Size() - 1)); // 축소는 실패하지 않는다
        pc = frames_[frames_.Size() - 1].pc;
        reload();
        return Result<bool>::Success(false);
    };
    // 할당 실패(OS)는 실행을 끝내고 Result 실패로 돌려준다.
    auto failed = [&](ErrorCode error) noexcept -> Result<VmCallResult> {
        Unwind();
        return Result<VmCallResult>::Failure(error);
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
            Retain(b[bb]);
            put(a, b[bb]);
            break;
        case Opcode::LoadI:
            s[a] = static_cast<uint64>(static_cast<int64>(DecodeSBx(word)));
            break;
        case Opcode::LoadK:
            s[a] = module.scalars[DecodeBx(word)];
            break;
        case Opcode::LoadStr: {
            const StringView text = module.String(DecodeBx(word));
            const Allocation allocation =
                Allocate(VmBufferKind::String, text.ByteCount(), 1, limits.memoryBytes);
            if (allocation.error != ErrorCode::None) {
                return failed(allocation.error);
            }

            if (allocation.fault != VmFaultKind::None) {
                return MakeFault(allocation.fault, at, steps);
            }

            platform::MemoryCopy(allocation.buffer->Bytes(), text.Data(), text.ByteCount());
            put(a, allocation.buffer);
            break;
        }
        case Opcode::LoadNullR:
            r[a] = 0;
            break;
        case Opcode::LoadNullB:
            put(a, nullptr);
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
        case Opcode::EqStr:
            s[a] = VmCompareStrings(b[bb], b[c]) == 0 ? 1 : 0;
            break;
        case Opcode::NeStr:
            s[a] = VmCompareStrings(b[bb], b[c]) != 0 ? 1 : 0;
            break;
        case Opcode::LtStr:
            s[a] = VmCompareStrings(b[bb], b[c]) < 0 ? 1 : 0;
            break;
        case Opcode::LeStr:
            s[a] = VmCompareStrings(b[bb], b[c]) <= 0 ? 1 : 0;
            break;
        case Opcode::EqB:
        case Opcode::NeB: {
            // 값 비교: 문자열(LC5b1). 컨테이너·struct의 값 비교는 LC5b2·LC5b3(V5).
            const VmBuffer* left = b[bb];
            const VmBuffer* right = b[c];
            if ((left != nullptr && left->kind != VmBufferKind::String) ||
                (right != nullptr && right->kind != VmBufferKind::String)) {
                return MakeFault(VmFaultKind::ContractViolation, at, steps);
            }

            const bool equal = VmCompareStrings(left, right) == 0;
            s[a] = (op == Opcode::EqB) == equal ? 1 : 0;
            break;
        }
        case Opcode::EqR:
            // handle 원시 비교다. 파괴된 객체를 null로 읽는 비교(SL 3.3절 — generation 검사)는
            // LC5d의 host IsAlive를 거친다(V5 자리).
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
                equal = floats ? detail::VmFloatSlotsEqual(s[bb + slot], s[c + slot])
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
        case Opcode::ToStr: {
            // Scalar 묶음 B..B+C의 보간 표기(SparksVm 2.2절)를 새 문자열로 A에 둔다.
            const uint32 extra = DecodeAx(words[pc++]);
            StringBuilder<VmFormatInlineBytes> text(*allocator_);
            fault = detail::VmFormatScalars(text, extra >> 16, s + bb, c);
            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            if (text.HasFailed()) {
                return failed(text.Status().Error());
            }

            const Allocation allocation =
                Allocate(VmBufferKind::String, text.ByteCount(), 1, limits.memoryBytes);
            if (allocation.error != ErrorCode::None) {
                return failed(allocation.error);
            }

            if (allocation.fault != VmFaultKind::None) {
                return MakeFault(allocation.fault, at, steps);
            }

            platform::MemoryCopy(allocation.buffer->Bytes(), text.View().Data(), text.ByteCount());
            put(a, allocation.buffer);
            break;
        }
        case Opcode::Concat: {
            // 창의 Buf B..B+C(문자열 조각)를 이어 새 문자열로 A에 둔다.
            uint64 total = 0;
            for (uint32 piece = 0; piece < c; ++piece) {
                total += VmStringOf(b[bb + piece]).ByteCount();
            }

            const Allocation allocation =
                Allocate(VmBufferKind::String, total, 1, limits.memoryBytes);
            if (allocation.error != ErrorCode::None) {
                return failed(allocation.error);
            }

            if (allocation.fault != VmFaultKind::None) {
                return MakeFault(allocation.fault, at, steps);
            }

            char* cursor = allocation.buffer->Bytes();
            for (uint32 piece = 0; piece < c; ++piece) {
                const StringView text = VmStringOf(b[bb + piece]);
                platform::MemoryCopy(cursor, text.Data(), text.ByteCount());
                cursor += text.ByteCount();
            }

            put(a, allocation.buffer);
            break;
        }
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
            // bank C의 A부터 B개를 0번부터로 옮긴다(앞에서부터 — 대상이 원본보다 앞이다). Buf는
            // 참조를 옮긴다(복사 뒤 원본 자리를 놓는다).
            if (c == static_cast<uint32>(BytecodeBank::Buf)) {
                for (uint32 index = 0; index < bb; ++index) {
                    Retain(b[a + index]);
                    put(index, b[a + index]);
                }
            } else {
                uint64* values = c == 0 ? s : r;
                for (uint32 index = 0; index < bb; ++index) {
                    values[index] = values[a + index];
                }
            }

            VmCallResult result;
            auto left = leave(c, bb, result);
            if (!left) {
                return failed(left.Error());
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
                return MakeFault(VmFaultKind::ContractViolation, at, steps); // Buf Result — LC5b3
            }

            const uint64 code = s[a];
            s[0] = code;
            for (uint32 slot = 1; slot < current.returnSlots && slot < VmMaxReturnSlots; ++slot) {
                s[slot] = 0;
            }

            VmCallResult result;
            auto left = leave(0, current.returnSlots, result);
            if (!left) {
                return failed(left.Error());
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
            Unwind();
            return Result<VmCallResult>::Success(result);
        }
        case Opcode::IsSomeR:
            s[a] = r[bb] != 0 ? 1 : 0;
            break;
        case Opcode::IsSomeB:
            s[a] = b[bb] != nullptr ? 1 : 0;
            break;
        case Opcode::TryS: {
            // 성공(코드 0)이면 값 C − 1 slot을 A로, 실패면 EXTRA 거리(EXTRA 다음 단어 기준)로 간다.
            const int32 distance = DecodeSAx(words[pc++]);
            if (s[bb] == 0) {
                // 겹치는 구간도 원본 값으로 옮긴다(memmove 의미 — 대상이 원본 뒤면 뒤에서부터).
                if (a > bb + 1) {
                    for (uint32 slot = c - 1; slot > 0; --slot) {
                        s[a + slot - 1] = s[bb + slot];
                    }
                } else {
                    for (uint32 slot = 0; slot + 1 < c; ++slot) {
                        s[a + slot] = s[bb + 1 + slot];
                    }
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
                return failed(pushed.Error());
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
            // 아직 구현하지 않은 명령(V5 — LC5b2·LC5b3·LC5c·LC5d·LC6).
            return MakeFault(VmFaultKind::ContractViolation, at, steps);
        }
    }
}

} // namespace rocks::sparks
