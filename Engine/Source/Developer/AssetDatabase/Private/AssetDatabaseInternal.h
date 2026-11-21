#pragma once

#include <AssetDatabase/ProjectLayout.h>
#include <Core/Containers/Span.h>
#include <Core/Guid.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>

// AssetDatabase 모듈 안에서 함께 쓰는 도우미다(Docs/Design/AssetSystem.ko.md 7.1절).
namespace rocks::asset_database_detail {

// pieces를 차례로 out 뒤에 붙인다. 처음 실패한 붙이기의 오류를 돌려준다(그때까지 붙인 것은 남는다 —
// 호출자는 실패하면 out을 버린다). 짧은 조각 여러 개를 한 실패 지점으로 묶는다.
inline Result<void> AppendAll(StringBuilderBase& out, Span<const StringView> pieces) noexcept {
    for (StringView piece : pieces) {
        if (auto appended = out.TryAppend(piece); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

// guid 문자열(36자 소문자 하이픈 형식)의 길이와 하이픈 위치다.
inline constexpr usize EditorGuidTextBytes = 36;
inline constexpr usize EditorGuidHyphens[] = {8, 13, 18, 23};
inline constexpr StringView EditorTempSuffix = ".tmp";
static_assert(1 + EditorGuidTextBytes + 4 == TemporaryFileSuffixBytes,
              "Save temporary suffix must match the layout path limit");

inline bool EditorIsGuidHyphen(usize index) noexcept {
    for (usize hyphen : EditorGuidHyphens) {
        if (index == hyphen) {
            return true;
        }
    }

    return false;
}

// value를 36자 소문자 하이픈 형식으로 붙인다(high 16자리 다음 low 16자리).
inline Result<void> EditorAppendGuidText(String& out, Guid value) noexcept {
    constexpr char Digits[] = "0123456789abcdef";
    constexpr usize NibbleBits = 4;
    constexpr usize WordNibbles = 16;
    char text[EditorGuidTextBytes];
    usize nibble = 0;
    for (usize at = 0; at < EditorGuidTextBytes; ++at) {
        if (EditorIsGuidHyphen(at)) {
            text[at] = '-';
            continue;
        }

        const uint64 word = nibble < WordNibbles ? value.high : value.low;
        const usize shift = (WordNibbles - 1 - nibble % WordNibbles) * NibbleBits;
        text[at] = Digits[(word >> shift) & 0xf];
        ++nibble;
    }

    return out.TryAppend(StringView(text, EditorGuidTextBytes));
}

// "<stem>.<36자 소문자 guid>.tmp" 꼴의 임시 파일 이름이면 stem을 준다(아니면 빈 view). 저장·캐시
// 쓰기의 임시 파일(7.1절 15·16)과 고아 정리가 쓴다.
inline StringView EditorTempStem(StringView name) noexcept {
    const usize tail = 1 + EditorGuidTextBytes + EditorTempSuffix.ByteCount();
    if (name.ByteCount() <= tail ||
        !name.EndsWith(EditorTempSuffix, StringComparison::CaseSensitive)) {
        return StringView();
    }

    const usize guidStart = name.ByteCount() - EditorTempSuffix.ByteCount() - EditorGuidTextBytes;
    if (name.Data()[guidStart - 1] != '.') {
        return StringView();
    }

    for (usize at = 0; at < EditorGuidTextBytes; ++at) {
        const char c = name.Data()[guidStart + at];
        const bool valid =
            EditorIsGuidHyphen(at) ? c == '-' : ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        if (!valid) {
            return StringView();
        }
    }

    return StringView(name.Data(), guidStart - 1);
}

// 저장 임시 파일 이름(정확히 "<이름>.rasset.<36자 소문자 guid>.tmp", 이름은 비지 않음)인지다.
inline bool EditorIsOrphanTempName(StringView name) noexcept {
    const StringView stem = EditorTempStem(name);
    return stem.ByteCount() > EditorPackageExtension.ByteCount() &&
           stem.EndsWith(EditorPackageExtension);
}

} // namespace rocks::asset_database_detail
