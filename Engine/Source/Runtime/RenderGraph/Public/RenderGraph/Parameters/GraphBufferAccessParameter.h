#pragma once

#include <Graphics/ResourceAccess.h>
#include <RenderGraph/GraphTypes.h>

// 그래프 버퍼 접근만 선언 멤버다(ShaderSystem 16.2절 SSD59 GraphBufferAccess, UE
// RDG_BUFFER_ACCESS). 셰이더에 묶지 않고 버퍼 전체의 의존만 만든다(간접 인자·정점 버퍼·복사).
// null이면 의존이 아니다.
//
// 사용 예:
//   render::GraphBufferAccessParameter arguments;
//   parameters->arguments = {indirectArgs, graphics::ResourceAccess::IndirectArgs};
namespace rocks::render {

struct alignas(16) GraphBufferAccessParameter {
    GraphBuffer buffer;
    graphics::ResourceAccess access = graphics::ResourceAccess::Unknown;
    GraphAccessFlags flags = GraphAccessFlags::None;
};

} // namespace rocks::render
