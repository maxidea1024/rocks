#include <TestSupport/Test.h>
#include <Core/Diagnostics/ProfileScope.h>
#include <Core/Function.h>
#include <Core/Lifetime/ModuleLease.h>
#include <Core/Memory/SystemAllocator.h>

using namespace rocks;

namespace {
struct ReentryCallable {
    int* live;
    int* selected;
    int value;
    Function<void()>* target;
    Function<void()>* replacement;

    ReentryCallable(int& count, int& selection, int initial,
                    Function<void()>* destination = nullptr,
                    Function<void()>* next = nullptr) noexcept
        : live(&count), selected(&selection), value(initial), target(destination),
          replacement(next) {
        ++*live;
    }

    ReentryCallable(ReentryCallable&& other) noexcept
        : live(other.live), selected(other.selected), value(other.value), target(other.target),
          replacement(other.replacement) {
        other.live = nullptr;
    }

    ~ReentryCallable() noexcept {
        if (live != nullptr) {
            --*live;
            if (target != nullptr) {
                *target = Move(*replacement);
            }
        }
    }

    void operator()() noexcept {
        *selected = value;
    }
};

struct ReentryLease {
    int references = 0;
    ModuleLease* target = nullptr;
    ModuleLease* replacement = nullptr;
};

Result<ModuleLease> AcquireReentryLease(ReentryLease& state) noexcept {
    return ModuleLease::Acquire(
        &state, [](void* raw) noexcept { ++static_cast<ReentryLease*>(raw)->references; },
        [](void* raw) noexcept {
            auto& value = *static_cast<ReentryLease*>(raw);
            --value.references;
            if (value.target != nullptr) {
                *value.target = Move(*value.replacement);
            }
        });
}
} // namespace

TEST(Function_ReentryDuringOldCaptureDestructionKeepsNewOwner) {
    SystemAllocator allocator;
    int live = 0;
    int selected = 0;
    Function<void()> target;
    auto preparedResult =
        Function<void()>::TryCreate(allocator, ReentryCallable(live, selected, 3));
    REQUIRE(preparedResult.IsOk());
    Function<void()> prepared(Move(preparedResult.Value()));
    auto old = Function<void()>::TryCreate(allocator,
                                           ReentryCallable(live, selected, 1, &target, &prepared));
    auto incoming = Function<void()>::TryCreate(allocator, ReentryCallable(live, selected, 2));
    REQUIRE(old.IsOk());
    REQUIRE(incoming.IsOk());
    target = Move(old.Value());
    CHECK(live == 3);
    target = Move(incoming.Value());
    CHECK(live == 1);
    CHECK(!prepared);
    target();
    CHECK(selected == 3);
    target.Reset();
    CHECK(live == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ModuleLease_ReentrantOldReleaseDoesNotLoseReplacement) {
    ModuleLease target;
    ReentryLease preparedState;
    ReentryLease incomingState;
    auto preparedResult = AcquireReentryLease(preparedState);
    REQUIRE(preparedResult.IsOk());
    ModuleLease prepared(Move(preparedResult.Value()));
    ReentryLease oldState{0, &target, &prepared};
    auto old = AcquireReentryLease(oldState);
    auto incoming = AcquireReentryLease(incomingState);
    REQUIRE(old.IsOk());
    REQUIRE(incoming.IsOk());
    target = Move(old.Value());
    target = Move(incoming.Value());
    CHECK(oldState.references == 0);
    CHECK(incomingState.references == 0);
    CHECK(preparedState.references == 1);
    CHECK(!prepared);
    target.Reset();
    CHECK(preparedState.references == 0);
}

TEST(Diagnostics_ProfilePreservesNameTruncationFlag) {
    LogBuffer buffer;
    char name[200];
    for (usize i = 0; i + 1 < sizeof(name); ++i) {
        name[i] = 'x';
    }
    name[sizeof(name) - 1] = '\0';
    {
        ProfileScope profile(buffer, name);
    }
    REQUIRE(buffer.Size() == 2);
    CHECK(buffer.At(0)->truncated);
    CHECK(buffer.At(1)->truncated);
}
