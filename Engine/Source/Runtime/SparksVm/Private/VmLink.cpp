#include <Platform/Memory.h>
#include <SparksVm/Opcode.h>
#include <SparksVm/Vm.h>
#include <SparksVm/VmHost.h>

#include "VmLibrary.h"

// 링크다(Docs/Design/SparksVm.ko.md 6.1절, LC5d1): 참조 표를 처음부터 끝까지 한 번 묶고(언어
// 함수는 VM 구현 표, native·필드·문맥·리터럴은 host), 필드 명령의 배치를 묶음과 대조한다. 하나라도
// 못 묶으면 모듈 전체를 거부한다.

namespace rocks::sparks {

namespace {

constexpr usize VmLinkReasonBytes = 256;
constexpr uint8 VmLinkRefBank = static_cast<uint8>(BytecodeBank::Ref);
constexpr uint8 VmLinkBufBank = static_cast<uint8>(BytecodeBank::Buf);

// host가 묶는 참조 종류인지다.
bool HostBound(BytecodeRefKind kind) noexcept {
    return kind == BytecodeRefKind::NativeFunction || kind == BytecodeRefKind::Field ||
           kind == BytecodeRefKind::Context || kind == BytecodeRefKind::Literal;
}

// 표 번호의 글자다(없으면 빈 글자).
StringView TextOf(const BytecodeModule& module, uint32 index) noexcept {
    return index == NoBytecodeIndex ? StringView() : module.String(index);
}

// 거부 사유를 적고 InvalidArgument(사유를 적지 못하면 그 오류)를 돌려준다.
Result<void> Reject(StringBuilderBase& reason, const BytecodeModule& module, uint32 ref,
                    StringView why) noexcept {
    const BytecodeRef& entry = module.refs[ref];
    reason.AppendFormat("ref {} ({} {}): {}", ref, BytecodeRefKindName(entry.kind),
                        module.String(entry.name), why);
    if (reason.HasFailed()) {
        return Result<void>::Failure(reason.Status().Error());
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// 필드 명령의 bank다(필드 명령이 아니면 false).
bool FieldBank(Opcode op, uint8& bank) noexcept {
    switch (op) {
    case Opcode::GetFS:
    case Opcode::SetFS:
        bank = static_cast<uint8>(BytecodeBank::Scalar);
        return true;
    case Opcode::GetFR:
    case Opcode::SetFR:
        bank = VmLinkRefBank;
        return true;
    case Opcode::GetFB:
    case Opcode::SetFB:
        bank = VmLinkBufBank;
        return true;
    default:
        return false;
    }
}

} // namespace

bool VmHostCall::PutBuffer(uint32 at, VmBuffer* value) noexcept {
    if (at >= buffers_.Size()) {
        execution_->Release(value);
        return false;
    }

    VmBuffer* previous = buffers_[at];
    buffers_[at] = value;
    execution_->Release(previous);
    return true;
}

Result<void> LinkModule(const BytecodeModule& module, VmHost* host, VmLinkedModule& out,
                        StringBuilderBase& reason) noexcept {
    out.module_ = &module;
    out.host_ = host;
    out.hosts_.Clear();
    out.languageIndex_.Clear();
    out.languages_.Clear();
    const usize count = module.refs.Size();
    if (auto resized = out.hosts_.TryResize(count); !resized) {
        return resized;
    }

    if (auto resized = out.languageIndex_.TryResize(count); !resized) {
        return resized;
    }

    for (usize ref = 0; ref < count; ++ref) {
        out.hosts_[ref] = VmHostBinding();
        out.languageIndex_[ref] = NoBytecodeIndex;
    }

    StringBuilder<VmLinkReasonBytes> why(out.hosts_.GetAllocator());
    for (uint32 ref = 0; ref < count; ++ref) {
        const BytecodeRef& entry = module.refs[ref];
        if (entry.kind == BytecodeRefKind::LanguageFunction) {
            const StringView name = module.String(entry.name);
            if (detail::VmLibrary::Deferred(name)) {
                continue; // 대기·host 언어 함수(LC6 — 부르면 ContractViolation, V5)
            }

            detail::VmLanguageBinding binding;
            if (entry.arguments == NoBytecodeIndex ||
                !detail::VmParseSignature(module.String(entry.arguments), binding.signature)) {
                return Reject(reason, module, ref, "invalid language function descriptor");
            }

            // 이름과 서술자 모양으로 구현을 고른다(리뷰 A1 — 모양이 다르면 거부).
            binding.function = detail::VmLibrary::Find(name, module.String(entry.arguments));
            if (binding.function == nullptr) {
                return Reject(reason, module, ref, "unknown language function or descriptor shape");
            }

            const usize at = out.languages_.Size();
            if (auto grown = out.languages_.TryResize(at + detail::VmLanguageWords); !grown) {
                return grown;
            }

            platform::MemoryCopy(out.languages_.Data() + at, &binding, sizeof(binding));
            out.languageIndex_[ref] = static_cast<uint32>(at / detail::VmLanguageWords);
            continue;
        }

        if (!HostBound(entry.kind)) {
            continue; // LC5d2·LC6이 묶는다(그 명령은 실행 중 ContractViolation — V5)
        }

        if (host == nullptr) {
            return Reject(reason, module, ref, "no host to bind this reference");
        }

        VmBindRequest request;
        request.kind = entry.kind;
        request.ref = ref;
        request.name = module.String(entry.name);
        if (entry.kind == BytecodeRefKind::Literal) {
            request.literal = TextOf(module, entry.detail);
            request.namedArguments = TextOf(module, entry.arguments);
        } else {
            request.descriptor = TextOf(module, entry.arguments);
            if (entry.kind == BytecodeRefKind::NativeFunction && entry.detail != NoBytecodeIndex &&
                entry.detail < count) {
                request.typeArgument = module.String(module.refs[entry.detail].name);
            }
        }

        why.Clear();
        auto bound = host->Bind(request, why);
        if (!bound) {
            if (bound.Error() == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            return Reject(reason, module, ref,
                          why.IsEmpty() ? StringView("rejected by the host") : why.View());
        }

        const VmHostBinding& binding = bound.Value();
        const bool single = binding.bank == VmLinkRefBank || binding.bank == VmLinkBufBank;
        if (entry.kind == BytecodeRefKind::Context &&
            (binding.bank != VmLinkRefBank || binding.slots != 1)) {
            return Reject(reason, module, ref, "a context value must be one Ref slot");
        }

        if ((entry.kind == BytecodeRefKind::Field || entry.kind == BytecodeRefKind::Literal) &&
            (binding.bank > VmLinkBufBank || binding.slots == 0 ||
             (single && binding.slots != 1))) {
            return Reject(reason, module, ref, "the host returned an invalid value layout");
        }

        out.hosts_[ref] = binding;
    }

    // 필드 명령(GETF·SETF)의 bank·slot 수(C)가 묶은 필드 배치와 같아야 한다.
    for (const BytecodeFunction& function : module.functions) {
        for (uint32 address = 0; address < function.wordCount; ++address) {
            const uint32 word = module.words[function.firstWord + address];
            const Opcode op = DecodeOpcode(word);
            const bool extra = InfoOf(op).extra;
            uint8 bank = 0;
            if (FieldBank(op, bank)) {
                const uint32 ref = DecodeAx(module.words[function.firstWord + address + 1]);
                const VmHostBinding& field = out.hosts_[ref];
                if (field.bank != bank || field.slots != DecodeC(word)) {
                    reason.AppendFormat("function {} word {}: the field instruction does not "
                                        "match the bound field '{}'",
                                        module.String(function.name), address,
                                        module.String(module.refs[ref].name));
                    if (reason.HasFailed()) {
                        return Result<void>::Failure(reason.Status().Error());
                    }

                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }
            }

            // CALLL의 창(이 frame의 할당 수부터 frame 끝)이 서술자의 피연산자·결과를 담아야 한다
            // (리뷰 A2 — 창 밖 레지스터를 읽거나 쓰지 않는다).
            if (op == Opcode::CallL) {
                const uint32 index = out.languageIndex_[DecodeBx(word)];
                if (index != NoBytecodeIndex) {
                    const auto& binding = *reinterpret_cast<const detail::VmLanguageBinding*>(
                        out.languages_.Data() + usize(index) * detail::VmLanguageWords);
                    const detail::VmSignature& signature = binding.signature;
                    uint32 need[BytecodeBankCount] = {};
                    for (uint32 operand = 0; operand < signature.operandCount; ++operand) {
                        const detail::VmTypeNode& node =
                            signature.nodes[signature.operands[operand]];
                        need[node.bank] += node.slots;
                    }

                    const detail::VmTypeNode& result = signature.nodes[signature.result];
                    const detail::VmTypeNode& receiver = signature.nodes[signature.operands[0]];
                    const bool mutates =
                        signature.operandCount > 0 &&
                        detail::VmLanguageMutates(module.String(module.refs[DecodeBx(word)].name));
                    const uint32 resultEnd =
                        result.slots +
                        (mutates && receiver.bank == result.bank ? receiver.slots : 0);
                    need[result.bank] =
                        need[result.bank] > resultEnd ? need[result.bank] : resultEnd;
                    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                        if (need[bank] > uint32(function.frame[bank]) - function.registers[bank]) {
                            reason.AppendFormat("function {} word {}: the language call does not "
                                                "fit the call window",
                                                module.String(function.name), address);
                            if (reason.HasFailed()) {
                                return Result<void>::Failure(reason.Status().Error());
                            }

                            return Result<void>::Failure(ErrorCode::InvalidArgument);
                        }
                    }
                }
            }

            address += extra ? 1 : 0;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks
