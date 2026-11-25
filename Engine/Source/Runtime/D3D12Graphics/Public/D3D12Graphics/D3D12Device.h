#pragma once

#include <Graphics/Device.h>

namespace rocks::graphics {

// Direct3D 12 장치를 만든다. 고성능 하드웨어 어댑터(Feature Level 11_0 이상)를 고르고, 없거나
// desc.preferSoftware이면 WARP를 쓴다. desc.enableValidation이면 debug layer를 켠다(설치되지
// 않았으면 켜지 않고 계속한다). 장치·큐 생성 실패는 DependencyFailed, Windows가 아니면
// Unsupported다.
// - report: null이 아니면 결과를 채운다(실패면 실패한 API 호출 이름·HRESULT·설명, 성공이면 빈 값).
//   생성 실패 로그는 backend를 고르는 호출자가 report로 남긴다(대체 후보가 있으면 Warning, 모든
//   후보 실패면 Error). 성공하면 장치가 "Created ... device" Info를 한 번 남긴다.
//
// 사용 예:
//   DeviceCreateReport report;
//   auto created = CreateD3D12Device(allocator, desc, &report);
//   if (!created) {
//       // report.stage = "D3D12CreateDevice (WARP)", report.nativeCode = HRESULT
//   }
//
// 구현 규약:
// - 프레임 2개를 동시에 진행하며 프레임 slot 재사용은 해당 fence 완료로 판정한다.
// - 스왑체인은 flip-model(R8G8B8A8_UNORM 버퍼, RGBA8UnormSrgb RTV)이다.
// - 정점 버퍼는 upload heap에 두고(정적 소량 데이터), 텍스처는 default heap에 복사 후 GPU 완료를
// 기다린다.
// - 셰이더는 쿠킹된 DXIL(D3D12_SM6, CreateShader)이다. 정점 입력 시맨틱은 ATTRIBUTE{location},
//   그리기 상수는 root 32-bit 상수(b0 space3)이고 root signature는 셰이더 layout으로 만든다.
Result<UniquePtr<Device>> CreateD3D12Device(Allocator& allocator, const DeviceDesc& desc,
                                            DeviceCreateReport* report = nullptr) noexcept;

} // namespace rocks::graphics
