#include <Tests/Support/Test.h>
#include <Tests/Support/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldAccess.h>
#include <ObjectSerialization/ObjectGraph.h>

using namespace rocks;

namespace {

constexpr Guid NodeType{0x53455249414cULL, 1};
constexpr Guid NumberId{1, 1};
constexpr Guid TextId{1, 2};
constexpr Guid LeftId{1, 3};
constexpr Guid RightId{1, 4};
constexpr Guid SmallId{1, 5};
constexpr Guid KeyId{1, 6};
constexpr Guid SignedId{1, 7};
constexpr Guid FloatId{1, 8};
constexpr Guid DoubleId{1, 9};
constexpr Guid BoolId{1, 10};
uint32 LiveNodes = 0;

struct GraphNode final : Object {
    explicit GraphNode(Allocator& allocator) noexcept : text(allocator) {
        ++LiveNodes;
    }

    ~GraphNode() noexcept {
        --LiveNodes;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(left);
        visitor.Visit(right);
    }

    uint32 number = 7;
    String text;
    ObjectRef<GraphNode> left;
    ObjectRef<GraphNode> right;
    uint8 small = 3;
    Guid key{};
    int64 signedNumber = -1;
    float weight = 1.25f;
    double precision = 2.5;
    bool enabled = false;
};

template <auto Member>
RuntimeFieldOps FieldOps(Guid id) noexcept {
    return {
        id,
        [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
           FieldValue& value) noexcept {
            return detail::ReadGeneratedField<GraphNode>(registry, access, object, Member, value);
        },
        [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
           const FieldValue& value) noexcept {
            return detail::WriteGeneratedField<GraphNode>(registry, access, object, Member, value);
        },
        [](ConstructionContext& transaction, ObjectHandle object,
           const FieldValue& value) noexcept {
            return detail::WriteGeneratedStagedField<GraphNode>(transaction, object, Member, value);
        }};
}

FieldSchema SchemaField(Guid id, ValueKind kind) noexcept {
    FieldSchema field;
    field.id = id;
    field.name = {"testField", 9};
    field.valueKind = kind;
    if (kind == ValueKind::ObjectReference) {
        field.kind = FieldKind::ObjectReference;
        field.referencedType = NodeType;
    }
    return field;
}

Result<TypeRegistrationToken> RegisterNode(TypeRegistry& types, uint32 version = 1,
                                           bool complete = true) noexcept {
    const FieldSchema fields[] = {SchemaField(NumberId, ValueKind::UInt32),
                                  SchemaField(TextId, ValueKind::Utf8String),
                                  SchemaField(LeftId, ValueKind::ObjectReference),
                                  SchemaField(RightId, ValueKind::ObjectReference),
                                  SchemaField(SmallId, ValueKind::UInt8),
                                  SchemaField(KeyId, ValueKind::Guid),
                                  SchemaField(SignedId, ValueKind::Int64),
                                  SchemaField(FloatId, ValueKind::Float32),
                                  SchemaField(DoubleId, ValueKind::Float64),
                                  SchemaField(BoolId, ValueKind::Bool)};
    const RuntimeFieldOps accessors[] = {
        FieldOps<&GraphNode::number>(NumberId),       FieldOps<&GraphNode::text>(TextId),
        FieldOps<&GraphNode::left>(LeftId),           FieldOps<&GraphNode::right>(RightId),
        FieldOps<&GraphNode::small>(SmallId),         FieldOps<&GraphNode::key>(KeyId),
        FieldOps<&GraphNode::signedNumber>(SignedId), FieldOps<&GraphNode::weight>(FloatId),
        FieldOps<&GraphNode::precision>(DoubleId),    FieldOps<&GraphNode::enabled>(BoolId)};
    auto ops = MakeRuntimeTypeOps<GraphNode>();
    ops.fields = accessors;
    ops.completeFieldLayout = complete;
    return types.Register({NodeType, version, fields, {"GraphNode", 9}}, Move(ops));
}

struct SourceGraph {
    SystemAllocator allocator;
    TypeRegistry types{allocator};
    ObjectRegistry registry{allocator, types, {88, 99}};
    RootRef<GraphNode> root;
    RootRef<GraphNode> child;

