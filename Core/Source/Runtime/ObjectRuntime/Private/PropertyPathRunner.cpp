#include "PropertyPathInternal.h"

#include <Object/NameField.h>

// 경로 연산의 실행(detail::PathRunner — 입력 형식·문맥·단계 해석·끝 지점 분류·참조 lease·검증
// 지점·Borrow·생성 thunk 실행)과 공개 연산 진입점(PathRead·PathWrite·PathArrayLength·
// PathInsertElement·PathRemoveElement·PathClearArray)이다.
namespace rocks {

namespace detail {

namespace {

// 경로 thunk가 FieldValue로 표현할 수 없는 값 종류다(성질 표의 aggregate — struct 값·배열 통째).
bool IsAggregateKind(ValueKind kind) noexcept {
    return ValueKindIsAggregate(kind);
}

// 쓰기 검증(ValidateFieldWrite·등록된 write) 오류의 사유다: Unsupported는 유효 플래그에 ReadOnly가
// 있을 때만 ReadOnly(그 밖의 Unsupported — 수동 callback의 정책 등 — 는 None), 값 종류·enum
// ID(TypeMismatch)와 값 내용(InvalidArgument)은 InvalidValue다.
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

Result<void> PathRunner::Run(PathCursor& cursor, PropertyPath path) noexcept {
    const Span<const PathStep> steps = path.steps;
    const PathOperation operation = cursor.operation_;
    // 1. 입력 형식.
    if (steps.Size() == 0) {
        return cursor.Fail(ErrorCode::InvalidArgument);
    }

    if (steps.Size() > MaxPropertyPathSteps) {
        cursor.current_ = MaxPropertyPathSteps;
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::TooDeep);
    }

    for (usize index = 0; index < steps.Size(); ++index) {
        if (!IsCanonicalPathStep(steps[index])) {
            cursor.current_ = index;
            return cursor.Fail(ErrorCode::InvalidArgument);
        }
    }

    const usize last = steps.Size() - 1;
    cursor.current_ = last;
    if ((operation == PathOperation::Insert || operation == PathOperation::Remove) &&
        !steps[last].indexed) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::IndexRequired);
    }

    if ((operation == PathOperation::Length || operation == PathOperation::Clear) &&
        steps[last].indexed) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::IndexNotAllowed);
    }

    if ((operation == PathOperation::Write || operation == PathOperation::Insert) &&
        cursor.input_ && cursor.input_->kind == ValueKind::Opaque) {
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
    }

    // 2. 문맥·수명(소유 스레드·문맥·생존 — staged·DestroyRequested 객체는 거부된다). 소유 타입
    // lease는 완료까지 쥔다(해석한 스키마·함수표·StructView의 수명).
    cursor.current_ = 0;
    auto type = cursor.registry_.AcquireType(cursor.access_, cursor.owner_);
    if (!type) {
        return cursor.Fail(type.Error());
    }

    cursor.ownerType_ = Move(type.Value());
    // 3. 단계 순서의 해석: FieldId → 스키마 위치(정렬 색인), 모양, 유효 플래그 누적.
    for (usize index = 0; index < steps.Size(); ++index) {
        cursor.current_ = index;
        const PathStep& step = steps[index];
        PathFrame& frame = cursor.frames_[index];
        usize position = TypeLease::NoField;
        const FieldSchema* field = nullptr;
        if (index == 0) {
            position = cursor.ownerType_.FieldIndex(step.fieldId);
            if (position == TypeLease::NoField) {
                return cursor.Fail(ErrorCode::NotFound, PropertyPathError::UnknownField);
            }

            field = &cursor.ownerType_.Schema().fields[position];
            frame.enumeration = cursor.ownerType_.FieldEnum(position);
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
            frame.target = parent.target.FieldStruct(position);
            frame.effectiveFlags = PropagateFieldFlags(parent.effectiveFlags, field->flags);
        }

        frame.field = field;
        frame.schemaIndex = position;
        frame.index = step.index;
        frame.indexed = step.indexed;
        cursor.count_ = index + 1;
        if (step.indexed && field->value.kind != ValueKind::Array) {
            return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotArray);
        }

        if (index < last) {
            const ValueKind kind = step.indexed ? field->element.kind : field->value.kind;
            if (kind != ValueKind::Struct || frame.target.IsEmpty()) {
                return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct);
            }
        }
    }

    cursor.current_ = last;
    const PathFrame& end = cursor.frames_[last];
    if ((operation == PathOperation::Length || operation == PathOperation::Clear) &&
        end.field->value.kind != ValueKind::Array) {
        return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotArray);
    }

    // 4. 끝 지점 분류: struct·배열 통째 접근, 값 유무, thunk 유무(Unsupported).
    const ValueKind kind = end.indexed ? end.field->element.kind : end.field->value.kind;
    const bool topValue = steps.Size() == 1 && !IsAggregateKind(end.field->value.kind);
    switch (operation) {
    case PathOperation::Read:
    case PathOperation::Write:
        if (kind == ValueKind::Opaque) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::NoPathAccess);
        }

        if (IsAggregateKind(kind)) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
        }

        break;
    case PathOperation::Insert:
        if (kind == ValueKind::Struct && cursor.input_) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
        }

        if (kind != ValueKind::Struct && !cursor.input_) {
            return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
        }

        break;
    case PathOperation::Remove:
        if (kind == ValueKind::Struct && cursor.output_) {
            return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::AggregateValue);
        }

        break;
    default:
        break;
    }

    for (usize index = 0; index < steps.Size(); ++index) {
        cursor.current_ = index;
        if (index == 0) {
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
        const TypeId expected = end.indexed ? end.field->element.type : end.field->value.type;
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

            auto found = cursor.registry_.AcquireType(cursor.access_, value->reference);
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
    target.element = end.indexed;
    if (value) {
        FieldWriteBinding binding;
        binding.value = end.indexed ? end.field->element : end.field->value;
        if (auto valid = ValidateFieldWrite(binding, target, *value); !valid) {
            return cursor.Fail(valid.Error(), WriteReason(valid.Error(), end.effectiveFlags));
        }

        // Name 토큰은 이 Registry의 pool 것이어야 한다(FieldWriteOperation과 같은 검사 — struct
        // 멤버·배열 원소의 생성 thunk는 토큰을 그대로 대입하므로 여기가 검사 지점이다).
        if (value->kind == ValueKind::Name) {
            if (auto valid = ValidateNameFieldToken(cursor.registry_.Names(), value->name,
                                                    value->GetAllocator());
                !valid) {
                return cursor.Fail(valid.Error(), WriteReason(valid.Error(), end.effectiveFlags));
            }
        }
    }

    if (operation == PathOperation::Insert || operation == PathOperation::Remove ||
        operation == PathOperation::Clear) {
        const ArrayEdit edit = operation == PathOperation::Insert   ? ArrayEdit::Insert
                               : operation == PathOperation::Remove ? ArrayEdit::Remove
                                                                    : ArrayEdit::Clear;
        if (auto valid = ValidateArrayEdit(target, edit); !valid) {
            return cursor.Fail(valid.Error(), valid.Error() == ErrorCode::Unsupported
                                                  ? PropertyPathError::ReadOnly
                                                  : PropertyPathError::NotArray);
        }
    }

    // 최상위 단일 값은 등록된 read·write 한 경로다(수동 함수표·동적 필드도 같다). 쓰기는 위
    // 검증(5·6단계)을 previous read·할당보다 먼저 거친다 — 오류 순서상 ValidateFieldWrite가
    // OutOfMemory보다 먼저다(등록된 write는 같은 검증을 다시 한다).
    if (topValue) {
        return operation == PathOperation::Read ? RunTopRead(cursor) : RunTopWrite(cursor);
    }

    // 7. Borrow(쓰기는 ResolveForWrite — encode 고정 중이면 Busy)와 읽기 고정.
    cursor.current_ = 0;
    const bool reading = operation == PathOperation::Read || operation == PathOperation::Length;
    auto borrowed = reading
                        ? cursor.registry_.Resolve<Object>(cursor.access_, cursor.owner_)
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
    if (operation == PathOperation::Read) {
        if (auto pinned = cursor.Pin(); !pinned) {
            return pinned;
        }
    }

    // 8. 생성 thunk 실행(필드 하나에 thunk 하나 — 끝이 아니면 Descend).
    Result<void> result = cursor.topOps_->path(*cursor.topOps_, cursor);
    if (!result) {
        cursor.Record(result.Error(), PropertyPathError::None);
    }

    return result;
}

} // namespace detail

