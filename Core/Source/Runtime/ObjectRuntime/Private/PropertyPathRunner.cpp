#include "PropertyPathInternal.h"

#include <Core/Text/NamePool.h>
#include <Object/ClassRef.h>
#include <Object/ConstructionContext.h>
#include <Object/InstancedStruct.h>
#include <Object/NameField.h>
#include <Object/ObjectBatchPublish.h>
#include <Object/ValueCopy.h>

// 경로 연산의 실행(detail::PathRunner — 입력 형식·문맥·단계 해석·key 해석·끝 지점 분류·참조
// lease·검증 지점·Borrow·생성 thunk 실행, thunk가 부르는 넣기 준비)과 공개 연산 진입점이다:
// PathRead·PathWrite·PathCount·PathInsertElement·PathRemoveElement·PathClear·PathContains·
// PathInsertKey·PathEraseKey·PathEmplaceOptional·PathResetOptional·PathIsOptionalSet.
namespace rocks {

namespace detail {

namespace {

// 경로 thunk가 FieldValue로 표현할 수 없는 값 종류다(성질 표의 aggregate — struct 값·배열 통째).
// 끝 지점의 종류(원소·Optional 내부 포함)에 쓴다 — 필드 하나는 FieldIsAggregate로 판정한다.
bool IsAggregateKind(ValueKind kind) noexcept {
    return ValueKindIsAggregate(kind);
}

// 필드가 key 단계를 받는 컨테이너(Map·Set — 리플렉션 B9f3)인지다.
bool IsKeyedKind(ValueKind kind) noexcept {
    return kind == ValueKind::Map || kind == ValueKind::Set;
}

// 필드가 원소 수를 읽는 컨테이너(Array·StaticArray·Map·Set)인지다(StaticArray의 비우기는
// FixedLength로 따로 거부한다).
bool IsContainerKind(ValueKind kind) noexcept {
    return ValueKindTakesIndex(kind) || IsKeyedKind(kind);
}

// 단계의 값이 FieldValue·struct로 접근하는 형식이다: 배열 원소·Map 항목·Set 원소 단계면 원소(Map은
// 값), Optional 필드면 내부(리플렉션 B9e), 아니면 값 형식.
const ValueType& StepValueType(const FieldSchema& field, PathSelector selector) noexcept {
    return selector != PathSelector::None || field.value.kind == ValueKind::Optional ? field.element
                                                                                     : field.value;
}

// 쓰기 검증(ValidateFieldWrite·등록된 write) 오류의 사유다: Unsupported는 유효 플래그에 ReadOnly가
// 있을 때만 ReadOnly(그 밖의 Unsupported — 수동 callback의 정책 등 — 는 None), 값 종류·enum
// ID(TypeMismatch)와 값 내용(InvalidArgument)은 InvalidValue다.
// 객체를 바꾸는 경로 연산인지다(struct 검증 hook 우회 판정 — Docs/Design/CurveData.ko.md 9.1절).
bool IsMutation(PathOperation operation) noexcept {
    switch (operation) {
    case PathOperation::Write:
    case PathOperation::Insert:
    case PathOperation::Remove:
    case PathOperation::Clear:
    case PathOperation::EmplaceOptional:
    case PathOperation::ResetOptional:
    case PathOperation::InsertKey:
    case PathOperation::EraseKey:
    case PathOperation::SetInstancedType:
    case PathOperation::ClearInstanced:
        return true;
    default:
        return false;
    }
}

// 해석한 단계 0..limit 중 위치 값이 검증 hook 있는 struct인 가장 바깥 단계다(없으면 IndexNone).
// struct 위치는 struct 필드·멤버·원소·Map 값과 멤버 단계로 들어가는 InstancedStruct(담긴 struct를
// 이미 풀었다 — total은 경로의 전체 단계 수라 그 뒤 단계가 있는지로 본다)다.
usize FindHookStep(const PathFrame* frames, usize limit, usize total) noexcept {
    for (usize index = 0; index < limit; ++index) {
        const PathFrame& frame = frames[index];
        const ValueKind kind = StepValueType(*frame.field, frame.selector).kind;
        const bool setElement =
            frame.selector == PathSelector::Key && frame.field->value.kind == ValueKind::Set;
        const bool structure = !setElement && !frame.target.IsEmpty() &&
                               (kind == ValueKind::Struct ||
                                (kind == ValueKind::InstancedStruct && index + 1 < total));
        if (structure && frame.target.StructOps().validate) {
            return index;
        }
    }

    return IndexNone;
}

PropertyPathError WriteReason(ErrorCode code, uint32 effectiveFlags) noexcept {
    switch (code) {
    case ErrorCode::Unsupported:
        return HasFlag(effectiveFlags, FieldFlags::ReadOnly) ? PropertyPathError::ReadOnly
                                                             : PropertyPathError::None;
    case ErrorCode::TypeMismatch:
    case ErrorCode::InvalidArgument:
        return PropertyPathError::InvalidValue;
    default:
        return PropertyPathError::None;
    }
}

} // namespace

Result<void> PathRunner::RunTopRead(PathCursor& cursor) noexcept {
    cursor.current_ = 0;
    auto borrowed = cursor.registry_.Resolve<Object>(cursor.access_, cursor.owner_);
    if (!borrowed) {
        return cursor.Fail(borrowed.Error());
    }

    cursor.borrow_ = Move(borrowed.Value());
    // 수동 함수표의 read도 고정 아래 부른다(read는 쓰기 Borrow를 얻지 않으므로 이 고정에 막히지
    // 않는다).
    if (auto pinned = cursor.Pin(); !pinned) {
        return pinned;
    }

    FieldValue candidate(cursor.output_->GetAllocator());
    if (auto read =
            cursor.topOps_->read(cursor.registry_, cursor.access_, cursor.owner_, candidate);
        !read) {
        return cursor.Fail(read.Error());
    }

    return cursor.PublishRead(candidate);
}

Result<void> PathRunner::RunTopWrite(PathCursor& cursor) noexcept {
    cursor.current_ = 0;
    FieldValue captured(cursor.output_ ? cursor.output_->GetAllocator()
                                       : cursor.input_->GetAllocator());
    if (cursor.output_) {
        // previous는 write 직전에 등록된 read로 얻는다. 쓰기용 Borrow로 고정 중이면 read 전에
        // Busy를 판정하고, 고정 아래 previous의 allocator로 복사한 뒤 owner를 다시 확인한다.
        auto borrowed = cursor.registry_.ResolveForWrite<Object>(cursor.access_, cursor.owner_);
        if (!borrowed) {
            return cursor.Fail(borrowed.Error());
        }

        cursor.borrow_ = Move(borrowed.Value());
        if (auto pinned = cursor.Pin(); !pinned) {
            return pinned;
        }

        if (auto read =
                cursor.topOps_->read(cursor.registry_, cursor.access_, cursor.owner_, captured);
            !read) {
            return cursor.Fail(read.Error());
        }

        if (auto alive = cursor.CheckOwner(); !alive) {
            return alive;
        }

        cursor.pin_.Reset();
        cursor.borrow_.Reset();
    }

    // 등록된 write(FieldWriteOperation)가 자기 검증·ResolveForWrite·게시 직전 재확인을 한다(생성
    // write는 String·Soft 할당 동안 스스로 고정한다). 고정을 쥐면 자기 자신 때문에 Busy가 되므로
    // 쥐지 않는다.
    if (auto written =
            cursor.topOps_->write(cursor.registry_, cursor.access_, cursor.owner_, *cursor.input_);
        !written) {
        return cursor.Fail(written.Error(),
                           WriteReason(written.Error(), cursor.frames_[0].effectiveFlags));
    }

    cursor.PublishOutput(captured);
    return Result<void>::Success();
}

Result<void> PathRunner::ResolveKey(PathCursor& cursor, PathFrame& frame,
                                    const PathKey& key) noexcept {
    const FieldSchema& field = *frame.field;
    const ValueType& keyType = field.value.kind == ValueKind::Map ? field.key : field.element;
    if (key.kind != keyType.kind) {
        return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::InvalidKey);
    }

