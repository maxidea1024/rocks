#include "TextInternal.h"

// 텍스트 codec 문맥과 Soft·ClassRef 경로 key hook이다(리플렉션 B9i3b — Docs/Design/
// ReflectionExtensions.ko.md 10.3·10.4절, 경로 key 텍스트는 7.3절).
namespace rocks::reflection_text {

TextCodecContext::KeyTextHook::KeyTextHook(TextCodecContext& context) noexcept
    : owner(&context), parsed(context.GetAllocator()) {
}

Result<PathKeyValue> TextCodecContext::KeyTextHook::ParseKeyText(const ValueType& keyType,
                                                                 StringView text) noexcept {
    detail::ScalarFailure failure;
    const TextScalarType type{keyType, nullptr};
    auto read = detail::ParseScalarCore(*owner, type, text, parsed, failure);
    if (!read) {
        return Result<PathKeyValue>::Failure(read.Error());
    }

    if (!read.Value()) {
        failed = true;
        this->failure = failure.code;
        return Result<PathKeyValue>::Failure(failure.code == TextDiagnosticCode::UnknownType
                                                 ? ErrorCode::NotFound
                                                 : ErrorCode::InvalidArgument);
    }

    // Soft의 객체 경로는 parsed가 갖고 있다(다음 hook 호출 전까지 유효하다 — 경로 해석이 곧바로
    // 정규형으로 복사한다).
    PathKeyValue value;
    value.kind = keyType.kind;
    value.guid = parsed.guid;
    value.text = parsed.string.View();
    return Result<PathKeyValue>::Success(value);
}

Result<void> TextCodecContext::KeyTextHook::FormatKeyText(const ValueType& keyType,
                                                          const PathKeyValue& value,
                                                          String& out) noexcept {
    FieldValue key(owner->GetAllocator());
    key.kind = keyType.kind;
    key.guid = value.guid;
    if (auto assigned = key.string.TryAssign(value.text); !assigned) {
        return assigned;
    }

    StringBuilder<128> text(owner->GetAllocator());
    if (auto formatted = FormatTextScalar(*owner, TextScalarType{keyType, nullptr}, key, text);
        !formatted) {
        return formatted;
    }

    return out.TryAppend(text.View());
}

TextCodecContext::TextCodecContext(TypeRegistry& types, const TextCodecOptions& options,
                                   TextDiagnostics& diagnostics, Allocator& allocator) noexcept
    : types_(&types), options_(&options), diagnostics_(&diagnostics), allocator_(&allocator),
      keyHook_(*this) {
}

Result<void> TextCodecContext::Start() noexcept {
    if (started_) {
        return Result<void>::Success();
    }

    if (auto valid = ValidateTextCodecOptions(*options_); !valid) {
        return valid;
    }

    diagnostics_->SetLimit(options_->maxDiagnostics);
    started_ = true;
    return Result<void>::Success();
}

Result<void> TextCodecContext::ChargeInput(usize bytes) noexcept {
    const uint64 limit = options_->maxInputBytes;
    if (bytes > limit || consumed_ > limit - bytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    consumed_ += bytes;
    return Result<void>::Success();
}

PathKeyTextHook& TextCodecContext::KeyHook() noexcept {
    return keyHook_;
}

TextDiagnosticCode TextCodecContext::TakeKeyFailure() noexcept {
    const TextDiagnosticCode code =
        keyHook_.failed ? keyHook_.failure : TextDiagnosticCode::InvalidValue;
    keyHook_.failed = false;
    keyHook_.failure = TextDiagnosticCode::InvalidValue;
    return code;
}

} // namespace rocks::reflection_text
