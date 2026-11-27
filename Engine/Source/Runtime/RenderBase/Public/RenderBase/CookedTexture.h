#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Graphics/Device.h>
#include <RenderBase/RenderHandle.h>
#include <RenderBase/RenderLane.h>
#include <RenderBase/RenderNotices.h>
#include <Meta/LaneTransfer.h>

// 쿠킹 텍스처 업로드다(Docs/Design/RenderBase 6.5절 "업로드", 18.5절 7항 — SM7). 에셋 타입(Texture
// 모듈)을 모르는 중립 입력(CookedTextureSource)을 6.5절 계약으로 검사해 장치 텍스처 설정을 만들고,
// mip을 장치 초기 픽셀 배치로 묶어 렌더 lane에서 만든다. TexturePlatformData와 패키지 bulk를 이
// 입력으로 바꾸는 일은 텍스처 에셋을 아는 층(지금은 Sandbox, 다음은 WorldIntegration)이 한다.
//
// 사용 예(게임 lane — 패키지 공개 뒤):
//   CookedTextureMip mips[...];                    // 크기·byte 수, 스트리밍 mip은 bulk byte
//   CookedTextureSource source;                    // 차원·형식·크기·꼬리·샘플러
//   source.mips = Span<const CookedTextureMip>(mips);
//   source.tail = tailBytes;
//   CookedTextureIssue issue = CookedTextureIssue::None;
//   auto handle = lane.ReserveHandle(RenderResourceKind::Texture);
//   auto upload = PrepareCookedTexture(allocator, source, device, handle.Value(), &issue);
//   if (!upload) { /* CorruptEntry(CorruptData)·UnsupportedCapability, issue가 원인 */ }
//   lane.PostResource(Move(upload.Value()), &CreateCookedTextureOnLane);
//
// 스레드: 검사·묶기는 어느 스레드에서나 부를 수 있다(장치 능력은 바뀌지 않는 값이다). 입력 byte는
// 호출 동안만 빌린다. CreateCookedTextureOnLane은 렌더 lane 작업이다.
namespace rocks::render {

// 쿠킹 샘플러의 maxAnisotropy 0("전역 값")을 바꿀 값이다(UE r.MaxAnisotropy 기본값 4 —
// Core/Private/HAL/ConsoleManager.cpp, 적용 RHIUtilities.h ComputeAnisotropyRT). 전역 CVar는 렌더러
// 설정 단계에서 더한다.
inline constexpr uint32 CookedTextureDefaultMaxAnisotropy = 4;

// 상주 꼬리 덩어리 안 mip 시작 정렬이다(6.5절 — 쿠킹이 사이를 0으로 채운다).
inline constexpr uint64 CookedTextureTailAlignment = 16;

// 쿠킹 mip 하나다. byteSize는 패딩을 뺀 크기(6.5절 공식)다. 스트리밍 mip(앞쪽 mip 수 −
// residentTailMips개)은 bulk가 그 mip의 byte 전체이고, 상주 mip은 tailOffset이 꼬리 덩어리 안
// 시작이다(bulk는 비운다).
struct CookedTextureMip {
    uint32 width = 0;
    uint32 height = 0;
    // Texture3D의 깊이다(다른 차원은 1).
    uint32 depth = 1;
    uint64 byteSize = 0;
    Span<const uint8> bulk{};
    uint64 tailOffset = 0;
};

// 쿠킹 텍스처 하나의 업로드 입력이다(6.5절 메타). byte는 빌린다.
struct CookedTextureSource {
    graphics::TextureDimension dimension = graphics::TextureDimension::Texture2D;
    graphics::PixelFormat format = graphics::PixelFormat::Unknown;
    // 형식의 sRGB 여부와 같아야 한다(PixelFormat 결정 PX1 (가) — sRGB는 형식에 들어 있다).
    bool srgb = false;
    // 기준 크기(mip 0)다. depth는 Texture3D만, slices는 배열 층 수·cube 면 수(6 × cube 수)다
    // (Texture2D·Texture3D는 1).
    uint32 width = 0;
    uint32 height = 0;
    uint32 depth = 1;
    uint32 slices = 1;
    // 상주 꼬리 mip 수(1–mip 수)다. 가장 작은 mip들이 꼬리 덩어리에 있다.
    uint32 residentTailMips = 0;
    // 큰 mip(0)부터다.
    Span<const CookedTextureMip> mips{};
    // 상주 꼬리 덩어리다.
    Span<const uint8> tail{};
    // 쿠킹 샘플러다. maxAnisotropy 0은 전역 값(CookedTextureDefaultMaxAnisotropy)이다.
    graphics::SamplerDesc sampler;
};

// 검사가 거부한 원인이다. Corrupt 계열(데이터가 6.5절 계약과 다름)은 CorruptData(6.6절), Device
// 계열(장치가 못 함)은 UnsupportedCapability로 돌려준다.
enum class CookedTextureIssue : uint8 {
    None,
    // --- CorruptEntry(CorruptData) ---
    // 모르는 차원
    InvalidDimension,
    // 모르는 형식(Unknown 포함)
    InvalidFormat,
    // 깊이·깊이 스텐실·다중 plane 형식(쿠킹 색 텍스처가 아님)
    NonColorFormat,
    // srgb 칸이 형식의 sRGB 여부와 다름
    SrgbMismatch,
    // 0 크기, 차원별 크기 규칙(Texture2D slice 1, cube 정사각·slice 6 배수, volume slice 1, 3D가
    // 아닌 depth ≠ 1) 위반
    InvalidExtent,
    // mip 수 0 또는 전체 mip 수 초과
    MipCount,
    // mip 크기가 사슬(MipExtent)과 다름
    MipExtent,
    // mip byte 수가 공식(이미지 byte × depth × slice)과 다름
    MipByteSize,
    // 상주 꼬리 mip 수 0·mip 수 초과
    ResidentTail,
    // 스트리밍 mip bulk 크기가 byte 수와 다름(잘린 bulk)
    BulkSize,
    // 꼬리 오프셋이 16 byte 정렬이 아님
    TailAlignment,
    // 꼬리 오프셋이 앞 mip과 겹치거나 덩어리 밖(잘린 꼬리)
    TailRange,
    // BC 최상위 mip이 블록 배수가 아님(PixelFormatInfo::topLevelBlockAligned)
    BlockAlignment,
    // 샘플러 값 범위 밖
    InvalidSampler,
    // --- UnsupportedCapability ---
    // 형식 능력(Sampled·Mipmaps·Texture3D·TextureCube)이 없음
    UnsupportedFormat,
    // cube array 기능(DeviceFeatures::textureCubeArray)이 없음
    UnsupportedDimension,
    // 장치 한도(CheckTextureLimits — 한 변·slice 수) 초과
    DeviceLimit
};

// 원인 이름이다(열거자 이름, 정적 수명, 범위 밖 "Unknown").
ROCKS_RENDERBASE_API const char* CookedTextureIssueName(CookedTextureIssue issue) noexcept;

// source를 6.5절 계약으로 검사하고 장치 텍스처 설정(Sampled, view swizzle 항등, 쿠킹 샘플러)을
// 만든다. caps는 장치 능력, formatCaps는 그 형식의 장치 능력(Device::GetPixelFormatCaps)이다.
// 오류: 위 CookedTextureIssue 주석의 두 갈래. issue가 null이 아니면 원인을 쓴다(성공이면 None).
ROCKS_RENDERBASE_API Result<graphics::TextureDesc>
CheckCookedTexture(const CookedTextureSource& source, const graphics::DeviceCaps& caps,
                   graphics::PixelFormatCaps formatCaps,
                   CookedTextureIssue* issue = nullptr) noexcept;

// 검사를 마친 source의 mip을 장치 초기 픽셀 배치(TextureDesc 주석 — mip 0부터, 꼬리 정렬 패딩
// 없음)로 묶어 렌더 lane 소유 byte로 복사한다. 검사하지 않은 입력이면 CorruptData일 수 있다
// (상주 꼬리 수·bulk 크기·꼬리 범위가 맞지 않음).
// 오류: 크기 넘침 Overflow, 할당 실패.
ROCKS_RENDERBASE_API Result<RenderBlob>
PackCookedTexturePixels(Allocator& allocator, const CookedTextureSource& source) noexcept;

// 렌더 lane에 넘길 업로드 값이다(CreateCookedTextureOnLane).
struct CookedTextureUpload {
    RenderHandle handle;
    graphics::TextureDesc desc;
    RenderBlob pixels;
};

// 검사(device의 능력)와 묶기를 한 번에 한다. 오류: CheckCookedTexture·PackCookedTexturePixels의
// 오류. allocator는 다른 스레드의 해제를 허용해야 한다(RenderBlob — 렌더 lane에서 놓는다).
ROCKS_RENDERBASE_API Result<CookedTextureUpload>
PrepareCookedTexture(Allocator& allocator, const CookedTextureSource& source,
                     const graphics::Device& device, RenderHandle handle,
                     CookedTextureIssue* issue = nullptr) noexcept;

// 렌더 lane 작업이다: 장치 텍스처를 만들어 upload.handle에 잇고 ResourceReady를 통지한다. 생성이
// 실패하면 handle을 Failed로 두고 ResourceFailed(오류)를 통지한다. handle이 이미 Retire됐으면 만든
// 텍스처를 장치에 Retire하고 통지하지 않는다. 픽셀 byte는 작업이 끝나면 놓인다.
ROCKS_RENDERBASE_API void CreateCookedTextureOnLane(RenderLaneContext& context,
                                                    CookedTextureUpload& upload) noexcept;

} // namespace rocks::render

ROCKS_LANE_TRANSFERABLE(rocks::render::CookedTextureUpload);
