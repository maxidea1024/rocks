#include <TestSupport/Test.h>
#include <TestSupport/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Localization/Text.h>

// 리플렉션 B9l Text 영속 API 시험이다(Docs/Design/ReflectionExtensions.ko.md 13절):
// TryGetPersistentForm의 세 형태와 Format·Integer Unsupported, DeepClone의 형태 보존·원래
// allocator와 독립인 수명·깊이·작업 한도·할당 실패 정리.
using namespace rocks;

namespace {

constexpr TextId GreetingId{0x51, 0x52};

void CheckResolves(Allocator& allocator, const Text& text, StringView expected) noexcept {
    auto resolved = ResolveText(allocator, text, {});
    REQUIRE(resolved);
    CHECK(resolved.Value().value.View() == expected);
}

} // namespace

// 비어 있음·Invariant·Localized는 형태와 내용을 돌려주고 Format·Integer는 Unsupported다(out은
// 그대로).
TEST(TextPersist_FormsAndUnsupportedHistories) {
    SystemAllocator allocator;
    {
        TextPersistentView view;
        Text empty;
        REQUIRE(empty.TryGetPersistentForm(view));
        CHECK(view.form == TextPersistentForm::Empty && view.source.IsEmpty() &&
              view.id == TextId{} && view.sourceRevision == 0);

        auto invariant = Text::Invariant(allocator, "plain");
        REQUIRE(invariant);
        REQUIRE(invariant.Value().TryGetPersistentForm(view));
        CHECK(view.form == TextPersistentForm::Invariant && view.source == "plain" &&
              view.id == TextId{} && view.sourceRevision == 0);

        auto localized = Text::Localized(allocator, GreetingId, 7, "Hello");
        REQUIRE(localized);
        REQUIRE(localized.Value().TryGetPersistentForm(view));
        CHECK(view.form == TextPersistentForm::Localized && view.source == "Hello" &&
              view.id == GreetingId && view.sourceRevision == 7);

        auto number = Text::Integer(allocator, -42);
        REQUIRE(number);
        CheckResolves(allocator, number.Value(), "-42");
        auto integer = number.Value().TryGetPersistentForm(view);
        CHECK(!integer && integer.Error() == ErrorCode::Unsupported);
        CHECK(view.form == TextPersistentForm::Localized && view.source == "Hello");

        const TextArgument arguments[] = {{"n", &number.Value()}};
        auto pattern = Text::Invariant(allocator, "count {n}");
        REQUIRE(pattern);
        auto formatted = Text::Format(allocator, pattern.Value(), arguments);
        REQUIRE(formatted);
        auto format = formatted.Value().TryGetPersistentForm(view);
        CHECK(!format && format.Error() == ErrorCode::Unsupported);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Text::Localized가 거부하는 집합(textId 0·sourceRevision 0)이다. Serialization ReadTextWire·
// WriteTextWire의 거부 집합과의 대조는 ObjectSerialization TextGraph 시험이 한다.
TEST(TextPersist_LocalizedRejectsZeroIdentity) {
    SystemAllocator allocator;
    {
        auto noId = Text::Localized(allocator, TextId{}, 1, "a");
        CHECK(!noId && noId.Error() == ErrorCode::InvalidArgument);
        auto noRevision = Text::Localized(allocator, GreetingId, 0, "a");
        CHECK(!noRevision && noRevision.Error() == ErrorCode::InvalidArgument);
        CHECK(Text::Localized(allocator, GreetingId, 1, "a"));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// DeepClone은 형태를 보존하고(Format 이력 포함) 원래 allocator가 사라진 뒤에도 유효하다. 비어 있는
// Text는 비어 있는 Text다.
TEST(TextPersist_DeepCloneOutlivesSourceAllocator) {
    SystemAllocator target;
    {
        Text localizedCopy;
        Text formatCopy;
        {
            SystemAllocator source;
            auto localized = Text::Localized(source, GreetingId, 3, "Hi");
            REQUIRE(localized);
            auto cloned = localized.Value().DeepClone(target);
            REQUIRE(cloned);
            localizedCopy = Move(cloned.Value());

            auto pattern = Text::Invariant(source, "{a}-{b}");
            auto shared = Text::Invariant(source, "x");
            REQUIRE(pattern && shared);
            const TextArgument arguments[] = {{"a", &shared.Value()}, {"b", &shared.Value()}};
            auto formatted = Text::Format(source, pattern.Value(), arguments);
            REQUIRE(formatted);
            auto formatClone = formatted.Value().DeepClone(target);
            REQUIRE(formatClone);
            formatCopy = Move(formatClone.Value());

            Text empty;
            auto emptyClone = empty.DeepClone(target);
            REQUIRE(emptyClone);
            CHECK(!emptyClone.Value().IsValid());
        }

        TextPersistentView view;
        REQUIRE(localizedCopy.TryGetPersistentForm(view));
        CHECK(view.form == TextPersistentForm::Localized && view.source == "Hi" &&
              view.id == GreetingId && view.sourceRevision == 3);
        CheckResolves(target, formatCopy, "x-x");
        auto format = formatCopy.TryGetPersistentForm(view);
        CHECK(!format && format.Error() == ErrorCode::Unsupported);
    }

    CHECK(target.Stats().liveAllocations == 0);
}

// 깊이(root = 1)·작업 한도를 넘으면 CapacityExceeded, 64를 넘는 maxDepth는 InvalidArgument다. 할당
// 실패는 그 오류이고 만든 노드를 모두 반환한다.
TEST(TextPersist_DeepCloneLimitsAndAllocationFailure) {
    SystemAllocator allocator;
    {
        auto leaf = Text::Invariant(allocator, "leaf");
        REQUIRE(leaf);
        auto pattern = Text::Invariant(allocator, "<{v}>");
        REQUIRE(pattern);
        const TextArgument arguments[] = {{"v", &leaf.Value()}};
        auto nested = Text::Format(allocator, pattern.Value(), arguments);
        REQUIRE(nested);

        TextResolveLimits shallow;
        shallow.maxDepth = 1;
        auto deep = nested.Value().DeepClone(allocator, shallow);
        CHECK(!deep && deep.Error() == ErrorCode::CapacityExceeded);
        TextResolveLimits tight;
        tight.maxWork = 3;
        auto busy = nested.Value().DeepClone(allocator, tight);
        CHECK(!busy && busy.Error() == ErrorCode::CapacityExceeded);
        TextResolveLimits invalid;
        invalid.maxDepth = 65;
        auto rejected = nested.Value().DeepClone(allocator, invalid);
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);

        // 할당 시도마다 실패시켜 본다: 실패는 OutOfMemory이고 누수가 없다.
        test::NFailAllocator failing(allocator);
        const uint64 before = allocator.Stats().liveAllocations;
        for (uint64 attempt = 1; attempt <= 16; ++attempt) {
            failing.FailOnAttempt(attempt);
            auto cloned = nested.Value().DeepClone(failing);
            if (cloned) {
                CheckResolves(allocator, cloned.Value(), "<leaf>");
                break;
            }

            CHECK(cloned.Error() == ErrorCode::OutOfMemory);
            CHECK(allocator.Stats().liveAllocations == before);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
