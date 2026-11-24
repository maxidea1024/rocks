#pragma once

#include <AssetDatabase/AssetTextResolver.h>
#include <AssetDatabase/EditorPackageSet.h>
#include <AssetDatabase/ProjectLayout.h>
#include <Core/Algorithms.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Guid.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <ReflectionText/TextCodecOptions.h>

// AssetDatabase 모듈 안에서 함께 쓰는 도우미다(Docs/Design/AssetSystem 7.1절).
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

// Content 상대 이름 규칙(RefreshPaths 입력·감시 이벤트 경로): 비지 않은 유효한 UTF-8, 앞뒤 '/'
// 없음, 빈 구성 요소·
// "."·".." 없음.
inline bool IsContentRelativeName(StringView name) noexcept {
    if (name.IsEmpty() || !name.IsValidUtf8()) {
        return false;
    }

    usize start = 0;
    for (usize at = 0; at <= name.ByteCount(); ++at) {
        if (at < name.ByteCount() && name.Data()[at] != '/') {
            continue;
        }

        const StringView component(name.Data() + start, at - start);
        if (component.IsEmpty() || component == "." || component == "..") {
            return false;
        }

        start = at + 1;
    }

    return true;
}

// 진단·보고의 표시 이름을 out에 쓴다(AssetIndexDiagnostic::fileName·별칭 prune의 파일 이름). 한
// 줄·유효한 UTF-8이고 서로 다른 OS 이름이 같은 표시 이름이 되지 않게 역슬래시·제어
// 문자(0x00–0x1f·0x7f)와, 이름이 유효한 UTF-8이 아니면 0x80 이상 byte를 16진(역슬래시 x 두
// 자리)으로 적는다. 나머지는 그대로다(유효한 UTF-8 이름의 비ASCII 문자 포함). 길이를 먼저 세어
// 버퍼를 한 번 잡고 채운다.
inline Result<void> AssignDisplayNameAs(String& out, StringView raw, bool escapeHigh) noexcept {
    constexpr uint8 FirstPrintable = 0x20;
    constexpr uint8 Delete = 0x7f;
    constexpr uint8 FirstNonAscii = 0x80;
    constexpr usize EscapeBytes = 4;
    constexpr char Digits[] = "0123456789abcdef";
    constexpr uint32 NibbleBits = 4;
    constexpr uint8 NibbleMask = 0x0f;
    auto escaped = [&](uint8 value) noexcept {
        return value < FirstPrintable || value == Delete || value == '\\' ||
               (escapeHigh && value >= FirstNonAscii);
    };
    usize bytes = 0;
    for (usize index = 0; index < raw.ByteCount(); ++index) {
        bytes += escaped(static_cast<uint8>(raw.Data()[index])) ? EscapeBytes : 1;
    }

    Array<char> text(out.GetAllocator());
    if (auto sized = text.TryResize(bytes); !sized) {
        return sized;
    }

    usize at = 0;
    for (usize index = 0; index < raw.ByteCount(); ++index) {
        const uint8 value = static_cast<uint8>(raw.Data()[index]);
        if (!escaped(value)) {
            text[at++] = raw.Data()[index];
            continue;
        }

        text[at++] = '\\';
        text[at++] = 'x';
        text[at++] = Digits[value >> NibbleBits];
        text[at++] = Digits[value & NibbleMask];
    }

    return out.TryAssign(StringView(text.Data(), text.Size()));
}

inline Result<void> AssignDisplayName(String& out, StringView raw) noexcept {
    return AssignDisplayNameAs(out, raw, !raw.IsValidUtf8());
}

// 비교 키 집합이다(Add로 모으고 Finish가 정렬·중복 제거한 뒤 찾는다 — RefreshPaths·감시의 이름
// 대조, O(log n) 찾기).
class NameSet {
public:
    explicit NameSet(Allocator& allocator) noexcept : keys_(allocator) {
    }

    Result<void> Add(StringView key) noexcept {
        String copy(keys_.GetAllocator());
        if (auto assigned = copy.TryAssign(key); !assigned) {
            return assigned;
        }

        return keys_.TryPushBack(Move(copy));
    }

    void Finish() noexcept {
        algo::Sort(keys_.View(), [](const String& left, const String& right) noexcept {
            return StringView::CompareBytes(left.View(), right.View()) < 0;
        });
        usize kept = 0;
        for (usize at = 0; at < keys_.Size(); ++at) {
            if (kept != 0 && keys_[kept - 1].View() == keys_[at].View()) {
                continue;
            }

            if (kept != at) {
                keys_[kept] = Move(keys_[at]);
            }

            ++kept;
        }

        while (keys_.Size() > kept) {
            (void)keys_.TryPopBack();
        }
    }

    bool Contains(StringView key) const noexcept {
        const usize at =
            algo::LowerBound(keys_.View(), key, [](const String& value, StringView probe) noexcept {
                return StringView::CompareBytes(value.View(), probe) < 0;
            });
        return at != keys_.Size() && keys_[at].View() == key;
    }

