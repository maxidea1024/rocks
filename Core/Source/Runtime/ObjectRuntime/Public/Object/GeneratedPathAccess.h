#pragma once

#include <Base/TypeTraits.h>
#include <Object/GeneratedFieldAccess.h>
#include <Object/PathCursor.h>
#include <Object/ValueCopy.h>
#include <Platform/Fatal.h>

// 경로 접근(B6) 생성 thunk 템플릿이다(Docs/Design/ReflectionFoundations.ko.md 7.1절). MetaCompiler
// 생성 헤더가 include하고 그 안에서만 인스턴스화한다: 클래스 Struct·Array·Optional<값 struct>·
// Map·Set 필드의 함수표에 PathClassField, struct 멤버의 StructFieldOps 둘째 인자에
// PathStructMember를 낸다. 대상 종류는 ValueKind Target으로 명시하고(requires 탐지 형질을 쓰지
// 않는다 — 컨테이너는 부분 특수화 ArrayField·OptionalField·MapField, enum은 GeneratedType<E>를 직접
// 쓰므로 생성 헤더가 없으면 조용히 거짓이 아니라 컴파일 오류다), 정적 타입과 등록 스키마를 실행 시
// 대조해 void* 타입 혼동을 막는다. Map·Set의 key 단계(리플렉션 B9f3 —
// Docs/Design/ReflectionExtensions.ko.md 7.3절)는 PathKeyedContainer다. Serialization에 의존하지
// 않는다.
namespace rocks::detail {

// 경로의 참조 멤버 쓰기 단일 지점이다(7.1절): StructRefArray 원소 안이면 그 원소 위치의 live
// SetElementReference(컨테이너 기준 범위 검사 — 위치 오버로드는 ObjectRegistry의 private이고 이
// 함수만 friend다), 아니면 소유 객체 저장소 안 주소의 SetReference다.
// 비정규 null은 Registry가 ObjectHandle{}로 정규화한다.
template <class T>
Result<void> SetPathMemberReference(PathCursor& cursor, ObjectRef<T>& field,
                                    ObjectHandle target) noexcept {
    if (const StructElementLocation* element = cursor.Element()) {
        return cursor.Registry().SetElementReference(cursor.Access(), cursor.OwnerHandle(),
                                                     *element, field, target);
    }

    return cursor.Registry().SetReference(cursor.Access(), cursor.OwnerHandle(), field, target);
}

// 경로가 만드는 새 원소다: noexcept E(Allocator&)가 있으면 allocator를 넘기고 없으면 E()다(struct
// 원소의 기본 원소 — MakeRuntimeStructOps와 같은 규칙).
template <class E>
E MakePathElement(Allocator& allocator) noexcept {
    return MakeAllocatedValue<E>(allocator);
}

// 지금 단계의 struct 값(struct 필드·멤버 또는 struct 원소)으로 내려간다: 등록 struct의 native 키와
// S를 대조한 뒤 다음 단계 멤버 thunk를 부른다.
template <class S>
Result<void> PathDescend(PathCursor& cursor, S& value) noexcept {
    if (auto checked = cursor.CheckStruct(NativeTypeKey<S>()); !checked) {
        return checked;
    }

    return cursor.Descend(&value);
}

// 단일 값 끝 지점(struct 멤버 또는 Array<E> 원소)의 읽기·쓰기다. String·Soft는 cursor가 고정 아래
// 새 값을 만들어 Swap으로 게시하고, 참조는 SetPathMemberReference, 그 밖은 할당 없이 제자리에 쓴다.
template <ValueKind Target, class T>
Result<void> PathValue(PathCursor& cursor, T& value) noexcept {
    const bool read = cursor.Operation() == PathOperation::Read;
    if constexpr (Target == ValueKind::Utf8String) {
        return read ? cursor.ReadText(value, nullptr) : cursor.WriteText(value, nullptr);
    } else if constexpr (Target == ValueKind::SoftObjectReference) {
        return read ? cursor.ReadText(value.objectPath, &value.asset)
                    : cursor.WriteText(value.objectPath, &value.asset);
    } else {
        if (read) {
            FieldValue candidate(cursor.Output()->GetAllocator());
            if (auto copied = CopyTargetValue<Target>(value, candidate); !copied) {
                return cursor.Fail(copied.Error());
            }

            return cursor.PublishRead(candidate);
        }

        if constexpr (Target == ValueKind::ObjectReference) {
            const ObjectHandle previous = value.Handle();
            if (auto set = SetPathMemberReference(cursor, value, cursor.Input()->reference); !set) {
                return cursor.Fail(set.Error());
            }

            cursor.PublishReference(previous);
            return Result<void>::Success();
        } else {
            // weak 대상은 정적 대상 타입의 파생이어야 한다(생존·스키마 기대 타입은 검증 지점이
            // 봤다).
            if constexpr (Target == ValueKind::WeakObjectReference) {
                if (auto checked = cursor.CheckWeakTarget(
                        NativeTypeKey<typename WeakReferenceField<T>::Target>());
                    !checked) {
                    return checked;
                }
            }

            // ClassRef 값은 정적 Base의 파생이어야 한다(등록·스키마 Base는 검증 지점이 봤다).
            if constexpr (Target == ValueKind::ClassReference) {
                if (auto checked = cursor.CheckClassTarget(
                        NativeTypeKey<typename ClassReferenceField<T>::Target>(), value.Id());
                    !checked) {
                    return checked;
                }
            }

            // 할당하지 않는 쓰기다(옛 값 복사도 할당 없음).
            FieldValue* previous = cursor.Output();
            FieldValue captured(previous ? previous->GetAllocator()
                                         : cursor.Input()->GetAllocator());
            if (previous) {
                if (auto copied = CopyTargetValue<Target>(value, captured); !copied) {
                    return cursor.Fail(copied.Error());
                }
            }

            if (auto written = WriteTargetValue<Target>(value, *cursor.Input()); !written) {
                return cursor.FailValue(written.Error());
            }

            cursor.PublishOutput(captured);
            return Result<void>::Success();
        }
    }
}

// Array<E>의 원소 삽입이다(7.1절): (고정 아래) 원소를 배열의 allocator로 만들고, 용량이 충분하면
// TryEmplaceAt의 용량 내 경로(무할당)로, 모자라면 같은 allocator의 임시 배열에 성장 규칙
// (ArrayGrowthCapacity)대로 예약한 뒤 owner를 다시 확인하고 원소를 무할당으로 옮겨
// `old(Move(live)); live = Move(temp);`로 게시한다. 이동 생성은 live를 빈 상태로 두고, 빈 배열로의
// 이동 대입은 버퍼를 할당하거나 반환하지 않으므로 옛 버퍼의 해제 콜백은 게시된 새 버퍼만 본다.
// 고정을 푼 뒤 옛 버퍼를 반환한다.
template <ValueKind Target, class E>
Result<void> InsertArrayElement(PathCursor& cursor, Array<E>& array) noexcept {
    const usize index = cursor.Frame().index;
    const usize size = array.Size();
    if (auto valid = cursor.CheckIndex(size); !valid) {
        return valid;
    }

    if (auto fits = cursor.CheckLength(size + 1); !fits) {
        return fits;
    }

    if constexpr (Target == ValueKind::WeakObjectReference) {
        if (auto checked =
                cursor.CheckWeakTarget(NativeTypeKey<typename WeakReferenceField<E>::Target>());
            !checked) {
            return checked;
        }
    }

    if constexpr (Target == ValueKind::ClassReference) {
        if (auto checked =
                cursor.CheckClassTarget(NativeTypeKey<typename ClassReferenceField<E>::Target>());
            !checked) {
            return checked;
        }
    }

    if (size >= MaxSize / sizeof(E)) {
        return cursor.Fail(ErrorCode::Overflow);
    }

    if (auto pinned = cursor.Pin(); !pinned) {
        return pinned;
    }

    Allocator& allocator = array.GetAllocator();
    E element = MakePathElement<E>(allocator);
    if constexpr (Target != ValueKind::Struct) {
        if (auto written = WriteTargetValue<Target>(element, *cursor.Input()); !written) {
            return cursor.FailValue(written.Error());
        }
    }

    if (size < array.Capacity()) {
        if (auto alive = cursor.CheckOwner(); !alive) {
            return alive;
        }

        // 용량 안의 삽입은 원소를 옮기기만 한다(moved-from 원소의 소멸자는 allocator를 부르지
        // 않는다는 계약).
        if (!array.TryEmplaceAt(index, Move(element))) {
            platform::Fatal("path array insertion within capacity failed");
        }

        cursor.ReleasePin();
        return Result<void>::Success();
    }

    Array<E> grown(allocator);
    if (auto reserved =
            grown.TryReserve(ArrayGrowthCapacity(array.Capacity(), size + 1, sizeof(E)));
        !reserved) {
        return cursor.Fail(reserved.Error());
    }

    // 원소를 옮기기 전에 확인한다(소유자가 파괴됐으면 live를 건드리지 않는다).
    if (auto alive = cursor.CheckOwner(); !alive) {
        return alive;
    }

    for (usize position = 0; position <= size; ++position) {
        auto moved =
            position == index
                ? grown.TryEmplaceBack(Move(element))
                : grown.TryEmplaceBack(Move(array[position < index ? position : position - 1]));
        if (!moved) {
            platform::Fatal("path array insertion into a reserved buffer failed");
        }
    }

    Array<E> old(Move(array));
    array = Move(grown);
    cursor.ReleasePin();
    return Result<void>::Success();
}

// Array<E>의 원소 삭제다(7.1절): 원소를 지역으로 옮겨 뺀 뒤(`E e(Move(live[i]));
// live.TryRemoveAt(i);` — 무할당) 게시하고 원소는 게시 뒤 소멸한다(TryRemoveAt이 제자리에서
// 소멸하면 소멸 콜백 중 불일치 상태가 보인다). 단일 값 원소의 removed는 allocator가 같으면 옮기고
// 다르면 고정 아래 복사한다.
template <ValueKind Target, class E>
Result<void> RemoveArrayElement(PathCursor& cursor, Array<E>& array) noexcept {
    const usize index = cursor.Frame().index;
    if (auto valid = cursor.CheckIndex(array.Size()); !valid) {
        return valid;
    }

    if constexpr (Target == ValueKind::Struct) {
        // 소멸을 게시 뒤로 미루려고 붙잡는다(소멸자가 자명한 원소 — Binds Vec3 등 — 도
        // 경고 없이).
        [[maybe_unused]] E element(Move(array[index]));
        if (!array.TryRemoveAt(index)) {
            platform::Fatal("path array removal failed");
        }

        return Result<void>::Success();
    } else {
        FieldValue* removed = cursor.Output();
        FieldValue captured(removed ? removed->GetAllocator() : array.GetAllocator());
        bool moveText = false;
        if (removed) {
            Result<void> prepared = Result<void>::Success();
            if constexpr (Target == ValueKind::Utf8String) {
                prepared = cursor.CaptureText(array[index], nullptr, captured, moveText);
            } else if constexpr (Target == ValueKind::SoftObjectReference) {
                prepared = cursor.CaptureText(array[index].objectPath, &array[index].asset,
                                              captured, moveText);
            } else {
                prepared = CopyTargetValue<Target>(array[index], captured);
                if (!prepared) {
                    return cursor.Fail(prepared.Error());
                }
            }

            if (!prepared) {
                return prepared;
            }
        }

        // String·Soft만 아래에서 쓴다(스칼라 원소는 게시 뒤 소멸을 위해 붙잡을 뿐이다).
        [[maybe_unused]] E element(Move(array[index]));
        if (!array.TryRemoveAt(index)) {
            platform::Fatal("path array removal failed");
        }

        cursor.ReleasePin();
        if (moveText) {
            if constexpr (Target == ValueKind::Utf8String) {
                captured.string.Swap(element);
            } else if constexpr (Target == ValueKind::SoftObjectReference) {
                captured.string.Swap(element.objectPath);
            }
        }

        if (removed) {
            cursor.PublishOutput(captured);
        }

        return Result<void>::Success();
    }
}

// Array<E> 필드·멤버의 단계다: struct 원소 배열은 연산과 무관하게 먼저 원소 struct를 정적 대조하고,
// index 없는 끝 단계는 길이·비우기(용량까지 반환 — 옛 원소·버퍼는 게시 뒤 소멸), index 있는 중간
// 단계는 struct 원소로 내려가고, 끝 단계는 원소 읽기·쓰기·삽입·삭제다.
template <ValueKind Target, class E>
Result<void> PathContainer(PathCursor& cursor, Array<E>& array) noexcept {
    const PathFrame& frame = cursor.Frame();
    const PathOperation operation = cursor.Operation();
    if constexpr (Target == ValueKind::Struct) {
        if (auto checked = cursor.CheckStruct(NativeTypeKey<E>()); !checked) {
            return checked;
        }
    }

    if (frame.selector == PathSelector::None) {
        if (operation == PathOperation::Length) {
            cursor.SetLength(array.Size());
            return Result<void>::Success();
        }

        if (operation == PathOperation::Clear) {
            Array<E> old(Move(array));
            return Result<void>::Success();
        }

        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
    }

    if (!cursor.IsLast()) {
        if constexpr (Target == ValueKind::Struct) {
            if (auto valid = cursor.CheckIndex(array.Size()); !valid) {
                return valid;
            }

            return cursor.Descend(&array[frame.index]);
        } else {
            return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct);
        }
    }

