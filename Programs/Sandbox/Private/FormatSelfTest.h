#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 픽셀 형식(Docs/Design/PixelFormat.ko.md PF2·PF3·PF6)을 실제 장치로 확인한다.
// - 모든 backend: 형식 86개 × 용도(Sampled·RenderTarget·DepthStencil·파이프라인 색 형식)에서 "능력
//   있음 ⇔ 생성 성공"이고, 능력이 없으면 Unsupported다.
// - ReadTexture를 지원하는 backend(D3D12): 형식마다 mip·plane별 업로드·readback 바이트 왕복과
//   UpdateTexture 왕복, sRGB 짝·깊이·스텐실·비디오 Y/UV view를 셰이더로 읽은 값.
// device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunFormatChecks(const char* name, rocks::graphics::Device& device,
                              rocks::Allocator& allocator) noexcept;

} // namespace sandbox
