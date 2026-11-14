#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/TypeRegistry.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

// A0-Oa Outer·이름·정책·다중 root Commit 시험이다(Docs/Design/ObjectFoundations.ko.md 1절·9절 Oa).
// GC·색인·할당 실패·재진입은 ObjectIdentityGcTests.cpp가 본다.
using namespace rocks;

namespace {

constexpr TypeId NodeType{0x4f49444e4f444531ULL, 1};
constexpr TypeId PackageType{0x4f4944504b474531ULL, 1};
constexpr TypeId AssetType{0x4f49444153534531ULL, 1};
constexpr TypeId WorldType{0x4f4944574f524c31ULL, 1};
constexpr TypeId WorldDerivedType{0x4f4944574f524c32ULL, 1};

struct IdentityNode final : Object {
    ObjectRef<IdentityNode> link;

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(link);
    }
};

struct IdentityPackage final : Object {};

struct IdentityAsset final : Object {};

struct IdentityWorld : Object {};

struct IdentityWorldDerived final : IdentityWorld {};

bool RegisterTypes(TypeRegistry& types) noexcept {
    if (!types.Register({NodeType, 1, {}, {}}, MakeRuntimeTypeOps<IdentityNode>())) {
        return false;
    }

    if (!types.Register({PackageType, 1, {}, {}}, MakeRuntimeTypeOps<IdentityPackage>())) {
        return false;
    }

    if (!types.Register({AssetType, 1, {}, {}}, MakeRuntimeTypeOps<IdentityAsset>())) {
        return false;
    }

    TypeSchema world{WorldType, 1, {}, {}};
    world.flags = static_cast<uint32>(TypeFlags::WorldOnly);
    if (!types.Register(world, MakeRuntimeTypeOps<IdentityWorld>())) {
        return false;
    }

    // WorldOnly는 상속된다(파생 등록에도 플래그가 있어야 한다).
    TypeSchema derived{WorldDerivedType, 1, {}, {}};
    derived.baseType = WorldType;
    derived.flags = static_cast<uint32>(TypeFlags::WorldOnly);
    RuntimeTypeOps derivedOps = MakeRuntimeTypeOps<IdentityWorldDerived>();
    derivedOps.nativeBaseType = NativeTypeKey<IdentityWorld>();
    return static_cast<bool>(types.Register(derived, Move(derivedOps)));
}

RegistryOptions PackagePolicy() noexcept {
    RegistryOptions options;
    options.policy.packageNativeType = NativeTypeKey<IdentityPackage>();
    options.policy.rejectWorldOnly = true;
    return options;
}

Result<ObjectHandle> NewNode(ObjectRegistry& registry, ObjectHandle outer, StringView name,
                             RootRef<Object>& root) noexcept {
    auto made = registry.NewObject<Object>(registry.OwnerAccess(), NodeType, outer, name);
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    root = Move(made.Value());
    return Result<ObjectHandle>::Success(root.Handle());
}

ErrorCode NewNodeError(ObjectRegistry& registry, ObjectHandle outer, StringView name) noexcept {
    auto made = registry.NewObject<Object>(registry.OwnerAccess(), NodeType, outer, name);
    return made ? ErrorCode::None : made.Error();
}

bool NameIs(ObjectRegistry& registry, ObjectHandle handle, StringView expected,
            Allocator& allocator) noexcept {
    String name(allocator);
    return registry.GetName(registry.OwnerAccess(), handle, name) && name.View() == expected;
}

bool PathIs(ObjectRegistry& registry, ObjectHandle handle, StringView expected,
            Allocator& allocator) noexcept {
    String path(allocator);
    return registry.GetObjectPath(registry.OwnerAccess(), handle, path) && path.View() == expected;
}

bool Finds(const Result<ObjectHandle>& found, ObjectHandle expected) noexcept {
    return found && found.Value() == expected;
}

} // namespace

