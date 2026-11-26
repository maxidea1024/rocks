#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <GraphicsTypes/ChannelSwizzle.h>
#include <RenderBase/CookedTexture.h>
#include <Texture/TextureSource.h>

// 쿠킹 텍스처 데이터(TexturePlatformData — Texture 모듈)를 RenderBase 업로드 입력으로 바꾼다
// (Docs/Design/RenderBase 6.5절 "업로드", 18.5절 7항 "변환"·8항). RenderBase는 텍스처 에셋을
// 모르므로 에셋을 아는 이 층이 바꾼다(UE도 장치 자원 만들기는 Engine 모듈의 텍스처 자원 클래스가
// 한다). 에셋 텍스처는 TextureRenderCache가 이 함수를 부르고, 에셋이 아닌 메모리 플랫폼
// 데이터(도구·시험)는 직접 부른다.
//
// 사용 예(패키지 공개 뒤, 게임 lane):
//   PlatformTextureSource converted(allocator);
//   auto made = MakePlatformTextureSource(texture.platformData, &ReadPackageBulk, &package,
//                                         converted);
//   if (!made) { /* InvalidArgument(CorruptEntry)·reader 오류·할당 실패 */ }
//   auto upload = render::PrepareCookedTexture(allocator, converted.source, device, handle);
//
// 스레드: 순수 변환이라 어느 스레드에서나 부를 수 있다(reader가 허용하는 스레드 — 패키지 bulk는
// 게임 스레드 전용이다). 수명: 결과의 mip·꼬리 byte는 reader가 준 byte를 빌린다.
namespace rocks {

// bulk ref의 raw byte를 준다(패키지 bulk — Package::BulkBytes). context는 호출자 값이고, 돌려준
// span은 변환 결과를 쓰는 동안 유효해야 한다. 오류는 그 오류다(변환이 그대로 돌려준다).
using PlatformBulkReader = Result<Span<const uint8>> (*)(void* context,
                                                         const BulkDataRef& ref) noexcept;

// 변환 결과다. source의 mip·꼬리 byte는 mips 배열과 reader가 준 byte를 빌린다(source.mips는 mips를
// 가리키므로 이 객체를 옮기거나 복사하지 않는다).
struct PlatformTextureSource {
    explicit PlatformTextureSource(Allocator& allocator) noexcept : mips(allocator) {
    }

    PlatformTextureSource(const PlatformTextureSource&) = delete;
    PlatformTextureSource& operator=(const PlatformTextureSource&) = delete;

    render::CookedTextureSource source;
    Array<render::CookedTextureMip> mips;
    // 쿠킹 swizzle(sampler type 계약 — 셰이더가 적용한다, view swizzle은 항등 — 6.5절)이다.
    graphics::ChannelSwizzle swizzle;
};

// 쿠킹 텍스처 차원 → 장치 차원이다(두 enum의 순서가 달라 값 그대로가 아니다). 범위 밖 값은 장치
// 차원 범위 밖 값(graphics::IsValidTextureDimension이 거짓)을 돌려준다.
ROCKS_WORLDINTEGRATION_API graphics::TextureDimension
GraphicsTextureDimension(TextureDimension dimension) noexcept;

// data를 out으로 바꾼다(out의 앞 값은 지운다). 6.5절 계약 검사는 RenderBase CheckCookedTexture가
// 한다(여기서는 번호·enum 범위와 bulk 유무만 본다). 스트리밍 mip(앞쪽 numMips −
// residentTailMips개)은 mip bulk를, 상주 mip은 tailBulk 안 오프셋을 쓴다. 오류: 모르는
// 차원·PixelFormat 번호·샘플러 enum·swizzle 값, mip 배열 수 ≠ numMips, 스트리밍 mip·꼬리의 빈 bulk
// ref는 InvalidArgument(CorruptEntry), reader의 오류는 그대로, 할당 실패. 실패해도 out은 일부
// 채워질 수 있으며 쓰지 않는다.
ROCKS_WORLDINTEGRATION_API Result<void>
MakePlatformTextureSource(const TexturePlatformData& data, PlatformBulkReader reader, void* context,
                          PlatformTextureSource& out) noexcept;

} // namespace rocks
