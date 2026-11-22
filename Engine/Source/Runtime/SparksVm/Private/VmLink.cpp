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

// host가 묶는 참조인지다(TYPE이 없는 Type·StructField — 엔진 타입·Sparks 클래스 — 포함, 6.5절).
bool HostBound(const BytecodeRef& ref) noexcept {
    const BytecodeRefKind kind = ref.kind;
    return kind == BytecodeRefKind::NativeFunction || kind == BytecodeRefKind::Field ||
           kind == BytecodeRefKind::Context || kind == BytecodeRefKind::Literal ||
           kind == BytecodeRefKind::VirtualFunction ||
           ((kind == BytecodeRefKind::Type || kind == BytecodeRefKind::StructField) &&
            ref.detail == NoBytecodeIndex);
}

// 표 번호의 글자다(없으면 빈 글자).
StringView TextOf(const BytecodeModule& module, uint32 index) noexcept {
    return index == NoBytecodeIndex ? StringView() : module.String(index);
}

// 거부 사유를 적고 InvalidArgument(사유를 적지 못하면 그 오류)를 돌려준다.
Result<void> Reject(StringBuilderBase& reason, const BytecodeModule& module, uint32 ref,
                    StringView why, StringView detail = StringView()) noexcept {
    const BytecodeRef& entry = module.refs[ref];
    reason.AppendFormat("ref {} ({} {}): {}", ref, BytecodeRefKindName(entry.kind),
                        module.String(entry.name), why);
    // 모양 불일치는 서술자를 함께 보인다(진단).
    if (!detail.IsEmpty()) {
        constexpr usize MaxShown = 64; // 신뢰할 수 없는 서술자는 잘라 보인다
        const StringView shown(detail.Data(),
                               detail.ByteCount() < MaxShown ? detail.ByteCount() : MaxShown);
        reason.AppendFormat(" [{}{}]", shown, detail.ByteCount() > MaxShown ? "..." : "");
    }

    if (reason.HasFailed()) {
        return Result<void>::Failure(reason.Status().Error());
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

constexpr uint32 VmLinkLiteralRefMask = 0xFFFF;
constexpr uint32 VmLinkLiteralCountShift = 16;
constexpr uint32 VmLinkLiteralCountMask = 0x3F;
constexpr uint32 VmLinkLiteralBankShift = 22;
constexpr uint32 VmLinkLiteralBankMask = 0x3;

// 함수 function의 주소 address 명령을 거부한다.
Result<void> RejectWord(StringBuilderBase& reason, const BytecodeModule& module,
                        const BytecodeFunction& function, uint32 address, StringView why) noexcept {
    reason.AppendFormat("function {} word {}: {}", module.String(function.name), address, why);
    if (reason.HasFailed()) {
        return Result<void>::Failure(reason.Status().Error());
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// 서술자의 피연산자(bank별 합)와 결과(값을 바꾸면 수신자 뒤)가 함수의 호출 창에 들어가는지다.
bool FitsWindow(const detail::VmSignature& signature, const BytecodeFunction& function,
                bool mutates) noexcept {
    uint32 need[BytecodeBankCount] = {};
    for (uint32 operand = 0; operand < signature.operandCount; ++operand) {
        const detail::VmTypeNode& node = signature.nodes[signature.operands[operand]];
        need[node.bank] += node.slots;
    }

    const detail::VmTypeNode& result = signature.nodes[signature.result];
    const bool sameBank =
        signature.operandCount > 0 && signature.nodes[signature.operands[0]].bank == result.bank;
    const uint32 resultEnd =
        result.slots + (mutates && sameBank ? signature.nodes[signature.operands[0]].slots : 0);
    need[result.bank] = need[result.bank] > resultEnd ? need[result.bank] : resultEnd;
    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
        if (need[bank] > uint32(function.frame[bank]) - function.registers[bank]) {
            return false;
        }
    }

    return true;
}

// struct 필드 명령의 bank다(아니면 false).
bool StructFieldBank(Opcode op, uint8& bank) noexcept {
    switch (op) {
    case Opcode::GetSFS:
    case Opcode::SetSFS:
        bank = static_cast<uint8>(BytecodeBank::Scalar);
        return true;
    case Opcode::GetSFR:
    case Opcode::SetSFR:
        bank = VmLinkRefBank;
        return true;
    case Opcode::GetSFB:
    case Opcode::SetSFB:
        bank = VmLinkBufBank;
        return true;
    default:
        return false;
    }
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
    out.imports_.Clear();
    out.shapes_.Clear();
    out.localFields_.Clear();
    out.keys_.Clear();
    out.importsLinked_ = false;
    out.linked_ = false;
    ++out.generation_;
    const usize count = module.refs.Size();
    if (auto resized = out.hosts_.TryResize(count); !resized) {
        return resized;
    }

    if (auto resized = out.languageIndex_.TryResize(count); !resized) {
        return resized;
    }

    if (auto resized = out.imports_.TryResize(count); !resized) {
        return resized;
    }

    if (auto resized = out.shapes_.TryResize(count); !resized) {
        return resized;
    }

    if (auto resized = out.localFields_.TryResize(count); !resized) {
        return resized;
    }

    for (usize ref = 0; ref < count; ++ref) {
        out.localFields_[ref] = VmLinkedModule::LocalField();
    }

    if (auto resized = out.keys_.TryResize(count); !resized) {
        return resized;
    }

    // Event·Field 참조의 정규 key: `<경로>:Name`이면 그 경로, 이 모듈 클래스의 선언이면 이 모듈
    // 경로, 그 밖(엔진 선언)은 빈 경로다.
    const StringView ownPath = TextOf(module, module.path);
    for (uint32 ref = 0; ref < count; ++ref) {
        const BytecodeRef& entry = module.refs[ref];
        out.keys_[ref] = VmEventKey();
        if (entry.kind != BytecodeRefKind::Event && entry.kind != BytecodeRefKind::Field) {
            continue;
        }

        const StringView name = module.String(entry.name);
        usize colon = name.ByteCount();
        for (usize at = 0; at < name.ByteCount() && colon == name.ByteCount(); ++at) {
            colon = name.Data()[at] == ':' ? at : colon;
        }

        if (colon < name.ByteCount()) {
            out.keys_[ref].path = StringView(name.Data(), colon);
            out.keys_[ref].name = StringView(name.Data() + colon + 1, name.ByteCount() - colon - 1);
            continue;
        }

        usize dot = name.ByteCount();
        for (usize at = 0; at < name.ByteCount() && dot == name.ByteCount(); ++at) {
            dot = name.Data()[at] == '.' ? at : dot;
        }

        const StringView owner(name.Data(), dot < name.ByteCount() ? dot : 0);
        bool local = false;
        for (const BytecodeClass& candidate : module.classes) {
            local = local || module.String(candidate.name) == owner;
        }

        out.keys_[ref].path = local ? ownPath : StringView();
        out.keys_[ref].name = name;
    }

    bool imports = false;

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
                return Reject(reason, module, ref, "unknown language function or descriptor shape",
                              module.String(entry.arguments));
            }

            const usize at = out.languages_.Size();
            if (auto grown = out.languages_.TryResize(at + detail::VmLanguageWords); !grown) {
                return grown;
            }

            platform::MemoryCopy(out.languages_.Data() + at, &binding, sizeof(binding));
            out.languageIndex_[ref] = static_cast<uint32>(at / detail::VmLanguageWords);
            continue;
        }

        // import·가상 함수·이벤트의 서술자 모양(6.5절 — 대상 함수와 대조, 이벤트는 EMIT 창과
        // handler 인자 — SparksRuntime 2.3절).
        if (entry.kind == BytecodeRefKind::ImportedFunction ||
            entry.kind == BytecodeRefKind::VirtualFunction ||
            (entry.kind == BytecodeRefKind::Event && entry.arguments != NoBytecodeIndex)) {
            detail::VmSignature signature;
            if (entry.arguments == NoBytecodeIndex ||
                !detail::VmParseSignature(module.String(entry.arguments), signature)) {
                return Reject(reason, module, ref, "invalid call descriptor");
            }

            VmLinkedModule::Shape& shape = out.shapes_[ref];
            for (uint32 operand = 0; operand < signature.operandCount; ++operand) {
                const detail::VmTypeNode& node = signature.nodes[signature.operands[operand]];
                shape.parameters[node.bank] =
                    static_cast<uint16>(shape.parameters[node.bank] + node.slots);
            }

            const detail::VmTypeNode& result = signature.nodes[signature.result];
            shape.returnBank = result.slots == 0 ? 0 : result.bank;
            shape.returnSlots = result.slots;
            shape.valid = true;
        }

        if (entry.kind == BytecodeRefKind::ImportedFunction) {
            imports = true; // LinkImports가 푼다(6.5절)
            continue;
        }

        // 같은 모듈 Sparks 클래스 필드(`Class.Field`)는 로컬로 묶는다(SparksRuntime 2.2절).
        if (entry.kind == BytecodeRefKind::Field) {
            const StringView name = module.String(entry.name);
            usize dot = name.ByteCount();
            for (usize at = 0; at < name.ByteCount(); ++at) {
                dot = name.Data()[at] == '.' ? at : dot;
            }

            const StringView owner(name.Data(), dot < name.ByteCount() ? dot : 0);
            const StringView member =
                dot < name.ByteCount()
                    ? StringView(name.Data() + dot + 1, name.ByteCount() - dot - 1)
                    : StringView();
            bool local = false;
            for (uint32 index = 0; index < module.classes.Size() && !local; ++index) {
                const BytecodeClass& entryClass = module.classes[index];
                if (module.String(entryClass.name) != owner) {
                    continue;
                }

                for (uint32 at = entryClass.firstField;
                     at < entryClass.firstField + entryClass.fieldCount; ++at) {
                    const BytecodeTypeField& field = module.classFields[at];
                    if (module.String(field.name) == member) {
                        VmLinkedModule::LocalField& bound = out.localFields_[ref];
                        bound.classIndex = index;
                        bound.bank = field.bank;
                        bound.slots = field.slots;
                        bound.offset = field.offset;
                        bound.held = (field.flags & BytecodeFieldHeld) != 0;
                        out.hosts_[ref].bank = field.bank;
                        out.hosts_[ref].slots = field.slots;
                        local = true;
                    }
                }
            }

            if (local) {
                continue;
            }
        }

        if (!HostBound(entry)) {
            continue; // LC5d2·LC6이 묶는다(그 명령은 실행 중 ContractViolation — V5)
        }

        // TYPE 없는 Type·StructField는 host가 없으면 묶지 않는다(컨테이너 원소 타입 같은 참조도
        // Type이다 — 그 값을 쓰는 NEWS·CAST·TOSTR·GETSF는 실행 중 ContractViolation).
        const bool optional =
            entry.kind == BytecodeRefKind::Type || entry.kind == BytecodeRefKind::StructField;
        if (host == nullptr) {
            if (optional) {
                continue;
            }

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

        if ((entry.kind == BytecodeRefKind::Field || entry.kind == BytecodeRefKind::Literal ||
             entry.kind == BytecodeRefKind::StructField) &&
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
            // 엔진 struct 필드(TYPE 없는 StructField)의 GETSF·SETSF도 host 배치와 대조한다(6.5절).
            const bool hostStructField =
                StructFieldBank(op, bank) &&
                module.refs[DecodeAx(module.words[function.firstWord + address + 1])].detail ==
                    NoBytecodeIndex;
            if (hostStructField || FieldBank(op, bank)) {
                const uint32 ref = DecodeAx(module.words[function.firstWord + address + 1]);
                const VmHostBinding& field = out.hosts_[ref];
                const bool unbound = hostStructField && host == nullptr; // 실행 중 계약 위반
                if (!unbound && (field.bank != bank || field.slots != DecodeC(word))) {
                    StringBuilder<VmLinkReasonBytes> why(out.hosts_.GetAllocator());
                    why.AppendFormat("the field instruction does not match the bound field '{}'",
                                     module.String(module.refs[ref].name));
                    if (why.HasFailed()) {
                        return Result<void>::Failure(why.Status().Error());
                    }

                    return RejectWord(reason, module, function, address, why.View());
                }
            }

            // NEWLIT: 결과 bank가 묶음과 같고, 결과 묶음이 frame 안이며, 이름 지정 인자가 있으면
            // 창이 결과를 담아야 한다(LC5d1 리뷰 B3).
            if (op == Opcode::NewLit) {
                const uint32 payload = DecodeAx(module.words[function.firstWord + address + 1]);
                const VmHostBinding& literal = out.hosts_[payload & VmLinkLiteralRefMask];
                const uint32 bank = (payload >> VmLinkLiteralBankShift) & VmLinkLiteralBankMask;
                const uint32 count = (payload >> VmLinkLiteralCountShift) & VmLinkLiteralCountMask;
                const uint32 window = uint32(function.frame[bank < BytecodeBankCount ? bank : 0]) -
                                      function.registers[bank < BytecodeBankCount ? bank : 0];
                if (bank != literal.bank || DecodeA(word) + literal.slots > function.frame[bank] ||
                    (count > 0 && window < literal.slots)) {
                    return RejectWord(reason, module, function, address,
                                      "the literal result does not fit the bound literal");
                }
            }

            // CALLN·CALLV·EMIT: 서술자의 피연산자·결과가 창에 들어가야 한다(LC5d1 리뷰 B4 — host가
            // 창 밖을 쓰지 않게 VM이 먼저 막는다).
            if (op == Opcode::CallN || op == Opcode::CallV || op == Opcode::Emit) {
                const BytecodeRef& entry = module.refs[DecodeBx(word)];
                detail::VmSignature signature;
                if (entry.arguments == NoBytecodeIndex ||
                    !detail::VmParseSignature(module.String(entry.arguments), signature) ||
                    !FitsWindow(signature, function, false)) {
                    return RejectWord(reason, module, function, address,
                                      "the native call does not fit the call window");
                }
            }

            // CALLL의 창(이 frame의 할당 수부터 frame 끝)이 서술자의 피연산자·결과를 담아야 한다
            // (리뷰 A2 — 창 밖 레지스터를 읽거나 쓰지 않는다).
            if (op == Opcode::CallL) {
                const uint32 index = out.languageIndex_[DecodeBx(word)];
                if (index != NoBytecodeIndex) {
                    const auto& binding = *reinterpret_cast<const detail::VmLanguageBinding*>(
                        out.languages_.Data() + usize(index) * detail::VmLanguageWords);
                    const bool mutates =
                        detail::VmLanguageMutates(module.String(module.refs[DecodeBx(word)].name));
                    if (!FitsWindow(binding.signature, function, mutates)) {
                        return RejectWord(reason, module, function, address,
                                          "the language call does not fit the call window");
                    }
                }
            }

            address += extra ? 1 : 0;
        }
    }

    out.importsLinked_ = !imports;
    out.linked_ = true;
    return Result<void>::Success();
}

VmFaultKind VmHost::ResolveVirtual(VmHostCall&, uint64, VmVirtualTarget&) noexcept {
    return VmFaultKind::ContractViolation;
}

bool VmHost::IsA(uint64, uint64) noexcept {
    return false;
}

VmFaultKind VmHost::InitializeStruct(VmHostCall&) noexcept {
    return VmFaultKind::None;
}

bool VmHost::AppendName(uint64, StringBuilderBase&) noexcept {
    return false;
}

bool VmHost::AppendEnumName(uint64, uint64, StringBuilderBase&) noexcept {
    return false;
}

Result<void> LinkImports(Span<VmLinkedModule* const> modules, StringBuilderBase& reason) noexcept {
    return LinkImports(modules, Span<const VmLinkedModule* const>(), reason);
}

Result<void> LinkImports(Span<VmLinkedModule* const> modules,
                         Span<const VmLinkedModule* const> targets,
                         StringBuilderBase& reason) noexcept {
    if (modules.IsEmpty()) {
        return Result<void>::Success();
    }

    // 모두 링크에 성공했고 같은 host여야 한다(LC5d2 리뷰 B1·B2 — host마다 handle 공간이 다르다).
    for (const VmLinkedModule* target : targets) {
        if (target == nullptr || !target->linked_ || target->host_ != modules[0]->host_) {
            reason.Append(target == nullptr || !target->linked_
                              ? "a module in the import set is not linked"
                              : "the modules are linked to different hosts");
            if (reason.HasFailed()) {
                return Result<void>::Failure(reason.Status().Error());
            }

            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    for (VmLinkedModule* linked : modules) {
        if (linked == nullptr || !linked->linked_ || linked->host_ != modules[0]->host_) {
            reason.Append(linked == nullptr || !linked->linked_
                              ? "a module in the import set is not linked"
                              : "the modules are linked to different hosts");
            if (reason.HasFailed()) {
                return Result<void>::Failure(reason.Status().Error());
            }

            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    for (VmLinkedModule* linked : modules) {
        const BytecodeModule& module = *linked->module_;
        for (uint32 ref = 0; ref < module.refs.Size(); ++ref) {
            const BytecodeRef& entry = module.refs[ref];
            if (entry.kind != BytecodeRefKind::ImportedFunction) {
                continue;
            }

            // 이름은 `<import 경로>:Owner.Name`이다(첫 `:`가 경로의 끝).
            const StringView name = module.String(entry.name);
            usize colon = name.ByteCount();
            for (usize index = 0; index < name.ByteCount() && colon == name.ByteCount(); ++index) {
                colon = name.Data()[index] == ':' ? index : colon;
            }

            if (colon == name.ByteCount()) {
                return Reject(reason, module, ref, "an import name needs '<path>:'");
            }

            const StringView path(name.Data(), colon);
            const StringView member(name.Data() + colon + 1, name.ByteCount() - colon - 1);
            // 같은 경로가 둘 다에 있으면 modules가 우선이다(reload 묶음의 새 revision).
            const VmLinkedModule* target = nullptr;
            for (const VmLinkedModule* candidate : targets) {
                const BytecodeModule& other = *candidate->module_;
                if (other.path != NoBytecodeIndex && other.String(other.path) == path) {
                    target = candidate;
                }
            }

            for (const VmLinkedModule* candidate : modules) {
                const BytecodeModule& other = *candidate->module_;
                if (other.path != NoBytecodeIndex && other.String(other.path) == path) {
                    target = candidate;
                }
            }

            if (target == nullptr) {
                return Reject(reason, module, ref, "no linked module has this path");
            }

            const BytecodeModule& other = *target->module_;
            uint32 function = NoBytecodeIndex;
            for (uint32 index = 0; index < other.functions.Size(); ++index) {
                function = other.String(other.functions[index].name) == member ? index : function;
            }

            if (function == NoBytecodeIndex) {
                return Reject(reason, module, ref, "the module has no such function");
            }

            // 모양 대조: 서술자의 bank별 피연산자 slot 합 = 매개변수 수, 결과 = 반환 bank·slot 수.
            detail::VmSignature signature;
            if (entry.arguments == NoBytecodeIndex ||
                !detail::VmParseSignature(module.String(entry.arguments), signature)) {
                return Reject(reason, module, ref, "invalid import descriptor");
            }

            uint32 parameters[BytecodeBankCount] = {};
            for (uint32 operand = 0; operand < signature.operandCount; ++operand) {
                const detail::VmTypeNode& node = signature.nodes[signature.operands[operand]];
                parameters[node.bank] += node.slots;
            }

            const BytecodeFunction& called = other.functions[function];
            const detail::VmTypeNode& result = signature.nodes[signature.result];
            bool same = result.slots == called.returnSlots &&
                        (result.slots == 0 || result.bank == called.returnBank);
            for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                same = same && parameters[bank] == called.parameters[bank];
            }

            if (!same) {
                return Reject(reason, module, ref, "the descriptor does not match the function");
            }

            linked->imports_[ref].module = target;
            linked->imports_[ref].function = function;
            linked->imports_[ref].generation = target->generation_;
        }

        // 이 import를 부르는 CALL마다 부르는 쪽 창이 대상 매개변수를 담는지 본다(LC5d2 리뷰 S3 —
        // 로드에서 거부).
        for (const BytecodeFunction& function : module.functions) {
            for (uint32 address = 0; address < function.wordCount; ++address) {
                const uint32 word = module.words[function.firstWord + address];
                const Opcode op = DecodeOpcode(word);
                if ((op == Opcode::Call || op == Opcode::CallW) &&
                    module.refs[DecodeBx(word)].kind == BytecodeRefKind::ImportedFunction) {
                    const VmLinkedModule::Import& import = linked->imports_[DecodeBx(word)];
                    const BytecodeFunction& called =
                        import.module->module_->functions[import.function];
                    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                        if (called.parameters[bank] >
                            uint32(function.frame[bank]) - function.registers[bank]) {
                            return RejectWord(reason, module, function, address,
                                              "the imported call does not fit the call window");
                        }
                    }
                }

                address += InfoOf(op).extra ? 1 : 0;
            }
        }
    }

    for (VmLinkedModule* linked : modules) {
        linked->importsLinked_ = true;
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks
