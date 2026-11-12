#pragma once

#include <Base/Types.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

// 재현 가능한 의사 난수 stream이다. 같은 (seed, stream)에서 시작하면 모든 지원 플랫폼과 빌드
// 구성에서 같은 순서의 값을 만든다. 절차적 생성, replay, 결정론적 시뮬레이션, 테스트 입력처럼
// "같은 입력이면 같은 결과"가 필요한 곳에 사용한다.
//
// 사용 예:
//   RandomStream random(levelSeed);                 // 기본 stream
//   RandomStream loot(levelSeed, 2);                // 같은 seed, 다른 계통(stream)
//   const int32 damage = random.NextInt32InRange(10, 20);  // 10 이상 20 이하
//   const float angle = random.NextFloatInRange(0.0f, 360.0f);  // 0 이상 360 미만
//
// 알고리즘: PCG32 (M. E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically
// Good Algorithms for Random Number Generation", 2014)의 XSH RR 출력 함수를 사용한다.
// - 내부 상태는 64bit LCG다. state' = state * 6364136223846793005 + increment (mod 2^64).
// - increment = (stream << 1) | 1이다. 항상 홀수이므로 모든 stream의 주기가 2^64다.
// - 출력은 갱신 전 상태에서 계산한다. 상위 bit를 xorshift로 접어 32bit를 고르고,
//   최상위 5bit 값만큼 오른쪽으로 회전한다. 품질이 낮은 LCG 하위 bit는 출력에 쓰지 않는다.
// - 초기화 절차는 공개 참조 구현(pcg-c-basic의 srandom)과 같다. 따라서 seed 42, stream 54의
//   출력은 참조 데모 출력과 일치하며 테스트가 이를 고정한다.
//
// 계약과 주의점:
// - 스레드: 인스턴스 하나를 여러 스레드가 동시에 사용하면 data race다. 잠금이 없으므로 스레드나
//   작업마다 별도 인스턴스를 만들고, 서로 다른 stream 또는 seed를 배정한다.
// - 보안: 암호학적 난수가 아니다. 연속 출력 몇 개로 내부 상태를 복원·예측할 수 있으므로 토큰,
//   키, 세션 식별자, 금전 가치가 걸린 추첨, 치트 방지 비밀값에 사용하지 않는다.
// - 플랫폼 동일성: 정수 API는 64bit 정수 연산만 쓰므로 bit 단위로 같다. NextFloat01과
//   NextDouble01의 변환은 반올림이 없는 정확한 변환이라 역시 bit 단위로 같다. NextFloatInRange는
//   IEEE-754 기본 환경(최근접 짝수 반올림, FTZ/DAZ 꺼짐)을 전제한다. 곱과 합을 별도 문장으로
//   나눠 기본 -ffp-contract=on에서 FMA 축약이 일어나지 않게 한다. fast-math, fp-contract=fast,
//   FTZ/DAZ 환경에서는 플랫폼 간 동일성을 보장하지 않는다.
// - 호환성: 출력 순서는 저장된 replay·절차적 콘텐츠와 연결되는 외부 계약이다. 알고리즘, 초기화,
//   값 변환, 함수별 소비 횟수를 바꾸면 기존 결과가 달라지므로 golden 테스트를 함께 갱신하고
//   변경 사실을 알린다.
// - 소비 횟수: 각 함수가 내부 32bit 출력을 몇 번 소비하는지 아래 선언에 적는다. 분기마다 호출
//   횟수가 다르면 이후 순서도 달라지므로 replay가 필요한 코드는 호출 순서를 고정한다.
// - stream 독립성: 서로 다른 stream은 다른 순서를 만들지만, LCG 기반이므로 stream 사이의 완전한
//   통계적 독립성까지 보장하지는 않는다. 독립성이 중요한 통계 실험에는 seed도 함께 바꾼다.
// - 복사: 값 복사는 현재 위치까지 그대로 복제한다. 미리보기·rollback에는 유용하지만, 의도치 않게
//   복사한 두 사본은 같은 값을 만들어 상관된 결과가 된다.
//
// UE 5.6 FRandomStream과의 의미 차이(코드를 공유하지 않으며 의미만 비교한다):
// - 생성기: UE는 32bit LCG(Seed = Seed * 196314165 + 907633515)로 주기가 2^32이고, 자체 주석에서
//   하위 bit 품질이 낮다고 경고한다. Rocks는 64bit 상태 PCG32로 stream마다 주기 2^64이며
//   2^63개의 stream을 고를 수 있다.
// - seed: UE는 int32 seed 하나와 FName 해시 초기화를 제공하며 NAME_None이면 현재 시각으로
//   초기화해 재현성이 사라진다. 기본 생성자는 seed 0이다. Rocks는 uint64 seed와 stream을 명시적으로
//   받고 시각 기반 초기화와 기본 생성자를 제공하지 않는다.
// - 스레드: UE는 함수를 const로 두고 mutable seed를 갱신한다. "thread-safe" 주석과 달리 같은
//   인스턴스의 동시 호출은 경쟁이다. Rocks는 상태를 바꾸는 함수를 non-const로 두고 스레드 안전성을
//   주장하지 않는다.
// - [0,1) 실수: UE GetFraction/FRand는 상태 상위 23bit를 가수에 넣어 2^-23 간격 값을 만든다.
//   Rocks NextFloat01은 24bit로 2^-24 간격이며 NextDouble01(53bit)을 별도로 제공한다.
// - 정수 범위: UE RandRange(Min, Max)도 양 끝을 포함하지만 RandHelper가
//   Trunc(GetFraction() * float(Range))로 계산해 편향이 있고, GetFraction 값이 2^23가지뿐이라
//   Range가 2^23을 넘으면 나올 수 없는 값이 생긴다. Max - Min + 1을 int32로 계산하므로 전체 int32
//   범위에서는 overflow하고, Range가 0 이하이면 경고 없이 Min을 반환한다. Rocks NextInt32InRange는
//   거절 표본으로 편향이 없고, [INT32_MIN, INT32_MAX] 전체 범위를 지원하며, minimum > maximum은
//   Fatal이다.
// - 실수 범위: UE FRandRange는 InMin + (InMax - InMin) * FRand()라서 반올림으로 InMax가 나올 수
//   있고 입력 검사가 없다. Rocks NextFloatInRange는 maximum을 반환하지 않으며 유한하지 않은 입력과
//   minimum >= maximum은 Fatal이다.
// - 범위 밖 기능: UE의 단위 벡터, 원뿔, 상자 내부 점 함수는 여기서 제공하지 않는다. 벡터 타입
//   위에서 이 stream을 사용하는 별도 함수로 둔다.

