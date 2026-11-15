#pragma once

#include <Base/Endian.h>
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

// ---- seed를 넣은 해시(hash flooding 방어) ----
// 위 Hash<T>는 seed가 없어 입력을 고를 수 있는 상대가 충돌을 미리 계산할 수 있다(MixHash는 공개
// 전단사, FNV-1a는 64비트 전체 충돌도 만들 수 있다). 파일·네트워크에서 온 키로 해시 표를 채우는
// 곳은 아래 SeededHash<T>를 쓴다. seed가 해시 계산 안에 들어가므로(정책 해시 뒤에 섞는 것이
// 아니다) seed를 모르는 상대는 색인 칸 충돌을 고를 수 없다. 순서가 해시와 무관한 컨테이너
// (OrderedMap·OrderedSet)의 기본 정책이다. HashMap·HashSet은 순회 순서가 해시에 달려 있어 기본
// 정책을 바꾸지 않았다(충돌 공격을 보장하지 않는다 — Docs/Implementation/HashCollections.ko.md).

// SipHash의 128비트 비밀 key다.
struct HashSeed {
    uint64 k0 = 0;
    uint64 k1 = 0;
};

namespace detail {

// SipHash-2-4의 상태다. SipHash24·SipHash24Words가 같이 쓴다(공개 API가 아니다).
class SipHash24State {
public:
    explicit SipHash24State(HashSeed seed) noexcept
        : v0_(seed.k0 ^ 0x736f6d6570736575ull), v1_(seed.k1 ^ 0x646f72616e646f6dull),
          v2_(seed.k0 ^ 0x6c7967656e657261ull), v3_(seed.k1 ^ 0x7465646279746573ull) {
    }

    // 메시지 word 하나(little-endian 8 byte)를 압축한다(2 라운드).
    void Absorb(uint64 word) noexcept {
        v3_ ^= word;
        Round();
        Round();
        v0_ ^= word;
    }

    // 남은 byte(little-endian, 8 byte 미만)와 전체 길이로 마지막 word를 만들고 마무리한다(4
    // 라운드).
    uint64 Finish(usize bytes, uint64 tail) noexcept {
        Absorb((static_cast<uint64>(bytes & 0xff) << 56) | tail);
        v2_ ^= 0xff;
        Round();
        Round();
        Round();
        Round();
        return v0_ ^ v1_ ^ v2_ ^ v3_;
    }

private:
    static uint64 Rotate(uint64 value, uint32 bits) noexcept {
        return (value << bits) | (value >> (64 - bits));
    }

    void Round() noexcept {
        v0_ += v1_;
        v1_ = Rotate(v1_, 13);
        v1_ ^= v0_;
        v0_ = Rotate(v0_, 32);
        v2_ += v3_;
        v3_ = Rotate(v3_, 16);
        v3_ ^= v2_;
        v0_ += v3_;
        v3_ = Rotate(v3_, 21);
        v3_ ^= v0_;
        v2_ += v1_;
        v1_ = Rotate(v1_, 17);
        v1_ ^= v2_;
        v2_ = Rotate(v2_, 32);
    }

    uint64 v0_;
    uint64 v1_;
    uint64 v2_;
    uint64 v3_;
};

} // namespace detail

// SipHash-2-4(Aumasson·Bernstein 2012)다. key를 모르면 출력을 예측할 수 없는 키 있는 해시(PRF)로,
// 해시 표의 hash flooding 방어용이다. 실행 중 분포용이며 영속 식별자·MAC이 아니다(메시지 인증은
// Crypto 모듈의 MAC — BLAKE2b keyed, Docs/Design/Crypto.ko.md 4절). bytes가 0이면 data는
// nullptr여도 된다. 정렬되지 않은 주소도 된다.
//
// 사용 예:
//   const uint64 hash = SipHash24(seed, text.Data(), text.ByteCount());
inline uint64 SipHash24(HashSeed seed, const void* data, usize bytes) noexcept {
    detail::SipHash24State state(seed);
    const uint8* input = static_cast<const uint8*>(data);
    const usize whole = bytes - bytes % sizeof(uint64);
    for (usize offset = 0; offset < whole; offset += sizeof(uint64)) {
        state.Absorb(LoadLittleEndian<uint64>(input + offset));
    }

    uint64 tail = 0;
    for (usize index = 0; index < bytes % sizeof(uint64); ++index) {
        tail |= static_cast<uint64>(input[whole + index]) << (index * 8);
    }

    return state.Finish(bytes, tail);
}