    switch (operation) {
    case PathOperation::Insert:
        return InsertArrayElement<Target>(cursor, array);
    case PathOperation::Remove:
        return RemoveArrayElement<Target>(cursor, array);
    case PathOperation::Read:
    case PathOperation::Write:
        if constexpr (Target == ValueKind::Struct) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
        } else {
            if (auto valid = cursor.CheckIndex(array.Size()); !valid) {
                return valid;
            }

            return PathValue<Target>(cursor, array[frame.index]);
        }
    default:
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::IndexNotAllowed);
    }
}

// ObjectRefArray 필드·멤버의 단계다(원소 null 허용 — 비정규 null은 Registry가 정규화한다). Registry
// 컨테이너 API로 바꾸며 고정을 쥐지 않는다(자체 재판단 규칙 — 5.6절).
template <ValueKind Target, class T>
Result<void> PathContainer(PathCursor& cursor, ObjectRefArray<T>& array) noexcept {
    ObjectRegistry& registry = cursor.Registry();
    const ObjectAccessContext& access = cursor.Access();
    const ObjectHandle owner = cursor.OwnerHandle();
    const PathFrame& frame = cursor.Frame();
    const PathOperation operation = cursor.Operation();
    if (frame.selector == PathSelector::None) {
        if (operation == PathOperation::Length) {
            cursor.SetLength(array.Size());
            return Result<void>::Success();
        }

        if (operation == PathOperation::Clear) {
            return cursor.Finish(registry.ClearReferences(access, owner, array));
        }

        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
    }

    if (!cursor.IsLast()) {
        return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct);
    }

    if (auto valid = cursor.CheckIndex(array.Size()); !valid) {
        return valid;
    }

    const usize index = frame.index;
    switch (operation) {
    case PathOperation::Read: {
        FieldValue candidate(cursor.Output()->GetAllocator());
        candidate.kind = ValueKind::ObjectReference;
        candidate.reference = array[index];
        return cursor.PublishRead(candidate);
    }
    case PathOperation::Write: {
        const ObjectHandle previous = array[index];
        if (auto set =
                registry.SetReferenceAt(access, owner, array, index, cursor.Input()->reference);
            !set) {
            return cursor.Fail(set.Error());
        }

        cursor.PublishReference(previous);
        return Result<void>::Success();
    }
    case PathOperation::Insert:
        if (auto fits = cursor.CheckLength(array.Size() + 1); !fits) {
            return fits;
        }

        return cursor.Finish(
            registry.InsertReferenceAt(access, owner, array, index, cursor.Input()->reference));
    case PathOperation::Remove: {
        const ObjectHandle previous = array[index];
        if (auto removed = registry.RemoveReferenceAt(access, owner, array, index); !removed) {
            return cursor.Fail(removed.Error());
        }

        cursor.PublishReference(previous);
        return Result<void>::Success();
    }
    default:
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::IndexNotAllowed);
    }
}

