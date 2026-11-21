#include <Core/Algorithms.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Memory.h>

#include "VmArithmetic.h"
#include "VmLibrary.h"

// Array·Map·Set·Result·Random 언어 함수다(Docs/Design/SparksVm.ko.md 6a절, SL 부록 B). 값을 바꾸는
// 메서드는 수신자(Buf 창 0번)를 그 자리에서 바꾼다(COW — 공유된 버퍼는 먼저 복제).

namespace rocks::sparks::detail {

namespace {

using C = VmLibraryContext;

constexpr uint32 VmBuf = static_cast<uint32>(BytecodeBank::Buf);
constexpr uint32 VmScalar = static_cast<uint32>(BytecodeBank::Scalar);
constexpr uint32 VmRef = static_cast<uint32>(BytecodeBank::Ref);
constexpr uint32 VmNotFound = NumericLimits<uint32>::Max();
constexpr uint32 VmSlotLimit = 32;

VmBuffer* Child(uint64 bits) noexcept {
    return reinterpret_cast<VmBuffer*>(static_cast<usize>(bits));
}

uint64* Slots(VmBuffer* buffer) noexcept {
    return reinterpret_cast<uint64*>(buffer->Bytes());
}

uint32 Stride(const VmBuffer* buffer) noexcept {
    return uint32(buffer->keySlots) + buffer->valueSlots;
}

// 수신자(피연산자 0)의 원소 배치 표지다: Array·Set은 원소, Map은 key·값.
uint32 MarkerOf(const C& call) noexcept {
    const VmTypeNode& receiver = call.Operand(0);
    const VmTypeNode& first = call.Node(receiver.child[0]);
    if (receiver.code == VmTypeCode::Map) {
        const VmTypeNode& value = call.Node(receiver.child[1]);
        return EncodeContainerMarker(value.bank, value.slots, first.bank, first.slots);
    }

    return EncodeContainerMarker(first.bank, first.slots, 0, 0);
}

// 수신자 버퍼가 서술자의 컨테이너 종류이고, 배치가 정해졌으면 그 배치가 서술자와 같은지다(리뷰 A3 —
// host가 다른 배치의 컨테이너를 넘겨도 버퍼 밖을 읽거나 쓰지 않는다). null은 빈 컨테이너다.
bool Fits(const C& call) noexcept {
    const VmBuffer* buffer = call.Buffer(0);
    if (buffer == nullptr) {
        return true;
    }

    const VmTypeCode code = call.Operand(0).code;
    const VmBufferKind kind = code == VmTypeCode::Array ? VmBufferKind::Array
                              : code == VmTypeCode::Map ? VmBufferKind::Map
                                                        : VmBufferKind::Set;
    if (buffer->kind != kind) {
        return false;
    }

    if (buffer->capacity == 0) {
        return true;
    }

    const VmTypeNode& receiver = call.Operand(0);
    const VmTypeNode& first = call.Node(receiver.child[0]);
    if (code == VmTypeCode::Map) {
        const VmTypeNode& value = call.Node(receiver.child[1]);
        return buffer->keyBank == first.bank && buffer->keySlots == first.slots &&
               buffer->valueBank == value.bank && buffer->valueSlots == value.slots;
    }

    if (code == VmTypeCode::Set) {
        return buffer->keyBank == first.bank && buffer->keySlots == first.slots &&
               buffer->valueSlots == 0;
    }

    return buffer->keySlots == 0 && buffer->valueBank == first.bank &&
           buffer->valueSlots == first.slots;
}

// 피연산자 operand(원소 하나)를 slot 비트로 읽는다(Buf는 포인터 비트).
uint32 Load(const C& call, uint32 operand, uint64* out) noexcept {
    const VmTypeNode& node = call.Operand(operand);
    for (uint32 slot = 0; slot < node.slots; ++slot) {
        out[slot] = node.bank == VmBuf      ? uint64(reinterpret_cast<usize>(call.Buffer(operand)))
                    : node.bank == VmScalar ? call.Scalars(operand)[slot]
                                            : call.r[call.signature->offsets[operand] + slot];
    }

    return node.slots;
}

// 원소 slot 묶음 둘이 같은지다(문자열은 byte, 그 밖은 비트 — `==` 가능한 원소만 온다).
bool Same(C& call, const uint64* left, const uint64* right, uint32 slots, uint32 bank) noexcept {
    for (uint32 slot = 0; slot < slots; ++slot) {
        if (bank == VmRef) {
            // 죽은 객체는 null과 같다(SL 3.3절 — 리뷰 B2).
            const uint64 a = VmLibrary::Alive(call, left[slot]) ? left[slot] : 0;
            const uint64 b = VmLibrary::Alive(call, right[slot]) ? right[slot] : 0;
            if (a != b) {
                return false;
            }

            continue;
        }

        if (bank != VmBuf) {
            if (left[slot] != right[slot]) {
                return false;
            }

            continue;
        }

        const VmBuffer* a = Child(left[slot]);
        const VmBuffer* b = Child(right[slot]);
        if ((a == nullptr) != (b == nullptr) || (a != nullptr && VmStringOf(a) != VmStringOf(b))) {
            return false;
        }
    }

    return true;
}

// 원소 묶음을 넣는다(Buf는 참조 +1).
void Store(uint64* target, const uint64* source, uint32 slots, uint32 bank) noexcept {
    for (uint32 slot = 0; slot < slots; ++slot) {
        target[slot] = source[slot];
        if (bank == VmBuf && Child(source[slot]) != nullptr) {
            ++Child(source[slot])->references;
        }
    }
}

// 원소 element를 지우고 뒤를 당긴다(그 원소의 Buf slot은 놓는다). 수신자는 이미 이 실행만 쥔다.
void Erase(C& call, VmBuffer* buffer, uint32 element) noexcept {
    const uint32 stride = Stride(buffer);
    uint64* slots = Slots(buffer);
    uint64* target = slots + uint64(element) * stride;
    for (uint32 slot = 0; slot < stride; ++slot) {
        const bool key = slot < buffer->keySlots;
        if ((key ? buffer->keyBank : buffer->valueBank) == VmBuf) {
            VmLibrary::Release(call, Child(target[slot]));
        }
    }

    const uint64 tail = (uint64(buffer->count) - element - 1) * stride;
    if (tail != 0) {
        platform::MemoryMove(target, target + stride, tail * sizeof(uint64));
    }

    --buffer->count;
    VmContainerReindex(buffer);
}

// 수신자를 이 실행만 쥔 버퍼로 만든다(원소 needed개 자리).
VmFaultKind Own(C& call, uint64 needed) noexcept {
    return VmLibrary::Reserve(call, call.Buffer(0), needed, MarkerOf(call));
}

// 선형 찾기(Array — 원소 하나 = 값 묶음).
uint32 IndexOf(C& call, VmBuffer* buffer, const uint64* value, uint32 slots, uint32 bank) noexcept {
    if (buffer == nullptr) {
        return VmNotFound;
    }

    for (uint32 element = 0; element < buffer->count; ++element) {
        if (Same(call, Slots(buffer) + uint64(element) * Stride(buffer), value, slots, bank)) {
            return element;
        }
    }

    return VmNotFound;
}

// ---- Array ----

VmFaultKind ArrayCount(C& call) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    call.s[0] = call.Buffer(0) == nullptr ? 0 : call.Buffer(0)->count;
    return VmFaultKind::None;
}

VmFaultKind ArrayInsertAt(C& call, uint32 indexOperand, uint32 valueOperand, bool append) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    VmBuffer* current = call.Buffer(0);
    const uint64 count = current == nullptr ? 0 : current->count;
    const int64 index = append ? int64(count) : call.Integer(indexOperand);
    if (index < 0 || uint64(index) > count) {
        return VmFaultKind::IndexOutOfRange;
    }

