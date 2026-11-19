#pragma once

#include <Base/Types.h>
#include <Object/ObjectIdentity.h>

// 참조 위치 표(M3, Docs/Design/GarbageCollectionScale.ko.md 3.4·5절)다. 생성 코드(또는 같은 모양의
// 수동 함수)가 타입의 강한 간선 칸 위치를 이 빌더에 알린다. Registry는 타입마다 첫 객체에서 한 번
// 표를 만들고, GC marker가 Trace 함수를 부르는 대신 표의 위치에서 handle을 바로 읽는다(배열 버퍼는
// 큐 앞쪽에서 미리 읽는다). offsetof는 표준 배치가 아닌 클래스에서 경고(오류)라 실제 객체의 멤버
// 주소 차이로 잰다(같은 타입의 모든 객체에서 같다).
namespace rocks::detail {

// 표 한 칸의 종류다.
// - Single: 그 위치가 ObjectHandle 하나다(ObjectRef<T> — 크기·배치가 ObjectHandle과 같다).
// - Array: 그 위치가 ObjectRefArrayStorage다(ObjectRefArray<T> — 원소 data·size를 읽는다).
enum class ReferenceSlotKind : uint8 {
    Single,
    Array
};

struct ReferenceSlot {
    uint32 offset = 0;
    ReferenceSlotKind kind = ReferenceSlotKind::Single;
};

// 표를 모으는 빌더다. 표로 나타낼 수 없는 필드(StructRefArray·ObjectRefMap·참조 struct·정적 배열)가
// 있으면 Unsupported를 불러 그 타입은 Trace로 남는다. 칸 수가 MaxSlots를 넘어도 Trace로 남는다.
class ReferenceLayoutBuilder {
public:
    static constexpr uint32 MaxSlots = 32;

    explicit ReferenceLayoutBuilder(const void* object) noexcept
        : base_(static_cast<const unsigned char*>(object)) {
    }

    // ObjectRef<T> 필드 하나다(field는 그 멤버의 주소).
    void Single(const void* field) noexcept {
        Add(field, ReferenceSlotKind::Single);
    }

    // ObjectRefArray<T> 필드 하나다(field는 그 멤버의 주소).
    void Array(const void* field) noexcept {
        Add(field, ReferenceSlotKind::Array);
    }

    void Unsupported() noexcept {
        supported_ = false;
    }

    bool Supported() const noexcept {
        return supported_;
    }

    uint32 Count() const noexcept {
        return count_;
    }

    const ReferenceSlot* Slots() const noexcept {
        return slots_;
    }

private:
    void Add(const void* field, ReferenceSlotKind kind) noexcept {
        const auto* at = static_cast<const unsigned char*>(field);
        if (count_ == MaxSlots || at < base_) {
            supported_ = false;
            return;
        }

        slots_[count_++] = {static_cast<uint32>(at - base_), kind};
    }

    const unsigned char* base_;
    ReferenceSlot slots_[MaxSlots];
    uint32 count_ = 0;
    bool supported_ = true;
};

} // namespace rocks::detail
