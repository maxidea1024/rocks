#include <ReflectionTestFixtures/FunctionFixture.generated.h>
#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/Event.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 이벤트 시험이다(Docs/Design/EventReflection.ko.md EV1b — ED1·ED2·ED4·ED6–ED8·ED13): 지연 상태
// (구독 없으면 할당 없음, 16 byte), native 구독과 리플렉션 바인딩의 섞인 등록 순서, 바인딩 거부
// (같은 쌍·서명), 대상 파괴·회수 뒤 자동 해제(발행·Compact·TryBind 분할 상환), 파괴 요청된 발행자,
// 중첩 발행, 발행 중 추가·제거(실행 중인 자기 바인딩 포함)·발행자 파괴, UnbindAll·Clear, 할당
// 실패(상태·항목·frame 인자 복사).
using namespace rocks;
using reflection_fixture::CallPoint;
using reflection_fixture::CallTarget;

namespace event_tests_file {

namespace {

Guid IdOf(TypeRegistry& types, const char* name) noexcept {
    auto type = types.Acquire(TypeIdOf<CallTarget>());
    if (!type) {
        return Guid{};
    }

    for (const FunctionSchema& function : type.Value().Schema().functions) {
        if (StringView(function.name.Data(), function.name.Size()) == StringView(name)) {
            return function.id;
        }
    }

    return Guid{};
}

int32 TotalOf(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    auto borrow = registry.Resolve<CallTarget>(registry.OwnerAccess(), handle);
    if (!borrow) {
        return -1;
    }

    auto target = borrow.Value().Get();
    return target ? target.Value()->total : -1;
}

ErrorCode BroadcastError(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <class T>
ErrorCode ResultError(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 대상의 Relay hook을 바꾼다(발행 밖에서 — 발행 중엔 thunk가 대상을 쓰기로 빌린다).
template <class Callable>
bool SetRelay(ObjectRegistry& registry, ObjectHandle handle, Callable&& callable) noexcept {
    auto borrow = registry.Resolve<CallTarget>(registry.OwnerAccess(), handle);
    if (!borrow) {
        return false;
    }

    auto target = borrow.Value().Get();
    if (!target) {
        return false;
    }

    target.Value()->relay = Delegate<void()>::CreateLambda(Forward<Callable>(callable));
    return true;
}

// 시험 하나의 Registry·대상 객체 셋이다(발행자 publisher와 리스너 a·b).
struct Fixture {
    explicit Fixture(Allocator& allocator) noexcept
        : types(allocator), registry(allocator, types, {0x45564e54, 1}) {
    }

    bool Start() noexcept {
        if (!GeneratedType<CallTarget>::Register(types)) {
            return false;
        }

        auto made = [&](RootRef<CallTarget>& out) noexcept {
            auto created = registry.NewObject<CallTarget>(access, TypeIdOf<CallTarget>(), {}, {});
            if (!created) {
                return false;
            }

            out = Move(created.Value());
            return true;
        };
        return made(publisher) && made(a) && made(b);
    }

    TypeRegistry types;
    ObjectRegistry registry;
    ObjectAccessContext access = registry.OwnerAccess();
    RootRef<CallTarget> publisher;
    RootRef<CallTarget> a;
    RootRef<CallTarget> b;
};

} // namespace

static_assert(sizeof(Event<int32>) == 2 * sizeof(void*),
              "an event field is an allocator and a state");

// 인자 규칙(ED1): nothrow 복사 값·복사 가능한 형식의 const T&만, StringView·비const 참조·rvalue
// 참조·복사할 수 없는 값(String — const String&로)은 아니다.
static_assert(detail::IsEventArgument<int32>());
static_assert(detail::IsEventArgument<const String&>());
static_assert(detail::IsEventArgument<const CallPoint&>());
static_assert(detail::IsEventArgument<WeakObjectRef<CallTarget>>());
static_assert(detail::IsEventArgument<ObjectHandle>());
static_assert(!detail::IsEventArgument<StringView>());
static_assert(!detail::IsEventArgument<const StringView&>());
static_assert(!detail::IsEventArgument<int32&>());
static_assert(!detail::IsEventArgument<const int32&&>());
static_assert(!detail::IsEventArgument<String>());

TEST(Event_LazyStateAndMixedOrder) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        const EventContext context{fixture.registry, fixture.access, fixture.publisher.Handle()};
        const Guid add = IdOf(fixture.types, "Add");

        // 구독이 없으면 상태가 없고 발행은 할당하지 않는다.
        Event<int32> event(allocator);
        const usize before = allocator.Stats().liveAllocations;
        REQUIRE(event.TryBroadcast(context, 1));
        CHECK(!event.IsBound() && event.BindingCount() == 0);
        CHECK(allocator.Stats().liveAllocations == before);

        // native → 리플렉션 → native 순서로 등록하면 그 순서로 불린다(앞 native는 0, 뒤 native는
        // 바인딩이 더한 값을 본다).
        int32 seenFirst = -1;
        int32 seenLast = -1;
        ObjectRegistry& registry = fixture.registry;
        const ObjectHandle a = fixture.a.Handle();
        REQUIRE(event.TryAddLambda([&](int32) noexcept { seenFirst = TotalOf(registry, a); }));
        REQUIRE(event.TryBind(registry, fixture.access, a, add));
        REQUIRE(event.TryAddLambda([&](int32) noexcept { seenLast = TotalOf(registry, a); }));
        CHECK(event.IsBound() && event.BindingCount() == 1);
        REQUIRE(event.TryBroadcast(context, 5));
        CHECK(seenFirst == 0 && seenLast == 5 && TotalOf(registry, a) == 5);

        // 같은 (대상, 함수)는 AlreadyExists, 서명이 다르면 TypeMismatch, 함수 없음은 NotFound.
        auto again = event.TryBind(registry, fixture.access, a, add);
        CHECK(!again && again.Error() == ErrorCode::AlreadyExists);
        auto note = event.TryBind(registry, fixture.access, a, IdOf(fixture.types, "Note"));
        CHECK(!note && note.Error() == ErrorCode::TypeMismatch);
        auto missing = event.TryBind(registry, fixture.access, a, Guid{9, 9});
        CHECK(!missing && missing.Error() == ErrorCode::NotFound);

        // 다른 대상의 같은 함수는 받는다.
        REQUIRE(event.TryBind(registry, fixture.access, fixture.b.Handle(), add));
        REQUIRE(event.TryBroadcast(context, 2));
        CHECK(TotalOf(registry, a) == 7 && TotalOf(registry, fixture.b.Handle()) == 2);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Event_DeadTargetsAreRemoved) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const EventContext context{registry, fixture.access, fixture.publisher.Handle()};
        const Guid add = IdOf(fixture.types, "Add");
        Event<int32> event(allocator);
        REQUIRE(event.TryBind(registry, fixture.access, fixture.a.Handle(), add));
        REQUIRE(event.TryBind(registry, fixture.access, fixture.b.Handle(), add));

