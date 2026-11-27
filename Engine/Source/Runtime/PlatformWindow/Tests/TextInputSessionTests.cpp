#include "../Private/Common/TextInputSession.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

// IME 기록 버퍼·적용 규칙 시험이다(Docs/Design/UiToolkit 19.2절): 표지 경계 순서, 연속 확정,
// 선택을 바꾸는 조합 시작, 취소, 문맥 해제 뒤 기록 버림, 넘침.
using namespace rocks;
using namespace rocks::platform;

namespace text_input_session_tests_file {

namespace {

// 미리보기 층을 따로 두지 않는 가짜 문맥이다. 글자는 UTF-16 그대로 든다.
class FakeClient final : public TextInputClient {
public:
    explicit FakeClient(Allocator& allocator) noexcept : text(allocator), allocator_(allocator) {
    }

    bool AllowsInputMethod() const noexcept override {
        return allow;
    }

    void GetSelection(uint32& begin, uint32& length) const noexcept override {
        begin = selectionBegin;
        length = selectionLength;
    }

    void SetSelection(uint32 begin, uint32 length) noexcept override {
        selectionBegin = begin;
        selectionLength = length;
    }

    uint32 GetTextLength() const noexcept override {
        return static_cast<uint32>(text.Size());
    }

    Result<void> GetText(uint32 begin, uint32 length,
                         Array<char16_t>& out) const noexcept override {
        if (begin + length > text.Size()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (uint32 i = 0; i < length; ++i) {
            auto added = out.TryPushBack(text[begin + i]);
            if (!added) {
                return added;
            }
        }

        return Result<void>::Success();
    }

    bool GetScreenBounds(Box2<float>&) const noexcept override {
        return false;
    }

    Optional<uint32> GetCharacterIndexFromPoint(Vec2) const noexcept override {
        return {};
    }

    void SetText(uint32 begin, uint32 length, Span<const char16_t> value) noexcept override {
        Array<char16_t> next(allocator_);
        for (usize i = 0; i < begin; ++i) {
            (void)next.TryPushBack(text[i]);
        }

        for (const char16_t unit : value) {
            (void)next.TryPushBack(unit);
        }

        for (usize i = begin + length; i < text.Size(); ++i) {
            (void)next.TryPushBack(text[i]);
        }

        text = Move(next);
        ++edits;
    }

    bool GetTextBounds(uint32, uint32, Box2<float>&) const noexcept override {
        return false;
    }

    void BeginComposition() noexcept override {
        ++begins;
    }

    void UpdateCompositionRange(uint32 begin, uint32 length) noexcept override {
        rangeBegin = begin;
        rangeLength = length;
    }

    void EndComposition() noexcept override {
        ++ends;
    }

    bool TextIs(const char16_t* expected) const noexcept {
        usize i = 0;
        for (; expected[i] != 0; ++i) {
            if (i >= text.Size() || text[i] != expected[i]) {
                return false;
            }
        }

        return i == text.Size();
    }

    Array<char16_t> text;
    bool allow = true;
    uint32 selectionBegin = 0;
    uint32 selectionLength = 0;
    uint32 rangeBegin = 0;
    uint32 rangeLength = 0;
    uint32 begins = 0;
    uint32 ends = 0;
    uint32 edits = 0;

private:
    Allocator& allocator_;
};

Span<const char16_t> Units(const char16_t* text) noexcept {
    usize length = 0;
    while (text[length] != 0) {
        ++length;
    }

    return {text, length};
}

uint32 RecordOrZero(TextInputSession& session, TextInputRecordKind kind, const char16_t* text,
                    uint32 cursor = 0) noexcept {
    const auto result = session.Record(kind, Units(text), cursor);
    return result.serial ? *result.serial : 0;
}

} // namespace

// 표지 번호까지만 적용한다: [확정 "한", 조합 "ㄱ"]에서 첫 표지는 확정만 넣고 caret을 그 뒤에 둔다.
// 그래서 확정 표지 뒤의 키(Left)는 확정된 글자 뒤에서 움직인다.
TEST(PlatformWindow_TextInputSessionAppliesRecordsUpToTheMarker) {
    SystemAllocator allocator;
    FakeClient client(allocator);
    TextInputSession session;
    CHECK(!session.AcceptsInputMethod());
    CHECK(!session.Record(TextInputRecordKind::Commit, Units(u"x"), 0).serial);

    session.Activate(client);
    CHECK(session.AcceptsInputMethod());
    const uint32 commit = RecordOrZero(session, TextInputRecordKind::Commit, u"\uD55C");
    const uint32 composition =
        RecordOrZero(session, TextInputRecordKind::Composition, u"\u3131", 1);
    REQUIRE(commit != 0);
    REQUIRE(composition == commit + 1);
    CHECK(session.PendingCount() == 2);

    CHECK(session.Process(commit));
    CHECK(client.TextIs(u"\uD55C"));
    CHECK(client.selectionBegin == 1);
    CHECK(client.begins == 1);
    CHECK(client.ends == 1);
    CHECK(!session.IsComposing());
    CHECK(session.PendingCount() == 1);

    CHECK(session.Process(composition));
    CHECK(client.TextIs(u"\uD55C\u3131"));
    CHECK(session.IsComposing());
    CHECK(client.rangeBegin == 1);
    CHECK(client.rangeLength == 1);
    CHECK(client.selectionBegin == 2);
    // 같은 표지를 다시 처리해도 아무것도 없다.
    CHECK(!session.Process(composition));
}

// 한 펌프 안의 연속 확정 둘이 모두 순서대로 들어간다(메시지 시점에 읽었으므로 앞 확정을 잃지
// 않는다).
TEST(PlatformWindow_TextInputSessionKeepsConsecutiveCommits) {
    SystemAllocator allocator;
    FakeClient client(allocator);
    TextInputSession session;
    session.Activate(client);
    (void)RecordOrZero(session, TextInputRecordKind::Composition, u"\u314E", 1);
    (void)RecordOrZero(session, TextInputRecordKind::Commit, u"\uD55C");
    (void)RecordOrZero(session, TextInputRecordKind::Composition, u"\u3131", 1);
    const uint32 last = RecordOrZero(session, TextInputRecordKind::Commit, u"\uAE00");
    CHECK(session.Process(last));
    CHECK(client.TextIs(u"\uD55C\uAE00"));
    CHECK(client.begins == 2);
    CHECK(client.ends == 2);
    CHECK(client.selectionBegin == 2);
    CHECK(session.PendingCount() == 0);
}

// 첫 조합은 선택 영역을 조합 범위로 삼아 바꾼다. 확정 없이 끝나면 조합 범위를 지운다.
TEST(PlatformWindow_TextInputSessionReplacesSelectionAndCancelsOnEnd) {
    SystemAllocator allocator;
    FakeClient client(allocator);
    client.SetText(0, 0, Units(u"abcd"));
    client.SetSelection(1, 2);
    TextInputSession session;
    session.Activate(client);
    (void)RecordOrZero(session, TextInputRecordKind::Composition, u"\u304B", 1);
    const uint32 end = RecordOrZero(session, TextInputRecordKind::End, u"");
    REQUIRE(session.Process(end - 1));
    CHECK(client.TextIs(u"a\u304Bd"));
    CHECK(client.rangeBegin == 1);
    CHECK(client.rangeLength == 1);
    REQUIRE(session.Process(end));
    CHECK(client.TextIs(u"ad"));
    CHECK(client.selectionBegin == 1);
    CHECK(client.ends == 1);
    CHECK(!session.IsComposing());
    // 조합 없이 온 '끝'은 아무것도 바꾸지 않는다.
    const uint32 edits = client.edits;
    CHECK(session.Process(RecordOrZero(session, TextInputRecordKind::End, u"")));
    CHECK(client.edits == edits);

    // 조합 밖의 빈 조합도 아무 일이 아니다 — 선택 영역을 조합으로 잡아 지우지 않는다.
    client.SetSelection(0, 2);
    const uint32 begins = client.begins;
    CHECK(session.Process(RecordOrZero(session, TextInputRecordKind::Composition, u"")));
    CHECK(client.TextIs(u"ad"));
    CHECK(client.begins == begins);
    CHECK(!session.IsComposing());
}

// 끄면 남은 기록을 먼저 처리하고, 그래도 조합 중이면 그대로 확정한다. 옛 표지는 새 문맥의 기록을
// 처리하지 않는다. 취소는 남은 기록을 버린다.
TEST(PlatformWindow_TextInputSessionDropsRecordsOfReleasedContexts) {
    SystemAllocator allocator;
    FakeClient first(allocator);
    FakeClient second(allocator);
    TextInputSession session;
    session.Activate(first);
    const uint32 stale = RecordOrZero(session, TextInputRecordKind::Composition, u"\u3131", 1);
    CHECK(session.Deactivate(first));
    CHECK(first.TextIs(u"\u3131"));
    CHECK(first.ends == 1);
    CHECK(!session.Deactivate(first));
    CHECK(!session.Active());

    session.Activate(second);
    const uint32 fresh = RecordOrZero(session, TextInputRecordKind::Commit, u"\uD55C");
    CHECK(fresh > stale);
    CHECK(!session.Process(stale));
    CHECK(second.edits == 0);
    CHECK(session.Process(fresh));
    CHECK(second.TextIs(u"\uD55C"));

    const uint32 dropped = RecordOrZero(session, TextInputRecordKind::Composition, u"x", 1);
    CHECK(session.Cancel(second));
    CHECK(!session.Cancel(first));
    CHECK(!session.Process(dropped));
    CHECK(second.TextIs(u"\uD55C"));

    // IME를 허용하지 않는 문맥(비밀번호)은 기록하지 않는다 — 확정 문자는 Text 이벤트로 온다.
    second.allow = false;
    CHECK(!session.AcceptsInputMethod());
    CHECK(!session.Record(TextInputRecordKind::Commit, Units(u"x"), 0).serial);
}

// 기록이 넘치면 남은 조합을 지우고 확정은 지키며, 그래도 넘치면 '끝' 하나만 남긴다.
TEST(PlatformWindow_TextInputSessionOverflowKeepsCommitsThenCancels) {
    SystemAllocator allocator;
    FakeClient client(allocator);
    TextInputSession session;
    session.Activate(client);
    (void)RecordOrZero(session, TextInputRecordKind::Commit, u"a");
    for (usize i = 1; i < MaxTextInputRecords; ++i) {
        REQUIRE(RecordOrZero(session, TextInputRecordKind::Composition, u"b", 1) != 0);
    }

    CHECK(session.PendingCount() == MaxTextInputRecords);
    const auto kept = session.Record(TextInputRecordKind::Commit, Units(u"c"), 0);
    REQUIRE(kept.serial);
    CHECK(!kept.overflowed);
    CHECK(session.PendingCount() == 2);
    CHECK(session.Process(*kept.serial));
    CHECK(client.TextIs(u"ac"));

    // 확정만으로 가득 차면 넘침이다.
    for (usize i = 0; i < MaxTextInputRecords; ++i) {
        REQUIRE(RecordOrZero(session, TextInputRecordKind::Commit, u"d") != 0);
    }

    const auto overflow = session.Record(TextInputRecordKind::Commit, Units(u"e"), 0);
    REQUIRE(overflow.serial);
    CHECK(overflow.overflowed);
    CHECK(session.PendingCount() == 1);
    CHECK(session.Process(*overflow.serial));
    CHECK(client.TextIs(u"ac"));

    // 문자 용량을 넘는 문자열 하나도 넘침이다(조합 중이면 '끝'이 조합을 지운다).
    (void)session.Process(RecordOrZero(session, TextInputRecordKind::Composition, u"z", 1));
    CHECK(session.IsComposing());
    Array<char16_t> huge(allocator);
    REQUIRE(huge.TryResize(MaxTextInputCodeUnits + 1));
    const auto tooLong = session.Record(TextInputRecordKind::Commit, huge.View(), 0);
    REQUIRE(tooLong.serial);
    CHECK(tooLong.overflowed);
    CHECK(session.Process(*tooLong.serial));
    CHECK(client.TextIs(u"ac"));
    CHECK(!session.IsComposing());
}

} // namespace text_input_session_tests_file
