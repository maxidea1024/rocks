#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// graph v2 저장 집합·NodeTable·편의 복원 도달 검사 시험이다(Docs/Design/ObjectFoundations.ko.md
// 5.1·5.2·5.5절, 9절 Od1): 호출자 순서 = local ID, 고아 노드, 집합 밖 참조 Reject, Include의 발견
// 순서·Outer 사슬·거부 조건(결정 17), NodeTable의 Outer·이름 왕복과 Outer 순서·기준 Outer 규칙,
// 도달 검사(inner → outer, 패키지 정책의 기준 Outer 아래 보존), prepare.
using namespace rocks;
using test::graph::SameBytes;

namespace {

constexpr TypeId LinkType{0x4753455454ULL, 1};
constexpr TypeId PackType{0x4753455454ULL, 2};
constexpr Guid NumberId{0x4753455446ULL, 1};
constexpr Guid LeftId{0x4753455446ULL, 2};
constexpr Guid RightId{0x4753455446ULL, 3};

// 생성자 실행 수의 단조 누계다(rollback으로 줄지 않는다).
uint32 ConstructedLinks = 0;

struct Link final : Object {
    explicit Link(Allocator&) noexcept {
        ++ConstructedLinks;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(left);
        visitor.Visit(right);
    }

    uint32 number = 0;
    ObjectRef<Link> left;
    ObjectRef<Link> right;
};

// 패키지 정책 Registry의 패키지 타입이다(필드 없음).
struct Pack final : Object {
    explicit Pack(Allocator&) noexcept {
    }
};

template <auto Member>
RuntimeFieldOps LinkField(Guid id) noexcept {
    return {id,
            [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
               FieldValue& value) noexcept {
                return detail::ReadGeneratedField<Link>(registry, access, object, Member, value);
            },
            [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
               const FieldValue& value) noexcept {
                return detail::WriteGeneratedField<Link>(registry, access, object, Member, value);
            },
            [](ConstructionContext& transaction, ObjectHandle object,
               const FieldValue& value) noexcept {
                return detail::WriteGeneratedStagedField<Link>(transaction, object, Member, value);
            },
            &detail::EncodeThroughFieldValue,
            &detail::DecodeThroughFieldValue};
}

bool RegisterTypes(TypeRegistry& types) noexcept {
    FieldSchema fields[3];
    fields[0].id = NumberId;
    fields[0].name = {"number", 6};
    fields[0].value.kind = ValueKind::UInt32;
    fields[1].id = LeftId;
    fields[1].name = {"left", 4};
    fields[1].kind = FieldKind::ObjectReference;
    fields[1].value = {ValueKind::ObjectReference, LinkType};
    fields[2].id = RightId;
    fields[2].name = {"right", 5};
    fields[2].kind = FieldKind::ObjectReference;
    fields[2].value = {ValueKind::ObjectReference, LinkType};
    const RuntimeFieldOps ops[] = {LinkField<&Link::number>(NumberId),
                                   LinkField<&Link::left>(LeftId),
                                   LinkField<&Link::right>(RightId)};
    auto link = MakeRuntimeTypeOps<Link>();
    link.fields = ops;
    link.completeFieldLayout = true;
    auto pack = MakeRuntimeTypeOps<Pack>();
    pack.completeFieldLayout = true;
    return types.Register({LinkType, 1, fields, {"Link", 4}}, Move(link)).IsOk() &&
           types.Register({PackType, 1, {}, {"Pack", 4}}, Move(pack)).IsOk();
}

RegistryOptions PackagePolicy() noexcept {
    RegistryOptions options;
    options.policy.packageNativeType = NativeTypeKey<Pack>();
    return options;
}

// 객체를 만들고 roots에 RootRef를 쥔다(handle을 돌려준다).
Result<ObjectHandle> New(ObjectRegistry& registry, Array<RootRef<Object>>& roots,
                         ObjectHandle outer = {}, StringView name = {},
                         TypeId type = LinkType) noexcept {
    auto made = registry.NewObject<Object>(registry.OwnerAccess(), type, outer, name);
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    const ObjectHandle handle = made.Value().Handle();
    if (!roots.TryPushBack(Move(made.Value()))) {
        return Result<ObjectHandle>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<ObjectHandle>::Success(handle);
}

// owner의 left(또는 right)를 target으로 바꾸고 number를 쓴다.
Result<void> Connect(ObjectRegistry& registry, ObjectHandle owner, ObjectHandle target,
                     bool right = false) noexcept {
    auto borrowed = registry.Resolve<Link>(registry.OwnerAccess(), owner);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return registry.SetReference(registry.OwnerAccess(), owner,
                                 right ? value.Value()->right : value.Value()->left, target);
}

Result<void> SetNumber(ObjectRegistry& registry, ObjectHandle owner, uint32 number) noexcept {
    auto borrowed = registry.Resolve<Link>(registry.OwnerAccess(), owner);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    value.Value()->number = number;
    return Result<void>::Success();
}

// 로드한 객체의 값이다(실패하면 0·null).
struct Seen {
    uint32 number = 0;
    ObjectHandle left{};
    ObjectHandle right{};
};

Seen Look(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    Seen seen;
    auto borrowed = registry.Resolve<Link>(registry.OwnerAccess(), handle);
    if (!borrowed) {
        return seen;
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return seen;
    }

    seen.number = value.Value()->number;
    seen.left = value.Value()->left.Handle();
    seen.right = value.Value()->right.Handle();
    return seen;
}

bool SameNodes(Span<const ObjectHandle> actual, Span<const ObjectHandle> expected) noexcept {
    if (actual.Size() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < actual.Size(); ++index) {
        if (!(actual[index] == expected[index])) {
            return false;
        }
    }

    return true;
}

bool PathIs(ObjectRegistry& registry, ObjectHandle handle, StringView expected,
            Allocator& allocator) noexcept {
    String path(allocator);
    return registry.GetObjectPath(registry.OwnerAccess(), handle, path) && path.View() == expected;
}

ObjectHandle OuterOf(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    auto outer = registry.GetOuter(registry.OwnerAccess(), handle);
    return outer ? outer.Value() : ObjectHandle{.registry = 1};
}

// 진단을 기록한다(마지막 경로 단계의 필드 ID).
struct Diagnostics final : FieldCodecHooks {
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (count < 8) {
            kinds[count] = diagnostic.kind;
            values[count] = diagnostic.value;
            objects[count] = diagnostic.objectIndex;
            paths[count] = diagnostic.path.Size();
            fields[count] = diagnostic.path.Size() != 0
                                ? diagnostic.path[diagnostic.path.Size() - 1].fieldId
                                : Guid{};
        }

        ++count;
        return Result<void>::Success();
    }

    FieldDiagnosticKind kinds[8] = {};
    int64 values[8] = {};
    uint32 objects[8] = {};
    usize paths[8] = {};
    Guid fields[8] = {};
    usize count = 0;
};

// 진단 하나가 ExternalReference(value 0)이고 노드 objectIndex의 field 위치인지 본다.
bool RejectedAt(const Diagnostics& diagnostics, uint32 objectIndex, Guid field) noexcept {
    return diagnostics.count == 1 &&
           diagnostics.kinds[0] == FieldDiagnosticKind::ExternalReference &&
           diagnostics.values[0] == 0 && diagnostics.objects[0] == objectIndex &&
           diagnostics.paths[0] == 1 && diagnostics.fields[0] == field;
}

} // namespace

