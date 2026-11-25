#include <UiWidget/RetainerEffectMaterial.h>

namespace rocks::ui {

RetainerEffectMaterial::RetainerEffectMaterial(Allocator& allocator) noexcept
    : hlsl_(allocator), glsl_(allocator), textureParameter_(allocator) {
}

StringView RetainerEffectMaterial::TextureParameter() const noexcept {
    return textureParameter_.ByteCount() ? textureParameter_.View() : StringView("Texture");
}

Result<void> RetainerEffectMaterial::SetShaders(StringView hlsl, StringView glsl,
                                                StringView textureParameter) noexcept {
    if (hlsl.IsEmpty() || glsl.IsEmpty() || textureParameter.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    auto& allocator = hlsl_.GetAllocator();
    String nextHlsl(allocator), nextGlsl(allocator), nextParameter(allocator);
    auto result = nextHlsl.TryAssign(hlsl);
    if (result) {
        result = nextGlsl.TryAssign(glsl);
    }
    if (result) {
        result = nextParameter.TryAssign(textureParameter);
    }
    if (result) {
        hlsl_.Swap(nextHlsl);
        glsl_.Swap(nextGlsl);
        textureParameter_.Swap(nextParameter);
    }
    return result;
}

Result<SharedPtr<const RetainerEffect>>
RetainerEffectMaterial::Snapshot(Allocator& allocator) const noexcept {
    return RetainerEffect::Create(allocator, hlsl_.View(), glsl_.View(), TextureParameter());
}

} // namespace rocks::ui
