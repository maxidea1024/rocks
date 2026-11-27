#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <GraphicsTypes/PixelFormat.h>
#include <Texture/TextureSource.h>

// 쿠킹과 같은 배치의 TexturePlatformData를 메모리에 만든다(Docs/Design/RenderBase 6.5절
// 배치, 18.5절 7항). 엔진 절차 텍스처와 self-test 메모리 장면이 쿠킹 텍스처와 같은 업로드
// 경로(WorldIntegration PlatformTextureSource → RenderBase CookedTexture)를 거치게 한다. 스트리밍
// mip은 mip마다 bulk 하나, 상주 꼬리는 큰 mip부터 16 byte 정렬로 이은 덩어리 하나다(쿠킹 처리기
// CookFillPlatformData와 같다). bulk id는 1부터다.
//
// 사용 예:
//   MemoryTextureSpec spec;
//   spec.dimension = TextureDimension::Texture2D;
//   spec.format = graphics::PixelFormat::RGBA8Unorm;
//   spec.width = spec.height = 4;
//   spec.mips = 3;
//   MemoryPlatformTexture texture(allocator);
//   texture.Build(spec, &Fill, &state);              // Fill(state, level, slice, x, y, z, value)
//   MakePlatformTextureSource(texture.Data(), &MemoryPlatformTexture::ReadBulk, &texture, out);
namespace sandbox {

using namespace rocks;

// 만들 텍스처다. slices는 배열 층 수·cube 면 수(6 × cube 수)이고 Texture2D·Volume은 1이다. depth는
// Volume만 쓴다. residentTailMips 0은 모든 mip을 꼬리에 둔다.
struct MemoryTextureSpec {
    TextureDimension dimension = TextureDimension::Texture2D;
    graphics::PixelFormat format = graphics::PixelFormat::RGBA8Unorm;
    uint32 width = 1;
    uint32 height = 1;
    uint32 depth = 1;
    uint32 slices = 1;
    uint32 mips = 1;
    uint32 residentTailMips = 0;
    TextureSamplerSettings sampler;
    TextureSwizzle swizzle[4] = {TextureSwizzle::R, TextureSwizzle::G, TextureSwizzle::B,
                                 TextureSwizzle::A};
};

// texel 값 하나를 준다(level mip, slice 배열 층·면, x·y·z texel). value는 TexelCodec 부호화
// 입력이다(블록 형식은 압축 전 texel 값).
using MemoryTexelFunction = void (*)(void* context, uint32 level, uint32 slice, uint32 x, uint32 y,
                                     uint32 z, float (&value)[4]) noexcept;

class MemoryPlatformTexture {
public:
    explicit MemoryPlatformTexture(Allocator& allocator) noexcept;
    MemoryPlatformTexture(const MemoryPlatformTexture&) = delete;
    MemoryPlatformTexture& operator=(const MemoryPlatformTexture&) = delete;

    // spec대로 만든다(앞 값은 지운다). 블록 압축 형식(TexelCodec IsBlockCodecFormat — BC1·BC3·BC4·
    // BC5)은 fill 값을 RGBA8로 쓴 뒤 mip마다 TexelCodec EncodeBlockImage로 압축한다.
    // 오류: TexelCodec이 다루지 않는 형식·0 크기·mip 수 0 InvalidArgument, 할당 실패.
    Result<void> Build(const MemoryTextureSpec& spec, MemoryTexelFunction fill,
                       void* context) noexcept;

    const TexturePlatformData& Data() const noexcept {
        return data_;
    }

    // 고쳐 쓰는 시험(잘린 bulk 등)용이다.
    TexturePlatformData& MutableData() noexcept {
        return data_;
    }

    // bulk ref의 byte다(PlatformBulkReader — context는 이 객체). 오류: 모르는 id NotFound.
    static Result<Span<const uint8>> ReadBulk(void* context, const BulkDataRef& ref) noexcept;

private:
    Allocator& allocator_;
    TexturePlatformData data_;
    // bulk id − 1 번째 byte다.
    Array<Array<uint8>> bulks_;
};

} // namespace sandbox
