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
// ASCII 대소문자 비교와 UE5.8 번호 접미사만 지원한다. 비ASCII Unicode case folding과
// 정규화는 아직 지원하지 않는다. 비교는 발급·stale 여부를 검증하지 않는다.
struct Name {
    // GetNumber가 번호 없는 이름에 돌려주는 "번호 없음" 표지다(UE NAME_NO_NUMBER = -1 대응). 유효한
    // 외부 번호는 0 이상이므로 음수 하나를 표지로 쓴다. NameTextParts::GetNumber도 같은 값을 쓴다.
    static constexpr int64 NoNumber = -1;

    uint64 domain = 0;
    uint64 id = 0;
    uint64 displayId = 0;
    // 0은 번호 없음, 그 외에는 외부 접미사 + 1이다. 영속 ID로 사용하지 않는다.
    uint32 number = 0;
    NameKind kind = NameKind::None;

    // 발급 여부가 아니라 토큰의 구조만 검사한다. 실제 수명과 소유권은 Acquire로 확인한다.
    constexpr bool IsValid() const noexcept {
        if (number > static_cast<uint32>(NumericLimits<int32>::Max())) {
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

    // 외부 번호다. 번호가 없으면 NoNumber다. 숫자 0도 유효한 접미사다.
    constexpr int64 GetNumber() const noexcept {
        return number == 0 ? NoNumber : static_cast<int64>(number) - 1;
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

// Name 원문을 base와 번호 접미사로 나눈 결과다. SplitNameText가 만든다.
struct NameTextParts {
    // 번호 접미사 `_N`을 뺀 앞부분이다. 입력 view의 앞부분을 가리키므로 입력 storage가 살아 있는
    // 동안만 쓴다. 번호가 없으면 입력 전체다.
    StringView base;
    // Name::number와 같은 표현이다: 0은 번호 없음, 그 밖은 외부 번호 + 1("Socket_7"은 8).
    uint32 number = 0;

    constexpr bool HasNumber() const noexcept {
        return number != 0;
    }

    // 외부 번호다(Name::GetNumber와 같다). 번호가 없으면 Name::NoNumber이고 0도 유효한 번호다.
    constexpr int64 GetNumber() const noexcept {
        return number == 0 ? Name::NoNumber : static_cast<int64>(number) - 1;
    }
};

// text를 NamePool::TryIntern·TryMakeNameKey와 같은 규칙(UE 5.8 ParseNumberFromName)으로 base와
// 번호로 나눈다. 표시 문자열의 base나 번호를 따로 다룰 때 쓴다.
//
// 번호 접미사 규칙(하나라도 어기면 입력 전체가 base이고 번호가 없다):
// - 끝의 ASCII 10진 숫자 1–10자리 바로 앞이 '_'다("Socket_7"은 base "Socket", 번호 7).
// - 두 자리 이상이면 첫 숫자가 '0'이 아니다("Socket_0"은 번호 0, "Socket_07"은 번호 없음).
// - 값이 int32 최댓값(2,147,483,647) 미만이다(번호는 0–2,147,483,646).
// - 숫자 앞의 '_' 하나만 뗀다("Socket__7"은 base "Socket_", "a_1_2"는 base "a_1"·번호 2).
//
// 분해는 문법만 본다. "_7"처럼 base가 비면 base는 빈 문자열이고 번호는 7이지만, Name으로는 빈
// base의 번호 이름이 기본 None이다(TryIntern·NameTextEquals). UTF-8 유효성은 검사하지 않는다 —
// '_'와 숫자는 ASCII이고 UTF-8 다중 byte 문자에는 0x80 미만 byte가 없으므로 비ASCII 문자 중간에서
// 자르지 않는다. embedded NUL도 base의 일부다. 할당하지 않으며 pool·allocator가 필요 없다.
//
// 사용 예:
//   NameTextParts parts = SplitNameText("Socket_7");
//   // parts.base == "Socket", parts.GetNumber() == 7
ROCKS_CORE_API NameTextParts SplitNameText(StringView text) noexcept;

// 두 Name 원문이 같은 Name인지 pool·intern 없이 판정한다. 같은 pool에 두 원문을 TryIntern한 결과의
// operator==와 같은 답이고, true이면 TryMakeNameKey의 key도 같다(유효 UTF-8 입력). Map<Name>
// 키·Set<Name> 원소의 중복 판정, 경로 key 비교처럼 토큰 없이 원문끼리 비교할 때 쓴다. hash가
// 필요하면 같은 규칙의 TryMakeNameKey를 쓴다.
//
// 규칙(NamePool 비교 규칙과 같다):
// - 번호는 SplitNameText로 나누고 같아야 한다("Socket_7" != "Socket_07" != "Socket").
// - base는 ASCII 문자만 대소문자를 무시하고 비ASCII는 byte로 비교한다("Socket" == "SOCKET",
//   "Ünit" == "ÜNIT", "Ünit" != "ünit"). Unicode case folding·정규화는 하지 않는다(NFC 여부는
//   호출자가 미리 검사한다).
// - None: 빈 문자열, 빈 base의 번호 이름("_7"), 번호 없는 "None"(대소문자 무시)은 모두 같은
//   None이다. "None_3"은 번호 있는 이름이라 "none_3"과 같고 None·"None_4"와 다르다.
//
// 표시 byte가 같은지(리플렉션 차이 저장의 같음)와는 다르다 — 그것은 StringView의 ==로 본다.
// 잘못된 UTF-8은 검사하지 않고 위 byte 규칙대로 비교한다. TryIntern은 그런 입력을 거부하므로 Name
// 의미가 필요한 호출자는 먼저 ValidateUtf8로 거른다. 할당하지 않고 입력만 읽으므로 어느
// 스레드에서도 쓸 수 있다.
//
// 사용 예:
//   NameTextEquals("Socket_7", "SOCKET_7");  // true
//   NameTextEquals("Socket_7", "Socket_07"); // false
//   NameTextEquals("none", "");              // true(둘 다 None)
ROCKS_CORE_API bool NameTextEquals(StringView left, StringView right) noexcept;

// immutable 문자열의 조회 수명을 붙드는 이동 전용 lease다. pool 객체 자체를 소유하지 않으므로
// pool/allocator는 모든 lease 반환과 진행 중 호출 종료까지 살아 있어야 한다.
// 서로 다른 lease의 반환은 병행 가능하지만 같은 lease의 이동/반환/조회 경합은 caller가 막는다.
class ROCKS_CORE_API NameLease {
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