TEST(ObjectIdentity_NamesAreUniqueIgnoringCaseAndNormalizationWithinAnOuter) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f4901, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[12];
        auto outer = NewNode(registry, {}, {}, roots[0]);
        auto other = NewNode(registry, {}, {}, roots[1]);
        REQUIRE(outer && other);
        auto body = NewNode(registry, outer.Value(), "Body", roots[2]);
        REQUIRE(body);
        CHECK(registry.Stats().namedObjects == 1);

        // 대소문자만 다른 이름은 같은 이름이다. 원래 표기는 그대로 남는다.
        CHECK(NewNodeError(registry, outer.Value(), "body") == ErrorCode::AlreadyExists);
        CHECK(NewNodeError(registry, outer.Value(), "BODY") == ErrorCode::AlreadyExists);
        auto taken = registry.IsNameTaken(access, outer.Value(), "bOdY");
        CHECK(taken && taken.Value());
        CHECK(Finds(registry.FindChild(access, outer.Value(), "BODY"), body.Value()));
        CHECK(NameIs(registry, body.Value(), "Body", allocator));

        // NFC끼리의 caseless 동치: Å(U+00C5)·å(U+00E5), Ǆ(U+01C4)·ǅ(U+01C5).
        REQUIRE(NewNode(registry, outer.Value(), "\xC3\x85sa", roots[3]));
        CHECK(NewNodeError(registry, outer.Value(), "\xC3\xA5SA") == ErrorCode::AlreadyExists);
        REQUIRE(NewNode(registry, outer.Value(), "\xC7\x84ok", roots[4]));
        CHECK(NewNodeError(registry, outer.Value(), "\xC7\x85OK") == ErrorCode::AlreadyExists);

        // 단순 folding은 ß를 ss로 바꾸지 않으므로 Straße와 STRASSE는 공존한다.
        REQUIRE(NewNode(registry, outer.Value(), "Straße", roots[5]));
        REQUIRE(NewNode(registry, outer.Value(), "STRASSE", roots[6]));

        // 다른 Outer는 다른 이름 공간이다. 클래스는 키가 아니다.
        auto otherBody = NewNode(registry, other.Value(), "Body", roots[7]);
        REQUIRE(otherBody);
        CHECK(Finds(registry.FindChild(access, other.Value(), "body"), otherBody.Value()));
        auto asset = registry.NewObject<Object>(access, AssetType, outer.Value(), "BODY");
        CHECK(!asset && asset.Error() == ErrorCode::AlreadyExists);

        // Outer 없는 이름 있는 객체는 최상위 이름 공간에서 유일하다.
        auto top = NewNode(registry, {}, "Top", roots[8]);
        REQUIRE(top);
        CHECK(NewNodeError(registry, {}, "TOP") == ErrorCode::AlreadyExists);
        CHECK(Finds(registry.FindChild(access, {}, "top"), top.Value()));
        CHECK(NewNodeError(registry, {}, "Body") == ErrorCode::None);
        CHECK(registry.FindChild(access, outer.Value(), "Missing").Error() == ErrorCode::NotFound);
        CHECK(registry.FindChild(access, outer.Value(), "a.b").Error() ==
              ErrorCode::InvalidArgument);
        CHECK(registry.FindChild(access, {}, "").Error() == ErrorCode::InvalidArgument);
        CHECK(NewNodeError(registry, outer.Value(), "bad name") == ErrorCode::InvalidArgument);
        // Registry는 정규화하지 않는다: 분해형 Å는 키가 이미 있는 NFC형과 같아도 형식 오류다.
        CHECK(NewNodeError(registry, outer.Value(), "A\xCC\x8Asa") == ErrorCode::InvalidArgument);
        CHECK(registry.FindChild(access, outer.Value(), "A\xCC\x8Asa").Error() ==
              ErrorCode::InvalidArgument);
        CHECK(registry.IsNameTaken(access, outer.Value(), "A\xCC\x8Asa").Error() ==
              ErrorCode::InvalidArgument);
        CHECK(registry.Stats().namedObjects == 8);

        for (auto& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        CHECK(registry.Stats().namedObjects == 0);
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 공개 전 객체도 생성 때부터 이름을 점유한다. 조회(FindChild)는 공개된 객체만 보고, rollback하면
// 이름이 바로 빈다.
TEST(ObjectIdentity_ReservedNamesBlockOtherTransactionsAndRollbackFreesThem) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f4902, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> outerRoot;
        auto outer = NewNode(registry, {}, {}, outerRoot);
        REQUIRE(outer);
        {
            ConstructionContext first(registry, access);
            auto reserved = first.Create<IdentityNode>(NodeType, outer.Value(), "Shared");
            REQUIRE(reserved);
            CHECK(first.TransactionId() != 0);
            {
                ConstructionContext second(registry, access);
                auto clash = second.Create<IdentityNode>(NodeType, outer.Value(), "SHARED");
                CHECK(!clash && clash.Error() == ErrorCode::AlreadyExists);
                // 생성 실패는 트랜잭션을 오류 상태로 고정한다.
                auto after = second.Create<IdentityNode>(NodeType, outer.Value(), "Else");
                CHECK(!after && after.Error() == ErrorCode::AlreadyExists);
            }

            auto taken = registry.IsNameTaken(access, outer.Value(), "shared");
            CHECK(taken && taken.Value());
            CHECK(registry.FindChild(access, outer.Value(), "Shared").Error() ==
                  ErrorCode::NotFound);
            String reservedName(allocator);
            CHECK(registry.GetName(access, reserved.Value(), reservedName).Error() ==
                  ErrorCode::StaleHandle);
            // 공개 전 Outer 아래 이름도 점유 기준으로 본다.
            auto inner = first.Create<IdentityNode>(NodeType, reserved.Value(), "Inner");
            REQUIRE(inner);
            auto innerTaken = registry.IsNameTaken(access, reserved.Value(), "INNER");
            CHECK(innerTaken && innerTaken.Value());
            {
                // 다른 트랜잭션이 열려 있을 때도 같다(점유 질의는 어느 트랜잭션의 Reserved든 본다).
                ConstructionContext other(registry, access);
                REQUIRE(other.Create<IdentityNode>(NodeType));
                auto foreignTaken = registry.IsNameTaken(access, reserved.Value(), "inner");
                CHECK(foreignTaken && foreignTaken.Value());
                auto foreignFree = registry.IsNameTaken(access, reserved.Value(), "Other");
                CHECK(foreignFree && !foreignFree.Value());
            }
            CHECK(registry.Stats().namedObjects == 2);
        }

        CHECK(registry.Stats().namedObjects == 0);
        auto freed = registry.IsNameTaken(access, outer.Value(), "Shared");
        CHECK(freed && !freed.Value());
        RootRef<Object> reused;
        CHECK(NewNode(registry, outer.Value(), "shared", reused));
        reused.Reset();
        outerRoot.Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIdentity_EmptyNamesAndRepresentativeChildRules) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f4903, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[8];
        // Outer 없는 빈 이름은 여럿이어도 되고 색인에 넣지 않는다.
        auto package = NewNode(registry, {}, {}, roots[0]);
        auto second = NewNode(registry, {}, {}, roots[1]);
        REQUIRE(package && second);
        CHECK(registry.Stats().namedObjects == 0);

        // 대표 자식은 outermost의 유일한 자식이다.
        auto representative = NewNode(registry, package.Value(), {}, roots[2]);
        REQUIRE(representative);
        CHECK(NewNodeError(registry, package.Value(), {}) == ErrorCode::AlreadyExists);
        CHECK(NewNodeError(registry, package.Value(), "Named") == ErrorCode::InvalidArgument);
        auto repTaken = registry.IsNameTaken(access, package.Value(), {});
        CHECK(repTaken && repTaken.Value());
        CHECK(Finds(registry.FindChild(access, package.Value(), {}), representative.Value()));
        auto summary = registry.DescribeOuter(access, package.Value());
        REQUIRE(summary);
        CHECK(summary.Value().isOutermost);
        CHECK(summary.Value().hasRepresentative);
        CHECK(!summary.Value().hasNamedInners);
        CHECK(summary.Value().outermost == package.Value());
        CHECK(summary.Value().pathBytes == 0);

        // 이름 있는 Inner가 있는 outermost에는 대표 자식을 둘 수 없다.
        auto named = NewNode(registry, second.Value(), "Named", roots[3]);
        REQUIRE(named);
        CHECK(NewNodeError(registry, second.Value(), {}) == ErrorCode::InvalidArgument);
        auto secondSummary = registry.DescribeOuter(access, second.Value());
        REQUIRE(secondSummary);
        CHECK(secondSummary.Value().hasNamedInners && !secondSummary.Value().hasRepresentative);

        // 깊은 객체의 빈 이름은 거부한다. Outer 없는 빈 이름의 조회·점유 질의는 InvalidArgument다.
        CHECK(NewNodeError(registry, representative.Value(), {}) == ErrorCode::InvalidArgument);
        CHECK(NewNodeError(registry, named.Value(), {}) == ErrorCode::InvalidArgument);
        CHECK(registry.IsNameTaken(access, {}, {}).Error() == ErrorCode::InvalidArgument);
        auto deep = registry.DescribeOuter(access, representative.Value());
        REQUIRE(deep);
        CHECK(!deep.Value().isOutermost && deep.Value().outermost == package.Value());
        CHECK(registry.DescribeOuter(access, {}).Error() == ErrorCode::InvalidArgument);
        CHECK(registry.Stats().namedObjects == 2);
        for (auto& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIdentity_ObjectPathsJoinNamesBelowTheRepresentative) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f4904, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[8];
        auto package = NewNode(registry, {}, {}, roots[0]);
        REQUIRE(package);
        auto asset = NewNode(registry, package.Value(), {}, roots[1]);
        REQUIRE(asset);
        auto body = NewNode(registry, asset.Value(), "Body", roots[2]);
        REQUIRE(body);
        auto socket = NewNode(registry, body.Value(), "Socket", roots[3]);
        REQUIRE(socket);

        CHECK(PathIs(registry, socket.Value(), "Body.Socket", allocator));
        CHECK(PathIs(registry, body.Value(), "Body", allocator));
        CHECK(PathIs(registry, asset.Value(), "", allocator));
        CHECK(PathIs(registry, package.Value(), "", allocator));
        CHECK(NameIs(registry, asset.Value(), "", allocator));
        auto summary = registry.DescribeOuter(access, socket.Value());
        REQUIRE(summary);
        CHECK(summary.Value().pathBytes == 11);

        // 빈 경로는 대표 자식이고 비교는 대소문자를 무시한다.
        CHECK(Finds(registry.FindObject(access, package.Value(), ""), asset.Value()));
        CHECK(Finds(registry.FindObject(access, package.Value(), "body"), body.Value()));
        CHECK(Finds(registry.FindObject(access, package.Value(), "BODY.socket"), socket.Value()));
        CHECK(registry.FindObject(access, package.Value(), "Body.Missing").Error() ==
              ErrorCode::NotFound);
        CHECK(registry.FindObject(access, package.Value(), "Missing.Socket").Error() ==
              ErrorCode::NotFound);
        // 형식은 조회 전에 모두 본다(앞 구성 요소가 없어도 뒤의 형식 오류가 먼저다).
        CHECK(registry.FindObject(access, package.Value(), "Missing.bad name").Error() ==
              ErrorCode::InvalidArgument);
        CHECK(registry.FindObject(access, package.Value(), "Body..Socket").Error() ==
              ErrorCode::InvalidArgument);
        CHECK(registry.FindObject(access, package.Value(), ".Body").Error() ==
              ErrorCode::InvalidArgument);
        CHECK(registry.FindObject(access, package.Value(), "Body.").Error() ==
              ErrorCode::InvalidArgument);
        CHECK(registry.FindObject(access, body.Value(), "Socket").Error() ==
              ErrorCode::InvalidArgument);
        char tooLong[MaxObjectPathBytes + 1];
        for (char& character : tooLong) {
            character = 'a';
        }

        CHECK(registry.FindObject(access, package.Value(), StringView(tooLong, sizeof(tooLong)))
                  .Error() == ErrorCode::InvalidArgument);

        // 대표 자식이 없는 outermost는 그 바로 아래에서 경로가 시작하고 빈 경로는 NotFound다.
        auto plain = NewNode(registry, {}, {}, roots[4]);
        REQUIRE(plain);
        auto direct = NewNode(registry, plain.Value(), "Direct", roots[5]);
        REQUIRE(direct);
        CHECK(PathIs(registry, direct.Value(), "Direct", allocator));
        CHECK(Finds(registry.FindObject(access, plain.Value(), "direct"), direct.Value()));
        CHECK(registry.FindObject(access, plain.Value(), "").Error() == ErrorCode::NotFound);

        auto outer = registry.GetOuter(access, socket.Value());
        CHECK(Finds(outer, body.Value()));
        CHECK(Finds(registry.GetOutermost(access, socket.Value()), package.Value()));
        auto none = registry.GetOuter(access, package.Value());
        CHECK(none && none.Value().IsNull());
        CHECK(Finds(registry.GetOutermost(access, package.Value()), package.Value()));
        for (auto& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 경로는 4 KiB 이하이고, 1 byte 이름이면 Outer 깊이는 2048이 한도다.
TEST(ObjectIdentity_PathLengthIsBoundedAt4KiBAndDeepChainsResolve) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f4905, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> outermostRoot;
        auto outermost = NewNode(registry, {}, {}, outermostRoot);
        REQUIRE(outermost);
        constexpr usize Depth = 2048;
        ObjectHandle chain[Depth];
        RootRef<Object> leafRoot;
        {
            ConstructionContext transaction(registry, access);
            ObjectHandle outer = outermost.Value();
            for (usize level = 0; level < Depth; ++level) {
                auto made = transaction.Create<IdentityNode>(NodeType, outer, "a");
                REQUIRE(made);
                chain[level] = made.Value();
                outer = made.Value();
            }

            // 가장 깊은 객체 하나만 root여도 inner → outer로 사슬 전체가 산다.
            const ObjectHandle leaf[] = {chain[Depth - 1]};
            RootRef<Object> held[1];
            REQUIRE(transaction.Commit(leaf, held));
            leafRoot = Move(held[0]);
        }

        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().liveObjects == Depth + 1);
        // 2047 수준의 경로는 4093 byte: "bb"는 4096(성공), "ccc"는 4097(CapacityExceeded).
        auto level2047 = registry.DescribeOuter(access, chain[Depth - 2]);
        REQUIRE(level2047);
        CHECK(level2047.Value().pathBytes == 4093);
        RootRef<Object> boundary;
        CHECK(NewNode(registry, chain[Depth - 2], "bb", boundary));
        CHECK(NewNodeError(registry, chain[Depth - 2], "ccc") == ErrorCode::CapacityExceeded);
        CHECK(NewNodeError(registry, chain[Depth - 1], "a") == ErrorCode::CapacityExceeded);

        String path(allocator);
        REQUIRE(registry.GetObjectPath(access, chain[Depth - 1], path));
        CHECK(path.ByteCount() == 4095);
        CHECK(Finds(registry.FindObject(access, outermost.Value(), path.View()), chain[Depth - 1]));
        String boundaryPath(allocator);
        REQUIRE(registry.GetObjectPath(access, boundary.Handle(), boundaryPath));
        CHECK(boundaryPath.ByteCount() == MaxObjectPathBytes);

        // 깊은 사슬도 스택 없이 모은다(전위 순서 = 사슬 순서, "bb"는 2047 수준 바로 뒤).
        Array<ObjectHandle> inners(allocator);
        REQUIRE(registry.CollectInners(access, outermost.Value(), InnerOrder::DepthFirst, inners));
        REQUIRE(inners.Size() == Depth + 1);
        bool ordered = true;
        for (usize level = 0; level + 1 < Depth; ++level) {
            ordered = ordered && inners[level] == chain[level];
        }

        CHECK(ordered);
        CHECK(inners[Depth - 1] == chain[Depth - 1]);
        CHECK(inners[Depth] == boundary.Handle());
        Array<ObjectHandle> direct(allocator);
        REQUIRE(registry.CollectInners(access, outermost.Value(), InnerOrder::Direct, direct));
        CHECK(direct.Size() == 1 && direct[0] == chain[0]);

        boundary.Reset();
        leafRoot.Reset();
        outermostRoot.Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// RequestDestroy는 이름을 남긴다(UE의 MarkAsGarbage와 같다). 공개 전·무효 handle은 StaleHandle이다.
