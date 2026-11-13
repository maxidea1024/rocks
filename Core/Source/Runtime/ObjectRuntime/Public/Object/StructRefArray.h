#pragma once

#include <Core/Memory/Allocator.h>
#include <Meta/GeneratedType.h>
#include <Object/RuntimeTypeOps.h>
#include <Platform/Fatal.h>
#include <new>

namespace rocks {

class ObjectRegistry;

template <class S>
class StructRefArray;

namespace detail {

// StructRefArray 원소 타입 하나의 생성·소멸 정보다(타입별 정적 값 StructElementOpsOf<S>::Value).
// trace는 넣지 않는다: owner 생성자에서 이 표가 인스턴스화될 때 GeneratedType<S>::Trace까지
// 인스턴스화되면 생성 헤더가 없는 곳에서 컴파일이 깨진다(Trace는 StructRefArray::Trace가 직접
// 부른다).
struct StructElementOps {
    usize size = 0;
    usize alignment = 0;
    // memory(size·alignment 블록)에 기본 원소를 만든다. noexcept S(Allocator&)가 있으면 allocator를
    // 넘기고 없으면 noexcept S()다.
    void (*construct)(void* memory, Allocator& allocator) noexcept = nullptr;
    void (*destroy)(void* memory) noexcept = nullptr;
};

template <class S>
struct StructElementOpsOf {
    static_assert(
        __is_nothrow_constructible(S, Allocator&) || __is_nothrow_constructible(S),
        "StructRefArray elements need `explicit S(Allocator&) noexcept` or `S() noexcept`");
    static_assert(__is_nothrow_destructible(S),
                  "StructRefArray elements need a noexcept destructor");

    static void Construct(void* memory, Allocator& allocator) noexcept {
        if constexpr (__is_constructible(S, Allocator&)) {
            ::new (memory) S(allocator);
        } else {
            ::new (memory) S();
        }
    }

    static void Destroy(void* memory) noexcept {
        static_cast<S*>(memory)->~S();
    }

    static constexpr StructElementOps Value{sizeof(S), alignof(S), &Construct, &Destroy};
};

// StructRefArray의 타입 없는 저장소다. 원소마다 블록 하나(주소 고정)이고 blocks는 그 포인터
// 색인이다. Registry만 바꾼다.
struct StructRefArrayStorage {
    Allocator* allocator = nullptr;
    const StructElementOps* element = nullptr;
    void** blocks = nullptr;
    uint32 size = 0;
    uint32 capacity = 0;
};

// 컨테이너 원소 하나의 위치다(원소 안 참조 쓰기의 범위 검사 — ObjectRegistry::SetEdge). 같은
// 컨테이너가 바뀌기 전까지만 뜻이 있다.
struct StructElementLocation {
    const StructRefArrayStorage* storage = nullptr;
    usize index = 0;
};

// codec이 원소 위치를 만들고 저장소를 읽게 한다(변경은 Registry만 한다).
struct StructRefArrayAccess {
    template <class S>
    static StructElementLocation Location(const StructRefArray<S>& array, usize index) noexcept {
        return {&array.storage_, index};
    }

