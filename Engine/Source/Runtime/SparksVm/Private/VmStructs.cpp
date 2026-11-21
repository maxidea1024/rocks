#include <Platform/Memory.h>
#include <SparksVm/Vm.h>

// struct·Buf Result 명령이다(Docs/Design/SparksVm.ko.md 2.2절, LC5b3). struct 값 버퍼는 TYPE 구역의
// 배치대로 bank 묶음 순(Scalar·Ref·Buf) slot이고 끝의 Buf slot이 원소 버퍼다. Buf Result는 slot 0이
// 오류 코드(0 성공), 그 뒤가 값이다.

namespace rocks::sparks {

namespace {

constexpr uint32 VmStructBufBank = static_cast<uint32>(BytecodeBank::Buf);
constexpr uint32 VmStructScalarBank = static_cast<uint32>(BytecodeBank::Scalar);

uint64* VmStructSlots(VmBuffer* buffer) noexcept {
    return reinterpret_cast<uint64*>(buffer->Bytes());
}

VmBuffer* VmStructChild(uint64 bits) noexcept {
    return reinterpret_cast<VmBuffer*>(static_cast<usize>(bits));
}

uint64 VmStructBits(VmBuffer* buffer) noexcept {
    return static_cast<uint64>(reinterpret_cast<usize>(buffer));
}

// 필드 명령의 값 bank다(GETSF·SETSF의 S·R·B 순).
uint32 VmFieldBank(Opcode op) noexcept {
    return op >= Opcode::SetSFS ? uint32(op) - uint32(Opcode::SetSFS)
                                : uint32(op) - uint32(Opcode::GetSFS);
}

} // namespace

VmExecution::Allocation VmExecution::MakeErrorResult(uint64 code, uint64 budget) noexcept {
    Allocation allocation = AllocateBytes(VmBufferKind::Result, sizeof(uint64), budget);
    if (allocation.buffer != nullptr) {
        allocation.buffer->count = 1;
        VmStructSlots(allocation.buffer)[0] = code;
    }

    return allocation;
}

VmFaultKind VmExecution::Structured(Opcode op, uint32 word, uint32 extra, const Banks& banks,
                                    uint64 budget, ErrorCode& error) noexcept {
    const BytecodeModule& module = *module_;
    const uint32 a = DecodeA(word);
    const uint32 bb = DecodeB(word);
    const uint32 c = DecodeC(word);
    auto put = [&](uint32 at, VmBuffer* value) noexcept {
        VmBuffer* previous = banks.b[at];
        banks.b[at] = value;
        Release(previous);
    };
    // bank 레지스터 at부터 slots개를 target에 복사한다(Buf는 참조 +1).
    auto store = [&](uint64* target, uint32 bank, uint32 at, uint32 slots) noexcept {
        for (uint32 slot = 0; slot < slots; ++slot) {
            if (bank == VmStructBufBank) {
                Retain(banks.b[at + slot]);
                target[slot] = VmStructBits(banks.b[at + slot]);
            } else {
                target[slot] = bank == VmStructScalarBank ? banks.s[at + slot] : banks.r[at + slot];
            }
        }
    };
    // source의 slots개를 bank 레지스터 at으로 읽는다(Buf는 참조 +1).
    auto load = [&](const uint64* source, uint32 bank, uint32 at, uint32 slots) noexcept {
        for (uint32 slot = 0; slot < slots; ++slot) {
            if (bank == VmStructBufBank) {
                VmBuffer* child = VmStructChild(source[slot]);
                Retain(child);
                put(at + slot, child);
            } else if (bank == VmStructScalarBank) {
                banks.s[at + slot] = source[slot];
            } else {
                banks.r[at + slot] = source[slot];
            }
        }
    };

    switch (op) {
    case Opcode::NewS: {
        // TYPE의 struct 배치로 모든 slot이 0·null인 버퍼(기본값은 컴파일러의 SETSF).
        const BytecodeType& type = module.types[module.refs[DecodeBx(word)].detail];
        const uint64 slots = uint64(type.slots[0]) + type.slots[1] + type.slots[2];
        const Allocation allocation =
            AllocateBytes(VmBufferKind::Struct, slots * sizeof(uint64), budget);
        error = allocation.error;
        if (allocation.buffer == nullptr) {
            return allocation.fault;
        }

        allocation.buffer->count = static_cast<uint32>(slots);
        allocation.buffer->capacity = type.slots[VmStructBufBank];
        uint64* target = VmStructSlots(allocation.buffer);
        for (uint64 slot = 0; slot < slots; ++slot) {
            target[slot] = 0;
        }

        put(a, allocation.buffer);
        return VmFaultKind::None;
    }
    case Opcode::GetSFS:
    case Opcode::GetSFR:
    case Opcode::GetSFB: {
        // A ← struct B의 필드(C slot).
        const uint32 field = module.refs[DecodeAx(extra)].detail;
        VmBuffer* value = banks.b[bb];
        if (field == NoBytecodeIndex || value == nullptr || value->kind != VmBufferKind::Struct) {
            return VmFaultKind::ContractViolation; // 엔진 struct(LC5d)·null struct
        }

        load(VmStructSlots(value) + module.typeFields[field].offset, VmFieldBank(op), a, c);
        return VmFaultKind::None;
    }
    case Opcode::SetSFS:
    case Opcode::SetSFR:
    case Opcode::SetSFB: {
        // struct A의 필드 ← B(C slot). 공유된 struct는 먼저 복제한다(COW).
        const uint32 field = module.refs[DecodeAx(extra)].detail;
        VmBuffer*& slot = banks.b[a];
        if (field == NoBytecodeIndex || slot == nullptr || slot->kind != VmBufferKind::Struct) {
            return VmFaultKind::ContractViolation;
        }

        if (slot->references > 1) {
            const Allocation allocation =
                AllocateBytes(VmBufferKind::Struct, uint64(slot->count) * sizeof(uint64), budget);
            error = allocation.error;
            if (allocation.buffer == nullptr) {
                return allocation.fault;
            }

            VmBuffer* copy = allocation.buffer;
            copy->count = slot->count;
            copy->capacity = slot->capacity;
            platform::MemoryCopy(VmStructSlots(copy), VmStructSlots(slot),
                                 uint64(slot->count) * sizeof(uint64));
            for (uint32 at = copy->count - copy->capacity; at < copy->count; ++at) {
                Retain(VmStructChild(VmStructSlots(copy)[at]));
            }

            --slot->references;
            slot = copy;
        }

        const uint32 bank = VmFieldBank(op);
        uint64* target = VmStructSlots(slot) + module.typeFields[field].offset;
        const uint64 previous = target[0]; // Buf 필드는 slot 하나다
        store(target, bank, bb, c);
        if (bank == VmStructBufBank) {
            Release(VmStructChild(previous));
        }

        return VmFaultKind::None;
    }
    case Opcode::Ok: {
        // A ← 값 B(EXTRA: 값 bank << 8 | slot 수)의 성공 Buf Result.
        const uint32 bank = (DecodeAx(extra) >> 8) & 0x3;
        const uint32 slots = DecodeAx(extra) & 0xFF;
        const Allocation allocation =
            AllocateBytes(VmBufferKind::Result, (uint64(slots) + 1) * sizeof(uint64), budget);
        error = allocation.error;
        if (allocation.buffer == nullptr) {
            return allocation.fault;
        }

        VmBuffer* result = allocation.buffer;
        result->count = slots + 1;
        result->valueBank = static_cast<uint8>(bank);
        result->valueSlots = static_cast<uint8>(slots);
        VmStructSlots(result)[0] = 0;
        store(VmStructSlots(result) + 1, bank, bb, slots);
        put(a, result);
        return VmFaultKind::None;
    }
    case Opcode::Err: {
        // A ← Scalar B의 `Error` case로 실패한 Buf Result(코드 case + 1, 값 없음).
        const Allocation allocation = MakeErrorResult(banks.s[bb] + 1, budget);
        error = allocation.error;
        if (allocation.buffer == nullptr) {
            return allocation.fault;
        }

        put(a, allocation.buffer);
        return VmFaultKind::None;
    }
    default:
        return VmFaultKind::ContractViolation;
    }
}

} // namespace rocks::sparks
