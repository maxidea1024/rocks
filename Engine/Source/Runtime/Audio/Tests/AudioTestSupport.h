#pragma once

#include <Audio/AudioBuffer.h>
#include <Core/Math/RandomStream.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

#include <string.h>

// Audio DSP1 시험(Docs/Design/AudioSystem 3.20.8절)이 함께 쓰는 버퍼 생성·비교 도구다.
namespace rocks::audio::audiotest {

// 시험 버퍼는 늘 유효한 인자로 만든다(실패는 시험 코드 오류다).
inline PlanarBuffer MakeBuffer(uint32 channels, uint32 frames, Allocator& allocator) noexcept {
    auto created = PlanarBuffer::TryCreate(channels, frames, allocator);
    if (!created) {
        platform::Fatal("Audio test buffer was rejected");
    }

    return Move(created.Value());
}

// [-1, 1) 무작위 값으로 채운다.
inline void FillRandom(const PlanarView& view, RandomStream& random) noexcept {
    for (uint32 channel = 0; channel < view.channelCount; ++channel) {
        for (uint32 frame = 0; frame < view.frameCount; ++frame) {
            view.channels[channel][frame] = random.NextFloatInRange(-1.0f, 1.0f);
        }
    }
}

// 모든 채널에 같은 값을 채운다.
inline void Fill(const PlanarView& view, float value) noexcept {
    for (uint32 channel = 0; channel < view.channelCount; ++channel) {
        for (uint32 frame = 0; frame < view.frameCount; ++frame) {
            view.channels[channel][frame] = value;
        }
    }
}

// 두 보기의 내용이 bit 단위로 같은지 본다(블록 분할 불변 시험).
inline bool BitEqual(const PlanarConstView& left, const PlanarConstView& right) noexcept {
    if (left.channelCount != right.channelCount || left.frameCount != right.frameCount) {
        return false;
    }

    for (uint32 channel = 0; channel < left.channelCount; ++channel) {
        if (memcmp(left.channels[channel], right.channels[channel],
                   sizeof(float) * left.frameCount) != 0) {
            return false;
        }
    }

    return true;
}

} // namespace rocks::audio::audiotest