// 저장 집합은 호출자가 정하고 순서가 local ID다. 어느 노드에서도 닿지 않는 노드(고아)도 쓴다. 편의
// 복원은 노드 1에서 닿지 않는 노드가 있으면 생성자 전에 InvalidArgument다.
TEST(GraphSet_CallerOrderIsLocalIdAndOrphansAreSaved) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x475331, 1});
        ObjectRegistry target(allocator, types, {0x475331, 2});
        {
            Array<RootRef<Object>> roots(allocator);
            auto a = New(source, roots);
            auto b = New(source, roots);
            auto c = New(source, roots);
            REQUIRE(a && b && c);
            REQUIRE(Connect(source, a.Value(), b.Value()));
            REQUIRE(SetNumber(source, a.Value(), 1));
            REQUIRE(SetNumber(source, b.Value(), 2));
            REQUIRE(SetNumber(source, c.Value(), 3));

            ObjectGraphSaveOptions options;
            options.writeNodeTable = true;
            const ObjectHandle order[] = {c.Value(), a.Value(), b.Value()};
            auto saved =
                SaveObjectGraphSet(allocator, source, source.OwnerAccess(), order, options);
            REQUIRE(saved);
            CHECK(SameNodes(saved.Value().Nodes(), order));
            const Span<const uint8> bytes = saved.Value().Bytes();
            CHECK(test::graph::ReadU32(bytes, test::graph::NodeCountOffset) == 3);
            CHECK(test::graph::ReadU32(bytes, test::graph::FlagsOffset) ==
                  test::graph::NodeTableFlag);

            // 노드 1(c)에서 a·b에 닿지 않는다 — 생성자 전에 거부한다.
            const uint32 constructed = ConstructedLinks;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(), bytes).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(ConstructedLinks == constructed);
            CHECK(target.Stats().liveObjects == 0 && target.Stats().reservedObjects == 0);

            // a에서 b에 닿으므로 {a, b}는 복원된다(local ID = 호출자 순서).
            const ObjectHandle reachable[] = {a.Value(), b.Value()};
            auto pair =
                SaveObjectGraphSet(allocator, source, source.OwnerAccess(), reachable, options);
            REQUIRE(pair);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          pair.Value().Bytes());
            REQUIRE(loaded);
            const Seen root = Look(target, loaded.Value().Handle());
            CHECK(root.number == 1);
            CHECK(Look(target, root.left).number == 2);

            // NodeTable 없이도 쓴다(노드 표는 바깥이 가진다). 편의 복원은 NodeTable을 요구한다.
            ObjectGraphSaveOptions bare;
            auto plain = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), order, bare);
            REQUIRE(plain);
            CHECK(test::graph::ReadU32(plain.Value().Bytes(), test::graph::FlagsOffset) == 0);
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  plain.Value().Bytes())
                      .Error() == ErrorCode::InvalidArgument);

            // TakeBytes 뒤 Bytes()는 비고 노드 목록은 남는다.
            Array<uint8> taken = pair.Value().TakeBytes();
            CHECK(taken.Size() != 0 && pair.Value().Bytes().Size() == 0);
            CHECK(SameNodes(pair.Value().Nodes(), reachable));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 입력 검사: 노드 0개·null handle·중복은 InvalidArgument, maxObjects(기본 4096)를 넘으면