TEST(ObjectIdentity_DestroyRequestedObjectsKeepTheirIdentity) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f4906, 1});
        ObjectRegistry foreign(allocator, types, {0x4f4906, 2});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[3];
        auto outer = NewNode(registry, {}, {}, roots[0]);
        REQUIRE(outer);
        auto doomed = NewNode(registry, outer.Value(), "Doomed", roots[1]);
        REQUIRE(doomed);
        auto inner = NewNode(registry, doomed.Value(), "Inner", roots[2]);
        REQUIRE(inner);
        REQUIRE(registry.RequestDestroy(access, doomed.Value()));

        CHECK(NameIs(registry, doomed.Value(), "Doomed", allocator));
        CHECK(PathIs(registry, inner.Value(), "Doomed.Inner", allocator));
        CHECK(Finds(registry.FindChild(access, outer.Value(), "doomed"), doomed.Value()));
        CHECK(Finds(registry.FindChild(access, doomed.Value(), "inner"), inner.Value()));
        CHECK(Finds(registry.GetOuter(access, doomed.Value()), outer.Value()));
        auto taken = registry.IsNameTaken(access, outer.Value(), "Doomed");
        CHECK(taken && taken.Value());
        // 파괴 요청된 객체는 새 Outer가 될 수 없다(생성 규칙 5).
        CHECK(NewNodeError(registry, doomed.Value(), "New") == ErrorCode::StaleHandle);
        CHECK(registry.DescribeOuter(access, doomed.Value()).Error() == ErrorCode::StaleHandle);
        // DestroyRequested Inner는 모으지 않는다(그 부분 트리째).
        Array<ObjectHandle> inners(allocator);
        REQUIRE(registry.CollectInners(access, outer.Value(), InnerOrder::DepthFirst, inners));
        CHECK(inners.Size() == 0);

        // 다른 Registry·무효 handle.
        RootRef<Object> foreignRoot;
        auto alien = NewNode(foreign, {}, "Alien", foreignRoot);
        REQUIRE(alien);
        String scratch(allocator);
        CHECK(registry.GetName(access, alien.Value(), scratch).Error() == ErrorCode::StaleHandle);
        CHECK(registry.FindChild(access, alien.Value(), "x").Error() == ErrorCode::StaleHandle);
        CHECK(registry.GetOuter(access, ObjectHandle{1, 2, 3}).Error() == ErrorCode::StaleHandle);
        CHECK(NewNodeError(registry, alien.Value(), "x") == ErrorCode::StaleHandle);
        foreignRoot.Reset();
        REQUIRE(foreign.Shutdown(foreign.OwnerAccess()));
        for (auto& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

struct ForeignLookup {
    ObjectRegistry* registry = nullptr;
    ObjectHandle outer;
    ErrorCode findChild = ErrorCode::None;
    ErrorCode describe = ErrorCode::None;
};

void LookupFromForeignThread(void* context) noexcept {
    auto& state = *static_cast<ForeignLookup*>(context);
    const auto access = state.registry->OwnerAccess();
    state.findChild = state.registry->FindChild(access, state.outer, "x").Error();
    state.describe = state.registry->DescribeOuter(access, state.outer).Error();
}

} // namespace

