#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

namespace rocks {

class NamePool;

// 실행 중 발급한 이름 토큰의 형태다. ID 값은 저장·네트워크 형식이 아니다.
enum class NameKind : uint8 {
    None,
    Normal,
    NoneDisplay
};

// 기본값·빈 문자열·"None"과 대소문자 변형은 모두 None이다. None_N은 번호 있는 이름이다.
// 일반 이름의 domain은 pool 세션을 구별한다. 표시를 보존하는 None도 Acquire 시 해당
// pool의 domain을 확인하지만 동등 비교는 domain과 표시 차이를 무시한다.
// ASCII 대소문자 비교와 UE5.6 번호 접미사만 지원한다. 비ASCII Unicode case folding과
// 정규화는 아직 지원하지 않는다. 비교는 발급·stale 여부를 검증하지 않는다.
struct Name {
    uint64 domain = 0;
    uint64 id = 0;
    uint64 displayId = 0;
    // 0은 번호 없음, 그 외에는 외부 접미사 + 1이다. 영속 ID로 사용하지 않는다.
    uint32 number = 0;
    NameKind kind = NameKind::None;

    // 발급 여부가 아니라 토큰의 구조만 검사한다. 실제 수명과 소유권은 Acquire로 확인한다.
    constexpr bool IsValid() const noexcept {
        if (number > 2147483647u) {
            return false;
        }
        if (kind == NameKind::None) {
            return domain == 0 && id == 0 && displayId == 0 && number == 0;
        }
        if (kind == NameKind::Normal) {
            return domain != 0 && id != 0 && displayId != 0;
        }
        if (kind == NameKind::NoneDisplay) {
            return domain != 0 && id == 0 && displayId != 0;
        }
        return false;
    }

    constexpr bool IsNone() const noexcept {
        return IsValid() && kind != NameKind::Normal && number == 0;
    }

    constexpr bool HasNumber() const noexcept {
        return number != 0;
    }

    // -1은 번호가 없다는 뜻이다. 숫자 0도 유효한 접미사다.
    constexpr int64 GetNumber() const noexcept {
        return number == 0 ? -1 : static_cast<int64>(number) - 1;
    }

    friend constexpr bool operator==(Name left, Name right) noexcept {
        const bool leftValid = left.IsValid();
        const bool rightValid = right.IsValid();
        if (!leftValid || !rightValid) {
            return left.domain == right.domain && left.id == right.id &&
                   left.displayId == right.displayId && left.number == right.number &&
                   left.kind == right.kind;
        }
        if (left.kind != NameKind::Normal || right.kind != NameKind::Normal) {
            return left.kind != NameKind::Normal && right.kind != NameKind::Normal &&
                   left.number == right.number;
        }
        return left.domain == right.domain && left.id == right.id && left.number == right.number;
    }
};

// 실행·pool과 무관한 이름의 비교 identity다. 저장 파일·네트워크 메시지에서 런타임 token(Name의
// domain·id는 실행마다 다르다) 대신 쓴다.
//
// 사용 예(저장):
//   auto key = TryMakeNameKey("Weapon_3");          // 또는 lease.Key()
//   uint8 bytes[NameKey::EncodedBytes];
//   key.Value().Encode(bytes);                      // NameKey::AlgorithmVersion도 파일 머리에 기록
//
// 규칙:
// - ASCII 대소문자만 다른 이름은 같은 key다("Weapon"과 "WEAPON"). 비ASCII는 byte 그대로 비교한다.
// - number는 Name::number와 같다: 0은 번호 없음, 그 밖은 외부 번호 + 1("Weapon_3"은 4).
// - None 규칙도 NamePool과 같다: 빈 문자열과 빈 base의 번호 이름("_5")은 None key {0, 0}, "None"의
//   대소문자 변형은 hash 0이다("None_3"은 {0, 4}).
// - hash는 고정 알고리즘 v1(AlgorithmVersion)이다: base byte를 ASCII 소문자로 바꿔 FNV-1a 64로
//   hash하고 splitmix64 마무리를 적용한다(결과 0은 1로 바꾼다). 버전·플랫폼 사이에 바뀌지 않는다.
//
// 주의(충돌):
// - 64-bit hash이므로 서로 다른 이름이 같은 key일 수 있고, 일부러 충돌시키는 것도 어렵지 않다.
// key만
//   으로 이름을 복원하거나 신뢰하지 않는다.
// - 영속 형식은 이름 원문 표를 함께 저장하고 읽을 때 원문으로 key를 다시 계산해 확인한다. 이름 표를
//   만들 때 서로 다른 원문이 같은 key이면 그 형식에서 오류로 처리한다(쓰는 쪽 책임).
// - 네트워크 상대가 보낸 key는 그 연결에서 원문으로 확인한 이름에만 대응시킨다.
struct NameKey {
    // 영속 형식은 이 버전을 함께 기록한다. hash 알고리즘이나 Name 비교 규칙(예: Unicode case
    // folding 도입)이 바뀌면 새 버전을 정의하고, 읽을 때 버전이 다르면 이름 원문으로 다시 계산한다.
    static constexpr uint32 AlgorithmVersion = 1;