    template <class S>
    static const StructRefArrayStorage& Storage(const StructRefArray<S>& array) noexcept {
        return array.storage_;
    }
};

} // namespace detail

// 객체 안에 직접 포함하는 참조 struct(ObjectRef 멤버를 가진 리플렉션 struct) 목록이다. ObjectRef는
// 이동할 수 없고 Registry가 참조 주소를 범위로 검사하므로, 원소마다 블록 하나를 두어 원소 주소를
// 고정한다(삽입·삭제·성장은 포인터 색인만 옮긴다). 원소 주소는 그 원소의 Remove·Clear·Assign과
// owner 소멸 때만 무효가 된다.
//
// 사용 예:
//   class Mesh : public Object {
//   public:
//       explicit Mesh(Allocator& allocator) noexcept : materials(allocator) {
//       }
//       void Trace(TraceVisitor& visitor) const noexcept {
//           materials.Trace(visitor);                    // 원소마다 GeneratedType<S>::Trace
//       }
//       StructRefArray<MaterialSlot> materials;
//   };
//
//   auto index = registry.AddElement(access, mesh, meshPointer->materials); // 기본 원소 추가
//   if (!index) {
//       return Result<void>::Failure(index.Error());
//   }
//
//   MaterialSlot& slot = meshPointer->materials[index.Value()];
//   slot.slotName = 3; // 비참조 멤버는 Borrow 안에서 직접 쓴다
//   return registry.SetElementReference(access, mesh, meshPointer->materials, index.Value(),
//                                       slot.material, material);
//
// 계약:
// - 소유 객체의 저장소 안에 직접 멤버로 둔다(클래스 필드나 인라인 struct 멤버). 블록·색인은
// 생성자에
//   준 allocator(보통 Registry allocator)에서 할당하며 그 allocator가 소유 객체보다 오래 살아야
//   한다.
// - 원소 추가·삽입·삭제·비우기와 원소 안 참조 쓰기는 ObjectRegistry(AddElement·InsertElementAt·
//   RemoveElementAt·ClearElements·SetElementReference)나 생성 트랜잭션(ConstructionContext::
//   AssignElements·SetElementReference)으로만 한다. live 변경은 owner의 Borrow를 쥔 채 호출한다.
// - 비참조 멤버는 owner Borrow 안에서 operator[]로 직접 읽고 쓴다(소유 스레드).
// - 원소 S는 ObjectRefArray·StructRefArray를 (중첩까지) 담지 않는다(원소 블록이 owner 저장소 밖이라
//   Registry가 그 컨테이너의 변경을 거부한다).
// - 복사·이동은 소유자 검증을 우회하므로 금지한다.
template <class S>
class StructRefArray {
public:
    explicit StructRefArray(Allocator& allocator) noexcept {
        storage_.allocator = &allocator;
        storage_.element = &detail::StructElementOpsOf<S>::Value;
    }

    StructRefArray(const StructRefArray&) = delete;
    StructRefArray& operator=(const StructRefArray&) = delete;
    StructRefArray(StructRefArray&&) = delete;
    StructRefArray& operator=(StructRefArray&&) = delete;

    // 원소를 역순으로 소멸하고 블록·색인을 반환한다.
    ~StructRefArray() {
        for (uint32 remaining = storage_.size; remaining != 0; --remaining) {
            void* block = storage_.blocks[remaining - 1];
            storage_.element->destroy(block);
            if (!storage_.allocator->Deallocate(block)) {
                platform::Fatal("StructRefArray element deallocation failed");
            }
        }

        if (storage_.blocks != nullptr && !storage_.allocator->Deallocate(storage_.blocks)) {
            platform::Fatal("StructRefArray index deallocation failed");
        }
    }

    usize Size() const noexcept {
        return storage_.size;
    }

    bool IsEmpty() const noexcept {
        return storage_.size == 0;
    }

    // 범위 밖 index는 Fatal이다. 참조 멤버(ObjectRef)는 대입할 수 없으므로 Registry로 쓴다.
    S& operator[](usize index) noexcept {
        if (index >= storage_.size) {
            platform::Fatal("StructRefArray index out of range");
        }

        return *static_cast<S*>(storage_.blocks[index]);
    }

    const S& operator[](usize index) const noexcept {
        if (index >= storage_.size) {
            platform::Fatal("StructRefArray index out of range");
        }

        return *static_cast<const S*>(storage_.blocks[index]);
    }

    // 소유자의 Trace에서 호출해 원소마다 GeneratedType<S>::Trace로 참조를 방문한다. 멤버 템플릿이라
    // 생성 헤더가 있는 곳(생성 Trace)에서만 인스턴스화된다.
    template <class Visitor>
    void Trace(Visitor& visitor) const noexcept {
        for (uint32 index = 0; index < storage_.size; ++index) {
            GeneratedType<S>::Trace(*static_cast<const S*>(storage_.blocks[index]), visitor);
        }
    }

private:
    friend class ObjectRegistry;
    friend struct detail::StructRefArrayAccess;

    detail::StructRefArrayStorage storage_;
};

} // namespace rocks
