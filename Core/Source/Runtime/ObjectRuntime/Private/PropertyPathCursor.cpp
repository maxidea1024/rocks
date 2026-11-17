#include <Object/PathCursor.h>
#include <Object/SoftObjectRef.h>

// 경로 접근 cursor(detail::PathCursor — Object/PathCursor.h)의 멤버 구현이다. 실패 기록, 정적 대조,
// 내려가기, index·길이 검사, 고정·owner 재확인, 게시, String·Soft 끝 지점 읽기·쓰기를 둔다. 연산
// 실행 순서는 PropertyPathRunner.cpp, 문자열 경로는 PropertyPathText.cpp다.
namespace rocks::detail {

PathCursor::PathCursor(ObjectRegistry& registry, const ObjectAccessContext& access,
                       const PathTarget& target, PathOperation operation, const FieldValue* input,
                       FieldValue* output, PropertyPathFailure* failure) noexcept
    : registry_(registry), access_(access), owner_(target.Object()), root_(target.GetKind()),
      construction_(target.Transaction()), rootLease_(target.Lease()), rootView_(target.View()),
      rootValue_(target.Storage()), rootAllocator_(target.ValueAllocator()), rootBox_(target.Box()),
      stage_(target.References()), operation_(operation), input_(input), output_(output),
      failure_(failure) {
    if (failure_) {
        *failure_ = {};
    }
}

bool PathCursor::AcceptsReadOnlyWrites() const noexcept {
    return root_ == PathTarget::Kind::Staged ||
           (root_ == PathTarget::Kind::Detached && rootBox_->fromStaged_);
}

Allocator& PathCursor::ValueAllocator() const noexcept {
    if (root_ == PathTarget::Kind::Struct) {
        return *rootAllocator_;
    }

    if (root_ == PathTarget::Kind::Detached) {
        return rootBox_->GetAllocator();
    }

    return registry_.ObjectAllocator();
}

Result<void> PathCursor::RequireLive() noexcept {
    if (!IsLive()) {
        return Fail(ErrorCode::Unsupported, PropertyPathError::NoPathAccess);
    }

    return Result<void>::Success();
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

Result<void> PathCursor::CheckTarget(ValueKind target, ValueKind wrapper) noexcept {
    const FieldSchema& field = *Frame().field;
    const ValueKind kind = field.value.kind;
    const bool wrapped = ValueKindIsContainer(kind);
    const bool matches = wrapper == ValueKind::Opaque ? !wrapped : kind == wrapper;
    if (FieldTargetType(field).kind != target || !matches) {
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

Result<void> PathCursor::CheckKey(ValueKind key, TypeId enumeration) noexcept {
    const ValueType& type = FieldKeyType(*Frame().field);
    if (type.kind != key || (key == ValueKind::Enum && !(type.type == enumeration))) {
        return Fail(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
    }

    return Result<void>::Success();
}

Result<void> PathCursor::CheckKeyClass(const void* staticNative) noexcept {
    // 검증 지점(6단계)이 넣을 non-null 키의 클래스 lease를 쥐었다(등록·스키마 Base 파생 검사 포함).
    if (Frame().key.guid == Guid{}) {
        return Result<void>::Success();
    }

    if (!keyClass_.IsValid() || !keyClass_.IsNativeA(staticNative)) {
        return Fail(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
    }

    return Result<void>::Success();
}

Result<void> PathCursor::CheckWeakTarget(const void* staticNative) noexcept {
    // 검증 지점(5단계)이 non-null 대상의 실제 타입 lease를 쥐었다(생존·문맥 검사 포함).
    if (!input_ || input_->reference.IsNull()) {
        return Result<void>::Success();
    }

    if (!referencedType_.IsValid() || (staticNative && !referencedType_.IsNativeA(staticNative))) {
        return Fail(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
    }

    return Result<void>::Success();
}

Result<void> PathCursor::CheckClassTarget(const void* staticNative, TypeId current) noexcept {
    // 검증 지점(6단계)이 non-null 값의 클래스 lease를 쥐었다(등록·스키마 Base 파생 검사 포함).
    if (!input_ || input_->guid == Guid{}) {
        return Result<void>::Success();
    }

    if (!referencedType_.IsValid() || !referencedType_.IsNativeA(staticNative)) {
        return Fail(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
    }

    // Deprecated 클래스는 지금 값과 같은 재기록만 받는다(리플렉션 B9h2).
    if (HasFlag(referencedType_.Schema().flags, TypeFlags::Deprecated) && input_->guid != current) {
        return Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
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
    // struct 값에서 시작한 경로의 첫 단계는 그 struct의 멤버다.
    if (current_ == 0 && root_ == PathTarget::Kind::Struct) {
        if (rootView_.IsEmpty() || rootView_.StructOps().nativeType != structNative) {
            return Fail(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
        }

        return Result<void>::Success();
    }

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
    if (frame.selector != PathSelector::Index || frame.index > size ||
        (!insertion && frame.index == size)) {
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
    // staged 객체는 그 트랜잭션의 소유 스레드만 보고 encode 대상이 아니며, 분리된 값은 호출자가
    // 소유한다 — 고정할 것이 없다(리플렉션 B9i3c0).
    if (pin_.IsValid() || !IsLive()) {
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
    if (!IsLive()) {
        return Result<void>::Success();
    }

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

Result<void> PathCursor::PublishKeys(Array<FieldValue>& keys) noexcept {
    if (auto alive = CheckOwner(); !alive) {
        return alive;
    }

    pin_.Reset();
    // 옛 원소는 이 함수를 나갈 때(고정 밖) 반환한다.
    Array<FieldValue> previous(Move(*keys_));
    *keys_ = Move(keys);
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

} // namespace rocks::detail