// StructRefArray 필드·멤버의 단계다: 연산과 무관하게 먼저 원소 struct를 정적 대조하고, 원소
// 삽입·삭제·비우기는 Registry API(값 없음), 원소 멤버는 그 원소 위치로 들어가 멤버 단위로
// 한다(원소를 통째로 읽거나 쓰지 않는다).
template <ValueKind Target, class S>
Result<void> PathContainer(PathCursor& cursor, StructRefArray<S>& array) noexcept {
    ObjectRegistry& registry = cursor.Registry();
    const ObjectAccessContext& access = cursor.Access();
    const ObjectHandle owner = cursor.OwnerHandle();
    const PathFrame& frame = cursor.Frame();
    const PathOperation operation = cursor.Operation();
    if (auto checked = cursor.CheckStruct(NativeTypeKey<S>()); !checked) {
        return checked;
    }

    if (frame.selector == PathSelector::None) {
        if (operation == PathOperation::Length) {
            cursor.SetLength(array.Size());
            return Result<void>::Success();
        }

        if (operation == PathOperation::Clear) {
            return cursor.Finish(registry.ClearElements(access, owner, array));
        }

        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
    }

    if (auto valid = cursor.CheckIndex(array.Size()); !valid) {
        return valid;
    }

    const usize index = frame.index;
    if (!cursor.IsLast()) {
        cursor.EnterElement(StructRefArrayAccess::Location(array, index));
        return cursor.Descend(&array[index]);
    }

    switch (operation) {
    case PathOperation::Insert:
        if (auto fits = cursor.CheckLength(array.Size() + 1); !fits) {
            return fits;
        }

        return cursor.Finish(registry.InsertElementAt(access, owner, array, index));
    case PathOperation::Remove:
        return cursor.Finish(registry.RemoveElementAt(access, owner, array, index));
    case PathOperation::Read:
    case PathOperation::Write:
        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
    default:
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::IndexNotAllowed);
    }
}

