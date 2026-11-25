#pragma once

#include <Material/Material.h>
#include <UiCore/RetainerEffect.h>

namespace rocks::ui {

// 보관된 자식 표면을 입력으로 받는 UI 효과 재질이다. 텍스처 매개변수 이름은 에셋에 보존된다.
// HLSL의 PSMain은 COLOR0 색조와 TEXCOORD0 UV를 받고 GLES는 위치 0·1을 사용한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API RetainerEffectMaterial final : public MaterialBase {
    ROCKS_GENERATED_BODY()
public:
    explicit RetainerEffectMaterial(Allocator& allocator) noexcept;
    Result<void> SetShaders(StringView hlsl, StringView glsl,
                            StringView textureParameter = "Texture") noexcept;
    Result<SharedPtr<const RetainerEffect>> Snapshot(Allocator& allocator) const noexcept;
    StringView TextureParameter() const noexcept;

private:
    ROCKS_PROPERTY(Name = "hlsl") String hlsl_;
    ROCKS_PROPERTY(Name = "glsl") String glsl_;
    ROCKS_PROPERTY(Name = "textureParameter") String textureParameter_;
};

} // namespace rocks::ui
