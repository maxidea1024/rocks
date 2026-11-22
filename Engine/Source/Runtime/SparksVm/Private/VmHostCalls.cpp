#include <SparksVm/Opcode.h>
#include <SparksVm/Vm.h>
#include <SparksVm/VmHost.h>

// host 명령(GETF·SETF·GETCTX·CALLN·NEWLIT)의 실행이다(Docs/Design/SparksVm.ko.md 6.2절, LC5d1).
// 창을 VmHostCall로 만들어 링크한 host를 부른다.

namespace rocks::sparks {

namespace {

constexpr uint32 VmNewLitRefMask = 0xFFFF;
constexpr uint32 VmNewLitBankShift = 22;
constexpr uint32 VmNewLitBankMask = 0x3;
constexpr uint32 VmNewLitCountShift = 16;
constexpr uint32 VmNewLitCountMask = 0x3F;

} // namespace

bool VmExecution::Alive(uint64 handle) noexcept {
    // 0이 아니고 host가 살아 있다고 답한 handle만 있음이다(SL 4.1절 — 파괴된 객체는 null).
    VmHost* host = link_->host_;
    return handle != 0 && host != nullptr && host->IsAlive(handle);
}

VmFaultKind VmExecution::ResolveVirtualCall(uint32 ref, const Banks& banks, VmVirtualTarget& target,
                                            ErrorCode& error) noexcept {
    VmHost* host = link_->host_;
    const BytecodeFunction& current = module_->functions[frames_[frames_.Size() - 1].function];
    const uint32 refs = uint32(current.frame[1]) - current.registers[1];
    if (host == nullptr || refs == 0) {
        return VmFaultKind::ContractViolation;
    }

    VmHostCall call;
    call.execution_ = this;
    call.binding_ = link_->hosts_[ref].id;
    call.scalars_ = Span<uint64>(banks.s + current.registers[0],
                                 uint32(current.frame[0]) - current.registers[0]);
    call.refs_ = Span<uint64>(banks.r + current.registers[1], refs);
    call.buffers_ = Span<VmBuffer*>(banks.b + current.registers[2],
                                    uint32(current.frame[2]) - current.registers[2]);
    const uint64 receiver = call.refs_[0];
    if (!Alive(receiver)) {
        return VmFaultKind::ContractViolation; // 꺼낸 참조에만 낸다(6.5절)
    }

    VmFaultKind fault = host->ResolveVirtual(call, receiver, target);
    if (fault == VmFaultKind::None && call.error_ == ErrorCode::None && target.native) {
        call.binding_ = target.binding;
        fault = host->CallNative(call);
    } else if (fault == VmFaultKind::None && call.error_ == ErrorCode::None) {
        // host가 준 Sparks 대상은 같은 host로 링크했고, 함수 모양이 가상 서술자와 같아야 한다
        // (LC5d2 리뷰 B2·S2).
        const VmLinkedModule::Shape& shape = link_->shapes_[ref];
        bool runnable = target.module != nullptr && target.module->Linked() &&
                        target.module->ImportsLinked() && target.module->host_ == link_->host_ &&
                        target.function < target.module->Module().functions.Size() && shape.valid;
        if (runnable) {
            const BytecodeFunction& called = target.module->Module().functions[target.function];
            runnable = called.returnSlots == shape.returnSlots &&
                       (shape.returnSlots == 0 || called.returnBank == shape.returnBank);
            for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
                runnable = runnable && called.parameters[bank] == shape.parameters[bank];
            }
        }

        fault = runnable ? VmFaultKind::None : VmFaultKind::ContractViolation;
    }

    error = call.error_;
    return fault;
}

