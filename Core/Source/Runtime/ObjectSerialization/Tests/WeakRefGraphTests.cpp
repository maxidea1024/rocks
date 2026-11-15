#include "WeakRefGraphTestSupport.h"

// 리플렉션 B9d1 WeakRef PROPERTY의 그래프 저장 시험이다(Docs/Design/ReflectionExtensions.ko.md
// 5.1절, 17절 B9d 항목): MetaCompiler 생성 fixture(WeakRefFixture.h)로 weak 클래스 필드·struct
// 멤버·Array<WeakRef> 원소를 같은 그래프 안에서 왕복하고, 저장 집합 밖 weak 대상을 정책별(Reject·
// Admit·Include 지연 덮어쓰기)로 처리하며, 죽은 대상은 진단 없이 null로 쓰고 산 대상의 형식 위반은
// 거부함을 본다. 복원·GC·도달 검사는 WeakRefGraphLoadTests.cpp, 쓰기 검증은
// WeakRefFieldTests.cpp다.
using namespace rocks;
using namespace rocks::test::weak;

namespace {

// 저장 집합 set(NodeTable 포함)을 policy와 hooks로 저장한다.
Result<ObjectGraphSaveResult> SaveSet(Allocator& allocator, ObjectRegistry& objects,
                                      Span<const ObjectHandle> set, GraphExternalPolicy policy,
                                      FieldCodecHooks* hooks,
                                      ObjectGraphLimits limits = {}) noexcept {
    ObjectGraphSaveOptions options;
    options.limits = limits;
    options.externals = policy;
    options.hooks = hooks;
    options.writeNodeTable = true;
    return SaveObjectGraphSet(allocator, objects, objects.OwnerAccess(), set, options);
}

} // namespace

