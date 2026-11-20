#include <Graphics/SplitTransitionTracker.h>

#include <Core/Hash.h>

namespace rocks::graphics {

namespace {

// 전이 하나의 fingerprint 단어 수다(handle 둘의 칸 4, 범위 4, 상태 2, 획득 1).
// ResourceTransition에는 padding byte가 있으므로(acquire) 구조체 byte 대신 칸을 단어로 옮겨
// 해시한다.
constexpr usize TransitionWords = 11;

} // namespace

// 칸이 늘면 아래 단어 목록도 늘려야 한다(bool acquire 뒤 padding까지 4 byte 한 칸).
static_assert(sizeof(ResourceTransition) == 4 * sizeof(uint32) + sizeof(SubresourceRange) +
                                                2 * sizeof(ResourceAccess) + sizeof(uint32),
              "ResourceTransition fields changed: update the split transition fingerprint words");

SplitTransitionTracker::SplitTransitionTracker(Allocator& allocator) noexcept : open_(allocator) {
}

uint64 SplitTransitionTracker::Fingerprint(Span<const ResourceTransition> transitions) noexcept {
    uint64 hash = transitions.Size();
    for (const ResourceTransition& transition : transitions) {
        const uint32 words[TransitionWords] = {transition.texture.slot.index,
                                               transition.texture.slot.generation,
                                               transition.buffer.slot.index,
                                               transition.buffer.slot.generation,
                                               transition.range.firstMip,
                                               transition.range.mipCount,
                                               transition.range.firstSlice,
                                               transition.range.sliceCount,
                                               static_cast<uint32>(transition.before),
                                               static_cast<uint32>(transition.after),
                                               transition.acquire ? 1u : 0u};
        hash = HashCombine(hash, SipHash24(HashSeed{}, words, sizeof(words)));
    }

    return hash;
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
