#pragma once

#include <Object/GeneratedPathAccess.h>

// Registry 컨테이너 준비 사본(리플렉션 B9i3c3 — Object/PreparedReferences.h,
// Docs/Design/ReflectionExtensions.ko.md 10.2절)의 생성 thunk 템플릿이다. 선언은
// Object/GeneratedPathAccess.h에 있고 이 헤더는 그 끝에서 include된다. 시작 위치에 사본이 걸린 경로
// 연산이 ObjectRefArray·ObjectRefMap에 닿으면 처음 닿을 때 컨테이너를 사본에 묶고, 그 뒤 연산은
// 객체 대신 사본을 읽고 바꾼다. 대상의 스키마 타입·생존 검증은 경로 실행기가 끝냈고 여기서는 C++
// 대상 타입을 정적 대조한다(CheckWeakTarget — 강한 대상도 같은 검사다).
namespace rocks::detail {

// ObjectRefArray<T> 준비 사본의 연산 표다(사본은 handle 목록).
template <class T>
struct ReferenceArrayStage {
    static void Publish(ObjectBatchPublish& batch, void* container,
                        PreparedReferences& stage) noexcept {
        batch.ReplaceReferences(*static_cast<ObjectRefArray<T>*>(container),
                                PreparedReferencesAccess::Buffer(stage));
    }

    static Result<void> PublishStaged(ConstructionContext& transaction, ObjectHandle owner,
                                      void* container, PreparedReferences& stage) noexcept {
        return transaction.AssignReferences(owner, *static_cast<ObjectRefArray<T>*>(container),
                                            PreparedReferencesAccess::Elements(stage).View());
    }

    // 대상 타입 키는 NativeTypeKey<T>()다. 리플렉션 클래스의 키는 생성 .cpp가 정의해 상수식이
    // 아니므로 처음 쓸 때 한 번 만든다(Docs/Design/BuildTargets 3.8절 BS8b).
    static const ReferenceStageOps& Ops() noexcept {
        static const ReferenceStageOps ops{NativeTypeKey<T>(), nullptr, nullptr, &Publish,
                                           &PublishStaged};
        return ops;
    }
};

// ObjectRefMap<K, T> 준비 사본의 연산 표다(사본은 컨테이너 allocator의 OrderedMap<K,
// ObjectHandle>).
template <class K, class T>
struct ReferenceMapStage {
    using Map = typename ObjectRefMap<K, T>::MapType;

    static void DestroyClone(void* clone) noexcept {
        Map* map = static_cast<Map*>(clone);
        Allocator& allocator = map->GetAllocator();
        map->~Map();
        if (!allocator.Deallocate(clone)) {
            platform::Fatal("Prepared reference map deallocation failed");
        }
    }

    static Result<void> Collect(const void* clone, Array<ObjectHandle>& out) noexcept {
        const Map& map = *static_cast<const Map*>(clone);
        if (auto reserved = out.TryReserve(out.Size() + map.Count()); !reserved) {
            return reserved;
        }

        map.ForEach(
            [&out](const K&, const ObjectHandle& value) noexcept { (void)out.TryPushBack(value); });
        return Result<void>::Success();
    }

    static void Publish(ObjectBatchPublish& batch, void* container,
                        PreparedReferences& stage) noexcept {
        batch.ReplaceMapReferences(*static_cast<ObjectRefMap<K, T>*>(container),
                                   *static_cast<Map*>(PreparedReferencesAccess::Clone(stage)));
    }

