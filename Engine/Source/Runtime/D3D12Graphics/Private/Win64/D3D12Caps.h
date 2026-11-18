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

// adapter에 연결된 디스플레이 가운데 HDR10(ST.2084·BT.2020) 색 공간으로 출력 중인 것이 있는지다
// (IDXGIOutput6::GetDesc1, UE WindowsD3D12Device.cpp:573–602와 같은 판정).
bool AdapterHasHdrOutput(IDXGIAdapter* adapter) noexcept;

// D3D 보장 최대 자원 크기(byte)다: min(max(128, 전용 VRAM MiB × 0.25), 2048) MiB(d3d12.h
// D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A·B·C_TERM).
uint64 GuaranteedResourceBytes(uint64 dedicatedVideoMemory) noexcept;

} // namespace rocks::graphics::d3d12
