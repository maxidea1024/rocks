#pragma once

#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefs.h>

namespace rocks::reflection_fixture {

struct ROCKS_REFLECT_TYPE("ad100000000000000000000000000001", 1) Base : Object {
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000011") Guid key {};
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000012") int32 count = 0;
};

struct ROCKS_REFLECT_TYPE("ad100000000000000000000000000002", 3) Node final : Base {
    explicit Node(Allocator& allocator) noexcept : label(allocator) {
    }

    // logical destroy 이후 필드를 다시 역참조하지 않고 destructor에서 commit 결과를 관찰한다.
    static inline bool destroyedWithOldLabel = false;
    static inline bool destroyedWithNewLabel = false;

    ~Node() noexcept {
        destroyedWithOldLabel = label.View() == StringView("old");
        destroyedWithNewLabel = label.View() == StringView("new");
    }

    ROCKS_REFLECT_FIELD("ad100000000000000000000000000021") String label;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000022") ObjectRef<Node> next;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000023") bool enabled = false;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000024") uint8 small = 0;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000025") uint16 medium = 0;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000026") uint32 flags = 0;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000027") uint64 wide = 0;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000028") int64 signedWide = 0;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000029") float weight = 0;
    ROCKS_REFLECT_FIELD("ad10000000000000000000000000002a") double precision = 0;
};

} // namespace rocks::reflection_fixture
