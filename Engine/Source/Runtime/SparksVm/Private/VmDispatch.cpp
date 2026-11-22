#include <Core/Math/Matrix4.h>
#include <Core/Math/Quaternion.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Memory.h>
#include <SparksVm/Vm.h>
#include <SparksVm/VmSession.h>

#include "VmArithmetic.h"
#include "VmFormat.h"

// Sparks VM dispatch 루프다(Docs/Design/SparksVm.ko.md 2–5절, LC5a·LC5b1). switch 기반 단일
// 루프이며 verifier를 통과한 모듈만 실행하므로 피연산자 범위를 다시 검사하지 않는다. Buf slot은
// null이 아니면 참조 하나를 가진다: 복사는 참조 +1, 덮어쓰기는 이전 값 −1이다.

namespace rocks::sparks {

namespace {

using detail::VmBits;
using detail::VmDouble;

// `FAULT Ax`의 번호다(SparksBytecode 3.1a절): 2는 hook 안 `try` 실패, 3은 경로 쓰기의 없는 원소.
// 1(도달하지 않는 경로)과 그 밖은 Unreachable로 본다.
constexpr uint32 VmFaultHookTryCode = 2;
constexpr uint32 VmFaultMissingElementCode = 3;

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
    uint64 steps = 0;
    uint32 pc = frames_[frames_.Size() - 1].pc;
    uint64* s = nullptr;
    uint64* r = nullptr;
    VmBuffer** b = nullptr;
    // frame이 바뀌거나 stack이 다시 할당되면 bank 포인터를 다시 잡는다.
    // frame의 모듈도 바뀐다(다른 모듈 함수 — 6.5절): 실행 문맥의 지금 모듈을 frame의 것으로 둔다.
    auto reload = [&]() noexcept {
        const Frame& frame = frames_[frames_.Size() - 1];
        link_ = frame.link;
        module_ = &frame.link->Module();
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
        const usize frameEnd = bufferBase + module_->functions[frame.function].frame[2];
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
        const BytecodeModule& module = *module_;
        const uint32* const words = module.words.Data();
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
        case Opcode::NeR: {
            // 죽은 handle은 null로 읽는다(SL 3.3절 — 6.2절 handle 생존).
            const uint64 left = Alive(r[bb]) ? r[bb] : 0;
            const uint64 right = Alive(r[c]) ? r[c] : 0;
            s[a] = (op == Opcode::EqR) == (left == right) ? 1 : 0;
            break;
        }
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
            if ((extra >> 16) == static_cast<uint32>(ScalarKind::Enum)) {
                // enum은 TYPE 구역의 case 이름이다(SparksVm 2.2절).
                const uint32 typeIndex = module.refs[extra & MaxBx].detail;
                if (typeIndex == NoBytecodeIndex) {
                    // 엔진 enum: host 이름 표(6.5절).
                    VmHost* host = link_->host_;
                    if (host == nullptr ||
                        !host->AppendEnumName(link_->hosts_[extra & MaxBx].id, s[bb], text)) {
                        if (text.HasFailed()) {
                            return failed(text.Status().Error()); // 할당 실패는 fault가 아니다
                        }

                        return MakeFault(VmFaultKind::ContractViolation, at, steps);
                    }
                } else {
                    const BytecodeType& type = module.types[typeIndex];
                    if (s[bb] >= type.fieldCount) {
                        return MakeFault(VmFaultKind::Unreachable, at, steps);
                    }

                    text.Append(module.String(
                        module.typeFields[type.firstField + static_cast<uint32>(s[bb])].name));
                }
            } else if ((extra >> 16) == static_cast<uint32>(ScalarKind::Name)) {
                // Name·Tag: host 이름 표(6.5절).
                VmHost* host = link_->host_;
                if (host == nullptr || !host->AppendName(s[bb], text)) {
                    if (text.HasFailed()) {
                        return failed(text.Status().Error());
                    }

                    return MakeFault(VmFaultKind::ContractViolation, at, steps);
                }
            } else {
                fault = detail::VmFormatScalars(text, extra >> 16, s + bb, c);
            }

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
            const uint64 code = s[a];
            if (current.returnBank == static_cast<uint8>(BytecodeBank::Buf)) {
                // 이 함수는 Buf Result를 돌려준다: 같은 오류 코드의 Result를 만든다.
                const Allocation allocation = MakeErrorResult(code, limits.memoryBytes);
                if (allocation.error != ErrorCode::None) {
                    return failed(allocation.error);
                }

                if (allocation.fault != VmFaultKind::None) {
                    return MakeFault(allocation.fault, at, steps);
                }

                put(0, allocation.buffer);
                VmCallResult result;
                auto left = leave(2, 1, result);
                if (!left) {
                    return failed(left.Error());
                }

                if (left.Value()) {
                    return Result<VmCallResult>::Success(result);
                }

                break;
            }

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
        case Opcode::RetErrB: {
            // Buf Result A의 오류를 이 함수의 반환 모양(Scalar Result 묶음 또는 Buf Result)으로.
            if (b[a] == nullptr || b[a]->kind != VmBufferKind::Result) {
                return MakeFault(VmFaultKind::ContractViolation, at, steps);
            }

            const uint64 code = reinterpret_cast<const uint64*>(b[a]->Bytes())[0];
            const BytecodeFunction& current =
                module.functions[frames_[frames_.Size() - 1].function];
            VmCallResult result;
            Result<bool> left = Result<bool>::Success(false);
            if (current.returnBank == static_cast<uint8>(BytecodeBank::Buf)) {
                const Allocation allocation = MakeErrorResult(code, limits.memoryBytes);
                if (allocation.error != ErrorCode::None) {
                    return failed(allocation.error);
                }

                if (allocation.fault != VmFaultKind::None) {
                    return MakeFault(allocation.fault, at, steps);
                }

                put(0, allocation.buffer);
                left = leave(2, 1, result);
            } else {
                s[0] = code;
                for (uint32 slot = 1; slot < current.returnSlots && slot < VmMaxReturnSlots;
                     ++slot) {
                    s[slot] = 0;
                }

                left = leave(0, current.returnSlots, result);
            }

            if (!left) {
                return failed(left.Error());
            }

            if (left.Value()) {
                return Result<VmCallResult>::Success(result);
            }

            break;
        }
        case Opcode::EndExecS:
        case Opcode::EndExecB: {
            // Result가 아닌 함수·handler의 `try` 실패: 실행 전체를 끝낸다(fault가 아니다).
            if (op == Opcode::EndExecB && (b[a] == nullptr || b[a]->kind != VmBufferKind::Result)) {
                return MakeFault(VmFaultKind::ContractViolation, at, steps);
            }

            VmCallResult result;
            result.outcome = VmOutcome::Ended;
            result.errorCode =
                op == Opcode::EndExecS ? s[a] : reinterpret_cast<const uint64*>(b[a]->Bytes())[0];
            result.steps = steps;
            Unwind();
            return Result<VmCallResult>::Success(result);
        }
        case Opcode::TryB: {
            // Buf Result B: 성공이면 값(C = 값 bank << 5 | slot 수)을 A로, 실패면 EXTRA 거리로
            // 간다.
            const int32 distance = DecodeSAx(words[pc++]);
            VmBuffer* value = b[bb];
            if (value == nullptr || value->kind != VmBufferKind::Result) {
                return MakeFault(VmFaultKind::ContractViolation, at, steps);
            }

            const uint64* payload = reinterpret_cast<const uint64*>(value->Bytes());
            if (payload[0] != 0) {
                pc = static_cast<uint32>(static_cast<int32>(pc) + distance);
                break;
            }

            const uint32 bank = c >> 5;
            const uint32 slots = c & 0x1F;
            // 값 배치가 명령의 C와 같아야 한다(LC5d1 리뷰 B2 — count 밖·다른 bank를 읽지 않는다).
            if (value->valueBank != bank || value->valueSlots != slots ||
                value->count != 1u + slots) {
                return MakeFault(VmFaultKind::ContractViolation, at, steps);
            }

            for (uint32 slot = 0; slot < slots; ++slot) {
                if (bank == static_cast<uint32>(BytecodeBank::Buf)) {
                    VmBuffer* child =
                        reinterpret_cast<VmBuffer*>(static_cast<usize>(payload[1 + slot]));
                    Retain(child);
                    put(a + slot, child);
                } else if (bank == static_cast<uint32>(BytecodeBank::Scalar)) {
                    s[a + slot] = payload[1 + slot];
                } else {
                    r[a + slot] = payload[1 + slot];
                }
            }

            break;
        }
        case Opcode::NewS:
        case Opcode::GetSFS:
        case Opcode::GetSFR:
        case Opcode::GetSFB:
        case Opcode::SetSFS:
        case Opcode::SetSFR:
        case Opcode::SetSFB:
        case Opcode::Ok:
        case Opcode::Err: {
            // struct·Buf Result 명령(VmStructs.cpp — SparksVm 2.2절).
            const uint32 extra = InfoOf(op).extra ? words[pc++] : 0;
            ErrorCode error = ErrorCode::None;
            Banks banks;
            banks.s = s;
            banks.r = r;
            banks.b = b;
            fault = Structured(op, word, extra, banks, limits.memoryBytes, error);
            if (error != ErrorCode::None) {
                return failed(error);
            }

            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            break;
        }
        case Opcode::IsSomeR:
            s[a] = Alive(r[bb]) ? 1 : 0;
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
            const uint32 refIndex = DecodeBx(word);
            const BytecodeRef& ref = module.refs[refIndex];
            // 같은 모듈 함수, 또는 LinkImports가 푼 다른 모듈 함수(6.5절).
            const VmLinkedModule* target = link_;
            uint32 callee = ref.detail;
            if (ref.kind == BytecodeRefKind::ImportedFunction) {
                // 대상이 그 뒤 다시 링크됐으면(세대가 다르면) 계약 위반이다(LC5d2 리뷰 B1).
                const VmLinkedModule::Import& import = link_->imports_[refIndex];
                if (import.module == nullptr || !import.module->Linked() ||
                    !import.module->ImportsLinked() ||
                    import.generation != import.module->generation_ ||
                    import.function >= import.module->Module().functions.Size()) {
                    return MakeFault(VmFaultKind::ContractViolation, at, steps);
                }

                target = import.module;
                callee = import.function;
            } else if (ref.kind != BytecodeRefKind::Function) {
                return MakeFault(VmFaultKind::ContractViolation, at, steps);
            }

            if (frames_.Size() >= limits.callDepth) {
                return MakeFault(VmFaultKind::BudgetExceeded, at, steps);
            }

            Frame& caller = frames_[frames_.Size() - 1];
            const BytecodeFunction& current = module.functions[caller.function];
            const BytecodeFunction& called = target->Module().functions[callee];
            uint32 base[BytecodeBankCount] = {};
            for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                base[bank] = caller.base[bank] + current.registers[bank];
                // 다른 모듈 함수의 매개변수가 창에 들어가야 한다(같은 모듈은 verifier가 본다).
                if (called.parameters[bank] >
                    uint32(current.frame[bank]) - current.registers[bank]) {
                    return MakeFault(VmFaultKind::ContractViolation, at, steps);
                }
            }