    uint64 value[VmSlotLimit] = {};
    const uint32 slots = Load(call, valueOperand, value);
    const uint32 bank = call.Operand(valueOperand).bank;
    if (const VmFaultKind fault = Own(call, count + 1);
        fault != VmFaultKind::None || call.error != ErrorCode::None) {
        return fault;
    }

    VmBuffer* buffer = call.Buffer(0);
    uint64* at = Slots(buffer) + uint64(index) * slots;
    const uint64 tail = (count - uint64(index)) * slots;
    if (tail != 0) {
        platform::MemoryMove(at + slots, at, tail * sizeof(uint64));
    }

    Store(at, value, slots, bank);
    ++buffer->count;
    return VmFaultKind::None;
}

VmFaultKind ArrayAdd(C& call) noexcept {
    return ArrayInsertAt(call, 0, 1, true);
}

VmFaultKind ArrayInsert(C& call) noexcept {
    return ArrayInsertAt(call, 1, 2, false);
}

VmFaultKind ArrayRemoveAt(C& call) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    VmBuffer* current = call.Buffer(0);
    const int64 index = call.Integer(1);
    if (current == nullptr || index < 0 || uint64(index) >= current->count) {
        return VmFaultKind::IndexOutOfRange;
    }

    if (const VmFaultKind fault = Own(call, current->count);
        fault != VmFaultKind::None || call.error != ErrorCode::None) {
        return fault;
    }

    Erase(call, call.Buffer(0), static_cast<uint32>(index));
    return VmFaultKind::None;
}