// CapacityExceeded다. 4096개 집합은 저장·복원된다.
TEST(GraphSet_InputValidationAndObjectBoundary) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x475332, 1});
        ObjectRegistry target(allocator, types, {0x475332, 2});
        {
            Array<RootRef<Object>> roots(allocator);
            Array<ObjectHandle> chain(allocator);
            REQUIRE(chain.TryReserve(4097));
            for (uint32 index = 0; index < 4097; ++index) {
                auto made = New(source, roots);
                REQUIRE(made);
                REQUIRE(chain.TryPushBack(made.Value()));
                if (index != 0) {
                    REQUIRE(Connect(source, chain[index - 1], made.Value()));
                }
            }

            const auto access = source.OwnerAccess();
            CHECK(SaveObjectGraphSet(allocator, source, access, {}).Error() ==
                  ErrorCode::InvalidArgument);
            const ObjectHandle withNull[] = {chain[0], ObjectHandle{}};
            CHECK(SaveObjectGraphSet(allocator, source, access, withNull).Error() ==
                  ErrorCode::InvalidArgument);
            const ObjectHandle twice[] = {chain[0], chain[1], chain[0]};
            CHECK(SaveObjectGraphSet(allocator, source, access, twice).Error() ==
                  ErrorCode::InvalidArgument);
            // 개수가 중복보다 먼저다: 중복을 섞은 4097개는 CapacityExceeded다.
            chain[4096] = chain[0];
            CHECK(SaveObjectGraphSet(allocator, source, access, chain.View()).Error() ==
                  ErrorCode::CapacityExceeded);
            chain[4096] = roots[4096].Handle();
            // maxObjects는 local ID가 외부 참조 비트와 겹치지 않게 2^31 − 1 이하다(옵션 오류).
            ObjectGraphSaveOptions huge;
            huge.limits.maxObjects = 0x80000000u;
            CHECK(SaveObjectGraphSet(allocator, source, access, {}, huge).Error() ==
                  ErrorCode::InvalidArgument);

            // 4097개는 입력에서, 4096개 사슬의 편의 저장은 Include가 4097번째를 더할 때 거부한다.
            CHECK(SaveObjectGraphSet(allocator, source, access, chain.View()).Error() ==
                  ErrorCode::CapacityExceeded);
            CHECK(SaveObjectGraph(allocator, source, access, chain[0]).Error() ==
                  ErrorCode::CapacityExceeded);

            // 마지막 노드의 참조를 끊으면 4096개 사슬이 되고 저장·복원된다.
            ObjectGraphSaveOptions options;
            options.externals = GraphExternalPolicy::Include;
            options.writeNodeTable = true;
            const Span<const ObjectHandle> boundary(chain.Data(), 4096);
            CHECK(SaveObjectGraphSet(allocator, source, access, boundary, options).Error() ==
                  ErrorCode::CapacityExceeded);
            REQUIRE(Connect(source, chain[4095], ObjectHandle{}));
            auto saved = SaveObjectGraph(allocator, source, access, chain[0]);
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            CHECK(target.Stats().liveObjects == 4096);
            loaded.Value().Reset();
            REQUIRE(target.Collect(target.OwnerAccess()));
            ObjectGraphLoadOptions fewer;
            fewer.limits.maxObjects = 4095;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  saved.Value().View(), fewer)
                      .Error() == ErrorCode::CapacityExceeded);
            CHECK(target.Stats().liveObjects == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Reject(기본): 집합 밖 참조는 그 위치의 ExternalReference 진단(value 0) 뒤 InvalidArgument다.
TEST(GraphSet_RejectPolicyReportsExternalReference) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x475333, 1});
        {
            Array<RootRef<Object>> roots(allocator);
            auto a = New(source, roots);
            auto b = New(source, roots);
            auto outside = New(source, roots);
            REQUIRE(a && b && outside);
            REQUIRE(Connect(source, a.Value(), b.Value()));
            REQUIRE(Connect(source, b.Value(), outside.Value(), true));
            const ObjectHandle set[] = {a.Value(), b.Value()};
            Diagnostics diagnostics;
            ObjectGraphSaveOptions options;
            options.hooks = &diagnostics;
            CHECK(
                SaveObjectGraphSet(allocator, source, source.OwnerAccess(), set, options).Error() ==
                ErrorCode::InvalidArgument);
            CHECK(RejectedAt(diagnostics, 1, RightId));
            options.hooks = nullptr;
            CHECK(
                SaveObjectGraphSet(allocator, source, source.OwnerAccess(), set, options).Error() ==
                ErrorCode::InvalidArgument);
            const ObjectHandle all[] = {a.Value(), b.Value(), outside.Value()};
            CHECK(SaveObjectGraphSet(allocator, source, source.OwnerAccess(), all, options));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Include의 순서는 발견 순 BFS(필드 순)다 — 편의 저장과 같은 집합·순서·byte다.
TEST(GraphSet_IncludeOrderIsDiscoveryBfs) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x475334, 1});
        {
            Array<RootRef<Object>> roots(allocator);
            auto a = New(source, roots);
            auto b = New(source, roots);
            auto c = New(source, roots);
            auto d = New(source, roots);
            auto e = New(source, roots);
            REQUIRE(a && b && c && d && e);
            // a → (c, b), c → (d, a), b → (e, d): 발견 순은 a, c, b, d, e다.
            REQUIRE(Connect(source, a.Value(), c.Value()));
            REQUIRE(Connect(source, a.Value(), b.Value(), true));
            REQUIRE(Connect(source, c.Value(), d.Value()));
            REQUIRE(Connect(source, c.Value(), a.Value(), true));
            REQUIRE(Connect(source, b.Value(), e.Value()));
            REQUIRE(Connect(source, b.Value(), d.Value(), true));
            ObjectGraphSaveOptions options;
            options.externals = GraphExternalPolicy::Include;
            options.writeNodeTable = true;
            const ObjectHandle root[] = {a.Value()};
            auto set = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), root, options);
            REQUIRE(set);
            const ObjectHandle expected[] = {a.Value(), c.Value(), b.Value(), d.Value(), e.Value()};
            CHECK(SameNodes(set.Value().Nodes(), expected));
            auto convenience = SaveObjectGraph(allocator, source, source.OwnerAccess(), a.Value());
            REQUIRE(convenience);
            CHECK(SameBytes(set.Value().Bytes(), convenience.Value().View()));

            // 같은 그래프의 다른 Registry 복원·재저장 byte가 같다.
            ObjectRegistry target(allocator, types, {0x475334, 2});
            {
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              convenience.Value().View());
                REQUIRE(loaded);
                auto resaved = SaveObjectGraph(allocator, target, target.OwnerAccess(),
                                               loaded.Value().Handle());
                REQUIRE(resaved);
                CHECK(SameBytes(resaved.Value().View(), convenience.Value().View()));
            }

            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Include는 대상의 Outer 사슬 중 집합에 없는 조상을 바깥쪽부터 먼저 더하고, 집합 안 조상·기준
