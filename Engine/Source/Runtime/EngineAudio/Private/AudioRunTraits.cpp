#include <Core/App/ProcessCommandLine.h>
#include <EngineAudio/AudioRunTraits.h>

namespace rocks {

namespace {

constexpr StringView NoSoundFlag = "--no-sound";
constexpr StringView MuteAudioFlag = "--mute-audio";
constexpr StringView AllowCommandletAudioFlag = "--allow-commandlet-audio";
constexpr StringView BenchmarkFlag = "--benchmark";

bool HasFlag(Span<const StringView> arguments, StringView flag) noexcept {
    for (const StringView argument : arguments) {
        if (argument == flag) {
            return true;
        }
    }

    return false;
}

} // namespace

AudioRunTraits ResolveAudioRunTraits(build::TargetType target, Span<const StringView> arguments,
                                     bool acceptDevelopmentFlags) noexcept {
    AudioRunTraits traits;
    const bool allowCommandletAudio =
        acceptDevelopmentFlags && HasFlag(arguments, AllowCommandletAudioFlag);
    if (target == build::TargetType::Server) {
        traits.audioOutput = false;
    } else if (target == build::TargetType::Program && !allowCommandletAudio) {
        traits.audioOutput = false;
    }

    if (HasFlag(arguments, NoSoundFlag)) {
        traits.audioOutput = false;
    }

    if (acceptDevelopmentFlags && HasFlag(arguments, BenchmarkFlag)) {
        traits.audioOutput = false;
    }

    traits.audioMuted = HasFlag(arguments, MuteAudioFlag);
    return traits;
}

AudioRunTraits CurrentAudioRunTraits() noexcept {
    Span<const StringView> arguments;
    if (ProcessCommandLine::IsCaptured()) {
        arguments = ProcessCommandLine::Get().Arguments();
    }

#if ROCKS_BUILD_RELEASE
    constexpr bool acceptDevelopmentFlags = false;
#else
    constexpr bool acceptDevelopmentFlags = true;
#endif
    return ResolveAudioRunTraits(build::CurrentTargetType(), arguments, acceptDevelopmentFlags);
}

} // namespace rocks