VmFaultKind ClearAll(C& call) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    VmBuffer* current = call.Buffer(0);
    if (current == nullptr || current->count == 0) {
        return VmFaultKind::None;
    }

    if (const VmFaultKind fault = Own(call, current->count);
        fault != VmFaultKind::None || call.error != ErrorCode::None) {
        return fault;
    }

    // 원소 버퍼를 한꺼번에 놓고 색인을 한 번 비운다(리뷰 A6 — 원소마다 지우면 O(n²)).
    VmBuffer* buffer = call.Buffer(0);
    const uint32 stride = Stride(buffer);
    uint64* slots = Slots(buffer);
    for (uint64 at = 0; at < uint64(buffer->count) * stride; ++at) {
        const bool key = at % stride < buffer->keySlots;
        if ((key ? buffer->keyBank : buffer->valueBank) == VmBuf) {
            VmLibrary::Release(call, Child(slots[at]));
            slots[at] = 0;
        }
    }

    buffer->count = 0;
    VmContainerReindex(buffer);
    return VmFaultKind::None;
}

VmFaultKind ArrayReverse(C& call) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    VmBuffer* current = call.Buffer(0);
    if (current == nullptr || current->count < 2) {
        return VmFaultKind::None;
    }

    if (const VmFaultKind fault = Own(call, current->count);
        fault != VmFaultKind::None || call.error != ErrorCode::None) {
        return fault;
    }

    VmBuffer* buffer = call.Buffer(0);
    const uint32 stride = Stride(buffer);
    uint64* slots = Slots(buffer);
    for (uint32 low = 0, high = buffer->count - 1; low < high; ++low, --high) {
        for (uint32 slot = 0; slot < stride; ++slot) {
            const uint64 swap = slots[uint64(low) * stride + slot];
            slots[uint64(low) * stride + slot] = slots[uint64(high) * stride + slot];
            slots[uint64(high) * stride + slot] = swap;
        }
    }

    return VmFaultKind::None;
}

VmFaultKind ArrayContains(C& call) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    uint64 value[VmSlotLimit] = {};
    const uint32 slots = Load(call, 1, value);
    call.s[0] = IndexOf(call, call.Buffer(0), value, slots, call.Operand(1).bank) != VmNotFound;
    return VmFaultKind::None;
}

