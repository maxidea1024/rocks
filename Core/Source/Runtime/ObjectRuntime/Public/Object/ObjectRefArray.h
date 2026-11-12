#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Object/RuntimeTypeOps.h>
#include <Platform/Fatal.h>

namespace rocks {

class ObjectRegistry;

namespace detail {

// ObjectRefArray의 타입 없는 저장소다. Registry만 원소를 바꾼다.
struct ObjectRefArrayStorage {
    Allocator* allocator = nullptr;
    ObjectHandle* data = nullptr;
    uint32 size = 0;
    uint32 capacity = 0;
};

} // namespace detail

// 객체 안에 직접 포함하는 가변 길이 강한 GC 간선 목록이다(ObjectRef의 배열판). 원소는 T 또는 T의
// 파생 객체 handle이며 null은 담지 않는다. 같은 대상을 여러 번 담을 수 있다.
//
// 사용 예:
//   class Actor : public Object {
//   public:
//       explicit Actor(Allocator& allocator) noexcept : components_(allocator) {
//       }
//       void Trace(TraceVisitor& visitor) const noexcept {
//           components_.Trace(visitor);                  // 모든 원소를 GC에 알린다
//       }
//       ObjectRefArray<Component> components_;
//   };
//
//   registry.AddReference(access, actor, actorPointer->components_, component);   // 강한 간선 추가
//   registry.RemoveReference(access, actor, actorPointer->components_, component);
//   for (ObjectHandle handle : actorPointer->components_.View()) { ... }
//
// 계약:
// - 소유 객체의 저장소 안에 직접 멤버로 둔다(배열 객체 자체를 heap에 따로 두지 않는다). 원소 버퍼는
//   생성자에 준 allocator(보통 Registry allocator)에서 할당하며 그 allocator가 소유 객체보다 오래
//   살아야 한다.
// - 변경은 ObjectRegistry의 AddReference·RemoveReference·RemoveReferenceAt·ClearReferences로만
//   하며, 호출 동안 소유 객체의 Borrow를 쥐고 있어야 한다(배열 포인터를 얻은 그 Borrow).
// - 추가는 대상 타입·생존·같은 Registry를 검증하고, 증분 GC Marking 중이면 대상을 shade한 뒤
//   게시한다(삽입 장벽). 소유자의 Trace는 Trace(visitor)로 모든 원소를 조건 없이 방문해야 한다.
// - 읽기(Size·operator[]·View)는 소유자 Borrow 범위에서 소유 스레드가 한다. View의 Span은 다음
//   변경까지만 유효하다(추가가 버퍼를 재배치할 수 있다).
// - 복사·이동은 소유자 검증을 우회하므로 금지한다. 원소 버퍼는 소유 객체 소멸(저장소 회수) 때
//   반환한다.
template <class T = Object>
class ObjectRefArray {
public:
    explicit ObjectRefArray(Allocator& allocator) noexcept {
        storage_.allocator = &allocator;
    }

    ObjectRefArray(const ObjectRefArray&) = delete;
    ObjectRefArray& operator=(const ObjectRefArray&) = delete;
    ObjectRefArray(ObjectRefArray&&) = delete;
    ObjectRefArray& operator=(ObjectRefArray&&) = delete;

    ~ObjectRefArray() {
        if (storage_.data != nullptr && !storage_.allocator->Deallocate(storage_.data)) {
            platform::Fatal("ObjectRefArray storage deallocation failed");
        }
    }

    usize Size() const noexcept {
        return storage_.size;
    }

    bool IsEmpty() const noexcept {
        return storage_.size == 0;
    }

    // 범위 밖 index는 Fatal이다.
    ObjectHandle operator[](usize index) const noexcept {
        if (index >= storage_.size) {
            platform::Fatal("ObjectRefArray index out of range");
        }
        return storage_.data[index];
    }

    Span<const ObjectHandle> View() const noexcept {
        return Span<const ObjectHandle>(storage_.data, storage_.size);
    }

    // 첫 번째로 handle과 같은 원소의 위치다. 없으면 NotFound 값(Size())을 반환한다.
    usize IndexOf(ObjectHandle handle) const noexcept {
        for (uint32 index = 0; index < storage_.size; ++index) {
            if (storage_.data[index] == handle) {
                return index;
            }
        }
        return storage_.size;
    }

    bool Contains(ObjectHandle handle) const noexcept {
        return IndexOf(handle) != storage_.size;
    }

    // 소유자의 Trace에서 호출해 모든 원소를 강한 간선으로 방문한다.
    void Trace(TraceVisitor& visitor) const noexcept {
        for (uint32 index = 0; index < storage_.size; ++index) {
            visitor.Visit(storage_.data[index]);
        }
    }

private:
    friend class ObjectRegistry;

    detail::ObjectRefArrayStorage storage_;
};

} // namespace rocks
