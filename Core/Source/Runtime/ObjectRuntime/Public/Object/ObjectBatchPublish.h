#pragma once

#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

// 객체 하나의 여러 필드 변경을 한 번에 게시하는 묶음 게시 구간이다(리플렉션 B9i2 —
// Docs/Design/ReflectionExtensions.ko.md 10.2절). 텍스트 대입 묶음(ReflectionText의
// PreparedAssignments::Publish)이 Prepare에서 만든 준비 단위를 게시할 때 쓴다:
//   1. Prepare: 지금 값 위에서 새 값을 만든다(값 부분 트리는 사본, 단일 참조 위치는 (위치, 옛 대상,
//      새 대상), Registry 컨테이너는 새 버퍼). 그때 ObjectRegistry::MutationStamp로 owner의 변경
//      표지를 읽어 두고 새 Hard·Weak 대상을 RootRef(TryPin)로 고정한다.
//   2. 게시 직전: owner의 읽기 Borrow(Resolve — ResolveForWrite는 표지를 올린다)를 쥐고
//      ObjectRegistry::BeginBatchPublish(표지, 새 대상, 간선 변경)로 구간에 들어간다. 대상
//      생존·타입 재확인과 관찰자 공간 예약이 여기서 끝나므로 실패는 모두 이 단계에서 나고 그때
//      아무것도 바뀌지 않는다.
//   3. 구간 안: 아래 Set*·Replace* 게시는 할당하지 않고 실패하지 않는다(장벽 shade는 미리 잡은 GC
//      작업 용량을 쓴다). 값 부분 트리는 호출자가 직접 옮긴다(구간 안에는 콜백이 없다).
//   4. End(또는 소멸): 관찰자 기록을 한 번에 적용하고 owner의 변경 표지를 올린 뒤 구간을 나온다. 옛
//      버퍼(PreparedReferenceArray·준비한 Map에 넘어온 옛 내용)는 그 뒤 호출자가 반환한다.
//
// 사용 예(owner의 ObjectRef 필드 하나와 ObjectRefArray 하나를 함께 바꾼다):
//   const uint64 stamp = registry.MutationStamp(access, owner).Value();      // Prepare 때
//   ...
//   auto borrowed = registry.Resolve<Node>(access, owner);
//   const BatchTargetCheck targets[] = {ExpectBatchTarget<Node>(child),
//   ExpectBatchTarget<Node>(leaf)}; const BatchEdgeChange edges[] = {{oldNext, child},
//   {ObjectHandle{}, leaf}}; auto batch = registry.BeginBatchPublish(access, owner, stamp, targets,
//   edges); if (!batch) {
//       return Result<void>::Failure(batch.Error());   // Busy·StaleHandle·TypeMismatch·OOM 등
//   }
//
//   batch.Value().SetReference(node->next, child);
//   batch.Value().ReplaceReferences(node->children, prepared);   // prepared = {leaf}
//   batch.Value().End();
namespace rocks {

namespace detail {

// ObjectRefMap 옛 내용(OrderedMap<K, ObjectHandle>)에 value가 있는지다(묶음 게시의 retained —
// 재확인에 실패한 값만 찾는다, 리플렉션 B9i5).
template <class K>
bool BatchMapHolds(const void* map, ObjectHandle value) noexcept {
    bool found = false;
    static_cast<const OrderedMap<K, ObjectHandle>*>(map)->ForEach(
        [&found, value](const K&, const ObjectHandle& old) noexcept {
            found = found || old == value;
        });
    return found;
}

} // namespace detail

// 새 대상 하나의 재확인이다. target은 non-null이고 nativeType은 기대 타입(NativeTypeKey<T>() —
// nullptr면 타입을 보지 않는다)이다. Hard(ObjectRef·컨테이너 원소)와 Weak(WeakObjectRef) 새 대상을
// 모두 넣는다.
struct BatchTargetCheck {
    ObjectHandle target{};
    const void* nativeType = nullptr;
};

// T(또는 파생) 대상의 재확인 항목을 만든다.
template <class T>
BatchTargetCheck ExpectBatchTarget(ObjectHandle target) noexcept {
    return {target, NativeTypeKey<T>()};
}

// 강한 참조 위치 하나의 간선 변경(removed → added, 어느 쪽이든 null 가능)이다. 컨테이너 교체는
// 뺀 원소마다 {원소, null}, 더한 원소마다 {null, 원소}로 적는다(그대로 남는 원소는 적지 않아도
// 된다). 관찰자 예약과 기록이 이 목록으로 정해지므로, 구간 안의 게시가 실제로 바꾼 간선과 같아야
// 한다(End가 검사합으로 대조한다 — 다르면 Fatal).
struct BatchEdgeChange {
    ObjectHandle removed{};
    ObjectHandle added{};
};

// 묶음 게시로 ObjectRefArray에 넣을 새 원소 버퍼다. 구간 밖에서 만들고(할당은 여기서만 한다)
// ObjectBatchPublish::ReplaceReferences가 배열의 버퍼와 맞바꾼다. 맞바꾼 뒤에는 옛 버퍼를 들고
// 있다가 소멸할 때 반환한다(구간을 나온 뒤에 소멸시킨다). allocator는 바꿀 배열의 allocator와
// 같아야 한다. 원소의 대상 검증·장벽은 게시 때 Registry가 한다(여기서는 handle만 담는다).
//
// 사용 예:
//   PreparedReferenceArray prepared(allocator);
//   const ObjectHandle elements[] = {first, ObjectHandle{}, second};
//   if (auto assigned = prepared.TryAssign(elements); !assigned) {
//       return assigned;   // OutOfMemory·Overflow
//   }
class ROCKS_OBJECTRUNTIME_API PreparedReferenceArray {
public:
    explicit PreparedReferenceArray(Allocator& allocator) noexcept {
        storage_.allocator = &allocator;
    }