VmFaultKind ArrayIndexOf(C& call) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    uint64 value[VmSlotLimit] = {};
    const uint32 slots = Load(call, 1, value);
    const uint32 at = IndexOf(call, call.Buffer(0), value, slots, call.Operand(1).bank);
    call.s[0] = at != VmNotFound ? 1 : 0;
    call.s[1] = at != VmNotFound ? at : 0;
    return VmFaultKind::None;
}

VmFaultKind ArrayRemove(C& call) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    uint64 value[VmSlotLimit] = {};
    const uint32 slots = Load(call, 1, value);
    const uint32 at = IndexOf(call, call.Buffer(0), value, slots, call.Operand(1).bank);
    if (at == VmNotFound) {
        call.s[0] = 0;
        return VmFaultKind::None;
    }

    // 찾은 원소의 Buf 비트는 Own(복제) 뒤에도 같은 내용이다 — 번호로 지운다.
    if (const VmFaultKind fault = Own(call, call.Buffer(0)->count);
        fault != VmFaultKind::None || call.error != ErrorCode::None) {
        return fault;
    }

    Erase(call, call.Buffer(0), at);
    call.s[0] = 1;
    return VmFaultKind::None;
}

VmFaultKind ArraySort(C& call) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    // 오름차순(정수·실수·문자열 byte 순·enum case 번호 — `<` 가능한 원소, slot 하나).
    VmBuffer* current = call.Buffer(0);
    if (current == nullptr || current->count < 2) {
        return VmFaultKind::None;
    }

    if (const VmFaultKind fault = Own(call, current->count);
        fault != VmFaultKind::None || call.error != ErrorCode::None) {
        return fault;
    }

    VmBuffer* buffer = call.Buffer(0);
    const VmTypeCode code = call.Node(call.Operand(0).child[0]).code;
    Span<uint64> values(Slots(buffer), buffer->count);
    algo::Sort(values, [code](uint64 left, uint64 right) noexcept {
        if (code == VmTypeCode::String) {
            return StringView::CompareBytes(VmStringOf(Child(left)), VmStringOf(Child(right))) < 0;
        }

        if (VmIsSigned(code)) {
            return int64(left) < int64(right);
        }

        if (VmIsFloat(code)) {
            return VmDouble(left) < VmDouble(right);
        }

        return left < right;
    });
    return VmFaultKind::None;
}

// ---- Map·Set ----

VmFaultKind MapContainsKey(C& call) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    uint64 key[VmSlotLimit] = {};
    Load(call, 1, key);
    call.s[0] = VmContainerFind(call.Buffer(0), key) != VmNotFound ? 1 : 0;
    return VmFaultKind::None;
}

VmFaultKind KeyedRemove(C& call) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    uint64 key[VmSlotLimit] = {};
    Load(call, 1, key);
    const uint32 at = VmContainerFind(call.Buffer(0), key);
    if (at == VmNotFound) {
        call.s[0] = 0;
        return VmFaultKind::None;
    }

    if (const VmFaultKind fault = Own(call, call.Buffer(0)->count);
        fault != VmFaultKind::None || call.error != ErrorCode::None) {
        return fault;
    }

    Erase(call, call.Buffer(0), at);
    call.s[0] = 1;
    return VmFaultKind::None;
}

// Map의 key(keys면) 또는 값을 새 Array로 돌려준다(삽입 순).
VmFaultKind MapColumn(C& call, bool keys) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    VmBuffer* map = call.Buffer(0);
    const VmTypeNode& receiver = call.Operand(0);
    const VmTypeNode& column = call.Node(receiver.child[keys ? 0 : 1]);
    const uint64 count = map == nullptr ? 0 : map->count;
    VmFaultKind fault = VmFaultKind::None;
    VmBuffer* array =
        VmLibrary::Allocate(call, VmBufferKind::Array, count * column.slots, sizeof(uint64), fault);
    if (array == nullptr) {
        return fault;
    }

    array->count = static_cast<uint32>(count);
    array->capacity = static_cast<uint32>(count);
    array->valueBank = column.bank;
    array->valueSlots = column.slots;
    for (uint64 element = 0; element < count; ++element) {
        const uint64* source = Slots(map) + element * Stride(map) + (keys ? 0 : map->keySlots);
        Store(Slots(array) + element * column.slots, source, column.slots, column.bank);
    }

    VmLibrary::Put(call, 0, array);
    return VmFaultKind::None;
}