            caller.pc = pc;
            if (auto pushed = PushFrame(*target, callee, base); !pushed) {
                return failed(pushed.Error());
            }

            pc = frames_[frames_.Size() - 1].pc;
            reload();
            break;
        }
        case Opcode::CallV: {
            // 가상 호출(6.5절): host가 대상을 정한다 — native는 그 자리에서, Sparks 함수는 frame을
            // 만든다(같은 창 규칙).
            Banks banks;
            banks.s = s;
            banks.r = r;
            banks.b = b;
            VmVirtualTarget target;
            ErrorCode error = ErrorCode::None;
            fault = ResolveVirtualCall(DecodeBx(word), banks, target, error);
            if (error != ErrorCode::None) {
                return failed(error);
            }

            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            if (target.native) {
                break;
            }

            if (frames_.Size() >= limits.callDepth) {
                return MakeFault(VmFaultKind::BudgetExceeded, at, steps);
            }

            Frame& caller = frames_[frames_.Size() - 1];
            const BytecodeFunction& current = module.functions[caller.function];
            const BytecodeFunction& called = target.module->Module().functions[target.function];
            uint32 base[BytecodeBankCount] = {};
            for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                base[bank] = caller.base[bank] + current.registers[bank];
                if (called.parameters[bank] >
                    uint32(current.frame[bank]) - current.registers[bank]) {
                    return MakeFault(VmFaultKind::ContractViolation, at, steps);
                }
            }

