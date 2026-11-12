#include "UnicodeTables.generated.h"

#include <Core/Containers/Array.h>
#include <Core/Text/Unicode.h>

namespace rocks {
namespace {

namespace tables = detail::unicode;

constexpr usize MaxInputBytes = usize{64} << 20;

// Hangul 음절의 알고리즘 분해·합성(Unicode 본문 §3.12).
constexpr char32_t SBase = 0xAC00;
constexpr char32_t LBase = 0x1100;
constexpr char32_t VBase = 0x1161;
constexpr char32_t TBase = 0x11A7;
constexpr uint32 LCount = 19;
constexpr uint32 VCount = 21;
constexpr uint32 TCount = 28;
constexpr uint32 NCount = VCount * TCount;
constexpr uint32 SCount = LCount * NCount;

constexpr uint32 QuickYes = 0;
constexpr uint32 QuickNo = 1;
constexpr uint32 QuickMaybe = 2;

uint32 PropertyRecord(char32_t point) noexcept {
    if (point > 0x10FFFF) {
        return static_cast<uint32>(GeneralCategory::Cn) << 8;
    }
    const uint32 block = tables::PropertyStage1[point >> 8];
    return tables::PropertyRecords[tables::PropertyBlocks[block * 256u + (point & 0xFFu)]];
}

uint32 CombiningClassOf(char32_t point) noexcept {
    return PropertyRecord(point) & 0xFFu;
}

uint32 QuickCheckOf(NormalizationForm form, char32_t point) noexcept {
    const uint32 record = PropertyRecord(point);
    return form == NormalizationForm::NFC ? (record >> 13) & 3u : (record >> 15) & 1u;
}

bool IsHangulSyllable(char32_t point) noexcept {
    return point >= SBase && point < SBase + SCount;
}

const tables::DecompositionEntry* FindDecomposition(char32_t point) noexcept {
    usize low = 0;
    usize high = tables::DecompositionEntriesCount;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const uint32 code = tables::DecompositionEntries[middle].codePoint;
        if (code == point) {
            return &tables::DecompositionEntries[middle];
        }
        if (code < point) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return nullptr;
}

// 정준 합성 결과다. 합성하지 않으면 0이다(0은 어떤 합성의 결과도 아니다).
char32_t Compose(char32_t first, char32_t second) noexcept {
    if (first >= LBase && first < LBase + LCount && second >= VBase && second < VBase + VCount) {
        return SBase + ((first - LBase) * VCount + (second - VBase)) * TCount;
    }
    if (IsHangulSyllable(first) && (first - SBase) % TCount == 0 && second > TBase &&
        second < TBase + TCount) {
        return first + (second - TBase);
    }
    const uint64 key = (static_cast<uint64>(first) << 21) | second;
    usize low = 0;
    usize high = tables::CompositionEntriesCount;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const uint64 candidate = tables::CompositionEntries[middle].key;
        if (candidate == key) {
            return tables::CompositionEntries[middle].composite;
        }
        if (candidate < key) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return 0;
}

// 정규화 한 번의 상태다. 조각(segment)은 다음 경계 문자 앞에서 끝나며 정준 순서·합성을 거쳐
// 내보낸다.
class Normalizer {
public:
    Normalizer(NormalizationForm form, Allocator& allocator, StringBuilderBase& out) noexcept
        : form_(form), segment_(allocator), scratch_(allocator), out_(out) {
    }

    Result<void> Add(char32_t point) noexcept {
        if (IsHangulSyllable(point)) {
            const uint32 index = point - SBase;
            if (auto added = AddDecomposed(LBase + index / NCount); !added) {
                return added;
            }
            if (auto added = AddDecomposed(VBase + (index % NCount) / TCount); !added) {
                return added;
            }
            if (index % TCount != 0) {
                return AddDecomposed(TBase + index % TCount);
            }
            return Result<void>::Success();
        }
        if (const tables::DecompositionEntry* entry = FindDecomposition(point)) {
            for (uint32 index = 0; index < entry->length; ++index) {
                if (auto added = AddDecomposed(tables::DecompositionData[entry->offset + index]);
                    !added) {
                    return added;
                }
            }
            return Result<void>::Success();
        }
        return AddDecomposed(point);
    }

