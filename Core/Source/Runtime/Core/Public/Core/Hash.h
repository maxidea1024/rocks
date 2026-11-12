#pragma once

#include <Base/Guid.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>

namespace rocks {

// 실행 중 해시 테이블의 분포용이다. 파일·네트워크 식별자나 암호학적 해시가 아니다.
constexpr uint64 MixHash(uint64 value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

template <typename T>
struct Hash {
    uint64 operator()(const T& value) const noexcept {
        static_assert(__is_integral(T) || __is_enum(T),
                      "Provide a custom hasher for this key type");
        return MixHash(static_cast<uint64>(value));
    }
};

template <typename T>
struct Equal {
    bool operator()(const T& left, const T& right) const noexcept(noexcept(left == right)) {
        return left == right;
    }
};

inline uint64 HashStringBytes(StringView value) noexcept {
    uint64 hash = 14695981039346656037ull;
    for (usize index = 0; index < value.ByteCount(); ++index) {
        hash ^= static_cast<uint8>(value.Data()[index]);
        hash *= 1099511628211ull;
    }

    return MixHash(hash);
}

template <>
struct Hash<StringView> {
    uint64 operator()(StringView value) const noexcept {
        return HashStringBytes(value);
    }
};

template <>
struct Hash<String> {
    uint64 operator()(const String& value) const noexcept {
        return HashStringBytes(value.View());
    }

    uint64 operator()(StringView value) const noexcept {
        return HashStringBytes(value);
    }
};

template <>
struct Equal<String> {
    bool operator()(const String& left, const String& right) const noexcept {
        return left.View() == right.View();
    }

    bool operator()(const String& left, StringView right) const noexcept {
        return left.View() == right;
    }
};

template <>
struct Hash<Name> {
    uint64 operator()(Name value) const noexcept {
        if (value.IsValid() && value.kind != NameKind::Normal) {
            return MixHash(value.number);
        }
        if (!value.IsValid()) {
            return MixHash(value.domain ^ MixHash(value.id) ^ MixHash(value.displayId) ^
                           MixHash(value.number) ^ MixHash(static_cast<uint8>(value.kind)));
        }
        return MixHash(value.domain ^ MixHash(value.id) ^ MixHash(value.number));
    }
};

template <>
struct Hash<Guid> {
    uint64 operator()(Guid value) const noexcept {
        return MixHash(value.high ^ MixHash(value.low));
    }
};

} // namespace rocks