// GetState()가 반환하고 RestoreState()가 받는 전체 상태다. 이 값만 저장하면 나중에 같은 위치부터
// 같은 순서를 이어서 만들 수 있다. 모든 bit 조합이 유효한 상태다.
struct RandomStreamState {
    // Reset()이 되돌아갈 초기 seed다.
    uint64 seed = 0;
    // stream 선택값이다. GetState()는 실제로 쓰이는 하위 63bit만 담아 반환한다.
    uint64 stream = 0;
    // 다음 출력을 계산할 64bit LCG 상태다.
    uint64 state = 0;

    friend constexpr bool operator==(const RandomStreamState&,
                                     const RandomStreamState&) noexcept = default;
};

// 소유 자원이 없는 24 byte 값 타입이다. 복사·이동·파괴에 부수 효과가 없다.
class RandomStream {
public:
    // stream을 생략했을 때 쓰는 값이다. increment가 Knuth MMIX LCG와 pcg-cpp 기본 increment인
    // 1442695040888963407이 되도록 고른 값이며, 특정 값에 의미를 두지 않는다면 이 기본값을 쓴다.
    static constexpr uint64 DefaultStream = 0x0a02bdbf7bb3c0a7ull;

    // seed와 stream으로 초기화한다. 모든 값이 유효하다. stream은 하위 63bit만 사용하므로
    // stream과 stream ^ (1 << 63)은 같은 순서를 만든다. 시드 의도를 숨기지 않도록 기본 생성자는
    // 제공하지 않는다.
    constexpr explicit RandomStream(uint64 seed, uint64 stream = DefaultStream) noexcept {
        Seed(seed, stream);
    }