// 길이가 정해진 키(정수·enum·Guid·Name)용 word 경로다. words[0..count)를 little-endian으로 이어
// 붙인 count * 8 byte에 대한 SipHash24와 결과가 같고, byte 모으기·꼬리 처리를 건너뛴다.
//
// 사용 예:
//   const uint64 words[2] = {guid.high, guid.low};
//   const uint64 hash = SipHash24Words(seed, words, 2);
inline uint64 SipHash24Words(HashSeed seed, const uint64* words, usize count) noexcept {
    detail::SipHash24State state(seed);
    for (usize index = 0; index < count; ++index) {
        state.Absorb(words[index]);
    }

    return state.Finish(count * sizeof(uint64), 0);
}

// 이 프로세스의 해시 seed다. 처음 부를 때 OS 난수(platform::TryFillSecureRandom)로 한 번 정하고
// 그 뒤로는 같은 값이다(thread-safe). OS 난수를 읽지 못하면 Fatal이다(Windows BCryptGenRandom
// 실패 — macOS·Android는 실패를 돌려주지 않는다). 재현이 필요한 시험은 SeededHash에 seed를 직접
// 넘긴다. fork한 자식 프로세스는 같은 seed를 물려받는다.
HashSeed ProcessHashSeed() noexcept;

// SeededHash 특수화가 같이 쓰는 seed 보관과 생성자다. 기본 생성은 ProcessHashSeed()이고, 시험·
// 결정적 재현은 seed를 넘긴다. 같은 컨테이너 안에서는 seed가 바뀌면 안 되므로 생성 뒤에는 읽기만
// 된다(정책 계약).
class SeededHashBase {
public:
    SeededHashBase() noexcept : seed_(ProcessHashSeed()) {
    }

    explicit SeededHashBase(HashSeed injected) noexcept : seed_(injected) {
    }

    HashSeed Seed() const noexcept {
        return seed_;
    }

private:
    HashSeed seed_;
};

// seed를 넣은 키 해시다. 기본은 정수·enum(값을 64비트로 넓혀 SipHash)이고 String·StringView·
// Name·Guid는 특수화가 있다. 다른 키 타입은 특수화를 더한다. 동등한 키는 같은 seed에서 같은 값이다.
//
// 사용 예:
//   OrderedMap<String, int32> map(allocator);                                  // 프로세스 seed
//   OrderedMap<int32, int32> fixed(allocator, SeededHash<int32>(HashSeed{1, 2}));  // 시험용 고정
template <typename T>
struct SeededHash : SeededHashBase {
    using SeededHashBase::SeededHashBase;

    SeededHash() noexcept = default;

    uint64 operator()(const T& value) const noexcept {
        static_assert(__is_integral(T) || __is_enum(T),
                      "Provide a SeededHash specialization for this key type");
        const uint64 widened = static_cast<uint64>(value);
        return SipHash24Words(Seed(), &widened, 1);
    }
};

template <>
struct SeededHash<StringView> : SeededHashBase {
    using SeededHashBase::SeededHashBase;

    SeededHash() noexcept = default;

    uint64 operator()(StringView value) const noexcept {
        return SipHash24(Seed(), value.Data(), value.ByteCount());
    }
};

// String 키는 StringView로 할당 없이 조회할 수 있다(Equal<String>과 짝).
template <>
struct SeededHash<String> : SeededHashBase {
    using SeededHashBase::SeededHashBase;

    SeededHash() noexcept = default;

    uint64 operator()(const String& value) const noexcept {
        return SipHash24(Seed(), value.View().Data(), value.View().ByteCount());
    }

    uint64 operator()(StringView value) const noexcept {
        return SipHash24(Seed(), value.Data(), value.ByteCount());
    }
};

// Hash<Name>과 같은 필드를 쓴다(같은 Name이면 같은 값 — 대소문자 표시 변형은 같은 비교 id다).
template <>
struct SeededHash<Name> : SeededHashBase {
    using SeededHashBase::SeededHashBase;

    SeededHash() noexcept = default;

    uint64 operator()(Name value) const noexcept {
        uint64 words[5] = {};
        usize count = 0;
        if (value.IsValid() && value.kind != NameKind::Normal) {
            words[count++] = value.number;
        } else if (!value.IsValid()) {
            words[count++] = value.domain;
            words[count++] = value.id;
            words[count++] = value.displayId;
            words[count++] = value.number;
            words[count++] = static_cast<uint8>(value.kind);
        } else {
            words[count++] = value.domain;
            words[count++] = value.id;
            words[count++] = value.number;
        }

        return SipHash24Words(Seed(), words, count);
    }
};

template <>
struct SeededHash<Guid> : SeededHashBase {
    using SeededHashBase::SeededHashBase;

    SeededHash() noexcept = default;

    uint64 operator()(Guid value) const noexcept {
        const uint64 words[2] = {value.high, value.low};
        return SipHash24Words(Seed(), words, 2);
    }
};

} // namespace rocks
