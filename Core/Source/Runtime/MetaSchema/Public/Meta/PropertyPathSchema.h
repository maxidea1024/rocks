#pragma once

#include <Core/Text/Name.h>
#include <Core/Text/StringView.h>
#include <Meta/TypeSchema.h>

// 속성 경로 표현(PathSelector·PathKey·PathStep·PropertyPath)과 경로 비교다(리플렉션 B9f3 —
// Docs/Design/ReflectionExtensions.ko.md 1.5절). 경로 연산은 Object/PropertyPath.h가 둔다.

namespace rocks {

// 경로 단계가 필드 값 안의 무엇을 가리키는지다(리플렉션 B9f3 — Docs/Design/
// ReflectionExtensions.ko.md 1.5절).
//   None  — 필드(또는 struct 멤버) 자체다. index는 0이어야 한다.
//   Index — 배열 원소다. index가 원소 위치다(진단 경로에서는 Map 항목·Set 원소의 삽입 순번도 이
//           선택자로 쓴다 — 1.6절).
//   Key   — Map 항목·Set 원소다(편집 경로 전용). index는 PropertyPath::keys 안의 위치다.
enum class PathSelector : uint8 {
    None,
    Index,
    Key
};

// Map 키·Set 원소 하나의 값이다(리플렉션 B9f3 — 1.5절). kind는 키 종류(Map이면 FieldSchema::key의
// 종류, Set이면 element의 종류 — 성질 표의 Map 키 종류)이고 wire는 그 종류의 wire payload
// 정규형이다(ValueArray 원소와 같은 byte — Bool 1 byte 0·1, UInt8·UInt16·UInt32는 U32 4 byte,
// UInt64 8 byte, 부호 있는 정수·enum은 I64 8 byte, Guid·AssetId·ClassRef는 16 byte, String·Name은
// u64 길이 + UTF-8, Digest256은 u64 길이 32 + 32 byte, SoftObjectRef는 Guid + String, 정수는
// little-endian). wire는 비소유 뷰이며 경로를 쓰는 동안 유효해야 한다. key가 wire payload라서
// 경로를 Record로 그대로 인코딩할 수 있다(CU4의 소유형 경로). Object/PathKey.h의
// AppendPathKey·ReadPathKey가 정규형을 만들고 읽는다.
//
// 사용 예:
//   Array<uint8> wire(allocator);
//   PathKeyValue android;
//   android.kind = ValueKind::Name;
//   android.text = "Android";
//   if (auto made = AppendPathKey(android, wire); !made) { ... }
//   const PathKey keys[] = {{ValueKind::Name, wire.View()}};
struct PathKey {
    ValueKind kind = ValueKind::Opaque;
    Span<const uint8> wire{};
};

// 필드 경로의 한 단계다(7절 PropertyPath, 6.4절 진단 경로): 필드 ID와 선택자(PathSelector). 원소
// index는 배열 필드의 단계에 둔다([slots[2], material] — slots 단계가 selector Index, index 2를
// 가진다). Map 항목·Set 원소는 selector Key와 PropertyPath::keys 위치다(리플렉션 B9f3 —
// [perPlatform[Android]]). 진단 경로(FieldDiagnostic 등)에서는 Map 항목·Set 원소를 삽입 순번
// Index로 표기한다(리플렉션 B9f2 — Docs/Design/ReflectionExtensions.ko.md 1.6절, 키를 복사하지
// 않는다).
struct PathStep {
    Guid fieldId;
    uint32 index = 0;
    PathSelector selector = PathSelector::None;
};

// 속성 경로의 최대 단계 수다(Docs/Design/ReflectionFoundations.ko.md 7.1절): 최상위 필드 1 + struct
// 중첩 MaxStructDepth(16) — codec 위치 프레임 수와 같다. 원소 index·key는 단계를 늘리지 않는다.
inline constexpr uint32 MaxPropertyPathSteps = MaxStructDepth + 1;

// 객체 하나 안의 속성 경로다(7절 — FieldId 체인 + 원소 index·Map key). 참조를 따라 다른 객체로
// 들어가지 않는다(객체 지정은 handle이 맡는다). steps·keys(selector Key 단계가 index로 가리키는 키
// 값 — 리플렉션 B9f3)는 비소유 뷰이며 쓰는 동안 유효해야 한다. 문자열 경로
// ("slots[2].material"·"perPlatform[Android]")는 ParsePropertyPath(Object/PropertyPath.h)가 이
// 표현으로 바꾼다.
//
// 사용 예:
//   const PathStep steps[] = {{SlotsId, 2, PathSelector::Index}, {MaterialId}};
//   PropertyPath path{steps};
//   const PathStep entry[] = {{PerPlatformId, 0, PathSelector::Key}};
//   PropertyPath keyed{entry, keys};   // keys[0]은 Name 키 "Android"
struct PropertyPath {
    Span<const PathStep> steps;
    Span<const PathKey> keys{};
};

// 단계가 정규형인지다: selector None은 index 0, Key는 index < keyCount(그 경로의 keys 수)이고 그
// 밖의 selector 값은 정규형이 아니다. 경로 연산은 정규형이 아닌 단계를 InvalidArgument로 거부한다 —
// 같은 위치를 가리키는 경로가 한 표현만 가지므로 경로 동등성(SamePropertyPath)으로 Undo 기록을
// 병합할 수 있다. 진단 경로(keys 없음)는 keyCount 0으로 본다.
constexpr bool IsCanonicalPathStep(const PathStep& step, usize keyCount = 0) noexcept {
    switch (step.selector) {
    case PathSelector::None:
        return step.index == 0;
    case PathSelector::Index:
        return true;
    case PathSelector::Key:
        return step.index < keyCount;
    }

    return false;
}

namespace detail {

// Name 키 wire(u64 길이 + UTF-8 — 1.5절)의 표시 문자열이다. 형식이 맞지 않으면 false다.
inline bool PathKeyText(const PathKey& key, StringView& text) noexcept {
    constexpr usize prefix = sizeof(uint64);
    if (key.wire.Size() < prefix) {
        return false;
    }

    uint64 length = 0;
    for (usize index = 0; index < prefix; ++index) {
        length |= uint64{key.wire[index]} << (8 * index);
    }

    if (length != key.wire.Size() - prefix) {
        return false;
    }

    text = StringView(reinterpret_cast<const char*>(key.wire.Data() + prefix),
                      static_cast<usize>(length));
    return true;
}

// 두 키가 같은 Map 항목·Set 원소인지다(SamePropertyPath — 1.5절): 종류가 같고 wire byte가 같다.
// Name 키는 Core Name 규칙(NameTextEquals — ASCII 대소문자 무시·번호 접미)으로 비교한다(형식이
// 맞지 않는 wire는 byte로 비교한다).
inline bool SamePathKey(const PathKey& left, const PathKey& right) noexcept {
    if (left.kind != right.kind) {
        return false;
    }

    if (left.kind == ValueKind::Name) {
        StringView a;
        StringView b;
        if (PathKeyText(left, a) && PathKeyText(right, b)) {
            return NameTextEquals(a, b);
        }
    }

    if (left.wire.Size() != right.wire.Size()) {
        return false;
    }

    for (usize index = 0; index < left.wire.Size(); ++index) {
        if (left.wire[index] != right.wire[index]) {
            return false;
        }
    }

    return true;
}

} // namespace detail

// 두 경로가 같은 위치인지다(단계마다 필드 ID·선택자·index가 같고, Key 단계는 index 대신 그 키 값이
// 같다 — wire byte, Name 키는 Core Name 규칙으로 비교한다, 리플렉션 B9f3). 정규형 경로끼리의
// 비교다(keys 범위 밖 Key 단계는 같지 않다고 본다). keys 배열의 순서·쓰지 않는 칸은 비교하지
// 않는다.
//
// 사용 예(Undo 기록 병합):
//   if (SamePropertyPath(last.View(), next.View())) { /* 같은 위치의 연속 편집 */ }
inline bool SamePropertyPath(PropertyPath left, PropertyPath right) noexcept {
    if (left.steps.Size() != right.steps.Size()) {
        return false;
    }

    for (usize index = 0; index < left.steps.Size(); ++index) {
        const PathStep& a = left.steps[index];
        const PathStep& b = right.steps[index];
        if (!(a.fieldId == b.fieldId) || a.selector != b.selector) {
            return false;
        }

        if (a.selector != PathSelector::Key) {
            if (a.index != b.index) {
                return false;
            }

            continue;
        }

        if (a.index >= left.keys.Size() || b.index >= right.keys.Size() ||
            !detail::SamePathKey(left.keys[a.index], right.keys[b.index])) {
            return false;
        }
    }

    return true;
}

} // namespace rocks