        // 파괴 요청된 대상: 발행이 Destroyed를 알리고 그 바인딩을 지운다(다른 대상은 계속 불린다).
        REQUIRE(registry.RequestDestroy(fixture.access, fixture.a.Handle()));
        CHECK(BroadcastError(event.TryBroadcast(context, 3)) == ErrorCode::Destroyed);
        CHECK(TotalOf(registry, fixture.b.Handle()) == 3);
        CHECK(event.BindingCount() == 1);
        REQUIRE(event.TryBroadcast(context, 3));
        CHECK(TotalOf(registry, fixture.b.Handle()) == 6);

        // 회수된 대상: 발행 없이 Compact가 지운다.
        const ObjectHandle b = fixture.b.Handle();
        REQUIRE(registry.RequestDestroy(fixture.access, b));
        fixture.b.Reset();
        fixture.a.Reset();
        REQUIRE(registry.Collect(fixture.access));
        CHECK(event.Compact(registry, fixture.access) == 1);
        CHECK(event.BindingCount() == 0 && !event.IsBound());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 한 번도 발행하지 않아도 TryBind가 죽은 바인딩을 분할 상환으로 정리한다(ED7).
TEST(Event_TryBindCompactsWithoutBroadcast) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const Guid add = IdOf(fixture.types, "Add");
        Event<int32> event(allocator);
        constexpr int Rounds = 64;
        for (int round = 0; round < Rounds; ++round) {
            auto created =
                registry.NewObject<CallTarget>(fixture.access, TypeIdOf<CallTarget>(), {}, {});
            REQUIRE(created);
            REQUIRE(event.TryBind(registry, fixture.access, created.Value().Handle(), add));
            REQUIRE(registry.RequestDestroy(fixture.access, created.Value().Handle()));
            CHECK(event.BindingCount() <= 2 * 1 + MulticastCompactSlack + 1);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Event_PublisherNestedAndRemovalDuringBroadcast) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        const EventContext context{registry, access, fixture.publisher.Handle()};
        const Guid add = IdOf(fixture.types, "Add");
        const ObjectHandle a = fixture.a.Handle();
        const ObjectHandle b = fixture.b.Handle();

