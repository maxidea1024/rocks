#pragma once

#include <UiCore/Rendering/UiRenderer.h>

// 갤러리 견본용 절차 생성 텍스처다(Docs/Design/UiToolkit 16.10절). 이미지·9분할 Box·Border brush,
// 진행 막대 Mask, Viewport 장면처럼 텍스처가 있어야 보이는 견본에 쓴다. 에셋 파일 없이 시작할 때
// 렌더러 텍스처로 만들고, 소멸할 때 돌려준다.
//
// 사용 예:
//   GalleryTextures textures(renderer);
//   if (!textures.Create(allocator)) { ... }
//   brush.resource = textures.Checker();
namespace rocks::ui::gallery {

class GalleryTextures {
public:
    explicit GalleryTextures(UiRenderer& renderer) noexcept;
    ~GalleryTextures() noexcept;

    GalleryTextures(const GalleryTextures&) = delete;
    GalleryTextures& operator=(const GalleryTextures&) = delete;

    Result<void> Create(Allocator& allocator) noexcept;

    // 체크무늬(64x64, 8칸)·9분할 테두리(32x32, 모서리 8 — 여백 비율 0.25)·가로
    // 그라디언트(256x64)다.
    SlotHandle Checker() const noexcept;
    SlotHandle Frame() const noexcept;
    SlotHandle Gradient() const noexcept;

    // 9분할 테두리 텍스처의 여백 비율이다(Brush::margin에 넣는다).
    static constexpr float FrameMargin = 0.25f;

private:
    template <typename Pixel>
    Result<SlotHandle> Add(Allocator& allocator, uint32 width, uint32 height,
                           Pixel&& pixel) noexcept;

    UiRenderer& renderer_;
    SlotHandle checker_{};
    SlotHandle frame_{};
    SlotHandle gradient_{};
};

} // namespace rocks::ui::gallery
