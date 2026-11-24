#include <Core/Module/ModuleEntry.h>
#include <Gameplay/Gameplay.module.generated.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/LocalPlayer.generated.h>

// Gameplay 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일이 없다 — 리플렉션
// 타입은 엔진의 모듈 타입 등록기가 등록한다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Gameplay)

namespace rocks {

const char* NetModeName(NetMode mode) noexcept {
    switch (mode) {
    case NetMode::Standalone:
        return "Standalone";
    case NetMode::DedicatedServer:
        return "DedicatedServer";
    case NetMode::ListenServer:
        return "ListenServer";
    case NetMode::Client:
        return "Client";
    }

    return "Unknown";
}

namespace {

char LowerAscii(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool SameKey(StringView left, StringView right) noexcept {
    if (left.ByteCount() != right.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < left.ByteCount(); ++index) {
        if (LowerAscii(left.Data()[index]) != LowerAscii(right.Data()[index])) {
            return false;
        }
    }

    return true;
}

} // namespace

bool FindMapOption(StringView options, StringView key, StringView& value) noexcept {
    // '?'로 나눈 항목마다 "key" 또는 "key=value"다. 앞의 맵 이름 부분(첫 '?' 전)은 보지 않는다.
    const char* const data = options.Data();
    const usize size = options.ByteCount();
    usize at = 0;
    while (at < size && data[at] != '?') {
        ++at;
    }

    while (at < size) {
        const usize start = at + 1;
        usize end = start;
        while (end < size && data[end] != '?') {
            ++end;
        }

        usize equals = start;
        while (equals < end && data[equals] != '=') {
            ++equals;
        }

        if (SameKey(options.SliceBytes(start, equals - start), key)) {
            value = equals < end ? options.SliceBytes(equals + 1, end - equals - 1)
                                 : options.SliceBytes(end, 0);
            return true;
        }

        at = end;
    }

    return false;
}

Result<void> RegisterGameplayTypes(ModuleRegistration& registration) noexcept {
    return generated::Gameplay::RegisterModuleTypes(registration);
}

LocalPlayer::LocalPlayer(Allocator& allocator) noexcept : subsystems_(allocator) {
}

LocalPlayer::~LocalPlayer() noexcept = default;

} // namespace rocks
