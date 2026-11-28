#include <Core/Hash/TypeHash.h>
#include <Platform/Memory.h>
#include <SparksVm/Vm.h>

// 컨테이너 명령이다(Docs/Design/SparksVm.ko.md 2.2절, LC5b2). 원소 배열은 삽입 순서이고 원소 하나는
// key slot 뒤 값 slot이다. Map·Set은 원소 배열 뒤에 열린 주소 해시 색인(원소 번호 + 1, 0은 빈
// 칸)을 둔다. Buf slot은 원소 버퍼의 참조 하나를 가진 포인터 비트다.

namespace rocks::sparks {

namespace {

// 처음 원소를 넣을 때의 용량과 늘리는 배수다.
constexpr uint64 VmContainerFirstCapacity = 4;
constexpr uint64 VmContainerGrowth = 2;

// 해시 색인의 최소 칸 수와 원소 대비 배수(빈 칸이 늘 남게)다.
constexpr uint64 VmIndexMinimum = 8;
constexpr uint64 VmIndexLoad = 2;

// 색인의 빈 칸이다(원소 번호 + 1을 둔다).
constexpr uint32 VmIndexEmpty = 0;

// 원소를 찾지 못했다.
constexpr uint32 VmNoElement = NumericLimits<uint32>::Max();

// 한 묶음 slot 수의 상한(컨테이너 표지 5비트)이다.
constexpr uint32 VmMarkerSlotLimit = 32;

constexpr uint32 VmBufBank = static_cast<uint32>(BytecodeBank::Buf);
constexpr uint32 VmScalarBank = static_cast<uint32>(BytecodeBank::Scalar);

// 지금 frame의 bank별 레지스터다(VmExecution::Banks와 같은 모양 — 그 타입은 private이다).
struct VmBankView {
    uint64* s = nullptr;
    uint64* r = nullptr;
    VmBuffer** b = nullptr;
};

struct VmLayout {
    uint32 valueBank = 0;
    uint32 valueSlots = 0;
    uint32 keyBank = 0;
    uint32 keySlots = 0;
};

VmLayout VmDecodeMarker(uint32 marker) noexcept {
    VmLayout layout;
    layout.valueSlots = marker & 0x1F;
    layout.valueBank = (marker >> 5) & 0x3;
    layout.keySlots = (marker >> 8) & 0x1F;
    layout.keyBank = (marker >> 13) & 0x3;
    return layout;
}

bool VmHashed(const VmBuffer* buffer) noexcept {
    return buffer->kind == VmBufferKind::Map || buffer->kind == VmBufferKind::Set;
}

uint32 VmStride(const VmBuffer* buffer) noexcept {
    return uint32(buffer->keySlots) + buffer->valueSlots;
}

uint64* VmSlots(VmBuffer* buffer) noexcept {
    return reinterpret_cast<uint64*>(buffer->Bytes());
}

uint32* VmIndexTable(VmBuffer* buffer) noexcept {
    return reinterpret_cast<uint32*>(VmSlots(buffer) + uint64(buffer->capacity) * VmStride(buffer));
}

VmBuffer* VmChild(uint64 bits) noexcept {
    return reinterpret_cast<VmBuffer*>(static_cast<usize>(bits));
}

uint64 VmChildBits(VmBuffer* buffer) noexcept {
    return static_cast<uint64>(reinterpret_cast<usize>(buffer));
}

uint64 VmIndexCapacity(uint64 capacity) noexcept {
    uint64 size = VmIndexMinimum;
    while (size < capacity * VmIndexLoad) {
        size *= 2;
    }

    return size;
}

// key 묶음의 해시다(문자열 key는 byte, 그 밖은 slot 비트 — Core 분포용 해시).
uint64 VmHashKey(const uint64* key, uint32 slots, uint32 bank) noexcept {
    uint64 hash = 0;
    for (uint32 slot = 0; slot < slots; ++slot) {
        const VmBuffer* child = bank == VmBufBank ? VmChild(key[slot]) : nullptr;
        hash = HashCombine(hash, bank == VmBufBank ? HashStringBytes(VmStringOf(child))
                                                   : MixHash(key[slot]));
    }

    return hash;
}

bool VmSameKey(const uint64* left, const uint64* right, uint32 slots, uint32 bank) noexcept {
    for (uint32 slot = 0; slot < slots; ++slot) {
        if (bank == VmBufBank) {
            const VmBuffer* a = VmChild(left[slot]);
            const VmBuffer* b = VmChild(right[slot]);
            if ((a == nullptr) != (b == nullptr) ||
                (a != nullptr && VmStringOf(a) != VmStringOf(b))) {
                return false;
            }
        } else if (left[slot] != right[slot]) {
            return false;
        }
    }

    return true;
}

// key의 원소 번호다(없으면 VmNoElement).
uint32 VmFind(VmBuffer* buffer, const uint64* key) noexcept {
    if (buffer == nullptr || buffer->count == 0 || buffer->indexCapacity == 0) {
        return VmNoElement;
    }

    const uint32 stride = VmStride(buffer);
    const uint64 mask = buffer->indexCapacity - 1;
    const uint32* table = VmIndexTable(buffer);
    for (uint64 position = VmHashKey(key, buffer->keySlots, buffer->keyBank) & mask;;
         position = (position + 1) & mask) {
        const uint32 entry = table[position];
        if (entry == VmIndexEmpty) {
            return VmNoElement;
        }

        if (VmSameKey(VmSlots(buffer) + uint64(entry - 1) * stride, key, buffer->keySlots,
                      buffer->keyBank)) {
            return entry - 1;
        }
    }
}

void VmIndexInsert(VmBuffer* buffer, uint32 element) noexcept {
    const uint64 mask = buffer->indexCapacity - 1;
    uint32* table = VmIndexTable(buffer);
    const uint64* key = VmSlots(buffer) + uint64(element) * VmStride(buffer);
    uint64 position = VmHashKey(key, buffer->keySlots, buffer->keyBank) & mask;
    while (table[position] != VmIndexEmpty) {
        position = (position + 1) & mask;
    }

    table[position] = element + 1;
}

// 레지스터 묶음 하나(bank의 at부터 slots개)를 원소 자리에 복사한다(Buf는 참조 +1).
void VmStore(uint64* target, const VmBankView& banks, uint32 bank, uint32 at,
             uint32 slots) noexcept {
    for (uint32 slot = 0; slot < slots; ++slot) {
        if (bank == VmBufBank) {
            VmBuffer* child = banks.b[at + slot];
            if (child != nullptr) {
                ++child->references;
            }

            target[slot] = VmChildBits(child);
        } else {
            target[slot] = bank == VmScalarBank ? banks.s[at + slot] : banks.r[at + slot];
        }
    }
}

// 레지스터 묶음 하나를 key 비교용 slot 묶음으로 읽는다(Buf는 포인터 비트).
void VmLoadKey(uint64* key, const VmBankView& banks, uint32 bank, uint32 at,
               uint32 slots) noexcept {
    for (uint32 slot = 0; slot < slots; ++slot) {
        key[slot] = bank == VmBufBank      ? VmChildBits(banks.b[at + slot])
                    : bank == VmScalarBank ? banks.s[at + slot]
                                           : banks.r[at + slot];
    }
}

} // namespace

VmFaultKind VmExecution::Reserve(VmBuffer*& slot, uint64 needed, uint32 marker, uint64 budget,
                                 ErrorCode& error) noexcept {
    // 공유된 버퍼(참조 둘 이상)는 복제하고, 자리가 모자라면 늘린다(SparksVm 2.2절 COW). 빈
    // 컨테이너는 표지의 배치를 적는다(Set은 원소를 key 자리에).
    VmBuffer* current = slot;
    if (current == nullptr) {
        return VmFaultKind::ContractViolation; // null 컨테이너(`T?`)는 컴파일러가 먼저 꺼낸다
    }

    if (current->references == 1 && current->capacity >= needed) {
        return VmFaultKind::None;
    }

    VmLayout layout;
    if (current->capacity > 0) {
        layout.keyBank = current->keyBank;
        layout.keySlots = current->keySlots;
        layout.valueBank = current->valueBank;
        layout.valueSlots = current->valueSlots;
    } else {
        layout = VmDecodeMarker(marker);
        if (current->kind == VmBufferKind::Set) {
            layout.keyBank = layout.valueBank;
            layout.keySlots = layout.valueSlots;
            layout.valueBank = 0;
            layout.valueSlots = 0;
        } else if (current->kind == VmBufferKind::Array) {
            layout.keyBank = 0;
            layout.keySlots = 0;
        }
    }

    // 용량: 2배(처음 4)로 늘리되 단일 버퍼 상한(원소 수·byte — 색인 포함)에 들어가는 만큼으로
    // 자른다. 필요한 원소 수가 상한을 넘을 때만 BudgetExceeded다(SparksVm 2.2절).
    const uint64 stride = uint64(layout.keySlots) + layout.valueSlots;
    const bool hashed = VmHashed(current);
    const uint64 elementBytes =
        stride * sizeof(uint64) + (hashed ? VmIndexLoad * 2 * sizeof(uint32) : 0);
    uint64 limit = elementBytes == 0 ? MaxBufferElements : MaxBufferBytes / elementBytes;
    limit = limit > MaxBufferElements ? MaxBufferElements : limit;
    if (needed > limit) {
        return VmFaultKind::BudgetExceeded;
    }

    uint64 capacity = current->capacity;
    if (capacity < needed) {
        capacity = capacity == 0 ? VmContainerFirstCapacity : capacity * VmContainerGrowth;
        capacity = capacity > limit ? limit : capacity;
        capacity = capacity < needed ? needed : capacity;
    }

    const uint64 indexCapacity = hashed ? VmIndexCapacity(capacity) : 0;
    const uint64 payload = capacity * stride * sizeof(uint64) + indexCapacity * sizeof(uint32);
    const Allocation allocation = AllocateBytes(current->kind, payload, budget);
    if (allocation.error != ErrorCode::None) {
        error = allocation.error;
        return VmFaultKind::None;
    }

    if (allocation.fault != VmFaultKind::None) {
        return allocation.fault;
    }

    VmBuffer* grown = allocation.buffer;
    grown->keyBank = static_cast<uint8>(layout.keyBank);
    grown->keySlots = static_cast<uint8>(layout.keySlots);
    grown->valueBank = static_cast<uint8>(layout.valueBank);
    grown->valueSlots = static_cast<uint8>(layout.valueSlots);
    grown->capacity = static_cast<uint32>(capacity);
    grown->indexCapacity = static_cast<uint32>(indexCapacity);
    grown->count = current->count;
    const uint64 used = uint64(current->count) * stride;
    if (used != 0) {
        platform::MemoryCopy(VmSlots(grown), VmSlots(current), used * sizeof(uint64));
    }

    if (hashed) {
        uint32* table = VmIndexTable(grown);
        for (uint64 at = 0; at < indexCapacity; ++at) {
            table[at] = VmIndexEmpty;
        }

        for (uint32 element = 0; element < grown->count; ++element) {
            VmIndexInsert(grown, element);
        }
    }

    if (current->references == 1) {
        FreeShell(current); // 원소의 참조는 새 버퍼로 옮겼다
    } else {
        // 공유: 복사한 원소 버퍼의 참조를 더하고 이 실행의 참조 하나를 놓는다.
        uint64* slots = VmSlots(grown);
        for (uint64 at = 0; at < used; ++at) {
            const bool key = at % stride < layout.keySlots;
            if ((key ? layout.keyBank : layout.valueBank) == VmBufBank &&
                VmChild(slots[at]) != nullptr) {
                ++VmChild(slots[at])->references;
            }
        }

        --current->references;
    }

    slot = grown;
    return VmFaultKind::None;
}

Result<VmBuffer*> VmExecution::NewContainer(VmBufferKind kind, uint32 marker,
                                            Span<const uint64> elements) noexcept {
    // 원소 배치를 검사하고, 빈 컨테이너에 Reserve로 자리를 만든 뒤 원소를 차례로 넣는다(Map·Set은
    // 색인도 — 같은 key는 InvalidArgument).
    if (kind != VmBufferKind::Array && kind != VmBufferKind::Map && kind != VmBufferKind::Set) {
        return Result<VmBuffer*>::Failure(ErrorCode::InvalidArgument);
    }

    // 표지는 verifier의 표지 규칙과 같다(리뷰 A8): 남는 비트 없음, bank 0–2, Ref·Buf 묶음은 1 slot.
    const VmLayout layout = VmDecodeMarker(marker);
    auto validGroup = [](uint32 bank, uint32 slots) noexcept {
        return bank <= VmBufBank && (bank == 0 || slots <= 1);
    };
    if (EncodeContainerMarker(layout.valueBank, layout.valueSlots, layout.keyBank,
                              layout.keySlots) != marker ||
        !validGroup(layout.valueBank, layout.valueSlots) ||
        !validGroup(layout.keyBank, layout.keySlots)) {
        return Result<VmBuffer*>::Failure(ErrorCode::InvalidArgument);
    }

    const bool keyed = kind == VmBufferKind::Map;
    const uint64 stride = uint64(layout.valueSlots) + (keyed ? layout.keySlots : 0);
    if (layout.valueSlots == 0 || (keyed && layout.keySlots == 0) ||
        (!keyed && layout.keySlots != 0) || elements.Size() % stride != 0) {
        return Result<VmBuffer*>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 count = elements.Size() / stride;
    Allocation empty = AllocateBytes(kind, 0, 0);
    if (empty.error != ErrorCode::None) {
        return Result<VmBuffer*>::Failure(empty.error);
    }

    VmBuffer* buffer = empty.buffer;
    ErrorCode error = ErrorCode::None;
    const VmFaultKind fault =
        count == 0 ? VmFaultKind::None : Reserve(buffer, count, marker, 0, error);
    if (error != ErrorCode::None || fault != VmFaultKind::None) {
        Release(buffer);
        return Result<VmBuffer*>::Failure(error != ErrorCode::None ? error
                                                                   : ErrorCode::CapacityExceeded);
    }

    const bool hashed = VmHashed(buffer);
    for (uint64 element = 0; element < count; ++element) {
        const uint64* source = elements.Data() + element * stride;
        if (hashed && VmFind(buffer, source) != VmNoElement) {
            Release(buffer);
            return Result<VmBuffer*>::Failure(ErrorCode::InvalidArgument);
        }

        uint64* target = VmSlots(buffer) + element * stride;
        for (uint64 slot = 0; slot < stride; ++slot) {
            const bool key = slot < buffer->keySlots;
            target[slot] = source[slot];
            if ((key ? buffer->keyBank : buffer->valueBank) == VmBufBank) {
                Retain(VmChild(source[slot]));
            }
        }

        ++buffer->count;
        if (hashed) {
            VmIndexInsert(buffer, static_cast<uint32>(element));
        }
    }

    return Result<VmBuffer*>::Success(buffer);
}

VmFaultKind VmExecution::Container(Opcode op, uint32 word, uint32 marker, const Banks& registers,
                                   uint64 budget, ErrorCode& error) noexcept {
    const VmBankView banks{registers.s, registers.r, registers.b};
    const uint32 a = DecodeA(word);
    const uint32 bb = DecodeB(word);
    const uint32 c = DecodeC(word);
    const VmLayout layout = VmDecodeMarker(marker);
    // 표지의 묶음이 버퍼의 배치(key 자리면 useKey)와 같은지다. 배치가 아직 없는(빈) 버퍼는 맞는
    // 것으로 본다 — 다른 배치의 명령이 버퍼 밖을 읽거나 쓰지 않게 한다(SparksVm 2.2절).
    auto matches = [](const VmBuffer* buffer, uint32 bank, uint32 slots, bool useKey) noexcept {
        if (buffer->capacity == 0) {
            return true;
        }

        return useKey ? buffer->keyBank == bank && buffer->keySlots == slots
                      : buffer->valueBank == bank && buffer->valueSlots == slots;
    };
    // GETE 결과 묶음(`T?`)에서 원소 값 묶음의 slot 수다(Scalar는 앞 표지 slot을 뺀다).
    const uint32 elementSlots = layout.valueBank == VmScalarBank && layout.valueSlots > 0
                                    ? layout.valueSlots - 1
                                    : layout.valueSlots;
    // Buf 레지스터 at에 value를 둔다(참조는 넘겨받는다 — 이전 값을 놓는다).
    auto put = [&](uint32 at, VmBuffer* value) noexcept {
        VmBuffer* previous = banks.b[at];
        banks.b[at] = value;
        Release(previous);
    };
    // 원소 묶음(element의 slot부터 slots개)을 bank 레지스터 at으로 읽는다(Buf는 참조 +1).
    auto load = [&](VmBuffer* buffer, uint32 element, uint32 offset, uint32 bank, uint32 at,
                    uint32 slots) noexcept {
        const uint64* source = VmSlots(buffer) + uint64(element) * VmStride(buffer) + offset;
        for (uint32 slot = 0; slot < slots; ++slot) {
            if (bank == VmBufBank) {
                VmBuffer* child = VmChild(source[slot]);
                if (child != nullptr) {
                    ++child->references;
                }

                put(at + slot, child);
            } else if (bank == VmScalarBank) {
                banks.s[at + slot] = source[slot];
            } else {
                banks.r[at + slot] = source[slot];
            }
        }
    };
    // 원소의 값 자리를 레지스터 묶음으로 바꾼다(이전 Buf 값은 놓는다).
    auto replace = [&](VmBuffer* buffer, uint32 element, uint32 bank, uint32 at,
                       uint32 slots) noexcept {
        uint64* target = VmSlots(buffer) + uint64(element) * VmStride(buffer) + buffer->keySlots;
        uint64 previous[VmMarkerSlotLimit] = {};
        for (uint32 slot = 0; slot < slots; ++slot) {
            previous[slot] = target[slot];
        }

        VmStore(target, banks, bank, at, slots);
        for (uint32 slot = 0; slot < slots && bank == VmBufBank; ++slot) {
            Release(VmChild(previous[slot]));
        }
    };
    // Map에 key(bank 레지스터 keyAt)의 값(valueAt)을 넣거나 바꾼다.
    auto assign = [&](uint32 at, uint32 keyAt, uint32 valueAt) noexcept -> VmFaultKind {
        uint64 key[VmMarkerSlotLimit] = {};
        VmLoadKey(key, banks, layout.keyBank, keyAt, layout.keySlots);
        const uint32 found = VmFind(banks.b[at], key);
        VmBuffer*& slot = banks.b[at];
        const uint64 needed = found == VmNoElement ? uint64(slot->count) + 1 : slot->count;
        if (const VmFaultKind fault = Reserve(slot, needed, marker, budget, error);
            fault != VmFaultKind::None || error != ErrorCode::None) {
            return fault;
        }

        if (found != VmNoElement) {
            replace(slot, found, layout.valueBank, valueAt, layout.valueSlots);
            return VmFaultKind::None;
        }

        uint64* target = VmSlots(slot) + uint64(slot->count) * VmStride(slot);
        VmStore(target, banks, layout.keyBank, keyAt, layout.keySlots);
        VmStore(target + slot->keySlots, banks, layout.valueBank, valueAt, layout.valueSlots);
        ++slot->count;
        VmIndexInsert(slot, slot->count - 1);
        return VmFaultKind::None;
    };
    // GETE의 없음(Scalar는 표지 0과 0, Ref 0, Buf null)이다.
    auto none = [&]() noexcept {
        for (uint32 slot = 0; slot < layout.valueSlots; ++slot) {
            if (layout.valueBank == VmBufBank) {
                put(a + slot, nullptr);
            } else if (layout.valueBank == VmScalarBank) {
                banks.s[a + slot] = 0;
            } else {
                banks.r[a + slot] = 0;
            }
        }
    };

    switch (op) {
    case Opcode::NewA:
    case Opcode::NewSet:
    case Opcode::NewM: {
        const VmBufferKind kind = op == Opcode::NewA     ? VmBufferKind::Array
                                  : op == Opcode::NewSet ? VmBufferKind::Set
                                                         : VmBufferKind::Map;
        const Allocation allocation = AllocateBytes(kind, 0, budget);
        error = allocation.error;
        if (allocation.buffer != nullptr) {
            put(a, allocation.buffer);
        }

        return allocation.fault;
    }
    case Opcode::Append: {
        VmBuffer*& slot = banks.b[a];
        if (slot != nullptr &&
            !matches(slot, layout.valueBank, layout.valueSlots, slot->kind == VmBufferKind::Set)) {
            return VmFaultKind::ContractViolation;
        }

        if (slot != nullptr && slot->kind == VmBufferKind::Set) {
            // Set: 이미 있으면 넣지 않는다(원소는 key 자리).
            uint64 key[VmMarkerSlotLimit] = {};
            VmLoadKey(key, banks, layout.valueBank, bb, layout.valueSlots);
            if (VmFind(slot, key) != VmNoElement) {
                return VmFaultKind::None;
            }

            if (const VmFaultKind fault =
                    Reserve(slot, uint64(slot->count) + 1, marker, budget, error);
                fault != VmFaultKind::None || error != ErrorCode::None) {
                return fault;
            }

            VmStore(VmSlots(slot) + uint64(slot->count) * VmStride(slot), banks, layout.valueBank,
                    bb, layout.valueSlots);
            ++slot->count;
            VmIndexInsert(slot, slot->count - 1);
            return VmFaultKind::None;
        }

        if (slot == nullptr || slot->kind != VmBufferKind::Array) {
            return VmFaultKind::ContractViolation;
        }

        if (const VmFaultKind fault = Reserve(slot, uint64(slot->count) + 1, marker, budget, error);
            fault != VmFaultKind::None || error != ErrorCode::None) {
            return fault;
        }

        VmStore(VmSlots(slot) + uint64(slot->count) * VmStride(slot), banks, layout.valueBank, bb,
                layout.valueSlots);
        ++slot->count;
        return VmFaultKind::None;
    }
    case Opcode::Put:
        if (banks.b[a] == nullptr || banks.b[a]->kind != VmBufferKind::Map ||
            !matches(banks.b[a], layout.valueBank, layout.valueSlots, false) ||
            !matches(banks.b[a], layout.keyBank, layout.keySlots, true)) {
            return VmFaultKind::ContractViolation;
        }

        return assign(a, bb, c);
    case Opcode::SetE: {
        // SETE A B C: 컨테이너 A[key B] = 값 C. Array는 범위 밖이면 IndexOutOfRange.
        VmBuffer*& slot = banks.b[a];
        if (slot == nullptr) {
            return VmFaultKind::ContractViolation;
        }

        if (!matches(slot, layout.valueBank, layout.valueSlots, false)) {
            return VmFaultKind::ContractViolation;
        }

        if (slot->kind == VmBufferKind::Map) {
            return matches(slot, layout.keyBank, layout.keySlots, true)
                       ? assign(a, bb, c)
                       : VmFaultKind::ContractViolation;
        }

        if (slot->kind != VmBufferKind::Array || layout.keyBank != VmScalarBank ||
            layout.keySlots != 1) {
            return VmFaultKind::ContractViolation;
        }

        const int64 index = static_cast<int64>(banks.s[bb]);
        if (index < 0 || static_cast<uint64>(index) >= slot->count) {
            return VmFaultKind::IndexOutOfRange;
        }

        if (const VmFaultKind fault = Reserve(slot, slot->count, marker, budget, error);
            fault != VmFaultKind::None || error != ErrorCode::None) {
            return fault;
        }

        replace(slot, static_cast<uint32>(index), layout.valueBank, c, layout.valueSlots);
        return VmFaultKind::None;
    }
    case Opcode::GetE: {
        // GETE A B C: A ← 컨테이너 B[key C]의 `T?`(표지의 값 묶음은 결과 A의 모양).
        VmBuffer* container = banks.b[bb];
        uint32 element = VmNoElement;
        if (container != nullptr && !matches(container, layout.valueBank, elementSlots, false)) {
            return VmFaultKind::ContractViolation;
        }

        if (container != nullptr && container->kind == VmBufferKind::Array) {
            if (layout.keyBank != VmScalarBank || layout.keySlots != 1) {
                return VmFaultKind::ContractViolation;
            }

            const int64 index = static_cast<int64>(banks.s[c]);
            element = index >= 0 && static_cast<uint64>(index) < container->count
                          ? static_cast<uint32>(index)
                          : VmNoElement;
        } else if (container != nullptr && container->kind == VmBufferKind::Map) {
            if (!matches(container, layout.keyBank, layout.keySlots, true)) {
                return VmFaultKind::ContractViolation;
            }

            uint64 key[VmMarkerSlotLimit] = {};
            VmLoadKey(key, banks, layout.keyBank, c, layout.keySlots);
            element = VmFind(container, key);
        } else if (container != nullptr) {
            return VmFaultKind::ContractViolation;
        }

        if (element == VmNoElement) {
            none();
            return VmFaultKind::None;
        }

        // 원소 값을 먼저 읽고(Buf는 참조 +1) 쓴다 — A가 B와 같은 레지스터여도 안전하다.
        const uint32 offset = container->keySlots;
        if (layout.valueBank == VmScalarBank) {
            banks.s[a] = 1;
            load(container, element, offset, VmScalarBank, a + 1, layout.valueSlots - 1);
        } else {
            load(container, element, offset, layout.valueBank, a, layout.valueSlots);
        }

        return VmFaultKind::None;
    }
    case Opcode::Len:
        banks.s[a] = banks.b[bb] == nullptr ? 0 : banks.b[bb]->count;
        return VmFaultKind::None;
    case Opcode::GetKey:
    case Opcode::GetVal: {
        // 삽입 순 C번 원소의 key(Map·Set)·값(Map·Array)이다. 표지의 값 묶음은 결과 A의 모양이다.
        VmBuffer* container = banks.b[bb];
        const uint64 index = banks.s[c];
        if (container == nullptr || index >= container->count) {
            return VmFaultKind::IndexOutOfRange;
        }

        // GETKEY는 Map·Set의 key 자리, GETVAL은 Map·Array의 값 자리다.
        const bool key = op == Opcode::GetKey;
        if ((key && container->kind == VmBufferKind::Array) ||
            (!key && container->kind == VmBufferKind::Set) ||
            !matches(container, layout.valueBank, layout.valueSlots, key)) {
            return VmFaultKind::ContractViolation;
        }

        load(container, static_cast<uint32>(index), key ? 0 : container->keySlots, layout.valueBank,
             a, layout.valueSlots);
        return VmFaultKind::None;
    }
    default:
        return VmFaultKind::ContractViolation;
    }
}

} // namespace rocks::sparks

namespace rocks::sparks::detail {

uint32 VmContainerFind(VmBuffer* buffer, const uint64* key) noexcept {
    return VmFind(buffer, key);
}

void VmContainerIndexInsert(VmBuffer* buffer, uint32 element) noexcept {
    if (VmHashed(buffer) && buffer->indexCapacity != 0) {
        VmIndexInsert(buffer, element);
    }
}

void VmContainerReindex(VmBuffer* buffer) noexcept {
    if (!VmHashed(buffer) || buffer->indexCapacity == 0) {
        return;
    }

    uint32* table = VmIndexTable(buffer);
    for (uint32 at = 0; at < buffer->indexCapacity; ++at) {
        table[at] = VmIndexEmpty;
    }

    for (uint32 element = 0; element < buffer->count; ++element) {
        VmIndexInsert(buffer, element);
    }
}

} // namespace rocks::sparks::detail
