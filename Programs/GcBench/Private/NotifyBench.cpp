#include "NotifyBench.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/FieldNotifications.h>
#include <Object/GeneratedPathAccess.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>

#include <stdio.h>

// 변경 통지 비용 벤치 본문이다(NotifyBench.h — Docs/Design/ChangeNotification.ko.md CN-D12).
namespace gc_bench {

namespace {

using namespace rocks;

constexpr TypeId NotifyNodeTypeId{0x4763424e4f544659ULL, 1};
constexpr Guid FirstFieldId{0x4763424e4f544631ULL, 1};
constexpr Guid SecondFieldId{0x4763424e4f544632ULL, 1};
// setter 경유 write(CN-D14)를 재는 통지 없는 필드다.
constexpr Guid ThirdFieldId{0x4763424e4f544633ULL, 1};
constexpr uint32 FirstTarget = 0;
constexpr uint32 SubscriberCounts[] = {1, 8, 64};

// 통지 대상 필드 둘을 가진 측정 객체다(수동 함수표로 등록된 write 비용도 잰다).
class NotifyNode final : public Object {
public:
    explicit NotifyNode(Allocator&) noexcept {
    }

    int32 first = 0;
    int32 second = 0;
    int32 third = 0;
};

// 할당 수를 세는 allocator다(측정 구간의 할당 0을 확인한다).
class CountingAllocator final : public Allocator {
public:
    explicit CountingAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&allocations_, 1);
        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

    uint64 Allocations() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&allocations_);
    }

private:
    Allocator& inner_;
    uint64 allocations_ = 0;
};

const FieldSchema NotifyFields[] = {
    {FirstFieldId,
     FieldKind::Value,
     static_cast<uint32>(FieldFlags::Notify),
     Span<const char>("first", 5),
     {},
     ValueType{ValueKind::Int32, TypeId{}}},
    {SecondFieldId,
     FieldKind::Value,
     static_cast<uint32>(FieldFlags::Notify),
     Span<const char>("second", 6),
     {},
     ValueType{ValueKind::Int32, TypeId{}}},
    {ThirdFieldId,
     FieldKind::Value,
     0,
     Span<const char>("third", 5),
     {},
     ValueType{ValueKind::Int32, TypeId{}}},
};

// 측정 필드 하나의 수동 함수표다(등록된 write — FieldWriteOperation의 통지 판정과 구독자가 있을
// 때의 사전 read를 거친다).
template <int32 NotifyNode::* Member>
RuntimeFieldOps NodeFieldOps(Guid id) noexcept {
    RuntimeFieldOps ops;
    ops.fieldId = id;
    ops.read = +[](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
                   FieldValue& out) noexcept -> Result<void> {
        auto borrowed = registry.Resolve<NotifyNode>(access, object);
        if (!borrowed) {
            return Result<void>::Failure(borrowed.Error());
        }

        auto node = borrowed.Value().Get();
        if (!node) {
            return Result<void>::Failure(node.Error());
        }

        out.kind = ValueKind::Int32;
        out.present = true;
        out.signedInteger = node.Value()->*Member;
        return Result<void>::Success();
    };
    ops.write = +[](ObjectRegistry& registry, const ObjectAccessContext& access,
                    ObjectHandle object, const FieldValue& value) noexcept -> Result<void> {
        auto borrowed = registry.ResolveForWrite<NotifyNode>(access, object);
        if (!borrowed) {
            return Result<void>::Failure(borrowed.Error());
        }

        auto node = borrowed.Value().Get();
        if (!node) {
            return Result<void>::Failure(node.Error());
        }

        node.Value()->*Member = static_cast<int32>(value.signedInteger);
        return Result<void>::Success();
    };
    ops.writeStaged =
        +[](ConstructionContext&, ObjectHandle, const FieldValue&) noexcept -> Result<void> {
        return Result<void>::Failure(ErrorCode::Unsupported);
    };
    return ops;
}

