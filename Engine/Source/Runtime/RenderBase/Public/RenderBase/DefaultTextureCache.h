#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Graphics/Device.h>

// (값, 형식, 차원) 키의 1×1 기본 텍스처 캐시다(Docs/Design/RenderBase 11.1절 RR17, 엔진
// 셰이더 14.5절 4항, UE FSystemTextures::GetDefaultTexture 대응). 내장 자원 팩(BuiltinResources)은
// 고정 목록이고 이 캐시는 pass가 요구하는 임의 (값, 형식, 차원)의 대체 텍스처를 만든다(9.6절 typed
// variant와 같은 역할).
//
// 텍스처는 Sampled 1×1(3D 1×1×1, cube 면 6, 배열 층 1, cube 배열 cube 1), mip 1, 샘플러
// point·wrap이다. 값은 형식으로 부호화한다(EncodeDefaultTexel). 소유 스레드 전용이다(장치 생성 규칙
// — 잠금이 없다). 파괴자는 Retire하지 않는다(장치를 모른다). 주인이 렌더 lane 종료 단계에서
// Retire한다.
//
// 사용 예:
//   render::DefaultTextureCache defaults;
//   auto black = defaults.Find(device, graphics::TextureDimension::Texture3D,
//                              graphics::PixelFormat::RGBA16Float, {0, 0, 0, 0});
//   ...
//   defaults.Retire(device);   // lane 종료
namespace rocks::render {

// texel 하나의 최대 byte 수(RGBA32 형식 16 byte)다.
inline constexpr usize MaxDefaultTexelBytes = 16;

// value를 format texel 하나로 부호화해 out에 쓰고 byte 수를 돌려준다. unorm·snorm은 [0, 1]·[-1,
// 1]로 자른 뒤 반올림, sRGB 형식의 RGB는 Core LinearColor::ToSRGB(알파는 선형), half는 Core Float16
// (round-to-nearest-even), 정수 형식은 값을 그 형식 범위로 자른 정수(소수는 0 쪽으로)다. 받는 형식:
// R8·RG8·RGBA8 Unorm, RGBA8Snorm, RGBA8UnormSrgb, BGRA8Unorm·BGRA8UnormSrgb, R8Uint·RGBA8Uint, R16·
// RG16·RGBA16 Float, R16Unorm·RG16Unorm·RGBA16Unorm, R16Uint·RGBA16Uint, R32·RG32·RGBA32 Float,
// R32Uint·R32Sint·RG32Uint·RGBA32Uint. 그 밖(묶음·깊이·압축·비디오 형식)은 Unsupported다. out이
// MaxDefaultTexelBytes보다 작으면 InvalidArgument다.
ROCKS_RENDERBASE_API Result<usize> EncodeDefaultTexel(graphics::PixelFormat format,
                                                      const graphics::ClearColor& value,
                                                      Span<uint8> out) noexcept;

class ROCKS_RENDERBASE_API DefaultTextureCache {
public:
    // 항목 수 상한이다(pass가 요구하는 조합은 몇 개다 — 넘으면 호출자 오류로 본다).
    static constexpr usize MaxDefaultTextures = 64;

    DefaultTextureCache() noexcept = default;
    DefaultTextureCache(const DefaultTextureCache&) = delete;
    DefaultTextureCache& operator=(const DefaultTextureCache&) = delete;

    // (dimension, format, value)의 텍스처다. 같은 키(값은 bit 비교)는 같은 handle이고, 없으면
    // 만든다. 실패: 부호화 표 밖 형식·cube 배열 능력 없음(DeviceFeatures::textureCubeArray)·장치가
    // 그 형식·차원을 만들지 못함 Unsupported, 항목 상한 CapacityExceeded, 장치 생성 실패는 그
    // 오류(담지 않는다).
    Result<graphics::TextureHandle> Find(graphics::Device& device,
                                         graphics::TextureDimension dimension,
                                         graphics::PixelFormat format,
                                         const graphics::ClearColor& value) noexcept;

    // 모든 텍스처를 Retire하고 비운다(다시 Find하면 새로 만든다).
    void Retire(graphics::Device& device) noexcept;

    usize Size() const noexcept {
        return count_;
    }

private:
    struct Entry {
        graphics::TextureDimension dimension = graphics::TextureDimension::Texture2D;
        graphics::PixelFormat format = graphics::PixelFormat::Unknown;
        uint32 valueBits[4] = {};
        graphics::TextureHandle texture;
    };

    Entry entries_[MaxDefaultTextures];
    usize count_ = 0;
};

} // namespace rocks::render