// 같은 그래프 안 weak 값(클래스 필드·Object 기대 필드의 자기 참조·struct 멤버·Array 원소와 null
// 원소·원소 struct 안 멤버·서로를 가리키는 weak 순환)이 노드 번호로 왕복하고, 복원한 weak는 새
// 노드 객체를 가리키며(TryPin 됨) 다시 저장한 byte가 같다.
TEST(WeakRefGraph_RoundTripInsideTheGraph) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x57524b, 1});
        ObjectRegistry target(allocator, types, {0x57524b, 2});
        {
            RootRef<WeakHolder> a = New<WeakHolder>(source);
            RootRef<WeakHolder> b = New<WeakHolder>(source);
            const ObjectHandle ha = a.Handle();
            const ObjectHandle hb = b.Handle();
            SetStrong(source, ha, &WeakHolder::strong, hb);
            With(source, ha, [&](WeakHolder& value) noexcept {
                value.value = 1;
                value.target = WeakRef<WeakTarget>(hb);
                value.any = WeakRef<>(ha);
                value.link.target = WeakRef<WeakTarget>(hb);
                value.link.slot = 4;
                REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(hb)));
                REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>()));
                REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(ha)));
                auto link = value.group.links.TryEmplaceBack();
                REQUIRE(link);
                link.Value()->target = WeakRef<WeakTarget>(hb);
                link.Value()->slot = 9;
            });
            With(source, hb, [&](WeakHolder& value) noexcept {
                value.value = 2;
                value.target = WeakRef<WeakTarget>(ha);
            });

            Recorder hooks;
            ObjectGraphSaveOptions options;
            options.hooks = &hooks;
            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), ha, options);
            REQUIRE(saved);
            CHECK(hooks.count == 0);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            const ObjectHandle la = loaded.Value().Handle();
            ObjectHandle lb{};
            With(target, la, [&](WeakHolder& value) noexcept {
                lb = value.strong.Handle();
                CHECK(lb != ObjectHandle{} && lb != la);
                CHECK(value.value == 1);
                CHECK(value.target.Handle() == lb);
                CHECK(value.any.Handle() == la);
                CHECK(value.link.target.Handle() == lb && value.link.slot == 4);
                REQUIRE(value.targets.Size() == 3);
                CHECK(value.targets[0].Handle() == lb);
                CHECK(value.targets[1].Handle() == ObjectHandle{});
                CHECK(value.targets[2].Handle() == la);
                REQUIRE(value.group.links.Size() == 1);
                CHECK(value.group.links[0].target.Handle() == lb);
                CHECK(value.group.links[0].slot == 9);
                auto pinned = value.target.TryPin(target, target.OwnerAccess());
                CHECK(pinned);
            });
            With(target, lb, [&](WeakHolder& value) noexcept {
                CHECK(value.value == 2);
                CHECK(value.target.Handle() == la);
            });

            auto resaved = SaveObjectGraph(allocator, target, target.OwnerAccess(), la);
            REQUIRE(resaved);
            CHECK(test::graph::SameBytes(saved.Value().View(), resaved.Value().View()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// Reject 정책: 저장 집합 밖 weak 대상은 null로 쓰고 위치마다 ExternalReference 진단(값 2)을 남기며
// 저장은 성공한다(강한 참조만 거부 대상 — 같은 대상의 강한 참조는 값 0 진단 뒤 InvalidArgument).
TEST(WeakRefGraph_RejectPolicyWritesNullAndReports) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x57524b, 3});
        {
            RootRef<WeakHolder> a = New<WeakHolder>(objects);
            RootRef<WeakHolder> b = New<WeakHolder>(objects);
            const ObjectHandle ha = a.Handle();
            const ObjectHandle hb = b.Handle();
            With(objects, ha, [&](WeakHolder& value) noexcept {
                value.target = WeakRef<WeakTarget>(hb);
                value.link.target = WeakRef<WeakTarget>(hb);
                REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(hb)));
                REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(ha)));
            });

            const ObjectHandle set[] = {ha};
            Recorder hooks;
            auto saved = SaveSet(allocator, objects, set, GraphExternalPolicy::Reject, &hooks);
            REQUIRE(saved);
            CHECK(hooks.count == 3);
            CHECK(hooks.Count(FieldDiagnosticKind::ExternalReference, 2) == 3);
            CHECK(hooks.HasWeakNull(0, HolderField("target"), false, 0));
            CHECK(hooks.HasWeakNull(0, HolderField("targets"), true, 0));
            CHECK(hooks.HasWeakNull(0, HolderField("link"), false, 0,
                                    SchemaField(TypeOf<WeakLink>(), "target").id));
            CHECK(saved.Value().Externals().Size() == 0);

            ObjectRegistry target(allocator, types, {0x57524b, 4});
            {
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              saved.Value().Bytes());
                REQUIRE(loaded);
                const ObjectHandle la = loaded.Value().Handle();
                With(target, la, [&](WeakHolder& value) noexcept {
                    CHECK(value.target.Handle() == ObjectHandle{});
                    CHECK(value.link.target.Handle() == ObjectHandle{});
                    REQUIRE(value.targets.Size() == 2);
                    CHECK(value.targets[0].Handle() == ObjectHandle{});
                    CHECK(value.targets[1].Handle() == la);
                });
            }

            REQUIRE(target.Shutdown(target.OwnerAccess()));

            // 같은 대상의 강한 참조는 거부된다(값 0).
            SetStrong(objects, ha, &WeakHolder::strong, hb);
            hooks.Clear();
            auto rejected = SaveSet(allocator, objects, set, GraphExternalPolicy::Reject, &hooks);
            CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
            CHECK(hooks.Count(FieldDiagnosticKind::ExternalReference, 0) == 1);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// Admit 정책: weak는 slot 캐시(앞 노드의 강한 참조가 Import한 대상 포함)를 쓰지 않고 출현마다
