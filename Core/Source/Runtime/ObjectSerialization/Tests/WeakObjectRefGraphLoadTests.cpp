#include "WeakObjectRefGraphTestSupport.h"

// 리플렉션 B9d1 WeakObjectRef PROPERTY의 복원·GC
// 시험이다(Docs/Design/ReflectionExtensions.ko.md 5.1절, 17절 B9d 항목): weak 값은 같은 그래프 안
// 노드 번호만 받고(외부 참조 번호·범위 밖 번호 InvalidArgument), 노드 대상은 강한 참조와 같은 기대
// 형식 규칙(TypeMismatch)을 따르며, weak 간선은 편의 복원의 도달 검사·GC가 따라가지 않고, weak를
// 담은 struct는 1단계 관찰 대상이 될 수 없다.
using namespace rocks;
using namespace rocks::test::weak;

namespace {

// 노드마다 Record 하나인 NodeTable 그래프다(externalCount는 header 값).
Array<uint8> Graph(Allocator& allocator, Span<const test::graph::Node> nodes,
                   uint32 externalCount = 0) noexcept {
    test::graph::Header header;
    header.externalCount = externalCount;
    auto built = test::graph::Build(allocator, nodes, header);
    if (!built) {
        platform::Fatal("test graph build failed");
    }

    return Move(built.Value());
}

// WeakHolder 필드 하나만 담은 Record다.
Array<uint8> HolderRecord(Allocator& allocator, StringView field, WireType wire,
                          Span<const uint8> payload) noexcept {
    return OneFieldRecord(allocator, TypeIdOf<WeakHolder>(), 1, HolderField(field), wire, payload);
}

// 필드 없는 Record다.
Array<uint8> EmptyRecord(Allocator& allocator, TypeId type) noexcept {
    return OneFieldRecord(allocator, type, 1, Guid{}, WireType::U32, {});
}

// 단계형 복원 1단계(Validate)의 결과다.
ErrorCode Validate(Allocator& allocator, ObjectRegistry& objects, TypeRegistry& types,
                   Span<const uint8> bytes, Span<const TypeId> importTypes = {},
                   FieldCodecHooks* hooks = nullptr) noexcept {
    ObjectGraphLoadOptions options;
    options.hooks = hooks;
    ObjectGraphStage stage(allocator, objects, types, objects.OwnerAccess(), options);
    ObjectGraphInput input;
    input.bytes = bytes;
    input.importTypes = importTypes;
    return ErrorOf(stage.Validate(input));
}

} // namespace