    auto value = ReadPathKey(key);
    if (!value) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidKey);
    }

    // 미선언 enum 키는 어느 컨테이너에도 없다(복원·저장·쓰기가 모두 거부한다) — 값 오류다.
    if (keyType.kind == ValueKind::Enum &&
        (!frame.keyEnumeration ||
         !IsDeclaredEnumValue(*frame.keyEnumeration, value.Value().signedInteger))) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidKey);
    }

    frame.key = value.Value();
    frame.keyName = Name{};
    frame.keyFound = true;
    if (keyType.kind != ValueKind::Name) {
        return Result<void>::Success();
    }

    // Name 키 조회는 pool을 늘리지 않는다(NamePool::TryFind — pool에 없으면 그 키도 없다). pool이
    // 없는 Registry에는 기본 None만 있다.
    const StringView text = frame.key.text;
    NamePool* names = cursor.registry_.Names();
    if (!names) {
        frame.keyFound = NameTextEquals(text, StringView());
        return Result<void>::Success();
    }

    auto found = names->TryFind(text);
    if (found) {
        frame.keyName = found.Value();
        return Result<void>::Success();
    }

    if (found.Error() != ErrorCode::NotFound) {
        return cursor.Fail(found.Error());
    }

    frame.keyFound = false;
    return Result<void>::Success();
}

Result<void> PathRunner::PrepareInsertedKey(PathCursor& cursor, PathFrame& frame) noexcept {
    const FieldSchema& field = *frame.field;
    const ValueType& keyType = field.value.kind == ValueKind::Map ? field.key : field.element;
    // ClassRef 키는 값과 같이 이 Registry에 등록된 클래스이고 스키마 Base의 파생이어야 한다(그
    // lease는 완료까지 쥐고 생성 thunk가 정적 Base를 대조한다 — CheckKeyClass).
    if (keyType.kind == ValueKind::ClassReference && frame.key.guid != Guid{}) {
        auto found = CheckWritableClassReferenceLease(
            cursor.registry_.AcquireRegisteredType(cursor.access_, frame.key.guid), keyType.type,
            nullptr);
        if (!found) {
            return cursor.Fail(found.Error(), PropertyPathError::InvalidKey);
        }

        cursor.keyClass_ = Move(found.Value());
        return Result<void>::Success();
    }

    if (keyType.kind != ValueKind::Name) {
        return Result<void>::Success();
    }

    // 넣을 수 있는 Name 키는 Name 필드 값과 같은 내용 규칙을 지키고 그 표시 문자열 그대로
    // intern한다(같은 원문의 표시 entry가 있으면 그것을 다시 쓴다 — 대소문자 변형은 별도 표시
    // entry다). intern은 대상을 바꾸지 않으므로 뒤 단계가 실패해도 entry는 남는다.
    NamePool* names = cursor.registry_.Names();
    if (!names) {
        if (NameTextEquals(frame.key.text, StringView())) {
            frame.keyName = Name{};
            frame.keyFound = true;
            return Result<void>::Success();
        }

        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::InvalidKey);
    }

    Allocator& scratch =
        cursor.input_ ? cursor.input_->GetAllocator() : cursor.registry_.ObjectAllocator();
    if (auto valid = ValidateNameFieldText(frame.key.text, scratch); !valid) {
        return cursor.Fail(valid.Error(), valid.Error() == ErrorCode::InvalidArgument
                                              ? PropertyPathError::InvalidKey
                                              : PropertyPathError::None);
    }

    auto interned = names->TryIntern(frame.key.text);
    if (!interned) {
        return cursor.Fail(interned.Error());
    }

    frame.keyName = interned.Value();
    frame.keyFound = true;
    return Result<void>::Success();
}

Result<void> PathCursor::PrepareInsertedKey() noexcept {
    return PathRunner::PrepareInsertedKey(*this, frames_[current_]);
}

Result<void> PathRunner::ResolveInstancedStep(PathCursor& cursor, PropertyPath path,
                                              usize index) noexcept {
    // 같은 시작 위치에서 그 단계까지의 앞부분을 조회 연산으로 돌린다(읽기 Borrow·고정은 그 연산
    // 안에서 쥐고 놓는다 — 담긴 struct는 InstancedStruct를 담지 않으므로 앞부분에 다른
    // InstancedStruct 멤버 단계는 없다).
    PropertyPathFailure failure;
    TypeLease type;
    PathCursor query(cursor.registry_, cursor.access_, cursor.target_,
                     PathOperation::QueryInstanced, nullptr, nullptr, &failure);
    query.instancedOut_ = &type;
    const PropertyPath prefix{Span<const PathStep>(path.steps.Data(), index + 1), path.keys};
    if (auto queried = Run(query, prefix); !queried) {
        cursor.current_ = failure.step < index ? failure.step : index;
        return cursor.Fail(queried.Error(), failure.reason);
    }

    cursor.current_ = index;
    if (!type.IsValid()) {
        return cursor.Fail(ErrorCode::NotFound, PropertyPathError::InstancedEmpty);
    }

    cursor.instancedType_ = Move(type);
    cursor.instancedStep_ = index;
    cursor.frames_[index].target = cursor.instancedType_.Struct();
    return Result<void>::Success();
}