// 정적 배열(T x[N] — 리플렉션 B9g) 필드·멤버의 단계다: struct 원소는 연산과 무관하게 먼저 원소
// struct를 정적 대조하고, index 없는 끝 단계는 길이(N)만 받는다(비우기는 경로 실행기가
// Unsupported(FixedLength)로 먼저 거부한다). index 있는 중간 단계는 struct 원소로 내려가고, 끝
// 단계는 원소 읽기·쓰기(PathValue — 참조 원소는 SetPathMemberReference, 소유 객체 저장소 안
// 주소)다. 원소 수를 바꾸는 삽입·삭제는 경로 실행기가 먼저 거부하므로 여기 오면 계약 위반 방어로
// Unsupported다.
template <ValueKind Target, class E, usize N>
Result<void> PathStaticArray(PathCursor& cursor, E (&array)[N]) noexcept {
    const PathFrame& frame = cursor.Frame();
    const PathOperation operation = cursor.Operation();
    if constexpr (Target == ValueKind::Struct) {
        if (auto checked = cursor.CheckStruct(NativeTypeKey<E>()); !checked) {
            return checked;
        }
    }

    if (frame.selector == PathSelector::None) {
        if (operation == PathOperation::Length) {
            cursor.SetLength(N);
            return Result<void>::Success();
        }

        return cursor.Fail(ErrorCode::Unsupported, operation == PathOperation::Clear
                                                       ? PropertyPathError::FixedLength
                                                       : PropertyPathError::AggregateValue);
    }

    if (!cursor.IsLast()) {
        if constexpr (Target == ValueKind::Struct) {
            if (auto valid = cursor.CheckIndex(N); !valid) {
                return valid;
            }

            return cursor.Descend(&array[frame.index]);
        } else {
            return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct);
        }
    }

    switch (operation) {
    case PathOperation::Insert:
    case PathOperation::Remove:
        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::FixedLength);
    case PathOperation::Read:
    case PathOperation::Write:
        if constexpr (Target == ValueKind::Struct) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
        } else {
            if (auto valid = cursor.CheckIndex(N); !valid) {
                return valid;
            }

            return PathValue<Target>(cursor, array[frame.index]);
        }
    default:
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::IndexNotAllowed);
    }
}

