#include <ReflectionTestFixtures/FunctionFixture.generated.h>
#include <ReflectionTestFixtures/NotifyFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/FieldNotifications.h>
#include <Object/FunctionCall.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 실행 중 필드 변경 통지 시험이다(Docs/Design/ChangeNotification.ko.md CN1 — CN-D1–CN-D11):
// 통지 번호 배치(기반 번호 그대로 → 자기 필드 → 자기 함수), 대상·전체 구독과 해제, 대상이 아닌 ID의
// 거부, 구독자 없는 통지, 통지 중 구독 추가·해제·파괴 요청, 깊이 상한, 파괴 요청·GC 회수의 정리,
// 할당 실패, 등록 검사(Notify의 자리 — EditorOnly 필드·Pure 아닌 함수).
using namespace rocks;
using reflection_fixture::NotifyModel;
using reflection_fixture::NotifyModelChild;

namespace field_notification_tests_file {

namespace {

Guid FieldIdOf(TypeRegistry& types, TypeId type, const char* name) noexcept {
    auto lease = types.Acquire(type);
    if (!lease) {
        return Guid{};
    }

    for (const FieldSchema& field : lease.Value().Schema().fields) {
        if (StringView(field.name.Data(), field.name.Size()) == StringView(name)) {
            return field.id;
        }
    }

    return Guid{};
}

Guid FunctionIdOf(TypeRegistry& types, TypeId type, const char* name) noexcept {
    auto function = types.FindFunctionByName(type, name);
    return function ? function.Value().Schema().id : Guid{};
}

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 시험 하나의 Registry와 fixture 등록이다.
struct Fixture {
    explicit Fixture(Allocator& allocator) noexcept
        : types(allocator), registry(allocator, types, {0x4e4f5459, 1}) {
    }

    bool Start() noexcept {
        if (!GeneratedType<NotifyModel>::Register(types) ||
            !GeneratedType<NotifyModelChild>::Register(types)) {
            return false;
        }

        health = FieldIdOf(types, TypeIdOf<NotifyModel>(), "health");
        label = FieldIdOf(types, TypeIdOf<NotifyModel>(), "label");
        plain = FieldIdOf(types, TypeIdOf<NotifyModel>(), "plain");
        partner = FieldIdOf(types, TypeIdOf<NotifyModel>(), "partner");
        speed = FieldIdOf(types, TypeIdOf<NotifyModelChild>(), "speed");
        doubled = FunctionIdOf(types, TypeIdOf<NotifyModel>(), "Doubled");
        plainFunction = FunctionIdOf(types, TypeIdOf<NotifyModel>(), "Plain");
        fast = FunctionIdOf(types, TypeIdOf<NotifyModelChild>(), "Fast");
        return health != Guid{} && speed != Guid{} && doubled != Guid{} && fast != Guid{};
    }

    template <class T>
    RootRef<T> Make() noexcept {
        auto created = registry.NewObject<T>(access, TypeIdOf<T>(), {}, {});
        return created ? Move(created.Value()) : RootRef<T>();
    }

    TypeRegistry types;
    ObjectRegistry registry;
    ObjectAccessContext access = registry.OwnerAccess();
    Guid health{};
    Guid label{};
    Guid plain{};
    Guid partner{};
    Guid speed{};
    Guid doubled{};
    Guid plainFunction{};
    Guid fast{};
};

// 받은 통지를 센다.
struct Received {
    ObjectHandle object{};
    Guid target{};
    uint32 count = 0;
};

FieldChangeDelegate Counter(Received& received) noexcept {
    return FieldChangeDelegate::CreateLambda(
        [&received](ObjectHandle object, Guid target) noexcept {
            received.object = object;
            received.target = target;
            ++received.count;
        });
}

// 소멸할 때 같은 Registry의 다른 구독 하나를 해제하는 포획 값이다(RAII 구독 guard — 제거 도중의
// 재진입을 만든다). 이동하면 원본은 아무것도 하지 않는다.
struct UnsubscribeOnDestroy {
    UnsubscribeOnDestroy(ObjectRegistry* registry, const FieldNotifyHandle* other) noexcept
        : registry(registry), other(other) {
    }

    UnsubscribeOnDestroy(UnsubscribeOnDestroy&& source) noexcept
        : registry(source.registry), other(source.other) {
        source.registry = nullptr;
    }

