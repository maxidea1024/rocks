#pragma once

#include "../DemoText.h"
#include "GalleryStrings.h"
#include "GalleryTextures.h"

#include <UiCore/Style.h>
#include <UiFramework/Button.h>
#include <UiFramework/TextLabel.h>

// 갤러리 탭들이 함께 쓰는 부품이다(Docs/Design/UiToolkit 16장). 텍스트는 데모 글꼴 조합으로
// shaping하고, 색은 지정하지 않으면 부모 전경색을 따른다(UE STextBlock 기본 UseForeground).
namespace rocks::ui {

class VectorImageAtlas;

} // namespace rocks::ui

namespace rocks::ui::gallery {

// 갤러리 탭을 만드는 동안과 창이 열려 있는 동안 살아 있는 공용 자원이다(GalleryWindow가 소유하고,
// 탭 내용보다 오래 산다).
struct GalleryContext {
    Allocator& allocator;
    DemoText& text;
    const GalleryStrings& strings;
    // 이미지·9분할·장면 견본용 절차 생성 텍스처다.
    const GalleryTextures& textures;
    // 등록한 SVG 아이콘이다(Icons 탭 목록 — 없으면 빈 탭).
    const VectorImageAtlas* icons = nullptr;
};

// UE 갤러리 이름표 색(AppStyle "Colors.White50" — 흰색 50%)이다.
inline constexpr LinearColor LabelColor{1.0f, 1.0f, 1.0f, 0.5f};

// UE 글꼴 지정(StarshipCoreStyle.cpp 34–44행)이다.
inline constexpr FontStyle NormalFont{Typeface::Regular, NormalFontSize};
inline constexpr FontStyle NormalBoldFont{Typeface::Bold, NormalFontSize};
inline constexpr FontStyle SmallFont{Typeface::Regular, SmallFontSize};
inline constexpr FontStyle SmallBoldFont{Typeface::Bold, SmallFontSize};
inline constexpr FontStyle LargeFont{Typeface::Regular, LargeFontSize};

// value를 font로 shaping한 레이블이다. color가 없으면 전경색을 상속한다.
Result<SharedPtr<TextLabel>> MakeText(GalleryContext& context, StringView value,
                                      const FontStyle& font = NormalFont,
                                      Optional<ColorRef> color = {}) noexcept;

// 레이블 하나를 담은 버튼이다(UE 갤러리 LeftRightLabel의 텍스트만 있는 경우).
Result<SharedPtr<Button>> MakeTextButton(GalleryContext& context, StringView label,
                                         const ButtonStyle& style) noexcept;

// content를 그리지 않는 틀에 넣고 padding(왼쪽, 위, 오른쪽, 아래)만큼 띄운다(UE SBorder Padding·
// 빈 brush).
Result<SharedPtr<Element>> PadContent(GalleryContext& context, const SharedPtr<Element>& content,
                                      Vec4 padding) noexcept;

// content를 고정 크기 상자에 담는다(UE SBox WidthOverride·HeightOverride).
Result<SharedPtr<Element>> Sized(GalleryContext& context, const SharedPtr<Element>& content,
                                 Vec2 size) noexcept;

// 묶음 제목(흰색 50%)이다(UE 갤러리 "BASE COLORS" 같은 머리).
Result<SharedPtr<TextLabel>> MakeSectionTitle(GalleryContext& context, StringView title) noexcept;

// 여러 결과 중 처음 실패한 것의 오류다. 모두 성공이면 ErrorCode::None이다.
template <typename... Results>
ErrorCode FirstError(const Results&... results) noexcept {
    ErrorCode code = ErrorCode::None;
    const auto take = [&code](const auto& result) noexcept {
        if (code == ErrorCode::None && !result) {
            code = result.Error();
        }
    };
    (take(results), ...);
    return code;
}

// 결과가 실패면 첫 오류를 보존하는 도우미다. 탭을 짜는 긴 순서에서 오류 검사를 한곳에 모은다.
class BuildStatus {
public:
    // what은 실패를 로그에 남길 때의 견본 이름이다(첫 실패만 남긴다).
    template <typename T>
    bool Take(Result<T>& result, StringView what = {}) noexcept {
        if (status_ && !result) {
            status_ = Result<void>::Failure(result.Error());
            ReportFailure(what, result.Error());
        }

        return static_cast<bool>(status_);
    }

    bool Ok() const noexcept {
        return static_cast<bool>(status_);
    }

    ErrorCode Error() const noexcept {
        return status_.Error();
    }

private:
    static void ReportFailure(StringView what, ErrorCode error) noexcept;

    Result<void> status_ = Result<void>::Success();
};

} // namespace rocks::ui::gallery