    Result<void> Finish() noexcept {
        return Flush();
    }

private:
    // 분해된 문자 하나를 조각에 넣는다. 결합 등급 0이고 (NFC면) 앞 문자와 합성될 수 없는 문자
    // (NFC_QC = Yes)는 앞 조각과 독립이므로 그 앞에서 조각을 끊는다.
    Result<void> AddDecomposed(char32_t point) noexcept {
        if (!segment_.IsEmpty() && CombiningClassOf(point) == 0 &&
            (form_ == NormalizationForm::NFD || QuickCheckOf(form_, point) == QuickYes)) {
            if (auto flushed = Flush(); !flushed) {
                return flushed;
            }
        }
        return segment_.TryPushBack(point);
    }

    Result<void> Flush() noexcept {
        if (segment_.IsEmpty()) {
            return Result<void>::Success();
        }
        if (auto ordered = OrderCanonically(); !ordered) {
            return ordered;
        }
        usize count = segment_.Size();
        if (form_ == NormalizationForm::NFC) {
            count = ComposeSegment();
        }
        for (usize index = 0; index < count; ++index) {
            if (auto appended = out_.TryAppendCodePoint(segment_[index]); !appended) {
                return appended;
            }
        }
        segment_.Clear();
        return Result<void>::Success();
    }

    // 결합 등급이 0이 아닌 연속 구간마다 결합 등급으로 안정 정렬한다. 짧은 구간은 삽입 정렬, 긴
    // 구간은 결합 등급 계수 정렬(O(n))이다.
    Result<void> OrderCanonically() noexcept {
        char32_t* points = segment_.Data();
        const usize size = segment_.Size();
        usize start = 0;
        while (start < size) {
            if (CombiningClassOf(points[start]) == 0) {
                ++start;
                continue;
            }
            usize end = start;
            while (end < size && CombiningClassOf(points[end]) != 0) {
                ++end;
            }
            if (end - start <= 32) {
                for (usize index = start + 1; index < end; ++index) {
                    const char32_t value = points[index];
                    const uint32 combining = CombiningClassOf(value);
                    usize position = index;
                    while (position > start && CombiningClassOf(points[position - 1]) > combining) {
                        points[position] = points[position - 1];
                        --position;
                    }
                    points[position] = value;
                }
            } else {
                if (auto sorted = CountingSort(points + start, end - start); !sorted) {
                    return sorted;
                }
                points = segment_.Data();
            }
            start = end;
        }

        return Result<void>::Success();
    }

    Result<void> CountingSort(char32_t* points, usize count) noexcept {
        usize counts[257] = {};
        for (usize index = 0; index < count; ++index) {
            ++counts[CombiningClassOf(points[index]) + 1];
        }
        for (usize bucket = 1; bucket < 257; ++bucket) {
            counts[bucket] += counts[bucket - 1];
        }
        scratch_.Clear();
        if (auto resized = scratch_.TryResize(count); !resized) {
            return resized;
        }
        for (usize index = 0; index < count; ++index) {
            scratch_[counts[CombiningClassOf(points[index])]++] = points[index];
        }
        for (usize index = 0; index < count; ++index) {
            points[index] = scratch_[index];
        }
        return Result<void>::Success();
    }

    // 정준 합성(UAX #15): 마지막 시작 문자와 그 뒤 문자가 차단되지 않았고 합성 쌍이 있으면 합친다.
    // 결과 길이를 반환한다(조각 앞부분에 다시 쓴다).
    usize ComposeSegment() noexcept {
        char32_t* points = segment_.Data();
        const usize size = segment_.Size();
        usize starter = 0;
        uint32 lastClass = CombiningClassOf(points[0]);
        if (lastClass != 0) {
            lastClass = 256; // 비시작 문자로 시작하면 앞에 합성할 시작 문자가 없다.
        }
        usize written = 1;
        for (usize index = 1; index < size; ++index) {
            const char32_t point = points[index];
            const uint32 combining = CombiningClassOf(point);
            const char32_t composite = lastClass == 256 ? 0 : Compose(points[starter], point);
            if (composite != 0 && (lastClass < combining || lastClass == 0)) {
                points[starter] = composite;
                continue;
            }
            if (combining == 0) {
                starter = written;
            }
            lastClass = combining;
            points[written++] = point;
        }

        return written;
    }

