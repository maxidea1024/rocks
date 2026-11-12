#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <TestSupport/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <ObjectSerialization/ObjectGraph.h>

using namespace rocks;
using reflection_fixture::Base;
using reflection_fixture::Node;

namespace {
constexpr Guid HolderType{0xc0dec0de, 1};
constexpr Guid HolderEdge{0xc0dec0de, 2};
constexpr Guid AliasBaseType{0xc0dec0de, 3};
uint32 ConstructedForReview = 0;

struct BaseHolder final : Object {
    BaseHolder() noexcept {
        ++ConstructedForReview;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(edge);
    }

    ObjectRef<Base> edge;
};

Result<TypeRegistrationToken> RegisterHolder(TypeRegistry& types) noexcept {
    const FieldSchema fields[] = {{HolderEdge,
                                   FieldKind::ObjectReference,
                                   0,
                                   {"edge", 4},
                                   {},
                                   ValueKind::ObjectReference,
                                   GeneratedType<Base>::Id()}};
    const RuntimeFieldOps accessors[] = {
        {HolderEdge,
         [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle owner,
            FieldValue& output) noexcept {
             return detail::ReadGeneratedField<BaseHolder>(registry, access, owner,
                                                           &BaseHolder::edge, output);
         },
         [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle owner,
            const FieldValue& input) noexcept {
             return detail::WriteGeneratedField<BaseHolder>(registry, access, owner,
                                                            &BaseHolder::edge, input);
         },
         [](ConstructionContext& transaction, ObjectHandle owner,
            const FieldValue& input) noexcept {
             return detail::WriteGeneratedStagedField<BaseHolder>(transaction, owner,
                                                                  &BaseHolder::edge, input);
         }}};
    auto ops = MakeRuntimeTypeOps<BaseHolder>();
    ops.fields = accessors;
    ops.completeFieldLayout = true;
    return types.Register({HolderType, 1, fields, {"BaseHolder", 10}}, Move(ops));
}

bool RegisterGenerated(TypeRegistry& types, bool countConstruction = false) noexcept {
    if (!GeneratedType<Base>::Register(types)) {
        return false;
    }
    auto ops = GeneratedType<Node>::Ops();
    if (countConstruction) {
        ops.construct = [](void* memory, Allocator& allocator) noexcept -> Object* {
            ++ConstructedForReview;
            return MakeRuntimeTypeOps<Node>().construct(memory, allocator);
        };
    }
    return types.Register(GeneratedType<Node>::Schema(), Move(ops)).IsOk();
}

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }
    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

bool AppendObject(ArchiveWriter& writer, uint64 localId, Guid type, uint32 version,
                  Span<const uint8> record) noexcept {
    return writer.WriteU64(localId).IsOk() && writer.WriteGuid(type).IsOk() &&
           writer.WriteU32(version).IsOk() && writer.WriteBytes(record).IsOk();
}

Result<Array<uint8>> HolderGraph(Allocator& allocator, Guid targetType,
                                 uint32 targetVersion) noexcept {
    ArchiveWriter reference(allocator);
    auto written = reference.WriteU64(2);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }
    RecordBuilder holder(allocator, HolderType, 1);
    auto added = holder.AddField(HolderEdge, WireType::U64, reference.View());
    if (!added) {
        return Result<Array<uint8>>::Failure(added.Error());
    }
    auto holderBytes = holder.Build();
    if (!holderBytes) {
        return Result<Array<uint8>>::Failure(holderBytes.Error());
    }
    RecordBuilder target(allocator, targetType, targetVersion);
    auto targetBytes = target.Build();
    if (!targetBytes) {
        return Result<Array<uint8>>::Failure(targetBytes.Error());
    }
    // 공개 envelope 명세만 조립한다. 참조가 실제로 대입 가능한지는 loader가 판단한다.
    ArchiveWriter output(allocator);
    const uint8 magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
    if (!output.WriteRaw(magic) || !output.WriteU32(1) || !output.WriteU32(2) ||
        !output.WriteU32(1) ||
        !AppendObject(output, 1, HolderType, 1, holderBytes.Value().View()) ||
        !AppendObject(output, 2, targetType, targetVersion, targetBytes.Value().View())) {
        return Result<Array<uint8>>::Failure(ErrorCode::DependencyFailed);
    }
    return Result<Array<uint8>>::Success(output.TakeBytes());
}
} // namespace