    // 트랜잭션의 AssignMapReferences는 키 목록을 받으므로 키 사본을 컨테이너 allocator로 만든다.
    static Result<void> PublishStaged(ConstructionContext& transaction, ObjectHandle owner,
                                      void* container, PreparedReferences& stage) noexcept {
        const Map& map = *static_cast<const Map*>(PreparedReferencesAccess::Clone(stage));
        ObjectRefMap<K, T>& target = *static_cast<ObjectRefMap<K, T>*>(container);
        Allocator& allocator = target.GetAllocator();
        Array<K> keys(allocator);
        Array<ObjectHandle> values(allocator);
        if (auto reserved = keys.TryReserve(map.Count()); !reserved) {
            return reserved;
        }

        if (auto reserved = values.TryReserve(map.Count()); !reserved) {
            return reserved;
        }

        ErrorCode failure = ErrorCode::None;
        map.ForEach([&](const K& key, const ObjectHandle& value) noexcept {
            if (failure != ErrorCode::None) {
                return;
            }

            auto made = ObjectRefMapKey<K>::Make(allocator, key);
            if (!made) {
                failure = made.Error();
                return;
            }

            (void)keys.TryPushBack(Move(made.Value()));
            (void)values.TryPushBack(value);
        });
        if (failure != ErrorCode::None) {
            return Result<void>::Failure(failure);
        }

        return transaction.AssignMapReferences(owner, target, keys.View(), values.View());
    }

    // 대상 타입 키는 ReferenceArrayStage::Ops와 같이 처음 쓸 때 NativeTypeKey<T>()로 만든다.
    static const ReferenceStageOps& Ops() noexcept {
        static const ReferenceStageOps ops{NativeTypeKey<T>(), &DestroyClone, &Collect, &Publish,
                                           &PublishStaged};
        return ops;
    }
};

inline ObjectHandle NormalizedReference(ObjectHandle handle) noexcept {
    return handle.IsNull() ? ObjectHandle{} : handle;
}

// ObjectRefArray의 준비 사본 단계다: 원소 수·비우기(index 없는 끝), 원소 읽기·쓰기·넣기·지우기를
// 사본(handle 목록)에서 한다. 오류 규칙은 객체 갈래(PathContainer)와 같다.
template <class T>
Result<void> PathStagedReferenceArray(PathCursor& cursor, ObjectRefArray<T>& array) noexcept {
    if (auto bound = cursor.BindReferences(&array, ReferenceArrayStage<T>::Ops(),
                                           array.GetAllocator(), array.View(), nullptr);
        !bound) {
        return bound;
    }

    Array<ObjectHandle>& elements = PreparedReferencesAccess::Elements(*cursor.StagedReferences());
    const PathFrame& frame = cursor.Frame();
    const PathOperation operation = cursor.Operation();
    if (frame.selector == PathSelector::None) {
        if (operation == PathOperation::Length) {
            cursor.SetLength(elements.Size());
            return Result<void>::Success();
        }

        if (operation == PathOperation::Clear) {
            elements.Clear();
            return Result<void>::Success();
        }

        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
    }

    if (!cursor.IsLast()) {
        return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct);
    }

    if (auto valid = cursor.CheckIndex(elements.Size()); !valid) {
        return valid;
    }

    const usize index = frame.index;
    switch (operation) {
    case PathOperation::Read: {
        FieldValue candidate(cursor.Output()->GetAllocator());
        candidate.kind = ValueKind::ObjectReference;
        candidate.reference = elements[index];
        return cursor.PublishRead(candidate);
    }
    case PathOperation::Write: {
        if (auto checked = cursor.CheckWeakTarget(NativeTypeKey<T>()); !checked) {
            return checked;
        }

        const ObjectHandle previous = elements[index];
        elements[index] = NormalizedReference(cursor.Input()->reference);
        cursor.PublishReference(previous);
        return Result<void>::Success();
    }
    case PathOperation::Insert: {
        if (auto fits = cursor.CheckLength(elements.Size() + 1); !fits) {
            return fits;
        }

        if (auto checked = cursor.CheckWeakTarget(NativeTypeKey<T>()); !checked) {
            return checked;
        }

        if (auto inserted =
                elements.TryInsert(NormalizedReference(cursor.Input()->reference), index);
            !inserted) {
            return cursor.Fail(inserted.Error());
        }

        return Result<void>::Success();
    }
    case PathOperation::Remove: {
        const ObjectHandle previous = elements[index];
        (void)elements.TryRemoveAt(index);
        cursor.PublishReference(previous);
        return Result<void>::Success();
    }
    default:
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::IndexNotAllowed);
    }
}