    NormalizationForm form_;
    Array<char32_t> segment_;
    Array<char32_t> scratch_;
    StringBuilderBase& out_;
};

Result<CodePointRange> CheckedCodePoints(StringView input) noexcept {
    if (input.ByteCount() > MaxInputBytes) {
        return Result<CodePointRange>::Failure(ErrorCode::Overflow);
    }
    return input.TryCodePoints();
}

// 정규화 결과를 임시 builder에 만든 뒤 out에 한 번에 붙인다(실패하면 out은 그대로).
Result<void> NormalizeInto(NormalizationForm form, StringView input,
                           StringBuilderBase& out) noexcept {
    auto points = CheckedCodePoints(input);
    if (!points) {
        return Result<void>::Failure(points.Error());
    }
    StringBuilder<256> result(out.GetAllocator());
    Normalizer normalizer(form, out.GetAllocator(), result);
    for (char32_t point : points.Value()) {
        if (auto added = normalizer.Add(point); !added) {
            return added;
        }
    }
    if (auto finished = normalizer.Finish(); !finished) {
        return finished;
    }
    return out.TryAppend(result.View());
}

} // namespace

StringView UnicodeVersionText() noexcept {
    return StringView(tables::UnicodeVersion);
}

Result<void> TryNormalize(NormalizationForm form, StringView input,
                          StringBuilderBase& out) noexcept {
    return NormalizeInto(form, input, out);
}

Result<bool> IsNormalized(NormalizationForm form, StringView input, Allocator& allocator) noexcept {
    auto points = CheckedCodePoints(input);
    if (!points) {
        return Result<bool>::Failure(points.Error());
    }
    uint32 lastClass = 0;
    bool maybe = false;
    for (char32_t point : points.Value()) {
        const uint32 combining = CombiningClassOf(point);
        if (combining != 0 && lastClass > combining) {
            return Result<bool>::Success(false);
        }
        const uint32 quick = QuickCheckOf(form, point);
        if (quick == QuickNo) {
            return Result<bool>::Success(false);
        }
        maybe = maybe || quick == QuickMaybe;
        lastClass = combining;
    }
    if (!maybe) {
        return Result<bool>::Success(true);
    }
    // 애매한 경우는 정규화 결과와 비교한다.
    StringBuilder<256> normalized(allocator);
    if (auto made = NormalizeInto(form, input, normalized); !made) {
        return Result<bool>::Failure(made.Error());
    }
    return Result<bool>::Success(normalized.View() == input);
}

Result<void> TrySimpleCaseFold(StringView input, StringBuilderBase& out) noexcept {
    auto points = CheckedCodePoints(input);
    if (!points) {
        return Result<void>::Failure(points.Error());
    }
    StringBuilder<256> result(out.GetAllocator());
    for (char32_t point : points.Value()) {
        if (auto appended = result.TryAppendCodePoint(SimpleCaseFold(point)); !appended) {
            return appended;
        }
    }
    return out.TryAppend(result.View());
}

Result<void> TryMakeCaselessKey(StringView input, StringBuilderBase& out) noexcept {
    StringBuilder<256> decomposed(out.GetAllocator());
    if (auto made = NormalizeInto(NormalizationForm::NFD, input, decomposed); !made) {
        return made;
    }
    StringBuilder<256> folded(out.GetAllocator());
    if (auto made = TrySimpleCaseFold(decomposed.View(), folded); !made) {
        return made;
    }
    return NormalizeInto(NormalizationForm::NFC, folded.View(), out);
}

GeneralCategory GetGeneralCategory(char32_t codePoint) noexcept {
    return static_cast<GeneralCategory>((PropertyRecord(codePoint) >> 8) & 0x1Fu);
}

uint8 GetCanonicalCombiningClass(char32_t codePoint) noexcept {
    return static_cast<uint8>(CombiningClassOf(codePoint));
}

bool IsDefaultIgnorable(char32_t codePoint) noexcept {
    return ((PropertyRecord(codePoint) >> 16) & 1u) != 0;
}

char32_t SimpleCaseFold(char32_t codePoint) noexcept {
    usize low = 0;
    usize high = tables::FoldingRangesCount;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const tables::FoldingRange& range = tables::FoldingRanges[middle];
        if (codePoint < range.first) {
            high = middle;
        } else if (codePoint > range.last) {
            low = middle + 1;
        } else {
            if ((codePoint - range.first) % range.stride != 0) {
                return codePoint;
            }
            return static_cast<char32_t>(static_cast<int64>(codePoint) + range.delta);
        }
    }

    return codePoint;
}

} // namespace rocks