VmFaultKind MapKeys(C& call) noexcept {
    return MapColumn(call, true);
}

VmFaultKind MapValues(C& call) noexcept {
    return MapColumn(call, false);
}

VmFaultKind SetAdd(C& call) noexcept {
    if (!Fits(call)) {
        return VmFaultKind::ContractViolation;
    }

    uint64 value[VmSlotLimit] = {};
    const uint32 slots = Load(call, 1, value);
    VmBuffer* current = call.Buffer(0);
    if (VmContainerFind(current, value) != VmNotFound) {
        call.s[0] = 0;
        return VmFaultKind::None;
    }

    const uint64 count = current == nullptr ? 0 : current->count;
    if (const VmFaultKind fault = Own(call, count + 1);
        fault != VmFaultKind::None || call.error != ErrorCode::None) {
        return fault;
    }

    VmBuffer* buffer = call.Buffer(0);
    Store(Slots(buffer) + uint64(buffer->count) * slots, value, slots, call.Operand(1).bank);
    ++buffer->count;
    VmContainerIndexInsert(buffer, buffer->count - 1); // 새 원소 하나만(리뷰 A6)
    call.s[0] = 1;
    return VmFaultKind::None;
}

// ---- Result ----

VmFaultKind ResultValue(C& call) noexcept {
    // T? — 성공이면 값, 실패면 없음.
    const VmTypeNode& receiver = call.Operand(0);
    if (receiver.bank == VmScalar) {
        const uint32 valueSlots = receiver.slots - 1;
        uint64 copy[VmSlotLimit] = {};
        for (uint32 slot = 0; slot < receiver.slots; ++slot) {
            copy[slot] = call.Scalars(0)[slot];
        }

        const bool ok = copy[0] == 0;
        call.s[0] = ok ? 1 : 0;
        for (uint32 slot = 0; slot < valueSlots; ++slot) {
            call.s[1 + slot] = ok ? copy[1 + slot] : 0;
        }

        return VmFaultKind::None;
    }

    VmBuffer* result = call.Buffer(0);
    if (result == nullptr || result->kind != VmBufferKind::Result) {
        return VmFaultKind::ContractViolation;
    }

    const uint64* payload = Slots(result);
    const bool ok = payload[0] == 0 && result->count > 1;
    if (call.ResultNode().bank == VmBuf) {
        VmBuffer* value = ok ? Child(payload[1]) : nullptr;
        VmLibrary::Retain(value);
        VmLibrary::Put(call, 0, value);
    } else {
        call.r[0] = ok ? payload[1] : 0;
    }

    return VmFaultKind::None;
}

VmFaultKind ResultError(C& call) noexcept {
    // Error? — 실패면 case 번호(코드 − 1), 성공이면 없음.
    const VmTypeNode& receiver = call.Operand(0);
    uint64 code = 0;
    if (receiver.bank == VmScalar) {
        code = call.Scalars(0)[0];
    } else {
        VmBuffer* result = call.Buffer(0);
        if (result == nullptr || result->kind != VmBufferKind::Result) {
            return VmFaultKind::ContractViolation;
        }

        code = Slots(result)[0];
    }

    call.s[0] = code != 0 ? 1 : 0;
    call.s[1] = code != 0 ? code - 1 : 0;
    return VmFaultKind::None;
}

// ---- Random ----