        // 중첩 발행: 첫 native 구독이 한 번 같은 이벤트를 다시 발행한다 — 안쪽 바인딩은 안쪽
        // 인자(10), 바깥 바인딩은 바깥 인자(1)를 받는다.
        Event<int32> event(allocator);
        bool nested = false;
        REQUIRE(event.TryAddLambda([&](int32) noexcept {
            if (!nested) {
                nested = true;
                event.Broadcast(context, 10);
            }
        }));
        REQUIRE(event.TryBind(registry, access, a, add));
        REQUIRE(event.TryBroadcast(context, 1));
        CHECK(TotalOf(registry, a) == 11);

        // 발행 중 제거: 앞의 native 구독이 아직 불리지 않은 바인딩을 지우면 그 발행에서 불리지
        // 않는다.
        Event<int32> removal(allocator);
        DelegateHandle later;
        REQUIRE(removal.TryAddLambda([&](int32) noexcept { CHECK(removal.Remove(later)); }));
        auto bound = removal.TryBind(registry, access, b, add);
        REQUIRE(bound);
        later = bound.Value();
        REQUIRE(removal.TryBroadcast(context, 4));
        CHECK(TotalOf(registry, b) == 0 && removal.BindingCount() == 0);

        // UnbindAll(대상)·Clear.
        REQUIRE(removal.TryBind(registry, access, a, add));
        REQUIRE(removal.TryBind(registry, access, b, add));
        CHECK(removal.UnbindAll(a) == 1 && removal.BindingCount() == 1);
        removal.Clear();
        CHECK(!removal.IsBound() && removal.BindingCount() == 0);

