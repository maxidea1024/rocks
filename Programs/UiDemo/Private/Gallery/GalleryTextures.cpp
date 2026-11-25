#include "GalleryTextures.h"

namespace rocks::ui::gallery {

namespace {

// RGBA 한 화소의 byte 수다.
constexpr usize RgbaBytes = 4;
constexpr uint32 CheckerSize = 64;
constexpr uint32 CheckerCell = 8;
constexpr uint32 FrameSize = 32;
constexpr uint32 FrameBorder = 8;
constexpr uint32 GradientWidth = 256;
constexpr uint32 GradientHeight = 64;
constexpr uint8 Opaque = 255;

struct Rgba {
    uint8 r;
    uint8 g;
    uint8 b;
    uint8 a;
};

} // namespace

GalleryTextures::GalleryTextures(graphics::Device& device, SlotArray<BrushTexture>& textures,
                                 graphics::PipelineHandle imagePipeline) noexcept
    : device_(device), textures_(textures), imagePipeline_(imagePipeline) {
}

GalleryTextures::~GalleryTextures() noexcept {
    const SlotHandle handles[] = {checker_, frame_, gradient_};
    for (SlotHandle handle : handles) {
        if (handle.IsNull()) {
            continue;
        }

        if (const BrushTexture* texture = textures_.Find(handle)) {
            device_.Retire(texture->texture);
        }

        (void)textures_.TryRemove(handle);
    }
}

template <typename Pixel>
Result<SlotHandle> GalleryTextures::Add(Allocator& allocator, uint32 width, uint32 height,
                                        Pixel&& pixel) noexcept {
    Array<uint8> pixels(allocator);
    if (auto allocated = pixels.TryResize(static_cast<usize>(width) * height * RgbaBytes);
        !allocated) {
        return Result<SlotHandle>::Failure(allocated.Error());
    }

    for (uint32 y = 0; y < height; ++y) {
        for (uint32 x = 0; x < width; ++x) {
            const Rgba color = pixel(x, y);
            const usize i = RgbaBytes * (static_cast<usize>(width) * y + x);
            pixels[i] = color.r;
            pixels[i + 1] = color.g;
            pixels[i + 2] = color.b;
            pixels[i + 3] = color.a;
        }
    }

    graphics::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    auto texture = device_.CreateTexture(desc, pixels.View());
    if (!texture) {
        return Result<SlotHandle>::Failure(texture.Error());
    }

    auto slot = textures_.TryAdd({texture.Value(), imagePipeline_});
    if (!slot) {
        device_.Retire(texture.Value());
        return Result<SlotHandle>::Failure(slot.Error());
    }

    return slot;
}

Result<void> GalleryTextures::Create(Allocator& allocator) noexcept {
    // 체크무늬: 밝은 회색·어두운 회색 8칸(투명 배경 견본에 쓰는 무늬).
    auto checker = Add(allocator, CheckerSize, CheckerSize, [](uint32 x, uint32 y) noexcept {
        const bool light = ((x / CheckerCell) + (y / CheckerCell)) % 2 == 0;
        const uint8 value = light ? 200 : 120;
        return Rgba{value, value, value, Opaque};
    });
    if (!checker) {
        return Result<void>::Failure(checker.Error());
    }

    checker_ = checker.Value();
    // 9분할 테두리: 모서리 8은 파랑, 변은 초록, 가운데는 반투명 흰색이라 늘어나는 구역이 보인다.
    auto frame = Add(allocator, FrameSize, FrameSize, [](uint32 x, uint32 y) noexcept {
        const bool edgeX = x < FrameBorder || x >= FrameSize - FrameBorder;
        const bool edgeY = y < FrameBorder || y >= FrameSize - FrameBorder;
        if (edgeX && edgeY) {
            return Rgba{70, 140, 255, Opaque};
        }

        if (edgeX || edgeY) {
            return Rgba{90, 200, 120, Opaque};
        }

        return Rgba{255, 255, 255, 64};
    });
    if (!frame) {
        return Result<void>::Failure(frame.Error());
    }

    frame_ = frame.Value();
    // 가로 그라디언트: 왼쪽 주황 → 오른쪽 보라, 위아래로 조금 어두워진다(Viewport 장면·mirror
    // 견본).
    auto gradient = Add(allocator, GradientWidth, GradientHeight, [](uint32 x, uint32 y) noexcept {
        const float u = static_cast<float>(x) / (GradientWidth - 1);
        const float v = static_cast<float>(y) / (GradientHeight - 1);
        const float shade = 1.0f - 0.4f * v;
        const auto channel = [&](float from, float to) noexcept {
            return static_cast<uint8>((from + (to - from) * u) * shade);
        };
        return Rgba{channel(255, 140), channel(150, 70), channel(40, 220), Opaque};
    });
    if (!gradient) {
        return Result<void>::Failure(gradient.Error());
    }

    gradient_ = gradient.Value();
    return Result<void>::Success();
}

SlotHandle GalleryTextures::Checker() const noexcept {
    return checker_;
}

SlotHandle GalleryTextures::Frame() const noexcept {
    return frame_;
}

SlotHandle GalleryTextures::Gradient() const noexcept {
    return gradient_;
}

} // namespace rocks::ui::gallery
