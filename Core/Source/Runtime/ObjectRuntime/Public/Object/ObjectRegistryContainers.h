#pragma once

#include <Object/ObjectRegistry.h>

// ObjectRegistry 컨테이너·참조 변경 템플릿의 클래스 밖 정의다. 선언과 사용 규칙(목적·오류·수명·
// 사용 예)은 Object/ObjectRegistry.h의 클래스에 있고, 이 헤더는 그 끝에서 include된다. 정의는
// SetReference, ObjectRefArray 변경(AddReference…ClearReferences), StructRefArray 변경
// (AddElement…SetElementReference), ConstructionContext가 거치는 staged 지점과 경로 접근의 원소
// 참조 쓰기(private), WeakRef<T>::TryPin 순서다.
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