// weak = true·slot 0·Required 아님으로 묻는다. Import·Null 모두 null + 진단(값 2 — weak import는
// 없다)이고 외부 참조로 집계하지 않으며, Reject는 진단(값 0) 뒤 InvalidArgument다.
TEST(WeakRefGraph_AdmitAsksEveryWeakOccurrenceWithoutSlotCache) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x57524b, 5});
        {
            RootRef<WeakHolder> c = New<WeakHolder>(objects);
            RootRef<WeakHolder> a = New<WeakHolder>(objects);
            RootRef<WeakHolder> b = New<WeakHolder>(objects);
            const ObjectHandle hb = b.Handle();
            // 노드 1(c)의 강한 참조가 b를 먼저 Import한다(second는 편의 복원의 도달 검사용).
            SetStrong(objects, c.Handle(), &WeakHolder::strong, hb);
            SetStrong(objects, c.Handle(), &WeakHolder::second, a.Handle());
            With(objects, a.Handle(), [&](WeakHolder& value) noexcept {
                value.target = WeakRef<WeakTarget>(hb);
                value.link.target = WeakRef<WeakTarget>(hb);
                REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(hb)));
                REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(hb)));
            });

            const ObjectHandle set[] = {c.Handle(), a.Handle()};
            const ExternalAdmission answers[] = {ExternalAdmission::Import,
                                                 ExternalAdmission::Null};
            for (ExternalAdmission answer : answers) {
                Recorder hooks;
                hooks.weakAnswer = answer;
                auto saved = SaveSet(allocator, objects, set, GraphExternalPolicy::Admit, &hooks);
                REQUIRE(saved);
                REQUIRE(hooks.askedCount == 5);
                CHECK(!hooks.asked[0].weak && hooks.asked[0].target == hb &&
                      hooks.asked[0].slot == 1);
                CHECK(hooks.WeakAsked() == 4);
                for (uint32 index = 1; index < hooks.askedCount; ++index) {
                    CHECK(hooks.asked[index].weak && hooks.asked[index].target == hb);
                    CHECK(hooks.asked[index].slot == 0 && !hooks.asked[index].required);
                    CHECK(hooks.asked[index].expectedType == TypeIdOf<WeakTarget>());
                }

                CHECK(hooks.Count(FieldDiagnosticKind::ExternalReference, 2) == 4);
                CHECK(hooks.HasWeakNull(1, HolderField("targets"), true, 1));
                REQUIRE(saved.Value().Externals().Size() == 1);
                CHECK(saved.Value().Externals()[0].occurrences == 1);

                ObjectRegistry target(allocator, types, {0x57524b, 6});
                {
                    // 외부 대상(b)은 대상 Registry의 같은 타입 객체로 준다.
                    RootRef<WeakHolder> import = New<WeakHolder>(target);
                    const ObjectHandle imports[] = {import.Handle()};
                    ObjectGraphLoadOptions options;
                    options.imports = imports;
                    auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                                  saved.Value().Bytes(), options);
                    REQUIRE(loaded);
                    With(target, loaded.Value().Handle(), [&](WeakHolder& value) noexcept {
                        CHECK(value.strong.Handle() == import.Handle());
                    });
                }

                REQUIRE(target.Shutdown(target.OwnerAccess()));
            }

            Recorder rejecting;
            rejecting.weakAnswer = ExternalAdmission::Reject;
            auto rejected =
                SaveSet(allocator, objects, set, GraphExternalPolicy::Admit, &rejecting);
            CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
            CHECK(rejecting.WeakAsked() == 1);
            CHECK(rejecting.Count(FieldDiagnosticKind::ExternalReference, 0) == 1);

            // hook이 없으면 Admit은 모두 Reject다.
            auto noHook = SaveSet(allocator, objects, set, GraphExternalPolicy::Admit, nullptr);
            CHECK(!noHook && noHook.Error() == ErrorCode::InvalidArgument);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// Include 정책: encode 때 집합 밖이던 weak 위치를 기록했다가 모든 노드를 encode한 뒤 최종 집합으로