Result<void> PathRunner::Run(PathCursor& cursor, PropertyPath path) noexcept {
    const Span<const PathStep> steps = path.steps;
    const PathOperation operation = cursor.operation_;
    const bool keyOperation = operation == PathOperation::Contains ||
                              operation == PathOperation::InsertKey ||
                              operation == PathOperation::EraseKey;
    // 1. 입력 형식.
    if (steps.Size() == 0) {
        return cursor.Fail(ErrorCode::InvalidArgument);
    }

    if (steps.Size() > MaxPropertyPathSteps) {
        cursor.current_ = MaxPropertyPathSteps;
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::TooDeep);
    }

    for (usize index = 0; index < steps.Size(); ++index) {
        if (!IsCanonicalPathStep(steps[index], path.keys.Size())) {
            cursor.current_ = index;
            return cursor.Fail(ErrorCode::InvalidArgument);
        }
    }

    const usize last = steps.Size() - 1;
    const PathSelector lastSelector = steps[last].selector;
    cursor.current_ = last;
    if ((operation == PathOperation::Insert || operation == PathOperation::Remove) &&
        lastSelector != PathSelector::Index) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::IndexRequired);
    }

    if ((operation == PathOperation::Length || operation == PathOperation::Clear ||
         operation == PathOperation::ReadKeys) &&
        lastSelector != PathSelector::None) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::IndexNotAllowed);
    }

    // 키 읽기(PathReadKeys — 리플렉션 B9i4)는 출력 키 목록이 있어야 한다.
    if ((operation == PathOperation::ReadKeys) != (cursor.keys_ != nullptr)) {
        return cursor.Fail(ErrorCode::InvalidArgument);
    }

    if (keyOperation && lastSelector != PathSelector::Key) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::KeyRequired);
    }

    if ((operation == PathOperation::Write || operation == PathOperation::Insert) &&
        cursor.input_ && cursor.input_->kind == ValueKind::Opaque) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
    }

    // Detach·Attach(리플렉션 B9i3c0)는 필드·멤버 하나 단위다. Detach는 빈 상자에 채우고 Attach는 찬
    // 상자를 쓴다. 상자에서 시작한 경로가 같은 상자를 대상으로 삼을 수 없다.
    const bool boxOperation =
        operation == PathOperation::Detach || operation == PathOperation::Attach;
    if (boxOperation) {
        if (lastSelector != PathSelector::None) {
            return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::IndexNotAllowed);
        }

        const bool wantEmpty = operation == PathOperation::Detach;
        if (!cursor.box_ || cursor.box_->IsEmpty() != wantEmpty || cursor.box_ == cursor.rootBox_) {
            return cursor.Fail(ErrorCode::InvalidArgument);
        }
    }

    // 참조 자리 찾기(PathPrepareReference — 리플렉션 B9i3c2)는 객체(게시·staged)의 Write다.
    if (cursor.reference_) {
        if (operation != PathOperation::Write) {
            return cursor.Fail(ErrorCode::InvalidArgument);
        }

        if (cursor.IsDetached()) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::NoPathAccess);
        }
    }

    // Registry 컨테이너 준비 사본(PathTarget::WithReferences — 리플렉션 B9i3c3)은
    // 객체(게시·staged)의 컨테이너 연산이다.
    if (cursor.stage_) {
        const bool staged =
            operation == PathOperation::Read || operation == PathOperation::Write ||
            operation == PathOperation::Length || operation == PathOperation::Insert ||
            operation == PathOperation::Remove || operation == PathOperation::Clear ||
            operation == PathOperation::Contains || operation == PathOperation::EraseKey;
        if (!staged || cursor.IsDetached() || cursor.reference_ || cursor.box_) {
            return cursor.Fail(ErrorCode::InvalidArgument);
        }
    }

    // StructRefArray 원소 지우기 준비물(PathPrepareStructElements — 리플렉션 B9i3c4)은 객체(게시·
    // staged)의 원소 수 읽기다.
    if (cursor.elements_ && (operation != PathOperation::Length || cursor.IsDetached() ||
                             cursor.stage_ || cursor.reference_ || cursor.box_)) {
        return cursor.Fail(ErrorCode::InvalidArgument);
    }

    // 원소 추가 준비물의 게시 전 원소 블록(PathTarget::WithAddedElement — 리플렉션 B9i5)은 struct
    // 값 시작 위치다.
    if (cursor.added_ && (cursor.root_ != PathTarget::Kind::Struct || cursor.stage_ ||
                          cursor.reference_ || cursor.elements_)) {
        return cursor.Fail(ErrorCode::InvalidArgument);
    }

    // 2. 문맥·수명. 게시된 객체는 소유 스레드·문맥·생존(staged·DestroyRequested 객체는 거부된다),
    // staged 객체는 그 트랜잭션의 AcquireType(registry가 트랜잭션의 것이 아니면 WrongContext)이다.
    // 소유 타입 lease는 완료까지 쥔다(해석한 스키마·함수표·StructView의 수명). 분리된 값은 lease가
    // 없다 — struct 값은 호출자의 lease, 상자는 상자가 쥔 lease가 수명을 지킨다.
    cursor.current_ = 0;
    if (cursor.root_ == PathTarget::Kind::Object || cursor.root_ == PathTarget::Kind::Staged) {
        Result<TypeLease> type = Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
        if (cursor.root_ == PathTarget::Kind::Object) {
            type = cursor.registry_.AcquireType(cursor.access_, cursor.owner_);
        } else if (!cursor.construction_) {
            return cursor.Fail(ErrorCode::InvalidArgument);
        } else if (&cursor.construction_->Registry() != &cursor.registry_) {
            return cursor.Fail(ErrorCode::WrongContext);
        } else {
            type = cursor.construction_->AcquireType(cursor.owner_);
        }

        if (!type) {
            return cursor.Fail(type.Error());
        }

        cursor.ownerType_ = Move(type.Value());
    } else if (cursor.root_ == PathTarget::Kind::Struct) {
        if (!cursor.rootLease_ || cursor.rootView_.IsEmpty() || !cursor.rootValue_ ||
            !cursor.rootAllocator_) {
            return cursor.Fail(ErrorCode::InvalidArgument);
        }

        // Registry 컨테이너는 owner 저장소 안에서만 뜻이 있다.
        if (cursor.rootView_.ContainsReferenceArray()) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::NoPathAccess);
        }
    } else if (!cursor.rootBox_ || cursor.rootBox_->IsEmpty()) {
        return cursor.Fail(ErrorCode::InvalidArgument);
    }

    // 3. 단계 순서의 해석: FieldId → 스키마 위치(정렬 색인), 모양, 유효 플래그 누적, key 값.
    // staged 객체와 거기서 분리한 상자는 ReadOnly를 막지 않는다(리플렉션 B9i3c1 — 공개 전 객체의
    // 초기값은 ReadOnly 필드도 정한다, Docs/Design/ReflectionExtensions.ko.md 10.2절).
    const bool acceptsReadOnly = cursor.AcceptsReadOnlyWrites();
    for (usize index = 0; index < steps.Size(); ++index) {
        cursor.current_ = index;
        const PathStep& step = steps[index];
        PathFrame& frame = cursor.frames_[index];
        usize position = TypeLease::NoField;
        const FieldSchema* field = nullptr;
        if (index == 0 && cursor.root_ == PathTarget::Kind::Detached) {
            // 상자의 첫 단계는 상자에 담은 필드 자신이다(바깥 유효 플래그를 이어 받는다).
            const DetachedValue& box = *cursor.rootBox_;
            if (!(step.fieldId == box.field_->id)) {
                return cursor.Fail(ErrorCode::NotFound, PropertyPathError::UnknownField);
            }

            position = 0;
            field = box.field_;
            frame.enumeration = box.enumeration_;
            frame.keyEnumeration = box.keyEnumeration_;
            frame.target = box.target_;
            frame.effectiveFlags = box.effectiveFlags_;
        } else if (index == 0 && cursor.root_ == PathTarget::Kind::Struct) {
            position = cursor.rootView_.FieldIndex(step.fieldId);
            if (position == StructView::NoField) {
                return cursor.Fail(ErrorCode::NotFound, PropertyPathError::UnknownField);
            }

            field = &cursor.rootView_.Schema().fields[position];
            frame.enumeration = cursor.rootView_.FieldEnum(position);
            frame.keyEnumeration = cursor.rootView_.FieldKeyEnum(position);
            frame.target = cursor.rootView_.FieldStruct(position);
            frame.effectiveFlags = field->flags;
        } else if (index == 0) {
            position = cursor.ownerType_.FieldIndex(step.fieldId);
            if (position == TypeLease::NoField) {
                return cursor.Fail(ErrorCode::NotFound, PropertyPathError::UnknownField);
            }

            field = &cursor.ownerType_.Schema().fields[position];
            frame.enumeration = cursor.ownerType_.FieldEnum(position);
            frame.keyEnumeration = cursor.ownerType_.FieldKeyEnum(position);
            frame.target = cursor.ownerType_.FieldStruct(position);
            frame.effectiveFlags = field->flags;
        } else {
            const PathFrame& parent = cursor.frames_[index - 1];
            position = parent.target.FieldIndex(step.fieldId);
            if (position == StructView::NoField) {
                return cursor.Fail(ErrorCode::NotFound, PropertyPathError::UnknownField);
            }

            field = &parent.target.Schema().fields[position];
            frame.enumeration = parent.target.FieldEnum(position);
            frame.keyEnumeration = parent.target.FieldKeyEnum(position);
            frame.target = parent.target.FieldStruct(position);
            frame.effectiveFlags = PropagateFieldFlags(parent.effectiveFlags, field->flags);
        }

        if (acceptsReadOnly) {
            frame.effectiveFlags &= ~static_cast<uint32>(FieldFlags::ReadOnly);
        }

        frame.field = field;
        frame.schemaIndex = position;
        frame.index = step.index;
        frame.selector = step.selector;
        frame.key = PathKeyValue{};
        frame.keyName = Name{};
        frame.keyFound = true;
        cursor.count_ = index + 1;
        if (step.selector == PathSelector::Index && !ValueKindTakesIndex(field->value.kind)) {
            return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotArray);
        }

        if (step.selector == PathSelector::Key) {
            if (!IsKeyedKind(field->value.kind)) {
                return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotKeyed);
            }

            // Set 원소의 enum은 원소 enum(FieldEnum)이다.
            if (field->value.kind == ValueKind::Set) {
                frame.keyEnumeration = frame.enumeration;
            }

            if (auto resolved = ResolveKey(cursor, frame, path.keys[step.index]); !resolved) {
                return resolved;
            }
        }

        if (index < last) {
            const ValueKind kind = StepValueType(*field, step.selector).kind;
            const bool setElement =
                step.selector == PathSelector::Key && field->value.kind == ValueKind::Set;
            // InstancedStruct 뒤 멤버 단계는 지금 담긴 struct로 해석한다(리플렉션 B9j — 같은
            // 시작 위치에서 이 단계까지 담긴 타입을 먼저 조회한다).
            if (kind == ValueKind::InstancedStruct && !setElement) {
                if (auto resolved = ResolveInstancedStep(cursor, path, index); !resolved) {
                    return resolved;
                }
            } else if (kind != ValueKind::Struct || setElement || frame.target.IsEmpty()) {
                return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct);
            }
        }
    }

    const bool objectRoot =
        cursor.root_ == PathTarget::Kind::Object || cursor.root_ == PathTarget::Kind::Staged;

    cursor.current_ = last;
    PathFrame& end = cursor.frames_[last];
    if ((operation == PathOperation::Length || operation == PathOperation::Clear) &&
        !IsContainerKind(end.field->value.kind)) {
        return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotArray);
    }

    if (operation == PathOperation::ReadKeys && end.field->value.kind != ValueKind::Map &&
        end.field->value.kind != ValueKind::Set) {
        return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotKeyed);
    }

    // 정적 배열(리플렉션 B9g)은 길이가 N으로 고정이다: 원소 삽입·삭제·비우기는 값·쓰기 검증보다
    // 먼저 Unsupported(FixedLength)다(원소는 [i] 쓰기로 바꾼다).
    if ((operation == PathOperation::Insert || operation == PathOperation::Remove ||
         operation == PathOperation::Clear) &&
        end.field->value.kind == ValueKind::StaticArray) {
        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::FixedLength);
    }

    // Optional<값 struct> 값 만들기·비우기·set 여부 조회의 끝은 그 필드여야 한다(단일 값 Optional은
    // PathRead·PathWrite의 present로 다룬다 — 리플렉션 B9e).
    const bool optionalEdit =
        operation == PathOperation::EmplaceOptional || operation == PathOperation::ResetOptional;
    const bool optionalStructOperation = optionalEdit || operation == PathOperation::QueryOptional;
    if (optionalStructOperation &&
        (end.selector != PathSelector::None || end.field->value.kind != ValueKind::Optional ||
         end.field->element.kind != ValueKind::Struct)) {
        return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotOptional);
    }

    // 4. 끝 지점 분류: struct·배열 통째 접근, 값 유무, thunk 유무(Unsupported).
    const ValueType& endType = StepValueType(*end.field, end.selector);
    const ValueKind kind = endType.kind;
    const bool setElement =
        end.selector == PathSelector::Key && end.field->value.kind == ValueKind::Set;
    cursor.setElement_ = setElement;
    // InstancedStruct 형 바꾸기·비우기·타입 조회(리플렉션 B9j)의 끝은 InstancedStruct 값(필드·멤버·
    // 원소·Map 값)이다.
    const bool instancedEdit =
        operation == PathOperation::SetInstancedType || operation == PathOperation::ClearInstanced;
    if ((instancedEdit || operation == PathOperation::QueryInstanced) &&
        (kind != ValueKind::InstancedStruct || setElement)) {
        return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotInstanced);
    }

    // 원소·Map 값으로 FieldValue 없이 다루는 값(struct·InstancedStruct)이다.
    const bool valueless = kind == ValueKind::Struct || kind == ValueKind::InstancedStruct;
    // 최상위 단일 값의 등록된 read·write 경로는 게시된 객체의 값 연산만 쓴다(staged·분리된 값과
    // Detach·Attach는 경로 thunk — 리플렉션 B9i3c0).
    const bool topValue = cursor.root_ == PathTarget::Kind::Object && steps.Size() == 1 &&
                          !FieldIsAggregate(*end.field) && !boxOperation && !cursor.reference_ &&
                          !cursor.stage_;
    // 준비 사본의 끝은 ObjectRefArray·ObjectRefMap(그 원소·항목 포함)이다.
    if (cursor.stage_ &&
        (FieldTargetType(*end.field).kind != ValueKind::ObjectReference ||
         (end.field->value.kind != ValueKind::Array && end.field->value.kind != ValueKind::Map))) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
    }

    // 원소 지우기 준비물의 끝은 struct 원소 배열 필드·멤버 통째다(StructRefArray인지는 thunk가
    // 묶으며 가린다 — 값 struct 배열은 PathPrepareStructElements가 거부한다).
    if (cursor.elements_ &&
        (end.selector != PathSelector::None || end.field->value.kind != ValueKind::Array ||
         end.field->element.kind != ValueKind::Struct)) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
    }

    // 참조 자리 찾기의 끝은 단일 강한 참조 위치다(Registry 컨테이너 원소는 그 컨테이너 단위).
    if (cursor.reference_ &&
        (kind != ValueKind::ObjectReference ||
         (end.selector != PathSelector::None && end.field->value.kind != ValueKind::StaticArray))) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
    }

    switch (operation) {
    case PathOperation::Attach:
        // 상자는 같은 스키마 레코드의 필드여야 한다(C++ 형식은 thunk가 대조한다).
        if (cursor.box_->field_ != end.field) {
            return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
        }

        if (HasFlag(end.effectiveFlags, FieldFlags::ReadOnly)) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::ReadOnly);
        }

        // 객체(게시·staged)에 놓는 상자는 게시 전에 검증 hook을 지난다(Docs/Design/CurveData.ko.md
        // 9.1절): 자리 위(바깥) struct에 hook이 있으면 상자만으로 검증할 수 없으므로 거부하고, 상자
        // 값 트리 안의 hook은 안쪽부터 부른다.
        // struct 값 시작 위치(대입 묶음의 Value 대상 — 설정·콘솔)도 같다(게시 자리다).
        if (objectRoot || cursor.root_ == PathTarget::Kind::Struct) {
            const usize hook = FindHookStep(cursor.frames_, last, steps.Size());
            if (hook != IndexNone) {
                cursor.current_ = hook;
                return cursor.Fail(ErrorCode::Unsupported,
                                   PropertyPathError::InsideValidatedStruct);
            }

            if (cursor.validateHooks_) {
                if (auto checked = cursor.box_->ValidateTree(); !checked) {
                    return cursor.Fail(ErrorCode::InvalidArgument,
                                       PropertyPathError::StructValidation);
                }
            }
        }

        break;
    case PathOperation::Read:
    case PathOperation::Write:
        // Set 원소는 키라 값이 없다(PathContains·PathInsertKey·PathEraseKey).
        if (setElement) {
            return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotMap);
        }

        if (kind == ValueKind::Opaque) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::NoPathAccess);
        }

        if (IsAggregateKind(kind)) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
        }

        break;
    case PathOperation::Insert:
        // struct·InstancedStruct 원소는 기본 값(InstancedStruct는 비어 있음)을 넣는다.
        if (valueless && cursor.input_) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
        }

        if (!valueless && !cursor.input_) {
            return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
        }

        break;
    case PathOperation::Remove:
    case PathOperation::EraseKey:
        if (valueless && !setElement && cursor.output_) {
            return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::AggregateValue);
        }

        break;
    case PathOperation::InsertKey:
        // 단일 값 Map 항목은 값이 필요하다(PathWrite로 넣는다).
        if (!setElement && !valueless) {
            return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
        }

        break;
    default:
        break;
    }

    for (usize index = 0; index < steps.Size(); ++index) {
        cursor.current_ = index;
        if (index == 0 && cursor.root_ == PathTarget::Kind::Detached) {
            // 상자는 Detach가 멤버 형식의 진입 함수를 채웠다.
            if (!cursor.rootBox_->enter_) {
                return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::NoPathAccess);
            }
        } else if (index == 0 && cursor.root_ == PathTarget::Kind::Struct) {
            if (!cursor.rootView_.FieldOps(cursor.frames_[0].schemaIndex).path) {
                return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::NoPathAccess);
            }
        } else if (index == 0) {
            const RuntimeFieldOps* ops = cursor.ownerType_.FieldOps(cursor.frames_[0].schemaIndex);
            const bool available =
                ops && (topValue ? (operation == PathOperation::Read
                                        ? ops->read != nullptr
                                        : ops->write && (!cursor.output_ || ops->read != nullptr))
                                 : ops->path != nullptr);
            if (!available) {
                return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::NoPathAccess);
            }

            cursor.topOps_ = ops;
        } else if (!cursor.frames_[index - 1]
                        .target.FieldOps(cursor.frames_[index].schemaIndex)
                        .path) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::NoPathAccess);
        }
    }

    // 5. 참조 값(강한·weak)의 lease(FieldWriteOperation과 같은 순서 — 기대 타입, non-null 대상).
    // 완료까지 쥔다.
    cursor.current_ = last;
    const FieldValue* value =
        operation == PathOperation::Write || operation == PathOperation::Insert ? cursor.input_
                                                                                : nullptr;
    const bool referenceKind =
        kind == ValueKind::ObjectReference || kind == ValueKind::WeakObjectReference;
    if (value && referenceKind && value->kind == kind) {
        const TypeId expected = endType.type;
        if (expected != TypeId{}) {
            auto lease = cursor.registry_.AcquireRegisteredType(cursor.access_, expected);
            if (!lease) {
                return cursor.Fail(lease.Error());
            }

            cursor.expectedType_ = Move(lease.Value());
        }

        if (!value->reference.IsNull()) {
            // weak 대상이 다른 Registry의 객체면 WrongContext다(FieldWriteOperation과 같다).
            if (kind == ValueKind::WeakObjectReference &&
                !cursor.registry_.OwnsHandle(value->reference)) {
                return cursor.Fail(ErrorCode::WrongContext);
            }

            // staged 객체의 강한 참조는 같은 트랜잭션의 공개 전 객체도 가리킬 수 있다(그 트랜잭션의
            // AcquireType — weak 대상은 게시된 객체만).
            auto found = cursor.construction_ && kind == ValueKind::ObjectReference
                             ? cursor.construction_->AcquireType(value->reference)
                             : cursor.registry_.AcquireType(cursor.access_, value->reference);
            if (!found) {
                return cursor.Fail(found.Error());
            }

            cursor.referencedType_ = Move(found.Value());
        }
    }

    // 6. 단일 검증 지점.
    FieldWriteTarget target;
    target.actual = end.field;
    target.effectiveFlags = end.effectiveFlags;
    target.actualEnum = end.enumeration;
    target.targetExpectedNative =
        cursor.expectedType_.IsValid() ? cursor.expectedType_.Ops().nativeType : nullptr;
    target.referenced = cursor.referencedType_.IsValid() ? &cursor.referencedType_ : nullptr;
    target.element = end.selector != PathSelector::None;
    if (value) {
        FieldWriteBinding binding;
        binding.value = endType;
        binding.optional =
            end.selector == PathSelector::None && end.field->value.kind == ValueKind::Optional;
        if (auto valid = ValidateFieldWrite(binding, target, *value); !valid) {
            return cursor.Fail(valid.Error(), WriteReason(valid.Error(), end.effectiveFlags));
        }

        // Name 토큰은 이 Registry의 pool 것이어야 한다(FieldWriteOperation과 같은 검사 — struct
        // 멤버·배열 원소의 생성 thunk는 토큰을 그대로 대입하므로 여기가 검사 지점이다). Optional을
        // unset으로 되돌리는 쓰기는 토큰을 보지 않는다.
        if (value->kind == ValueKind::Name && value->present) {
            if (auto valid = ValidateNameFieldToken(cursor.registry_.Names(), value->name,
                                                    value->GetAllocator());
                !valid) {
                return cursor.Fail(valid.Error(), WriteReason(valid.Error(), end.effectiveFlags));
            }
        }

        // ClassRef 값은 이 Registry에 등록된 클래스이고 스키마 Base의 파생이어야 한다
        // (FieldWriteOperation과 같은 검사 — 리플렉션 B9d2). 그 lease는 완료까지 쥐고 생성 thunk가
        // 정적 Base를 대조한다(CheckClassTarget).
        if (value->kind == ValueKind::ClassReference && value->guid != Guid{}) {
            const TypeId base = endType.type;
            // Deprecated 클래스 거부는 지금 값을 아는 생성 thunk가 한다(같은 값의 재기록은 받는다 —
            // CheckClassTarget, 최상위 단일 값은 등록된 write).
            auto found = CheckClassReferenceLease(
                cursor.registry_.AcquireRegisteredType(cursor.access_, value->guid), base, nullptr);
            if (!found) {
                return cursor.Fail(found.Error(), WriteReason(found.Error(), end.effectiveFlags));
            }

            cursor.referencedType_ = Move(found.Value());
        }
    }

    const bool structural =
        operation == PathOperation::Insert || operation == PathOperation::Remove ||
        operation == PathOperation::Clear || operation == PathOperation::InsertKey ||
        operation == PathOperation::EraseKey;
    if (structural) {
        const ArrayEdit edit =
            operation == PathOperation::Insert || operation == PathOperation::InsertKey
                ? ArrayEdit::Insert
            : operation == PathOperation::Clear ? ArrayEdit::Clear
                                                : ArrayEdit::Remove;
        if (auto valid = ValidateArrayEdit(target, edit); !valid) {
            return cursor.Fail(valid.Error(), valid.Error() == ErrorCode::Unsupported
                                                  ? PropertyPathError::ReadOnly
                                                  : PropertyPathError::NotArray);
        }
    }

    if (optionalEdit) {
        if (auto valid = ValidateOptionalEdit(target); !valid) {
            return cursor.Fail(valid.Error(), valid.Error() == ErrorCode::Unsupported
                                                  ? PropertyPathError::ReadOnly
                                                  : PropertyPathError::NotOptional);
        }
    }

    // InstancedStruct 형 바꾸기·비우기(리플렉션 B9j): 유효 플래그 ReadOnly는 Unsupported(ReadOnly).
    // 형 바꾸기의 새 타입은 이 Registry의 TypeRegistry에 등록된 struct(미등록 NotFound, struct가
    // 아닌 레코드 InvalidArgument)이고 담기 조건과 깊이 합(끝 단계의 위치 깊이 + 그 struct 깊이 ≤
    // MaxStructDepth — 위치 깊이는 끝 단계 앞의 struct 단계 수, struct 값 시작 위치는 1을 더한다)을
    // 지켜야 한다(InvalidArgument). 사유는 InvalidValue다. lease는 thunk가 넘겨받는다.
    if (instancedEdit) {
        if (auto valid = ValidateInstancedEdit(target); !valid) {
            return cursor.Fail(valid.Error(), valid.Error() == ErrorCode::Unsupported
                                                  ? PropertyPathError::ReadOnly
                                                  : PropertyPathError::NotInstanced);
        }
    }

    if (operation == PathOperation::SetInstancedType) {
        auto lease =
            cursor.registry_.Types().AcquireValueType(cursor.instancedRequest_, TypeKind::Struct);
        if (!lease) {
            return cursor.Fail(lease.Error() == ErrorCode::TypeMismatch ? ErrorCode::InvalidArgument
                                                                        : lease.Error(),
                               PropertyPathError::InvalidValue);
        }

        // 상자(분리된 값) 위 경로는 상자가 기록한 원래 자리의 깊이에서 시작한다.
        const uint32 position =
            static_cast<uint32>(last) + (cursor.root_ == PathTarget::Kind::Struct ? 1u : 0u) +
            (cursor.root_ == PathTarget::Kind::Detached ? cursor.rootBox_->position_ : 0u);
        if (auto allowed = CheckInstancedStructContent(lease.Value().Struct()); !allowed) {
            return cursor.Fail(allowed.Error(), PropertyPathError::InvalidValue);
        }

        // InstancedStruct<Base>(리플렉션 B9k)는 Base 자신이나 그 파생만 받는다.
        if (endType.type != TypeId{} && !lease.Value().IsA(endType.type)) {
            return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
        }

        if (auto fits = CheckInstancedStructDepth(position, lease.Value().Struct()); !fits) {
            return cursor.Fail(fits.Error(), PropertyPathError::InvalidValue);
        }

        cursor.instancedInput_ = Move(lease.Value());
    }

    // 최상위 단일 값은 등록된 read·write 한 경로다(수동 함수표·동적 필드도 같다). 쓰기는 위
    // 검증(5·6단계)을 previous read·할당보다 먼저 거친다 — 오류 순서상 ValidateFieldWrite가
    // OutOfMemory보다 먼저다(등록된 write는 같은 검증을 다시 한다).
    if (topValue) {
        return operation == PathOperation::Read ? RunTopRead(cursor) : RunTopWrite(cursor);
    }

    // 검증 hook 우회(Docs/Design/CurveData.ko.md 9.1절·17.2절): 객체(게시·staged)의 변경이 hook
    // 있는 struct 안을 바꾸거나(가장 바깥 hook struct 단계), 값을 만드는 연산(원소·Map 값·Optional
    // 내부· InstancedStruct 내용)이 트리에 hook이 있는 struct 기본값을 만들면(끝 단계) 연산하지
    // 않고 그 단계를 남긴다 — 진입점이 그 자리를 상자로 분리해 연산·검증한 뒤 게시한다. 끝 지점
    // 분류·검증(값싼 거부) 뒤, Borrow 직전이다. 쓰기 접근 판정(Busy·Destroyed·StaleHandle)은 제자리
    // 경로와 같은 순서·코드로 먼저 본다. 참조 자리·준비 사본·원소 준비물은 hook struct 안에
    // 없다(hook struct는 참조가 없다).
    if (cursor.redirectHooks_ && objectRoot && IsMutation(operation) && !cursor.reference_ &&
        !cursor.stage_ && !cursor.elements_) {
        usize hook = FindHookStep(cursor.frames_, steps.Size(), steps.Size());
        if (hook == IndexNone) {
            // Registry 컨테이너(StructRefArray — 참조 struct 원소)는 상자로 분리할 수 없으므로 원소
            // 기본값은 제자리로 만든다(그 원소의 hook 멤버는 대입 묶음의 추가 원소 검증과 뒤 쓰기가
            // 본다).
            const bool structEnd = kind == ValueKind::Struct && !end.target.IsEmpty() &&
                                   !end.target.ContainsStrongReference() &&
                                   end.target.StructOps().validateTree != nullptr;
            const bool creates =
                ((operation == PathOperation::Insert ||
                  (operation == PathOperation::InsertKey && !setElement) ||
                  operation == PathOperation::EmplaceOptional) &&
                 structEnd) ||
                (operation == PathOperation::SetInstancedType && cursor.instancedInput_.IsValid() &&
                 cursor.instancedInput_.StructOps().validateTree != nullptr);
            hook = creates ? last : IndexNone;
        }

        if (hook != IndexNone) {
            // staged는 쓰기 접근 판정(Required 검사 중 Busy 등)을 먼저 본다. 게시된 객체는 쓰기
            // Borrow가 변경 표지를 올리므로 먼저 얻지 않는다 — encode 고정 Busy는 묶음 게시 구간이,
            // 파괴·무효 handle은 분리(읽기 Borrow)가 같은 코드로 낸다.
            cursor.current_ = 0;
            if (cursor.root_ == PathTarget::Kind::Staged) {
                auto probe = cursor.construction_->GetForWrite<Object>(cursor.owner_);
                if (!probe) {
                    return cursor.Fail(probe.Error());
                }
            }

            cursor.hookStep_ = hook;
            return Result<void>::Success();
        }
    }

    // 게시 전 원소 블록을 바꾸면 준비물의 추가 원소 검증 표시를 내린다(게시 전에 다시 검증).
    if (IsMutation(operation)) {
        cursor.MarkAddedElementWritten();
    }

    // 7. Borrow(쓰기는 ResolveForWrite — encode 고정 중이면 Busy)와 읽기 고정. Detach·Attach는
    // 대상을 바꾸지 않으므로 읽기 Borrow다(변경 표지를 올리지 않는다 — 대입 묶음이 표지를 읽은 뒤
    // 준비한다). staged 객체는 트랜잭션의 Get(쓰기는 GetForWrite), 분리된 값은 Borrow가 없다.
    cursor.current_ = 0;
    const bool reading = cursor.IsReading() || boxOperation || cursor.reference_ != nullptr ||
                         cursor.stage_ != nullptr;
    if (cursor.root_ == PathTarget::Kind::Object) {
        auto borrowed =
            reading ? cursor.registry_.Resolve<Object>(cursor.access_, cursor.owner_)
                    : cursor.registry_.ResolveForWrite<Object>(cursor.access_, cursor.owner_);
        if (!borrowed) {
            return cursor.Fail(borrowed.Error());
        }

        cursor.borrow_ = Move(borrowed.Value());
        auto object = cursor.borrow_.Get();
        if (!object) {
            return cursor.Fail(object.Error());
        }

        cursor.object_ = object.Value();
    } else if (cursor.root_ == PathTarget::Kind::Staged) {
        auto object = reading ? cursor.construction_->Get<Object>(cursor.owner_)
                              : cursor.construction_->GetForWrite<Object>(cursor.owner_);
        if (!object) {
            return cursor.Fail(object.Error());
        }

        cursor.object_ = object.Value();
    }

    if (operation == PathOperation::Read || operation == PathOperation::Detach ||
        operation == PathOperation::ReadKeys) {
        if (auto pinned = cursor.Pin(); !pinned) {
            return pinned;
        }
    }

    // 8. 생성 thunk 실행(필드 하나에 thunk 하나 — 끝이 아니면 Descend). 분리된 값은 struct 값의 첫
    // 멤버 thunk 또는 상자의 진입 함수다.
    Result<void> result = Result<void>::Success();
    if (cursor.root_ == PathTarget::Kind::Detached) {
        result = cursor.rootBox_->enter_(cursor, cursor.rootBox_->storage_);
    } else if (cursor.root_ == PathTarget::Kind::Struct) {
        const RuntimeStructFieldOps& member =
            cursor.rootView_.FieldOps(cursor.frames_[0].schemaIndex);
        result = member.path(member, cursor, cursor.rootValue_);
    } else {
        result = cursor.topOps_->path(*cursor.topOps_, cursor);
    }

    if (!result) {
        cursor.Record(result.Error(), PropertyPathError::None);
    }

    return result;
}

} // namespace detail