namespace {

using detail::PathOperation;
using detail::PathRunner;

Result<void> RunOperation(ObjectRegistry& registry, const ObjectAccessContext& access,
                          ObjectHandle handle, PropertyPath path, PathOperation operation,
                          const FieldValue* input, FieldValue* output, PropertyPathFailure* failure,
                          usize* length) noexcept {
    detail::PathCursor cursor =
        PathRunner::Make(registry, access, handle, operation, input, output, failure);
    Result<void> result = PathRunner::Run(cursor, path);
    if (result && length) {
        *length = PathRunner::Length(cursor);
    }

    return result;
}

} // namespace

Result<void> PathRead(ObjectRegistry& registry, const ObjectAccessContext& access,
                      ObjectHandle handle, PropertyPath path, FieldValue& out,
                      PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, handle, path, PathOperation::Read, nullptr, &out, failure,
                        nullptr);
}

Result<void> PathWrite(ObjectRegistry& registry, const ObjectAccessContext& access,
                       ObjectHandle handle, PropertyPath path, const FieldValue& value,
                       FieldValue* previous, PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, handle, path, PathOperation::Write, &value, previous,
                        failure, nullptr);
}

Result<usize> PathArrayLength(ObjectRegistry& registry, const ObjectAccessContext& access,
                              ObjectHandle handle, PropertyPath path,
                              PropertyPathFailure* failure) noexcept {
    usize length = 0;
    auto result = RunOperation(registry, access, handle, path, PathOperation::Length, nullptr,
                               nullptr, failure, &length);
    if (!result) {
        return Result<usize>::Failure(result.Error());
    }

    return Result<usize>::Success(length);
}

Result<void> PathInsertElement(ObjectRegistry& registry, const ObjectAccessContext& access,
                               ObjectHandle handle, PropertyPath path, const FieldValue* value,
                               PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, handle, path, PathOperation::Insert, value, nullptr,
                        failure, nullptr);
}

Result<void> PathRemoveElement(ObjectRegistry& registry, const ObjectAccessContext& access,
                               ObjectHandle handle, PropertyPath path, FieldValue* removed,
                               PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, handle, path, PathOperation::Remove, nullptr, removed,
                        failure, nullptr);
}

Result<void> PathClearArray(ObjectRegistry& registry, const ObjectAccessContext& access,
                            ObjectHandle handle, PropertyPath path,
                            PropertyPathFailure* failure) noexcept {
    return RunOperation(registry, access, handle, path, PathOperation::Clear, nullptr, nullptr,
                        failure, nullptr);
}

} // namespace rocks
