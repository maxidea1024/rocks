#include "LaunchProbe.h"

#include <Launch/EntryPoint.h>

namespace {

const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription Description =
        rocks::launch::probe::MakeDescription("LaunchProbe",
                                              rocks::launch::CrashReportingMode::Full);
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