    // 생성자와 같은 규칙으로 다시 초기화한다. stream을 생략하면 기존 stream을 유지하지 않고
    // DefaultStream을 사용한다. 즉 Seed(a, b) 이후 상태는 RandomStream(a, b)와 같다.
    constexpr void Seed(uint64 seed, uint64 stream = DefaultStream) noexcept {
        seed_ = seed;
        increment_ = (stream << 1u) | 1u;
        // 상태를 0에서 한 번 전진시킨 뒤 seed를 더하고 다시 전진시킨다. seed가 출력 함수에 바로
        // 노출되지 않게 하고, 참조 구현과 같은 첫 출력을 만들기 위한 순서다.
        state_ = 0;
        Step();
        state_ += seed;
        Step();
    }

    // 현재 seed와 stream의 첫 위치로 되돌린다. RestoreState() 뒤라면 복원한 seed가 기준이다.
    constexpr void Reset() noexcept {
        Seed(seed_, increment_ >> 1u);
    }

    constexpr RandomStreamState GetState() const noexcept {
        return RandomStreamState{seed_, increment_ >> 1u, state_};
    }

    // GetState()로 저장한 위치부터 이어서 생성한다. 모든 입력이 유효하며 stream 최상위 bit는
    // 무시한다. 같은 state 값이라도 stream이 다르면 다른 순서가 된다.
    constexpr void RestoreState(const RandomStreamState& snapshot) noexcept {
        seed_ = snapshot.seed;
        increment_ = (snapshot.stream << 1u) | 1u;
        state_ = snapshot.state;
    }

    // [0, 2^32) 균등 정수다. 출력 1회를 소비한다.
    constexpr uint32 NextUInt32() noexcept {
        const uint64 previous = state_;
        Step();
        // XSH RR: 상위 bit에 중간 bit를 xor로 섞은 뒤 bit 27..58을 32bit 결과로 고른다.
        // 회전량은 상태 최상위 5bit에서 얻어 결과와 회전량이 같은 bit에 의존하지 않게 한다.
        const uint32 folded = static_cast<uint32>(((previous >> 18u) ^ previous) >> 27u);
        const uint32 rotation = static_cast<uint32>(previous >> 59u);
        return RotateRight(folded, rotation);
    }

    // [0, bound) 균등 정수다. bound == 0은 빈 구간이므로 Fatal이다.
    // 2^32를 bound로 나눈 나머지만큼의 하위 출력값을 버리고 다시 뽑아(거절 표본) 나머지 연산의
    // 편향을 제거한다. 버리는 확률은 bound / 2^32 미만이며, bound가 2의 거듭제곱이면 버리지 않는다.
    // 소비 횟수는 1 + 거절 횟수다. 기대 소비는 2회 미만이지만 상한은 고정되어 있지 않다.
    // 거절 규칙과 나머지 사상은 참조 구현의 bounded 함수와 같은 결과를 만든다.
    constexpr uint32 NextUInt32Below(uint32 bound) noexcept {
        if (bound == 0u) {
            platform::Fatal("RandomStream::NextUInt32Below requires bound > 0");
        }
        // (2^32 - bound) mod bound == 2^32 mod bound. 이보다 작은 출력을 버리면 남은 출력 개수가
        // bound의 배수가 되어 각 나머지가 같은 횟수로 나온다.
        const uint32 threshold = (0u - bound) % bound;
        for (;;) {
            const uint32 value = NextUInt32();
            if (value >= threshold) {
                return value % bound;
            }
        }
    }