TEST(ObjectIdentity_OuterStateRulesAndAccessChecks) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f4907, 1});
        const auto access = registry.OwnerAccess();
        const ObjectAccessContext wrong{{0x4f4907, 99}};
        RootRef<Object> outerRoot;
        auto outer = NewNode(registry, {}, {}, outerRoot);
        REQUIRE(outer);
        {
            ConstructionContext first(registry, access);
            auto reserved = first.Create<IdentityNode>(NodeType);
            REQUIRE(reserved);
            // 같은 트랜잭션의 공개 전 Outer는 된다(요약도 받는다).
            auto summary = first.DescribeOuter(reserved.Value());
            REQUIRE(summary);
            CHECK(summary.Value().isOutermost && summary.Value().outermost == reserved.Value());
            CHECK(first.Create<IdentityNode>(NodeType, reserved.Value(), "Child"));
            // 다른 트랜잭션의 공개 전 Outer는 StaleHandle이다.
            ConstructionContext second(registry, access);
            auto foreign = second.Create<IdentityNode>(NodeType, reserved.Value(), "Child");
            CHECK(!foreign && foreign.Error() == ErrorCode::StaleHandle);
            CHECK(registry.DescribeOuter(access, reserved.Value()).Error() ==
                  ErrorCode::StaleHandle);
            // 고정된 오류는 Rollback 뒤에도 남는다(트랜잭션 상태 규칙).
            second.Rollback();
            CHECK(second.DescribeOuter(outer.Value()).Error() == ErrorCode::StaleHandle);
            CHECK(second.TransactionId() == 0);
        }

        // 다른 접근 문맥은 WrongContext다(생성은 시작 실패가 고정된 오류다).
        auto mismatched = registry.NewObject<Object>(wrong, NodeType, outer.Value(), "x");
        CHECK(!mismatched && mismatched.Error() == ErrorCode::WrongContext);
        CHECK(registry.FindChild(wrong, outer.Value(), "x").Error() == ErrorCode::WrongContext);
        CHECK(registry.DescribeOuter(wrong, outer.Value()).Error() == ErrorCode::WrongContext);
        Array<ObjectHandle> inners(allocator);
        CHECK(registry.CollectInners(wrong, outer.Value(), InnerOrder::Direct, inners).Error() ==
              ErrorCode::WrongContext);

        // 다른 스레드는 WrongThread다.
        ForeignLookup lookup;
        lookup.registry = &registry;
        lookup.outer = outer.Value();
        platform::Thread thread;
        REQUIRE(thread.Start(LookupFromForeignThread, &lookup) == platform::ThreadStatus::Success);
        if (thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            thread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Object identity lookup thread failed to join");
        }

        CHECK(lookup.findChild == ErrorCode::WrongThread);
        CHECK(lookup.describe == ErrorCode::WrongThread);
        outerRoot.Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIdentity_PackagePolicyRulesAndWorldOnlyRejection) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f4908, 1}, PackagePolicy());
        const auto access = registry.OwnerAccess();
        CHECK(registry.Policy() == PackagePolicy().policy);
        auto package = registry.NewObject<IdentityPackage>(access, PackageType);
        REQUIRE(package);
        // 패키지는 이름·Outer가 없다.
        auto named = registry.NewObject<IdentityPackage>(access, PackageType, {}, "Named");
        CHECK(!named && named.Error() == ErrorCode::InvalidArgument);
        {
            ConstructionContext transaction(registry, access);
            auto nested =
                transaction.Create<IdentityPackage>(PackageType, package.Value().Handle());
            CHECK(!nested && nested.Error() == ErrorCode::InvalidArgument);
        }

        // 패키지가 아닌 객체는 Outer가 있어야 한다(NewObject에도 같은 규칙).
        auto orphan = registry.NewObject<IdentityAsset>(access, AssetType);
        CHECK(!orphan && orphan.Error() == ErrorCode::InvalidArgument);
        auto namedOrphan = registry.NewObject<IdentityAsset>(access, AssetType, {}, "Orphan");
        CHECK(!namedOrphan && namedOrphan.Error() == ErrorCode::InvalidArgument);

        // 패키지 아래 대표 에셋과 하위 객체.
        auto asset =
            registry.NewObject<IdentityAsset>(access, AssetType, package.Value().Handle(), {});
        REQUIRE(asset);
        auto body =
            registry.NewObject<IdentityAsset>(access, AssetType, asset.Value().Handle(), "Body");
        REQUIRE(body);
        auto summary = registry.DescribeOuter(access, body.Value().Handle());
        REQUIRE(summary);
        CHECK(summary.Value().outermostIsPackage);
        CHECK(summary.Value().outermost == package.Value().Handle());

        // WorldOnly(상속 포함)는 Unsupported이고 패키지 규칙(8단계)보다 먼저다.
        auto world =
            registry.NewObject<IdentityWorld>(access, WorldType, asset.Value().Handle(), "World");
        CHECK(!world && world.Error() == ErrorCode::Unsupported);
        auto derived = registry.NewObject<IdentityWorldDerived>(access, WorldDerivedType,
                                                                asset.Value().Handle(), "Derived");
        CHECK(!derived && derived.Error() == ErrorCode::Unsupported);
        auto worldOrphan = registry.NewObject<IdentityWorld>(access, WorldType);
        CHECK(!worldOrphan && worldOrphan.Error() == ErrorCode::Unsupported);

        body.Value().Reset();
        asset.Value().Reset();
        package.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 규칙 6–9 순수 함수는 Registry 없이 Outer 요약으로 판정한다(그래프 1단계가 같은 함수를 쓴다).