    // key의 조상(빈 문자열 = Content 포함, key 자신 제외)이 집합에 있는지다.
    bool Above(StringView key) const noexcept {
        if (key.IsEmpty()) {
            return false;
        }

        if (Contains(StringView())) {
            return true;
        }

        for (usize at = 0; at < key.ByteCount(); ++at) {
            if (key.Data()[at] == '/' && Contains(StringView(key.Data(), at))) {
                return true;
            }
        }

        return false;
    }

    // key 자신이나 그 조상이 집합에 있는지다.
    bool Covers(StringView key) const noexcept {
        return Contains(key) || Above(key);
    }

private:
    Array<String> keys_;
};

// key가 base와 같거나 그 아래인지다(구성 요소 경계 '/' — 논리 경로 키·Content 상대 이름 공용).
inline bool IsSameOrUnder(StringView key, StringView base) noexcept {
    if (key == base) {
        return true;
    }

    return key.ByteCount() > base.ByteCount() &&
           key.StartsWith(base, StringComparison::CaseSensitive) &&
           key.Data()[base.ByteCount()] == '/';
}

// text(구성 요소를 '/'로 이은 이름)에서 앞 components개 구성 요소를 뺀 나머지다(앞 '/' 포함 —
// 없으면 빈 문자열). 논리 경로 텍스트의 앞부분(마운트·디렉터리)을 바꿀 때 쓴다(키와 텍스트의 byte
// 수가 달라 구성 요소 수로 자른다).
inline StringView AfterComponents(StringView text, usize components) noexcept {
    usize seen = 0;
    for (usize at = 0; at < text.ByteCount(); ++at) {
        if (text.Data()[at] == '/' && at != 0) {
            ++seen;
            if (seen == components) {
                return StringView(text.Data() + at, text.ByteCount() - at);
            }
        }
    }

    return StringView();
}

// 논리 경로 텍스트의 구성 요소 수다(앞 '/' 다음부터 '/'로 나눈 것 — "/Game/A"는 2).
inline usize LogicalComponents(StringView text) noexcept {
    usize count = 0;
    for (usize at = 0; at < text.ByteCount(); ++at) {
        count += text.Data()[at] == '/' ? 1 : 0;
    }

    return count;
}

// 대입 codec 옵션이다(Edit 용도, 참조는 resolver, Name은 집합 Registry의 pool — asset-new·set·
// import가 같이 쓴다).
inline reflection_text::TextCodecOptions CodecOptions(EditorPackageSet& set,
                                                      AssetTextResolver& resolver) noexcept {
    reflection_text::TextCodecOptions options;
    options.resolver = &resolver;
    options.names = set.Registry().Names();
    return options;
}

// path(절대 OS 경로)의 파일 전체를 bytes에 읽는다. 없으면 existed 거짓·빈 bytes로 성공한다. 크기가
// limit보다 크면 tooLarge 참으로 성공한다(읽지 않는다). 오류: 열기·크기·읽기 IO 오류(원본 오류 코드
// — 크기를 잰 뒤 줄었으면 EndOfFile), 할당 실패. 별칭 표·원본 루트 표·임포트 원본 읽기가 쓴다.
Result<void> ReadWhole(platform::FileSystem& files, StringView path, uint64 limit,
                       Array<uint8>& bytes, bool& existed, bool& tooLarge) noexcept;

// 대입들의 경로를 override 단위로 정규화해 pending에 더한다(Docs/Design/RuntimeClassMetadata.ko.md
// 'A5b' — asset-set·asset-new·임포트 설정 대입). 값 게시 전에 부른다(게시 뒤에는 예약한 자리로
// 합치기만 한다). 경로는 lease 스키마로 해석한다: 첫 `[` 앞까지(원소·key는 필드 단위), 해석이
// 실패하면 마지막 `.` 구간을 떼어 다시(InstancedStruct 내용·Optional 안은 그 필드까지). 오류: 할당
// 실패.
Result<void> PlanAssignmentOverrides(const TypeLease& lease,
                                     Span<const reflection_text::TextAssignment> assignments,
                                     OverrideSet& pending) noexcept;

// 대입들의 최상위 필드 이름(첫 `.`·`[` 앞)을 처음 나온 순서로 중복 없이 out에 더한다(설정 정규화
// hook의 assignedFields — Docs/Design/AssetSystem 12.12절). out의 이름은 대입 경로 byte를 가리킨다.
// 오류: 할당 실패.
Result<void> CollectAssignedFields(Span<const reflection_text::TextAssignment> assignments,
                                   Array<StringView>& out) noexcept;

// 두 단위 집합에 한쪽이 다른 쪽의 접두인(같은 단위 포함) 단위 쌍이 있는지다(asset-set 묶음 안
// Reset과 텍스트 대입의 겹침 — A5b2, 두 집합 크기의 곱에 비례).
bool UnitsOverlap(const OverrideSet& left, const OverrideSet& right) noexcept;

} // namespace rocks::asset_database_detail