    ~PreparedReferenceArray();
    PreparedReferenceArray(const PreparedReferenceArray&) = delete;
    PreparedReferenceArray& operator=(const PreparedReferenceArray&) = delete;

    // 내용을 elements로 바꾼다(null은 ObjectHandle{}로 정규화한다). 원소 수가 uint32 상한을 넘으면
    // Overflow, 할당 실패는 그 오류이고 그때 내용은 그대로다.
    Result<void> TryAssign(Span<const ObjectHandle> elements) noexcept;

    // 지금 내용이다(맞바꾼 뒤에는 옛 배열의 원소).
    Span<const ObjectHandle> View() const noexcept {
        return Span<const ObjectHandle>(storage_.data, storage_.size);
    }

private:
    friend class ObjectRegistry;

    detail::ObjectRefArrayStorage storage_;
};

// 진행 중인 묶음 게시 구간이다(ObjectRegistry::BeginBatchPublish가 만든다). 이동만 되고, End나
// 소멸이 구간을 닫는다. 구간이 열린 동안 그 Registry의 다른 변경·Borrow·RootRef 복제·GC 단계·
// Discard는 Busy다(처리 표시). 게시 함수는 소유 스레드에서 부르고, owner 저장소 밖 위치·재확인하지
// 않은 대상·다른 allocator 같은 계약 위반은 Fatal이다(실패를 돌려주지 않는다 — 실패할 수 있는
// 검사는 BeginBatchPublish가 끝냈다). 아무것도 게시하지 않고 닫으면 관찰자 기록·표지 증가 없이
// 나온다.
class ROCKS_OBJECTRUNTIME_API ObjectBatchPublish {
public:
    ObjectBatchPublish() noexcept = default;

    ObjectBatchPublish(ObjectBatchPublish&& other) noexcept
        : registry_(other.registry_), owner_(other.owner_) {
        other.registry_ = nullptr;
        other.owner_ = {};
    }

    ObjectBatchPublish& operator=(ObjectBatchPublish&& other) noexcept {
        if (this != &other) {
            End();
            registry_ = other.registry_;
            owner_ = other.owner_;
            other.registry_ = nullptr;
            other.owner_ = {};
        }

        return *this;
    }

    ObjectBatchPublish(const ObjectBatchPublish&) = delete;
    ObjectBatchPublish& operator=(const ObjectBatchPublish&) = delete;

    ~ObjectBatchPublish() {
        End();
    }

    bool IsActive() const noexcept {
        return registry_ != nullptr;
    }

    ObjectHandle Owner() const noexcept {
        return owner_;
    }

    // owner 저장소 안의 ObjectRef 필드(인라인 struct 멤버·StaticArray 원소 포함)를 target으로
    // 바꾼다(null이면 비운다). target은 BeginBatchPublish에서 재확인한 생존 T여야 한다. Marking
    // 중이면 shade한다.
    template <class T>
    void SetReference(ObjectRef<T>& field, ObjectHandle target) noexcept {
        Registry().PublishBatchReference(field, target);
    }

    // StructRefArray index 원소 안의 참조 멤버 field를 target으로 바꾼다(규칙은 SetReference와 같고
    // field는 그 원소 블록 안이어야 한다).
    template <class S, class T>
    void SetElementReference(StructRefArray<S>& array, usize index, ObjectRef<T>& field,
                             ObjectHandle target) noexcept {
        Registry().PublishBatchElementReference(array, index, field, target);
    }

