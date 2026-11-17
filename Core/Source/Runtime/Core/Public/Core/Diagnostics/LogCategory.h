#pragma once

#include <Base/Types.h>
#include <Core/Hash.h>
#include <Core/Text/StringView.h>

namespace rocks {

// 카테고리 이름의 최대 구간 수다(`Graphics.D3D12`는 2구간). 점 접두사 해시 배열의 크기이기도 하다.
inline constexpr usize MaxLogCategoryDepth = 8;

// 카테고리 이름의 최대 byte 수다(끝 NUL 제외). 출력 접두사 버퍼를 고정 크기로 잡기 위한 상한이다.
inline constexpr usize MaxLogCategoryNameBytes = 63;

namespace detail {

// LogCategory의 consteval 생성자가 이름 문법 위반을 컴파일 오류로 알리는 표지 함수다. constexpr이
// 아니므로 상수 평가 중에 부르면 컴파일이 실패하고, 컴파일러 진단에 이 함수 이름이 그대로 나온다.
// 런타임에는 부를 일이 없다(생성자가 consteval이라 런타임 경로가 없다).
inline void LogCategoryError_EmptyName() noexcept {
}

inline void LogCategoryError_NotNullTerminated() noexcept {
}

inline void LogCategoryError_EmptySegment() noexcept {
}

inline void LogCategoryError_InvalidCharacter() noexcept {
}

inline void LogCategoryError_TooManySegments() noexcept {
}

constexpr bool IsLogCategoryNameCharacter(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9');
}

} // namespace detail

// 로그 카테고리다. 상태가 없는 constexpr 값이며 이름과, consteval로 미리 계산한 점 접두사별 64-bit
// 해시 배열을 가진다(`Graphics.D3D12`이면 `Graphics`와 `Graphics.D3D12`의 해시 두 개). 전역 생성자·
// 가변 상태·런타임 레벨이 없다. 런타임 레벨 재정의는 기록 경로(후속 LogSystem)의 표가 갖는다.
//
// 이름 문법: `[A-Za-z0-9]+` 구간을 점 하나로 잇는다. 빈 구간(앞·뒤·연속 점)은 없다. 최대
// MaxLogCategoryNameBytes byte, MaxLogCategoryDepth 구간이다. 표기는 PascalCase다. 문법 위반은
// 컴파일 오류이며 진단에 detail::LogCategoryError_* 함수 이름이 나온다.
//
// 같은 이름을 여러 곳에서 정의하면 같은 카테고리다. 출력에는 이름이 나가므로 해시가 같아도 출력에서
// 카테고리가 섞이지 않는다. 해시는 HashStringBytes(실행 중 분포용)이며 영속 식별자가 아니다.
//
// 수명: 이름은 생성에 쓴 문자열 리터럴(정적 저장소)을 가리킨다. 보통 모듈 헤더에
// `inline constexpr`로 두므로 프로그램 수명 동안 유효하다.
//
// 사용 예:
//   inline constexpr LogCategory LogEngine{"Engine"};
//   inline constexpr LogCategory LogGraphicsD3D12{"Graphics.D3D12"};
//   logger.Write(LogLevel::Info, LogGraphicsD3D12, "Device created");
class LogCategory {
public:
    // 문자열 리터럴에서 만든다. 문법을 검사하고 접두사 해시를 계산한다. consteval이라 상수
    // 평가에서만 만들 수 있다(런타임 문자열로는 만들 수 없다).
    template <usize Count>
    consteval LogCategory(const char (&name)[Count]) noexcept : name_(name) {
        static_assert(Count - 1 <= MaxLogCategoryNameBytes,
                      "Log category name exceeds MaxLogCategoryNameBytes");
        if (name[Count - 1] != '\0') {
            detail::LogCategoryError_NotNullTerminated();
        }

        const usize length = Count - 1;
        if (length == 0) {
            detail::LogCategoryError_EmptyName();
        }

        usize segmentStart = 0;
        for (usize index = 0; index <= length; ++index) {
            if (index < length && name[index] != '.') {
                if (!detail::IsLogCategoryNameCharacter(name[index])) {
                    detail::LogCategoryError_InvalidCharacter();
                }

                continue;
            }

            if (index == segmentStart) {
                detail::LogCategoryError_EmptySegment();
            }

            if (prefixCount_ == MaxLogCategoryDepth) {
                detail::LogCategoryError_TooManySegments();
            }

            prefixBytes_[prefixCount_] = static_cast<uint8>(index);
            prefixHashes_[prefixCount_] = HashStringBytes(StringView(name, index));
            ++prefixCount_;
            segmentStart = index + 1;
        }
    }

