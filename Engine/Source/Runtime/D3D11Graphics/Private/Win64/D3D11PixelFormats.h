#pragma once

#include <GraphicsTypes/PixelFormat.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <d3d11_4.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// 픽셀 형식의 D3D11(DXGI) 대응 표와 장치 능력 질의다(Docs/Design/PixelFormat 8.4절, PF4).
// DXGI 형식은 D3D12와 같다(D3D12Graphics/Private/Win64/D3D12PixelFormats.cpp와 같은 표, D3D 공용
// 도우미 모듈로 합치는 것은 후속). 형식을 다른 형식으로 바꾸지 않는다.
namespace rocks::graphics::d3d11 {

// 형식 하나의 DXGI 형식들이다. 쓰지 않는 칸은 DXGI_FORMAT_UNKNOWN이다.
struct FormatMapping {
    // 자원 형식이다. sRGB 짝과 깊이 형식은 typeless다(view 형식을 따로 고른다).
    DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN;
    // 기본 SRV 형식이다. 깊이·스텐실은 깊이, 비디오는 plane 0(휘도)이다.
    DXGI_FORMAT srv = DXGI_FORMAT_UNKNOWN;
    // plane 1 SRV 형식이다. 깊이·스텐실은 스텐실(G 채널 uint), 비디오는 색차다.
    DXGI_FORMAT planeSrv = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT rtv = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uav = DXGI_FORMAT_UNKNOWN;
};

// format의 대응 행이다. 무효 형식은 모든 칸이 UNKNOWN인 행이다.
FormatMapping MappingOf(PixelFormat format) noexcept;

// texture 형식의 plane을 viewFormat으로 읽는 SRV 형식이다(viewFormat은 ResolveTextureViewFormat이
// 확인한 값). 같은 자원으로 만들 수 없는 조합이면 UNKNOWN이다.
DXGI_FORMAT ViewSrvFormat(PixelFormat texture, uint32 plane, PixelFormat viewFormat) noexcept;

// mip 하나의 D3D11 메모리 배치다(staging 텍스처의 행 바이트·행 수). Rocks 배치(plane 순서,
// Docs/Design/PixelFormat 6절)와 다른 곳:
// - D24S8은 texel 하나에 깊이 24비트와 스텐실 8비트(상위 byte), D32FS8은 8바이트(float 깊이, 스텐실
//   byte, 빈 3바이트)로 섞여 있다.
// - 비디오는 plane 0 행들 뒤에 같은 행 간격의 plane 1 행들이 온다(행 수 = 높이 + 높이 / 2).
struct DeviceLayout {
    uint64 rowBytes = 0;
    uint32 rows = 0;
};

DeviceLayout DeviceLayoutOf(PixelFormat format, uint32 width, uint32 height) noexcept;

// Rocks 배치의 mip 하나(source, PixelFormatImageBytes 크기)를 D3D11 배치(행 간격 layout.rowBytes)로
// 옮긴다. destination은 layout.rowBytes × layout.rows 바이트다.
void ToDeviceLayout(PixelFormat format, uint32 width, uint32 height, const uint8* source,
                    uint8* destination) noexcept;

// D3D11 배치(행 간격 rowPitch)에서 plane 하나를 빈틈 없는 행으로 꺼낸다(PixelFormatPlaneImageBytes
// 크기). D24 깊이 plane의 상위 byte는 0이다.
void FromDeviceLayout(PixelFormat format, uint32 plane, uint32 width, uint32 height,
                      const uint8* source, uint64 rowPitch, uint8* destination) noexcept;

// device에서 format의 능력을 CheckFormatSupport·CheckFeatureSupport(FORMAT_SUPPORT2)로
// 질의한다(D3D12 질의와 같은 규칙, UE D3D11Device.cpp:457–524). software는 WARP 어댑터다(드라이버
// 결함을 피할 형식을 뺀다).
PixelFormatCaps QueryFormatCaps(ID3D11Device* device, PixelFormat format, bool software) noexcept;

// format의 MSAA 표본 수 집합이다(비트 n = 2^n). caps에 RenderTarget이 있으면 RTV 형식,
// DepthStencil이면 DSV 형식이 MULTISAMPLE_RENDERTARGET을 지원할 때 2–32표본을
// CheckMultisampleQualityLevels로 묻는다(품질 수준이 1 이상인 표본 수). 둘 다 없으면 0, 있으면
// 1표본 비트를 늘 넣는다(D3D12 질의와 같은 규칙).
uint32 QuerySampleCounts(ID3D11Device* device, PixelFormat format, PixelFormatCaps caps) noexcept;

} // namespace rocks::graphics::d3d11
