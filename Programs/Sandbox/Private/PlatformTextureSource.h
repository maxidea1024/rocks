#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <GraphicsTypes/ChannelSwizzle.h>
#include <RenderBase/CookedTexture.h>
#include <Texture/TextureSource.h>

// 쿠킹 텍스처 데이터(TexturePlatformData — Texture 모듈)를 RenderBase 업로드 입력으로 바꾼다
// (Docs/Design/RenderBase 6.5절 "업로드", 18.5절 7항). 텍스처 에셋을 아는 층의 일이라 지금은
// Sandbox에 두고, SM8 WorldIntegration 텍스처 컴포넌트가 생기면 그리로 옮긴다.
//
// 사용 예(패키지 공개 뒤, 게임 lane):
//   PlatformTextureSource converted(allocator);
//   auto made = MakePlatformTextureSource(texture.platformData, &ReadPackageBulk, &package,
//                                         converted);
//   auto upload = render::PrepareCookedTexture(allocator, converted.source, device, handle);
namespace sandbox {

using namespace rocks;

// bulk ref의 raw byte를 준다(패키지 bulk — Package::BulkBytes). span은 변환 결과를 쓰는 동안
// 유효해야 한다. 오류는 그 오류다.
using PlatformBulkReader = Result<Span<const uint8>> (*)(void* context,
                                                         const BulkDataRef& ref) noexcept;

// 변환 결과다. source의 mip·꼬리 byte는 mips 배열과 reader가 준 byte를 빌린다.
struct PlatformTextureSource {
    explicit PlatformTextureSource(Allocator& allocator) noexcept : mips(allocator) {
    }

    render::CookedTextureSource source;
    Array<render::CookedTextureMip> mips;
    // 쿠킹 swizzle(sampler type 계약 — 셰이더가 적용한다, view swizzle은 항등)이다.
    graphics::ChannelSwizzle swizzle;
};

// 쿠킹 텍스처 차원 → 장치 차원이다(값 그대로가 아니다 — 두 enum의 순서가 다르다).
graphics::TextureDimension ToGraphicsDimension(TextureDimension dimension) noexcept;

// data를 out으로 바꾼다(out의 앞 값은 지운다). 검사는 RenderBase CheckCookedTexture가 한다(여기서는
// 번호·enum 범위만 본다). 오류: 모르는 차원·PixelFormat 번호·샘플러 enum·swizzle 값 InvalidArgument
// (CorruptEntry), mip 수 ≠ numMips InvalidArgument, reader의 오류(빈 ref는 InvalidArgument), 할당
// 실패.
Result<void> MakePlatformTextureSource(const TexturePlatformData& data, PlatformBulkReader reader,
                                       void* context, PlatformTextureSource& out) noexcept;

} // namespace sandbox
