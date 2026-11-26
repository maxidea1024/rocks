#pragma once

#include <Graphics/ResourceAccess.h>
#include <RenderGraph/GraphTypes.h>

// 그래프 텍스처 접근만 선언 멤버다(ShaderSystem 16.2절 SSD59 GraphTextureAccess, UE
// RDG_TEXTURE_ACCESS). 셰이더에 묶지 않고 의존만 만든다(복사 원본·대상, 기록 함수가 손으로 묶는
// 자원 등). 수집기가 GraphPassAccesses::Texture(texture, access, range, flags)로 넣는다. null이면
// 의존이 아니다.
//
// 사용 예:
//   render::GraphTextureAccessParameter history;
//   parameters->history = {previousColor, graphics::ResourceAccess::CopySource};
namespace rocks::render {

struct alignas(16) GraphTextureAccessParameter {
    GraphTexture texture;
    graphics::ResourceAccess access = graphics::ResourceAccess::Unknown;
    graphics::SubresourceRange range;
    GraphAccessFlags flags = GraphAccessFlags::None;
};

} // namespace rocks::render