// 제자리에 덮어쓴다 — 뒤 노드의 강한 참조가 대상을 더하면 그 local ID, 끝까지 밖이면 0과 기록
// 순서대로의 진단(값 2 — 기록할 때의 objectIndex·경로)이다. weak는 집합을 넓히지 않는다. 노드
// 순서를 바꾼 저장 집합도 같은 연결로 복원된다.
TEST(WeakRefGraph_IncludeOverwritesDeferredWeakIndependentOfNodeOrder) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x57524b, 7});
        {
            RootRef<WeakHolder> a = New<WeakHolder>(objects);
            RootRef<WeakHolder> b = New<WeakHolder>(objects);
            RootRef<WeakHolder> c = New<WeakHolder>(objects);
            RootRef<WeakHolder> d = New<WeakHolder>(objects);
            const ObjectHandle ha = a.Handle();
            const ObjectHandle hb = b.Handle();
            const ObjectHandle hc = c.Handle();
            const ObjectHandle hd = d.Handle();
            SetStrong(objects, ha, &WeakHolder::strong, hb);
            SetStrong(objects, hb, &WeakHolder::strong, hc);
            With(objects, ha, [&](WeakHolder& value) noexcept {
                value.value = 1;
                value.target = WeakRef<WeakTarget>(hc);
                REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(hd)));
                REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(hc)));
            });
            With(objects, hb, [&](WeakHolder& value) noexcept {
                value.value = 2;
                value.target = WeakRef<WeakTarget>(hc);
                value.link.target = WeakRef<WeakTarget>(hd);
            });
            With(objects, hc, [&](WeakHolder& value) noexcept {
                value.value = 3;
                value.any = WeakRef<>(ha);
            });

            // 노드 순서: Include 발견 순(a, b, c)과 호출자 순(a, c, b).
            const ObjectHandle discovered[] = {ha};
            const ObjectHandle reordered[] = {ha, hc, hb};
            const Span<const ObjectHandle> sets[] = {Span<const ObjectHandle>(discovered, 1),
                                                     Span<const ObjectHandle>(reordered, 3)};
            const Guid linkTarget = SchemaField(TypeOf<WeakLink>(), "target").id;
            for (usize round = 0; round < 2; ++round) {
                Recorder hooks;
                auto saved =
                    SaveSet(allocator, objects, sets[round], GraphExternalPolicy::Include, &hooks);
                REQUIRE(saved);
                REQUIRE(saved.Value().Nodes().Size() == 3);
                const uint32 bIndex = saved.Value().Nodes()[1] == hb ? 1 : 2;
                // d는 끝까지 집합 밖이다: a의 targets[0]과 b의 link.target이 기록 순서(노드
                // 순서)로 진단된다.
                REQUIRE(hooks.count == 2);
                CHECK(hooks.Count(FieldDiagnosticKind::ExternalReference, 2) == 2);
                CHECK(hooks.entries[0].objectIndex == 0);
                CHECK(hooks.entries[0].objectType == TypeIdOf<WeakHolder>());
                CHECK(hooks.HasWeakNull(0, HolderField("targets"), true, 0));
                CHECK(hooks.entries[1].objectIndex == bIndex);
                CHECK(hooks.HasWeakNull(bIndex, HolderField("link"), false, 0, linkTarget));

                ObjectRegistry target(allocator, types, {0x57524b, 8 + round});
                {
                    auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                                  saved.Value().Bytes());
                    REQUIRE(loaded);
                    const ObjectHandle la = loaded.Value().Handle();
                    ObjectHandle lb{};
                    ObjectHandle lc{};
                    With(target, la,
                         [&](WeakHolder& value) noexcept { lb = value.strong.Handle(); });
                    With(target, lb, [&](WeakHolder& value) noexcept {
                        lc = value.strong.Handle();
                        CHECK(value.value == 2);
                        CHECK(value.link.target.Handle() == ObjectHandle{});
                    });
                    With(target, la, [&](WeakHolder& value) noexcept {
                        CHECK(value.target.Handle() == lc);
                        REQUIRE(value.targets.Size() == 2);
                        CHECK(value.targets[0].Handle() == ObjectHandle{});
                        CHECK(value.targets[1].Handle() == lc);
                    });
                    With(target, lb,
                         [&](WeakHolder& value) noexcept { CHECK(value.target.Handle() == lc); });
                    With(target, lc, [&](WeakHolder& value) noexcept {
                        CHECK(value.value == 3);
                        CHECK(value.any.Handle() == la);
                    });
                }

                REQUIRE(target.Shutdown(target.OwnerAccess()));
            }

            // weak만 c를 가리키면 c는 집합에 들지 않는다(weak는 집합을 넓히지 않는다).
            SetStrong(objects, hb, &WeakHolder::strong, ObjectHandle{});
            Recorder hooks;
            auto narrowed =
                SaveSet(allocator, objects, discovered, GraphExternalPolicy::Include, &hooks);
            REQUIRE(narrowed);
            CHECK(narrowed.Value().Nodes().Size() == 2);
            CHECK(hooks.Count(FieldDiagnosticKind::ExternalReference, 2) == 5);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

