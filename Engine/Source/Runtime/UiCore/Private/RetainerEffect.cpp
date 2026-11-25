#include <UiCore/RetainerEffect.h>

namespace rocks::ui {

RetainerEffect::RetainerEffect(Allocator& allocator) noexcept
    : hlsl_(allocator), glsl_(allocator), textureParameter_(allocator) {
}

Result<SharedPtr<const RetainerEffect>>
RetainerEffect::Create(Allocator& allocator, StringView hlsl, StringView glsl,
                       StringView textureParameter) noexcept {
    if (hlsl.IsEmpty() || glsl.IsEmpty() || textureParameter.IsEmpty() ||
        hlsl.Contains(StringView("\0", 1)) || glsl.Contains(StringView("\0", 1))) {
        return Result<SharedPtr<const RetainerEffect>>::Failure(ErrorCode::InvalidArgument);
    }
    auto effect = SharedPtr<RetainerEffect>::TryMake(allocator, allocator);
    if (!effect) {
        return Result<SharedPtr<const RetainerEffect>>::Failure(effect.Error());
    }
    auto result = effect.Value()->hlsl_.TryAssign(hlsl);
    if (result) {
        result = effect.Value()->glsl_.TryAssign(glsl);
    }
    if (result) {
        result = effect.Value()->textureParameter_.TryAssign(textureParameter);
    }
    if (!result) {
        return Result<SharedPtr<const RetainerEffect>>::Failure(result.Error());
    }
    return Result<SharedPtr<const RetainerEffect>>::Success(effect.Value());
}

StringView RetainerEffect::Hlsl() const noexcept {
    return hlsl_.View();
}
StringView RetainerEffect::Glsl() const noexcept {
    return glsl_.View();
}
StringView RetainerEffect::TextureParameter() const noexcept {
    return textureParameter_.View();
}

} // namespace rocks::ui
