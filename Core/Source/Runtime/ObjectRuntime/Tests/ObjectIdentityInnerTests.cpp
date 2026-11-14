#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NameComponent.h>
#include <Object/ConstructionContext.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// A0-Oa CollectInners 순서·제외·실패, 조회의 비교 키 할당 실패, out allocator 콜백 재진입 시험이다
// (Docs/Design/ObjectFoundations.ko.md 1.2절·9절 Oa).
using namespace rocks;

namespace {

constexpr TypeId NodeType{0x4f4949494e4e4552ULL, 1};

struct InnerNode final : Object {};

Result<TypeRegistrationToken> Register(TypeRegistry& types) noexcept {
    return types.Register({NodeType, 1, {}, {}}, MakeRuntimeTypeOps<InnerNode>());
}

Result<ObjectHandle> NewNode(ObjectRegistry& registry, ObjectHandle outer, StringView name,
                             RootRef<Object>* root = nullptr) noexcept {
    auto made = registry.NewObject<Object>(registry.OwnerAccess(), NodeType, outer, name);
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    const ObjectHandle handle = made.Value().Handle();
    if (root != nullptr) {
        *root = Move(made.Value());
    }

    return Result<ObjectHandle>::Success(handle);
}

bool Same(const Array<ObjectHandle>& actual, Span<const ObjectHandle> expected) noexcept {
    if (actual.Size() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < expected.Size(); ++index) {
        if (!(actual[index] == expected[index])) {
            return false;
        }
    }

    return true;
}

// 할당마다 한 번 콜백을 부른 뒤 base로 넘긴다(out 문자열·배열이 이것으로 늘 때 Registry에 재진입).
class ReentrantAllocator final : public Allocator {
public:
    explicit ReentrantAllocator(Allocator& base) noexcept : base_(base) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (callback_ != nullptr) {
            void (*run)(void*) noexcept = callback_;
            callback_ = nullptr;
            run(context_);
        }

        return base_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return base_.Deallocate(address);
    }

    void Arm(void (*callback)(void*) noexcept, void* context) noexcept {
        callback_ = callback;
        context_ = context;
    }

private:
    Allocator& base_;
    void (*callback_)(void*) noexcept = nullptr;
    void* context_ = nullptr;
};

struct DestroyOnAllocate {
    ObjectRegistry* registry = nullptr;
    RootRef<Object>* root = nullptr;
    ObjectHandle target;
    bool destroyed = false;
    bool collected = false;
};

void DestroyAndCollect(void* context) noexcept {
    auto& state = *static_cast<DestroyOnAllocate*>(context);
    const auto access = state.registry->OwnerAccess();
    state.root->Reset();
    state.destroyed = static_cast<bool>(state.registry->RequestDestroy(access, state.target));
    state.collected = static_cast<bool>(state.registry->Collect(access));
}

struct MutateDuringCollect {
    ObjectRegistry* registry = nullptr;
    ObjectHandle outer;
    ObjectHandle child;
    ErrorCode created = ErrorCode::None;
    ErrorCode destroyed = ErrorCode::None;
    ErrorCode found = ErrorCode::None;
};

void MutateRegistry(void* context) noexcept {
    auto& state = *static_cast<MutateDuringCollect*>(context);
    const auto access = state.registry->OwnerAccess();
    auto made = state.registry->NewObject<Object>(access, NodeType, state.outer, "Late");
    state.created = made ? ErrorCode::None : made.Error();
    auto destroyed = state.registry->RequestDestroy(access, state.child);
    state.destroyed = destroyed ? ErrorCode::None : destroyed.Error();
    state.found = state.registry->FindChild(access, state.outer, "Late").Error();
}

} // namespace