    Result<void> Initialize() noexcept {
        auto registered = RegisterNode(types);
        if (!registered) {
            return Result<void>::Failure(registered.Error());
        }
        auto first = registry.NewObject<GraphNode>(registry.OwnerAccess(), NodeType);
        if (!first) {
            return Result<void>::Failure(first.Error());
        }
        root = Move(first.Value());
        auto second = registry.NewObject<GraphNode>(registry.OwnerAccess(), NodeType);
        if (!second) {
            return Result<void>::Failure(second.Error());
        }
        child = Move(second.Value());
        auto a = registry.Resolve<GraphNode>(registry.OwnerAccess(), root.Handle());
        if (!a) {
            return Result<void>::Failure(a.Error());
        }
        auto b = registry.Resolve<GraphNode>(registry.OwnerAccess(), child.Handle());
        if (!b) {
            return Result<void>::Failure(b.Error());
        }
        auto av = a.Value().Get();
        auto bv = b.Value().Get();
        if (!av || !bv) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
        av.Value()->number = 42;
        av.Value()->small = 255;
        av.Value()->key = {0x11223344, 0xaabbccdd};
        av.Value()->signedNumber = -9223372036854775807LL - 1;
        av.Value()->enabled = true;
        auto text = av.Value()->text.TryAssign("\xED\x95\x9C\xEA\xB8\x80 \xF0\x9F\x8E\xAE");
        if (!text) {
            return text;
        }
        auto edge = registry.SetReference(registry.OwnerAccess(), root.Handle(), av.Value()->left,
                                          child.Handle());
        if (!edge) {
            return edge;
        }
        edge = registry.SetReference(registry.OwnerAccess(), root.Handle(), av.Value()->right,
                                     child.Handle());
        if (!edge) {
            return edge;
        }
        edge = registry.SetReference(registry.OwnerAccess(), child.Handle(), bv.Value()->left,
                                     root.Handle());
        if (!edge) {
            return edge;
        }
        return registry.SetReference(registry.OwnerAccess(), child.Handle(), bv.Value()->right,
                                     child.Handle());
    }
};

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

Result<Array<uint8>> SingleGraph(Allocator& allocator, Span<const uint8> record,
                                 uint32 version = 1) noexcept {
    const uint8 magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
    ArchiveWriter writer(allocator);
    auto written = writer.WriteRaw(magic);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }
    written = writer.WriteU32(1);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }
    written = writer.WriteU32(1);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }
    written = writer.WriteU32(1);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }
    written = writer.WriteU64(1);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }
    written = writer.WriteGuid(NodeType);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }
    written = writer.WriteU32(version);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }
    written = writer.WriteBytes(record);
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }
    return Result<Array<uint8>>::Success(writer.TakeBytes());
}

Result<Array<uint8>> MigrateVersion(Allocator& allocator, TypeId type, uint32 sourceVersion,
                                    uint32 targetVersion, Span<const uint8> source,
                                    void* user) noexcept {
    ++*static_cast<uint32*>(user);
    auto reader = RecordReader::Open(allocator, source, type, sourceVersion, sourceVersion);
    if (!reader) {
        return Result<Array<uint8>>::Failure(reader.Error());
    }
    RecordBuilder builder(allocator, type, targetVersion);
    for (usize index = 0; index < reader.Value().FieldCount(); ++index) {
        auto field = reader.Value().Next();
        if (!field) {
            return Result<Array<uint8>>::Failure(field.Error());
        }
        auto added =
            builder.AddField(field.Value().id, field.Value().wireType, field.Value().payload);
        if (!added) {
            return Result<Array<uint8>>::Failure(added.Error());
        }
    }
    return builder.Build();
}

Result<void> LoadWithFailure(test::NFailAllocator& allocator, TypeRegistry& types,
                             Span<const uint8> bytes, uint64* attempts = nullptr) noexcept {
    ObjectRegistry registry(allocator, types, {123, 456});
    auto result = LoadObjectGraph(allocator, registry, types, registry.OwnerAccess(), bytes);
    if (attempts != nullptr) {
        *attempts = allocator.Attempts();
    }
    if (!result) {
        if (registry.Stats().liveObjects != 0 || registry.Stats().reservedObjects != 0) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
        return Result<void>::Failure(result.Error());
    }
    result.Value().Reset();
    auto collected = registry.Collect(registry.OwnerAccess());
    return collected ? Result<void>::Success() : Result<void>::Failure(collected.Error());
}

} // namespace

