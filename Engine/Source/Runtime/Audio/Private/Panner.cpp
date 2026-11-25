#include <Audio/Panner.h>

#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>

namespace rocks::audio {

using platform::PlatformMath;

namespace {

// 한 바퀴(도)다.
constexpr float FullCircleDegrees = 360.0f;

// 수평 방향 길이가 이보다 짧으면 방위각을 정할 수 없다고 보고 0°를 준다(UE SMALL_NUMBER).
constexpr double MinHorizontalLength = 1.0e-8;

// 방위각 계산에 들어가는 스피커 하나다. channelIndex는 레이아웃 안 색인이다.
struct SpeakerPosition {
    uint32 channelIndex;
    float azimuth;
};

// 레이아웃마다 방위각 오름차순 스피커 표다(UE
// InitializeChannelAzimuthMap·DeviceChannelAzimuthPositions). 색인은 AudioFormat.h 내부 순서(FL 0,
// FR 1, FC 2, LFE 3, SL 4, SR 5, BL 6, BR 7)다.
constexpr SpeakerPosition StereoSpeakers[] = {{1, 90.0f}, {0, 270.0f}};
constexpr SpeakerPosition Surround51Speakers[] = {
    {1, 30.0f}, {5, 110.0f}, {4, 250.0f}, {0, 330.0f}};
constexpr SpeakerPosition Surround71Speakers[] = {{1, 30.0f},  {5, 110.0f}, {7, 150.0f},
                                                  {6, 210.0f}, {4, 250.0f}, {0, 330.0f}};

Span<const SpeakerPosition> SpeakersOf(ChannelLayout layout) noexcept {
    switch (layout) {
    case ChannelLayout::Stereo:
        return Span<const SpeakerPosition>(StereoSpeakers);
    case ChannelLayout::Surround51:
        return Span<const SpeakerPosition>(Surround51Speakers);
    case ChannelLayout::Surround71:
        return Span<const SpeakerPosition>(Surround71Speakers);
    case ChannelLayout::Mono:
        break;
    }

    return {};
}

bool IsUnitInterval(float value) noexcept {
    return value >= 0.0f && value <= 1.0f;
}

} // namespace

float ComputeAzimuthDegrees(const Vec3& listenerForward, const Vec3& listenerRight,
                            const Vec3& listenerToSound) noexcept {
    // UE GetAzimuth는 (앞 내적, 오른쪽 내적) 2D 방향의 atan(|y/x|)와 사분면으로 0–360°를 만든다.
    // atan2와 같은 결과이고, UE의 경계 결함(정왼쪽 90°, 정후면 0°, 수직 90°)만 Rocks는 270°·
    // 180°·0°로 바로잡는다(Docs/Design/AudioSystem 7절).
    const double x = static_cast<double>(Dot(listenerForward, listenerToSound));
    const double y = static_cast<double>(Dot(listenerRight, listenerToSound));
    if (PlatformMath::Sqrt(x * x + y * y) <= MinHorizontalLength) {
        return 0.0f;
    }

    double degrees = math::RadiansToDegrees(PlatformMath::Atan2(y, x));
    if (degrees < 0.0) {
        degrees += static_cast<double>(FullCircleDegrees);
    }

    // −0에 가까운 음수가 360으로 올라간 경우를 [0, 360)으로 되돌린다.
    const float result = static_cast<float>(degrees);
    return result >= FullCircleDegrees ? 0.0f : result;
}

Result<void> ComputeChannelGains(ChannelLayout layout, float azimuthDegrees,
                                 float nonSpatializedAmount, float lfeBleed,
                                 Span<float> out) noexcept {
    const uint32 channels = ChannelCount(layout);
    if (out.Size() < channels || !math::IsFinite(azimuthDegrees) ||
        !IsUnitInterval(nonSpatializedAmount) || !IsUnitInterval(lfeBleed)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (layout == ChannelLayout::Mono) {
        out[0] = 1.0f;
        return Result<void>::Success();
    }

    float azimuth = PlatformMath::Fmod(azimuthDegrees, FullCircleDegrees);
    if (azimuth < 0.0f) {
        azimuth += FullCircleDegrees;
    }

    if (azimuth >= FullCircleDegrees) {
        azimuth = 0.0f;
    }

    // 방위각 이상인 첫 스피커가 next, 그 앞(감싸기)이 prev다. 없으면 마지막과 처음 사이다.
    const Span<const SpeakerPosition> speakers = SpeakersOf(layout);
    usize nextIndex = 0;
    usize prevIndex = speakers.Size() - 1;
    for (usize index = 0; index < speakers.Size(); ++index) {
        if (azimuth <= speakers[index].azimuth) {
            nextIndex = index;
            prevIndex = index == 0 ? speakers.Size() - 1 : index - 1;
            break;
        }
    }

    const SpeakerPosition& prev = speakers[prevIndex];
    const SpeakerPosition& next = speakers[nextIndex];
    float nextAzimuth = next.azimuth;
    if (nextAzimuth < prev.azimuth) {
        nextAzimuth += FullCircleDegrees;
    }

    if (azimuth < prev.azimuth) {
        azimuth += FullCircleDegrees;
    }

    const float fraction = (azimuth - prev.azimuth) / (nextAzimuth - prev.azimuth);
    const float angle = fraction * math::HalfPi<float>;
    // sin·cos가 π/2 근처에서 1을 조금 넘을 수 있어 [0, 1]로 자른다(UE와 같다).
    const float nextPan = math::Clamp(PlatformMath::Sin(angle), 0.0f, 1.0f);
    const float prevPan = math::Clamp(PlatformMath::Cos(angle), 0.0f, 1.0f);

    const float omni = 1.0f / static_cast<float>(channels);
    for (uint32 index = 0; index < channels; ++index) {
        float pan = 0.0f;
        if (ChannelAt(layout, index) == AudioChannel::LowFrequency) {
            out[index] = lfeBleed;
            continue;
        }

        if (index == prev.channelIndex) {
            pan = prevPan;
        } else if (index == next.channelIndex) {
            pan = nextPan;
        }

        out[index] = math::Lerp(pan, omni, nonSpatializedAmount);
    }

    return Result<void>::Success();
}

Result<void> ComputeCenterChannelOnlyGains(ChannelLayout layout, Span<float> out) noexcept {
    const uint32 channels = ChannelCount(layout);
    if (out.Size() < channels) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (layout == ChannelLayout::Mono) {
        out[0] = 1.0f;
        return Result<void>::Success();
    }

    if (layout == ChannelLayout::Stereo) {
        const float equalPower = 1.0f / PlatformMath::Sqrt(2.0f);
        out[0] = equalPower;
        out[1] = equalPower;
        return Result<void>::Success();
    }

    for (uint32 index = 0; index < channels; ++index) {
        out[index] = ChannelAt(layout, index) == AudioChannel::FrontCenter ? 1.0f : 0.0f;
    }

    return Result<void>::Success();
}

} // namespace rocks::audio
