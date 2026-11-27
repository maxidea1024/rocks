#include "../Private/Common/TextInputDocument.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <TestSupport/Test.h>

// TSF 거울 문서 시험이다(Docs/Design/UiToolkit 22.13절): 거울 편집(바꿈·삽입·선택·조합 범위
// 따라가기·잘못된 범위·용량), 잠금을 풀 때 TSF 기록 순서와 문맥 적용(조합 시작·갱신·확정, 조합 밖
// 삽입, 선택만 바뀜, 재변환), 문맥 맞추기 통지.
using namespace rocks;
using namespace rocks::platform;

namespace text_input_document_tests_file {

namespace {

// 미리보기 층을 따로 두지 않는 가짜 문맥이다(글자는 UTF-16 그대로).
class DocumentClient final : public TextInputClient {
public:
    explicit DocumentClient(Allocator& allocator) noexcept
        : text(allocator), allocator_(allocator) {
    }

    bool AllowsInputMethod() const noexcept override {
        return true;
    }

    void GetSelection(uint32& begin, uint32& length) const noexcept override {
        begin = selectionBegin;
        length = selectionLength;
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

    void SetSelection(uint32 begin, uint32 length) noexcept override {
        selectionBegin = begin;
        selectionLength = length;
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
    }

    bool GetTextBounds(uint32, uint32, Box2<float>&) const noexcept override {
        return false;
    }

    bool GetScreenBounds(Box2<float>&) const noexcept override {
        return false;
    }

    Optional<uint32> GetCharacterIndexFromPoint(Vec2) const noexcept override {
        return {};
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
    uint32 selectionBegin = 0;
    uint32 selectionLength = 0;
    uint32 rangeBegin = 0;
    uint32 rangeLength = 0;
    uint32 begins = 0;
    uint32 ends = 0;

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

bool MirrorIs(const TextInputDocument& document, const char16_t* expected) noexcept {
    const Span<const char16_t> text = document.Text();
    usize i = 0;
    for (; expected[i] != 0; ++i) {
        if (i >= text.Size() || text[i] != expected[i]) {
            return false;
        }
    }

    return i == text.Size();
}

} // namespace

// 거울 문서는 커서(64 KB 둘) 스택 대신 allocator에 둔다.
#define ROCKS_TEST_DOCUMENT(name)                                                                  \
    auto name##Made = UniquePtr<TextInputDocument>::TryMake(allocator);                            \
    REQUIRE(name##Made);                                                                           \
    TextInputDocument& name = *name##Made.Value()

TEST(PlatformWindow_TextInputDocumentEditsFollowTsfRules) {
    SystemAllocator allocator;
    ROCKS_TEST_DOCUMENT(document);
    const auto synced = document.Sync(Units(u"abc"), 3, 0);
    CHECK(synced.textChanged && synced.change.start == 0 && synced.change.newEnd == 3);
    // SetText: 바꾸고 caret을 뒤에 둔다(UE SetSelection + InsertTextAtSelection).
    TextInputChange change;
    REQUIRE(document.Replace(1, 2, Units(u"XY"), change));
    CHECK(MirrorIs(document, u"aXYc"));
    CHECK(change.start == 1 && change.oldEnd == 2 && change.newEnd == 3);
    CHECK(document.SelectionStart() == 3 && document.SelectionEnd() == 3);
    // InsertTextAtSelection은 선택 자리다.
    REQUIRE(document.InsertAtSelection(Units(u"Z"), change));
    CHECK(MirrorIs(document, u"aXYZc"));
    CHECK(change.start == 3 && change.oldEnd == 3 && change.newEnd == 4);
    // 잘못된 범위·선택은 거부하고 거울은 그대로다.
    CHECK(!document.Replace(4, 9, Units(u"q"), change));
    CHECK(!document.Replace(3, 2, Units(u"q"), change));
    CHECK(!document.SetSelection(5, 6, false));
    CHECK(!document.SetSelection(3, 2, false));
    CHECK(MirrorIs(document, u"aXYZc"));
    REQUIRE(document.SetSelection(1, 3, true));
    CHECK(document.CaretAtStart());
    // 빈 조합의 caret에 넣은 글자는 조합 안이고, 앞쪽 편집은 조합을 민다.
    document.StartComposition(5, 5);
    REQUIRE(document.Replace(5, 5, Units(u"k"), change));
    CHECK(document.CompositionStart() == 5 && document.CompositionEnd() == 6);
    REQUIRE(document.Replace(0, 0, Units(u"_"), change));
    CHECK(document.CompositionStart() == 6 && document.CompositionEnd() == 7);
    // 범위 밖 조합 표시는 글자 안으로 자른다.
    document.UpdateComposition(2, 99);
    CHECK(document.CompositionStart() == 2 && document.CompositionEnd() == document.Length());
    document.EndComposition();
    CHECK(!document.IsComposing());
}

TEST(PlatformWindow_TextInputDocumentRejectsTextOverCapacity) {
    SystemAllocator allocator;
    ROCKS_TEST_DOCUMENT(document);
    Array<char16_t> large(allocator);
    REQUIRE(large.TryResize(MaxTextInputDocumentCodeUnits + 1));
    for (char16_t& unit : large) {
        unit = u'a';
    }

    // 문맥이 너무 길면 거울을 비우고 알린다(IMM으로 받는다).
    const auto synced = document.Sync(large.View(), 0, 0);
    CHECK(synced.tooLong && document.Length() == 0);
    // 거울 편집도 용량을 넘지 않는다.
    REQUIRE(!document.Sync(Span<const char16_t>(large.Data(), MaxTextInputDocumentCodeUnits), 0, 0)
                 .tooLong);
    TextInputChange change;
    CHECK(!document.InsertAtSelection(Units(u"b"), change));
    CHECK(document.Length() == MaxTextInputDocumentCodeUnits);
}

// 한국어 조합 한 글자: 시작·갱신·확정이 TSF 기록 순서로 나오고 문맥에 그대로 적용된다.
TEST(PlatformWindow_TextInputDocumentFlushesCompositionAsTsfRecords) {
    SystemAllocator allocator;
    DocumentClient client(allocator);
    for (const char16_t unit : Units(u"ab")) {
        (void)client.text.TryPushBack(unit);
    }

    client.selectionBegin = 2;
    TextInputSession session;
    session.Activate(client);
    ROCKS_TEST_DOCUMENT(document);
    (void)document.Sync(Units(u"ab"), 2, 0);
    CHECK(!document.HasUnflushedEdits());
    // 첫 잠금: 조합 시작 + "ㅎ".
    TextInputChange change;
    document.StartComposition(2, 2);
    REQUIRE(document.Replace(2, 2, Units(u"ㅎ"), change));
    document.UpdateComposition(2, 3);
    CHECK(document.HasUnflushedEdits());
    auto flushed = document.Flush(session);
    REQUIRE(flushed.serial && !flushed.overflowed);
    // Rebase·CompositionBegin·TextReplace·CompositionRange·Selection 다섯이다.
    CHECK(session.PendingCount() == 5);
    CHECK(!document.HasUnflushedEdits());
    CHECK(session.Process(*flushed.serial));
    CHECK(client.TextIs(u"abㅎ"));
    CHECK(client.begins == 1 && client.ends == 0);
    CHECK(client.rangeBegin == 2 && client.rangeLength == 1);
    CHECK(client.selectionBegin == 3 && client.selectionLength == 0);
    CHECK(session.IsComposing());
    // 두 번째 잠금: 조합 안 글자가 "하"로 바뀐다(미리보기 — 확정 아님).
    REQUIRE(document.Replace(2, 3, Units(u"하"), change));
    document.UpdateComposition(2, 3);
    flushed = document.Flush(session);
    REQUIRE(flushed.serial);
    CHECK(session.PendingCount() == 4);
    CHECK(session.Process(*flushed.serial));
    CHECK(client.TextIs(u"ab하"));
    CHECK(client.begins == 1 && client.ends == 0);
    // 끝: Rebase와 확정 하나뿐이다(글자·선택은 그대로).
    document.EndComposition();
    flushed = document.Flush(session);
    REQUIRE(flushed.serial);
    CHECK(session.PendingCount() == 2);
    CHECK(session.Process(*flushed.serial));
    CHECK(client.ends == 1 && !session.IsComposing());
    // 아무것도 바뀌지 않으면 기록이 없다.
    CHECK(!document.Flush(session).serial);
}

// 조합 밖 편집(이모지 패널·자동 고침)은 확정 편집 하나, 선택만 바뀌면 선택 기록 하나, 재변환은
// 선택한 글자 위 조합이다.
TEST(PlatformWindow_TextInputDocumentFlushesEditsOutsideComposition) {
    SystemAllocator allocator;
    DocumentClient client(allocator);
    for (const char16_t unit : Units(u"ab")) {
        (void)client.text.TryPushBack(unit);
    }

    client.selectionBegin = 2;
    TextInputSession session;
    session.Activate(client);
    ROCKS_TEST_DOCUMENT(document);
    (void)document.Sync(Units(u"ab"), 2, 0);
    TextInputChange change;
    REQUIRE(document.InsertAtSelection(Units(u"!"), change));
    auto flushed = document.Flush(session);
    REQUIRE(flushed.serial);
    CHECK(session.PendingCount() == 3);
    CHECK(session.Process(*flushed.serial));
    CHECK(client.TextIs(u"ab!"));
    CHECK(client.begins == 1 && client.ends == 1 && !session.IsComposing());
    CHECK(client.selectionBegin == 3);
    // 선택만 바뀜.
    REQUIRE(document.SetSelection(0, 1, false));
    flushed = document.Flush(session);
    REQUIRE(flushed.serial);
    CHECK(session.PendingCount() == 2);
    CHECK(session.Process(*flushed.serial));
    CHECK(client.selectionBegin == 0 && client.selectionLength == 1);
    CHECK(client.begins == 1);
    // 재변환: "b"를 골라 그 위에서 조합해 "B"로 바꾸고 확정한다.
    REQUIRE(document.SetSelection(1, 2, false));
    document.StartComposition(1, 2);
    REQUIRE(document.Replace(1, 2, Units(u"B"), change));
    document.UpdateComposition(1, 2);
    flushed = document.Flush(session);
    REQUIRE(flushed.serial);
    CHECK(session.Process(*flushed.serial));
    CHECK(client.TextIs(u"aB!") && session.IsComposing());
    CHECK(client.rangeBegin == 1 && client.rangeLength == 1);
    document.EndComposition();
    flushed = document.Flush(session);
    REQUIRE(flushed.serial);
    CHECK(session.Process(*flushed.serial));
    CHECK(!session.IsComposing() && client.ends == 2);
}

// 문맥 쪽 변화(타자·되돌리기)를 거울에 맞추고 TSF에 알릴 범위를 낸다.
TEST(PlatformWindow_TextInputDocumentSyncReportsContextChanges) {
    SystemAllocator allocator;
    ROCKS_TEST_DOCUMENT(document);
    (void)document.Sync(Units(u"hello"), 5, 0);
    auto synced = document.Sync(Units(u"help!o"), 5, 0);
    CHECK(synced.textChanged);
    CHECK(synced.change.start == 3 && synced.change.oldEnd == 4 && synced.change.newEnd == 5);
    CHECK(!synced.selectionChanged);
    synced = document.Sync(Units(u"help!o"), 1, 2);
    CHECK(!synced.textChanged && synced.selectionChanged);
    CHECK(document.SelectionStart() == 1 && document.SelectionEnd() == 3);
    // 맞춘 뒤에는 낼 편집이 없다.
    CHECK(!document.HasUnflushedEdits());
    // 문맥 선택이 글자를 넘으면 글자 끝으로 자른다.
    synced = document.Sync(Units(u"ab"), 9, 3);
    CHECK(document.SelectionStart() == 2 && document.SelectionEnd() == 2);
}

// 클라이언트에 글자를 넣는다(시험 준비).
void Fill(DocumentClient& client, const char16_t* text, uint32 caret) noexcept {
    client.text.Clear();
    for (const char16_t unit : Units(text)) {
        (void)client.text.TryPushBack(unit);
    }

    client.selectionBegin = caret;
    client.selectionLength = 0;
}

// 리뷰 2026-11-28: TIP를 통과한 키(공백)가 기록보다 먼저 문맥에 들어가도 새 조합은 그 뒤에 온다.
TEST(PlatformWindow_TextInputDocumentRebasesOnKeysAppliedBeforeTheRecords) {
    SystemAllocator allocator;
    DocumentClient client(allocator);
    Fill(client, u"\uAC00", 1);
    TextInputSession session;
    session.Activate(client);
    ROCKS_TEST_DOCUMENT(document);
    (void)document.Sync(Units(u"\uAC00"), 1, 0);
    // TSF는 거울(caret 1)에서 "ㄴ" 조합을 시작한다.
    TextInputChange change;
    document.StartComposition(1, 1);
    REQUIRE(document.Replace(1, 1, Units(u"\u3134"), change));
    document.UpdateComposition(1, 2);
    const auto flushed = document.Flush(session);
    REQUIRE(flushed.serial);
    // 그 사이 큐에서 먼저 처리된 Space가 문맥에 들어갔다(거울은 모른다).
    Fill(client, u"\uAC00 ", 2);
    CHECK(session.Process(*flushed.serial));
    CHECK(client.TextIs(u"\uAC00 \u3134"));
    CHECK(client.rangeBegin == 2 && client.rangeLength == 1);
    CHECK(client.selectionBegin == 3);
}

// 리뷰 2026-11-28: 한 잠금 안에서 조합이 끝나고 새로 시작해도(Flush 없이) 앞 조합은 확정된다.
TEST(PlatformWindow_TextInputDocumentCommitsAndRestartsWithinOneLock) {
    SystemAllocator allocator;
    DocumentClient client(allocator);
    Fill(client, u"", 0);
    TextInputSession session;
    session.Activate(client);
    ROCKS_TEST_DOCUMENT(document);
    (void)document.Sync({}, 0, 0);
    TextInputChange change;
    document.StartComposition(0, 0);
    REQUIRE(document.Replace(0, 0, Units(u"\uD55C"), change));
    document.UpdateComposition(0, 1);
    auto flushed = document.Flush(session);
    REQUIRE(flushed.serial && session.Process(*flushed.serial));
    CHECK(client.begins == 1 && client.ends == 0);
    // 끝 → 시작(다음 음절)을 한 번에 낸다.
    document.EndComposition();
    document.StartComposition(1, 1);
    REQUIRE(document.Replace(1, 1, Units(u"\u3131"), change));
    document.UpdateComposition(1, 2);
    flushed = document.Flush(session);
    REQUIRE(flushed.serial && session.Process(*flushed.serial));
    // 앞 조합 확정 한 번, 새 조합 시작 한 번.
    CHECK(client.ends == 1 && client.begins == 2 && session.IsComposing());
    CHECK(client.TextIs(u"\uD55C\u3131"));
    CHECK(client.rangeBegin == 1 && client.rangeLength == 1);
}

// TIP가 조합 글자를 비우고 끝내면(취소) 문맥 글자는 조합 전으로 돌아온다.
TEST(PlatformWindow_TextInputDocumentCancelledCompositionRestoresText) {
    SystemAllocator allocator;
    DocumentClient client(allocator);
    Fill(client, u"ab", 2);
    TextInputSession session;
    session.Activate(client);
    ROCKS_TEST_DOCUMENT(document);
    (void)document.Sync(Units(u"ab"), 2, 0);
    TextInputChange change;
    document.StartComposition(2, 2);
    REQUIRE(document.Replace(2, 2, Units(u"\u314E"), change));
    document.UpdateComposition(2, 3);
    auto flushed = document.Flush(session);
    REQUIRE(flushed.serial && session.Process(*flushed.serial));
    REQUIRE(document.Replace(2, 3, {}, change));
    document.EndComposition();
    flushed = document.Flush(session);
    REQUIRE(flushed.serial && session.Process(*flushed.serial));
    CHECK(client.TextIs(u"ab") && client.ends == 1 && !session.IsComposing());
    CHECK(client.selectionBegin == 2);
}

// 리뷰 2026-11-28: TSF 기록이 넘치면 남은 기록은 내지 않고, 넘침 '끝'이 세션이 따라간 조합 범위를
// 지워 문맥은 조합 전 글자로 돌아온다.
TEST(PlatformWindow_TextInputDocumentOverflowRestoresTheTextBeforeComposition) {
    SystemAllocator allocator;
    DocumentClient client(allocator);
    Fill(client, u"ab", 2);
    TextInputSession session;
    session.Activate(client);
    ROCKS_TEST_DOCUMENT(document);
    (void)document.Sync(Units(u"ab"), 2, 0);
    TextInputChange change;
    document.StartComposition(2, 2);
    REQUIRE(document.Replace(2, 2, Units(u"\u314E"), change));
    document.UpdateComposition(2, 3);
    auto flushed = document.Flush(session);
    REQUIRE(flushed.serial && session.Process(*flushed.serial));
    REQUIRE(client.TextIs(u"ab\u314E") && session.IsComposing());
    // 처리하지 않은 채 조합 안 caret만 오가게 해 버퍼(기록 64)를 넘긴다.
    bool overflowed = false;
    uint32 last = *flushed.serial;
    for (uint32 i = 0; i < MaxTextInputRecords && !overflowed; ++i) {
        REQUIRE(document.SetSelection(2 + (i % 2), 2 + (i % 2), false));
        flushed = document.Flush(session);
        overflowed = flushed.overflowed;
        last = flushed.serial ? *flushed.serial : last;
    }

    REQUIRE(overflowed);
    CHECK(session.Process(last));
    CHECK(client.TextIs(u"ab") && !session.IsComposing() && client.ends == 1);
}

#undef ROCKS_TEST_DOCUMENT

} // namespace text_input_document_tests_file
