#pragma once

#include <Graphics/DeviceCaps.h>

#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

// D3D11 장치 능력 질의다(RenderBase 4.4절 GX1). D3D12 질의(D3D12Graphics/Private/Win64/
// D3D12Caps.cpp)와 같은 규칙이며, D3D 공용 도우미 모듈로 합치는 것은 후속이다(D3D11PixelFormats.h와
// 같은 결정).
namespace rocks::graphics::d3d11 {

// device·adapter의 전역 한도와 기능을 채운다. 한도는 d3d11.h API 상수와 feature level(FL 11.0은
// UAV 8·VS UAV 없음, 11.1 이상은 UAV 64·모든 단계 UAV), 기능은 CheckFeatureSupport(OPTIONS3)와
// 어댑터 출력의 HDR 색 공간이다.
DeviceCaps QueryDeviceCaps(ID3D11Device* device, IDXGIAdapter1* adapter) noexcept;

} // namespace rocks::graphics::d3d11