namespace {

using detail::PathOperation;
using detail::PathRunner;

// 객체(게시·staged)의 변경이 검증 hook 있는 struct(단계 hook) 안을 바꾸거나 hook 트리가 있는 값을
// 만드는 경로다(Docs/Design/CurveData.ko.md 9.1절·17.2절): 그 자리(그 단계 — 원소·Map 값이면
// 선택자를 뗀 컨테이너 필드)를 상자로 분리하고, 상자 위에서 같은 연산을 한 뒤 원래 자리와 맞바꾼다.
// 상자 값 트리의 검증은 PathPrepareAttach가 한다(거부는 InvalidArgument(StructValidation)). 게시된
// 객체는 묶음 게시 구간이다(변경 표지·관찰자 규칙은 대입 묶음과 같다). staged 객체는 분리 전부터
// 게시 직전 사이에 그 트랜잭션의 staged 쓰기가 있었으면(분리 중을 포함한 재진입 변경) Busy로
// 게시하지 않는다. 바뀐 것이 없는 연산(이미 있는 Set 원소의 InsertKey)은 게시하지 않는다. 실패하면
// 객체와 출력 인자(output·length)는 그대로다 — 출력은 지역 값에 받아 두고 게시가 끝난 뒤 옮긴다.
// 상자 위 연산의 실패 위치는 원래 경로의 단계로 옮겨 알린다.
Result<void> RunThroughHookBox(ObjectRegistry& registry, const ObjectAccessContext& access,
                               const PathTarget& target, PropertyPath path, usize hook,
                               PathOperation operation, const FieldValue* input, FieldValue* output,
                               PropertyPathFailure* failure, usize* length,
                               TypeId instancedRequest) noexcept {
    PathStep unitSteps[MaxPropertyPathSteps];
    for (usize index = 0; index <= hook; ++index) {
        unitSteps[index] = path.steps[index];
    }

    unitSteps[hook].selector = PathSelector::None;
    unitSteps[hook].index = 0;
    const PropertyPath unit{Span<const PathStep>(unitSteps, hook + 1), path.keys};
    const bool live = target.GetKind() == PathTarget::Kind::Object;
    uint64 stamp = 0;
    if (live) {
        auto read = registry.MutationStamp(access, target.Object());
        if (!read) {
            return Result<void>::Failure(read.Error());
        }

        stamp = read.Value();
    }

    // staged 쓰기 누계는 분리 전에 잰다: staged는 Pin이 무동작이라 분리(깊은 복사)의 할당 콜백이
    // 같은 트랜잭션에 쓴 변경도 게시 전 대조에 잡혀야 한다(그렇지 않으면 상자 맞바꿈이 그 쓰기를
    // 덮는다).
    ConstructionContext* transaction = target.Transaction();
    const uint64 writes = !live && transaction ? transaction->WriteCount() : 0;
    DetachedValue box(registry.ObjectAllocator());
    if (auto detached = PathDetach(registry, access, target, unit, box, failure); !detached) {
        return detached;
    }

    const PropertyPath inner{
        Span<const PathStep>(path.steps.Data() + hook, path.steps.Size() - hook), path.keys};
    Optional<FieldValue> result;
    if (output) {
        result.Emplace(output->GetAllocator());
    }

    usize produced = 0;
    bool setElement = false;
    {
        detail::PathCursor cursor =
            PathRunner::Make(registry, access, PathTarget::Value(box), operation, input,
                             result.HasValue() ? &result.Value() : nullptr, failure);
        if (operation == PathOperation::SetInstancedType) {
            PathRunner::BindInstancedRequest(cursor, instancedRequest);
        }

        if (auto ran = PathRunner::Run(cursor, inner); !ran) {
            if (failure) {
                failure->step += hook;
            }

            return ran;
        }

        produced = PathRunner::Length(cursor);
        setElement = PathRunner::EndsAtSetElement(cursor);
    }

    // 이미 있는 Set 원소의 InsertKey만 바뀐 것이 없다(게시·변경 표지·관찰자 알림 없음). Map의 이미
    // 있는 키는 값을 기본 값으로 바꾸므로(길이 0이어도) 게시하고 검증한다.
    const bool changed = !(operation == PathOperation::InsertKey && produced == 0 && setElement);
    if (changed) {
        if (!live && transaction && transaction->WriteCount() != writes) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        PreparedAttach prepared;
        if (auto located =
                PathPrepareAttach(registry, access, target, unit, box, prepared, failure);
            !located) {
            return located;
        }

        if (!live) {
            if (auto published = prepared.Publish(); !published) {
                return published;
            }
        } else {
            auto owner = registry.Resolve(access, target.Object());
            if (!owner) {
                return Result<void>::Failure(owner.Error());
            }

            auto batch = registry.BeginBatchPublish(access, target.Object(), stamp, {}, {});
            if (!batch) {
                return Result<void>::Failure(batch.Error());
            }

            prepared.Publish(batch.Value());
            batch.Value().End();
        }
    }

    if (output) {
        *output = Move(result.Value());
    }

    if (length) {
        *length = produced;
    }

    return Result<void>::Success();
}

Result<void> RunOperation(ObjectRegistry& registry, const ObjectAccessContext& access,
                          const PathTarget& target, PropertyPath path, PathOperation operation,
                          const FieldValue* input, FieldValue* output, PropertyPathFailure* failure,
                          usize* length, TypeId instancedRequest = {}) noexcept {
    usize hook = IndexNone;
    {
        detail::PathCursor cursor =
            PathRunner::Make(registry, access, target, operation, input, output, failure);
        PathRunner::RedirectHooks(cursor);
        if (operation == PathOperation::SetInstancedType) {
            PathRunner::BindInstancedRequest(cursor, instancedRequest);
        }

        Result<void> result = PathRunner::Run(cursor, path);
        if (!result) {
            return result;
        }

        hook = PathRunner::HookStep(cursor);
        if (hook == IndexNone) {
            if (length) {
                *length = PathRunner::Length(cursor);
            }

            return result;
        }
    }

    return RunThroughHookBox(registry, access, target, path, hook, operation, input, output,
                             failure, length, instancedRequest);
}

// 결과가 Length 칸의 0·1인 연산(PathContains·PathInsertKey·PathIsOptionalSet)이다.
Result<bool> RunQuery(ObjectRegistry& registry, const ObjectAccessContext& access,
                      const PathTarget& target, PropertyPath path, PathOperation operation,
                      PropertyPathFailure* failure) noexcept {
    usize answer = 0;
    auto result =
        RunOperation(registry, access, target, path, operation, nullptr, nullptr, failure, &answer);
    if (!result) {
        return Result<bool>::Failure(result.Error());
    }

    return Result<bool>::Success(answer != 0);
}

} // namespace

