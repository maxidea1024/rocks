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

// 해시 둘을 순서 있게 섞는다(실행 중 해시 표 분포용 — HashCombine(a, b)와 HashCombine(b, a)는
// 다르다). 복합 키의 해시를 구성 요소 해시로 만들 때 쓴다.
//
// 사용 예:
//   return HashCombine(Hash<Guid>{}(key.type), MixHash(key.index));
constexpr uint64 HashCombine(uint64 seed, uint64 value) noexcept {
    return MixHash(seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2)));
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

// ASCII 대문자를 소문자로 접은 byte에 돌린 64비트 FNV-1a다(섞지 않은 원값 — 해시 표에는 필요하면
// MixHash를 더 씌운다). ASCII가 아닌 byte는 그대로다. 대소문자만 다른 두 문자열은 값이 같으므로
// ASCII 대소문자 무시 같음과 짝이 맞는 해시다. Name pool의 bucket 해시(번호 접미를 뗀 base)와
// TypeRegistry의 타입 도구 이름 색인이 같이 쓴다. 실행 중 분포용이며 영속 식별자가 아니다.
//
// 사용 예:
//   const uint64 bucket = HashStringBytesIgnoringAsciiCase(name) & mask;
inline uint64 HashStringBytesIgnoringAsciiCase(StringView value) noexcept {
    uint64 hash = 14695981039346656037ull;
    for (usize index = 0; index < value.ByteCount(); ++index) {
        const uint8 byte = static_cast<uint8>(value.Data()[index]);
        hash ^= byte >= 'A' && byte <= 'Z' ? static_cast<uint8>(byte + ('a' - 'A')) : byte;
        hash *= 1099511628211ull;
    }

    return hash;
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
