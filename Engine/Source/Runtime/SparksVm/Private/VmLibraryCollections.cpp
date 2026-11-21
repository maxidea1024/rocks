#include "VmLibrary.h"

// 언어 함수 구현(Docs/Design/SparksVm.ko.md 6a절, LC5c).

namespace rocks::sparks::detail {

Span<const VmLibraryEntry> VmCollectionEntries() noexcept {
    return Span<const VmLibraryEntry>();
}

} // namespace rocks::sparks::detail
