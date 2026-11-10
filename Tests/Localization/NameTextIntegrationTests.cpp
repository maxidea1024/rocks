#include <Tests/Support/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Localization/Text.h>

using namespace rocks;

TEST(NameTextIdentitySurvivesLanguageSwitchAndReadersKeepTheirSnapshot) {
    SystemAllocator allocator;
    {
        NamePool names(allocator);
        REQUIRE(names.Initialize());
        auto titleName = names.TryIntern("ui.inventory.title");
        REQUIRE(titleName);
        auto nameReader = names.Acquire(titleName.Value());
        REQUIRE(nameReader);

        // 게임/위젯의 이름은 세션 식별자이고 표시 문구는 별도 stable TextId다.
        // 번역을 바꿔도 intern identity를 다시 만들거나 번역된 문자열을 이름으로 쓰지 않는다.
        constexpr TextId titleId{0x524f434b53544558ULL, 101};
        auto title = Text::Localized(allocator, titleId, 3, "Inventory");
        REQUIRE(title);
        const CatalogEntry englishEntry{titleId, 3, "Inventory"};
        const CatalogEntry koreanEntry{titleId, 3, "\xEA\xB0\x80\xEB\xB0\xA9"};
        auto english = CatalogSnapshot::Create(allocator, "en-US", 1, {&englishEntry, 1});
        auto korean = CatalogSnapshot::Create(allocator, "ko-KR", 1, {&koreanEntry, 1});
        REQUIRE(english);
        REQUIRE(korean);

        CatalogSnapshot oldFrame;
        CatalogSnapshot newFrame;
        {
            CatalogStore store;
            REQUIRE(store.Publish(english.Value()));
            auto first = store.Acquire();
            REQUIRE(first);
            oldFrame = Move(first.Value());
            REQUIRE(store.Publish(korean.Value()));
            auto second = store.Acquire();
            REQUIRE(second);
            newFrame = Move(second.Value());
            store.Close();
            auto closed = store.Acquire();
            CHECK(!closed && closed.Error() == ErrorCode::ShuttingDown);
        }
        english.Value().Reset();
        korean.Value().Reset();

        // 게시 서비스와 원래 snapshot 래퍼를 없애도 프레임별 reader의 내용은 바뀌지 않는다.
        auto oldLabel = ResolveText(allocator, title.Value(), oldFrame);
        auto newLabel = ResolveText(allocator, title.Value(), newFrame);
        REQUIRE(oldLabel);
        REQUIRE(newLabel);
        CHECK(oldLabel.Value().value.View() == StringView("Inventory"));
        CHECK(newLabel.Value().value.View() == StringView("\xEA\xB0\x80\xEB\xB0\xA9"));
        CHECK(oldLabel.Value().diagnostics.missingTranslations == 0);
        CHECK(newLabel.Value().diagnostics.staleTranslations == 0);
        CHECK(oldFrame.Culture() == StringView("en-US"));
        CHECK(newFrame.Culture() == StringView("ko-KR"));
        auto unchangedName = names.TryIntern("ui.inventory.title");
        REQUIRE(unchangedName);
        CHECK(unchangedName.Value() == titleName.Value());

        // Name reader는 pool 자체에 의존하므로 그 반환 전 Shutdown이 성공해서는 안 된다.
        auto busy = names.Shutdown();
        CHECK(!busy && busy.Error() == ErrorCode::Busy);
        CHECK(nameReader.Value().View() == StringView("ui.inventory.title"));
        nameReader.Value().Reset();
        REQUIRE(names.Shutdown());

        oldFrame.Reset();
        newFrame.Reset();
        title.Value().Reset();
        // resolve 결과는 입력 노드와 catalog에서 독립한 소유 문자열이다.
        CHECK(oldLabel.Value().value.View() == StringView("Inventory"));
        CHECK(newLabel.Value().value.View() == StringView("\xEA\xB0\x80\xEB\xB0\xA9"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(NameTextRepeatedHeadlessSessionTeardownLeavesNoOwners) {
    SystemAllocator allocator;
    Name previous;
    for (usize session = 0; session < 128; ++session) {
        {
            NamePool names(allocator);
            REQUIRE(names.Initialize());
            auto current = names.TryIntern("score.caption");
            REQUIRE(current);
            if (previous.IsValid()) {
                auto stale = names.Acquire(previous);
                CHECK(!stale && stale.Error() == ErrorCode::StaleHandle);
            }
            previous = current.Value();

            constexpr TextId scoreId{0x53434f5245544558ULL, 1};
            auto pattern = Text::Localized(allocator, scoreId, 4, "Score: {score}");
            auto score = Text::Integer(allocator, -42);
            REQUIRE(pattern);
            REQUIRE(score);
            const TextArgument argument{"score", &score.Value()};
            auto formatted = Text::Format(allocator, pattern.Value(), {&argument, 1});
            REQUIRE(formatted);
            // stale 번역은 현재 sourceRevision의 원문 형식을 사용해야 한다.
            const CatalogEntry entry{scoreId, 3, "obsolete {other}"};
            auto snapshot = CatalogSnapshot::Create(allocator, "ko-KR", session + 1, {&entry, 1});
            REQUIRE(snapshot);
            auto rendered = ResolveText(allocator, formatted.Value(), snapshot.Value());
            REQUIRE(rendered);
            CHECK(rendered.Value().value.View() == StringView("Score: -42"));
            CHECK(rendered.Value().diagnostics.staleTranslations == 1);
            CHECK(rendered.Value().diagnostics.missingTranslations == 0);
            REQUIRE(names.Shutdown());
        }
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
    }
}