// Outer(노드 1의 Outer)에서 멈춘다. NodeTable은 Outer·이름(대표 자식·깊은 경로)을 왕복하고
// BaseOuter 비트의 outer 0은 복원의 baseOuter다.
TEST(GraphSet_IncludeAddsOuterChainFirstAndNodeTableRoundTrips) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x475335, 1});
        ObjectRegistry target(allocator, types, {0x475335, 2});
        {
            Array<RootRef<Object>> roots(allocator);
            // pkg(Outer·이름 없음) 아래 대표 자식 asset, asset 아래 Body.Socket과 Arm.
            auto pkg = New(source, roots);
            REQUIRE(pkg);
            auto asset = New(source, roots, pkg.Value());
            REQUIRE(asset);
            auto body = New(source, roots, asset.Value(), "Body");
            REQUIRE(body);
            auto socket = New(source, roots, body.Value(), "Socket");
            auto arm = New(source, roots, asset.Value(), "Arm");
            REQUIRE(socket && arm);
            REQUIRE(Connect(source, asset.Value(), socket.Value()));
            REQUIRE(SetNumber(source, socket.Value(), 7));

            ObjectGraphSaveOptions options;
            options.externals = GraphExternalPolicy::Include;
            options.writeNodeTable = true;
            const ObjectHandle root[] = {asset.Value()};
            auto set = SaveObjectGraphSet(allocator, source, source.OwnerAccess(), root, options);
            REQUIRE(set);
            // Socket보다 그 Outer Body가 먼저다(Arm은 닿지 않으므로 없다).
            const ObjectHandle expected[] = {asset.Value(), body.Value(), socket.Value()};
            CHECK(SameNodes(set.Value().Nodes(), expected));
            CHECK(test::graph::ReadU32(set.Value().Bytes(), test::graph::FlagsOffset) ==
                  (test::graph::NodeTableFlag | test::graph::BaseOuterFlag));
            auto bytes = SaveObjectGraph(allocator, source, source.OwnerAccess(), asset.Value());
            REQUIRE(bytes);
            CHECK(SameBytes(set.Value().Bytes(), bytes.Value().View()));

            // 기준 Outer 없이 복원하면 BaseOuter 비트와 달라 InvalidArgument다.
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::InvalidArgument);
            Array<RootRef<Object>> targetRoots(allocator);
            auto newPkg = New(target, targetRoots);
            REQUIRE(newPkg);
            ObjectGraphLoadOptions load;
            load.baseOuter = newPkg.Value();
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            const ObjectHandle loadedAsset = loaded.Value().Handle();
            CHECK(OuterOf(target, loadedAsset) == newPkg.Value());
            const ObjectHandle loadedSocket = Look(target, loadedAsset).left;
            CHECK(PathIs(target, loadedSocket, "Body.Socket", allocator));
            CHECK(PathIs(target, loadedAsset, "", allocator));
            CHECK(Look(target, loadedSocket).number == 7);
            auto found = target.FindObject(target.OwnerAccess(), newPkg.Value(), "body.socket");
            CHECK(found && found.Value() == loadedSocket);
            // 재저장 byte가 같다(Outer·이름 포함).
            auto resaved = SaveObjectGraph(allocator, target, target.OwnerAccess(), loadedAsset);
            REQUIRE(resaved);
            CHECK(SameBytes(resaved.Value().View(), bytes.Value().View()));

            // 노드 1이 Socket이면 기준 Outer는 Body다: 형제 Pin은 Body에서 멈춰 outer 0이 된다.
            auto pin = New(source, roots, body.Value(), "Pin");
            REQUIRE(pin);
            REQUIRE(Connect(source, socket.Value(), pin.Value()));
            const ObjectHandle socketRoot[] = {socket.Value()};
            auto siblings =
                SaveObjectGraphSet(allocator, source, source.OwnerAccess(), socketRoot, options);
            REQUIRE(siblings);
            const ObjectHandle siblingNodes[] = {socket.Value(), pin.Value()};
            CHECK(SameNodes(siblings.Value().Nodes(), siblingNodes));

            // BaseOuter 비트가 없는 그래프에 baseOuter를 주면 InvalidArgument, 죽은 baseOuter는
            // DescribeOuter의 오류다.
            auto top = SaveObjectGraph(allocator, source, source.OwnerAccess(), pkg.Value());
            REQUIRE(top);
            CHECK(test::graph::ReadU32(top.Value().View(), test::graph::FlagsOffset) ==
                  test::graph::NodeTableFlag);
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  top.Value().View(), load)
                      .Error() == ErrorCode::InvalidArgument);
            ObjectGraphLoadOptions stale;
            stale.baseOuter = ObjectHandle{0x475335, 99, 1};
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View(), stale)
                      .Error() == ErrorCode::StaleHandle);
            loaded.Value().Reset();
            for (RootRef<Object>& held : targetRoots) {
                held.Reset();
            }
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Include 거부 조건(결정 17): 대상이 기준 Outer이거나 노드 1의 조상이면, 조상 순회가 기준 Outer
// 위의 노드 1 조상을 만나면, 패키지 정책에서 대상의 outermost가 노드 1과 다르면 ExternalReference
// 진단 뒤 InvalidArgument다.
TEST(GraphSet_IncludeRejectsBaseOuterAncestorsAndOtherPackages) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x475336, 1});
        ObjectRegistry packaged(allocator, types, {0x475336, 2}, PackagePolicy());
        {
            Array<RootRef<Object>> roots(allocator);
            auto pkg = New(source, roots);
            REQUIRE(pkg);
            auto asset = New(source, roots, pkg.Value());
            REQUIRE(asset);
            auto body = New(source, roots, asset.Value(), "Body");
            REQUIRE(body);
            auto socket = New(source, roots, body.Value(), "Socket");
            auto arm = New(source, roots, asset.Value(), "Arm");
            REQUIRE(socket && arm);
            const auto access = source.OwnerAccess();
            Diagnostics diagnostics;
            ObjectGraphSaveOptions options;
            options.externals = GraphExternalPolicy::Include;
            options.writeNodeTable = true;
            options.hooks = &diagnostics;
            const ObjectHandle root[] = {socket.Value()};

            // 대상이 기준 Outer(Body) 자신이다.
            REQUIRE(Connect(source, socket.Value(), body.Value()));
            CHECK(SaveObjectGraphSet(allocator, source, access, root, options).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(RejectedAt(diagnostics, 0, LeftId));
            // 대상이 노드 1의 조상(Asset)이다.
            diagnostics.count = 0;
            REQUIRE(Connect(source, socket.Value(), asset.Value()));
            CHECK(SaveObjectGraphSet(allocator, source, access, root, options).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(RejectedAt(diagnostics, 0, LeftId));
            // Arm의 조상 순회가 기준 Outer(Body) 위의 Asset을 만난다.
            diagnostics.count = 0;
            REQUIRE(Connect(source, socket.Value(), arm.Value()));
            CHECK(SaveObjectGraphSet(allocator, source, access, root, options).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(RejectedAt(diagnostics, 0, LeftId));
            // 편의 저장도 같다(hooks 없음).
            CHECK(SaveObjectGraph(allocator, source, access, socket.Value()).Error() ==
                  ErrorCode::InvalidArgument);
            // 정책 없는 Registry에서 기준 Outer가 없으면 다른 outermost의 객체도 Outer 없음 노드로
            // 더한다.
            auto other = New(source, roots);
            REQUIRE(other);
            auto otherChild = New(source, roots, other.Value(), "Child");
            REQUIRE(otherChild);
            REQUIRE(Connect(source, pkg.Value(), otherChild.Value()));
            CHECK(SaveObjectGraph(allocator, source, access, pkg.Value()));
            // 기준 Outer(Body)가 있는데 사슬이 Outer 없음에 닿으면(다른 트리) 그 시점에 진단과 함께
            // 거부한다 — 대상 자신이 Outer 없음이어도, 조상이 Outer 없음이어도, NodeTable 없이
            // 써도.
            const ObjectHandle otherTrees[] = {other.Value(), otherChild.Value()};
            for (ObjectHandle tree : otherTrees) {
                REQUIRE(Connect(source, socket.Value(), tree));
                diagnostics.count = 0;
                CHECK(SaveObjectGraphSet(allocator, source, access, root, options).Error() ==
                      ErrorCode::InvalidArgument);
                CHECK(RejectedAt(diagnostics, 0, LeftId));
                ObjectGraphSaveOptions bare = options;
                bare.writeNodeTable = false;
                diagnostics.count = 0;
                CHECK(SaveObjectGraphSet(allocator, source, access, root, bare).Error() ==
                      ErrorCode::InvalidArgument);
                CHECK(RejectedAt(diagnostics, 0, LeftId));
            }

            // 패키지 정책: 다른 패키지의 객체는 거부한다(같은 패키지 안은 된다).
            Array<RootRef<Object>> packageRoots(allocator);
            auto first = New(packaged, packageRoots, {}, {}, PackType);
            auto second = New(packaged, packageRoots, {}, {}, PackType);
            REQUIRE(first && second);
            auto mine = New(packaged, packageRoots, first.Value());
            auto inner = New(packaged, packageRoots, mine.Value(), "Inner");
            auto theirs = New(packaged, packageRoots, second.Value());
            REQUIRE(mine && inner && theirs);
            REQUIRE(Connect(packaged, mine.Value(), inner.Value()));
            CHECK(SaveObjectGraph(allocator, packaged, packaged.OwnerAccess(), mine.Value()));
            REQUIRE(Connect(packaged, inner.Value(), theirs.Value()));
            diagnostics.count = 0;
            const ObjectHandle packageRoot[] = {mine.Value()};
            CHECK(SaveObjectGraphSet(allocator, packaged, packaged.OwnerAccess(), packageRoot,
                                     options)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(RejectedAt(diagnostics, 1, LeftId));
            for (RootRef<Object>& held : packageRoots) {
                held.Reset();
            }
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(packaged.Shutdown(packaged.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// NodeTable 규칙: Outer가 그 노드보다 뒤에 오면, 집합 밖 Outer가 기준 Outer(노드 1의 Outer)와
// 다르면, Outer 없음과 기준 Outer를 섞으면 InvalidArgument다. NodeTable 없이 쓰면 검사하지 않는다.
// 복원은 자기 번호 이상의 outer를 거부한다.
TEST(GraphSet_NodeTableOuterOrderAndBaseOuterRules) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x475337, 1});
        ObjectRegistry target(allocator, types, {0x475337, 2});
        {
            Array<RootRef<Object>> roots(allocator);
            auto pkg = New(source, roots);
            REQUIRE(pkg);
            auto asset = New(source, roots, pkg.Value());
            REQUIRE(asset);
            auto body = New(source, roots, asset.Value(), "Body");
            auto arm = New(source, roots, asset.Value(), "Arm");
            REQUIRE(body && arm);
            auto socket = New(source, roots, body.Value(), "Socket");
            auto loose = New(source, roots);
            REQUIRE(socket && loose);
            const auto access = source.OwnerAccess();
            ObjectGraphSaveOptions options;
            options.writeNodeTable = true;

            const ObjectHandle innerFirst[] = {socket.Value(), body.Value()};
            CHECK(SaveObjectGraphSet(allocator, source, access, innerFirst, options).Error() ==
                  ErrorCode::InvalidArgument);
            const ObjectHandle otherOuter[] = {body.Value(), socket.Value(), arm.Value()};
            CHECK(SaveObjectGraphSet(allocator, source, access, otherOuter, options));
            const ObjectHandle mismatch[] = {socket.Value(), arm.Value()};
            CHECK(SaveObjectGraphSet(allocator, source, access, mismatch, options).Error() ==
                  ErrorCode::InvalidArgument);
            const ObjectHandle mixed[] = {body.Value(), loose.Value()};
            CHECK(SaveObjectGraphSet(allocator, source, access, mixed, options).Error() ==
                  ErrorCode::InvalidArgument);
            const ObjectHandle mixedReverse[] = {loose.Value(), body.Value()};
            CHECK(SaveObjectGraphSet(allocator, source, access, mixedReverse, options).Error() ==
                  ErrorCode::InvalidArgument);
            ObjectGraphSaveOptions bare;
            CHECK(SaveObjectGraphSet(allocator, source, access, innerFirst, bare));
            CHECK(SaveObjectGraphSet(allocator, source, access, mixed, bare));

            // outer 0의 두 뜻: BaseOuter 비트가 없으면 Outer 없음이다(loose와 그 대표 자식).
            auto child = New(source, roots, loose.Value());
            REQUIRE(child);
            REQUIRE(Connect(source, loose.Value(), child.Value()));
            auto top = SaveObjectGraph(allocator, source, access, loose.Value());
            REQUIRE(top);
            {
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              top.Value().View());
                REQUIRE(loaded);
                CHECK(OuterOf(target, loaded.Value().Handle()) == ObjectHandle{});
                const ObjectHandle loadedChild = Look(target, loaded.Value().Handle()).left;
                CHECK(OuterOf(target, loadedChild) == loaded.Value().Handle());
            }

            REQUIRE(target.Collect(target.OwnerAccess()));
            // 복원: 노드 표의 outer가 자기 번호 이상이면 InvalidArgument다(생성자 전). 노드 1이
            // 노드 2를 참조해 도달 검사는 통과하므로 outer 검사만 거부한다(같은 그래프에 outer {0,
            // 1}은 복원된다 — 양성 대조).
            RecordBuilder record(allocator, LinkType, 1);
            auto empty = record.Build();
            REQUIRE(empty);
            ArchiveWriter second(allocator);
            REQUIRE(second.WriteU32(2));
            RecordBuilder linking(allocator, LinkType, 1);
            REQUIRE(linking.AddField(LeftId, WireType::U32, second.View()));
            auto first = linking.Build();
            REQUIRE(first);
            {
                const test::graph::Node nodes[] = {{LinkType, 0, {}, first.Value().View()},
                                                   {LinkType, 1, "B", empty.Value().View()}};
                auto graph = test::graph::Build(allocator, nodes);
                REQUIRE(graph);
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              graph.Value().View());
                REQUIRE(loaded);
                CHECK(OuterOf(target, Look(target, loaded.Value().Handle()).left) ==
                      loaded.Value().Handle());
            }

            REQUIRE(target.Collect(target.OwnerAccess()));
            const uint32 constructed = ConstructedLinks;
            const uint32 badOuters[][2] = {{1, 0}, {0, 2}, {0, 3}};
            for (const auto& outers : badOuters) {
                const test::graph::Node nodes[] = {
                    {LinkType, outers[0], {}, first.Value().View()},
                    {LinkType, outers[1], "B", empty.Value().View()}};
                auto graph = test::graph::Build(allocator, nodes);
                REQUIRE(graph);
                CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                      graph.Value().View())
                          .Error() == ErrorCode::InvalidArgument);
            }

            CHECK(ConstructedLinks == constructed);
            CHECK(target.Stats().liveObjects == 0);
            // 노드 표의 이름 규칙 위반은 생성(2단계)이 거부하고 rollback한다(생성 전 판정은
            // A0-Od2).
            const test::graph::Node named[] = {{LinkType, 0, {}, first.Value().View()},
                                               {LinkType, 1, "a.b", empty.Value().View()}};
            auto invalid = test::graph::Build(allocator, named);
            REQUIRE(invalid);
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  invalid.Value().View())
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(target.Stats().liveObjects == 0 && target.Stats().reservedObjects == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 편의 복원의 도달 검사: inner → outer로만 닿는 Outer 노드는 통과한다. 패키지 정책 Registry는 outer
// → inner도 간선이고, 기준 Outer가 있으면 outer 0 노드는 기준 Outer가 붙잡으므로 참조가 없어도
// 통과한다(정책 없는 Registry에서는 같은 그래프가 InvalidArgument다).
TEST(GraphSet_ReachabilityFollowsOuterEdgesAndPackageRetention) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x475338, 1});
        ObjectRegistry plain(allocator, types, {0x475338, 2});
        ObjectRegistry packaged(allocator, types, {0x475338, 3}, PackagePolicy());
        {
            Array<RootRef<Object>> roots(allocator);
            // r → s, s의 Outer o는 아무도 참조하지 않는다.
            auto r = New(source, roots);
            auto o = New(source, roots);
            REQUIRE(r && o);
            auto s = New(source, roots, o.Value(), "S");
            REQUIRE(s);
            REQUIRE(Connect(source, r.Value(), s.Value()));
            auto viaOuter = SaveObjectGraph(allocator, source, source.OwnerAccess(), r.Value());
            REQUIRE(viaOuter);
            {
                auto loaded = LoadObjectGraph(allocator, plain, types, plain.OwnerAccess(),
                                              viaOuter.Value().View());
                REQUIRE(loaded);
                CHECK(plain.Stats().liveObjects == 3);
                REQUIRE(plain.Collect(plain.OwnerAccess()));
                CHECK(plain.Stats().liveObjects == 3);
            }

            REQUIRE(plain.Collect(plain.OwnerAccess()));
            CHECK(plain.Stats().liveObjects == 0);

            // 패키지 정책 원본: pkg 아래 대표 자식 asset, asset 아래 Body·Mesh(서로 참조 없음).
            Array<RootRef<Object>> packageRoots(allocator);
            auto pkg = New(packaged, packageRoots, {}, {}, PackType);
            REQUIRE(pkg);
            auto asset = New(packaged, packageRoots, pkg.Value());
            REQUIRE(asset);
            auto body = New(packaged, packageRoots, asset.Value(), "Body");
            auto mesh = New(packaged, packageRoots, asset.Value(), "Mesh");
            REQUIRE(body && mesh);
            ObjectGraphSaveOptions options;
            options.writeNodeTable = true;
            const ObjectHandle members[] = {body.Value(), mesh.Value()};
            auto underBase =
                SaveObjectGraphSet(allocator, packaged, packaged.OwnerAccess(), members, options);
            REQUIRE(underBase);
            // 정책 없는 Registry: Mesh는 Body에서 닿지 않는다.
            auto plainBase = New(plain, roots);
            REQUIRE(plainBase);
            ObjectGraphLoadOptions plainLoad;
            plainLoad.baseOuter = plainBase.Value();
            CHECK(LoadObjectGraph(allocator, plain, types, plain.OwnerAccess(),
                                  underBase.Value().Bytes(), plainLoad)
                      .Error() == ErrorCode::InvalidArgument);
            // 패키지 정책: 기준 Outer(새 패키지의 대표 자식) 아래 노드는 붙잡힌다.
            auto newPkg = New(packaged, packageRoots, {}, {}, PackType);
            REQUIRE(newPkg);
            auto newAsset = New(packaged, packageRoots, newPkg.Value());
            REQUIRE(newAsset);
            ObjectGraphLoadOptions packageLoad;
            packageLoad.baseOuter = newAsset.Value();
            {
                auto loaded = LoadObjectGraph(allocator, packaged, types, packaged.OwnerAccess(),
                                              underBase.Value().Bytes(), packageLoad);
                REQUIRE(loaded);
                auto found = packaged.FindObject(packaged.OwnerAccess(), newPkg.Value(), "Mesh");
                CHECK(found.IsOk());
            }

            // 기준 Outer 없는 패키지 그래프: 노드 1(패키지)에서 outer → inner로 닿는다(정책 없는
            // Registry에서는 닿지 않는다).
            const ObjectHandle whole[] = {pkg.Value(), asset.Value(), body.Value(), mesh.Value()};
            auto package =
                SaveObjectGraphSet(allocator, packaged, packaged.OwnerAccess(), whole, options);
            REQUIRE(package);
            CHECK(test::graph::ReadU32(package.Value().Bytes(), test::graph::FlagsOffset) ==
                  test::graph::NodeTableFlag);
            CHECK(LoadObjectGraph(allocator, plain, types, plain.OwnerAccess(),
                                  package.Value().Bytes())
                      .Error() == ErrorCode::InvalidArgument);
            // 기준 Outer 없는 패키지 정책 복원은 노드 1의 패키지 하나만 공개한다: 참조로 닿아도
            // 다른 패키지(노드 1이 아닌 outer 0 노드)는 root 없이 공개되므로 생성 전에 거부한다.
            auto otherPkg = New(packaged, packageRoots, {}, {}, PackType);
            REQUIRE(otherPkg);
            auto otherAsset = New(packaged, packageRoots, otherPkg.Value());
            REQUIRE(otherAsset);
            REQUIRE(Connect(packaged, asset.Value(), otherAsset.Value()));
            const ObjectHandle two[] = {pkg.Value(), asset.Value(), otherPkg.Value(),
                                        otherAsset.Value()};
            auto twoPackages =
                SaveObjectGraphSet(allocator, packaged, packaged.OwnerAccess(), two, options);
            REQUIRE(twoPackages);
            {
                const uint32 constructed = ConstructedLinks;
                const uint32 live = packaged.Stats().liveObjects;
                CHECK(LoadObjectGraph(allocator, packaged, types, packaged.OwnerAccess(),
                                      twoPackages.Value().Bytes())
                          .Error() == ErrorCode::InvalidArgument);
                CHECK(ConstructedLinks == constructed);
                CHECK(packaged.Stats().liveObjects == live &&
                      packaged.Stats().reservedObjects == 0);
            }

            {
                auto loaded = LoadObjectGraph(allocator, packaged, types, packaged.OwnerAccess(),
                                              package.Value().Bytes());
                REQUIRE(loaded);
                Array<ObjectHandle> inners(allocator);
                REQUIRE(packaged.CollectInners(packaged.OwnerAccess(), loaded.Value().Handle(),
                                               InnerOrder::DepthFirst, inners));
                CHECK(inners.Size() == 3);
                for (RootRef<Object>& held : packageRoots) {
                    held.Reset();
                }
            }

            for (RootRef<Object>& held : roots) {
                held.Reset();
            }
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(plain.Shutdown(plain.OwnerAccess()));
        REQUIRE(packaged.Shutdown(packaged.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// prepare는 적용 뒤·Commit 전에 노드 handle(local ID 순)로 불리고, 실패하면 그 오류로 rollback한다.
TEST(GraphSet_PrepareRunsBeforeCommitAndFailureRollsBack) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x475339, 1});
        ObjectRegistry target(allocator, types, {0x475339, 2});
        {
            Array<RootRef<Object>> roots(allocator);
            auto a = New(source, roots);
            auto b = New(source, roots);
            REQUIRE(a && b);
            REQUIRE(Connect(source, a.Value(), b.Value()));
            auto bytes = SaveObjectGraph(allocator, source, source.OwnerAccess(), a.Value());
            REQUIRE(bytes);

            struct State {
                usize nodes = 0;
                ErrorCode fail = ErrorCode::None;
            } state;
            ObjectGraphLoadOptions options;
            options.prepareUser = &state;
            options.prepare = [](ConstructionContext& transaction, Span<const ObjectHandle> nodes,
                                 void* user) noexcept -> Result<void> {
                auto* seen = static_cast<State*>(user);
                seen->nodes = nodes.Size();
                if (seen->fail != ErrorCode::None) {
                    return Result<void>::Failure(seen->fail);
                }

                for (usize index = 0; index < nodes.Size(); ++index) {
                    auto value = transaction.GetForWrite<Link>(nodes[index]);
                    if (!value) {
                        return Result<void>::Failure(value.Error());
                    }

                    value.Value()->number = static_cast<uint32>(100 + index);
                }

                return Result<void>::Success();
            };
            {
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              bytes.Value().View(), options);
                REQUIRE(loaded);
                CHECK(state.nodes == 2);
                const Seen root = Look(target, loaded.Value().Handle());
                CHECK(root.number == 100);
                CHECK(Look(target, root.left).number == 101);
            }

            REQUIRE(target.Collect(target.OwnerAccess()));
            state.fail = ErrorCode::OutOfMemory;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View(), options)
                      .Error() == ErrorCode::OutOfMemory);
            CHECK(target.Stats().liveObjects == 0 && target.Stats().reservedObjects == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Include의 조상·NodeTable 이름을 쓰는 저장과 기준 Outer 아래 복원의 모든 할당 실패는
// OutOfMemory이고 할당·lease·borrow·객체를 원상태로 돌린다.
TEST(GraphSet_EveryAllocationFailureWithOuterChainRestoresState) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x47533a, 1});
        {
            Array<RootRef<Object>> roots(allocator);
            auto pkg = New(source, roots);
            REQUIRE(pkg);
            auto asset = New(source, roots, pkg.Value());
            REQUIRE(asset);
            auto body = New(source, roots, asset.Value(), "Body");
            REQUIRE(body);
            auto socket = New(source, roots, body.Value(), "Socket");
            REQUIRE(socket);
            REQUIRE(Connect(source, asset.Value(), socket.Value()));
            const auto access = source.OwnerAccess();
            test::NFailAllocator failure(allocator);
            uint64 attempts = 0;
            {
                auto baseline = SaveObjectGraph(failure, source, access, asset.Value());
                REQUIRE(baseline);
                attempts = failure.Attempts();
            }

            const auto stats = allocator.Stats();
            const auto leases = types.Stats().leases;
            for (uint64 index = 1; index <= attempts; ++index) {
                failure.FailOnAttempt(index);
                auto saved = SaveObjectGraph(failure, source, access, asset.Value());
                CHECK(!saved && saved.Error() == ErrorCode::OutOfMemory);
                CHECK(allocator.Stats().liveAllocations == stats.liveAllocations);
                CHECK(allocator.Stats().liveBytes == stats.liveBytes);
                CHECK(types.Stats().leases == leases);
                CHECK(source.Stats().borrows == 0 && source.Stats().encodePins == 0);
            }

            failure.DisableFailure();
            auto bytes = SaveObjectGraph(allocator, source, access, asset.Value());
            REQUIRE(bytes);
            // 매번 새 Registry(실패 할당기)에 기준 Outer를 만든 뒤 failAt번째 할당을 실패시킨다(0은
            // 실패 없음).
            auto loadOnce = [&](uint64 failAt, uint64* used) noexcept -> Result<void> {
                ObjectRegistry registry(failure, types, {0x47533a, 2});
                failure.DisableFailure();
                auto base = registry.NewObject<Object>(registry.OwnerAccess(), LinkType);
                if (!base) {
                    return Result<void>::Failure(ErrorCode::DependencyFailed);
                }

                ObjectGraphLoadOptions load;
                load.baseOuter = base.Value().Handle();
                if (failAt != 0) {
                    failure.FailOnAttempt(failAt);
                } else {
                    failure.DisableFailure();
                }

                auto loaded = LoadObjectGraph(failure, registry, types, registry.OwnerAccess(),
                                              bytes.Value().View(), load);
                if (used != nullptr) {
                    *used = failure.Attempts();
                }

                failure.DisableFailure();
                if (!loaded) {
                    if (registry.Stats().liveObjects != 1 ||
                        registry.Stats().reservedObjects != 0) {
                        return Result<void>::Failure(ErrorCode::DependencyFailed);
                    }

                    return Result<void>::Failure(loaded.Error());
                }

                return registry.Stats().liveObjects == 4
                           ? Result<void>::Success()
                           : Result<void>::Failure(ErrorCode::DependencyFailed);
            };
            REQUIRE(loadOnce(0, &attempts));
            const auto loadStats = allocator.Stats();
            for (uint64 index = 1; index <= attempts; ++index) {
                auto loaded = loadOnce(index, nullptr);
                CHECK(!loaded && loaded.Error() == ErrorCode::OutOfMemory);
                CHECK(allocator.Stats().liveAllocations == loadStats.liveAllocations);
                CHECK(allocator.Stats().liveBytes == loadStats.liveBytes);
                CHECK(types.Stats().leases == leases);
            }
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