    // [minimum, maximum] 양 끝을 포함하는 균등 정수다. minimum > maximum은 Fatal이다.
    // 결과는 항상 minimum + offset(2의 보수 modulo)이다. [INT32_MIN, INT32_MAX] 전체 범위는 출력
    // 1회를 거절 없이 offset으로 쓰므로 결과는 출력의 최상위 bit를 뒤집은 값이다. 그 밖에는
    // NextUInt32Below(maximum - minimum + 1)과 같은 횟수를 소비한다. minimum == maximum도 별도
    // 분기 없이 출력 1회를 소비하고 minimum을 반환한다. 범위 폭이 1인지에 따라 이후 순서가 갈라지지
    // 않게 하기 위한 선택이다.
    constexpr int32 NextInt32InRange(int32 minimum, int32 maximum) noexcept {
        if (minimum > maximum) {
            platform::Fatal("RandomStream::NextInt32InRange requires minimum <= maximum");
        }
        // 2의 보수 차이를 uint32에서 계산하면 최대 폭 2^32 - 1도 overflow 없이 표현된다.
        const uint32 span = static_cast<uint32>(maximum) - static_cast<uint32>(minimum);
        uint32 offset = 0;
        if (span == 0xffffffffu) {
            // 값 개수 2^32는 uint32 bound로 표현할 수 없고, 모든 32bit 출력이 그대로 유효하다.
            offset = NextUInt32();
        } else {
            offset = NextUInt32Below(span + 1u);
        }
        // uint32 → int32 변환은 C++20부터 2의 보수 modulo로 정의되어 있다.
        return static_cast<int32>(static_cast<uint32>(minimum) + offset);
    }

    // true와 false가 각각 1/2 확률이다. 출력 최상위 bit를 사용하며 1회를 소비한다.
    constexpr bool NextBool() noexcept {
        return (NextUInt32() >> 31u) != 0u;
    }

    // [0, 1) 균등 float다. 출력 상위 24bit를 k라 할 때 k * 2^-24를 반환한다. float 가수가 표현할 수
    // 있는 bit 수와 같아 변환과 곱셈에 반올림이 없다. 0.0f는 나올 수 있고 최댓값은
    // 1 - 2^-24이므로 1.0f는 나오지 않는다. 출력 1회를 소비한다.
    constexpr float NextFloat01() noexcept {
        return static_cast<float>(NextUInt32() >> 8u) * 0x1.0p-24f;
    }

    // [0, 1) 균등 double이다. 첫 출력을 상위 32bit, 둘째 출력을 하위 32bit로 이은 64bit 값의 상위
    // 53bit를 k라 할 때 k * 2^-53을 반환한다. 반올림이 없으므로 최댓값은 1 - 2^-53이다.
    // 출력 2회를 이 순서로 소비한다.
    constexpr double NextDouble01() noexcept {
        // 두 호출을 한 식에 쓰면 평가 순서가 정해지지 않으므로 별도 문장으로 순서를 고정한다.
        const uint64 high = NextUInt32();
        const uint64 low = NextUInt32();
        const uint64 bits53 = ((high << 32u) | low) >> 11u;
        return static_cast<double>(bits53) * 0x1.0p-53;
    }