namespace {

// weak 원소 count개(모두 집합 밖 대상 outside, 또는 null)를 가진 holder 하나를 Include로 저장할 때
// 성공하는 가장 작은 maxBytes(searchBytes일 때만 — 아니면 0)·maxWork와 출력이다(이분 탐색).
struct Minimums {
    usize bytes = 0;
    uint64 work = 0;
    Array<uint8> output;
};

Minimums MinimumsFor(Allocator& allocator, ObjectRegistry& objects, ObjectHandle holder,
                     ObjectHandle outside, uint32 count, bool searchBytes = true) noexcept {
    With(objects, holder, [&](WeakHolder& value) noexcept {
        value.targets.Clear();
        for (uint32 index = 0; index < count; ++index) {
            REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(outside)));
        }
    });

    const ObjectHandle set[] = {holder};
    const auto saves = [&](ObjectGraphLimits limits) noexcept {
        return SaveSet(allocator, objects, set, GraphExternalPolicy::Include, nullptr, limits)
            .IsOk();
    };

    Minimums result{0, 0, Array<uint8>(allocator)};
    usize lowBytes = 0;
    usize highBytes = searchBytes ? ObjectGraphLimits{}.maxBytes : 0;
    while (lowBytes + 1 < highBytes) {
        const usize middle = lowBytes + (highBytes - lowBytes) / 2;
        ObjectGraphLimits limits;
        limits.maxBytes = middle;
        (saves(limits) ? highBytes : lowBytes) = middle;
    }

    uint64 lowWork = 0;
    uint64 highWork = ObjectGraphLimits{}.maxWork;
    while (lowWork + 1 < highWork) {
        const uint64 middle = lowWork + (highWork - lowWork) / 2;
        ObjectGraphLimits limits;
        limits.maxWork = middle;
        (saves(limits) ? highWork : lowWork) = middle;
    }

    auto saved = SaveSet(allocator, objects, set, GraphExternalPolicy::Include, nullptr);
    if (saved) {
        result.output = saved.Value().TakeBytes();
    }

    result.bytes = highBytes;
    result.work = highWork;
    return result;
}

} // namespace