Result<void> PathRead(ObjectRegistry& registry, const ObjectAccessContext& access,
                      const PathTarget& target, PropertyPath path, FieldValue& out,
                      PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, target, path, PathOperation::Read, nullptr, &out, failure,
                        nullptr);
}

Result<void> PathWrite(ObjectRegistry& registry, const ObjectAccessContext& access,
                       const PathTarget& target, PropertyPath path, const FieldValue& value,
                       FieldValue* previous, PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, target, path, PathOperation::Write, &value, previous,
                        failure, nullptr);
}

Result<usize> PathCount(ObjectRegistry& registry, const ObjectAccessContext& access,
                        const PathTarget& target, PropertyPath path,
                        PropertyPathFailure* failure) noexcept {
    usize length = 0;
    auto result = RunOperation(registry, access, target, path, PathOperation::Length, nullptr,
                               nullptr, failure, &length);
    if (!result) {
        return Result<usize>::Failure(result.Error());
    }

    return Result<usize>::Success(length);
}

Result<void> PathReadKeys(ObjectRegistry& registry, const ObjectAccessContext& access,
                          const PathTarget& target, PropertyPath path, Array<FieldValue>& out,
                          PropertyPathFailure* failure) noexcept {
    detail::PathCursor cursor = PathRunner::Make(registry, access, target, PathOperation::ReadKeys,
                                                 nullptr, nullptr, failure);
    PathRunner::BindKeys(cursor, out);
    return PathRunner::Run(cursor, path);
}