        // 다른 문맥·파괴 요청된 발행자는 아무것도 부르지 않는다.
        CHECK(BroadcastError(
                  event.TryBroadcast(EventContext{registry, ObjectAccessContext{{0x9999, 1}},
                                                  fixture.publisher.Handle()},
                                     1)) == ErrorCode::WrongContext);
        CHECK(TotalOf(registry, a) == 11);
        REQUIRE(registry.RequestDestroy(access, fixture.publisher.Handle()));
        CHECK(BroadcastError(event.TryBroadcast(context, 1)) == ErrorCode::Destroyed);
        CHECK(TotalOf(registry, a) == 11);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// const String& 인자: native 구독은 발행자의 값을, 리플렉션 바인딩은 frame의 깊은 복사를 받는다.
// frame 인자 복사가 실패하면 리플렉션 바인딩은 불리지 않고 첫 실패가 OutOfMemory이며 native
// 구독은 그대로 불린다.
TEST(Event_StringArgumentAndFrameFailure) {
    SystemAllocator allocator;
    {
        test::NFailAllocator failing(allocator);
        Fixture fixture(failing);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const EventContext context{registry, fixture.access, fixture.publisher.Handle()};
        const ObjectHandle a = fixture.a.Handle();
        Event<const String&> event(allocator);
        usize seen = 0;
        REQUIRE(event.TryAddLambda(
            [&](const String& text) noexcept { seen += text.View().ByteCount(); }));
        REQUIRE(event.TryBind(registry, fixture.access, a, IdOf(fixture.types, "Note")));

        String text(allocator);
        REQUIRE(text.TryAssign("a text long enough to need its own allocation"));
        const int32 bytes = static_cast<int32>(text.View().ByteCount());
        REQUIRE(event.TryBroadcast(context, text));
        CHECK(seen == static_cast<usize>(bytes) && TotalOf(registry, a) == bytes);

        failing.FailOnAttempt(1);
        CHECK(BroadcastError(event.TryBroadcast(context, text)) == ErrorCode::OutOfMemory);
        failing.DisableFailure();
        CHECK(seen == static_cast<usize>(bytes) * 2 && TotalOf(registry, a) == bytes);
        CHECK(event.BindingCount() == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 발행자 자신의 함수에 바인딩해도 발행이 된다(발행자는 Borrow가 아니라 root로 붙잡는다 — ED6).
// 인자 없는 이벤트는 빈 frame을 쓴다.
TEST(Event_SelfBindingAndNoArguments) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectHandle publisher = fixture.publisher.Handle();
        const EventContext context{registry, fixture.access, publisher};
        Event<> event(allocator);
        int32 natives = 0;
        REQUIRE(event.TryAddLambda([&]() noexcept { ++natives; }));
        REQUIRE(event.TryBind(registry, fixture.access, publisher, IdOf(fixture.types, "Bump")));
        REQUIRE(event.TryBroadcast(context));
        REQUIRE(event.TryBroadcast(context));
        CHECK(natives == 2 && TotalOf(registry, publisher) == 200);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 발행 중 바인딩: native 구독과 실행 중인 리플렉션 함수가 더한 바인딩은 그 발행에서 불리지 않고
// 다음 발행부터 불린다(레코드 배열이 커져 옮겨져도 실행 중인 바인딩은 영향이 없다).
TEST(Event_BindDuringBroadcast) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        const EventContext context{registry, access, fixture.publisher.Handle()};
        const Guid add = IdOf(fixture.types, "Add");
        const ObjectHandle a = fixture.a.Handle();
        const ObjectHandle b = fixture.b.Handle();
        Event<int32> event(allocator);

        // native 구독이 b의 Add를 한 번 바인딩한다.
        bool boundB = false;
        REQUIRE(event.TryAddLambda([&](int32) noexcept {
            if (!boundB) {
                boundB = true;
                CHECK(event.TryBind(registry, access, b, add));
            }
        }));
        REQUIRE(event.TryBroadcast(context, 3));
        CHECK(TotalOf(registry, b) == 0 && event.BindingCount() == 1);
        REQUIRE(event.TryBroadcast(context, 3));
        CHECK(TotalOf(registry, b) == 3);

        // a의 Relay(리플렉션 바인딩)가 실행 중에 같은 이벤트에 새 대상 여럿을 바인딩한다(레코드
        // 배열이 자라 옮겨진다).
        constexpr int Extra = 16;
        RootRef<CallTarget> extras[Extra];
        for (RootRef<CallTarget>& extra : extras) {
            auto created = registry.NewObject<CallTarget>(access, TypeIdOf<CallTarget>(), {}, {});
            REQUIRE(created);
            extra = Move(created.Value());
        }

        Event<> signal(allocator);
        const Guid bump = IdOf(fixture.types, "Bump");
        bool grew = false;
        REQUIRE(SetRelay(registry, a, [&]() noexcept {
            if (!grew) {
                grew = true;
                for (RootRef<CallTarget>& extra : extras) {
                    CHECK(signal.TryBind(registry, access, extra.Handle(), bump));
                }
            }
        }));
        REQUIRE(signal.TryBind(registry, access, a, IdOf(fixture.types, "Relay")));
        REQUIRE(signal.TryBroadcast(context));
        CHECK(TotalOf(registry, a) == 1 && TotalOf(registry, extras[0].Handle()) == 0);
        CHECK(signal.BindingCount() == 1 + Extra);
        REQUIRE(signal.TryBroadcast(context));
        CHECK(TotalOf(registry, a) == 2 && TotalOf(registry, extras[Extra - 1].Handle()) == 100);
        REQUIRE(SetRelay(registry, a, []() noexcept {}));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 실행 중인 자기 리플렉션 바인딩을 Remove하면(항목 소멸은 Core가 미룬다) 레코드가 죽음 표시되어
// 같은 쌍을 바로 다시 바인딩할 수 있다. 그 발행에서 새 바인딩은 불리지 않는다.
TEST(Event_RemoveExecutingBindingAndRebind) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        const EventContext context{registry, access, fixture.publisher.Handle()};
        const Guid relay = IdOf(fixture.types, "Relay");
        const ObjectHandle a = fixture.a.Handle();
        // hook이 쓰는 상태다(lambda는 이 묶음 하나만 쥔다 — inline 용량).
        struct Rebind {
            Event<>* event;
            ObjectRegistry* registry;
            const ObjectAccessContext* access;
            ObjectHandle target;
            Guid function;
            DelegateHandle current;
            bool done = false;
            ErrorCode error = ErrorCode::None;
        };
        Event<> event(allocator);
        Rebind state{&event, &registry, &access, a, relay, {}};
        REQUIRE(SetRelay(registry, a, [&state]() noexcept {
            if (!state.done) {
                state.done = true;
                CHECK(state.event->Remove(state.current));
                auto again = state.event->TryBind(*state.registry, *state.access, state.target,
                                                  state.function);
                state.error = ResultError(again);
                if (again) {
                    state.current = again.Value();
                }
            }
        }));
        auto bound = event.TryBind(registry, access, a, relay);
        REQUIRE(bound);
        state.current = bound.Value();
        REQUIRE(event.TryBroadcast(context));
        CHECK(state.error == ErrorCode::None);
        CHECK(TotalOf(registry, a) == 1 && event.BindingCount() == 1);
        REQUIRE(event.TryBroadcast(context));
        CHECK(TotalOf(registry, a) == 2);
        CHECK(event.Remove(state.current) && event.BindingCount() == 0);
        REQUIRE(SetRelay(registry, a, []() noexcept {}));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// native 구독이 발행 중 발행자를 파괴 요청하고 Collect해도 발행자는 발행이 끝날 때까지 회수되지
// 않고(발행의 root), 뒤의 바인딩도 불린다. 발행이 끝난 뒤 Collect가 회수한다.
TEST(Event_PublisherDestroyedDuringBroadcast) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        const ObjectHandle publisher = fixture.publisher.Handle();
        const ObjectHandle a = fixture.a.Handle();
        const EventContext context{registry, access, publisher};
        fixture.publisher.Reset();
        Event<int32> event(allocator);
        // native 구독이 불리기 전엔 NotFound다.
        ErrorCode collected = ErrorCode::NotFound;
        ErrorCode lifecycle = ErrorCode::NotFound;
        bool requested = false;
        REQUIRE(event.TryAddLambda([&](int32) noexcept {
            if (requested) {
                return;
            }

            requested = true;
            CHECK(registry.RequestDestroy(access, publisher));
            collected = ResultError(registry.Collect(access));
            auto inspected = registry.InspectLifecycle(access, publisher);
            lifecycle = ResultError(inspected);
            CHECK(inspected && inspected.Value() == ObjectLifecycle::DestroyRequested);
        }));
        REQUIRE(event.TryBind(registry, access, a, IdOf(fixture.types, "Add")));
        REQUIRE(event.TryBroadcast(context, 4));
        CHECK(collected == ErrorCode::None && lifecycle == ErrorCode::None);
        CHECK(TotalOf(registry, a) == 4);
        REQUIRE(registry.Collect(access));
        CHECK(ResultError(registry.InspectLifecycle(access, publisher)) == ErrorCode::StaleHandle);
        CHECK(BroadcastError(event.TryBroadcast(context, 4)) == ErrorCode::StaleHandle);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 미해결 레코드(ED24): 발행에 끼지 않고, 나열은 바인딩과 같은 등록 번호 순서로 resolved 거짓으로
// 오며, null 대상·0 FunctionId는 InvalidArgument, 같은 쌍 보존은 AlreadyExists, 같은 쌍 TryBind
// 성공·Unbind·UnbindAll·Compact·Clear가 지운다.
TEST(Event_UnresolvedRecords) {
    struct Visited {
        ObjectHandle target;
        Guid functionId;
        bool resolved;
    };

    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        const EventContext context{registry, access, fixture.publisher.Handle()};
        const Guid add = IdOf(fixture.types, "Add");
        const Guid missing{7, 7};
        const ObjectHandle a = fixture.a.Handle();
        const ObjectHandle b = fixture.b.Handle();
        Event<int32> event(allocator);
        Array<Visited> visited(allocator);
        auto list = [&]() noexcept {
            visited.Clear();
            return event.ForEachBinding(
                [&](ObjectHandle target, Guid functionId, bool resolved) noexcept {
                    return visited.TryPushBack(Visited{target, functionId, resolved});
                });
        };

        // 미해결 레코드만 있으면 발행할 것이 없다(delegate 항목이 없다).
        REQUIRE(event.TryKeepUnresolved(a, missing));
        CHECK(event.UnresolvedCount() == 1 && !event.IsBound() && event.BindingCount() == 0);
        REQUIRE(event.TryBroadcast(context, 4));
        CHECK(TotalOf(registry, a) == 0);
        auto again = event.TryKeepUnresolved(a, missing);
        CHECK(!again && again.Error() == ErrorCode::AlreadyExists);
        CHECK(ResultError(event.TryKeepUnresolved(ObjectHandle{}, missing)) ==
              ErrorCode::InvalidArgument);
        CHECK(ResultError(event.TryKeepUnresolved(a, Guid{})) == ErrorCode::InvalidArgument);

        // 살아 있는 바인딩과 같은 쌍은 보존하지 않는다.
        REQUIRE(event.TryBind(registry, access, b, add));
        auto bound = event.TryKeepUnresolved(b, add);
        CHECK(!bound && bound.Error() == ErrorCode::AlreadyExists);

        // 나열: 바인딩과 미해결 레코드가 등록 번호 순서로 섞인다(a 미해결 → b 바인딩 → b 미해결).
        REQUIRE(event.TryKeepUnresolved(b, missing));
        REQUIRE(list());
        REQUIRE(visited.Size() == 3);
        CHECK(visited[0].target == a && visited[0].functionId == missing && !visited[0].resolved);
        CHECK(visited[1].target == b && visited[1].functionId == add && visited[1].resolved);
        CHECK(visited[2].target == b && visited[2].functionId == missing && !visited[2].resolved);

        // 같은 쌍 TryBind 성공은 그 미해결 레코드를 지운다(a의 Add를 미해결로 둔 뒤 바인딩).
        REQUIRE(event.TryKeepUnresolved(a, add));
        CHECK(event.UnresolvedCount() == 3);
        REQUIRE(event.TryBind(registry, access, a, add));
        CHECK(event.UnresolvedCount() == 2);

        // 바인딩 실패는 미해결 레코드를 건드리지 않는다.
        CHECK(!event.TryBind(registry, access, a, missing));
        CHECK(event.UnresolvedCount() == 2);

        // Unbind(target, fn)은 미해결 레코드만 있어도 참이다.
        CHECK(event.Unbind(a, missing));
        CHECK(event.UnresolvedCount() == 1 && !event.Unbind(a, missing));

        // UnbindAll(target)은 바인딩과 미해결 레코드를 함께 센다.
        CHECK(event.UnbindAll(b) == 2);
        CHECK(event.UnresolvedCount() == 0);

        // Compact는 대상이 사라진 미해결 레코드도 지운다(살아 있는 대상의 것은 둔다).
        REQUIRE(event.TryKeepUnresolved(a, missing));
        REQUIRE(event.TryKeepUnresolved(b, missing));
        REQUIRE(registry.RequestDestroy(access, b));
        CHECK(event.Compact(registry, access) == 1);
        CHECK(event.UnresolvedCount() == 1);

        // Clear는 전부 지운다.
        event.Clear();
        CHECK(event.UnresolvedCount() == 0 && event.BindingCount() == 0);
        REQUIRE(list());
        CHECK(visited.Size() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 상태·레코드·delegate 항목 할당이 몇 번째에서 실패해도 그 오류를 돌려주고 아무것도 남기지 않는다.
TEST(Event_AllocationFailureLeavesNothing) {
    SystemAllocator allocator;
    {
        test::NFailAllocator failing(allocator);
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const EventContext context{registry, fixture.access, fixture.publisher.Handle()};
        const Guid add = IdOf(fixture.types, "Add");
        constexpr uint64 MaxAttempts = 16;

        bool boundOnce = false;
        for (uint64 attempt = 1; attempt <= MaxAttempts && !boundOnce; ++attempt) {
            Event<int32> event(failing);
            failing.FailOnAttempt(attempt);
            auto bound = event.TryBind(registry, fixture.access, fixture.a.Handle(), add);
            failing.DisableFailure();
            if (bound) {
                boundOnce = true;
                CHECK(event.BindingCount() == 1 && event.IsBound());
                REQUIRE(event.TryBroadcast(context, 1));
                break;
            }

            CHECK(bound.Error() == ErrorCode::OutOfMemory);
            CHECK(event.BindingCount() == 0 && !event.IsBound());
            REQUIRE(event.TryBroadcast(context, 1));
        }
        CHECK(boundOnce);

        bool addedOnce = false;
        for (uint64 attempt = 1; attempt <= MaxAttempts && !addedOnce; ++attempt) {
            Event<int32> event(failing);
            int32 calls = 0;
            failing.FailOnAttempt(attempt);
            auto added = event.TryAddLambda([&](int32) noexcept { ++calls; });
            failing.DisableFailure();
            if (added) {
                addedOnce = true;
                REQUIRE(event.TryBroadcast(context, 1));
                CHECK(calls == 1);
                break;
            }

            CHECK(added.Error() == ErrorCode::OutOfMemory && !event.IsBound());
            REQUIRE(event.TryBroadcast(context, 1));
            CHECK(calls == 0);
        }
        CHECK(addedOnce);
        CHECK(TotalOf(registry, fixture.a.Handle()) == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace event_tests_file