// Optional<값 struct> 필드·멤버의 단계다(리플렉션 B9e): 먼저 내부 struct를 정적 대조하고, 끝이
// 아니면 set일 때만 내부 값으로 내려간다(unset이면 NotFound(OptionalUnset)). 끝 단계는 기본 값
// 만들기(고정 아래 객체 allocator로 새 값을 만들고 owner를 다시 확인한 뒤 옛 값을 지역으로 옮겨
// 무할당 이동으로 게시 — 옛 값은 고정을 푼 뒤 소멸)와 비우기(옛 값을 지역으로 옮겨 비운 뒤 게시 뒤
// 소멸)이고, 통째 읽기·쓰기는 AggregateValue다.
template <class S>
Result<void> PathOptionalStruct(PathCursor& cursor, Optional<S>& member) noexcept {
    if (auto checked = cursor.CheckStruct(NativeTypeKey<S>()); !checked) {
        return checked;
    }

    if (!cursor.IsLast()) {
        if (!member.HasValue()) {
            return cursor.Fail(ErrorCode::NotFound, PropertyPathError::OptionalUnset);
        }

        return cursor.Descend(&member.Value());
    }

    switch (cursor.Operation()) {
    case PathOperation::EmplaceOptional: {
        if (auto pinned = cursor.Pin(); !pinned) {
            return pinned;
        }

        S created = MakeAllocatedValue<S>(cursor.Registry().ObjectAllocator());
        if (auto alive = cursor.CheckOwner(); !alive) {
            return alive;
        }

        Optional<S> previous(Move(member));
        member.Reset();
        member.Emplace(Move(created));
        cursor.ReleasePin();
        return Result<void>::Success();
    }
    case PathOperation::ResetOptional: {
        Optional<S> previous(Move(member));
        member.Reset();
        return Result<void>::Success();
    }
    case PathOperation::QueryOptional:
        cursor.SetLength(member.HasValue() ? 1 : 0);
        return Result<void>::Success();
    default:
        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
    }
}