uint64 Next64(RandomStream& random) noexcept {
    const uint64 high = random.NextUInt32();
    return (high << 32) | random.NextUInt32();
}

VmFaultKind RandomInt(C& call) noexcept {
    // [min, maxExclusive) 균등(거부 표본 — 64비트 범위). min >= maxExclusive는 AssertFailed.
    const int64 minimum = call.Integer(0);
    const int64 maximum = call.Integer(1);
    if (minimum >= maximum) {
        return VmFaultKind::AssertFailed;
    }

    const uint64 range = uint64(maximum) - uint64(minimum);
    const uint64 threshold = (uint64(0) - range) % range;
    RandomStream& random = VmLibrary::Random(call);
    uint64 value = Next64(random);
    while (value < threshold) {
        value = Next64(random);
    }

    call.s[0] = uint64(minimum) + value % range;
    return VmFaultKind::None;
}

VmFaultKind RandomFloat(C& call) noexcept {
    const double minimum = call.Real(0);
    const double maximum = call.Real(1);
    if (minimum > maximum) {
        return VmFaultKind::AssertFailed;
    }

    double value = minimum + (maximum - minimum) * VmLibrary::Random(call).NextDouble01();
    if (call.ResultNode().code == VmTypeCode::F32) {
        value = static_cast<double>(static_cast<float>(value));
    }

    if (!math::IsFinite(value)) {
        return VmFaultKind::NonFinite;
    }

    call.s[0] = VmBits(value);
    return VmFaultKind::None;
}

VmFaultKind RandomChance(C& call) noexcept {
    call.s[0] = VmLibrary::Random(call).NextDouble01() < call.Real(0) ? 1 : 0;
    return VmFaultKind::None;
}

constexpr VmLibraryEntry VmCollectionTable[] = {
    {"Array.Count", "A<T>>i64", ArrayCount},
    {"Array.Add", "A<T>,T>void", ArrayAdd},
    {"Array.Insert", "A<T>,i64,T>void", ArrayInsert},
    {"Array.RemoveAt", "A<T>,i64>void", ArrayRemoveAt},
    {"Array.Clear", "A<T>>void", ClearAll},
    {"Array.Reverse", "A<T>>void", ArrayReverse},
    {"Array.Contains", "A<T>,T>b", ArrayContains},
    {"Array.IndexOf", "A<T>,T>?<i64>", ArrayIndexOf},
    {"Array.Remove", "A<T>,T>b", ArrayRemove},
    {"Array.Sort", "A<O>>void", ArraySort},
    {"Map.Count", "M<T,U>>i64", ArrayCount},
    {"Map.ContainsKey", "M<T,U>,T>b", MapContainsKey},
    {"Map.Remove", "M<T,U>,T>b", KeyedRemove},
    {"Map.Keys", "M<T,U>>A<T>", MapKeys},
    {"Map.Values", "M<T,U>>A<U>", MapValues},
    {"Map.Clear", "M<T,U>>void", ClearAll},
    {"Set.Count", "S<T>>i64", ArrayCount},
    {"Set.Add", "S<T>,T>b", SetAdd},
    {"Set.Remove", "S<T>,T>b", KeyedRemove},
    {"Set.Contains", "S<T>,T>b", MapContainsKey},
    {"Set.Clear", "S<T>>void", ClearAll},
    {"Result.Value", "R<T>>?<T>", ResultValue},
    {"Result.Error", "R<T>>?<e>", ResultError},
    {"Random.Int", "i64,i64>i64", RandomInt},
    {"Random.Float", "F,F>F", RandomFloat},
    {"Random.Chance", "F>b", RandomChance},
};

} // namespace

Span<const VmLibraryEntry> VmCollectionEntries() noexcept {
    return Span<const VmLibraryEntry>(VmCollectionTable,
                                      sizeof(VmCollectionTable) / sizeof(VmCollectionTable[0]));
}

} // namespace rocks::sparks::detail
