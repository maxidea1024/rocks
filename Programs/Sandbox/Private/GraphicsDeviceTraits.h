#pragma once

#include <Config/DeviceTraits.h>
#include <Graphics/DeviceCaps.h>

// 그래픽 장치 능력(Graphics DeviceCaps — RenderBase 4.4절 GX1)을 설정 계층의 기기 특성으로 옮긴다
// (Docs/Design/ConfigSystem.ko.md 6절 — 설정 CF4). Config 모듈은 Graphics를 모르므로 장치를 만든
// 호스트가 이 변환으로 특성을 채우고 HostSettings::ApplyDeviceTraits를 부른다.
namespace sandbox {

// caps의 한도·기능을 DeviceTraitNames의 GPU 특성(texture_size_2d·color_attachments·
// pixel_sampled_textures·sampler_anisotropy·compute_shaders·shader_model)으로 traits에 쓴다. 오류:
// DeviceTraits::SetNumber의 오류.
rocks::Result<void> AppendGraphicsDeviceTraits(const rocks::graphics::DeviceCaps& caps,
                                               rocks::config::DeviceTraits& traits) noexcept;

} // namespace sandbox
