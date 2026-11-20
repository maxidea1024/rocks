#include "LaunchProbe.h"

#include <Launch/EntryPoint.h>

namespace {

const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription Description =
        rocks::launch::probe::MakeDescription("LaunchWindowedProbe",
                                              rocks::launch::CrashReportingMode::Full);
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_WINDOWED_APPLICATION(Describe)
