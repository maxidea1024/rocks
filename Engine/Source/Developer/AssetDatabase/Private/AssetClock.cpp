#include <AssetDatabase/AssetClock.h>
#include <Platform/Clock.h>

// 시스템 시계다(Docs/Design/AssetSystem.ko.md 7.1절 9 — A2a).
namespace rocks {

int64 SystemAssetClock::NowTicks() noexcept {
    return platform::UtcTicks();
}

} // namespace rocks
