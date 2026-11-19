#include <ModuleLoader/ModuleReport.h>

namespace rocks {

const char* ModuleLoadCodeName(ModuleLoadCode code) noexcept {
    switch (code) {
    case ModuleLoadCode::Success:
        return "Success";
    case ModuleLoadCode::NotFound:
        return "NotFound";
    case ModuleLoadCode::Incompatible:
        return "Incompatible";
    case ModuleLoadCode::OsLoadFailed:
        return "OsLoadFailed";
    case ModuleLoadCode::EntryMissing:
        return "EntryMissing";
    case ModuleLoadCode::StartupFailed:
        return "StartupFailed";
    case ModuleLoadCode::DependencyFailed:
        return "DependencyFailed";
    case ModuleLoadCode::Cycle:
        return "Cycle";
    case ModuleLoadCode::WrongThread:
        return "WrongThread";
    case ModuleLoadCode::ShuttingDown:
        return "ShuttingDown";
    }

    return "Unknown";
}

const char* ModuleUnloadCodeName(ModuleUnloadCode code) noexcept {
    switch (code) {
    case ModuleUnloadCode::Success:
        return "Success";
    case ModuleUnloadCode::NotStarted:
        return "NotStarted";
    case ModuleUnloadCode::Unsupported:
        return "Unsupported";
    case ModuleUnloadCode::Busy:
        return "Busy";
    case ModuleUnloadCode::WrongThread:
        return "WrongThread";
    }

    return "Unknown";
}

} // namespace rocks
