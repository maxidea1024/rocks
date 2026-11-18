#include <Graphics/SplitTransitionTracker.h>

#include <Core/Hash.h>

namespace rocks::graphics {

static_assert(sizeof(ResourceTransition) ==
                  4 * sizeof(uint32) + sizeof(SubresourceRange) + 2 * sizeof(ResourceAccess),
              "ResourceTransition must have no padding bytes for its fingerprint");

SplitTransitionTracker::SplitTransitionTracker(Allocator& allocator) noexcept : open_(allocator) {
}

uint64 SplitTransitionTracker::Fingerprint(Span<const ResourceTransition> transitions) noexcept {
    return HashCombine(
        transitions.Size(),
        SipHash24(HashSeed{}, transitions.Data(), transitions.Size() * sizeof(ResourceTransition)));
}

SplitTransition SplitTransitionTracker::Begin(Span<const ResourceTransition> transitions) noexcept {
    if (transitions.IsEmpty()) {
        return {};
    }

    ++serial_;
    if (serial_ == 0) {
        ++serial_;
    }

    Open open;
    open.id = serial_;
    open.count = static_cast<uint32>(transitions.Size());
    open.fingerprint = Fingerprint(transitions);
    if (!open_.TryPushBack(open)) {
        return {};
    }

    return SplitTransition{open.id};
}

SplitEndResult SplitTransitionTracker::End(SplitTransition split,
                                           Span<const ResourceTransition> transitions) noexcept {
    if (split.IsNull()) {
        return SplitEndResult::Null;
    }

    for (usize index = 0; index < open_.Size(); ++index) {
        if (open_[index].id != split.id) {
            continue;
        }

        const Open open = open_[index];
        open_[index] = open_[open_.Size() - 1];
        auto popped = open_.TryPopBack();
        (void)popped;
        return open.count == transitions.Size() && open.fingerprint == Fingerprint(transitions)
                   ? SplitEndResult::Matched
                   : SplitEndResult::Mismatch;
    }

    return SplitEndResult::Unknown;
}

usize SplitTransitionTracker::Close() noexcept {
    const usize count = open_.Size();
    open_.Clear();
    return count;
}

} // namespace rocks::graphics