    // array의 원소 버퍼를 prepared와 맞바꾼다. 새 원소는 모두 재확인한 생존 T(또는 null)여야 하고
    // Marking 중이면 shade한다. prepared는 옛 원소를 받는다.
    template <class T>
    void ReplaceReferences(ObjectRefArray<T>& array, PreparedReferenceArray& prepared) noexcept {
        Registry().PublishBatchReferences(array, prepared);
    }

    // map의 내용을 prepared(같은 allocator의 OrderedMap<K, ObjectHandle> — 순회 중이면 안 된다)와
    // 맞바꾼다. 새 값은 모두 재확인한 생존 T(또는 null)여야 한다. prepared는 옛 내용을 받는다.
    template <class K, class T>
    void ReplaceMapReferences(ObjectRefMap<K, T>& map,
                              typename ObjectRefMap<K, T>::MapType& prepared) noexcept {
        Registry().PublishBatchMap(map, prepared);
    }

    // 관찰자 기록을 적용하고 owner의 변경 표지를 올린 뒤 구간을 닫는다. 닫힌 구간에서는 무동작이다.
    void End() noexcept;

private:
    friend class ObjectRegistry;
    friend class PreparedAttach;
    friend class PreparedReference;

    // 경로로 찾은 단일 참조 위치(element가 있으면 그 StructRefArray 원소 블록 안)를 target으로
    // 바꾼다(PreparedReference::Publish — 규칙은 SetReference와 같다).
    template <class T>
    void PublishReferenceAt(ObjectRef<T>& field, ObjectHandle target,
                            const detail::StructElementLocation* element) noexcept {
        Registry().PublishBatchReference(field, target, element);
    }

    // 값 부분 트리 자리 하나(location부터 bytes — element가 있으면 그 StructRefArray 원소 블록 안,
    // 없으면 owner 저장소 안)를 게시함을 기록한다(End가 변경 표지를 올린다). 교환은 호출자
    // (PreparedAttach::Publish)가 한다. 열린 구간·위치 검사 위반은 Fatal이다.
    void PublishValue(void* location, usize bytes,
                      const detail::StructElementLocation* element) noexcept {
        Registry().PublishBatchValue(location, bytes, element);
    }

    ObjectBatchPublish(ObjectRegistry* registry, ObjectHandle owner) noexcept
        : registry_(registry), owner_(owner) {
    }

    ObjectRegistry& Registry() const noexcept;

    ObjectRegistry* registry_ = nullptr;
    ObjectHandle owner_{};
};

template <class T>
void ObjectRegistry::PublishBatchReference(ObjectRef<T>& field, ObjectHandle target,
                                           const detail::StructElementLocation* element) noexcept {
    PublishBatchEdge(field.handle_, target, NativeTypeKey<T>(), element);
}

template <class S, class T>
void ObjectRegistry::PublishBatchElementReference(StructRefArray<S>& array, usize index,
                                                  ObjectRef<T>& field,
                                                  ObjectHandle target) noexcept {
    const detail::StructElementLocation location =
        detail::StructRefArrayAccess::Location(array, index);
    PublishBatchEdge(field.handle_, target, NativeTypeKey<T>(), &location);
}

template <class T>
void ObjectRegistry::PublishBatchReferences(ObjectRefArray<T>& array,
                                            PreparedReferenceArray& prepared) noexcept {
    PublishBatchArray(array.storage_, prepared.storage_, NativeTypeKey<T>());
}

template <class K, class T>
void ObjectRegistry::PublishBatchMap(ObjectRefMap<K, T>& map,
                                     OrderedMap<K, ObjectHandle>& prepared) noexcept {
    detail::ObjectRefMapStorage<K>& storage = map.storage_;
    CheckBatchContainer(&storage, sizeof(storage));
    if (&prepared.GetAllocator() != &storage.map.GetAllocator() || storage.map.IsIterating() ||
        prepared.IsIterating()) {
        platform::Fatal("Batch map replacement needs the same allocator and no iteration");
    }

    storage.map.ForEach(
        [this](const K&, ObjectHandle& value) noexcept { NoteBatchRemoved(value); });
    // 옛 내용에 있던 값은 이미 게시된 간선이라 재확인하지 않는다(리플렉션 B9i5 —
    // PublishBatchTarget).
    const void* nativeType = NativeTypeKey<T>();
    const void* old = &storage.map;
    prepared.ForEach([this, nativeType, old](const K&, ObjectHandle& value) noexcept {
        PublishBatchTarget(value, nativeType, &detail::BatchMapHolds<K>, old);
    });
    // 이동은 할당하지 않는다(같은 allocator의 저장소를 옮긴다).
    OrderedMap<K, ObjectHandle> previous(Move(storage.map));
    storage.map = Move(prepared);
    prepared = Move(previous);
    ++storage.revision;
}

} // namespace rocks
