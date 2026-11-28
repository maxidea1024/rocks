#pragma once

#include <Audio/AudioBuffer.h>
#include <Audio/AudioMixerTypes.h>
#include <Base/NumericLimits.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Memory.h>

// 믹서 구현 파일(AudioMixer.cpp·AudioMixerBuses.cpp)이 함께 쓰는 작은 도움 함수·상수다.
namespace rocks::audio::mixer_internal {

// 64비트 원자 word의 상위 32비트 자리와 하위 mask다(mailbox·stop-all·terminal 묶음).
inline constexpr uint32 HighWordShift = 32;
inline constexpr uint64 LowWordMask = NumericLimits<uint32>::Max();

// seqlock version의 홀수 bit(쓰는 중)다.
inline constexpr uint32 SeqlockWritingBit = 1;

// plan word {일련번호:56, 버퍼 색인:8}의 버퍼 bit 수와, plan 버퍼 수(미채택 상한 + RT가 쓰는 것
// 하나 — 3.45.4절)다.
inline constexpr uint32 PlanWordBufferBits = 8;
inline constexpr uint32 PlanBufferCount = MaxPendingPlans + 1;

inline uint32 NextGeneration(uint32 generation) noexcept {
    // 0은 무효 handle이라 건너뛴다.
    return generation + 1 == 0 ? 1 : generation + 1;
}

// generation 차이(부호 있음)다. 양수면 a가 b보다 새롭다(3.23.1절 — 2^31 한계).
inline int32 GenerationDelta(uint32 a, uint32 b) noexcept {
    return static_cast<int32>(a - b);
}

inline uint32 FloatBits(float value) noexcept {
    return platform::BitCast<uint32>(value);
}

inline float BitsFloat(uint32 bits) noexcept {
    return platform::BitCast<float>(bits);
}

// 범위 밖 값을 자르거나 기본값으로 바꾸고 바꾼 수를 센다(실시간 경로는 실패를 돌려줄 곳이 없다).
inline float ClampCounted(float value, float minimum, float maximum, float fallback,
                          uint64& counter) noexcept {
    if (!math::IsFinite(value)) {
        ++counter;
        return fallback;
    }

    if (value < minimum || value > maximum) {
        ++counter;
        return math::Clamp(value, minimum, maximum);
    }

    return value;
}

inline PlanarView WithChannels(PlanarView view, uint32 channels) noexcept {
    view.channelCount = channels;
    return view;
}

// destination = source(채널 수·frame 수가 같다).
inline void CopyView(const PlanarView& destination, const PlanarView& source) noexcept {
    for (uint32 channel = 0; channel < destination.channelCount; ++channel) {
        const float* from = source.channels[channel];
        float* to = destination.channels[channel];
        for (uint32 frame = 0; frame < destination.frameCount; ++frame) {
            to[frame] = from[frame];
        }
    }
}

} // namespace rocks::audio::mixer_internal