    // [minimum, maximum) 균등 float다. 두 값이 유한하고 minimum < maximum이어야 하며 위반은
    // Fatal이다. 출력 1회를 소비한다.
    //
    // 계산: 출력 상위 24bit로 u = k * 2^-24를 만들고 minimum + (maximum - minimum) * u를 double로
    // 계산한 뒤 float로 내림(floor)한다. 따라서 범위 안의 float f는 [f, f 다음 float) 폭에 비례하는
    // 확률로 나온다. 폭이 float 해상도보다 크면 결과 간격은 약 (maximum - minimum) * 2^-24다.
    // [0, 1)에서는 NextFloat01과 같은 값을 반환한다.
    //
    // maximum이 나오지 않는 이유: u <= 1 - 2^-24이므로 double 값과 maximum 사이에는 최소
    // (maximum - minimum) * 2^-24에 가까운 간격이 남는다. 이 간격은 maximum 근처 double 해상도보다
    // 훨씬 커서 double 값은 maximum보다 작다. float 최근접 반올림이 maximum으로 올림한 경우
    // (예: [1, 2)에서 u = 1 - 2^-24이면 2 - 2^-24가 짝수 반올림으로 2.0f가 된다) 내림 보정이 바로
    // 아래 float를 고른다. 순진한 float 식 minimum + (maximum - minimum) * NextFloat01()은 이 경우
    // maximum을 반환한다.
    constexpr float NextFloatInRange(float minimum, float maximum) noexcept {
        if (!IsFinite(minimum) || !IsFinite(maximum) || !(minimum < maximum)) {
            platform::Fatal("RandomStream::NextFloatInRange requires finite minimum < maximum");
        }
        const double unit = static_cast<double>(NextUInt32() >> 8u) * 0x1.0p-24;
        // double 차이는 float 최대 폭에서도 유한하다. 곱과 합을 별도 문장으로 두어 FMA 축약 여부에
        // 따라 플랫폼마다 결과가 달라지지 않게 한다.
        const double width = static_cast<double>(maximum) - static_cast<double>(minimum);
        const double scaled = width * unit;
        const double value = static_cast<double>(minimum) + scaled;
        float result = static_cast<float>(value);
        if (static_cast<double>(result) > value) {
            result = PreviousFloat(result);
        }
        if (!(result < maximum)) {
            // IEEE 기본 환경에서는 도달하지 않는다. FTZ/DAZ 같은 비기본 부동소수점 환경에서도
            // 반환값이 [minimum, maximum) 계약을 벗어나지 않게 하는 방어선이다.
            result = minimum;
        }
        return result;
    }

private:
    static constexpr uint64 Multiplier = 6364136223846793005ull;

    // unsigned 곱셈·덧셈은 2^64 modulo로 정의되어 있어 overflow가 의도된 동작이다.
    constexpr void Step() noexcept {
        state_ = state_ * Multiplier + increment_;
    }

    // rotation은 0..31이다. 왼쪽 이동량을 31로 mask해 rotation == 0일 때 32bit 이동(정의되지 않은
    // 동작)을 피한다.
    static constexpr uint32 RotateRight(uint32 value, uint32 rotation) noexcept {
        return (value >> rotation) | (value << ((0u - rotation) & 31u));
    }

    // compiler 최적화 설정과 무관하게 bit 표현으로 NaN·무한대를 판정한다.
    static constexpr bool IsFinite(float value) noexcept {
        return (platform::BitCast<uint32>(value) & 0x7f800000u) != 0x7f800000u;
    }

    // value보다 작은 float 중 가장 큰 값이다. value는 -FLT_MAX보다 큰 유한값이어야 한다.
    // ±0 바로 아래는 음의 최소 subnormal이고, 음수는 크기 bit를 늘려야 작아진다.
    static constexpr float PreviousFloat(float value) noexcept {
        const uint32 bits = platform::BitCast<uint32>(value);
        if ((bits & 0x7fffffffu) == 0u) {
            return platform::BitCast<float>(0x80000001u);
        }
        if ((bits >> 31u) != 0u) {
            return platform::BitCast<float>(bits + 1u);
        }
        return platform::BitCast<float>(bits - 1u);
    }

    uint64 state_ = 0;
    uint64 increment_ = 1;
    uint64 seed_ = 0;
};

} // namespace rocks