// 단일 값 Optional 필드·멤버(struct 멤버 — 최상위 필드는 등록된 read·write)의 끝 지점이다(리플렉션
// B9e). 읽기는 kind = Target, present = set 여부(unset이면 값 칸 기본값, enum이면 type만 그 enum
// ID). 쓰기는 검증 지점(ValidateFieldWrite)을 지난 값으로: previous가 있으면 옛 값(String은
// previous의 allocator로 고정 아래 복사), present = false면 옛 값을 지역으로 옮겨 비우고, 참이면
// String은 고정 아래 새 값(set이면 그 값의 allocator, unset이면 객체 allocator)을 만들고 owner를
// 다시 확인한 뒤 Swap·이동으로, 그 밖은 WriteTargetValue로 만든 새 값을 대입한다. 옛 값·버퍼는
// 고정을 푼 뒤 반환한다.
template <ValueKind Target, class T>
Result<void> PathOptionalValue(PathCursor& cursor, Optional<T>& member) noexcept {
    if (!cursor.IsLast()) {
        return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct);
    }

    const PathOperation operation = cursor.Operation();
    if (operation != PathOperation::Read && operation != PathOperation::Write) {
        return cursor.Fail(ErrorCode::InvalidArgument);
    }

    // 옛 값(읽기면 결과)을 담는다.
    FieldValue* output = cursor.Output();
    const bool read = operation == PathOperation::Read;
    FieldValue captured(output ? output->GetAllocator() : cursor.Input()->GetAllocator());
    if (output) {
        captured.kind = Target;
        captured.present = member.HasValue();
        if (member.HasValue()) {
            if constexpr (Target == ValueKind::Utf8String) {
                // Read는 처음부터 고정을 쥔다. 쓰기의 previous 복사도 고정 아래 한다.
                if (auto pinned = cursor.Pin(); !pinned) {
                    return pinned;
                }
            }

            if (auto copied = CopyTargetValue<Target>(member.Value(), captured); !copied) {
                return cursor.Fail(copied.Error());
            }
        } else if constexpr (Target == ValueKind::Enum) {
            captured.type = GeneratedType<T>::Id();
        }
    }

    if (read) {
        return cursor.PublishRead(captured);
    }

    const FieldValue& input = *cursor.Input();
    if (!input.present) {
        if (auto alive = cursor.CheckOwner(); !alive) {
            return alive;
        }

        Optional<T> previous(Move(member));
        member.Reset();
        cursor.ReleasePin();
        cursor.PublishOutput(captured);
        return Result<void>::Success();
    }

    if constexpr (Target == ValueKind::Utf8String) {
        if (auto pinned = cursor.Pin(); !pinned) {
            return pinned;
        }

        String replacement(member.HasValue() ? member.Value().GetAllocator()
                                             : cursor.Registry().ObjectAllocator());
        if (auto copied = replacement.TryAssign(input.string.View()); !copied) {
            return cursor.Fail(copied.Error());
        }

        if (auto alive = cursor.CheckOwner(); !alive) {
            return alive;
        }

        if (member.HasValue()) {
            member.Value().Swap(replacement);
        } else {
            member.Emplace(Move(replacement));
        }

        cursor.ReleasePin();
        cursor.PublishOutput(captured);
        return Result<void>::Success();
    } else {
        T candidate{};
        if (auto written = WriteTargetValue<Target>(candidate, input); !written) {
            return cursor.FailValue(written.Error());
        }

        if (auto alive = cursor.CheckOwner(); !alive) {
            return alive;
        }

        member.Emplace(candidate);
        cursor.ReleasePin();
        cursor.PublishOutput(captured);
        return Result<void>::Success();
    }
}