// 같은 Outer 안은 이름 byte 순(빈 이름 먼저), DepthFirst는 각 Inner 바로 뒤에 그 부분 트리(AS 5.1
// export 순서)다. 공개 전·DestroyRequested 객체는 부분 트리째 빠지고 결과는 out 끝에 붙는다.
TEST(ObjectIdentityInner_CollectInnersOrdersByNameAndSkipsUnpublishedSubtrees) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {0x4f4901, 0x10});
        const auto access = registry.OwnerAccess();
        enum : usize {
            Package,
            Asset,
            Alpha,
            Beta,
            Gamma,
            Y,
            Z,
            K,
            G,
            Count
        };
        ObjectHandle node[Count];
        RootRef<Object> held[Count];
        {
            ConstructionContext transaction(registry, access);
            auto create = [&](usize slot, ObjectHandle outer, StringView name) noexcept {
                auto made = transaction.Create<InnerNode>(NodeType, outer, name);
                node[slot] = made ? made.Value() : ObjectHandle{};
                return static_cast<bool>(made);
            };
            REQUIRE(create(Package, {}, {}));
            REQUIRE(create(Asset, node[Package], {}));
            // 고리 순서(생성 순서)와 이름 순서를 다르게 만든다.
            REQUIRE(create(Alpha, node[Asset], "alpha"));
            REQUIRE(create(Beta, node[Asset], "Beta"));
            REQUIRE(create(Gamma, node[Asset], "Gamma"));
            REQUIRE(create(Z, node[Beta], "z"));
            REQUIRE(create(Y, node[Beta], "y"));
            REQUIRE(create(K, node[Alpha], "k"));
            REQUIRE(create(G, node[Gamma], "g"));
            REQUIRE(transaction.Commit(node, held));
        }

        Array<ObjectHandle> out(allocator);
        REQUIRE(registry.CollectInners(access, node[Package], InnerOrder::Direct, out));
        const ObjectHandle direct[] = {node[Asset]};
        CHECK(Same(out, direct));
        out.Clear();
        REQUIRE(registry.CollectInners(access, node[Package], InnerOrder::DepthFirst, out));
        const ObjectHandle all[] = {node[Asset], node[Beta], node[Y],     node[Z],
                                    node[Gamma], node[G],    node[Alpha], node[K]};
        CHECK(Same(out, all));
        out.Clear();
        REQUIRE(registry.CollectInners(access, node[Asset], InnerOrder::Direct, out));
        const ObjectHandle names[] = {node[Beta], node[Gamma], node[Alpha]};
        CHECK(Same(out, names));

        // 결과는 out 끝에 붙는다.
        out.Clear();
        REQUIRE(out.TryPushBack(node[Package]));
        REQUIRE(registry.CollectInners(access, node[Beta], InnerOrder::DepthFirst, out));
        const ObjectHandle appended[] = {node[Package], node[Y], node[Z]};
        CHECK(Same(out, appended));

        // 공개 전 Inner와 파괴 요청된 Inner(그 부분 트리 포함)는 빠진다.
        {
            ConstructionContext transaction(registry, access);
            REQUIRE(transaction.Create<InnerNode>(NodeType, node[Asset], "Delta"));
            REQUIRE(registry.RequestDestroy(access, node[Gamma]));
            out.Clear();
            REQUIRE(registry.CollectInners(access, node[Package], InnerOrder::DepthFirst, out));
            const ObjectHandle published[] = {node[Asset], node[Beta],  node[Y],
                                              node[Z],     node[Alpha], node[K]};
            CHECK(Same(out, published));
        }

        // 잎 객체는 빈 결과이고 out은 그대로다.
        out.Clear();
        REQUIRE(registry.CollectInners(access, node[K], InnerOrder::DepthFirst, out));
        CHECK(out.Size() == 0);
        for (auto& root : held) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// out 용량·보조 배열 할당 실패는 그 오류이고 out은 호출 전 길이로 남는다. 할당 콜백 중 Registry