VmFaultKind VmExecution::CallHost(Opcode op, uint32 word, uint32 extra, const Banks& banks,
                                  ErrorCode& error) noexcept {
    VmHost* host = link_->host_;
    if (host == nullptr) {
        return VmFaultKind::ContractViolation; // 링크가 막는다 — 안전장치
    }

    const uint32 a = DecodeA(word);
    const uint32 bb = DecodeB(word);
    const uint32 c = DecodeC(word);
    VmHostCall call;
    call.execution_ = this;
    // bank의 first부터 count개를 창으로 둔다.
    auto place = [&](uint32 bank, uint32 first, uint32 count) noexcept {
        if (bank == static_cast<uint32>(BytecodeBank::Scalar)) {
            call.scalars_ = Span<uint64>(banks.s + first, count);
        } else if (bank == static_cast<uint32>(BytecodeBank::Ref)) {
            call.refs_ = Span<uint64>(banks.r + first, count);
        } else {
            call.buffers_ = Span<VmBuffer*>(banks.b + first, count);
        }
    };
    const BytecodeFunction& current = module_->functions[frames_[frames_.Size() - 1].function];
    // 이 frame의 호출 창(할당 수부터 frame 끝까지)을 bank마다 둔다.
    auto window = [&]() noexcept {
        for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
            place(bank, current.registers[bank], current.frame[bank] - current.registers[bank]);
        }
    };
    VmFaultKind fault = VmFaultKind::None;
    switch (op) {
    case Opcode::GetFS:
    case Opcode::GetFR:
    case Opcode::GetFB:
    case Opcode::SetFS:
    case Opcode::SetFR:
    case Opcode::SetFB: {
        const bool get = op == Opcode::GetFS || op == Opcode::GetFR || op == Opcode::GetFB;
        const uint32 bank = op == Opcode::GetFS || op == Opcode::SetFS   ? 0
                            : op == Opcode::GetFR || op == Opcode::SetFR ? 1
                                                                         : 2;
        const uint64 object = get ? banks.r[bb] : banks.r[a];
        if (!Alive(object)) {
            return VmFaultKind::ContractViolation; // 꺼낸 참조에만 낸다(6.2절)
        }

        call.binding_ = link_->hosts_[DecodeAx(extra)].id;
        place(bank, get ? a : bb, c);
        fault = get ? host->GetField(call, object) : host->SetField(call, object);
        break;
    }
    case Opcode::GetCtx:
        call.binding_ = link_->hosts_[DecodeBx(word)].id;
        place(static_cast<uint32>(BytecodeBank::Ref), a, 1);
        fault = host->GetContext(call);
        break;
    case Opcode::CallN:
        call.binding_ = link_->hosts_[DecodeBx(word)].id;
        window();
        fault = host->CallNative(call);
        break;
    case Opcode::NewLit: {
        // 결과는 결과 bank 창 0번에 받아 레지스터 A로 옮긴다(Buf는 참조를 옮긴다).
        const uint32 payload = DecodeAx(extra);
        const VmHostBinding& binding = link_->hosts_[payload & VmNewLitRefMask];
        const uint32 bank = (payload >> VmNewLitBankShift) & VmNewLitBankMask;
        const uint32 arguments = (payload >> VmNewLitCountShift) & VmNewLitCountMask;
        if (bank != binding.bank) {
            return VmFaultKind::ContractViolation;
        }

        call.binding_ = binding.id;
        if (arguments == 0) {
            // 이름 지정 인자가 없으면 창이 없다: 결과 레지스터 A를 바로 준다.
            place(bank, a, binding.slots);
            fault = host->MakeLiteral(call);
            break;
        }

        if (uint32(current.frame[bank]) - current.registers[bank] < binding.slots) {
            return VmFaultKind::ContractViolation;
        }

        window();
        fault = host->MakeLiteral(call);
        if (fault != VmFaultKind::None || call.error_ != ErrorCode::None) {
            break;
        }

        const uint32 base = current.registers[bank];
        for (uint32 slot = 0; slot < binding.slots; ++slot) {
            if (bank == static_cast<uint32>(BytecodeBank::Scalar)) {
                banks.s[a + slot] = banks.s[base + slot];
            } else if (bank == static_cast<uint32>(BytecodeBank::Ref)) {
                banks.r[a + slot] = banks.r[base + slot];
            } else if (a + slot != base + slot) {
                VmBuffer* previous = banks.b[a + slot];
                banks.b[a + slot] = banks.b[base + slot];
                banks.b[base + slot] = nullptr;
                Release(previous);
            }
        }

        break;
    }
    default:
        return VmFaultKind::ContractViolation;
    }

    error = call.error_;
    return fault;
}

} // namespace rocks::sparks
