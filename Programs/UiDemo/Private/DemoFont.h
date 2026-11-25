#pragma once

#include <UiFramework/TextLabel.h>
#include <UiRender/GlyphAtlasTexture.h>

namespace rocks::ui {

// Win64 데모에 쓰는 숫자와 한글 레이블을 공용 글리프 아틀라스에 보관한다.
class DemoFont {
public:
    DemoFont(Allocator& allocator, graphics::Device& device, SlotArray<BrushTexture>& textures,
             graphics::PipelineHandle pipeline) noexcept;
    Result<void> Initialize() noexcept;
    Result<void> SetText(TextLabel& label, StringView text, int32 height) noexcept;

private:
    struct Glyph {
        char32_t code;
        int32 height;
        float advance;
        Vec2 offset;
        IntRect rect;
    };

    Result<Glyph> FindGlyph(char32_t code, int32 height) noexcept;
    Result<void> Upload() noexcept;

    Allocator& allocator_;
    Array<Glyph> glyphs_;
    text::GlyphAtlasPage page_;
    GlyphAtlasTexture texture_;
    graphics::PipelineHandle pipeline_;
    uint64 uploaded_ = 0;
};

} // namespace rocks::ui