    // EncodedBytes byte의 고정 byte 표현이다(hash 8 byte little-endian, number 4 byte
    // little-endian). 구조체를 그대로 memcpy하면 padding·endianness 때문에 플랫폼마다 달라지므로 이
    // 형식을 쓴다.
    static constexpr usize EncodedBytes = 12;

    uint64 hash = 0;
    uint32 number = 0;

    constexpr bool IsNone() const noexcept {
        return hash == 0 && number == 0;
    }

    constexpr void Encode(uint8 (&out)[EncodedBytes]) const noexcept {
        for (usize index = 0; index < 8; ++index) {
            out[index] = static_cast<uint8>(hash >> (index * 8));
        }

        for (usize index = 0; index < 4; ++index) {
            out[8 + index] = static_cast<uint8>(number >> (index * 8));
        }
    }

    static constexpr NameKey Decode(const uint8 (&in)[EncodedBytes]) noexcept {
        NameKey key;
        for (usize index = 0; index < 8; ++index) {
            key.hash |= static_cast<uint64>(in[index]) << (index * 8);
        }

        for (usize index = 0; index < 4; ++index) {
            key.number |= static_cast<uint32>(in[8 + index]) << (index * 8);
        }

        return key;
    }

    friend constexpr bool operator==(NameKey, NameKey) noexcept = default;
};

// text의 NameKey다. NamePool::TryIntern과 같은 번호 접미사·None 규칙으로 해석한다. 잘못된 UTF-8은
// InvalidArgument다. pool·allocator가 필요 없다.
Result<NameKey> TryMakeNameKey(StringView text) noexcept;

// immutable 문자열의 조회 수명을 붙드는 이동 전용 lease다. pool 객체 자체를 소유하지 않으므로
// pool/allocator는 모든 lease 반환과 진행 중 호출 종료까지 살아 있어야 한다.
// 서로 다른 lease의 반환은 병행 가능하지만 같은 lease의 이동/반환/조회 경합은 caller가 막는다.
class NameLease {
public:
    NameLease() noexcept = default;
    ~NameLease() noexcept;
    NameLease(const NameLease&) = delete;
    NameLease& operator=(const NameLease&) = delete;
    NameLease(NameLease&& other) noexcept;
    NameLease& operator=(NameLease&& other) noexcept;

    // 빈 lease의 Identifier()도 기본 None이다. 조회 성공은 먼저 operator bool()로 확인한다.
    explicit operator bool() const noexcept;
    Name Identifier() const noexcept;

    // 반환 view는 이 lease 또는 같은 entry의 다른 살아 있는 lease가 있는 동안만 보관한다.
    // embedded NUL을 허용한다. 길이는 byte이며 C 문자열 소비자의 첫 NUL 규칙과 다르다.
    StringView View() const noexcept;

    // 유효한 번호 접미사 `_N`을 제외한 원문 이름이다. view 수명은 View()와 같다.
    StringView BaseView() const noexcept;
    // 이 이름의 실행 독립 key다. 빈 lease는 None key다.
    NameKey Key() const noexcept;

    // 새 lease admission이다. Shutdown 이후 ShuttingDown, 계수 소진은 Overflow다.
    // 실패해도 원본 lease와 그 view는 유효하다. 빈 lease는 StaleHandle이다.
    Result<NameLease> TryClone() const noexcept;
    void Reset() noexcept;

private:
    friend class NamePool;
    NameLease(NamePool& pool, Name name, StringView view) noexcept;

    NamePool* pool_ = nullptr;
    Name name_{};
    StringView view_{};
};

} // namespace rocks
