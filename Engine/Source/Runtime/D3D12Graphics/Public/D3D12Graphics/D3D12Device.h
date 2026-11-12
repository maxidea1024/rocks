#pragma once

#include <Graphics/Device.h>

namespace rocks::graphics {

// Direct3D 12 장치를 만든다. 고성능 하드웨어 어댑터(Feature Level 11_0 이상)를 고르고, 없거나
// desc.preferSoftware이면 WARP를 쓴다. desc.enableValidation이면 debug layer를 켠다(설치되지
// 않았으면 켜지 않고 계속한다). 장치·큐 생성 실패는 DependencyFailed, Windows가 아니면
// Unsupported다.
//
// 구현 규약:
// - 프레임 2개를 동시에 진행하며 프레임 slot 재사용은 해당 fence 완료로 판정한다.
// - 스왑체인은 flip-model(R8G8B8A8_UNORM 버퍼, RGBA8UnormSrgb RTV)이다.
// - 정점 버퍼는 upload heap에 두고(정적 소량 데이터), 텍스처는 default heap에 복사 후 GPU 완료를
// 기다린다.
// - 셰이더는 HLSL을 실행 시 SM 5.1로 컴파일한다. 정점 속성은 TEXCOORD{location} 의미 체계를 쓴다.
Result<UniquePtr<Device>> CreateD3D12Device(Allocator& allocator, const DeviceDesc& desc) noexcept;

} // namespace rocks::graphics