Result<void> PathInsertElement(ObjectRegistry& registry, const ObjectAccessContext& access,
                               const PathTarget& target, PropertyPath path, const FieldValue* value,
                               PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, target, path, PathOperation::Insert, value, nullptr,
                        failure, nullptr);
}

Result<void> PathRemoveElement(ObjectRegistry& registry, const ObjectAccessContext& access,
                               const PathTarget& target, PropertyPath path, FieldValue* removed,
                               PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, target, path, PathOperation::Remove, nullptr, removed,
                        failure, nullptr);
}

Result<void> PathClear(ObjectRegistry& registry, const ObjectAccessContext& access,
                       const PathTarget& target, PropertyPath path,
                       PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, target, path, PathOperation::Clear, nullptr, nullptr,
                        failure, nullptr);
}

Result<bool> PathContains(ObjectRegistry& registry, const ObjectAccessContext& access,
                          const PathTarget& target, PropertyPath path,
                          PropertyPathFailure* failure) noexcept {
    return RunQuery(registry, access, target, path, PathOperation::Contains, failure);
}

Result<bool> PathInsertKey(ObjectRegistry& registry, const ObjectAccessContext& access,
                           const PathTarget& target, PropertyPath path,
                           PropertyPathFailure* failure) noexcept {
    return RunQuery(registry, access, target, path, PathOperation::InsertKey, failure);
}