    // 전체 이름이다. 끝 NUL을 포함하지 않으며 정적 저장소를 가리킨다.
    constexpr StringView Name() const noexcept {
        return StringView(name_, prefixBytes_[prefixCount_ - 1]);
    }

    // 점 접두사(= 구간) 수다. 1 이상 MaxLogCategoryDepth 이하다.
    constexpr usize PrefixCount() const noexcept {
        return prefixCount_;
    }

    // index번째 점 접두사의 이름이다. 0은 첫 구간, PrefixCount() - 1은 전체 이름이다.
    // 범위 밖 index는 빈 view다.
    constexpr StringView PrefixName(usize index) const noexcept {
        return index < prefixCount_ ? StringView(name_, prefixBytes_[index]) : StringView();
    }

    // index번째 점 접두사의 해시(HashStringBytes(PrefixName(index)))다. 범위 밖 index는 0이다.
    // 해시가 같다고 이름이 같은 것은 아니다. 정확한 판정이 필요하면 이름을 비교한다.
    constexpr uint64 PrefixHash(usize index) const noexcept {
        return index < prefixCount_ ? prefixHashes_[index] : 0;
    }

    // 전체 이름의 해시다(PrefixHash(PrefixCount() - 1)).
    constexpr uint64 NameHash() const noexcept {
        return prefixHashes_[prefixCount_ - 1];
    }

private:
    const char* name_ = nullptr;
    uint64 prefixHashes_[MaxLogCategoryDepth]{};
    uint8 prefixBytes_[MaxLogCategoryDepth]{};
    uint8 prefixCount_ = 0;
};

static_assert(MaxLogCategoryNameBytes <= NumericLimits<uint8>::Max(),
              "prefix byte counts are stored as uint8");
static_assert(MaxLogCategoryDepth <= NumericLimits<uint8>::Max(),
              "prefix counts are stored as uint8");

// 카테고리 필터에 쓰는 점 접두사다. 카테고리의 이름이 이 접두사와 같거나 `접두사.`로 시작하면
// 일치한다(`Graphics`는 `Graphics`·`Graphics.D3D12`와 일치하고 `GraphicsX`와는 일치하지 않는다).
// 빈 값(depth 0)은 모든 카테고리와 일치한다.
//
// 판정: 먼저 같은 깊이의 접두사 해시를 비교하고, 같으면 이름 byte를 비교해 확정한다. 해시가 우연히
// 같아도 이름이 다르면 일치하지 않는다.
//
// 보통 LogCategory에서 암묵 변환으로 만든다. 칸을 직접 채울 수도 있지만 hash가 name의
// HashStringBytes와 다르면 어떤 카테고리와도 일치하지 않는다.
//
// 수명: name은 원본 문자열을 빌린다. LogCategory에서 만들면 정적 저장소이므로 제약이 없다.
//
// 사용 예:
//   inline constexpr LogCategory LogGraphics{"Graphics"};
//   logger.Add(sink, {LogLevel::Warning, LogGraphics}); // Graphics.* 의 Warning 이상
struct LogCategoryPrefix {
    StringView name;
    uint64 hash = 0;
    usize depth = 0;

    constexpr LogCategoryPrefix() noexcept = default;

    // 카테고리 전체 이름을 접두사로 쓴다.
    constexpr LogCategoryPrefix(const LogCategory& category) noexcept
        : name(category.Name()), hash(category.NameHash()), depth(category.PrefixCount()) {
    }

    // 모든 카테고리와 일치하는 빈 접두사인지다.
    constexpr bool MatchesAll() const noexcept {
        return depth == 0;
    }

    // category가 이 접두사 아래에 있는지다.
    constexpr bool Matches(const LogCategory& category) const noexcept {
        if (depth == 0) {
            return true;
        }

        if (depth > category.PrefixCount() || category.PrefixHash(depth - 1) != hash) {
            return false;
        }

        return category.PrefixName(depth - 1) == name;
    }
};

} // namespace rocks
