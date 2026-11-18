#pragma once

#include <Object/ObjectRegistry.h>

// ObjectRegistry 컨테이너·참조 변경 템플릿의 클래스 밖 정의다. 선언과 사용 규칙(목적·오류·수명·
// 사용 예)은 Object/ObjectRegistry.h의 클래스에 있고, 이 헤더는 그 끝에서 include된다. 정의는
// SetReference, ObjectRefArray 변경(AddReference…ClearReferences), ObjectRefMap 변경
// (SetMapReference…ClearMapReferences), StructRefArray 변경(AddElement…SetElementReference),
// ConstructionContext가 거치는 staged 지점과 경로 접근의 원소 참조 쓰기(private),
// WeakRef<T>::TryPin 순서다.
namespace rocks {

template <class T>
Result<void> ObjectRegistry::SetReference(const ObjectAccessContext& access, ObjectHandle owner,
                                          ObjectRef<T>& field, ObjectHandle target) noexcept {
    return SetEdge(access, owner, field.handle_, target, 0, NativeTypeKey<T>());
}

template <class T>
Result<void> ObjectRegistry::AddReference(const ObjectAccessContext& access, ObjectHandle owner,
                                          ObjectRefArray<T>& array, ObjectHandle target) noexcept {
    return InsertArrayEdge(access, owner, array.storage_, 0, true, target, NativeTypeKey<T>());
}

template <class T>
Result<void> ObjectRegistry::InsertReferenceAt(const ObjectAccessContext& access,
                                               ObjectHandle owner, ObjectRefArray<T>& array,
                                               usize index, ObjectHandle target) noexcept {
    return InsertArrayEdge(access, owner, array.storage_, index, false, target, NativeTypeKey<T>());
}

template <class T>
Result<void> ObjectRegistry::SetReferenceAt(const ObjectAccessContext& access, ObjectHandle owner,
                                            ObjectRefArray<T>& array, usize index,
                                            ObjectHandle target) noexcept {
    return ReplaceArrayEdge(access, owner, array.storage_, index, target, NativeTypeKey<T>());
}

template <class T>
Result<void> ObjectRegistry::RemoveReferenceAt(const ObjectAccessContext& access,
                                               ObjectHandle owner, ObjectRefArray<T>& array,
                                               usize index) noexcept {
    return RemoveArrayEdge(access, owner, array.storage_, index, ObjectHandle{}, false);
}

template <class T>
Result<void> ObjectRegistry::RemoveReferenceAtSwap(const ObjectAccessContext& access,
                                                   ObjectHandle owner, ObjectRefArray<T>& array,
                                                   usize index) noexcept {
    return RemoveArrayEdge(access, owner, array.storage_, index, ObjectHandle{}, false, true);
}

template <class T>
Result<void> ObjectRegistry::RemoveReference(const ObjectAccessContext& access, ObjectHandle owner,
                                             ObjectRefArray<T>& array,
                                             ObjectHandle target) noexcept {
    return RemoveArrayEdge(access, owner, array.storage_, 0, target, true);
}

template <class T>
Result<void> ObjectRegistry::ClearReferences(const ObjectAccessContext& access, ObjectHandle owner,
                                             ObjectRefArray<T>& array) noexcept {
    return ClearArrayEdges(access, owner, array.storage_);
}

// ObjectRefMap 편집 객체(detail::ObjectRefMapEditor)는 이 함수의 지역이라 Registry 호출이 돌아온
// 뒤(잠금 밖) 소멸하며 준비한 키 사본·예약·지운 키·옛 항목을 반환한다.
template <class K, class T, class Q>
Result<void> ObjectRegistry::SetMapReference(const ObjectAccessContext& access, ObjectHandle owner,
                                             ObjectRefMap<K, T>& map, const Q& key,
                                             ObjectHandle target,
                                             MapReferenceWrite* write) noexcept {
    detail::ObjectRefMapEditor<K, Q> edit(map.storage_, &key);
    return SetMapEdge(access, owner, edit, target, NativeTypeKey<T>(), write);
}

template <class K, class T, class Q>
Result<void> ObjectRegistry::EraseMapReference(const ObjectAccessContext& access,
                                               ObjectHandle owner, ObjectRefMap<K, T>& map,
                                               const Q& key, ObjectHandle* removed) noexcept {
    detail::ObjectRefMapEditor<K, Q> edit(map.storage_, &key);
    return EraseMapEdge(access, owner, edit, removed);
}

template <class K, class T>
Result<void> ObjectRegistry::ClearMapReferences(const ObjectAccessContext& access,
                                                ObjectHandle owner,
                                                ObjectRefMap<K, T>& map) noexcept {
    detail::ObjectRefMapEditor<K, K> edit(map.storage_, nullptr);
    return ClearMapEdges(access, owner, edit);
}

template <class S>
Result<usize> ObjectRegistry::AddElement(const ObjectAccessContext& access, ObjectHandle owner,
                                         StructRefArray<S>& array) noexcept {
    return InsertStructElement(access, owner, array.storage_, 0, true);
}

template <class S>
Result<void> ObjectRegistry::InsertElementAt(const ObjectAccessContext& access, ObjectHandle owner,
                                             StructRefArray<S>& array, usize index) noexcept {
    auto inserted = InsertStructElement(access, owner, array.storage_, index, false);
    return inserted ? Result<void>::Success() : Result<void>::Failure(inserted.Error());
}

template <class S>
Result<void> ObjectRegistry::RemoveElementAt(const ObjectAccessContext& access, ObjectHandle owner,
                                             StructRefArray<S>& array, usize index) noexcept {
    return RemoveStructElement(access, owner, array.storage_, index);
}

template <class S>
Result<void> ObjectRegistry::ClearElements(const ObjectAccessContext& access, ObjectHandle owner,
                                           StructRefArray<S>& array) noexcept {
    return ClearStructElements(access, owner, array.storage_);
}

template <class S, class T>
Result<void> ObjectRegistry::SetElementReference(const ObjectAccessContext& access,
                                                 ObjectHandle owner, StructRefArray<S>& array,
                                                 usize index, ObjectRef<T>& field,
                                                 ObjectHandle target) noexcept {
    const detail::StructElementLocation element{&array.storage_, index};
    return SetEdge(access, owner, field.handle_, target, 0, NativeTypeKey<T>(), &element);
}

template <class T>
Result<void> ObjectRegistry::AssignReferencesStaged(const ObjectAccessContext& access,
                                                    ObjectHandle owner, ObjectRefArray<T>& array,
                                                    Span<const ObjectHandle> targets,
                                                    uint64 transaction) noexcept {
    return AssignArrayEdges(access, owner, array.storage_, targets, transaction,
                            NativeTypeKey<T>());
}

template <class K, class T>
Result<void> ObjectRegistry::AssignMapReferencesStaged(const ObjectAccessContext& access,
                                                       ObjectHandle owner, ObjectRefMap<K, T>& map,
                                                       Span<const K> keys,
                                                       Span<const ObjectHandle> targets,
                                                       uint64 transaction) noexcept {
    if (keys.Size() != targets.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::ObjectRefMapEditor<K, K> edit(map.storage_, nullptr, keys);
    return AssignMapEdges(access, owner, edit, targets, transaction, NativeTypeKey<T>());
}

template <class S>
Result<void> ObjectRegistry::AssignElementsStaged(const ObjectAccessContext& access,
                                                  ObjectHandle owner, StructRefArray<S>& array,
                                                  usize count, uint64 transaction) noexcept {
    return AssignStructElements(access, owner, array.storage_, count, transaction);
}

template <class S, class T>
Result<void> ObjectRegistry::SetElementReferenceStaged(const ObjectAccessContext& access,
                                                       ObjectHandle owner, StructRefArray<S>& array,
                                                       usize index, ObjectRef<T>& field,
                                                       ObjectHandle target,
                                                       uint64 transaction) noexcept {
    const detail::StructElementLocation element{&array.storage_, index};
    return SetEdge(access, owner, field.handle_, target, transaction, NativeTypeKey<T>(), &element);
}

template <class T>
Result<void>
ObjectRegistry::SetElementReference(const ObjectAccessContext& access, ObjectHandle owner,
                                    const detail::StructElementLocation& element,
                                    ObjectRef<T>& field, ObjectHandle target) noexcept {
    return SetEdge(access, owner, field.handle_, target, 0, NativeTypeKey<T>(), &element);
}

template <class T>
Result<RootRef<T>> WeakRef<T>::TryPin(ObjectRegistry& registry,
                                      const ObjectAccessContext& access) const noexcept {
    return registry.TryPin<T>(access, handle_);
}

} // namespace rocks
