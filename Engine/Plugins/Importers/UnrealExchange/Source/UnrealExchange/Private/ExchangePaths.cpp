#include <UnrealExchange/ExchangePaths.h>

#include <Core/Encoding/Sha256.h>
#include <Core/Text/NameComponent.h>
#include <Core/Text/Unicode.h>
#include <UnrealExchange/UnrealExchangeFormat.h>

// UE 객체 경로 → Rocks 논리 경로 대응이다(Docs/Design/UnrealExchange.ko.md 9.1절, 10.4절 7·13번).

namespace rocks {

namespace {

// 정리한 이름에 해시 접미를 붙일 때 남기는 이름 앞부분의 byte 상한이다(64 - '-' - 해시 8자).
constexpr usize UnrealPathKeptBytes = UnrealPathComponentMaxBytes - 1 - UnrealPathHashChars;

constexpr char UnrealPathSeparator = '/';
constexpr char UnrealObjectSeparator = '.';
constexpr char UnrealPathReplacement = '_';
constexpr char UnrealPathPrefix = 'U';
constexpr char UnrealPathHashSeparator = '-';
constexpr uint8 AsciiControlLast = 0x1f;
constexpr uint8 AsciiDelete = 0x7f;

bool IsLetterOrNumber(GeneralCategory category) noexcept {
    switch (category) {
    case GeneralCategory::Lu:
    case GeneralCategory::Ll:
    case GeneralCategory::Lt:
    case GeneralCategory::Lm:
    case GeneralCategory::Lo:
    case GeneralCategory::Nd:
    case GeneralCategory::Nl:
    case GeneralCategory::No:
        return true;
    default:
        return false;
    }
}

bool IsMark(GeneralCategory category) noexcept {
    return category == GeneralCategory::Mn || category == GeneralCategory::Mc ||
           category == GeneralCategory::Me;
}

// 논리 경로 구성 요소에 그대로 둘 수 있는 code point인지 본다(AssetSystem 4.2절).
bool IsKeptCodePoint(char32_t point) noexcept {
    if (point == U'_' || point == U'-') {
        return true;
    }

    if (IsDefaultIgnorable(point)) {
        return false;
    }

    const GeneralCategory category = GetGeneralCategory(point);
    return IsLetterOrNumber(category) || IsMark(category);
}

// 구성 요소 하나를 정리해 out 뒤에 붙인다. original은 UE 구성 요소, prefix는 원래 경로의 그 구성
// 요소까지 접두(해시 입력)다. 정리 규칙으로 바뀌었으면 renamed를 참으로 둔다.
Result<void> AppendCleanComponent(StringView original, StringView prefix, StringBuilderBase& out,
                                  bool& renamed) noexcept {
    Allocator& allocator = out.GetAllocator();
    StringBuilder<UnrealPathComponentMaxBytes> normalized(allocator);
    if (auto made = TryNormalize(NormalizationForm::NFC, original, normalized); !made) {
        return made;
    }

    // 1. 받지 않는 code point를 '_'로 바꾼다.
    StringBuilder<UnrealPathComponentMaxBytes> cleaned(allocator);
    bool changed = false;
    auto points = normalized.View().TryCodePoints();
    if (!points) {
        return Result<void>::Failure(points.Error());
    }

    bool first = true;
    for (char32_t point : points.Value()) {
        const bool kept = IsKeptCodePoint(point);
        // 2. 정리한 첫 문자가 L*·N*가 아니면 'U'를 붙인다(L*이어도 Default_Ignorable이면 '_'가
        // 된다).
        if (first && !(kept && IsLetterOrNumber(GetGeneralCategory(point)))) {
            if (auto added = cleaned.TryAppendAscii(UnrealPathPrefix); !added) {
                return added;
            }

            changed = true;
        }

        first = false;
        if (kept) {
            if (auto added = cleaned.TryAppendCodePoint(static_cast<uint32>(point)); !added) {
                return added;
            }
        } else {
            if (auto added = cleaned.TryAppendAscii(UnrealPathReplacement); !added) {
                return added;
            }

            changed = true;
        }
    }

    // 3. Windows 예약 이름이면 'U'를 붙인다(판정은 Core 이름 규칙).
    NameComponentError reason = NameComponentError::None;
    if (auto valid = ValidateNameComponent(cleaned.View(), allocator, &reason); !valid) {
        if (reason == NameComponentError::None) {
            return valid;
        }
    }

    StringBuilder<UnrealPathComponentMaxBytes> named(allocator);
    if (reason == NameComponentError::ReservedDeviceName) {
        if (auto added = named.TryAppendAscii(UnrealPathPrefix); !added) {
            return added;
        }

        changed = true;
    }

    // 'U' 뒤에 결합 문자가 오면 합성될 수 있으므로 다시 NFC로 정규화한다.
    StringBuilder<UnrealPathComponentMaxBytes> composed(allocator);
    if (auto added = named.TryAppend(cleaned.View()); !added) {
        return added;
    }

    if (auto made = TryNormalize(NormalizationForm::NFC, named.View(), composed); !made) {
        return made;
    }

    if (!changed && composed.ByteCount() <= UnrealPathComponentMaxBytes) {
        return out.TryAppend(composed.View());
    }

    // 4. 잘라서 원래 경로 접두의 해시를 붙인다.
    renamed = true;
    const Sha256Digest digest = Sha256::Hash(
        Span<const uint8>(reinterpret_cast<const uint8*>(prefix.Data()), prefix.ByteCount()));
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    StringBuilder<UnrealPathComponentMaxBytes> suffixed(allocator);
    if (auto added = suffixed.TryAppend(composed.View().LeftBytesAtCodePoint(UnrealPathKeptBytes));
        !added) {
        return added;
    }

    if (auto added = suffixed.TryAppendAscii(UnrealPathHashSeparator); !added) {
        return added;
    }

    if (auto added = suffixed.TryAppend(StringView(hex, UnrealPathHashChars)); !added) {
        return added;
    }

    return TryNormalize(NormalizationForm::NFC, suffixed.View(), out);
}

} // namespace

bool IsValidUnrealObjectPath(StringView objectPath) noexcept {
    const usize size = objectPath.ByteCount();
    if (size == 0 || size > UnrealObjectPathMaxBytes ||
        objectPath.Data()[0] != UnrealPathSeparator || !objectPath.IsValidUtf8()) {
        return false;
    }

    usize dot = size;
    usize components = 0;
    usize componentBytes = 0;
    for (usize at = 0; at < size; ++at) {
        const char character = objectPath.Data()[at];
        const uint8 byte = static_cast<uint8>(character);
        if (byte <= AsciiControlLast || byte == AsciiDelete || character == ':' ||
            character == '\\') {
            return false;
        }

        if (character == UnrealObjectSeparator) {
            // '.'은 하나뿐이고 비지 않은 패키지 구성 요소 뒤에 온다.
            if (dot != size || componentBytes == 0) {
                return false;
            }

            dot = at;
            continue;
        }

        if (character == UnrealPathSeparator) {
            // 객체 이름 안의 '/'·빈 구성 요소는 받지 않는다.
            if (dot != size || (at != 0 && componentBytes == 0)) {
                return false;
            }

            ++components;
            componentBytes = 0;
            continue;
        }

        if (dot == size) {
            ++componentBytes;
        }
    }

    return dot != size && dot + 1 < size && components >= 2;
}

StringView UnrealPackagePath(StringView objectPath) noexcept {
    if (!IsValidUnrealObjectPath(objectPath)) {
        return StringView();
    }

    usize dot = 0;
    while (objectPath.Data()[dot] != UnrealObjectSeparator) {
        ++dot;
    }

    return StringView(objectPath.Data(), dot);
}

Result<void> ValidateUnrealImportRoot(StringView root, Allocator& scratch) noexcept {
    const usize size = root.ByteCount();
    if (size < 2 || size > UnrealPathMaxBytes || root.Data()[0] != UnrealPathSeparator) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    usize start = 1;
    for (usize at = 1; at <= size; ++at) {
        if (at != size && root.Data()[at] != UnrealPathSeparator) {
            continue;
        }

        if (auto valid =
                ValidateNameComponent(StringView(root.Data() + start, at - start), scratch);
            !valid) {
            return valid;
        }

        start = at + 1;
    }

    return Result<void>::Success();
}

Result<void> MapUnrealObjectPath(StringView objectPath, StringView root, StringBuilderBase& out,
                                 ExchangePathOutcome& outcome) noexcept {
    outcome = ExchangePathOutcome{};
    const StringView package = UnrealPackagePath(objectPath);
    if (package.IsEmpty()) {
        outcome.failure = ExchangePathOutcome::Failure::InvalidObjectPath;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = out.GetAllocator();
    if (auto valid = ValidateUnrealImportRoot(root, allocator); !valid) {
        if (valid.Error() == ErrorCode::InvalidArgument) {
            outcome.failure = ExchangePathOutcome::Failure::InvalidRoot;
        }

        return valid;
    }

    StringBuilder<UnrealPathMaxBytes> mapped(allocator);
    if (auto added = mapped.TryAppend(root); !added) {
        return added;
    }

    bool renamed = false;
    usize start = 1;
    const usize size = package.ByteCount();
    for (usize at = 1; at <= size; ++at) {
        if (at != size && package.Data()[at] != UnrealPathSeparator) {
            continue;
        }

        if (auto added = mapped.TryAppendAscii(UnrealPathSeparator); !added) {
            return added;
        }

        if (auto added = AppendCleanComponent(StringView(package.Data() + start, at - start),
                                              StringView(package.Data(), at), mapped, renamed);
            !added) {
            return added;
        }

        start = at + 1;
    }

    if (mapped.ByteCount() > UnrealPathMaxBytes) {
        outcome.failure = ExchangePathOutcome::Failure::TooLong;
        outcome.renamed = renamed;
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto added = out.TryAppend(mapped.View()); !added) {
        return added;
    }

    outcome.renamed = renamed;
    return Result<void>::Success();
}

} // namespace rocks