// Map·Set 필드·멤버의 단계다(리플렉션 B9f3 — 정의와 key 단계 도우미는 이 헤더 끝에서 include하는
// Object/GeneratedKeyedPathAccess.h).
template <ValueKind Target, class Container>
Result<void> PathKeyedContainer(PathCursor& cursor, Container& container) noexcept;

// 필드·멤버 값 하나(member — Member 형식)의 단계다: 정적 대조(대상 종류·컨테이너 종류(정적 배열·
// 배열·Optional·Map·Set)·enum ID) 뒤 정적 배열, 컨테이너, Map·Set, Optional, struct(내려가기), 단일
// 값 끝 지점으로 나눈다.
template <ValueKind Target, class Member>
Result<void> PathMember(PathCursor& cursor, Member& member) noexcept {
    constexpr bool fixed = StaticArrayField<Member>::Supported;
    constexpr bool container = ArrayField<Member>::Supported;
    constexpr bool optional = OptionalField<Member>::Supported;
    constexpr bool keyed = MapField<Member>::Supported;
    constexpr ValueKind wrapper = fixed       ? ValueKind::StaticArray
                                  : container ? ValueKind::Array
                                  : optional  ? ValueKind::Optional
                                  : keyed     ? FieldValueKind<Member>()
                                              : ValueKind::Opaque;
    if constexpr (fixed) {
        static_assert(TargetValueCheck<Target, typename StaticArrayField<Member>::Element>::Valid);
    } else if constexpr (container) {
        static_assert(TargetValueCheck<Target, typename ArrayField<Member>::Element>::Valid);
    } else if constexpr (optional) {
        static_assert(TargetValueCheck<Target, typename OptionalField<Member>::Inner>::Valid);
    } else if constexpr (keyed) {
        static_assert(TargetValueCheck<Target, typename MapField<Member>::Value>::Valid);
    } else {
        static_assert(TargetValueCheck<Target, Member>::Valid);
    }

    if (auto checked = cursor.CheckTarget(Target, wrapper); !checked) {
        return checked;
    }

    if constexpr (Target == ValueKind::Enum) {
        if constexpr (fixed) {
            if (auto checked = cursor.CheckEnum(
                    GeneratedType<typename StaticArrayField<Member>::Element>::Id());
                !checked) {
                return checked;
            }
        } else if constexpr (container) {
            if (auto checked =
                    cursor.CheckEnum(GeneratedType<typename ArrayField<Member>::Element>::Id());
                !checked) {
                return checked;
            }
        } else if constexpr (optional) {
            if (auto checked =
                    cursor.CheckEnum(GeneratedType<typename OptionalField<Member>::Inner>::Id());
                !checked) {
                return checked;
            }
        } else if constexpr (keyed) {
            if (auto checked =
                    cursor.CheckEnum(GeneratedType<typename MapField<Member>::Value>::Id());
                !checked) {
                return checked;
            }
        } else {
            if (auto checked = cursor.CheckEnum(GeneratedType<Member>::Id()); !checked) {
                return checked;
            }
        }
    }

    if constexpr (fixed) {
        return PathStaticArray<Target>(cursor, member);
    } else if constexpr (container) {
        return PathContainer<Target>(cursor, member);
    } else if constexpr (keyed) {
        return PathKeyedContainer<Target>(cursor, member);
    } else if constexpr (optional) {
        if constexpr (Target == ValueKind::Struct) {
            return PathOptionalStruct(cursor, member);
        } else {
            return PathOptionalValue<Target>(cursor, member);
        }
    } else if constexpr (Target == ValueKind::Struct) {
        if (cursor.IsLast()) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
        }

        return PathDescend(cursor, member);
    } else {
        return PathValue<Target>(cursor, member);
    }
}