// Include 지연 항목(위치·대상·노드 위치·경로 사본)의 메모리는 외부 참조 위치 표처럼 그래프 바이트
// 누계와 작업량에 청구된다: 집합 밖 weak 원소의 출력 byte는 null 원소와 같지만 필요한 maxBytes가
// 항목 수에 비례해 늘고(같은 경로 깊이라 정확히 선형) maxWork도 는다.
TEST(WeakRefGraph_IncludeDeferralsAreChargedToGraphBytesAndWork) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x57524b, 10});
        {
            RootRef<WeakHolder> holder = New<WeakHolder>(objects);
            RootRef<WeakHolder> outside = New<WeakHolder>(objects);
            const Minimums none =
                MinimumsFor(allocator, objects, holder.Handle(), ObjectHandle{}, 32);
            const Minimums small =
                MinimumsFor(allocator, objects, holder.Handle(), outside.Handle(), 32);
            const Minimums large =
                MinimumsFor(allocator, objects, holder.Handle(), outside.Handle(), 64);
            const Minimums largeNone =
                MinimumsFor(allocator, objects, holder.Handle(), ObjectHandle{}, 64);
            REQUIRE(none.output.Size() != 0 && small.output.Size() != 0);
            CHECK(test::graph::SameBytes(none.output.View(), small.output.View()));
            CHECK(test::graph::SameBytes(largeNone.output.View(), large.output.View()));
            CHECK(small.bytes > none.bytes);
            CHECK(large.bytes - largeNone.bytes == 2 * (small.bytes - none.bytes));
            CHECK(small.work > none.work);
            CHECK(large.work > small.work);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// Include 지연 항목의 작업량은 항목 수에 선형이다(경로 사본 표·항목 표는 기하 성장 — 정확한 크기
// 예약이면 항목마다 표 전체를 옮겨 제곱으로 는다): null 원소 대비 필요한 maxWork 증분이 항목 수를
// 두 배로 할 때마다 두 배 안팎이다(제곱이면 네 배). 기본 한도에서 Reject와 같은 규모(12,000개)도
// Include로 저장된다.
TEST(WeakRefGraph_IncludeDeferralWorkIsLinearInEntries) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x57524b, 13});
        {
            RootRef<WeakHolder> holder = New<WeakHolder>(objects);
            RootRef<WeakHolder> outside = New<WeakHolder>(objects);
            const uint32 counts[] = {1000, 2000, 4000};
            uint64 extra[3] = {};
            for (usize index = 0; index < 3; ++index) {
                const Minimums plain = MinimumsFor(allocator, objects, holder.Handle(),
                                                   ObjectHandle{}, counts[index], false);
                const Minimums deferred = MinimumsFor(allocator, objects, holder.Handle(),
                                                      outside.Handle(), counts[index], false);
                REQUIRE(deferred.work > plain.work);
                extra[index] = deferred.work - plain.work;
            }

            for (usize index = 1; index < 3; ++index) {
                // 두 배 항목의 증분은 1.5배보다 크고 2.5배보다 작다.
                CHECK(2 * extra[index] > 3 * extra[index - 1]);
                CHECK(2 * extra[index] < 5 * extra[index - 1]);
            }

            With(objects, holder.Handle(), [&](WeakHolder& value) noexcept {
                value.targets.Clear();
                for (uint32 index = 0; index < 12000; ++index) {
                    REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(outside.Handle())));
                }
            });
            const ObjectHandle set[] = {holder.Handle()};
            CHECK(SaveSet(allocator, objects, set, GraphExternalPolicy::Reject, nullptr));
            CHECK(SaveSet(allocator, objects, set, GraphExternalPolicy::Include, nullptr));
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 죽은 대상(파괴 요청 — Destroyed, 회수 뒤 — StaleHandle)을 가리키는 weak는 진단·hook 질의 없이
// null로 쓴다(weak의 정상 상태). 산 대상이 기대 타입을 어기면(C++에서 직접 넣은 다른 타입) 저장이
// TypeMismatch다(Object 기대 필드는 받는다).
TEST(WeakRefGraph_DeadTargetsEncodeNullAndTypeMismatchFails) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x57524b, 11});
        const auto access = objects.OwnerAccess();
        {
            RootRef<WeakHolder> a = New<WeakHolder>(objects);
            RootRef<WeakHolder> b = New<WeakHolder>(objects);
            const ObjectHandle ha = a.Handle();
            const ObjectHandle hb = b.Handle();
            With(objects, ha, [&](WeakHolder& value) noexcept {
                value.target = WeakRef<WeakTarget>(hb);
                REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(hb)));
            });

            REQUIRE(objects.RequestDestroy(access, hb));
            const ObjectHandle set[] = {ha};
            const GraphExternalPolicy policies[] = {GraphExternalPolicy::Reject,
                                                    GraphExternalPolicy::Admit,
                                                    GraphExternalPolicy::Include};
            for (GraphExternalPolicy policy : policies) {
                Recorder hooks;
                auto saved = SaveSet(allocator, objects, set, policy, &hooks);
                REQUIRE(saved);
                CHECK(hooks.count == 0 && hooks.askedCount == 0);
                CHECK(saved.Value().Nodes().Size() == 1);
            }

            // 회수 뒤의 stale handle도 같다.
            b.Reset();
            REQUIRE(objects.Collect(access));
            CHECK(!objects.InspectLifecycle(access, hb));
            for (GraphExternalPolicy policy : policies) {
                Recorder hooks;
                auto saved = SaveSet(allocator, objects, set, policy, &hooks);
                REQUIRE(saved);
                CHECK(hooks.count == 0 && hooks.askedCount == 0);
            }

            // 다른 Registry의 객체(C++로 직접 넣은 handle)는 죽은 대상이 아니라 문맥 오류다: 세
            // 정책 모두 진단·hook 질의 없이 WrongContext로 실패한다(조용한 null로 잃지 않는다).
            {
                ObjectRegistry foreign(allocator, types, {0x57524b, 12});
                {
                    RootRef<WeakHolder> stranger = New<WeakHolder>(foreign);
                    With(objects, ha, [&](WeakHolder& value) noexcept {
                        value.targets.Clear();
                        value.target = WeakRef<WeakTarget>(stranger.Handle());
                    });
                    for (GraphExternalPolicy policy : policies) {
                        Recorder hooks;
                        auto saved = SaveSet(allocator, objects, set, policy, &hooks);
                        CHECK(!saved && saved.Error() == ErrorCode::WrongContext);
                        CHECK(hooks.count == 0 && hooks.askedCount == 0);
                    }

                    With(objects, ha, [&](WeakHolder& value) noexcept {
                        value.target = WeakRef<WeakTarget>();
                        REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(stranger.Handle())));
                    });
                    auto element = SaveObjectGraph(allocator, objects, access, ha);
                    CHECK(!element && element.Error() == ErrorCode::WrongContext);
                    With(objects, ha, [](WeakHolder& value) noexcept { value.targets.Clear(); });
                }

                REQUIRE(foreign.Shutdown(foreign.OwnerAccess()));
            }

            // 기대 타입 위반: WeakRef<WeakTarget>에 C++로 직접 넣은 WeakOther.
            RootRef<WeakOther> other = New<WeakOther>(objects);
            With(objects, ha, [&](WeakHolder& value) noexcept {
                value.targets.Clear();
                value.target = WeakRef<WeakTarget>();
                value.any = WeakRef<>(other.Handle());
            });
            Recorder accepted;
            REQUIRE(SaveSet(allocator, objects, set, GraphExternalPolicy::Reject, &accepted));
            CHECK(accepted.Count(FieldDiagnosticKind::ExternalReference, 2) == 1);
            With(objects, ha, [&](WeakHolder& value) noexcept {
                value.target = WeakRef<WeakTarget>(other.Handle());
            });
            auto mismatched =
                SaveSet(allocator, objects, set, GraphExternalPolicy::Reject, nullptr);
            CHECK(!mismatched && mismatched.Error() == ErrorCode::TypeMismatch);
            With(objects, ha, [&](WeakHolder& value) noexcept {
                value.target = WeakRef<WeakTarget>();
                REQUIRE(value.targets.TryPushBack(WeakRef<WeakTarget>(other.Handle())));
            });
            auto element = SaveObjectGraph(allocator, objects, access, ha);
            CHECK(!element && element.Error() == ErrorCode::TypeMismatch);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