            caller.pc = pc;
            if (auto pushed = PushFrame(*target.module, target.function, base); !pushed) {
                return failed(pushed.Error());
            }

            pc = frames_[frames_.Size() - 1].pc;
            reload();
            break;
        }
        case Opcode::Await: {
            // 대기 handle B로 정지한다(SparksRuntime 2.4절): frame·레지스터를 session에 옮기고
            // Suspended를 돌려준다. 재개는 EXTRA 다음(재개 블록으로의 JMP)부터다.
            const uint32 resultShape = DecodeAx(words[pc++]);
            const uint64 wait = r[bb];
            if (session_ == nullptr || (wait & VmWaitTag) == 0) {
                return MakeFault(VmFaultKind::ContractViolation, at, steps);
            }

            frames_[frames_.Size() - 1].pc = pc;
            ErrorCode error = ErrorCode::None;
            uint32 suspension = NoBytecodeIndex;
            fault =
                session_->Suspend(wait, a, resultShape >> 8, resultShape & 0xFF, suspension, error);
            if (error != ErrorCode::None) {
                return failed(error);
            }

            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            VmCallResult suspended;
            suspended.outcome = VmOutcome::Suspended;
            suspended.steps = steps;
            suspended.wait = wait;
            suspended.suspension = suspension;
            return Result<VmCallResult>::Success(suspended);
        }
        case Opcode::Emit: {
            Banks banks;
            banks.s = s;
            banks.r = r;
            banks.b = b;
            ErrorCode error = ErrorCode::None;
            fault = EmitEvent(word, banks, error);
            if (error != ErrorCode::None) {
                return failed(error);
            }

            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            break;
        }
        case Opcode::Cast: {
            // `as?`(6.5절): 살아 있고 host가 그 타입이라고 답하면 B, 아니면 null.
            const uint32 type = DecodeAx(words[pc++]);
            VmHost* host = link_->host_;
            r[a] = host != nullptr && Alive(r[bb]) && host->IsA(r[bb], link_->hosts_[type].id)
                       ? r[bb]
                       : 0;
            break;
        }
        case Opcode::CallL: {
            // 언어 함수(SparksVm 6a절): 창은 이 frame의 bank별 할당 수 위다.
            const BytecodeFunction& current =
                module.functions[frames_[frames_.Size() - 1].function];
            Banks window;
            window.s = s + current.registers[0];
            window.r = r + current.registers[1];
            window.b = b + current.registers[2];
            ErrorCode error = ErrorCode::None;
            fault = CallLanguage(DecodeBx(word), window, limits.memoryBytes, error);
            if (error != ErrorCode::None) {
                return failed(error);
            }

            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            break;
        }
        case Opcode::Fault:
            return MakeFault(DecodeAx(word) == VmFaultHookTryCode ? VmFaultKind::HookTryFailed
                             : DecodeAx(word) == VmFaultMissingElementCode
                                 ? VmFaultKind::IndexOutOfRange
                                 : VmFaultKind::Unreachable,
                             at, steps);
        case Opcode::NewA:
        case Opcode::NewSet:
        case Opcode::NewM:
        case Opcode::Append:
        case Opcode::Put:
        case Opcode::GetE:
        case Opcode::SetE:
        case Opcode::Len:
        case Opcode::GetKey:
        case Opcode::GetVal: {
            // 컨테이너 명령(VmContainers.cpp — SparksVm 2.2절).
            const uint32 marker = InfoOf(op).extra ? DecodeAx(words[pc++]) : 0;
            ErrorCode error = ErrorCode::None;
            Banks banks;
            banks.s = s;
            banks.r = r;
            banks.b = b;
            fault = Container(op, word, marker, banks, limits.memoryBytes, error);
            if (error != ErrorCode::None) {
                return failed(error);
            }

            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            break;
        }
        case Opcode::GetFS:
        case Opcode::GetFR:
        case Opcode::GetFB:
        case Opcode::SetFS:
        case Opcode::SetFR:
        case Opcode::SetFB:
        case Opcode::GetCtx:
        case Opcode::CallN:
        case Opcode::NewLit: {
            // host 명령(VmHostCalls.cpp — SparksVm 6.2절).
            const uint32 extra = InfoOf(op).extra ? words[pc++] : 0;
            ErrorCode error = ErrorCode::None;
            Banks banks;
            banks.s = s;
            banks.r = r;
            banks.b = b;
            fault = CallHost(op, word, extra, banks, error);
            if (error != ErrorCode::None) {
                return failed(error);
            }

            if (fault != VmFaultKind::None) {
                return MakeFault(fault, at, steps);
            }

            break;
        }
        case Opcode::Extra:
        case Opcode::Count:
            // verifier를 통과한 모듈에서는 나오지 않는다.
            return MakeFault(VmFaultKind::Unreachable, at, steps);
        default:
            // 아직 구현하지 않은 명령(V5 — LC5c·LC5d·LC6).
            return MakeFault(VmFaultKind::ContractViolation, at, steps);
        }
    }
}

} // namespace rocks::sparks