TEST(ObjectGraphRoundTripPreservesCycleSharedIdentityAndTypedValues) {
    SourceGraph source;
    REQUIRE(source.Initialize());
    auto bytes = SaveObjectGraph(source.allocator, source.registry, source.registry.OwnerAccess(),
                                 source.root.Handle());
    REQUIRE(bytes);
    ObjectRegistry target(source.allocator, source.types, {100, 200});
    auto loaded = LoadObjectGraph(source.allocator, target, source.types, target.OwnerAccess(),
                                  bytes.Value().View());
    REQUIRE(loaded);
    CHECK(loaded.Value().Handle().registry != source.root.Handle().registry);
    CHECK(target.Stats().liveObjects == 2);
    {
        auto root = target.Resolve<GraphNode>(target.OwnerAccess(), loaded.Value().Handle());
        REQUIRE(root);
        auto value = root.Value().Get();
        REQUIRE(value);
        CHECK(value.Value()->number == 42);
        CHECK(value.Value()->small == 255);
        CHECK((value.Value()->key == Guid{0x11223344, 0xaabbccdd}));
        CHECK(value.Value()->signedNumber == -9223372036854775807LL - 1);
        CHECK(value.Value()->enabled);
        CHECK(value.Value()->weight == 1.25f);
        CHECK(value.Value()->precision == 2.5);
        CHECK(value.Value()->text.View() ==
              StringView("\xED\x95\x9C\xEA\xB8\x80 \xF0\x9F\x8E\xAE"));
        CHECK(value.Value()->left.Handle() == value.Value()->right.Handle());
        auto child = target.Resolve<GraphNode>(target.OwnerAccess(), value.Value()->left.Handle());
        REQUIRE(child);
        auto childValue = child.Value().Get();
        REQUIRE(childValue);
        CHECK(childValue.Value()->left.Handle() == loaded.Value().Handle());
        CHECK(childValue.Value()->right.Handle() == childValue.Value()->Handle());
    }
    auto second =
        SaveObjectGraph(source.allocator, target, target.OwnerAccess(), loaded.Value().Handle());
    REQUIRE(second);
    CHECK(SameBytes(bytes.Value().View(), second.Value().View()));
    loaded.Value().Reset();
    auto gc = target.Collect(target.OwnerAccess());
    REQUIRE(gc);
    CHECK(gc.Value().reclaimed == 2);
}

TEST(ObjectGraphUnknownFieldsSkipAndMissingFieldsNeedExplicitDefaultPolicy) {
    SourceGraph source;
    REQUIRE(source.Initialize());
    ArchiveWriter value(source.allocator);
    REQUIRE(value.WriteU32(99));
    RecordBuilder record(source.allocator, NodeType, 1);
    REQUIRE(record.AddField(NumberId, WireType::U32, value.View()));
    const uint8 unknown[] = {0xff, 0xff};
    REQUIRE(record.AddField({99, 99}, 0xf00du, unknown));
    auto recordBytes = record.Build();
    REQUIRE(recordBytes);
    auto bytes = SingleGraph(source.allocator, recordBytes.Value().View());
    REQUIRE(bytes);
    ObjectRegistry target(source.allocator, source.types, {101, 201});
    auto strict = LoadObjectGraph(source.allocator, target, source.types, target.OwnerAccess(),
                                  bytes.Value().View());
    CHECK(!strict);
    CHECK(strict.Error() == ErrorCode::NotFound);
    CHECK(target.Stats().liveObjects == 0);
    ObjectGraphLoadOptions options;
    options.allowMissingFields = true;
    auto permissive = LoadObjectGraph(source.allocator, target, source.types, target.OwnerAccess(),
                                      bytes.Value().View(), options);
    REQUIRE(permissive);
    auto object = target.Resolve<GraphNode>(target.OwnerAccess(), permissive.Value().Handle());
    REQUIRE(object);
    auto data = object.Value().Get();
    REQUIRE(data);
    CHECK(data.Value()->number == 99);
    CHECK(data.Value()->small == 3);
    CHECK(data.Value()->text.IsEmpty());
    CHECK(data.Value()->left.Handle().IsNull());
}

