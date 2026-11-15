#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

// NamePool::TryFind(intern 없는 조회)·NameTextEquals(pool 없는 비교)·SplitNameText(base·번호
// 분해)의 시험이다(Docs/Design/ReflectionExtensions.ko.md 4절 B9c1).

namespace {

// 기본 hash와, 모든 이름을 한 bucket에서 충돌시키는 축소 mask다.
constexpr uint64 HashMasks[] = {NumericLimits<uint64>::Max(), 0};

bool SameFields(Name left, Name right) noexcept {
    return left.domain == right.domain && left.id == right.id &&
           left.displayId == right.displayId && left.number == right.number &&
           left.kind == right.kind;
}

bool DisplayIs(NamePool& pool, Name name, StringView expected) noexcept {
    auto lease = pool.Acquire(name);
    return lease && lease.Value().View() == expected;
}

bool Readable(NamePool& pool, Name name) noexcept {
    auto lease = pool.Acquire(name);
    return static_cast<bool>(lease);
}

ErrorCode FindError(const NamePool& pool, StringView source) noexcept {
    auto found = pool.TryFind(source);
    return found ? ErrorCode::None : found.Error();
}

bool SameStats(const NamePoolStats& left, const NamePoolStats& right) noexcept {
    return left.domain == right.domain && left.entryCount == right.entryCount &&
           left.storageBytes == right.storageBytes && left.pendingEntries == right.pendingEntries &&
           left.pendingStorageBytes == right.pendingStorageBytes &&
           left.activeOperations == right.activeOperations &&
           left.activeLeases == right.activeLeases && left.accepting == right.accepting &&
           left.reclaiming == right.reclaiming;
}

// 전수 대조의 실패를 원문 색인과 함께 기록한다(CHECK는 식만 남긴다). right가 IndexNone이면 색인
// 하나짜리 실패다.
void RecordPairFailure(const char* what, usize left, usize right, int line) noexcept {
    char message[160];
    usize length = 0;
    auto append = [&](const char* text) {
        while (*text != '\0' && length + 1 < sizeof(message)) {
            message[length++] = *text++;
        }
    };
    auto appendIndex = [&](usize value) {
        char digits[24];
        usize count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count != 0 && length + 1 < sizeof(message)) {
            message[length++] = digits[--count];
        }
    };
    append(what);
    append(" left=");
    appendIndex(left);
    if (right != IndexNone) {
        append(" right=");
        appendIndex(right);
    }

    message[length] = '\0';
    test::RecordFailure(message, __FILE__, line);
}

// intern 후보 할당 안에서 멈춰(게시 전) 시험이 그 순간의 pool을 보게 한다. 할당은 gate 밖에서
// 불리므로 callback 안에서 probePool 조회도 해 본다(재진입이 교착하지 않아야 한다).
class PausingAllocator final : public Allocator {
public:
    explicit PausingAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (probePool != nullptr) {
            auto found = probePool->TryFind(probeInput);
            probeError = found ? ErrorCode::None : found.Error();
            probeFound = found ? found.Value() : Name{};
            probeStats = probePool->Stats();
        }

        arrived.Signal();
        if (released.Wait(10000) != platform::WaitResult::Signaled) {
            platform::Fatal("Name lookup test allocation was not released");
        }

        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

    const NamePool* probePool = nullptr;
    StringView probeInput;
    ErrorCode probeError = ErrorCode::Busy;
    Name probeFound{};
    NamePoolStats probeStats{};
    platform::ManualResetEvent arrived;
    platform::ManualResetEvent released;

private:
    Allocator& backing_;
};

struct FindWorker {
    NamePool* pool = nullptr;
    platform::ManualResetEvent* start = nullptr;
    StringView input;
    bool intern = false;
    Name name{};
    ErrorCode error = ErrorCode::None;
};

