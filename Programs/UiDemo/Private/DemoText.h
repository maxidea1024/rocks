#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <TextLayout/CompositeFont.h>
#include <TextLayout/GlyphCache.h>
#include <TextLayout/TextShaping.h>
#include <UiCore/Style.h>
#include <UiFramework/Text/TextLabel.h>
#include <UiRender/GlyphCacheTextures.h>

// 데모 레이블 텍스트다(Docs/Design/UiToolkit 9.1절 UW2). 엔진 글꼴 시스템으로 shaping·래스터한다:
// TextLayoutBackend(FreeType·HarfBuzz·ICU)와 저장소 시험 글꼴(Roboto 기본, 나눔고딕 한글, Noto
// Naskh Arabic 아랍 문자, 컬러 이모지 마지막 fallback)의 조합 글꼴로 ShapeText → GlyphCache를 거쳐
// 글리프를 GPU 텍스처(GlyphCacheTextures)에 두고, grapheme 클러스터·줄 경계·BiDi 수준을 붙여
// TextLabel::SetShapedText에 넘긴다. 엔진 기본 글꼴 에셋(글꼴 설계 FT3)이 생기면 시험 글꼴 대신
// 그것을 쓴다.
//
// 사용 예:
//   DemoText text(allocator, device, textures, maskPipeline, imagePipeline);
//   if (!text.Initialize()) { ... }
//   text.SetText(*label, "레이블 Label", 18);
namespace rocks::ui {

class DemoText {
public:
    DemoText(Allocator& allocator, graphics::Device& device, SlotArray<BrushTexture>& textures,
             graphics::PipelineHandle maskPipeline,
             graphics::PipelineHandle imagePipeline) noexcept;
    ~DemoText() noexcept;

    DemoText(const DemoText&) = delete;
    DemoText& operator=(const DemoText&) = delete;

    // 글꼴 라이브러리·backend·조합 글꼴·글리프 cache를 준비한다. 엔진 폴더·ICU 데이터·글꼴 파일을
    // 찾지 못하면 NotFound, 글꼴 오류는 그 오류다.
    Result<void> Initialize() noexcept;

    // value를 픽셀 높이 pixelHeight(96 DPI 기준 em 크기)로 shaping해 label에 넣는다. 실패하면
    // label은 이전 텍스트를 유지한다.
    Result<void> SetText(TextLabel& label, StringView value, int32 pixelHeight) noexcept;

    // value를 글꼴 지정(typeface·96 DPI 포인트 크기 — UE FSlateFontInfo)으로 shaping해 label에
    // 넣는다.
    Result<void> SetText(TextLabel& label, StringView value, const FontStyle& font) noexcept;

private:
    Result<void> LoadFace(StringView relativePath,
                          SharedPtr<const text::FontFaceData>& out) noexcept;
    Result<void> Shape(TextLabel& label, StringView value, float size, StringView typeface,
                       int32 letterSpacing) noexcept;

    Allocator& allocator_;
    GlyphCacheTextures textures_;
    graphics::PipelineHandle maskPipeline_;
    graphics::PipelineHandle imagePipeline_;
    Array<uint8> icuData_;
    UniquePtr<text::TextBackend> backend_;
    UniquePtr<text::ResolvedCompositeFont> font_;
    UniquePtr<text::GlyphCache> cache_;
};

} // namespace rocks::ui
