#pragma once

#include <Core/Guid.h>

namespace rocks {

// 에셋 식별자다(Docs/Design/AssetSystem.ko.md 4.1–4.2절). 128-bit Guid이며 0은 null(에셋 없음)이다.
// 문자열 표기는 RFC 9562 36자 소문자 하이픈 형식("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx")이고 해석도
// 그 형식만 받는다 (경로·파일 이름에 쓰이므로 정규 표기 하나만 둔다).
//
// 사용 예:
//   auto created = AssetId::TryNew();   // 새 에셋 ID(무작위 UUID v4)
//   auto text = TryFormat(allocator, "{}", created.Value());
//   // text: "3f2a9c1e-7b4d-4e8f-9a01-2c3d4e5f6a7b"
//   auto parsed = AssetId::TryParse(text.Value().View());
//
// 리플렉션 필드(ValueKind::AssetId)로 쓸 수 있고 저장 wire는 Guid다.
struct AssetId {
    Guid value{};

    // 표준 문자열의 길이다.
    static constexpr usize TextBytes = 36;

    constexpr bool IsNull() const noexcept {
        return value == Guid{};
    }

    friend constexpr bool operator==(const AssetId&, const AssetId&) noexcept = default;

    // 새 에셋 ID를 발급한다(OS 보안 난수 — 실패하면 Unsupported).
    [[nodiscard]] static Result<AssetId> TryNew() noexcept {
        auto created = TryNewGuid();
        if (!created) {
            return Result<AssetId>::Failure(created.Error());
        }

        return Result<AssetId>::Success(AssetId{created.Value()});
    }

    // 36자 소문자 하이픈 형식만 받는다(대문자·32자·중괄호는 InvalidArgument). null 표기도 받는다.
    [[nodiscard]] static Result<AssetId> TryParse(StringView text) noexcept {
        if (text.ByteCount() != TextBytes) {
            return Result<AssetId>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize index = 0; index < text.ByteCount(); ++index) {
            const char c = text.Data()[index];
            if (c >= 'A' && c <= 'F') {
                return Result<AssetId>::Failure(ErrorCode::InvalidArgument);
            }
        }

        auto parsed = TryParseGuid(text);
        if (!parsed) {
            return Result<AssetId>::Failure(parsed.Error());
        }

        return Result<AssetId>::Success(AssetId{parsed.Value()});
    }
};

// "{}"는 36자 소문자 표준 표기다.
template <>
struct Formatter<AssetId> {
    static Result<void> Format(StringBuilderBase& out, const AssetId& id,
                               StringView spec) noexcept {
        return Formatter<Guid>::Format(out, id.value, spec);
    }
};

} // namespace rocks
