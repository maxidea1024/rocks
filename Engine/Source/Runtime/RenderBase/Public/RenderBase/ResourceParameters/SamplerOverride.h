#pragma once

#include <GraphicsTypes/Sampler.h>

// 텍스처 자원 멤버의 짝 샘플러 값이다(Docs/Design/RenderBase 9.6절 결정 R12, ShaderSystem 16.2절
// SSD59). given이면 짝 샘플러(<이름>Sampler) slot에 desc를 묶고, 아니면 그리기 때 그 텍스처 desc의
// 기본 샘플러를 쓴다. 그래서 같은 텍스처를 pass마다 다른 샘플러로 읽을 수 있다.
//
// 사용 예:
//   parameters.source.texture = sceneColor;
//   parameters.source.sampler = SamplerOverride::Of(pointClamp);   // 없으면 텍스처 desc 샘플러
namespace rocks::render {

struct SamplerOverride {
    graphics::SamplerDesc desc;
    bool given = false;

    // desc를 쓰는 값이다.
    static constexpr SamplerOverride Of(const graphics::SamplerDesc& sampler) noexcept {
        SamplerOverride value;
        value.desc = sampler;
        value.given = true;
        return value;
    }
};

} // namespace rocks::render
