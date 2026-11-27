#pragma once

#include <UiCore/Rendering/UiRenderer.h>

namespace rocks::ui {

// 그리지 않는 렌더러다(UE FSlateNullRenderer). headless 실행과 CPU 시험에 쓴다. id를 발급·반환하고
// 그리기 호출을 센다. 캡처는 0으로 채운 픽셀이다.
class ROCKS_UICORE_API NullUiRenderer final : public UiRenderer {
public:
    struct Stats {
        uint64 drawCalls = 0;
        uint64 targetsDrawn = 0;
        uint64 passesDrawn = 0;
        uint64 packetsDrawn = 0;
        uint64 textureUpdates = 0;
        uint64 passesSkipped = 0;
    };

    explicit NullUiRenderer(Allocator& allocator) noexcept;

    Result<UiWindowSurfaceId>
    CreateWindowSurface(const UiWindowSurfaceDesc& desc) noexcept override;
    Result<void> ResizeWindowSurface(UiWindowSurfaceId surface, uint32 width,
                                     uint32 height) noexcept override;
    Result<void> DestroyWindowSurface(UiWindowSurfaceId surface) noexcept override;
    Result<UiTextureId> CreateTexture(const UiTextureDesc& desc,
                                      Span<const uint8> pixels) noexcept override;
    Result<void> UpdateTexture(UiTextureId texture, Span<const uint8> pixels) noexcept override;
    Result<void> ResizeRenderTarget(UiTextureId texture, uint32 width,
                                    uint32 height) noexcept override;
    void ReleaseTexture(UiTextureId texture) noexcept override;
    Result<UiTextureDesc> DescribeTexture(UiTextureId texture) const noexcept override;
    Result<void> DrawWindows(UiDrawBuffer& buffer) noexcept override;
    Result<uint64> RequestCapture(UiTextureId target) noexcept override;
    Result<void> WaitCapture(uint64 capture) noexcept override;
    Result<void> TakeCapture(uint64 capture, Array<uint8>& rgba) noexcept override;
    Result<void> Flush() noexcept override;

    // 살아 있는 텍스처·창 표면 수다(누수 확인).
    usize LiveTextures() const noexcept;
    usize LiveWindowSurfaces() const noexcept;
    const Stats& Statistics() const noexcept;

private:
    struct Capture {
        uint64 id = 0;
        uint32 width = 0;
        uint32 height = 0;
    };

    SlotArray<UiTextureDesc> textures_;
    SlotArray<UiWindowSurfaceDesc> windows_;
    Array<Capture> captures_;
    uint64 nextCapture_ = 0;
    Stats stats_;
};

} // namespace rocks::ui
