#include <Object/PathCursor.h>
#include <Object/SoftObjectRef.h>
#include <Platform/Fatal.h>

namespace rocks {

namespace detail {

// 경로 연산의 실행 순서다(Docs/Design/ReflectionFoundations.ko.md 7.1절 "오류 순서"):
// 입력 형식 → 문맥과 수명 → 단계 순서의 해석 → 끝 지점 분류(통째, thunk 없음) → 참조 값
// lease → 단일 검증 지점 → Borrow와 고정 → 생성 thunk 실행(정적 대조, index, 길이, 할당,
// 게시 직전 재확인).
struct PathRunner {
    static Result<void> Run(PathCursor& cursor, PropertyPath path) noexcept;

    static Result<void> RunTopRead(PathCursor& cursor) noexcept;
    static Result<void> RunTopWrite(PathCursor& cursor) noexcept;

    static PathCursor Make(ObjectRegistry& registry, const ObjectAccessContext& access,
                           ObjectHandle owner, PathOperation operation, const FieldValue* input,
                           FieldValue* output, PropertyPathFailure* failure) noexcept {
        return PathCursor(registry, access, owner, operation, input, output, failure);
    }

    static usize Length(const PathCursor& cursor) noexcept {
        return cursor.length_;
    }
};

namespace {

// 경로 thunk가 FieldValue로 표현할 수 없는 값 종류다(struct 값·배열 통째).
bool IsAggregateKind(ValueKind kind) noexcept {
    return kind == ValueKind::Struct || kind == ValueKind::Array;
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

PathCursor::PathCursor(ObjectRegistry& registry, const ObjectAccessContext& access,
                       ObjectHandle owner, PathOperation operation, const FieldValue* input,
                       FieldValue* output, PropertyPathFailure* failure) noexcept
    : registry_(registry), access_(access), owner_(owner), operation_(operation), input_(input),
      output_(output), failure_(failure) {
    if (failure_) {
        *failure_ = {};
    }
}

void PathCursor::Record(ErrorCode code, PropertyPathError reason) noexcept {
    // 실패하면 고정을 곧바로 푼다 — 호출자 지역의 임시 버퍼(새 값·옛 값 사본·새 원소·새 버퍼)는 그
    // 뒤에 반환된다(게시 경로의 ReleasePin과 같은 시점 규칙).
    pin_.Reset();
    if (failed_ || code == ErrorCode::None) {
        return;
    }

    failed_ = true;
    if (failure_) {
        failure_->reason = reason;
        failure_->step = current_;
        failure_->offset = 0;
    }
}

Result<void> PathCursor::Fail(ErrorCode code, PropertyPathError reason) noexcept {
    Record(code, reason);
    return Result<void>::Failure(code);
}

Result<void> PathCursor::FailValue(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::TypeMismatch:
        return Fail(code, PropertyPathError::StaticTypeMismatch);
    case ErrorCode::InvalidArgument:
        return Fail(code, PropertyPathError::InvalidValue);
    default:
        return Fail(code);
    }
}

Result<void> PathCursor::Finish(Result<void> result) noexcept {
    if (!result) {
        Record(result.Error(), PropertyPathError::None);
    }

    return result;
}

Result<Object*> PathCursor::Owner(const void* ownerNative) noexcept {
    if (!object_ || !ownerType_.IsNativeA(ownerNative)) {
        Record(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
        return Result<Object*>::Failure(ErrorCode::TypeMismatch);
    }

    return Result<Object*>::Success(object_);
}

Result<void> PathCursor::CheckTarget(ValueKind target, bool container) noexcept {
    const FieldSchema& field = *Frame().field;
    if (FieldTargetType(field).kind != target ||
        (field.value.kind == ValueKind::Array) != container) {
        return Fail(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
    }

    return Result<void>::Success();
}

Result<void> PathCursor::CheckEnum(TypeId enumeration) noexcept {
    if (!(FieldTargetType(*Frame().field).type == enumeration)) {
        return Fail(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
    }

    return Result<void>::Success();
}

Result<void> PathCursor::CheckStruct(const void* nativeType) noexcept {
    const StructView target = Frame().target;
    if (target.IsEmpty() || target.StructOps().nativeType != nativeType) {
        return Fail(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
    }

    return Result<void>::Success();
}

Result<void> PathCursor::CheckMemberOwner(const void* structNative) noexcept {
    if (current_ == 0 || frames_[current_ - 1].target.IsEmpty() ||
        frames_[current_ - 1].target.StructOps().nativeType != structNative) {
        return Fail(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
    }

    return Result<void>::Success();
}

Result<void> PathCursor::Descend(void* structValue) noexcept {
    if (!structValue || current_ + 1 >= count_ || frames_[current_].target.IsEmpty()) {
        return Fail(ErrorCode::InvalidArgument);
    }

    const RuntimeStructFieldOps& member =
        frames_[current_].target.FieldOps(frames_[current_ + 1].schemaIndex);
    if (!member.path) {
        ++current_;
        auto failed = Fail(ErrorCode::Unsupported, PropertyPathError::NoPathAccess);
        --current_;
        return failed;
    }

    ++current_;
    Result<void> result = member.path(member, *this, structValue);
    if (!result) {
        Record(result.Error(), PropertyPathError::None);
    }

    --current_;
    return result;
}

Result<void> PathCursor::CheckIndex(usize size) noexcept {
    const PathFrame& frame = Frame();
    const bool insertion = operation_ == PathOperation::Insert && IsLast();
    if (!frame.indexed || frame.index > size || (!insertion && frame.index == size)) {
        return Fail(ErrorCode::InvalidArgument, PropertyPathError::IndexOutOfRange);
    }

    return Result<void>::Success();
}

Result<void> PathCursor::CheckLength(usize newLength) noexcept {
    if (auto fits = ValidateArrayLength(*Frame().field, newLength); !fits) {
        return Fail(fits.Error(), fits.Error() == ErrorCode::CapacityExceeded
                                      ? PropertyPathError::TooManyElements
                                      : PropertyPathError::NotArray);
    }

    return Result<void>::Success();
}

Result<void> PathCursor::Pin() noexcept {
    if (pin_.IsValid()) {
        return Result<void>::Success();
    }

    auto pin = registry_.PinForEncode(access_, owner_);
    if (!pin) {
        return Fail(pin.Error());
    }

    pin_ = Move(pin.Value());
    return Result<void>::Success();
}

Result<void> PathCursor::CheckOwner() noexcept {
    auto alive = borrow_.Get();
    if (!alive) {
        return Fail(alive.Error());
    }

    return Result<void>::Success();
}

Result<void> PathCursor::PublishRead(FieldValue& value) noexcept {
    if (auto alive = CheckOwner(); !alive) {
        return alive;
    }

    pin_.Reset();
    *output_ = Move(value);
    return Result<void>::Success();
}

void PathCursor::PublishOutput(FieldValue& value) noexcept {
    if (output_) {
        *output_ = Move(value);
    }
}

void PathCursor::PublishReference(ObjectHandle previous) noexcept {
    if (!output_) {
        return;
    }

    // 참조 값의 문자열은 비어 있으므로 할당하지 않는다.
    FieldValue captured(output_->GetAllocator());
    captured.kind = ValueKind::ObjectReference;
    captured.reference = previous;
    *output_ = Move(captured);
}

Result<void> PathCursor::ReadText(const String& text, const AssetId* asset) noexcept {
    FieldValue candidate(output_->GetAllocator());
    candidate.kind = asset ? ValueKind::SoftObjectReference : ValueKind::Utf8String;
    if (asset) {
        candidate.guid = asset->value;
    }

    // Read는 처음부터 고정을 쥐므로 이 할당의 콜백이 text를 바꾸면 Busy다.
    if (auto copied = candidate.string.TryAssign(text.View()); !copied) {
        return Fail(copied.Error());
    }

    return PublishRead(candidate);
}

Result<void> PathCursor::CaptureText(const String& text, const AssetId* asset, FieldValue& captured,
                                     bool& moveText) noexcept {
    captured.kind = asset ? ValueKind::SoftObjectReference : ValueKind::Utf8String;
    if (asset) {
        captured.guid = asset->value;
    }

    if (&captured.GetAllocator() == &text.GetAllocator()) {
        // 게시 뒤 옛 버퍼를 옮긴다(추가 할당 없음).
        moveText = true;
        return Result<void>::Success();
    }

    // 다른 allocator(CU4 Undo 기록)는 고정 아래 게시 전에 복사한다 — Undo 기록이 객체 allocator의
    // 버퍼를 쥐지 않게.
    moveText = false;
    if (auto pinned = Pin(); !pinned) {
        return pinned;
    }

    if (auto copied = captured.string.TryAssign(text.View()); !copied) {
        return Fail(copied.Error());
    }

    return CheckOwner();
}

Result<void> PathCursor::WriteText(String& text, AssetId* asset) noexcept {
    const FieldValue& value = *input_;
    if (asset) {
        // 검증 지점(ValidateFieldWrite)이 이미 본 규칙이다 — thunk도 같은 규칙을 지킨다.
        if (auto valid = SoftObjectRef<>::Validate(AssetId{value.guid}, value.string.View());
            !valid) {
            return Fail(valid.Error(), PropertyPathError::InvalidValue);
        }
    }

    if (auto pinned = Pin(); !pinned) {
        return pinned;
    }

    String replacement(text.GetAllocator());
    if (auto copied = replacement.TryAssign(value.string.View()); !copied) {
        return Fail(copied.Error());
    }

    FieldValue captured(output_ ? output_->GetAllocator() : text.GetAllocator());
    bool moveText = false;
    if (output_) {
        const AssetId previousAsset = asset ? *asset : AssetId{};
        if (auto prepared = CaptureText(text, asset ? &previousAsset : nullptr, captured, moveText);
            !prepared) {
            return prepared;
        }
    }

    if (auto alive = CheckOwner(); !alive) {
        return alive;
    }

    // 게시: 할당·반환 없는 Swap이다. 옛 버퍼(replacement)는 고정을 푼 뒤 반환한다.
    if (asset) {
        *asset = AssetId{value.guid};
    }

    text.Swap(replacement);
    pin_.Reset();
    if (output_) {
        if (moveText) {
            captured.string.Swap(replacement);
        }

        *output_ = Move(captured);
    }

    return Result<void>::Success();
}

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

    // 5. 참조 값의 lease(FieldWriteOperation과 같은 순서 — 기대 타입, non-null 대상). 완료까지
    // 쥔다.
    cursor.current_ = last;
    const FieldValue* value =
        operation == PathOperation::Write || operation == PathOperation::Insert ? cursor.input_
                                                                                : nullptr;
    if (value && kind == ValueKind::ObjectReference && value->kind == ValueKind::ObjectReference) {
        const TypeId expected = end.indexed ? end.field->element.type : end.field->value.type;
        if (expected != TypeId{}) {
            auto lease = cursor.registry_.AcquireRegisteredType(cursor.access_, expected);
            if (!lease) {
                return cursor.Fail(lease.Error());
            }

            cursor.expectedType_ = Move(lease.Value());
        }

        if (!value->reference.IsNull()) {
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

bool IsIdentifierStart(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool IsIdentifierPart(char c) noexcept {
    return IsIdentifierStart(c) || (c >= '0' && c <= '9');
}

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// 경로가 struct 값으로 끝나는 단계인지다(다음 단계 `.`를 받을 수 있다 — index 없는 배열은 아니다).
bool IsStructStep(const FieldSchema& field, bool indexed) noexcept {
    return indexed ? field.value.kind == ValueKind::Array && field.element.kind == ValueKind::Struct
                   : field.value.kind == ValueKind::Struct;
}

usize DecimalDigits(uint32 value) noexcept {
    usize digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }

    return digits;
}

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

Result<void> ParsePropertyPath(const TypeLease& type, StringView text, PropertyPathBuffer& out,
                               PropertyPathFailure* failure) noexcept {
    const auto fail = [failure](ErrorCode code, PropertyPathError reason, usize step,
                                usize offset) noexcept {
        if (failure) {
            *failure = {reason, step, offset};
        }

        return Result<void>::Failure(code);
    };

    if (failure) {
        *failure = {};
    }

    if (!type.IsValid() || type.Kind() == TypeKind::Enum) {
        return fail(ErrorCode::InvalidArgument, PropertyPathError::None, 0, 0);
    }

    const char* data = text.Data();
    const usize size = text.ByteCount();
    PropertyPathBuffer parsed;
    StructView scope;
    usize position = 0;
    while (true) {
        // 이름(ASCII 식별자)을 읽어 지금 범위(클래스·struct lease 스키마 또는 struct view)에서
        // 찾는다.
        const usize nameStart = position;
        if (position >= size || !IsIdentifierStart(data[position])) {
            return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, parsed.count,
                        position);
        }

        while (position < size && IsIdentifierPart(data[position])) {
            ++position;
        }

        const Span<const char> name(data + nameStart, position - nameStart);
        const Span<const FieldSchema> fields =
            parsed.count == 0 ? type.Schema().fields : scope.Schema().fields;
        usize found = TypeLease::NoField;
        for (usize index = 0; index < fields.Size(); ++index) {
            // Opaque 필드는 경로로 닿을 수 없으므로 찾지 않는다.
            if (fields[index].value.kind != ValueKind::Opaque &&
                EqualsIgnoringAsciiCase(EffectiveToolName(fields[index]), name)) {
                found = index;
                break;
            }
        }

        if (found == TypeLease::NoField) {
            return fail(ErrorCode::NotFound, PropertyPathError::UnknownProperty, parsed.count,
                        nameStart);
        }

        const FieldSchema& field = fields[found];
        const StructView target =
            parsed.count == 0 ? type.FieldStruct(found) : scope.FieldStruct(found);
        PathStep& step = parsed.steps[parsed.count];
        step = PathStep{field.id, 0, false};
        const usize stepIndex = parsed.count++;
        if (position < size && data[position] == '[') {
            if (field.value.kind != ValueKind::Array) {
                return fail(ErrorCode::TypeMismatch, PropertyPathError::NotArray, stepIndex,
                            position);
            }

            // 정규 10진 index: `0` 하나 또는 0이 아닌 숫자로 시작, uint32 범위.
            ++position;
            const usize digitsStart = position;
            uint64 value = 0;
            while (position < size && IsDigit(data[position])) {
                value = value * 10 + static_cast<uint64>(data[position] - '0');
                if (value > 0xffffffffull) {
                    return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, stepIndex,
                                digitsStart);
                }

                ++position;
            }

            if (position == digitsStart ||
                (data[digitsStart] == '0' && position - digitsStart > 1)) {
                return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, stepIndex,
                            digitsStart);
            }

            if (position >= size || data[position] != ']') {
                return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, stepIndex,
                            position);
            }

            ++position;
            step.index = static_cast<uint32>(value);
            step.indexed = true;
            // 배열 원소는 배열일 수 없다(중첩 배열 없음).
            if (position < size && data[position] == '[') {
                return fail(ErrorCode::TypeMismatch, PropertyPathError::NotArray, stepIndex,
                            position);
            }
        }

        if (position == size) {
            break;
        }

        if (data[position] != '.') {
            return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, parsed.count,
                        position);
        }

        if (parsed.count == MaxPropertyPathSteps) {
            return fail(ErrorCode::InvalidArgument, PropertyPathError::TooDeep, parsed.count,
                        position);
        }

        if (!IsStructStep(field, step.indexed) || target.IsEmpty()) {
            return fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct, stepIndex, position);
        }

        ++position;
        scope = target;
    }

    out = parsed;
    return Result<void>::Success();
}

Result<void> FormatPropertyPath(const TypeLease& type, PropertyPath path, String& out) noexcept {
    const Span<const PathStep> steps = path.steps;
    if (!type.IsValid() || type.Kind() == TypeKind::Enum || steps.Size() == 0 ||
        steps.Size() > MaxPropertyPathSteps) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Span<const char> names[MaxPropertyPathSteps];
    usize bytes = 0;
    StructView scope;
    for (usize index = 0; index < steps.Size(); ++index) {
        const PathStep& step = steps[index];
        if (!IsCanonicalPathStep(step)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const usize position =
            index == 0 ? type.FieldIndex(step.fieldId) : scope.FieldIndex(step.fieldId);
        if (position == TypeLease::NoField) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        const FieldSchema& field =
            index == 0 ? type.Schema().fields[position] : scope.Schema().fields[position];
        if (field.value.kind == ValueKind::Opaque) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (step.indexed && field.value.kind != ValueKind::Array) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        const StructView target =
            index == 0 ? type.FieldStruct(position) : scope.FieldStruct(position);
        if (index + 1 < steps.Size() && (!IsStructStep(field, step.indexed) || target.IsEmpty())) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        names[index] = EffectiveToolName(field);
        bytes += names[index].Size() + (index != 0 ? 1 : 0) +
                 (step.indexed ? 2 + DecimalDigits(step.index) : 0);
        scope = target;
    }

    String result(out.GetAllocator());
    if (auto reserved = result.TryReserve(bytes); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < steps.Size(); ++index) {
        if (index != 0 && !result.TryAppend(".")) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (!result.TryAppend(StringView(names[index].Data(), names[index].Size()))) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (steps[index].indexed) {
            char digits[12];
            usize count = 0;
            uint32 value = steps[index].index;
            do {
                digits[sizeof(digits) - 1 - count++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value != 0);

            if (!result.TryAppend("[") ||
                !result.TryAppend(StringView(digits + sizeof(digits) - count, count)) ||
                !result.TryAppend("]")) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }
    }

    out.Swap(result);
    return Result<void>::Success();
}

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
