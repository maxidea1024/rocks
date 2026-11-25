#pragma once

#include <Core/Memory/SharedPtr.h>
#include <Core/Text/String.h>

namespace rocks::ui {

// GPU에 넘기는 변경 불가능한 효과 셰이더 스냅숏이다. 자식 표면은 HLSL t0/s0,
// GLES binding 0에 연결된다. 픽셀 셰이더는 UV와 색조를 받아 알파가 미리 곱해진 색을 돌려준다.
class ROCKS_UICORE_API RetainerEffect {
public:
    explicit RetainerEffect(Allocator& allocator) noexcept;
    static Result<SharedPtr<const RetainerEffect>> Create(Allocator& allocator, StringView hlsl,
                                                          StringView glsl,
                                                          StringView textureParameter) noexcept;
    StringView Hlsl() const noexcept;
    StringView Glsl() const noexcept;
    StringView TextureParameter() const noexcept;

private:
    String hlsl_;
    String glsl_;
    String textureParameter_;
};

} // namespace rocks::ui