TEST(ObjectGraphSchemaMigrationIsExplicitAndOutputIsRevalidated) {
    SourceGraph source;
    REQUIRE(source.Initialize());
    auto bytes = SaveObjectGraph(source.allocator, source.registry, source.registry.OwnerAccess(),
                                 source.root.Handle());
    REQUIRE(bytes);
    TypeRegistry newTypes(source.allocator);
    REQUIRE(RegisterNode(newTypes, 2));
    ObjectRegistry target(source.allocator, newTypes, {102, 202});
    auto old = LoadObjectGraph(source.allocator, target, newTypes, target.OwnerAccess(),
                               bytes.Value().View());
    CHECK(!old);
    CHECK(old.Error() == ErrorCode::Unsupported);
    uint32 calls = 0;
    ObjectGraphLoadOptions options;
    options.migrate = &MigrateVersion;
    options.migrationUser = &calls;
    auto migrated = LoadObjectGraph(source.allocator, target, newTypes, target.OwnerAccess(),
                                    bytes.Value().View(), options);
    REQUIRE(migrated);
    CHECK(calls == 2);
    CHECK(target.Stats().liveObjects == 2);
    migrated.Value().Reset();
    REQUIRE(target.Collect(target.OwnerAccess()));
    options.limits.maxMigrationBytes = 0;
    auto bounded = LoadObjectGraph(source.allocator, target, newTypes, target.OwnerAccess(),
                                   bytes.Value().View(), options);
    CHECK(!bounded);
    CHECK(bounded.Error() == ErrorCode::CapacityExceeded);
    CHECK(target.Stats().liveObjects == 0);
}

TEST(ObjectGraphRejectsForeignTypeRegistryAndIncompleteMetadata) {
    SourceGraph source;
    REQUIRE(source.Initialize());
    auto bytes = SaveObjectGraph(source.allocator, source.registry, source.registry.OwnerAccess(),
                                 source.root.Handle());
    REQUIRE(bytes);
    TypeRegistry incomplete(source.allocator);
    REQUIRE(RegisterNode(incomplete, 1, false));
    ObjectRegistry target(source.allocator, incomplete, {103, 203});
    auto wrong = LoadObjectGraph(source.allocator, target, source.types, target.OwnerAccess(),
                                 bytes.Value().View());
    CHECK(!wrong);
    CHECK(wrong.Error() == ErrorCode::WrongContext);
    auto missingLayout = LoadObjectGraph(source.allocator, target, incomplete, target.OwnerAccess(),
                                         bytes.Value().View());
    CHECK(!missingLayout);
    CHECK(missingLayout.Error() == ErrorCode::Unsupported);
    auto object = target.NewObject<GraphNode>(target.OwnerAccess(), NodeType);
    REQUIRE(object);
    auto save =
        SaveObjectGraph(source.allocator, target, target.OwnerAccess(), object.Value().Handle());
    CHECK(!save);
    CHECK(save.Error() == ErrorCode::Unsupported);
}

TEST(ObjectGraphMalformedKnownFieldsNeverPublishObjects) {
    SourceGraph source;
    REQUIRE(source.Initialize());
    ObjectRegistry target(source.allocator, source.types, {104, 204});

    struct Case {
        Guid id;
        WireType type;
        uint8 bytes[8];
        usize size;
        ErrorCode error;
    };

    const Case cases[] = {{NumberId, WireType::U64, {1}, 8, ErrorCode::TypeMismatch},
                          {NumberId, WireType::U32, {1}, 5, ErrorCode::InvalidArgument},
                          {SmallId, WireType::U32, {0, 1}, 4, ErrorCode::Overflow},
                          {BoolId, WireType::Bool, {2}, 1, ErrorCode::InvalidArgument},
                          {LeftId, WireType::U64, {2}, 8, ErrorCode::InvalidArgument}};
    ObjectGraphLoadOptions options;
    options.allowMissingFields = true;
    for (const auto& example : cases) {
        RecordBuilder record(source.allocator, NodeType, 1);
        REQUIRE(record.AddField(example.id, example.type, {example.bytes, example.size}));
        auto recordBytes = record.Build();
        REQUIRE(recordBytes);
        auto bytes = SingleGraph(source.allocator, recordBytes.Value().View());
        REQUIRE(bytes);
        auto loaded = LoadObjectGraph(source.allocator, target, source.types, target.OwnerAccess(),
                                      bytes.Value().View(), options);
        CHECK(!loaded);
        CHECK(loaded.Error() == example.error);
        CHECK(target.Stats().liveObjects == 0);
        CHECK(target.Stats().reservedObjects == 0);
    }
}

