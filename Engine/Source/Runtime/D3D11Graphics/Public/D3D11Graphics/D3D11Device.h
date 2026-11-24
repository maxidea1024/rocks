#pragma once

#include <Graphics/Device.h>

namespace rocks::graphics {

// Direct3D 11 장치를 만든다(설계: Docs/Design/EngineRuntime 10.2절). D3D12의 SM 6.6 기준을
// 받지 못하는 GPU·드라이버를 위한 Windows 대체 backend다.
//
// 어댑터는 고성능 하드웨어 어댑터를 차례로 시도하고(feature level 11_1·11_0), 모두 실패하거나
// desc.preferSoftware이면 WARP를 쓴다. desc.enableValidation이면 D3D11 debug layer와 DXGI debug를
// 켠다(SDK layer가 설치되지 않았으면 끄고 계속한다).
//
// 실패:
// - Windows가 아니면 Unsupported다.
// - ID3D11Fence(ID3D11Device5·ID3D11DeviceContext4, Windows 10 1703 이상·WDDM 2.0)를 만들 수 있는
//   어댑터가 WARP까지 없으면 Unsupported다. fence가 없으면 다른 스레드의 GPU 완료 대기(GX7)를
//   지킬 수 없기 때문이다.
// - 그 밖의 API 실패는 DependencyFailed, 할당 실패는 OutOfMemory다.
// - report가 있으면 실패한 단계·원본 HRESULT·설명을 적는다(성공이면 code만 None으로 둔다).
//
// 사용 예:
//   DeviceCreateReport report;
//   auto created = graphics::CreateD3D11Device(allocator, {.enableValidation = true}, &report);
//   if (!created) {
//       // report.stage, report.nativeCode(HRESULT), report.detail
//   }
//
// 구현 규약:
// - 프레임은 deferred context 하나에 기록하고 EndFrame이 immediate context에서 업로드 → 프레임
//   실행 → present → fence signal 순으로 제출한다. 그래서 쌓인 업로드는 언제나 그 프레임보다 먼저
//   실행된다(D3D12와 같은 GX8 순서).
// - 업로드 ring·staging은 시스템 메모리이며 제출 때 UpdateSubresource로 올린다(D3D11에는 계속 map해
//   둘 수 있는 업로드 heap이 없다). 생성 초기 데이터는 생성 호출 안에서 올린다.
// - 스왑체인은 flip model(R8G8B8A8_UNORM 버퍼, RGBA8UnormSrgb RTV)이다.
// - 셰이더는 쿠킹된 DXBC(D3D11_SM5, CreateShader)다. 정점 입력 시맨틱은 ATTRIBUTE{location},
//   그리기 상수는 루트 상수 register b13의 동적 상수 버퍼, 자원은 셰이더 layout의 register다.
// - 래스터 앞면은 화면 반시계(FrontCounterClockwise = TRUE)이고 PipelineDesc::cullMode를 따른다.
// - CalibrateClocks는 제출한 GPU 작업이 끝나기를 기다린다(D3D11에 queue clock calibration이 없다).
Result<UniquePtr<Device>> CreateD3D11Device(Allocator& allocator, const DeviceDesc& desc,
                                            DeviceCreateReport* report = nullptr) noexcept;

} // namespace rocks::graphics