// 변경 재진입은 Busy다.
TEST(ObjectIdentityInner_CollectInnersFailuresKeepOutputAndBlockReentrantMutation) {
    SystemAllocator system;
    {
        rocks::test::NFailAllocator registryAllocator(system);
        TypeRegistry types(system);
        REQUIRE(Register(types));
        ObjectRegistry registry(registryAllocator, types, {0x4f4902, 0x10});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[3];
        auto outer = NewNode(registry, {}, {}, &roots[0]);
        REQUIRE(outer);
        auto first = NewNode(registry, outer.Value(), "First", &roots[1]);
        auto second = NewNode(registry, outer.Value(), "Second", &roots[2]);
        REQUIRE(first && second);

        // out 성장 실패.
        rocks::test::NFailAllocator outAllocator(system);
        Array<ObjectHandle> out(outAllocator);
        REQUIRE(out.TryReserve(1));
        REQUIRE(out.TryPushBack(outer.Value()));
        outAllocator.FailOnAttempt(1);
        auto failed = registry.CollectInners(access, outer.Value(), InnerOrder::Direct, out);
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(out.Size() == 1);
        outAllocator.DisableFailure();

        // 보조 배열(Registry allocator) 실패. out 용량은 미리 있다.
        REQUIRE(out.TryReserve(8));
        registryAllocator.FailOnAttempt(1);
        failed = registry.CollectInners(access, outer.Value(), InnerOrder::DepthFirst, out);
        registryAllocator.DisableFailure();
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(out.Size() == 1);
        // 처리 표시는 내려가 있다.
        RootRef<Object> after;
        REQUIRE(NewNode(registry, outer.Value(), "Third", &after));

        // 할당 콜백의 재진입: 생성·파괴 요청·조회 모두 Busy이고 수집은 그대로 끝난다.
        ReentrantAllocator reentrant(system);
        Array<ObjectHandle> collected(reentrant);
        MutateDuringCollect state;
        state.registry = &registry;
        state.outer = outer.Value();
        state.child = first.Value();
        reentrant.Arm(MutateRegistry, &state);
        REQUIRE(registry.CollectInners(access, outer.Value(), InnerOrder::Direct, collected));
        CHECK(state.created == ErrorCode::Busy);
        CHECK(state.destroyed == ErrorCode::Busy);
        CHECK(state.found == ErrorCode::Busy);
        const ObjectHandle expected[] = {first.Value(), second.Value(), after.Handle()};
        CHECK(Same(collected, expected));
        auto lifecycle = registry.InspectLifecycle(access, first.Value());
        CHECK(lifecycle && lifecycle.Value() == ObjectLifecycle::Alive);

        after.Reset();
        for (auto& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// GetName·GetObjectPath는 잠금 안에서 스택에 복사한 뒤 붙이므로, out의 할당 콜백이 그 객체를
// 파괴 요청하고 수집해 이름 기록이 반환되어도 결과가 온전하다(ASan 구성에서 해제 후 읽기가 없음을
// 본다).
TEST(ObjectIdentityInner_NameQueriesSurviveReentrantDestroyAndCollect) {
    SystemAllocator system;
    {
        TypeRegistry types(system);
        REQUIRE(Register(types));
        ObjectRegistry registry(system, types, {0x4f4903, 0x10});
        const auto access = registry.OwnerAccess();
        char longest[MaxNameComponentBytes + 1];
        for (usize index = 0; index < MaxNameComponentBytes; ++index) {
            longest[index] = static_cast<char>('A' + index % 26);
        }

        longest[MaxNameComponentBytes] = '\0';
        RootRef<Object> root;
        auto named = NewNode(registry, {}, longest, &root);
        REQUIRE(named);
        ReentrantAllocator reentrant(system);
        DestroyOnAllocate state;
        state.registry = &registry;
        state.root = &root;
        state.target = named.Value();
        {
            String name(reentrant);
            reentrant.Arm(DestroyAndCollect, &state);
            REQUIRE(registry.GetName(access, named.Value(), name));
            CHECK(state.destroyed && state.collected);
            CHECK(name.View() == StringView(longest));
        }

        CHECK(registry.InspectLifecycle(access, named.Value()).Error() == ErrorCode::StaleHandle);

        // 경로: 가장 깊은 객체만 root다. 콜백이 그것을 놓고 수집하면 사슬 전체가 회수된다.
        auto package = NewNode(registry, {}, {});
        REQUIRE(package);
        auto middle = NewNode(registry, package.Value(), "Middle_Object_Name");
        REQUIRE(middle);
        RootRef<Object> leafRoot;
        auto leaf = NewNode(registry, middle.Value(), longest, &leafRoot);
        REQUIRE(leaf);
        DestroyOnAllocate pathState;
        pathState.registry = &registry;
        pathState.root = &leafRoot;
        pathState.target = leaf.Value();
        {
            String path(reentrant);
            reentrant.Arm(DestroyAndCollect, &pathState);
            REQUIRE(registry.GetObjectPath(access, leaf.Value(), path));
            CHECK(pathState.destroyed && pathState.collected);
            String expected(system);
            REQUIRE(expected.TryAppend("Middle_Object_Name."));
            REQUIRE(expected.TryAppend(StringView(longest)));
            CHECK(path.View() == expected.View());
        }

        CHECK(registry.InspectLifecycle(access, package.Value()).Error() == ErrorCode::StaleHandle);
        CHECK(registry.Stats().liveObjects == 0);
        CHECK(registry.Stats().namedObjects == 0);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 비ASCII 이름의 조회는 비교 키를 할당하므로 OutOfMemory를 낼 수 있고, Create는 그 실패가
// AlreadyExists 보다 먼저다. ASCII 이름은 할당하지 않는다.
TEST(ObjectIdentityInner_LookupKeyAllocationFailureIsOutOfMemory) {
    SystemAllocator system;
    {
        rocks::test::NFailAllocator fault(system);
        TypeRegistry types(system);
        REQUIRE(Register(types));
        ObjectRegistry registry(fault, types, {0x4f4904, 0x10});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[3];
        auto outer = NewNode(registry, {}, {}, &roots[0]);
        REQUIRE(outer);
        auto cheese = NewNode(registry, outer.Value(), "K\xC3\xA4se", &roots[1]);
        auto plain = NewNode(registry, outer.Value(), "Plain", &roots[2]);
        REQUIRE(cheese && plain);

        fault.FailOnAttempt(1);
        CHECK(registry.FindChild(access, outer.Value(), "K\xC3\x84SE").Error() ==
              ErrorCode::OutOfMemory);
        fault.FailOnAttempt(1);
        CHECK(registry.IsNameTaken(access, outer.Value(), "k\xC3\xA4se").Error() ==
              ErrorCode::OutOfMemory);
        fault.FailOnAttempt(1);
        CHECK(registry.FindObject(access, outer.Value(), "K\xC3\xA4se").Error() ==
              ErrorCode::OutOfMemory);
        fault.FailOnAttempt(1);
        auto found = registry.FindChild(access, outer.Value(), "PLAIN");
        CHECK(found && found.Value() == plain.Value());
        auto taken = registry.IsNameTaken(access, outer.Value(), "plain");
        CHECK(taken && taken.Value());
        found = registry.FindObject(access, outer.Value(), "pLaIn");
        CHECK(found && found.Value() == plain.Value());
        CHECK(fault.Attempts() == 0);
        fault.DisableFailure();
        found = registry.FindChild(access, outer.Value(), "K\xC3\x84SE");
        CHECK(found && found.Value() == cheese.Value());

        // Create: 저널·비교 키 할당 실패는 OutOfMemory이고, 할당이 모두 되면 AlreadyExists다.
        const ObjectStats before = registry.Stats();
        uint32 outOfMemory = 0;
        bool exists = false;
        for (uint64 attempt = 1; attempt < 64 && !exists; ++attempt) {
            fault.FailOnAttempt(attempt);
            auto made = registry.NewObject<Object>(access, NodeType, outer.Value(), "K\xC3\x84SE");
            fault.DisableFailure();
            REQUIRE(!made);
            if (made.Error() == ErrorCode::AlreadyExists) {
                exists = true;
            } else {
                CHECK(made.Error() == ErrorCode::OutOfMemory);
                ++outOfMemory;
            }

            CHECK(registry.Stats().namedObjects == before.namedObjects);
            CHECK(registry.Stats().reservedObjects == 0);
        }

        CHECK(exists);
        CHECK(outOfMemory >= 2);
        for (auto& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 슬롯 1만 개(chunk 157개)에서 한 Outer의 자식을 모은다. 고리는 Slot*라 순회가 chunk 수에 비례하지
// 않는다(H2 — 시간은 재지 않고 규모에서의 정확성만 본다). 생성 순서의 역순이 이름 순서다.
TEST(ObjectIdentityInner_TenThousandChildrenAcrossManyChunks) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {0x4f4905, 0x10});
        const auto access = registry.OwnerAccess();
        constexpr uint32 Count = 10000;
        Array<ObjectHandle> created(allocator);
        REQUIRE(created.TryResize(Count));
        ObjectHandle outer;
        RootRef<Object> outerRoot[1];
        {
            ConstructionContext transaction(registry, access);
            auto made = transaction.Create<InnerNode>(NodeType);
            REQUIRE(made);
            outer = made.Value();
            for (uint32 index = 0; index < Count; ++index) {
                const uint32 ordinal = Count - 1 - index;
                char name[16];
                snprintf(name, sizeof(name), "n%05u", ordinal);
                auto child = transaction.Create<InnerNode>(NodeType, outer, name);
                REQUIRE(child);
                created[ordinal] = child.Value();
            }

            const ObjectHandle roots[] = {outer};
            REQUIRE(transaction.Commit(roots, outerRoot));
        }

        CHECK(registry.Stats().chunks >= (Count + 1 + 63) / 64);
        CHECK(registry.Stats().namedObjects == Count);
        Array<ObjectHandle> direct(allocator);
        REQUIRE(registry.CollectInners(access, outer, InnerOrder::Direct, direct));
        CHECK(Same(direct, created.View()));
        Array<ObjectHandle> depthFirst(allocator);
        REQUIRE(registry.CollectInners(access, outer, InnerOrder::DepthFirst, depthFirst));
        CHECK(Same(depthFirst, created.View()));
        auto found = registry.FindChild(access, outer, "N04242");
        CHECK(found && found.Value() == created[4242]);
        outerRoot[0].Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
