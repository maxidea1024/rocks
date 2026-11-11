#pragma once

#include <Base/Types.h>
#include <Core/Math/MathUtility.h>

// 수학 테스트 공용 도우미다. 재현 가능한 xorshift 난수와 상대·절대 오차 비교를 제공한다.
// 실패한 무작위 사례는 테스트 seed(CurrentSeed)와 고정 상수로 다시 만들 수 있다.
namespace rocks::test {

struct MathRandom {
    uint64 state;

    explicit MathRandom(uint64 seed) noexcept : state(seed == 0 ? 0x9e3779b97f4a7c15ull : seed) {
    }

    uint64 Next() noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    // [0, 1) 균등 double(53-bit).
    double Unit() noexcept {
        return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0);
    }

    // [minimum, maximum) 균등 double.
    double Range(double minimum, double maximum) noexcept {
        return minimum + (maximum - minimum) * Unit();
    }
};

// |a - b| <= absolute + relative * max(|a|, |b|). NaN은 항상 false다.
template <typename T>
bool Near(T a, T b, T absolute, T relative = static_cast<T>(0)) noexcept {
    const T scale = math::Max(math::Abs(a), math::Abs(b));
    return math::Abs(a - b) <= absolute + relative * scale;
}

} // namespace rocks::test