Result<void> PathEraseKey(ObjectRegistry& registry, const ObjectAccessContext& access,
                          const PathTarget& target, PropertyPath path, FieldValue* removed,
                          PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, target, path, PathOperation::EraseKey, nullptr, removed,
                        failure, nullptr);
}

Result<void> PathEmplaceOptional(ObjectRegistry& registry, const ObjectAccessContext& access,
                                 const PathTarget& target, PropertyPath path,
                                 PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, target, path, PathOperation::EmplaceOptional, nullptr,
                        nullptr, failure, nullptr);
}

Result<void> PathResetOptional(ObjectRegistry& registry, const ObjectAccessContext& access,
                               const PathTarget& target, PropertyPath path,
                               PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, target, path, PathOperation::ResetOptional, nullptr,
                        nullptr, failure, nullptr);
}

Result<bool> PathIsOptionalSet(ObjectRegistry& registry, const ObjectAccessContext& access,
                               const PathTarget& target, PropertyPath path,
                               PropertyPathFailure* failure) noexcept {
    return RunQuery(registry, access, target, path, PathOperation::QueryOptional, failure);
}

Result<void> PathSetInstancedType(ObjectRegistry& registry, const ObjectAccessContext& access,
                                  const PathTarget& target, PropertyPath path, TypeId type,
                                  PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, target, path, PathOperation::SetInstancedType, nullptr,
                        nullptr, failure, nullptr, type);
}

Result<void> PathClearInstanced(ObjectRegistry& registry, const ObjectAccessContext& access,
                                const PathTarget& target, PropertyPath path,
                                PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, target, path, PathOperation::ClearInstanced, nullptr,
                        nullptr, failure, nullptr);
}

Result<TypeLease> PathReadInstancedType(ObjectRegistry& registry, const ObjectAccessContext& access,
                                        const PathTarget& target, PropertyPath path,
                                        PropertyPathFailure* failure) noexcept {
    TypeLease type;
    detail::PathCursor cursor = PathRunner::Make(
        registry, access, target, PathOperation::QueryInstanced, nullptr, nullptr, failure);
    PathRunner::BindInstancedType(cursor, type);
    if (auto queried = PathRunner::Run(cursor, path); !queried) {
        return Result<TypeLease>::Failure(queried.Error());
    }

    return Result<TypeLease>::Success(Move(type));
}

} // namespace rocks