TEST(ObjectGraphLimitsAreAppliedBeforePublication) {
    SourceGraph source;
    REQUIRE(source.Initialize());
    ObjectGraphLimits limits;
    limits.maxObjects = 1;
    auto tooMany = SaveObjectGraph(source.allocator, source.registry, source.registry.OwnerAccess(),
                                   source.root.Handle(), limits);
    CHECK(!tooMany);
    CHECK(tooMany.Error() == ErrorCode::CapacityExceeded);
    limits = {};
    limits.maxValueBytes = 1;
    auto tooLong = SaveObjectGraph(source.allocator, source.registry, source.registry.OwnerAccess(),
                                   source.root.Handle(), limits);
    CHECK(!tooLong);
    CHECK(tooLong.Error() == ErrorCode::CapacityExceeded);
    auto bytes = SaveObjectGraph(source.allocator, source.registry, source.registry.OwnerAccess(),
                                 source.root.Handle());
    REQUIRE(bytes);
    ObjectRegistry target(source.allocator, source.types, {105, 205});
    ObjectGraphLoadOptions options;
    options.limits.maxWork = 0;
    auto noWork = LoadObjectGraph(source.allocator, target, source.types, target.OwnerAccess(),
                                  bytes.Value().View(), options);
    CHECK(!noWork);
    CHECK(noWork.Error() == ErrorCode::CapacityExceeded);
    CHECK(target.Stats().liveObjects == 0);
}

TEST(ObjectGraphEverySaveAllocationFailureReleasesTemporaryOwnership) {
    SourceGraph source;
    REQUIRE(source.Initialize());
    test::NFailAllocator failure(source.allocator);
    uint64 attempts = 0;
    {
        auto baseline = SaveObjectGraph(failure, source.registry, source.registry.OwnerAccess(),
                                        source.root.Handle());
        REQUIRE(baseline);
        attempts = failure.Attempts();
    }
    const auto baselineStats = source.allocator.Stats();
    const auto baselineLeases = source.types.Stats().leases;
    for (uint64 index = 1; index <= attempts; ++index) {
        failure.FailOnAttempt(index);
        auto saved = SaveObjectGraph(failure, source.registry, source.registry.OwnerAccess(),
                                     source.root.Handle());
        CHECK(!saved);
        CHECK(saved.Error() == ErrorCode::OutOfMemory);
        CHECK(source.allocator.Stats().liveAllocations == baselineStats.liveAllocations);
        CHECK(source.allocator.Stats().liveBytes == baselineStats.liveBytes);
        CHECK(source.types.Stats().leases == baselineLeases);
        CHECK(source.registry.Stats().borrows == 0);
        CHECK(LiveNodes == 2);
    }
}

TEST(ObjectGraphEveryLoadAllocationFailureRollsBackNodesFieldsAndLeases) {
    SourceGraph source;
    REQUIRE(source.Initialize());
    auto bytes = SaveObjectGraph(source.allocator, source.registry, source.registry.OwnerAccess(),
                                 source.root.Handle());
    REQUIRE(bytes);
    test::NFailAllocator failure(source.allocator);
    uint64 attempts = 0;
    REQUIRE(LoadWithFailure(failure, source.types, bytes.Value().View(), &attempts));
    const auto baselineStats = source.allocator.Stats();
    const auto baselineLeases = source.types.Stats().leases;
    for (uint64 index = 1; index <= attempts; ++index) {
        failure.FailOnAttempt(index);
        auto loaded = LoadWithFailure(failure, source.types, bytes.Value().View());
        CHECK(!loaded);
        CHECK(loaded.Error() == ErrorCode::OutOfMemory);
        CHECK(source.allocator.Stats().liveAllocations == baselineStats.liveAllocations);
        CHECK(source.allocator.Stats().liveBytes == baselineStats.liveBytes);
        CHECK(source.types.Stats().leases == baselineLeases);
        CHECK(LiveNodes == 2);
    }
}

TEST(ObjectGraphDeterministicMutationHarnessHasNoPartialPublicationOrLeaks) {
    SourceGraph source;
    REQUIRE(source.Initialize());
    auto original = SaveObjectGraph(source.allocator, source.registry,
                                    source.registry.OwnerAccess(), source.root.Handle());
    REQUIRE(original);
    uint32 random = 0x7f4a7c15u;
    for (uint32 iteration = 0; iteration < 256; ++iteration) {
        ArchiveWriter scratch(source.allocator);
        REQUIRE(scratch.WriteRaw(original.Value().View()));
        auto bytes = scratch.TakeBytes();
        random = random * 1664525u + 1013904223u;
        const usize index = (random >> 16) % bytes.Size();
        bytes[index] ^= static_cast<uint8>(1u << (iteration % 8));
        ObjectRegistry target(source.allocator, source.types, {106, iteration});
        auto loaded = LoadObjectGraph(source.allocator, target, source.types, target.OwnerAccess(),
                                      bytes.View());
        if (loaded) {
            loaded.Value().Reset();
            REQUIRE(target.Collect(target.OwnerAccess()));
        }
        CHECK(target.Stats().liveObjects == 0);
        CHECK(target.Stats().reservedObjects == 0);
        CHECK(LiveNodes == 2);
    }
}