// setter 있는 측정 필드의 수동 함수표다(Docs/Design/ChangeNotification.ko.md CN-D14 — 등록된
// write가 상자 경유로 setter에 넘긴다. 상자 분리·연산이 path thunk를 쓴다). setter는 같은 값이면
// 아무것도 하지 않고 다르면 넣는다(통지 없는 필드).
template <int32 NotifyNode::* Member>
RuntimeFieldOps SetterFieldOps(Guid id) noexcept {
    RuntimeFieldOps ops = NodeFieldOps<Member>(id);
    ops.path = +[](const RuntimeFieldOps&, detail::PathCursor& cursor) noexcept -> Result<void> {
        return detail::PathClassField<NotifyNode, ValueKind::Int32>(cursor, Member);
    };
    ops.setter = +[](Object& object, void* value) noexcept -> Result<void> {
        int32& field = static_cast<NotifyNode&>(object).*Member;
        field = *static_cast<const int32*>(value);
        return Result<void>::Success();
    };
    return ops;
}

// 한 측정 결과를 출력한다(ns/연산, 구간의 할당 수).
void Report(const char* label, uint64 nanoseconds, uint64 operations, uint64 allocations) noexcept {
    const double perOperation =
        operations == 0 ? 0.0 : static_cast<double>(nanoseconds) / static_cast<double>(operations);
    printf("  %-40s %10.2f ns/op  allocations %llu\n", label, perOperation,
           static_cast<unsigned long long>(allocations));
}

} // namespace