    UnsubscribeOnDestroy(const UnsubscribeOnDestroy&) = delete;
    UnsubscribeOnDestroy& operator=(const UnsubscribeOnDestroy&) = delete;
    UnsubscribeOnDestroy& operator=(UnsubscribeOnDestroy&&) = delete;

    ~UnsubscribeOnDestroy() {
        if (registry != nullptr) {
            removed = registry->Unsubscribe(registry->OwnerAccess(), *other);
        }
    }

    void operator()(ObjectHandle, Guid) noexcept {
    }

    ObjectRegistry* registry;
    const FieldNotifyHandle* other;
    static inline bool removed = false;
};

// 생존 판정(Core Delegate C1)을 가진 호출 대상이다 — alive가 거짓이면 죽은 구독이다.
struct LiveTarget {
    void operator()(ObjectHandle, Guid) noexcept {
        ++*calls;
    }

    bool IsAlive() const noexcept {
        return *alive;
    }

    const bool* alive;
    uint32* calls;
};

} // namespace

// 번호 배치: 기반의 번호 그대로 → 자기 Notify 필드(schema 순서) → 자기 Notify 함수. 기반 lease로
// 얻은 번호가 파생 객체에서도 같은 대상이다. 통지하지 않는 필드·함수는 번호가 없다.
TEST(FieldNotification_IndexLayout) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto base = fixture.types.Acquire(TypeIdOf<NotifyModel>());
        auto child = fixture.types.Acquire(TypeIdOf<NotifyModelChild>());
        REQUIRE(base && child);
        CHECK(base.Value().NotifyCount() == 4);
        CHECK(base.Value().NotifyIndex(fixture.health) == 0);
        CHECK(base.Value().NotifyIndex(fixture.label) == 1);
        CHECK(base.Value().NotifyIndex(fixture.partner) == 2);
        CHECK(base.Value().NotifyIndex(fixture.doubled) == 3);
        CHECK(base.Value().NotifyIndex(fixture.plain) == NotifyIndexNone);
        CHECK(base.Value().NotifyIndex(fixture.plainFunction) == NotifyIndexNone);
        CHECK(base.Value().NotifyIndex(fixture.speed) == NotifyIndexNone);

        // 파생: 기반 넷(health·label·partner·Doubled — 번호 그대로) → 자기 필드 speed → 자기 함수
        // Fast.
        CHECK(child.Value().NotifyCount() == 6);
        for (uint32 index = 0; index < base.Value().NotifyCount(); ++index) {
            CHECK(child.Value().NotifyTarget(index) == base.Value().NotifyTarget(index));
        }

