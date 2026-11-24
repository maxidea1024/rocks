// 조합 글꼴 탐색이다(Docs/Design/Font 6.3·6.8절). 규칙은 UE
// FCompositeFontCache(FontCacheCompositeFont.cpp 198–208·394–406·614–679행)와 같다.
#include <TextLayout/CompositeFont.h>

namespace rocks::text {

namespace {

// 유니코드 code point의 최댓값이다.
constexpr uint32 MaxCodePoint = 0x10FFFF;

// 글리프가 없을 때 다시 고르는 대체 문자(U+FFFD)다.
constexpr uint32 ReplacementCharacter = 0xFFFD;

// culture 부모를 나누는 구분자다(ko-KR → ko, zh_Hant → zh).
bool IsCultureSeparator(char value) noexcept {
    return value == '-' || value == '_';
}

// culture가 목록의 culture이거나 그 자식(culture가 "목록-…")이면 true다. 대소문자는 ASCII 무시.
bool MatchesCulture(const Array<String>& cultures, StringView culture) noexcept {
    for (usize index = 0; index < cultures.Size(); ++index) {
        const StringView candidate = cultures[index].View();
        if (candidate.ByteCount() == 0 || candidate.ByteCount() > culture.ByteCount()) {
            continue;
        }

        if (!culture.StartsWith(candidate)) {
            continue;
        }

        if (candidate.ByteCount() == culture.ByteCount() ||
            IsCultureSeparator(culture.Data()[candidate.ByteCount()])) {
            return true;
        }
    }

    return false;
}

Result<void> OpenTypeface(Allocator& allocator, TextBackend& backend,
                          Span<const CompositeTypefaceEntry> entries, float scalingFactor,
                          CompositeFontTypeface& out) noexcept {
    if (!(scalingFactor > 0.0f)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    out.scalingFactor = scalingFactor;
    if (auto reserved = out.entries.TryReserve(entries.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < entries.Size(); ++index) {
        auto entry = out.entries.TryEmplaceBack(allocator);
        if (!entry) {
            return Result<void>::Failure(entry.Error());
        }

        if (auto named = entry.Value()->name.TryAssign(entries[index].name); !named) {
            return named;
        }

        auto face = backend.OpenFace(entries[index].face);
        if (!face) {
            return Result<void>::Failure(face.Error());
        }

        FontFaceInfo info(allocator);
        if (auto described = face.Value()->Info(info); !described) {
            return described;
        }

        entry.Value()->bold = info.bold;
        entry.Value()->italic = info.italic;
        entry.Value()->face = Move(face.Value());
    }

    return Result<void>::Success();
}

} // namespace

ResolvedCompositeFont::ResolvedCompositeFont(Allocator& allocator) noexcept
    : allocator_(allocator), defaultTypeface_(allocator), fallbackTypeface_(allocator),
      subTypefaces_(allocator), priorityRanges_(allocator), generalRanges_(allocator) {
}

Result<UniquePtr<ResolvedCompositeFont>>
ResolvedCompositeFont::TryCreate(Allocator& allocator, TextBackend& backend,
                                 const CompositeFontSpec& spec) noexcept {
    using Created = Result<UniquePtr<ResolvedCompositeFont>>;
    if (spec.defaultTypeface.Size() == 0) {
        return Created::Failure(ErrorCode::InvalidArgument);
    }

    auto made = UniquePtr<ResolvedCompositeFont>::TryMake(allocator, allocator);
    if (!made) {
        return Created::Failure(made.Error());
    }

    ResolvedCompositeFont& font = *made.Value();
    if (auto opened =
            OpenTypeface(allocator, backend, spec.defaultTypeface, 1.0f, font.defaultTypeface_);
        !opened) {
        return Created::Failure(opened.Error());
    }

    if (auto opened = OpenTypeface(allocator, backend, spec.fallbackTypeface,
                                   spec.fallbackScalingFactor, font.fallbackTypeface_);
        !opened) {
        return Created::Failure(opened.Error());
    }

    if (auto reserved = font.subTypefaces_.TryReserve(spec.subTypefaces.Size()); !reserved) {
        return Created::Failure(reserved.Error());
    }

    for (usize index = 0; index < spec.subTypefaces.Size(); ++index) {
        const CompositeSubTypeface& source = spec.subTypefaces[index];
        auto sub = font.subTypefaces_.TryEmplaceBack(allocator);
        if (!sub) {
            return Created::Failure(sub.Error());
        }

        if (source.typeface.Size() == 0) {
            return Created::Failure(ErrorCode::InvalidArgument);
        }

        if (auto opened = OpenTypeface(allocator, backend, source.typeface, source.scalingFactor,
                                       sub.Value()->typeface);
            !opened) {
            return Created::Failure(opened.Error());
        }

        for (usize range = 0; range < source.ranges.Size(); ++range) {
            const CodePointRange value = source.ranges[range];
            if (value.first > value.last || value.last > MaxCodePoint) {
                return Created::Failure(ErrorCode::InvalidArgument);
            }

            if (auto added = sub.Value()->ranges.TryPushBack(value); !added) {
                return Created::Failure(added.Error());
            }
        }

        for (usize culture = 0; culture < source.cultures.Size(); ++culture) {
            auto entry = sub.Value()->cultures.TryEmplaceBack(allocator);
            if (!entry) {
                return Created::Failure(entry.Error());
            }

            if (auto copied = entry.Value()->TryAssign(source.cultures[culture]); !copied) {
                return Created::Failure(copied.Error());
            }
        }
    }

    if (spec.lastResort.data.Get() != nullptr) {
        auto face = backend.OpenFace(spec.lastResort);
        if (!face) {
            return Created::Failure(face.Error());
        }

        font.lastResort_ = Move(face.Value());
    }

    // culture 없이 시작한다(일반 구간 표만 — culture 지정 보조 typeface는 무시).
    if (auto built = font.SetCulture(StringView()); !built) {
        return Created::Failure(built.Error());
    }

    return Created::Success(Move(made.Value()));
}

Result<void> ResolvedCompositeFont::SetCulture(StringView culture) noexcept {
    Array<CompositeFontRange> priority(allocator_);
    Array<CompositeFontRange> general(allocator_);
    uint32 order = 0;
    for (usize sub = 0; sub < subTypefaces_.Size(); ++sub) {
        const CompositeFontSubTypeface& value = subTypefaces_[sub];
        Array<CompositeFontRange>* target = nullptr;
        if (value.cultures.Size() == 0) {
            target = &general;
        } else if (culture.ByteCount() != 0 && MatchesCulture(value.cultures, culture)) {
            target = &priority;
        }

        for (usize range = 0; range < value.ranges.Size(); ++range, ++order) {
            if (target == nullptr) {
                continue;
            }

            const CompositeFontRange entry{value.ranges[range].first, value.ranges[range].last,
                                           static_cast<int32>(sub), order};
            if (auto added = target->TryPushBack(entry); !added) {
                return added;
            }
        }
    }

    priorityRanges_ = Move(priority);
    generalRanges_ = Move(general);
    return Result<void>::Success();
}

int32 ResolvedCompositeFont::FindRange(const Array<CompositeFontRange>& ranges,
                                       uint32 codePoint) const noexcept {
    // 덮는 구간 중 가장 작은 구간(크기가 같으면 먼저 선언한 구간)이 이긴다(UE 394–406행).
    int32 best = -1;
    uint32 bestSize = 0;
    uint32 bestOrder = 0;
    for (usize index = 0; index < ranges.Size(); ++index) {
        const CompositeFontRange& range = ranges[index];
        if (codePoint < range.first || codePoint > range.last) {
            continue;
        }

        const uint32 size = range.last - range.first;
        if (best < 0 || size < bestSize || (size == bestSize && range.order < bestOrder)) {
            best = range.subTypeface;
            bestSize = size;
            bestOrder = range.order;
        }
    }

    return best;
}

CompositeFontEntry& ResolvedCompositeFont::PickEntry(CompositeFontTypeface& typeface,
                                                     StringView typefaceName,
                                                     FontStyleRequest style) noexcept {
    for (usize index = 0; index < typeface.entries.Size(); ++index) {
        if (typeface.entries[index].name.View() == typefaceName) {
            return typeface.entries[index];
        }
    }

    // 이름이 없으면 속성(굵기·기울임) 일치 수가 가장 큰 entry, 같으면 먼저 선언한 entry다.
    usize best = 0;
    int32 bestScore = -1;
    for (usize index = 0; index < typeface.entries.Size(); ++index) {
        const CompositeFontEntry& entry = typeface.entries[index];
        const int32 score =
            (entry.bold == style.bold ? 1 : 0) + (entry.italic == style.italic ? 1 : 0);
        if (score > bestScore) {
            best = index;
            bestScore = score;
        }
    }

    return typeface.entries[best];
}

FontSelection ResolvedCompositeFont::SelectOnce(uint32 codePoint, StringView typefaceName,
                                                FontStyleRequest style,
                                                FontFallback fallback) noexcept {
    CompositeFontTypeface* chosen = &defaultTypeface_;
    int32 sub = FindRange(priorityRanges_, codePoint);
    if (sub < 0) {
        sub = FindRange(generalRanges_, codePoint);
    }

    if (sub >= 0) {
        chosen = &subTypefaces_[static_cast<usize>(sub)].typeface;
    }

    CompositeFontEntry& entry = PickEntry(*chosen, typefaceName, style);
    FontSelection selection{entry.face.Get(), chosen->scalingFactor,
                            entry.face->GlyphIndex(codePoint), codePoint};
    if (selection.glyphIndex != 0) {
        return selection;
    }

    if (chosen != &defaultTypeface_) {
        CompositeFontEntry& base = PickEntry(defaultTypeface_, typefaceName, style);
        const uint32 glyph = base.face->GlyphIndex(codePoint);
        if (glyph != 0) {
            return {base.face.Get(), defaultTypeface_.scalingFactor, glyph, codePoint};
        }
    }

    // fallback typeface는 글리프 검사 없이 받는다(UE 675행).
    if (fallbackTypeface_.entries.Size() != 0) {
        CompositeFontEntry& last = PickEntry(fallbackTypeface_, typefaceName, style);
        return {last.face.Get(), fallbackTypeface_.scalingFactor, last.face->GlyphIndex(codePoint),
                codePoint};
    }

    if (fallback != FontFallback::None && lastResort_.Get() != nullptr) {
        const uint32 glyph = lastResort_->GlyphIndex(codePoint);
        if (glyph != 0) {
            return {lastResort_.Get(), 1.0f, glyph, codePoint};
        }
    }

    return selection;
}

FontSelection ResolvedCompositeFont::Select(uint32 codePoint, StringView typefaceName,
                                            FontStyleRequest style,
                                            FontFallback fallback) noexcept {
    const FontSelection first = SelectOnce(codePoint, typefaceName, style, fallback);
    if (first.glyphIndex != 0 || codePoint == ReplacementCharacter) {
        return first;
    }

    // 어디에도 글리프가 없으면 U+FFFD로 다시 고른다(UE SlateTextShaper 599–604행).
    const FontSelection replacement =
        SelectOnce(ReplacementCharacter, typefaceName, style, fallback);
    return replacement.glyphIndex != 0 ? replacement : first;
}

} // namespace rocks::text