// ObjectRefMap의 준비 사본 단계다: 처음 닿으면 Map을 컨테이너 allocator로 깊게 복사해 묶는다(게시된
// 객체는 고정 아래). 원소 수·비우기(key 없는 끝), 조회·읽기·쓰기(없던 키는 객체 갈래와 같은 넣기
// 준비 — 길이·ClassRef 키 lease·Name 키 intern·정적 키 검사 — 뒤 끝에 넣는다)·지우기를 사본에서
// 한다. 오류 규칙은 객체 갈래(PathObjectRefMap)와 같다.
template <class K, class T>
Result<void> PathStagedReferenceMap(PathCursor& cursor, ObjectRefMap<K, T>& map) noexcept {
    using Stage = ReferenceMapStage<K, T>;
    using Map = typename Stage::Map;
    PreparedReferences& stage = *cursor.StagedReferences();
    void* clone = nullptr;
    if (!stage.IsReady()) {
        if (auto pinned = cursor.Pin(); !pinned) {
            return pinned;
        }

        Allocator& allocator = map.GetAllocator();
        auto memory = allocator.Allocate(sizeof(Map), alignof(Map));
        if (!memory) {
            return cursor.Fail(memory.Error());
        }

        Map* made = ::new (memory.Value()) Map(allocator);
        if (auto copied = ValueCopy<Map>::Copy(*made, map.View(), allocator); !copied) {
            Stage::DestroyClone(made);
            return cursor.Fail(copied.Error());
        }

        clone = made;
    }

    if (auto bound = cursor.BindReferences(&map, Stage::Ops(), map.GetAllocator(), {}, clone);
        !bound) {
        return bound;
    }

    Map& entries = *static_cast<Map*>(PreparedReferencesAccess::Clone(stage));
    const PathFrame& frame = cursor.Frame();
    const PathOperation operation = cursor.Operation();
    if (frame.selector == PathSelector::None) {
        if (operation == PathOperation::Length) {
            cursor.SetLength(entries.Count());
            return Result<void>::Success();
        }

        if (operation == PathOperation::Clear) {
            entries.Clear();
            return Result<void>::Success();
        }

        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
    }

    if constexpr (ReflectedEnumField<K>::Supported) {
        if (!GeneratedType<K>::IsDeclared(frame.key.signedInteger)) {
            return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidKey);
        }
    }

    if (!cursor.IsLast()) {
        return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct);
    }

    const auto lookup = PathLookupKey<K>(frame);
    ObjectHandle* found = frame.keyFound ? entries.Find(lookup) : nullptr;
    switch (operation) {
    case PathOperation::Contains:
        cursor.SetLength(found ? 1 : 0);
        return Result<void>::Success();
    case PathOperation::EraseKey: {
        if (!found) {
            return cursor.FailMissingKey();
        }

        const ObjectHandle previous = *found;
        (void)entries.Erase(lookup);
        cursor.PublishReference(previous);
        return Result<void>::Success();
    }
    case PathOperation::Read: {
        if (!found) {
            return cursor.FailMissingKey();
        }

        FieldValue candidate(cursor.Output()->GetAllocator());
        candidate.kind = ValueKind::ObjectReference;
        candidate.reference = *found;
        return cursor.PublishRead(candidate);
    }
    case PathOperation::Write: {
        if (auto checked = cursor.CheckWeakTarget(NativeTypeKey<T>()); !checked) {
            return checked;
        }

        const ObjectHandle target = NormalizedReference(cursor.Input()->reference);
        if (found) {
            const ObjectHandle previous = *found;
            *found = target;
            cursor.PublishReference(previous);
            return Result<void>::Success();
        }

        if (auto begun = BeginKeyInsertion<K>(cursor, entries); !begun) {
            return begun;
        }

        auto key = MakePathKey<K>(entries.GetAllocator(), cursor.Frame());
        if (!key) {
            return cursor.Fail(key.Error());
        }

        if (auto inserted = entries.Insert(Move(key.Value()), target); !inserted) {
            return cursor.Fail(inserted.Error());
        }

        cursor.ReleasePin();
        if (FieldValue* previous = cursor.Output()) {
            FieldValue captured(previous->GetAllocator());
            captured.kind = ValueKind::ObjectReference;
            captured.present = false;
            cursor.PublishOutput(captured);
        }

        return Result<void>::Success();
    }
    default:
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::KeyRequired);
    }
}

} // namespace rocks::detail