// 클래스 aggregate 필드(Struct·Array·Optional<값 struct>·Map·Set)의 경로 thunk 본체다(생성 코드가
// 함수표의 path 칸에 내는 람다가 부른다). 소유 객체는 실제 타입 레코드의 함수표에서 오므로
// static_cast<Owner*>이고, cursor가 실제 타입이 Owner의 파생인지 다시 확인한다.
//
// 생성 코드(B6b):
//   +[](const ::rocks::RuntimeFieldOps&, ::rocks::detail::PathCursor& cursor) noexcept
//       -> ::rocks::Result<void> {
//       return ::rocks::detail::PathClassField<::Mesh, ::rocks::ValueKind::Struct>(
//           cursor, &::Mesh::slots);
//   }
template <class Owner, ValueKind Target, class MemberOwner, class Member>
Result<void> PathClassField(PathCursor& cursor, Member MemberOwner::* member) noexcept {
    static_assert(IsBaseOfV<MemberOwner, Owner>);
    static_assert(IsBaseOfV<Object, Owner>);
    auto owner = cursor.Owner(NativeTypeKey<Owner>());
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    return PathMember<Target>(cursor, static_cast<Owner*>(owner.Value())->*member);
}

// struct 멤버의 경로 thunk다(StructFieldPathFunction — StructFieldOps의 둘째 인자). structValue는
// 바로 바깥 단계가 등록 struct와 대조한 S 값이다.
//
// 생성 코드(B6b):
//   detail::StructFieldOps<&::MaterialSlot::name>(
//       NameId, &detail::PathStructMember<&::MaterialSlot::name, ValueKind::Utf8String>)
template <auto MemberPointer, ValueKind Target>
Result<void> PathStructMember(const RuntimeStructFieldOps&, PathCursor& cursor,
                              void* structValue) noexcept {
    using S = typename StructMemberPointer<decltype(MemberPointer)>::Owner;
    if (auto checked = cursor.CheckMemberOwner(NativeTypeKey<S>()); !checked) {
        return checked;
    }

    return PathMember<Target>(cursor, static_cast<S*>(structValue)->*MemberPointer);
}

} // namespace rocks::detail

#include <Object/GeneratedKeyedPathAccess.h>