int RunNotifyBench(uint64 iterations, uint32 objects) noexcept {
    SystemAllocator system;
    CountingAllocator allocator(system);
    TypeRegistry types(allocator);
    RuntimeTypeOps ops = MakeRuntimeTypeOps<NotifyNode>();
    ops.hasStrongReferences = false;
    // 등록은 함수표를 복사한다(지역 배열이어도 된다).
    const RuntimeFieldOps fieldOps[] = {NodeFieldOps<&NotifyNode::first>(FirstFieldId),
                                        NodeFieldOps<&NotifyNode::second>(SecondFieldId),
                                        SetterFieldOps<&NotifyNode::third>(ThirdFieldId)};
    ops.fields = Span<const RuntimeFieldOps>(fieldOps);
    TypeSchema schema{NotifyNodeTypeId, 1, {}, {}};
    schema.fields = Span<const FieldSchema>(NotifyFields);
    auto registered = types.Register(schema, Move(ops));
    if (!registered) {
        printf("notify bench: type registration failed (error %u)\n",
               static_cast<uint32>(registered.Error()));
        return 1;
    }

    int status = 0;
    {
        ObjectRegistry registry(allocator, types, {0x47434e54, 1});
        const ObjectAccessContext access = registry.OwnerAccess();
        Array<RootRef<NotifyNode>> nodes(allocator);
        if (objects == 0 || !nodes.TryReserve(objects)) {
            printf("notify bench: object list allocation failed\n");
            return 1;
        }

        for (uint32 index = 0; index < objects; ++index) {
            auto created = registry.NewObject<NotifyNode>(access, NotifyNodeTypeId, {}, {});
            if (!created || !nodes.TryPushBack(Move(created.Value()))) {
                printf("notify bench: object creation failed\n");
                return 1;
            }
        }

        printf("\n== change notification bench (iterations %llu, objects %u) ==\n",
               static_cast<unsigned long long>(iterations), objects);
        uint64 failures = 0;

        // 1. 구독자 없는 통지.
        uint64 allocated = allocator.Allocations();
        uint64 started = platform::MonotonicNanoseconds();
        for (uint64 index = 0; index < iterations; ++index) {
            failures +=
                registry.NotifyIndex(access, nodes[index % objects].Handle(), FirstTarget) ? 0 : 1;
        }

        Report("notify, no subscribers", platform::MonotonicNanoseconds() - started, iterations,
               allocator.Allocations() - allocated);

        // 2. 구독자 있음·그 번호 비트 꺼짐(대상 1 구독, 대상 0 통지).
        uint64 calls = 0;
        const ObjectHandle watched = nodes[0].Handle();
        auto other = registry.TrySubscribe(
            access, watched, NotifyTarget{SecondFieldId},
            FieldChangeDelegate::CreateLambda([&calls](ObjectHandle, Guid) noexcept { ++calls; }));
        if (!other) {
            printf("notify bench: subscription failed\n");
            return 1;
        }

        allocated = allocator.Allocations();
        started = platform::MonotonicNanoseconds();
        for (uint64 index = 0; index < iterations; ++index) {
            failures += registry.NotifyIndex(access, watched, FirstTarget) ? 0 : 1;
        }

        Report("notify, subscribed, target bit off", platform::MonotonicNanoseconds() - started,
               iterations, allocator.Allocations() - allocated);
        (void)registry.Unsubscribe(access, other.Value());

        // 3. 구독자 n명 발행.
        for (const uint32 subscribers : SubscriberCounts) {
            Array<FieldNotifyHandle> handles(allocator);
            if (!handles.TryReserve(subscribers)) {
                printf("notify bench: handle list allocation failed\n");
                return 1;
            }

            for (uint32 index = 0; index < subscribers; ++index) {
                auto handle =
                    registry.TrySubscribe(access, watched, NotifyTarget{FirstFieldId},
                                          FieldChangeDelegate::CreateLambda(
                                              [&calls](ObjectHandle, Guid) noexcept { ++calls; }));
                if (!handle || !handles.TryPushBack(handle.Value())) {
                    printf("notify bench: subscription failed\n");
                    return 1;
                }
            }

            calls = 0;
            allocated = allocator.Allocations();
            started = platform::MonotonicNanoseconds();
            for (uint64 index = 0; index < iterations; ++index) {
                failures += registry.NotifyIndex(access, watched, FirstTarget) ? 0 : 1;
            }

            const uint64 elapsed = platform::MonotonicNanoseconds() - started;
            char label[64];
            snprintf(label, sizeof(label), "notify, %u subscribers", subscribers);
            Report(label, elapsed, iterations, allocator.Allocations() - allocated);
            snprintf(label, sizeof(label), "  per subscriber call (%u)", subscribers);
            Report(label, elapsed, calls, 0);
            if (calls != iterations * subscribers) {
                printf("notify bench: expected %llu calls, got %llu\n",
                       static_cast<unsigned long long>(iterations * subscribers),
                       static_cast<unsigned long long>(calls));
                status = 1;
            }

            for (const FieldNotifyHandle& handle : handles) {
                (void)registry.Unsubscribe(access, handle);
            }
        }

        // 4. 구독·해제.
        allocated = allocator.Allocations();
        started = platform::MonotonicNanoseconds();
        for (uint64 index = 0; index < iterations; ++index) {
            auto handle = registry.TrySubscribe(
                access, watched, NotifyTarget{FirstFieldId},
                FieldChangeDelegate::CreateLambda([](ObjectHandle, Guid) noexcept {}));
            failures += handle && registry.Unsubscribe(access, handle.Value()) ? 0 : 1;
        }

        Report("subscribe + unsubscribe", platform::MonotonicNanoseconds() - started, iterations,
               allocator.Allocations() - allocated);

        // 5. 등록된 write(FieldWriteOperation): 구독자 없음, 구독자 하나·바뀐 값(사전
        // read·비교·통지), 구독자 하나·같은 값(사전 read·비교만).
        {
            auto lease = types.Acquire(NotifyNodeTypeId);
            const RuntimeFieldOps* first = lease ? lease.Value().FieldOps(0) : nullptr;
            if (!first || !first->write) {
                printf("notify bench: field operation lookup failed\n");
                return 1;
            }

            FieldValue value(allocator);
            value.kind = ValueKind::Int32;
            const ObjectHandle plain = nodes[objects > 1 ? 1 : 0].Handle();
            allocated = allocator.Allocations();
            started = platform::MonotonicNanoseconds();
            for (uint64 index = 0; index < iterations; ++index) {
                value.signedInteger = static_cast<int64>(index & 1);
                failures += first->write(registry, access, plain, value) ? 0 : 1;
            }

            Report("registered write, no subscribers", platform::MonotonicNanoseconds() - started,
                   iterations, allocator.Allocations() - allocated);
            calls = 0;
            auto subscribed =
                registry.TrySubscribe(access, watched, NotifyTarget{FirstFieldId},
                                      FieldChangeDelegate::CreateLambda(
                                          [&calls](ObjectHandle, Guid) noexcept { ++calls; }));
            if (!subscribed) {
                printf("notify bench: subscription failed\n");
                return 1;
            }

            allocated = allocator.Allocations();
            started = platform::MonotonicNanoseconds();
            for (uint64 index = 0; index < iterations; ++index) {
                value.signedInteger = static_cast<int64>(index & 1) + 2;
                failures += first->write(registry, access, watched, value) ? 0 : 1;
            }

            Report("registered write, 1 subscriber, changed",
                   platform::MonotonicNanoseconds() - started, iterations,
                   allocator.Allocations() - allocated);
            if (calls != iterations) {
                printf("notify bench: expected %llu write notifications, got %llu\n",
                       static_cast<unsigned long long>(iterations),
                       static_cast<unsigned long long>(calls));
                status = 1;
            }

            calls = 0;
            allocated = allocator.Allocations();
            started = platform::MonotonicNanoseconds();
            for (uint64 index = 0; index < iterations; ++index) {
                failures += first->write(registry, access, watched, value) ? 0 : 1;
            }

            Report("registered write, 1 subscriber, same",
                   platform::MonotonicNanoseconds() - started, iterations,
                   allocator.Allocations() - allocated);
            if (calls != 0) {
                printf("notify bench: same-value writes notified %llu times\n",
                       static_cast<unsigned long long>(calls));
                status = 1;
            }

            (void)registry.Unsubscribe(access, subscribed.Value());
        }

        // 5b. setter 경유 등록된 write(CN-D14): 상자 분리(할당 하나)·상자 위 쓰기·검증·변경 표지
        // 대조· setter. 바뀐 값과 같은 값을 잰다.
        {
            auto lease = types.Acquire(NotifyNodeTypeId);
            const RuntimeFieldOps* third = lease ? lease.Value().FieldOps(2) : nullptr;
            if (!third || !third->write || !third->setter) {
                printf("notify bench: setter field operation lookup failed\n");
                return 1;
            }

            FieldValue value(allocator);
            value.kind = ValueKind::Int32;
            const ObjectHandle target = nodes[0].Handle();
            allocated = allocator.Allocations();
            started = platform::MonotonicNanoseconds();
            for (uint64 index = 0; index < iterations; ++index) {
                value.signedInteger = static_cast<int64>(index & 1);
                failures += third->write(registry, access, target, value) ? 0 : 1;
            }

            Report("setter write, changed", platform::MonotonicNanoseconds() - started, iterations,
                   allocator.Allocations() - allocated);
            allocated = allocator.Allocations();
            started = platform::MonotonicNanoseconds();
            for (uint64 index = 0; index < iterations; ++index) {
                failures += third->write(registry, access, target, value) ? 0 : 1;
            }

            Report("setter write, same", platform::MonotonicNanoseconds() - started, iterations,
                   allocator.Allocations() - allocated);
        }

        // 6. 통지 묶음 열고 닫기(경로 변경 연산마다 드는 비용 — 모은 통지 없음).
        allocated = allocator.Allocations();
        started = platform::MonotonicNanoseconds();
        for (uint64 index = 0; index < iterations; ++index) {
            auto group = registry.BeginNotifyGroup(access);
            failures += group ? 0 : 1;
        }

        Report("notify group begin + end", platform::MonotonicNanoseconds() - started, iterations,
               allocator.Allocations() - allocated);
        if (failures != 0) {
            printf("notify bench: %llu failed operations\n",
                   static_cast<unsigned long long>(failures));
            status = 1;
        }

        nodes.Clear();
        if (!registry.Shutdown(access)) {
            printf("notify bench: registry shutdown failed\n");
            status = 1;
        }
    }

    if (!types.Shutdown()) {
        printf("notify bench: type registry shutdown failed\n");
        status = 1;
    }

    return status;
}

} // namespace gc_bench