// weak 값은 같은 그래프 안 노드 번호만 받는다: 외부 참조 번호(0x80000000 | i — 같은 번호의 강한
// 참조는 받는다)와 nodeCount를 넘는 번호는 단일 필드·배열 원소 모두 InvalidArgument다. 0(null)과
// 자기 번호는 받는다.
TEST(WeakObjectRefGraphLoad_RejectsExternalAndOutOfRangeNumbers) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x57524c, 1});
        const TypeId importTypes[] = {TypeIdOf<WeakTarget>()};
        const uint32 external = test::graph::ExternalBit | 1;
        struct Case {
            const char* field;
            uint32 value;
            uint32 externals;
            ErrorCode expected;
        };

        const Case singles[] = {{"target", external, 1, ErrorCode::InvalidArgument},
                                {"strong", external, 1, ErrorCode::None},
                                {"target", 2, 0, ErrorCode::InvalidArgument},
                                {"target", 1, 0, ErrorCode::None},
                                {"target", 0, 0, ErrorCode::None}};
        for (const Case& item : singles) {
            Array<uint8> payload = U32Payload(allocator, item.value);
            Array<uint8> record =
                HolderRecord(allocator, StringView(item.field), WireType::U32, payload.View());
            const test::graph::Node node{TypeIdOf<WeakHolder>(), 0, {}, record.View()};
            Array<uint8> graph =
                Graph(allocator, Span<const test::graph::Node>(&node, 1), item.externals);
            CHECK(Validate(allocator, objects, types, graph.View(),
                           Span<const TypeId>(importTypes, item.externals)) == item.expected);
        }

        const uint32 externalElement[] = {1, external};
        const uint32 rangeElement[] = {0, 2};
        const uint32 validElement[] = {1, 0, 1};
        const Span<const uint32> arrays[] = {Span<const uint32>(externalElement, 2),
                                             Span<const uint32>(rangeElement, 2),
                                             Span<const uint32>(validElement, 3)};
        const ErrorCode expected[] = {ErrorCode::InvalidArgument, ErrorCode::InvalidArgument,
                                      ErrorCode::None};
        for (usize index = 0; index < 3; ++index) {
            Array<uint8> payload = U32ArrayPayload(allocator, arrays[index]);
            Array<uint8> record =
                HolderRecord(allocator, "targets", WireType::ValueArray, payload.View());
            const test::graph::Node node{TypeIdOf<WeakHolder>(), 0, {}, record.View()};
            Array<uint8> graph = Graph(allocator, Span<const test::graph::Node>(&node, 1), 1);
            CHECK(Validate(allocator, objects, types, graph.View(),
                           Span<const TypeId>(importTypes, 1)) == expected[index]);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 노드 대상의 기대 형식은 강한 참조와 같은 규칙이다(UE는 경고 후 null — 18.4절 차이): 기대 타입
// WeakTarget인 필드·배열 원소·struct 멤버가 WeakOther 노드를 가리키면 TypeMismatch이고, 기대 타입
// Object 필드(any)와 WeakTarget 파생(WeakHolder) 노드는 받는다.
TEST(WeakObjectRefGraphLoad_ExpectedTypeViolationIsRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x57524c, 2});
        Array<uint8> other = EmptyRecord(allocator, TypeIdOf<WeakOther>());
        const Guid linkTarget = SchemaField(TypeOf<WeakLink>(), "target").id;
        struct Case {
            const char* field;
            uint32 node;
            ErrorCode expected;
        };

        const Case cases[] = {{"target", 2, ErrorCode::TypeMismatch},
                              {"any", 2, ErrorCode::None},
                              {"target", 1, ErrorCode::None},
                              {"targets", 2, ErrorCode::TypeMismatch},
                              {"targets", 1, ErrorCode::None},
                              {"link", 2, ErrorCode::TypeMismatch},
                              {"link", 1, ErrorCode::None}};
        for (const Case& item : cases) {
            const StringView field(item.field);
            Array<uint8> payload(allocator);
            WireType wire = WireType::U32;
            if (field == "targets") {
                const uint32 values[] = {item.node};
                payload = U32ArrayPayload(allocator, Span<const uint32>(values, 1));
                wire = WireType::ValueArray;
            } else if (field == "link") {
                Array<uint8> value = U32Payload(allocator, item.node);
                payload = OneFieldRecord(allocator, TypeIdOf<WeakLink>(), 1, linkTarget,
                                         WireType::U32, value.View());
                wire = WireType::Record;
            } else {
                payload = U32Payload(allocator, item.node);
            }

            Array<uint8> holder = HolderRecord(allocator, field, wire, payload.View());
            const test::graph::Node nodes[] = {{TypeIdOf<WeakHolder>(), 0, {}, holder.View()},
                                               {TypeIdOf<WeakOther>(), 0, {}, other.View()}};
            Array<uint8> graph = Graph(allocator, Span<const test::graph::Node>(nodes, 2));
            CHECK(Validate(allocator, objects, types, graph.View()) == item.expected);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// weak 간선은 대상을 붙잡지 않는다: 편의 복원의 도달 검사는 weak로만 닿는 노드를 InvalidArgument로
// 거부하고(객체를 만들지 않는다 — 강한 참조로 닿으면 복원된다), 단계형 복원 1단계(Validate)는 그
// 그래프를 받으며, GC는
// weak만 남은 대상을 회수한다(그 뒤 weak의 TryPin은 실패하고 다시 저장하면 진단 없는 null이다).
TEST(WeakObjectRefGraphLoad_WeakEdgesAreNotReachabilityOrGcEdges) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x57524c, 3});
        ObjectRegistry target(allocator, types, {0x57524c, 4});
        const auto access = target.OwnerAccess();
        {
            RootRef<WeakHolder> a = New<WeakHolder>(objects);
            RootRef<WeakHolder> b = New<WeakHolder>(objects);
            With(objects, a.Handle(), [&](WeakHolder& value) noexcept {
                value.target = WeakObjectRef<WeakTarget>(b.Handle());
            });

            const ObjectHandle set[] = {a.Handle(), b.Handle()};
            ObjectGraphSaveOptions options;
            options.writeNodeTable = true;
            auto weakOnly =
                SaveObjectGraphSet(allocator, objects, objects.OwnerAccess(), set, options);
            REQUIRE(weakOnly);
            const auto before = target.Stats().liveObjects;
            auto rejected =
                LoadObjectGraph(allocator, target, types, access, weakOnly.Value().Bytes());
            CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
            CHECK(target.Stats().liveObjects == before && target.Stats().reservedObjects == 0);
            CHECK(Validate(allocator, target, types, weakOnly.Value().Bytes()) == ErrorCode::None);

            SetStrong(objects, a.Handle(), &WeakHolder::strong, b.Handle());
            auto strong =
                SaveObjectGraphSet(allocator, objects, objects.OwnerAccess(), set, options);
            REQUIRE(strong);
            auto loaded = LoadObjectGraph(allocator, target, types, access, strong.Value().Bytes());
            REQUIRE(loaded);
            const ObjectHandle la = loaded.Value().Handle();
            ObjectHandle lb{};
            With(target, la, [&](WeakHolder& value) noexcept {
                lb = value.strong.Handle();
                CHECK(value.target.Handle() == lb);
            });

            // 강한 참조를 끊으면 weak만 남은 b는 회수된다.
            With(target, la, [&](WeakHolder& value) noexcept {
                REQUIRE(target.SetReference(access, la, value.strong, ObjectHandle{}));
            });
            const auto alive = target.Stats().liveObjects;
            REQUIRE(target.Collect(access));
            CHECK(target.Stats().liveObjects == alive - 1);
            With(target, la, [&](WeakHolder& value) noexcept {
                CHECK(value.target.Handle() == lb);
                CHECK(!value.target.TryPin(target, access));
            });

            Recorder hooks;
            ObjectGraphSaveOptions resave;
            resave.hooks = &hooks;
            auto saved = SaveObjectGraph(allocator, target, access, la, resave);
            REQUIRE(saved);
            CHECK(hooks.count == 0);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// weak 참조를 (직간접으로) 담는 struct는 형질 ContainsWeakReference가 서고(강한 참조 형질과
// 무관하다) 1단계 관찰 대상이 될 수 없다(InvalidArgument — 1단계에는 대상 handle이 없다).
TEST(WeakObjectRefGraphLoad_ObservedStructsCannotHoldWeakReferences) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x57524c, 5});
        const TypeId structs[] = {TypeIdOf<WeakLink>(), TypeIdOf<WeakLinkGroup>()};
        for (TypeId id : structs) {
            auto lease = types.AcquireValueType(id, TypeKind::Struct);
            REQUIRE(lease);
            CHECK(lease.Value().Struct().ContainsWeakReference());
            CHECK(!lease.Value().Struct().ContainsStrongReference());
        }

        Array<uint8> record = EmptyRecord(allocator, TypeIdOf<WeakHolder>());
        const test::graph::Node node{TypeIdOf<WeakHolder>(), 0, {}, record.View()};
        Array<uint8> graph = Graph(allocator, Span<const test::graph::Node>(&node, 1));
        for (TypeId id : structs) {
            Recorder hooks;
            hooks.observed = id;
            CHECK(Validate(allocator, objects, types, graph.View(), {}, &hooks) ==
                  ErrorCode::InvalidArgument);
        }

        Recorder plain;
        CHECK(Validate(allocator, objects, types, graph.View(), {}, &plain) == ErrorCode::None);
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
