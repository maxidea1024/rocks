#pragma once

#include <Base/Types.h>
#include <Core/Diagnostics/LogCategory.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Memory.h>

// SpatialIndex 모듈의 구조들(LooseOctree·FlatBoundsSet)이 함께 쓰는 내부 도구다. 공개 API가 아니다.
namespace rocks::spatial {

inline constexpr LogCategory LogSpatialIndex{"SpatialIndex"};

// 프로세스 전역에서 한 번도 쓰지 않은 변경 세대를 돌려준다(1부터 늘어난다). 구조마다 세대를 따로
// 세면 이동 대입이나 같은 주소에 다시 만든 구조가 우연히 같은 세대를 가져, 다른 구조(또는 이전
// 상태)에서 만든 결과(LooseOctree 노드 묶음, FlatBoundsSet 가시성 비트 집합)를 받아들일 수 있다.
// 전역 단조 계수는 생성·이동·대입·변경마다 새 값을 주므로 (구조, 세대) 쌍이 다른 상태와 겹치지
// 않는다. 값이 겹치지 않기만 하면 되고 다른 메모리와의 순서는 필요 없으므로(결과의 발행·소비 순서는
// 읽기 범위의 atomic이 맡는다) relaxed 증가로 충분하다.
uint64 NextMutationEpoch() noexcept;

// float 값의 전순서 키다(−0과 +0은 같고, 비정규 값도 비트대로 구별한다). 부동소수 비교는 DAZ
// 환경에서 비정규 입력을 0으로 보므로 저장 경계의 불변 조건을 거짓으로 보고할 수 있어 정수로
// 비교한다. NaN은 오지 않는다.
inline int64 FloatOrderKey(float value) noexcept {
    const uint32 raw = platform::BitCast<uint32>(value);
    const int64 magnitude = static_cast<int64>(raw & 0x7fffffffu);
    return (raw & 0x80000000u) != 0 ? -magnitude : magnitude;
}

// stored < value(double)인지다. value 이상인 가장 작은 float r에 대해 stored < value ⇔ stored <
// r이다 (stored와 value 사이에 float가 끼지 않으므로).
inline bool FloatBelow(float stored, double value) noexcept {
    return FloatOrderKey(stored) < FloatOrderKey(math::ToFloatRoundedUp(value));
}

// stored > value(double)인지다. value 이하인 가장 큰 float r에 대해 stored > value ⇔ stored >
// r이다.
inline bool FloatAbove(float stored, double value) noexcept {
    return FloatOrderKey(stored) > FloatOrderKey(math::ToFloatRoundedDown(value));
}

// max(magnitude, |value|)를 비트로 계산한다(magnitude ≥ 0). DAZ 환경에서도 비정규 값을 0으로 보지
// 않는다.
inline float MaxMagnitude(float magnitude, float value) noexcept {
    const uint32 bits = platform::BitCast<uint32>(value) & 0x7fffffffu;
    return bits > platform::BitCast<uint32>(magnitude) ? platform::BitCast<float>(bits) : magnitude;
}

} // namespace rocks::spatial
