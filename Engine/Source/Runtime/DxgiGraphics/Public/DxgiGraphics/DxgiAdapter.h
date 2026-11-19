#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <Core/Text/String.h>

// D3D11·D3D12 backend가 함께 쓰는 DXGI 어댑터 도우미다(장치 능력 GX1). 어댑터 질의
// (AdapterHasHdrOutput·AssignAdapterName)는 Windows 빌드에만 정의가 있다(Private/Win64).
struct IDXGIAdapter;
struct IDXGIAdapter1;

namespace rocks::graphics::dxgi {

// D3D 보장 최대 자원 크기(byte)다: min(max(128, 전용 VRAM MiB × 0.25), 2048) MiB(d3d11.h·d3d12.h
// D3D1x_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A·B·C_TERM, 두 판의 값이 같다).
//
// 사용 예:
//   limits.bufferBytes = dxgi::GuaranteedResourceBytes(adapterDesc.DedicatedVideoMemory);
uint64 GuaranteedResourceBytes(uint64 dedicatedVideoMemory) noexcept;

// adapter에 연결된 디스플레이 가운데 HDR10(ST.2084·BT.2020) 색 공간으로 출력 중인 것이 있는지다
// (IDXGIOutput6::GetDesc1, UE WindowsD3D12Device.cpp:573–602·WindowsD3D11Device.cpp:453과 같은
// 판정). adapter가 null이거나 출력이 없으면 거짓이다. Windows 빌드에만 있다.
bool AdapterHasHdrOutput(IDXGIAdapter* adapter) noexcept;

// adapter의 설명 이름(DXGI_ADAPTER_DESC1::Description, UTF-16)을 UTF-8로 바꿔 name에 담는다. 장치
// 정보(DeviceInfo::adapterName)와 로그에 쓴다. Windows 빌드에만 있다.
// 실패하면 name은 그대로다: adapter가 null이거나 GetDesc1 실패는 DependencyFailed, 짝 없는 UTF-16
// surrogate는 InvalidArgument(Platform/Win64/WideString.h의 거부 규칙), 그 밖은 name 할당 오류다.
//
// 사용 예:
//   auto named = dxgi::AssignAdapterName(adapter.Get(), adapterName_);
//   (void)named; // 이름은 진단용이라 실패해도 장치 생성은 계속한다
Result<void> AssignAdapterName(IDXGIAdapter1* adapter, String& name) noexcept;

} // namespace rocks::graphics::dxgi
