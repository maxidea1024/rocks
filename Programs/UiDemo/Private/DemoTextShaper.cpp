#include "DemoTextShaper.h"

#include <Localization/Catalog.h>

namespace rocks::ui {

DemoTextShaper::DemoTextShaper(Allocator& allocator, DemoText& text) noexcept
    : allocator_(allocator), text_(text) {
}

Result<void> DemoTextShaper::Shape(TextLabel& label, const Text& text, float fontSize) noexcept {
    // 빈 스냅샷은 번역 없음이다 — 지역화 Text는 원문으로 풀린다.
    const CatalogSnapshot catalog;
    auto resolved = ResolveText(allocator_, text, catalog);
    if (!resolved) {
        return Result<void>::Failure(resolved.Error());
    }

    return text_.SetText(label, resolved.Value().value.View(),
                         FontStyle{Typeface::Regular, fontSize});
}

Result<void> DemoTextShaper::FindWordBoundaries(StringView utf8, Array<usize>& out) noexcept {
    return text_.FindWordBoundaries(utf8, out);
}

} // namespace rocks::ui