void RunFindWorker(void* raw) noexcept {
    auto& worker = *static_cast<FindWorker*>(raw);
    if (worker.start->Wait(10000) != platform::WaitResult::Signaled) {
        worker.error = ErrorCode::Busy;
        return;
    }

    auto result =
        worker.intern ? worker.pool->TryIntern(worker.input) : worker.pool->TryFind(worker.input);
    if (result) {
        worker.name = result.Value();
    } else {
        worker.error = result.Error();
    }
}

} // namespace

TEST(NameFind_ReturnsPublishedTokenWithoutAllocating) {
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        auto actor = pool.TryIntern("Actor_7");
        auto socket = pool.TryIntern("Socket");
        REQUIRE(actor && socket);
        const NamePoolStats before = pool.Stats();
        const uint64 allocationsBefore = system.Stats().totalAllocations;
        // 조회는 allocator를 부르지 않는다. 다음 할당 시도를 실패시켜도 결과가 같다.
        allocator.FailOnAttempt(1);

        auto foundActor = pool.TryFind("Actor_7");
        auto foundSocket = pool.TryFind("Socket");
        REQUIRE(foundActor && foundSocket);
        CHECK(SameFields(foundActor.Value(), actor.Value()));
        CHECK(SameFields(foundSocket.Value(), socket.Value()));
        CHECK(foundActor.Value().GetNumber() == 7);

        // 없는 이름, 같은 base의 다른 번호, 번호 없는 base, 선행 0은 모두 없음이다.
        CHECK(FindError(pool, "Missing") == ErrorCode::NotFound);
        CHECK(FindError(pool, "Actor_8") == ErrorCode::NotFound);
        CHECK(FindError(pool, "Actor") == ErrorCode::NotFound);
        CHECK(FindError(pool, "Actor_07") == ErrorCode::NotFound);
        CHECK(FindError(pool, "Socket_0") == ErrorCode::NotFound);
        CHECK(FindError(pool, StringView("Socket\0", 7)) == ErrorCode::NotFound);

        const NamePoolStats after = pool.Stats();
        CHECK(allocator.Attempts() == 0);
        CHECK(system.Stats().totalAllocations == allocationsBefore);
        CHECK(after.entryCount == before.entryCount && after.storageBytes == before.storageBytes);
        CHECK(after.pendingEntries == 0 && after.pendingStorageBytes == 0);
        CHECK(after.activeOperations == 0 && after.activeLeases == 0);
        allocator.DisableFailure();

        // 조회는 intern이 아니다: 찾지 못한 이름은 그 뒤에도 없다.
        CHECK(FindError(pool, "Missing") == ErrorCode::NotFound);
        REQUIRE(pool.Shutdown());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(NameFind_FullPoolStillFindsAndReportsNotFound) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        NamePoolConfig config;
        config.maxEntries = 1;
        REQUIRE(pool.Initialize(config));
        auto only = pool.TryIntern("Only_1");
        REQUIRE(only);
        CHECK(pool.TryIntern("Other").Error() == ErrorCode::CapacityExceeded);
        // 한도와 무관하다: 있는 이름은 찾고 없는 이름은 CapacityExceeded가 아니라 NotFound다.
        auto found = pool.TryFind("ONLY_1");
        REQUIRE(found);
        CHECK(SameFields(found.Value(), only.Value()));
        CHECK(FindError(pool, "Other") == ErrorCode::NotFound);
        CHECK(pool.Stats().entryCount == 1);
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameFind_CaseVariantReturnsFirstPublishedDisplay) {
    SystemAllocator allocator;
    for (const uint64 mask : HashMasks) {
        NamePool pool(allocator);
        NamePoolConfig config;
        config.hashMask = mask; // 0이면 모든 이름이 한 bucket에서 충돌한다.
        REQUIRE(pool.Initialize(config));
        // 첫 표시는 chain 순서(최근 게시가 앞)가 아니라 게시 순서로 정한다.
        auto upper = pool.TryIntern("ACTOR_7");
        auto lower = pool.TryIntern("actor_7");
        auto mixed = pool.TryIntern("Actor_7");
        auto other = pool.TryIntern("Actor_8");
        auto plain = pool.TryIntern("Actor");
        REQUIRE(upper && lower && mixed && other && plain);

        auto exact = pool.TryFind("actor_7");
        REQUIRE(exact);
        CHECK(SameFields(exact.Value(), lower.Value()));

        auto variant = pool.TryFind("aCtOr_7");
        REQUIRE(variant);
        CHECK(variant.Value() == mixed.Value());
        CHECK(variant.Value().displayId == upper.Value().displayId);
        CHECK(DisplayIs(pool, variant.Value(), "ACTOR_7"));

        auto numbered = pool.TryFind("ACTOR_8");
        auto unnumbered = pool.TryFind("aCTOR");
        REQUIRE(numbered && unnumbered);
        CHECK(SameFields(numbered.Value(), other.Value()));
        CHECK(SameFields(unnumbered.Value(), plain.Value()));
        CHECK(numbered.Value() != variant.Value() && unnumbered.Value() != variant.Value());
        CHECK(FindError(pool, "ACTOR_9") == ErrorCode::NotFound);

        // 비ASCII는 byte 비교다: "Ü"와 "ü"는 다른 이름이고 ASCII 부분만 대소문자를 무시한다.
        auto unit = pool.TryIntern("\xC3\x9C"
                                   "nit");
        REQUIRE(unit);
        auto foldedAscii = pool.TryFind("\xC3\x9C"
                                        "NIT");
        REQUIRE(foldedAscii);
        CHECK(SameFields(foldedAscii.Value(), unit.Value()));
        CHECK(FindError(pool, "\xC3\xBC"
                              "nit") == ErrorCode::NotFound);

        // embedded NUL은 이름의 일부이고 ASCII 대소문자만 무시한다.
        constexpr char upperNul[] = {'A', '\0', 'Z', '_', '3'};
        constexpr char lowerNul[] = {'a', '\0', 'z', '_', '3'};
        auto nul = pool.TryIntern(StringView(upperNul, sizeof(upperNul)));
        REQUIRE(nul);
        auto nulFound = pool.TryFind(StringView(lowerNul, sizeof(lowerNul)));
        REQUIRE(nulFound);
        CHECK(SameFields(nulFound.Value(), nul.Value()));
        CHECK(FindError(pool, StringView(lowerNul, 3)) == ErrorCode::NotFound);
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameFind_NoneRulesNeedNoEntry) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        // 기본 None은 entry 없이 언제나 찾는다.
        const StringView canonical[] = {StringView(nullptr), "",     "None", "_0", "_7",
                                        "_2147483646",       "none", "NONE"};
        for (StringView input : canonical) {
            auto found = pool.TryFind(input);
            REQUIRE(found);
            CHECK(SameFields(found.Value(), Name{}));
        }
        CHECK(pool.Stats().entryCount == 0);
        // "_2147483647"은 번호 범위 밖이라 일반 이름이고, 번호 있는 None은 entry가 있어야 한다.
        CHECK(FindError(pool, "_2147483647") == ErrorCode::NotFound);
        CHECK(FindError(pool, "None_3") == ErrorCode::NotFound);

        auto display = pool.TryIntern("nOnE");
        auto numbered = pool.TryIntern("none_3");
        REQUIRE(display && numbered);
        // byte가 같은 표시는 그 entry, 다른 표시 변형은 첫 표시인 정적 "None"이다.
        auto exact = pool.TryFind("nOnE");
        auto variant = pool.TryFind("NONE");
        REQUIRE(exact && variant);
        CHECK(SameFields(exact.Value(), display.Value()));
        CHECK(SameFields(variant.Value(), Name{}));
        CHECK(exact.Value() == variant.Value());

        auto numberVariant = pool.TryFind("NONE_3");
        REQUIRE(numberVariant);
        CHECK(SameFields(numberVariant.Value(), numbered.Value()));
        CHECK(numberVariant.Value().kind == NameKind::NoneDisplay);
        CHECK(!numberVariant.Value().IsNone() && numberVariant.Value().GetNumber() == 3);
        CHECK(DisplayIs(pool, numberVariant.Value(), "none_3"));
        CHECK(FindError(pool, "None_4") == ErrorCode::NotFound);
        CHECK(FindError(pool, "None_03") == ErrorCode::NotFound);
        CHECK(pool.Stats().entryCount == 2);
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameFind_StateAndInvalidInputLeavePoolUnchanged) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        const NamePoolStats stopped = pool.Stats();
        CHECK(FindError(pool, "Actor") == ErrorCode::ShuttingDown);
        CHECK(FindError(pool, "") == ErrorCode::ShuttingDown);
        CHECK(FindError(pool, StringView("\xff", 1)) == ErrorCode::ShuttingDown);
        CHECK(SameStats(pool.Stats(), stopped));
        REQUIRE(pool.Initialize());
        auto actor = pool.TryIntern("Actor");
        REQUIRE(actor);
        // 잘못된 UTF-8은 TryIntern처럼 InvalidArgument다.
        const NamePoolStats running = pool.Stats();
        CHECK(FindError(pool, StringView("\xff", 1)) == ErrorCode::InvalidArgument);
        CHECK(FindError(pool, StringView("Actor\xc3", 6)) == ErrorCode::InvalidArgument);
        CHECK(FindError(pool, "Missing") == ErrorCode::NotFound);
        CHECK(SameStats(pool.Stats(), running));
        CHECK(pool.TryIntern(StringView("\xff", 1)).Error() == ErrorCode::InvalidArgument);

        // Shutdown이 시작되면(lease 때문에 Busy인 닫힌 상태 포함) 조회도 닫힌다.
        auto lease = pool.Acquire(actor.Value());
        REQUIRE(lease);
        CHECK(pool.Shutdown().Error() == ErrorCode::Busy);
        const NamePoolStats closing = pool.Stats();
        CHECK(FindError(pool, "Actor") == ErrorCode::ShuttingDown);
        CHECK(FindError(pool, StringView("\xff", 1)) == ErrorCode::ShuttingDown);
        CHECK(SameStats(pool.Stats(), closing));
        lease.Value().Reset();
        REQUIRE(pool.Shutdown());
        const NamePoolStats shutDown = pool.Stats();
        CHECK(FindError(pool, "Actor") == ErrorCode::ShuttingDown);
        CHECK(SameStats(pool.Stats(), shutDown));

        // 재초기화한 pool은 비어 있다.
        REQUIRE(pool.Initialize());
        CHECK(FindError(pool, "Actor") == ErrorCode::NotFound);
        auto again = pool.TryIntern("Actor");
        REQUIRE(again);
        auto found = pool.TryFind("actor");
        REQUIRE(found);
        CHECK(SameFields(found.Value(), again.Value()));
        CHECK(found.Value().domain != actor.Value().domain);
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameFind_ConcurrentWithInternSeesPublishedOrNotFound) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        platform::ManualResetEvent start;
        constexpr usize Count = 8;
        const StringView inputs[Count] = {"Alpha_7", "alpha_7", "ALPHA_7", "aLpHa_7",
                                          "Alpha_7", "ALPHA_7", "alpha_7", "Beta"};
        FindWorker workers[Count]{};
        platform::Thread threads[Count];
        usize started = 0;
        for (; started < Count; ++started) {
            workers[started].pool = &pool;
            workers[started].start = &start;
            workers[started].input = inputs[started];
            workers[started].intern = started < 4;
            if (threads[started].Start(RunFindWorker, &workers[started]) !=
                platform::ThreadStatus::Success) {
                break;
            }
        }

        start.Signal();
        for (usize index = 0; index < started; ++index) {
            if (threads[index].WaitForCompletion(10000) != platform::WaitResult::Signaled ||
                threads[index].Join() != platform::ThreadStatus::Success) {
                platform::Fatal("Name lookup test worker did not finish");
            }
        }

        REQUIRE(started == Count);
        for (usize index = 0; index < 4; ++index) {
            CHECK(workers[index].error == ErrorCode::None);
            CHECK(workers[index].name == workers[0].name);
        }

        // 조회는 게시된 토큰이거나 아직 게시 전이면 NotFound다. 이름을 만들지 않는다.
        for (usize index = 4; index < 7; ++index) {
            CHECK(workers[index].error == ErrorCode::None ||
                  workers[index].error == ErrorCode::NotFound);
            if (workers[index].error == ErrorCode::None) {
                CHECK(workers[index].name == workers[0].name);
                CHECK(Readable(pool, workers[index].name));
            }
        }

        CHECK(workers[7].error == ErrorCode::NotFound);
        CHECK(pool.Stats().entryCount == 4);
        auto settled = pool.TryFind("ALPHA_7");
        REQUIRE(settled);
        CHECK(settled.Value() == workers[0].name);
        CHECK(DisplayIs(pool, settled.Value(), "ALPHA_7"));
        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameFind_PendingInternIsNotFoundUntilPublished) {
    SystemAllocator system;
    {
        PausingAllocator pausing(system);
        NamePool pool(pausing);
        REQUIRE(pool.Initialize());
        pausing.probePool = &pool;
        platform::ManualResetEvent start;
        start.Signal();

        // 첫 intern을 후보 할당 안에서 멈춘다: 예약은 잡혔지만 게시 전이다.
        pausing.probeInput = "gamma_3";
        FindWorker first{&pool, &start, "Gamma_3", true};
        platform::Thread firstThread;
        REQUIRE(firstThread.Start(RunFindWorker, &first) == platform::ThreadStatus::Success);
        const bool firstArrived = pausing.arrived.Wait(10000) == platform::WaitResult::Signaled;
        const NamePoolStats pending = pool.Stats();
        const ErrorCode exactWhilePending = FindError(pool, "Gamma_3");
        const ErrorCode variantWhilePending = FindError(pool, "GAMMA_3");
        const NamePoolStats afterFind = pool.Stats();
        pausing.released.Signal();
        if (firstThread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            firstThread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Name lookup pending worker did not finish");
        }

        REQUIRE(firstArrived);
        CHECK(pending.pendingEntries == 1 && pending.activeOperations == 1 &&
              pending.entryCount == 0);
        CHECK(exactWhilePending == ErrorCode::NotFound);
        CHECK(variantWhilePending == ErrorCode::NotFound);
        CHECK(SameStats(pending, afterFind));
        // allocator callback(gate 밖) 안의 조회도 교착 없이 같은 답을 낸다.
        CHECK(pausing.probeError == ErrorCode::NotFound);
        CHECK(pausing.probeStats.pendingEntries == 1 && pausing.probeStats.activeOperations == 1);
        REQUIRE(first.error == ErrorCode::None);
        auto published = pool.TryFind("gamma_3");
        REQUIRE(published);
        CHECK(SameFields(published.Value(), first.name));
        CHECK(DisplayIs(pool, published.Value(), "Gamma_3"));
        CHECK(pool.Stats().pendingEntries == 0 && pool.Stats().activeOperations == 0);

        // 대소문자 변형을 게시 전에 멈추면 조회는 이미 게시된 첫 표시를 돌려준다.
        pausing.arrived.Reset();
        pausing.released.Reset();
        pausing.probeInput = "GAMMA_3";
        pausing.probeError = ErrorCode::Busy;
        FindWorker variant{&pool, &start, "GAMMA_3", true};
        platform::Thread variantThread;
        REQUIRE(variantThread.Start(RunFindWorker, &variant) == platform::ThreadStatus::Success);
        const bool variantArrived = pausing.arrived.Wait(10000) == platform::WaitResult::Signaled;
        auto beforePublish = pool.TryFind("GAMMA_3");
        pausing.released.Signal();
        if (variantThread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            variantThread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Name lookup variant worker did not finish");
        }

        REQUIRE(variantArrived);
        REQUIRE(beforePublish);
        CHECK(SameFields(beforePublish.Value(), first.name));
        CHECK(pausing.probeError == ErrorCode::None);
        CHECK(SameFields(pausing.probeFound, first.name));
        REQUIRE(variant.error == ErrorCode::None);
        auto afterPublish = pool.TryFind("GAMMA_3");
        REQUIRE(afterPublish);
        CHECK(SameFields(afterPublish.Value(), variant.name));
        CHECK(afterPublish.Value() == first.name);
        CHECK(afterPublish.Value().displayId != first.name.displayId);
        CHECK(DisplayIs(pool, afterPublish.Value(), "GAMMA_3"));
        pausing.probePool = nullptr;
        REQUIRE(pool.Shutdown());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(NameTextEquals_FollowsPoolComparisonRules) {
    struct Case {
        StringView left;
        StringView right;
        bool equal;
    };
    const Case cases[] = {
        // ASCII 대소문자와 번호.
        {"Socket", "SOCKET", true},
        {"Socket_7", "sOCKET_7", true},
        {"Socket_7", "Socket_8", false},
        {"Socket_7", "Socket_07", false},
        {"Socket_07", "SOCKET_07", true}, // 선행 0은 접미사가 아니라 base 전체를 비교한다
        {"Socket", "Socket_0", false},
        {"Socket_0", "socket_0", true},
        {"Socket", "Sockets", false},
        {"Socket_7", "Socket7", false},
        {"Socket__7", "socket__7", true},
        {"Socket__7", "Socket_7", false},
        {"a_1_2", "A_1_2", true},
        {"a_1_2", "a_12", false},
        // 번호 경계(UE 5.8: 0–2,147,483,646, 10자리 이하).
        {"Socket_2147483646", "SOCKET_2147483646", true},
        {"Socket_2147483646", "Socket_2147483645", false},
        {"Socket_2147483647", "socket_2147483647", true},
        {"Socket_2147483647", "Socket_2147483646", false},
        {"Socket_9999999999", "SOCKET_9999999999", true},
        {"Socket_10000000000", "Socket_10000000000", true},
        {"Socket_-1", "Socket_1", false},
        // 비ASCII는 byte 비교, embedded NUL은 이름의 일부다.
        {"\xC3\x9C"
         "nit",
         "\xC3\x9C"
         "NIT",
         true},
        {"\xC3\x9C"
         "nit",
         "\xC3\xBC"
         "nit",
         false},
        {"\xC3\x9C_3", "\xC3\x9C_3", true},
        {"\xC3\x9C_3", "\xC3\x9C_4", false},
        {StringView("A\0Z_1", 5), StringView("a\0z_1", 5), true},
        {StringView("A\0Z", 3), StringView("A", 1), false},
        // None 규칙.
        {"", StringView(nullptr), true},
        {"", "None", true},
        {"none", "NONE", true},
        {"_7", "", true},
        {"_7", "_8", true}, // 빈 base의 번호 이름은 번호와 무관하게 기본 None이다
        {"_0", "none", true},
        {"None_3", "none_3", true},
        {"None_3", "None", false},
        {"None_3", "None_4", false},
        {"None_0", "None", false},
        {"None_0", "_0", false},
        {"_2147483647", "", false}, // 번호 범위 밖이라 base가 "_2147483647"인 일반 이름
        {"_00", "", false},
        {"Nonex", "None", false},
        {StringView("None\0", 5), "None", false},
        {"None", "Socket", false},
    };
    for (const Case& sample : cases) {
        CHECK(NameTextEquals(sample.left, sample.right) == sample.equal);
        CHECK(NameTextEquals(sample.right, sample.left) == sample.equal);
        CHECK(NameTextEquals(sample.left, sample.left));
        // 같으면 key도 같다(hash는 TryMakeNameKey). 입력은 모두 유효 UTF-8이다.
        auto leftKey = TryMakeNameKey(sample.left);
        auto rightKey = TryMakeNameKey(sample.right);
        REQUIRE(leftKey && rightKey);
        if (sample.equal) {
            CHECK(leftKey.Value() == rightKey.Value());
        }
    }
}

TEST(NameTextEquals_AgreesWithInternFindAndKey) {
    const StringView texts[] = {"Actor",
                                "actor",
                                "ACTOR_1",
                                "Actor_1",
                                "Actor_01",
                                "Actor_0",
                                "actor_0",
                                "None",
                                "none",
                                "NONE",
                                "None_0",
                                "NONE_0",
                                "None_7",
                                "nOnE_7",
                                "",
                                "_9",
                                "_2147483647",
                                "Mesh_10",
                                "mesh_10",
                                "Mesh_2",
                                "Mesh__2",
                                "a_b",
                                "a_B_1",
                                "A_b_1",
                                "x_",
                                "X_",
                                "PLAYER_2147483646",
                                "player_2147483646",
                                "Player_2147483647",
                                "\xC3\x9C"
                                "ber",
                                "\xC3\x9C"
                                "BER",
                                "\xC3\xBC"
                                "ber",
                                "\xEA\xB0\x80_1",
                                "\xEA\xB0\x80_01",
                                "a_9999999999",
                                StringView("q\0Q", 3),
                                StringView("Q\0q", 3)};
    constexpr usize Count = sizeof(texts) / sizeof(texts[0]);
    NameKey keys[Count];
    for (usize index = 0; index < Count; ++index) {
        auto key = TryMakeNameKey(texts[index]);
        REQUIRE(key);
        keys[index] = key.Value();
    }

    SystemAllocator allocator;
    for (const uint64 mask : HashMasks) {
        NamePoolConfig config;
        config.hashMask = mask;
        // 전체 pool: 모든 원문을 intern한다. TryFind는 byte가 같은 표시를 찾는다.
        NamePool full(allocator);
        REQUIRE(full.Initialize(config));
        Name interned[Count];
        for (usize index = 0; index < Count; ++index) {
            auto name = full.TryIntern(texts[index]);
            REQUIRE(name);
            interned[index] = name.Value();
        }

        for (usize left = 0; left < Count; ++left) {
            auto found = full.TryFind(texts[left]);
            if (!found || !SameFields(found.Value(), interned[left])) {
                RecordPairFailure("full TryFind differs from TryIntern", left, IndexNone, __LINE__);
            }

            for (usize right = 0; right < Count; ++right) {
                const bool sameName = interned[left] == interned[right];
                if (NameTextEquals(texts[left], texts[right]) != sameName) {
                    RecordPairFailure("NameTextEquals differs from intern ==", left, right,
                                      __LINE__);
                }

                if ((keys[left] == keys[right]) != sameName) {
                    RecordPairFailure("NameKey equality differs from intern ==", left, right,
                                      __LINE__);
                }
            }
        }

        // 부분 pool: 짝수 색인 원문만 intern한다. 홀수 색인의 대소문자 변형은 첫 표시 경로를 탄다.
        NamePool partial(allocator);
        REQUIRE(partial.Initialize(config));
        Name partialInterned[Count];
        for (usize index = 0; index < Count; index += 2) {
            auto name = partial.TryIntern(texts[index]);
            REQUIRE(name);
            partialInterned[index] = name.Value();
        }

        Name found[Count];
        bool present[Count]{};
        for (usize left = 0; left < Count; ++left) {
            auto result = partial.TryFind(texts[left]);
            present[left] = static_cast<bool>(result);
            if (result) {
                found[left] = result.Value();
            } else if (result.Error() != ErrorCode::NotFound) {
                RecordPairFailure("partial TryFind error is not NotFound", left, IndexNone,
                                  __LINE__);
            }

            // 찾는 조건: None이거나, intern한 원문 가운데 같은 Name이 있다.
            bool expected = NameTextEquals(texts[left], StringView());
            for (usize right = 0; right < Count; right += 2) {
                expected = expected || NameTextEquals(texts[left], texts[right]);
            }

            if (present[left] != expected) {
                RecordPairFailure("partial TryFind presence", left, IndexNone, __LINE__);
            }

            if (present[left] && !Readable(partial, found[left])) {
                RecordPairFailure("partial TryFind token is not readable", left, IndexNone,
                                  __LINE__);
            }

            if (present[left] && left % 2 == 0 && !SameFields(found[left], partialInterned[left])) {
                RecordPairFailure("partial TryFind exact display", left, IndexNone, __LINE__);
            }
        }

        for (usize left = 0; left < Count; ++left) {
            for (usize right = 0; right < Count; ++right) {
                if (!present[left]) {
                    continue;
                }

                const bool sameText = NameTextEquals(texts[left], texts[right]);
                if (present[right] && (found[left] == found[right]) != sameText) {
                    RecordPairFailure("partial TryFind == differs from NameTextEquals", left, right,
                                      __LINE__);
                }

                if (right % 2 == 0 && (found[left] == partialInterned[right]) != sameText) {
                    RecordPairFailure("partial TryFind differs from TryIntern", left, right,
                                      __LINE__);
                }
            }
        }

        REQUIRE(partial.Shutdown());
        REQUIRE(full.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameTextSplit_FollowsUe58NumberBounds) {
    struct Case {
        StringView input;
        StringView base;
        int64 number;
    };
    const Case cases[] = {
        {"Rocket", "Rocket", Name::NoNumber},
        {"Rocket_0", "Rocket", 0},
        {"Rocket_19", "Rocket", 19},
        {"Rocket_2147483646", "Rocket", 2147483646},
        {"Rocket_2147483647", "Rocket_2147483647", Name::NoNumber}, // int32 최댓값은 번호가 아니다
        {"Rocket_4294967296", "Rocket_4294967296", Name::NoNumber},
        {"Rocket_9999999999", "Rocket_9999999999", Name::NoNumber},   // 10자리지만 범위 밖
        {"Rocket_10000000000", "Rocket_10000000000", Name::NoNumber}, // 11자리
        {"Rocket_04", "Rocket_04", Name::NoNumber},                   // 선행 0
        {"Rocket_00", "Rocket_00", Name::NoNumber},
        {"Rocket_0000000001", "Rocket_0000000001", Name::NoNumber},
        {"Rocket_-1", "Rocket_-1", Name::NoNumber},
        {"Rocket_", "Rocket_", Name::NoNumber},
        {"Rocket7", "Rocket7", Name::NoNumber},
        {"Rocket__7", "Rocket_", 7},
        {"a_1_2", "a_1", 2},
        {"7", "7", Name::NoNumber},
        {"_7", "", 7}, // 문법상 빈 base·번호 7(Name으로는 기본 None)
        {"_", "_", Name::NoNumber},
        {"", "", Name::NoNumber},
        {"None_3", "None", 3},
        {"\xC3\x9C_3", "\xC3\x9C", 3},
        {"\xEA\xB0\x80\xEA\xB0\x80", "\xEA\xB0\x80\xEA\xB0\x80", Name::NoNumber},
        {StringView("a\0_5", 4), StringView("a\0", 2), 5},
        {StringView("a_5\0", 4), StringView("a_5\0", 4), Name::NoNumber},
    };
    for (const Case& sample : cases) {
        const bool numbered = sample.number != Name::NoNumber;
        const NameTextParts parts = SplitNameText(sample.input);
        CHECK(parts.base == sample.base);
        CHECK(parts.GetNumber() == sample.number);
        CHECK(parts.HasNumber() == numbered);
        CHECK(parts.number == (numbered ? static_cast<uint32>(sample.number + 1) : 0u));
        // base는 입력의 앞부분을 가리킨다(복사하지 않는다).
        CHECK(parts.base.IsEmpty() || parts.base.Data() == sample.input.Data());
    }

    // intern한 이름의 번호와 lease BaseView도 같은 표(UE 경계값)를 따른다. 빈 base의 번호 이름은
    // None이 되므로 뺀다.
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        for (const Case& sample : cases) {
            if (sample.base.IsEmpty()) {
                continue;
            }

            auto name = pool.TryIntern(sample.input);
            REQUIRE(name);
            CHECK(name.Value().GetNumber() == sample.number);
            auto lease = pool.Acquire(name.Value());
            REQUIRE(lease);
            CHECK(lease.Value().View() == sample.input);
            CHECK(lease.Value().BaseView() == sample.base);
        }

        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