TEST(ObjectIdentity_CheckNewObjectIdentityIsAPureRuleFunction) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        auto package = types.Acquire(PackageType);
        auto asset = types.Acquire(AssetType);
        auto world = types.Acquire(WorldDerivedType);
        REQUIRE(package && asset && world);
        const RegistryPolicy policy = PackagePolicy().policy;
        const RegistryPolicy none{};
        const OuterSummary newPackage = OuterSummary::ForNewOutermost(true);
        OuterSummary notPackage = OuterSummary::ForNewOutermost(false);

        CHECK(detail::CheckNewObjectIdentity(policy, package.Value(), nullptr, {}, allocator));
        CHECK(detail::CheckNewObjectIdentity(policy, asset.Value(), &newPackage, {}, allocator));
        CHECK(
            detail::CheckNewObjectIdentity(policy, asset.Value(), &newPackage, "Body", allocator));
        // outermost가 패키지가 아니면 거부한다.
        CHECK(detail::CheckNewObjectIdentity(policy, asset.Value(), &notPackage, "Body", allocator)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(detail::CheckNewObjectIdentity(none, asset.Value(), &notPackage, "Body", allocator));
        CHECK(detail::CheckNewObjectIdentity(policy, world.Value(), &newPackage, "W", allocator)
                  .Error() == ErrorCode::Unsupported);
        CHECK(detail::CheckNewObjectIdentity(none, world.Value(), nullptr, {}, allocator));
        // 이름 형식(7)은 패키지 규칙(8)보다 먼저다.
        CHECK(
            detail::CheckNewObjectIdentity(policy, package.Value(), nullptr, "bad name", allocator)
                .Error() == ErrorCode::InvalidArgument);
        CHECK(detail::CheckNewObjectIdentity(none, asset.Value(), nullptr, "CON", allocator)
                  .Error() == ErrorCode::InvalidArgument);
        // 대표 자식 규칙과 경로 길이(9).
        notPackage.hasNamedInners = true;
        CHECK(detail::CheckNewObjectIdentity(none, asset.Value(), &notPackage, {}, allocator)
                  .Error() == ErrorCode::InvalidArgument);
        notPackage.hasNamedInners = false;
        notPackage.hasRepresentative = true;
        CHECK(detail::CheckNewObjectIdentity(none, asset.Value(), &notPackage, "Named", allocator)
                  .Error() == ErrorCode::InvalidArgument);
        OuterSummary deep;
        deep.pathBytes = MaxObjectPathBytes - 3;
        CHECK(detail::CheckNewObjectIdentity(none, asset.Value(), &deep, "ab", allocator));
        CHECK(
            detail::CheckNewObjectIdentity(none, asset.Value(), &deep, "abc", allocator).Error() ==
            ErrorCode::CapacityExceeded);
        CHECK(detail::CheckNewObjectIdentity(none, asset.Value(), &deep, {}, allocator).Error() ==
              ErrorCode::InvalidArgument);
        package.Value().Reset();
        asset.Value().Reset();
        world.Value().Reset();
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 정책 없는 Registry는 이전처럼 Outer 없는 객체·WorldOnly 타입·이름 있는 패키지 타입을 받는다.
TEST(ObjectIdentity_RegistryWithoutPolicyKeepsPreviousCreationRules) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f4909, 1});
        const auto access = registry.OwnerAccess();
        CHECK(registry.Policy() == RegistryPolicy{});
        auto world = registry.NewObject<IdentityWorldDerived>(access, WorldDerivedType);
        auto asset = registry.NewObject<IdentityAsset>(access, AssetType);
        auto package = registry.NewObject<IdentityPackage>(access, PackageType, {}, "Named");
        REQUIRE(world && asset && package);
        auto nested = registry.NewObject<IdentityPackage>(access, PackageType,
                                                          package.Value().Handle(), "Nested");
        REQUIRE(nested);
        auto summary = registry.DescribeOuter(access, nested.Value().Handle());
        REQUIRE(summary);
        CHECK(!summary.Value().outermostIsPackage);
        world.Value().Reset();
        asset.Value().Reset();
        package.Value().Reset();
        nested.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIdentity_MultiRootCommitReturnsOneRootPerEntry) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f490a, 1});
        const auto access = registry.OwnerAccess();
        ObjectHandle first;
        ObjectHandle second;
        ObjectHandle inner;
        RootRef<Object> held[3];
        {
            ConstructionContext transaction(registry, access);
            auto a = transaction.Create<IdentityNode>(NodeType);
            auto b = transaction.Create<IdentityNode>(NodeType);
            REQUIRE(a && b);
            auto c = transaction.Create<IdentityNode>(NodeType, a.Value(), "Inner");
            REQUIRE(c);
            first = a.Value();
            second = b.Value();
            inner = c.Value();
            const uint64 id = transaction.TransactionId();
            CHECK(id != 0);
            // 같은 handle을 두 번 주면 RootRef도 둘이다.
            const ObjectHandle roots[] = {first, second, first};
            REQUIRE(transaction.Commit(roots, held));
            CHECK(transaction.TransactionId() == 0);
            CHECK(!transaction.Commit(roots, held));
        }

        CHECK(held[0].Handle() == first && held[1].Handle() == second && held[2].Handle() == first);
        CHECK(registry.Stats().roots == 3);
        CHECK(registry.Stats().liveObjects == 3);
        // 정책 없는 Registry에서 outer만 root면 inner는 회수된다.
        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().liveObjects == 2);
        CHECK(registry.InspectLifecycle(access, inner).Error() == ErrorCode::StaleHandle);
        CHECK(registry.FindChild(access, first, "Inner").Error() == ErrorCode::NotFound);
        held[0].Reset();
        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().liveObjects == 2);
        held[2].Reset();
        held[1].Reset();

        // roots가 비어도 된다. 붙잡는 것이 없으면 다음 GC에서 회수된다.
        {
            ConstructionContext transaction(registry, access);
            REQUIRE(transaction.Create<IdentityNode>(NodeType, {}, "Floating"));
            REQUIRE(transaction.Commit({}, {}));
        }

        CHECK(registry.FindChild(access, {}, "floating"));
        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().liveObjects == 0);
        CHECK(registry.FindChild(access, {}, "Floating").Error() == ErrorCode::NotFound);
        // 만든 객체가 없는 트랜잭션도 빈 roots로 끝낼 수 있다(기존 Commit()은 InvalidArgument).
        {
            ConstructionContext empty(registry, access);
            REQUIRE(empty.Commit({}, {}));
        }

        {
            ConstructionContext legacy(registry, access);
            CHECK(legacy.Commit().Error() == ErrorCode::InvalidArgument);
            // 실패한 Commit은 트랜잭션을 끝내지 않는다.
            CHECK(legacy.TransactionId() != 0);
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIdentity_MultiRootCommitFailuresKeepTheTransactionUnpublished) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f490b, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> existingRoot;
        auto existing = NewNode(registry, {}, "Existing", existingRoot);
        REQUIRE(existing);
        {
            ConstructionContext transaction(registry, access);
            auto made = transaction.Create<IdentityNode>(NodeType, {}, "Pending");
            REQUIRE(made);
            const ObjectHandle roots[] = {made.Value()};
            // out 크기 불일치·비어 있지 않은 out.
            CHECK(transaction.Commit(roots, {}).Error() == ErrorCode::InvalidArgument);
            RootRef<Object> occupied[1];
            auto clone = existingRoot.Clone();
            REQUIRE(clone);
            occupied[0] = Move(clone.Value());
            CHECK(transaction.Commit(roots, occupied).Error() == ErrorCode::InvalidArgument);
            CHECK(occupied[0].Handle() == existing.Value());
            occupied[0].Reset();
            // 이 트랜잭션 객체가 아닌 root(공개된 객체·다른 트랜잭션 객체)는 StaleHandle이다.
            const ObjectHandle published[] = {existing.Value()};
            RootRef<Object> out[1];
            CHECK(transaction.Commit(published, out).Error() == ErrorCode::StaleHandle);
            {
                ConstructionContext other(registry, access);
                auto foreign = other.Create<IdentityNode>(NodeType);
                REQUIRE(foreign);
                const ObjectHandle foreignRoots[] = {foreign.Value()};
                CHECK(transaction.Commit(foreignRoots, out).Error() == ErrorCode::StaleHandle);
            }

            CHECK(!out[0].IsValid());
            CHECK(registry.Stats().reservedObjects == 1);
            CHECK(registry.FindChild(access, {}, "Pending").Error() == ErrorCode::NotFound);
            // 실패 뒤에도 같은 트랜잭션을 다시 Commit할 수 있다.
            REQUIRE(transaction.Commit(roots, out));
            CHECK(Finds(registry.FindChild(access, {}, "pending"), made.Value()));
            out[0].Reset();
        }

        // staged 검증의 Outer: 생성 뒤 Outer가 파괴 요청되면 Commit은 StaleHandle이고 비공개로
        // 남는다.
        RootRef<Object> outerRoot;
        auto outer = NewNode(registry, {}, {}, outerRoot);
        REQUIRE(outer);
        {
            ConstructionContext transaction(registry, access);
            auto child = transaction.Create<IdentityNode>(NodeType, outer.Value(), "Child");
            REQUIRE(child);
            REQUIRE(registry.RequestDestroy(access, outer.Value()));
            const ObjectHandle roots[] = {child.Value()};
            RootRef<Object> out[1];
            CHECK(transaction.Commit(roots, out).Error() == ErrorCode::StaleHandle);
            CHECK(registry.Stats().reservedObjects == 1);
            auto taken = registry.IsNameTaken(access, outer.Value(), "Child");
            CHECK(taken && taken.Value());
            transaction.Rollback();
            CHECK(registry.Stats().reservedObjects == 0);
            auto freed = registry.IsNameTaken(access, outer.Value(), "Child");
            CHECK(freed && !freed.Value());
        }

        // 기존 Commit()은 첫 객체 하나를 root로 공개한다.
        {
            ConstructionContext transaction(registry, access);
            auto a = transaction.Create<IdentityNode>(NodeType, {}, "LegacyA");
            auto b = transaction.Create<IdentityNode>(NodeType, {}, "LegacyB");
            REQUIRE(a && b);
            auto root = transaction.Commit();
            REQUIRE(root);
            CHECK(root.Value().Handle() == a.Value());
            REQUIRE(registry.Collect(access));
            CHECK(registry.FindChild(access, {}, "LegacyB").Error() == ErrorCode::NotFound);
            CHECK(Finds(registry.FindChild(access, {}, "LegacyA"), a.Value()));
        }

        outerRoot.Reset();
        existingRoot.Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 트랜잭션 상태 규칙은 Create·Commit·DescribeOuter가 같다: 고정된 오류(시작 실패·실패한 Create)가
