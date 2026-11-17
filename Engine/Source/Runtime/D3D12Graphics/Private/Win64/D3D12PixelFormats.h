#pragma once

#include <GraphicsTypes/PixelFormat.h>

#include <Windows.h>
#include <d3d12.h>

// 픽셀 형식의 D3D12(DXGI) 대응 표와 장치 능력 질의다(Docs/Design/PixelFormat.ko.md 8.4절 PF2,
// 7·8.2절 PF3). 형식을 다른 형식으로 바꾸지 않는다. 표에 없는 형식(ETC2·EAC·ASTC·RGB8·R8/RG8 sRGB·
// R64)은 resource가 UNKNOWN이고 능력이 None이다.
namespace rocks::graphics::d3d12 {

// 형식 하나의 DXGI 형식들이다. 쓰지 않는 칸은 DXGI_FORMAT_UNKNOWN이다.
struct FormatMapping {
    // 자원 형식이다. sRGB 짝과 깊이 형식은 typeless다(view 형식을 따로 고른다).
    DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN;
    // 기본 SRV 형식이다. 깊이·스텐실은 깊이 plane, 비디오는 plane 0(휘도)이다.
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
// 확인한 값). 같은 자원으로 만들 수 없는 조합(다른 typeless 계열)이면 UNKNOWN이다.
DXGI_FORMAT ViewSrvFormat(PixelFormat texture, uint32 plane, PixelFormat viewFormat) noexcept;

// D3D12 subresource 번호다(plane 순서가 mip보다 바깥, 배열 1).
constexpr uint32 SubresourceIndex(uint32 mipLevel, uint32 plane, uint32 mipLevels) noexcept {
    return mipLevel + plane * mipLevels;
}

// DXGI 형식의 plane 수다(D3D12_FEATURE_FORMAT_INFO). 깊이·스텐실 typeless와 비디오는 2, 질의
// 실패는 1이다.
uint32 FormatPlaneCount(ID3D12Device* device, DXGI_FORMAT format) noexcept;

// device에서 format의 능력을 CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT)로 질의한다. 자원·
// SRV·RTV·DSV·UAV 형식마다 FORMAT_SUPPORT1·2 비트를 PixelFormatCaps로 옮긴다(UE D3D12Device.cpp:
// 202–270과 같은 구조). Multisample은 MSAA desc가 생기는 GX2까지 채우지 않는다.
PixelFormatCaps QueryFormatCaps(ID3D12Device* device, PixelFormat format) noexcept;

} // namespace rocks::graphics::d3d12