TEST(ObjectGraphGeneratedInheritanceRoundTripOutlivesSourceAndScratchAllocator) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterGenerated(types));
        ObjectRegistry target(allocator, types, {920, 2});
        RootRef<Object> restored;
        ObjectHandle sourceIdentity{};
        const char label[] = "root\0\xED\x95\x9C\xEA\xB8\x80 \xF0\x9F\x8E\xAE";
        {
            // 저장 출력, decoded 문자열, source graph가 모두 먼저 소멸해도 target은 독립적이다.
            SystemAllocator scratch;
            {
                ObjectRegistry source(allocator, types, {920, 1});
                auto first =
                    source.NewObject<Node>(source.OwnerAccess(), GeneratedType<Node>::Id());
                auto second =
                    source.NewObject<Node>(source.OwnerAccess(), GeneratedType<Node>::Id());
                REQUIRE(first && second);
                sourceIdentity = first.Value().Handle();
                {
                    auto a = source.Resolve<Node>(source.OwnerAccess(), first.Value().Handle());
                    auto b = source.Resolve<Node>(source.OwnerAccess(), second.Value().Handle());
                    REQUIRE(a && b);
                    auto av = a.Value().Get();
                    auto bv = b.Value().Get();
                    REQUIRE(av && bv);
                    av.Value()->key = {0xfedcba9876543210ull, 0x0123456789abcdefull};
                    av.Value()->count = -2147483647 - 1;
                    av.Value()->enabled = true;
                    av.Value()->small = 255;
                    av.Value()->medium = 65535;
                    av.Value()->flags = 0xffffffffu;
                    av.Value()->wide = 0xffffffffffffffffull;
                    av.Value()->signedWide = -9223372036854775807ll - 1;
                    av.Value()->weight = -1.25f;
                    av.Value()->precision = 0.125;
                    REQUIRE(av.Value()->label.TryAssign({label, sizeof(label) - 1}));
                    bv.Value()->count = 73;
                    REQUIRE(bv.Value()->label.TryAssign("child"));
                    REQUIRE(source.SetReference(source.OwnerAccess(), first.Value().Handle(),
                                                av.Value()->next, second.Value().Handle()));
                    REQUIRE(source.SetReference(source.OwnerAccess(), second.Value().Handle(),
                                                bv.Value()->next, first.Value().Handle()));
                }
                auto saved =
                    SaveObjectGraph(scratch, source, source.OwnerAccess(), first.Value().Handle());
                REQUIRE(saved);
                auto loaded = LoadObjectGraph(scratch, target, types, target.OwnerAccess(),
                                              saved.Value().View());
                REQUIRE(loaded);
                restored = Move(loaded.Value());
                auto again =
                    SaveObjectGraph(scratch, target, target.OwnerAccess(), restored.Handle());
                REQUIRE(again);
                CHECK(SameBytes(saved.Value().View(), again.Value().View()));
            }
            CHECK(scratch.Stats().liveAllocations == 0);
            CHECK(scratch.Stats().liveBytes == 0);
        }
        CHECK(restored.Handle().registry != sourceIdentity.registry);
        REQUIRE(target.Collect(target.OwnerAccess()));
        CHECK(target.Stats().liveObjects == 2);
        {
            auto borrowed = target.Resolve<Node>(target.OwnerAccess(), restored.Handle());
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            CHECK((value.Value()->key == Guid{0xfedcba9876543210ull, 0x0123456789abcdefull}));
            CHECK(value.Value()->count == -2147483647 - 1);
            CHECK(value.Value()->enabled && value.Value()->small == 255 &&
                  value.Value()->medium == 65535);
            CHECK(value.Value()->flags == 0xffffffffu &&
                  value.Value()->wide == 0xffffffffffffffffull);
            CHECK(value.Value()->signedWide == -9223372036854775807ll - 1);
            CHECK(value.Value()->weight == -1.25f && value.Value()->precision == 0.125);
            CHECK(value.Value()->label.View() == StringView(label, sizeof(label) - 1));
            auto child = target.Resolve<Node>(target.OwnerAccess(), value.Value()->next.Handle());
            REQUIRE(child);
            auto childValue = child.Value().Get();
            REQUIRE(childValue);
            CHECK(childValue.Value()->count == 73);
            CHECK(childValue.Value()->label.View() == StringView("child"));
            CHECK(childValue.Value()->next.Handle() == restored.Handle());
        }
        restored.Reset();
        auto collected = target.Collect(target.OwnerAccess());
        REQUIRE(collected);
        CHECK(collected.Value().reclaimed == 2);
        CHECK(target.Stats().liveObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectGraphDerivedReferenceIntoBaseFieldIsAccepted) {
    // ObjectRef<Base> 필드에 파생 Node 객체를 저장한 그래프는 SetReference와 같은 상속 규칙으로
    // 복원한다.
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterGenerated(types, true));
        REQUIRE(RegisterHolder(types));
        auto source = HolderGraph(allocator, GeneratedType<Node>::Id(),
                                  GeneratedType<Node>::Schema().version);
        REQUIRE(source);
        ObjectRegistry target(allocator, types, {921, 1});
        ObjectGraphLoadOptions options;
        options.allowMissingFields = true;
        ConstructedForReview = 0;
        auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                      source.Value().View(), options);
        REQUIRE(loaded);
        // BaseHolder와 Node가 각각 한 번씩 생성된다(두 생성자 모두 계수한다).
        CHECK(ConstructedForReview == 2);
        CHECK(target.Stats().liveObjects == 2);
        {
            auto holder = target.Resolve<BaseHolder>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(holder);
            auto value = holder.Value().Get();
            REQUIRE(value);
            auto node = target.Resolve<Node>(target.OwnerAccess(), value.Value()->edge.Handle());
            CHECK(node.IsOk());
        }
        loaded.Value().Reset();
        REQUIRE(target.Collect(target.OwnerAccess()));
        CHECK(target.Stats().liveObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectGraphUnrelatedReferenceTypeIsRejectedBeforeAnyConstructor) {
    // 선언 타입과 상속 관계가 없는 객체(BaseHolder 자신)를 가리키면 생성 전에 거부한다.
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterGenerated(types, true));
        REQUIRE(RegisterHolder(types));
        auto source = HolderGraph(allocator, HolderType, 1);
        REQUIRE(source);
        ObjectRegistry target(allocator, types, {923, 1});
        ObjectGraphLoadOptions options;
        options.allowMissingFields = true;
        ConstructedForReview = 0;
        const auto before = allocator.Stats();
        const auto leases = types.Stats().leases;
        auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                      source.Value().View(), options);
        CHECK(!loaded && loaded.Error() == ErrorCode::TypeMismatch);
        CHECK(ConstructedForReview == 0);
        CHECK(target.Stats().liveObjects == 0 && target.Stats().reservedObjects == 0);
        CHECK(target.Stats().chunks == 0);
        CHECK(types.Stats().leases == leases);
        CHECK(allocator.Stats().liveAllocations == before.liveAllocations);
        CHECK(allocator.Stats().liveBytes == before.liveBytes);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectGraphReferenceToNativeAliasTypePreservesAliasSchemaOnResave) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterGenerated(types));
        REQUIRE(RegisterHolder(types));
        auto aliasSchema = GeneratedType<Base>::Schema();
        aliasSchema.id = AliasBaseType;
        REQUIRE(types.Register(aliasSchema, GeneratedType<Base>::Ops()));
        auto source = HolderGraph(allocator, AliasBaseType, aliasSchema.version);
        REQUIRE(source);
        ObjectRegistry target(allocator, types, {922, 1});
        ObjectGraphLoadOptions options;
        options.allowMissingFields = true;
        auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                      source.Value().View(), options);
        REQUIRE(loaded);
        ObjectHandle aliasHandle{};
        {
            auto holder = target.Resolve<BaseHolder>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(holder);
            auto value = holder.Value().Get();
            REQUIRE(value);
            aliasHandle = value.Value()->edge.Handle();
            auto alias = target.Resolve<Base>(target.OwnerAccess(), aliasHandle);
            REQUIRE(alias);
            auto aliasValue = alias.Value().Get();
            REQUIRE(aliasValue);
            aliasValue.Value()->count = 314;
        }
        auto actual = target.AcquireType(target.OwnerAccess(), aliasHandle);
        REQUIRE(actual);
        CHECK(actual.Value().Schema().id == AliasBaseType);
        auto saved =
            SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
        REQUIRE(saved);
        ObjectRegistry second(allocator, types, {922, 2});
        auto reloaded =
            LoadObjectGraph(allocator, second, types, second.OwnerAccess(), saved.Value().View());
        REQUIRE(reloaded);
        {
            auto holder =
                second.Resolve<BaseHolder>(second.OwnerAccess(), reloaded.Value().Handle());
            REQUIRE(holder);
            auto value = holder.Value().Get();
            REQUIRE(value);
            auto aliasType = second.AcquireType(second.OwnerAccess(), value.Value()->edge.Handle());
            REQUIRE(aliasType);
            CHECK(aliasType.Value().Schema().id == AliasBaseType);
            auto alias = second.Resolve<Base>(second.OwnerAccess(), value.Value()->edge.Handle());
            REQUIRE(alias);
            auto aliasValue = alias.Value().Get();
            REQUIRE(aliasValue);
            CHECK(aliasValue.Value()->count == 314);
        }
        reloaded.Value().Reset();
        loaded.Value().Reset();
        REQUIRE(second.Collect(second.OwnerAccess()));
        REQUIRE(target.Collect(target.OwnerAccess()));
        CHECK(second.Stats().liveObjects == 0 && target.Stats().liveObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