// 먼저이고 Rollback 뒤에도 남으며, 그 밖의 끝난 트랜잭션은 InvalidArgument다.
TEST(ObjectIdentity_TransactionStateRuleIsSharedByCreateCommitAndDescribeOuter) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f490c, 1});
        const auto access = registry.OwnerAccess();
        const ObjectAccessContext wrong{{0x4f490c, 99}};
        RootRef<Object> roots[2];
        auto outer = NewNode(registry, {}, {}, roots[0]);
        REQUIRE(outer);
        REQUIRE(NewNode(registry, outer.Value(), "Taken", roots[1]));
        auto expectState = [&](ConstructionContext& transaction, ErrorCode expected) noexcept {
            CHECK(transaction.Create<IdentityNode>(NodeType).Error() == expected);
            CHECK(transaction.DescribeOuter(outer.Value()).Error() == expected);
            CHECK(transaction.Commit().Error() == expected);
            CHECK(transaction.Commit({}, {}).Error() == expected);
        };
        {
            // 시작 실패(다른 접근 문맥)는 고정된 오류다.
            ConstructionContext mismatched(registry, wrong);
            CHECK(mismatched.TransactionId() == 0);
            expectState(mismatched, ErrorCode::WrongContext);
        }

        {
            ConstructionContext failed(registry, access);
            auto clash = failed.Create<IdentityNode>(NodeType, outer.Value(), "taken");
            CHECK(!clash && clash.Error() == ErrorCode::AlreadyExists);
            expectState(failed, ErrorCode::AlreadyExists);
            failed.Rollback();
            expectState(failed, ErrorCode::AlreadyExists);
        }

        {
            ConstructionContext committed(registry, access);
            REQUIRE(committed.Commit({}, {}));
            expectState(committed, ErrorCode::InvalidArgument);
        }

        {
            ConstructionContext rolledBack(registry, access);
            rolledBack.Rollback();
            expectState(rolledBack, ErrorCode::InvalidArgument);
        }

        for (auto& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