        CHECK(child.Value().NotifyIndex(fixture.doubled) == 3);
        CHECK(child.Value().NotifyIndex(fixture.speed) == 4);
        CHECK(child.Value().NotifyIndex(fixture.fast) == 5);
        for (uint32 index = 0; index < child.Value().NotifyCount(); ++index) {
            CHECK(child.Value().NotifyIndex(child.Value().NotifyTarget(index)) == index);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 대상 구독은 그 대상만, 전체 구독은 모두 받는다. 해제 뒤에는 받지 않고 상태가 사라진다.
TEST(FieldNotification_SubscribeNotifyUnsubscribe) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        RootRef<NotifyModelChild> model = fixture.Make<NotifyModelChild>();
        REQUIRE(!model.Handle().IsNull());
        const ObjectHandle object = model.Handle();

        // 구독자가 없으면 아무것도 하지 않는다(대상이 아닌 ID도 판정하지 않는다).
        CHECK(registry.Notify(access, object, {fixture.health}));
        CHECK(registry.Notify(access, object, {fixture.plain}));
        CHECK(registry.Stats().notifiedObjects == 0);

        Received health;
        Received all;
        auto healthHandle =
            registry.TrySubscribe(access, object, {fixture.health}, Counter(health));
        REQUIRE(healthHandle);
        auto allHandle = registry.TrySubscribeAll(access, object, Counter(all));
        REQUIRE(allHandle);
        CHECK(registry.Stats().notifiedObjects == 1);

        REQUIRE(registry.Notify(access, object, {fixture.health}));
        CHECK(health.count == 1 && health.object == object && health.target == fixture.health);
        CHECK(all.count == 1 && all.target == fixture.health);

        // 다른 대상은 전체 구독만 받는다. 번호 통지도 같다(Fast = 5).
        REQUIRE(registry.Notify(access, object, {fixture.speed}));
        REQUIRE(registry.NotifyIndex(access, object, 5));
        CHECK(health.count == 1 && all.count == 3 && all.target == fixture.fast);

        // 구독자가 있으면 대상이 아닌 ID·범위 밖 번호는 InvalidArgument다.
        CHECK(ErrorOf(registry.Notify(access, object, {fixture.plain})) ==
              ErrorCode::InvalidArgument);
        CHECK(ErrorOf(registry.NotifyIndex(access, object, 6)) == ErrorCode::InvalidArgument);

        // 해제: 두 번째는 false, 모두 해제하면 상태가 사라진다.
        CHECK(registry.Unsubscribe(access, healthHandle.Value()));
        CHECK(!registry.Unsubscribe(access, healthHandle.Value()));
        REQUIRE(registry.Notify(access, object, {fixture.health}));
        CHECK(health.count == 1 && all.count == 4);
        CHECK(registry.Unsubscribe(access, allHandle.Value()));
        CHECK(registry.Stats().notifiedObjects == 0);
        REQUIRE(registry.Notify(access, object, {fixture.health}));
        CHECK(all.count == 4);

        // UnsubscribeAll은 수를 돌려준다.
        REQUIRE(registry.TrySubscribe(access, object, {fixture.label}, Counter(health)));
        REQUIRE(registry.TrySubscribe(access, object, {fixture.label}, Counter(all)));
        auto removed = registry.UnsubscribeAll(access, object);
        REQUIRE(removed);
        CHECK(removed.Value() == 2 && registry.Stats().notifiedObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 구독 거부: 그 타입에 없는 ID는 NotFound, 통지 대상이 아닌 필드·함수는 InvalidArgument, 다른
// 문맥은 WrongContext, 통지 대상이 없는 타입의 전체 구독은 InvalidArgument.
TEST(FieldNotification_SubscribeRejections) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        RootRef<NotifyModel> model = fixture.Make<NotifyModel>();
        const ObjectHandle object = model.Handle();
        Received received;
        CHECK(ErrorOf(registry.TrySubscribe(access, object, {Guid{1, 2}}, Counter(received))) ==
              ErrorCode::NotFound);
        CHECK(ErrorOf(registry.TrySubscribe(access, object, {fixture.speed}, Counter(received))) ==
              ErrorCode::NotFound);
        CHECK(ErrorOf(registry.TrySubscribe(access, object, {fixture.plain}, Counter(received))) ==
              ErrorCode::InvalidArgument);
        CHECK(ErrorOf(registry.TrySubscribe(access, object, {fixture.plainFunction},
                                            Counter(received))) == ErrorCode::InvalidArgument);
        const ObjectAccessContext other{ContextId{0x4e4f5459, 2}};
        CHECK(ErrorOf(registry.TrySubscribe(other, object, {fixture.health}, Counter(received))) ==
              ErrorCode::WrongContext);
        CHECK(ErrorOf(registry.Notify(other, object, {fixture.health})) == ErrorCode::WrongContext);
        CHECK(registry.Stats().notifiedObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 통지 중 구독 변경: 추가는 다음 통지부터, 자기 해제는 즉시, 파괴 요청은 상태를 비우고(뒤에서
// 반환) 다음 통지는 아무도 부르지 않는다.
TEST(FieldNotification_ChangesDuringNotify) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        RootRef<NotifyModel> model = fixture.Make<NotifyModel>();
        const ObjectHandle object = model.Handle();
        const Guid health = fixture.health;

        Received late;
        uint32 adds = 0;
        FieldNotifyHandle self{};
        uint32 selfCalls = 0;
        REQUIRE(registry.TrySubscribe(
            access, object, {health},
            FieldChangeDelegate::CreateLambda([&](ObjectHandle, Guid) noexcept {
                if (adds++ == 0) {
                    CHECK(registry.TrySubscribe(access, object, {health}, Counter(late)));
                }
            })));
        auto selfHandle = registry.TrySubscribe(
            access, object, {health},
            FieldChangeDelegate::CreateLambda([&](ObjectHandle, Guid) noexcept {
                ++selfCalls;
                CHECK(registry.Unsubscribe(access, self));
            }));
        REQUIRE(selfHandle);
        self = selfHandle.Value();

        REQUIRE(registry.Notify(access, object, {health}));
        CHECK(adds == 1 && late.count == 0 && selfCalls == 1);
        REQUIRE(registry.Notify(access, object, {health}));
        CHECK(adds == 2 && late.count == 1 && selfCalls == 1);

        // 통지 중 파괴 요청: 남은 구독자는 그 통지에서 빠지고 상태는 통지가 끝난 뒤 반환된다.
        Received after;
        REQUIRE(registry.TrySubscribe(
            access, object, {fixture.label},
            FieldChangeDelegate::CreateLambda([&](ObjectHandle, Guid) noexcept {
                CHECK(registry.RequestDestroy(access, object));
            })));
        REQUIRE(registry.TrySubscribe(access, object, {fixture.label}, Counter(after)));
        REQUIRE(registry.Notify(access, object, {fixture.label}));
        CHECK(after.count == 0);
        CHECK(registry.Stats().notifiedObjects == 0);
        CHECK(ErrorOf(registry.TrySubscribe(access, object, {health}, Counter(after))) ==
              ErrorCode::Destroyed);
        CHECK(registry.Notify(access, object, {health}));
        model.Reset();
        REQUIRE(registry.Collect(access));
        CHECK(ErrorOf(registry.Notify(access, object, {health})) == ErrorCode::StaleHandle);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 깊이 상한: 구독자가 같은 객체를 다시 알리면 MaxNotifyDepth까지만 부르고 나머지는 버린다(성공,
// 버린 수를 센다). 다른 객체로 이어지는 연쇄도 같은 깊이로 센다.
TEST(FieldNotification_DepthLimit) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        RootRef<NotifyModel> first = fixture.Make<NotifyModel>();
        RootRef<NotifyModel> second = fixture.Make<NotifyModel>();
        const ObjectHandle a = first.Handle();
        const ObjectHandle b = second.Handle();
        const Guid health = fixture.health;
        uint32 calls = 0;
        // a → b → a → ... 로 번갈아 알린다.
        REQUIRE(registry.TrySubscribe(
            access, a, {health},
            FieldChangeDelegate::CreateLambda([&](ObjectHandle, Guid) noexcept {
                ++calls;
                CHECK(registry.Notify(access, b, {health}));
            })));
        REQUIRE(registry.TrySubscribe(
            access, b, {health},
            FieldChangeDelegate::CreateLambda([&](ObjectHandle, Guid) noexcept {
                ++calls;
                CHECK(registry.Notify(access, a, {health}));
            })));
        REQUIRE(registry.Notify(access, a, {health}));
        CHECK(calls == MaxNotifyDepth);
        CHECK(registry.Stats().droppedNotifications == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// GC 회수는 남은 구독 상태를 반환한다(파괴 요청 없이 닿지 않게 된 객체).
TEST(FieldNotification_ReclaimReleasesState) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        RootRef<NotifyModel> model = fixture.Make<NotifyModel>();
        const ObjectHandle object = model.Handle();
        Received received;
        auto handle = registry.TrySubscribe(access, object, {fixture.health}, Counter(received));
        REQUIRE(handle);
        CHECK(registry.Stats().notifiedObjects == 1);
        model.Reset();
        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().notifiedObjects == 0);
        CHECK(!registry.Unsubscribe(access, handle.Value()));
        CHECK(received.count == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 할당이 몇 번째에서 실패해도 그 오류를 돌려주고 상태를 남기지 않는다.
TEST(FieldNotification_AllocationFailureLeavesNothing) {
    SystemAllocator allocator;
    {
        test::NFailAllocator failing(allocator);
        TypeRegistry types(allocator);
        ObjectRegistry registry(failing, types, {0x4e4f5459, 3});
        REQUIRE(GeneratedType<NotifyModel>::Register(types));
        const ObjectAccessContext access = registry.OwnerAccess();
        auto created = registry.NewObject<NotifyModel>(access, TypeIdOf<NotifyModel>(), {}, {});
        REQUIRE(created);
        const Guid health = FieldIdOf(types, TypeIdOf<NotifyModel>(), "health");
        constexpr uint64 MaxAttempts = 16;
        bool subscribed = false;
        for (uint64 attempt = 1; attempt <= MaxAttempts && !subscribed; ++attempt) {
            Received received;
            failing.FailOnAttempt(attempt);
            auto handle = registry.TrySubscribe(access, created.Value().Handle(), {health},
                                                Counter(received));
            failing.DisableFailure();
            if (handle) {
                subscribed = true;
                REQUIRE(registry.Notify(access, created.Value().Handle(), {health}));
                CHECK(received.count == 1);
                CHECK(registry.Unsubscribe(access, handle.Value()));
                break;
            }

            CHECK(handle.Error() == ErrorCode::OutOfMemory);
            CHECK(registry.Stats().notifiedObjects == 0);
        }
        CHECK(subscribed);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 재진입: 구독을 해제하면 포획 값의 소멸자가 같은 객체의 다른 구독을 해제한다 — 그 안쪽 해제가
// 목록을 옮기거나 상태를 반환하지 않고, 바깥이 끝난 뒤 상태가 사라진다.
TEST(FieldNotification_UnsubscribeFromCaptureDestructor) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        RootRef<NotifyModel> model = fixture.Make<NotifyModel>();
        const ObjectHandle object = model.Handle();
        Received label;
        auto labelHandle = registry.TrySubscribe(access, object, {fixture.label}, Counter(label));
        REQUIRE(labelHandle);
        const FieldNotifyHandle other = labelHandle.Value();
        UnsubscribeOnDestroy::removed = false;
        auto guarded = registry.TrySubscribe(
            access, object, {fixture.health},
            FieldChangeDelegate::CreateLambda(UnsubscribeOnDestroy(&registry, &other)));
        REQUIRE(guarded);
        CHECK(registry.Unsubscribe(access, guarded.Value()));
        CHECK(UnsubscribeOnDestroy::removed);
        CHECK(registry.Stats().notifiedObjects == 0);
        REQUIRE(registry.Notify(access, object, {fixture.label}));
        CHECK(label.count == 0);

        // UnsubscribeAll도 같다(비우는 도중의 재진입).
        auto again = registry.TrySubscribe(access, object, {fixture.label}, Counter(label));
        REQUIRE(again);
        const FieldNotifyHandle second = again.Value();
        UnsubscribeOnDestroy::removed = false;
        REQUIRE(registry.TrySubscribe(
            access, object, {fixture.health},
            FieldChangeDelegate::CreateLambda(UnsubscribeOnDestroy(&registry, &second))));
        auto all = registry.UnsubscribeAll(access, object);
        REQUIRE(all);
        CHECK(UnsubscribeOnDestroy::removed);
        CHECK(registry.Stats().notifiedObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 해제 handle의 ABA: 목록이 사라졌다 다시 생기면 같은 DelegateHandle 값이 나와도 옛 handle은 새
// 구독을 지우지 않는다.
TEST(FieldNotification_StaleHandleDoesNotRemoveNewSubscription) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        RootRef<NotifyModel> model = fixture.Make<NotifyModel>();
        const ObjectHandle object = model.Handle();
        Received first;
        Received second;
        auto old = registry.TrySubscribe(access, object, {fixture.health}, Counter(first));
        REQUIRE(old);
        CHECK(registry.Unsubscribe(access, old.Value()));
        auto fresh = registry.TrySubscribe(access, object, {fixture.health}, Counter(second));
        REQUIRE(fresh);
        CHECK(fresh.Value().delegate == old.Value().delegate);
        CHECK(!registry.Unsubscribe(access, old.Value()));
        REQUIRE(registry.Notify(access, object, {fixture.health}));
        CHECK(second.count == 1);

        // 전체 구독도 같다(상태가 사라졌다 다시 생긴다).
        CHECK(registry.Unsubscribe(access, fresh.Value()));
        auto oldAll = registry.TrySubscribeAll(access, object, Counter(first));
        REQUIRE(oldAll);
        CHECK(registry.Unsubscribe(access, oldAll.Value()));
        auto freshAll = registry.TrySubscribeAll(access, object, Counter(second));
        REQUIRE(freshAll);
        CHECK(!registry.Unsubscribe(access, oldAll.Value()));
        CHECK(registry.Unsubscribe(access, freshAll.Value()));

        // 다른 문맥의 해제는 false다.
        auto kept = registry.TrySubscribe(access, object, {fixture.health}, Counter(first));
        REQUIRE(kept);
        CHECK(!registry.Unsubscribe(ObjectAccessContext{ContextId{0x4e4f5459, 9}}, kept.Value()));
        CHECK(registry.Unsubscribe(access, kept.Value()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 통지 중 UnsubscribeAll 뒤 같은 통지 안의 새 구독은 남는다. 같은 객체의 중첩 통지 안에서 마지막
// 구독을 해제해도 바깥 통지가 끝난 뒤 상태가 사라진다. 통지 중 다른 객체를 많이 구독해 표가 커져도
// 바깥 통지가 이어진다.
TEST(FieldNotification_StateChangesInsideNotify) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        RootRef<NotifyModel> model = fixture.Make<NotifyModel>();
        const ObjectHandle object = model.Handle();
        const Guid health = fixture.health;
        Received late;
        bool once = false;
        REQUIRE(registry.TrySubscribe(
            access, object, {health},
            FieldChangeDelegate::CreateLambda([&](ObjectHandle, Guid) noexcept {
                if (!once) {
                    once = true;
                    CHECK(registry.UnsubscribeAll(access, object));
                    CHECK(registry.TrySubscribe(access, object, {health}, Counter(late)));
                }
            })));
        REQUIRE(registry.Notify(access, object, {health}));
        CHECK(registry.Stats().notifiedObjects == 1);
        REQUIRE(registry.Notify(access, object, {health}));
        CHECK(late.count == 1);
        REQUIRE(registry.UnsubscribeAll(access, object));

        // 중첩 통지 안의 마지막 해제.
        FieldNotifyHandle self{};
        uint32 depth = 0;
        auto handle = registry.TrySubscribe(
            access, object, {health},
            FieldChangeDelegate::CreateLambda([&](ObjectHandle, Guid) noexcept {
                if (++depth == 1) {
                    CHECK(registry.Notify(access, object, {health}));
                } else {
                    CHECK(registry.Unsubscribe(access, self));
                }
            }));
        REQUIRE(handle);
        self = handle.Value();
        REQUIRE(registry.Notify(access, object, {health}));
        CHECK(depth == 2 && registry.Stats().notifiedObjects == 0);

        // 통지 중 다른 객체 64개를 구독한다(표가 다시 배치된다).
        constexpr int Others = 64;
        RootRef<NotifyModel> others[Others];
        for (RootRef<NotifyModel>& other : others) {
            other = fixture.Make<NotifyModel>();
        }

        Received after;
        REQUIRE(registry.TrySubscribe(
            access, object, {health},
            FieldChangeDelegate::CreateLambda([&](ObjectHandle, Guid) noexcept {
                for (RootRef<NotifyModel>& other : others) {
                    CHECK(registry.TrySubscribe(access, other.Handle(), {health}, Counter(late)));
                }
            })));
        REQUIRE(registry.TrySubscribe(access, object, {health}, Counter(after)));
        REQUIRE(registry.Notify(access, object, {health}));
        CHECK(after.count == 1 && registry.Stats().notifiedObjects == Others + 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// CompactNotifications는 생존 판정이 거짓인 구독을 지우고, 통지 중에는 Busy다. Discard된 객체의
// 통지는 구독자를 부르지 않고 성공이다(구독 유무와 무관하게 같은 결과).
TEST(FieldNotification_CompactAndDiscard) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        ObjectRegistry& registry = fixture.registry;
        const ObjectAccessContext& access = fixture.access;
        RootRef<NotifyModel> model = fixture.Make<NotifyModel>();
        const ObjectHandle object = model.Handle();
        bool alive = true;
        uint32 calls = 0;
        REQUIRE(
            registry.TrySubscribe(access, object, {fixture.health},
                                  FieldChangeDelegate::CreateLambda(LiveTarget{&alive, &calls})));
        Received steady;
        REQUIRE(registry.TrySubscribe(access, object, {fixture.health}, Counter(steady)));
        REQUIRE(registry.Notify(access, object, {fixture.health}));
        CHECK(calls == 1);
        alive = false;
        auto compacted = registry.CompactNotifications(access, object);
        REQUIRE(compacted);
        CHECK(compacted.Value() == 1);
        REQUIRE(registry.Notify(access, object, {fixture.health}));
        CHECK(calls == 1 && steady.count == 2);

        // 통지 중 Compact는 Busy다.
        ErrorCode inside = ErrorCode::None;
        REQUIRE(registry.TrySubscribe(
            access, object, {fixture.label},
            FieldChangeDelegate::CreateLambda([&](ObjectHandle, Guid) noexcept {
                inside = ErrorOf(registry.CompactNotifications(access, object));
            })));
        REQUIRE(registry.Notify(access, object, {fixture.label}));
        CHECK(inside == ErrorCode::Busy);

        // Discard된 객체: 구독이 남아 있어도 부르지 않고 성공이다.
        REQUIRE(registry.Discard(access, object));
        CHECK(registry.Notify(access, object, {fixture.health}));
        CHECK(steady.count == 2);
        model.Reset();
        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().notifiedObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 등록 검사: EditorOnly 필드의 Notify, Pure·Const가 아닌 함수의 Notify는 InvalidArgument다(생성
// schema를 복사해 flag를 바꾼다).
TEST(FieldNotification_RegistrationRejectsMisplacedNotify) {
    SystemAllocator allocator;
    {
        const TypeSchema& generated = GeneratedType<NotifyModel>::Schema();
        Array<FieldSchema> fields(allocator);
        for (const FieldSchema& field : generated.fields) {
            REQUIRE(fields.TryPushBack(field));
        }

        Array<FunctionSchema> functions(allocator);
        for (const FunctionSchema& function : generated.functions) {
            REQUIRE(functions.TryPushBack(function));
        }

        {
            TypeRegistry types(allocator);
            TypeSchema edited = generated;
            edited.fields = fields.View();
            fields[0].flags |= static_cast<uint32>(FieldFlags::EditorOnly);
            CHECK(ErrorOf(types.Register(edited, GeneratedType<NotifyModel>::Ops())) ==
                  ErrorCode::InvalidArgument);
            fields[0].flags &= ~static_cast<uint32>(FieldFlags::EditorOnly);
        }

        // Notify 함수의 모양: Pure 없음, EditorOnly, Const 없음(Static 대신 — Static·Const는 함께
        // 둘 수 없다), Notify를 Pure 아닌 함수(Plain은 Pure라 Doubled의 Pure를 뺀다).
        constexpr uint32 Pure = static_cast<uint32>(FunctionFlags::Pure);
        constexpr uint32 Const = static_cast<uint32>(FunctionFlags::Const);
        constexpr uint32 EditorOnly = static_cast<uint32>(FunctionFlags::EditorOnly);
        const uint32 changes[][2] = {{Pure, 0}, {0, EditorOnly}, {Const, 0}};
        for (const auto& change : changes) {
            for (FunctionSchema& function : functions) {
                if ((function.flags & static_cast<uint32>(FunctionFlags::Notify)) != 0) {
                    function.flags = (function.flags & ~change[0]) | change[1];
                }
            }

            {
                TypeRegistry types(allocator);
                TypeSchema edited = generated;
                edited.functions = functions.View();
                CHECK(ErrorOf(types.Register(edited, GeneratedType<NotifyModel>::Ops())) ==
                      ErrorCode::InvalidArgument);
            }

            for (FunctionSchema& function : functions) {
                if ((function.flags & static_cast<uint32>(FunctionFlags::Notify)) != 0) {
                    function.flags = (function.flags & ~change[1]) | change[0];
                }
            }
        }

        // 상속 필드의 Notify flag가 기반과 다르면 상속 필드 대조가 거부한다(TypeMismatch).
        {
            TypeRegistry types(allocator);
            REQUIRE(GeneratedType<NotifyModel>::Register(types));
            const TypeSchema& childSchema = GeneratedType<NotifyModelChild>::Schema();
            Array<FieldSchema> childFields(allocator);
            for (const FieldSchema& field : childSchema.fields) {
                REQUIRE(childFields.TryPushBack(field));
            }

            for (FieldSchema& field : childFields) {
                if (StringView(field.name.Data(), field.name.Size()) == StringView("plain")) {
                    field.flags |= static_cast<uint32>(FieldFlags::Notify);
                }
            }

            TypeSchema edited = childSchema;
            edited.fields = childFields.View();
            CHECK(ErrorOf(types.Register(edited, GeneratedType<NotifyModelChild>::Ops())) ==
                  ErrorCode::TypeMismatch);
        }

        // struct 멤버의 Notify는 거부한다.
        {
            TypeRegistry types(allocator);
            const TypeSchema& point = GeneratedType<reflection_fixture::CallPoint>::Schema();
            Array<FieldSchema> members(allocator);
            for (const FieldSchema& field : point.fields) {
                REQUIRE(members.TryPushBack(field));
            }

            members[0].flags |= static_cast<uint32>(FieldFlags::Notify);
            TypeSchema edited = point;
            edited.fields = members.View();
            CHECK(ErrorOf(types.RegisterStruct(
                      edited, GeneratedType<reflection_fixture::CallPoint>::Ops())) ==
                  ErrorCode::InvalidArgument);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace field_notification_tests_file
