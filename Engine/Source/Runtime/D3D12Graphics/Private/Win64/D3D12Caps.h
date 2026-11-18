#pragma once

#include <Graphics/DeviceCaps.h>

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

// D3D12 장치 능력 질의다(RenderBase 4.4절 GX1). 장치를 만든 직후 한 번 부른다.
namespace rocks::graphics::d3d12 {

// device·adapter의 전역 한도와 기능을 채운다. 한도는 d3d12.h API 상수와 resource binding tier,
// 기능은 CheckFeatureSupport(FEATURE_LEVELS·SHADER_MODEL·OPTIONS·OPTIONS9)와 어댑터 출력의 HDR
// 색 공간이다. 질의가 실패한 기능은 거짓(보수적 값)이다. featureLevel에 장치의 최고 feature
// level을 돌려준다.
DeviceCaps QueryDeviceCaps(ID3D12Device* device, IDXGIAdapter1* adapter,
                           uint32& featureLevel) noexcept;

} // namespace rocks::graphics::d3d12
