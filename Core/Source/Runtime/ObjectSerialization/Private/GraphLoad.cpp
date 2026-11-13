#include "GraphInternal.h"

namespace rocks {
namespace {

using namespace graph_detail;

Result<Array<LoadNode>> ReadEnvelope(Allocator& allocator, Span<const uint8> source,
                                     Budget& budget) noexcept {
    auto envelope = ArchiveReader::Create(source, budget.Archive(true));
    if (!envelope) {
        return Result<Array<LoadNode>>::Failure(envelope.Error());
    }
    auto magic = envelope.Value().ReadRaw(sizeof(Magic));
    if (!magic) {
        return Result<Array<LoadNode>>::Failure(magic.Error());
    }
    for (usize index = 0; index < sizeof(Magic); ++index) {
        if (magic.Value()[index] != Magic[index]) {
            return Result<Array<LoadNode>>::Failure(ErrorCode::InvalidArgument);
        }
    }
    auto version = envelope.Value().ReadU32();
    if (!version) {
        return Result<Array<LoadNode>>::Failure(version.Error());
    }
    if (version.Value() != 1) {
        return Result<Array<LoadNode>>::Failure(ErrorCode::Unsupported);
    }
    auto count = envelope.Value().ReadU32();
    if (!count) {
        return Result<Array<LoadNode>>::Failure(count.Error());
    }
    auto root = envelope.Value().ReadU32();
    if (!root) {
        return Result<Array<LoadNode>>::Failure(root.Error());
    }
    if (count.Value() == 0 || root.Value() != 1) {
        return Result<Array<LoadNode>>::Failure(ErrorCode::InvalidArgument);
    }
    if (count.Value() > budget.limits.maxObjects) {
        return Result<Array<LoadNode>>::Failure(ErrorCode::CapacityExceeded);
    }
    if (static_cast<uint64>(count.Value()) * (ObjectHeaderBytes + 44) >
        envelope.Value().Remaining()) {
        return Result<Array<LoadNode>>::Failure(ErrorCode::InvalidArgument);
    }
    auto spent = budget.Spend(envelope.Value().WorkUsed());
    if (!spent) {
        return Result<Array<LoadNode>>::Failure(spent.Error());
    }
    spent = budget.Spend(count.Value());
    if (!spent) {
        return Result<Array<LoadNode>>::Failure(spent.Error());
    }
    Array<LoadNode> nodes(allocator);
    auto reserved = nodes.TryReserve(count.Value());
    if (!reserved) {
        return Result<Array<LoadNode>>::Failure(reserved.Error());
    }

    // 예약/초기화 비용을 먼저 차감한 나머지 예산만 body cursor에 준다.
    auto body = ArchiveReader::Create({source.Data() + HeaderBytes, source.Size() - HeaderBytes},
                                      budget.Archive(true));
    if (!body) {
        return Result<Array<LoadNode>>::Failure(body.Error());
    }
    for (uint32 index = 0; index < count.Value(); ++index) {
        auto local = body.Value().ReadU64();
        if (!local) {
            return Result<Array<LoadNode>>::Failure(local.Error());
        }
        if (local.Value() != static_cast<uint64>(index) + 1) {
            return Result<Array<LoadNode>>::Failure(ErrorCode::InvalidArgument);
        }
        auto type = body.Value().ReadGuid();
        if (!type) {
            return Result<Array<LoadNode>>::Failure(type.Error());
        }
        auto schemaVersion = body.Value().ReadU32();
        if (!schemaVersion) {
            return Result<Array<LoadNode>>::Failure(schemaVersion.Error());
        }
        if (type.Value() == Guid{} || schemaVersion.Value() == 0) {
            return Result<Array<LoadNode>>::Failure(ErrorCode::InvalidArgument);
        }
        auto record = body.Value().ReadBytes();
        if (!record) {
            return Result<Array<LoadNode>>::Failure(record.Error());
        }
        auto added = nodes.TryEmplaceBack(allocator);
        if (!added) {
            return Result<Array<LoadNode>>::Failure(added.Error());
        }
        added.Value()->type = type.Value();
        added.Value()->version = schemaVersion.Value();
        added.Value()->source = record.Value();
    }
    if (!body.Value().AtEnd()) {
        return Result<Array<LoadNode>>::Failure(ErrorCode::InvalidArgument);
    }
    spent = budget.Spend(body.Value().WorkUsed());
    if (!spent) {
        return Result<Array<LoadNode>>::Failure(spent.Error());
    }
    return Result<Array<LoadNode>>::Success(Move(nodes));
}

Result<RecordReader> OpenRecord(Allocator& allocator, Span<const uint8> source, TypeId type,
                                uint32 version, Budget& budget) noexcept {
    auto record = RecordReader::Open(allocator, source, type, version, version, budget.Record());
    if (!record) {
        return record;
    }
    auto spent = budget.Spend(record.Value().WorkUsed());
    if (!spent) {
        return Result<RecordReader>::Failure(spent.Error());
    }
    auto fields = budget.AddFields(record.Value().FieldCount());
    if (!fields) {
        return Result<RecordReader>::Failure(fields.Error());
    }
    return record;
}

Result<RecordReader> CurrentRecord(Allocator& allocator, LoadNode& node, Budget& budget,
                                   const ObjectGraphLoadOptions& options) noexcept {
    auto record = OpenRecord(allocator, node.source, node.type, node.version, budget);
    if (!record) {
        return record;
    }
    const uint32 target = node.lease.Schema().version;
    if (node.version == target) {
        return record;
    }
    if (options.migrate == nullptr) {
        return Result<RecordReader>::Failure(ErrorCode::Unsupported);
    }
    auto spent = budget.Spend(node.source.Size());
    if (!spent) {
        return Result<RecordReader>::Failure(spent.Error());
    }
    auto migrated = options.migrate(allocator, node.type, node.version, target, node.source,
                                    options.migrationUser);
    if (!migrated) {
        return Result<RecordReader>::Failure(migrated.Error());
    }
    if (migrated.Value().Size() > budget.limits.maxBytes ||
        migrated.Value().Size() > budget.limits.maxMigrationBytes - budget.migrationBytes) {
        return Result<RecordReader>::Failure(ErrorCode::CapacityExceeded);
    }
    spent = budget.Spend(migrated.Value().Size());
    if (!spent) {
        return Result<RecordReader>::Failure(spent.Error());
    }
    budget.migrationBytes += migrated.Value().Size();
    node.migrated = Move(migrated.Value());
    // 콜백 결과는 trusted schema 값으로 간주하지 않는다. 새 envelope와 모든 TLV를 다시 검사한다.
    return OpenRecord(allocator, node.migrated.View(), node.type, target, budget);
}

Result<void> DecodeNode(Allocator& allocator, TypeRegistry& types, LoadNode& node,
                        usize objectCount, Budget& budget,
                        const ObjectGraphLoadOptions& options) noexcept {
    auto type = types.Acquire(node.type);
    if (!type) {
        return Result<void>::Failure(type.Error());
    }
    node.lease = Move(type.Value());
    // 모든 노드를 생성하기 전에 검사한다. 잘못된 파일이 일부 생성자를 실행하면 안 된다.
    if (HasFlag(node.lease.Schema().flags, TypeFlags::Abstract) ||
        node.lease.Ops().construct == nullptr) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
    auto layout = ValidateLayout(node.lease, budget);
    if (!layout) {
        return layout;
    }
    auto record = CurrentRecord(allocator, node, budget, options);
    if (!record) {
        return Result<void>::Failure(record.Error());
    }
    const auto& schema = node.lease.Schema();
    auto spent = budget.Spend(record.Value().FieldCount());
    if (!spent) {
        return spent;
    }
    // 누락 허용 파일이 큰 schema를 가리켜도 없는 필드 수만큼 decoded 값을 선할당하지 않는다.
    auto reserved = node.values.TryReserve(record.Value().FieldCount());
    if (!reserved) {
        return reserved;
    }
    for (usize index = 0; index < record.Value().FieldCount(); ++index) {
        spent = budget.Spend(1);
        if (!spent) {
            return spent;
        }
        auto field = record.Value().Next();
        if (!field) {
            return Result<void>::Failure(field.Error());
        }
        usize schemaIndex = schema.fields.Size();
        for (usize candidate = 0; candidate < schema.fields.Size(); ++candidate) {
            spent = budget.Spend(1);
            if (!spent) {
                return spent;
            }
            if (schema.fields[candidate].id == field.Value().id) {
                schemaIndex = candidate;
                break;
            }
        }
        if (schemaIndex == schema.fields.Size()) {
            continue;
        } // unknown은 해석하지 않고 skip한다.
        if (HasFlag(schema.fields[schemaIndex].flags, FieldFlags::Transient)) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
        DecodedField decoded(allocator);
        decoded.schemaIndex = schemaIndex;
        auto value = DecodeValue(allocator, schema.fields[schemaIndex], field.Value(), budget,
                                 decoded.localReference, node.lease.FieldEnum(schemaIndex));
        if (!value) {
            return Result<void>::Failure(value.Error());
        }
        if (decoded.localReference > objectCount) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        decoded.value = Move(value.Value());
        if (!node.values.TryPushBack(Move(decoded))) {
            platform::Fatal("Object graph decoded field reservation failed");
        }
    }
    usize persistentFields = 0;
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        if (!HasFlag(schema.fields[index].flags, FieldFlags::Transient)) {
            ++persistentFields;
        }
    }
    if (!options.allowMissingFields && node.values.Size() != persistentFields) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }
    return Result<void>::Success();
}

Result<void> ValidateReachability(Allocator& allocator, TypeRegistry& types,
                                  const Array<LoadNode>& nodes, Budget& budget) noexcept {
    auto spent = budget.Spend(nodes.Size());
    if (!spent) {
        return spent;
    }
    Array<uint8> visited(allocator);
    Array<usize> queue(allocator);
    auto reserved = visited.TryReserve(nodes.Size());
    if (!reserved) {
        return reserved;
    }
    reserved = queue.TryReserve(nodes.Size());
    if (!reserved) {
        return reserved;
    }
    for (usize index = 0; index < nodes.Size(); ++index) {
        if (!visited.TryPushBack(uint8{0})) {
            platform::Fatal("Object graph reachability reservation failed");
        }
    }
    visited[0] = 1;
    if (!queue.TryPushBack(usize{0})) {
        platform::Fatal("Object graph root queue reservation failed");
    }
    for (usize index = 0; index < queue.Size(); ++index) {
        const auto& node = nodes[queue[index]];
        for (usize fieldIndex = 0; fieldIndex < node.values.Size(); ++fieldIndex) {
            spent = budget.Spend(1);
            if (!spent) {
                return spent;
            }
            const auto& field = node.values[fieldIndex];
            if (field.localReference == 0) {
                continue;
            }
            const usize target = static_cast<usize>(field.localReference - 1);
            const auto expectedId = node.lease.Schema().fields[field.schemaIndex].value.type;
            if (expectedId != Guid{}) {
                auto expected = types.Acquire(expectedId);
                if (!expected) {
                    return Result<void>::Failure(expected.Error());
                }
                // setter와 같은 native 상속 계약을 생성 전에 검사한다. 선언 타입의 파생 객체와 같은
                // native의 다른 TypeId는 허용한다.
                if (!nodes[target].lease.IsNativeA(expected.Value().Ops().nativeType)) {
                    return Result<void>::Failure(ErrorCode::TypeMismatch);
                }
            }
            if (visited[target] == 0) {
                visited[target] = 1;
                if (!queue.TryPushBack(target)) {
                    platform::Fatal("Object graph discovery queue reservation failed");
                }
            }
        }
    }
    if (queue.Size() != nodes.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<void>::Success();
}

Result<RootRef<Object>> ConstructGraph(ObjectRegistry& registry, const ObjectAccessContext& access,
                                       Array<LoadNode>& nodes, Budget& budget) noexcept {
    ConstructionContext transaction(registry, access);
    for (usize index = 0; index < nodes.Size(); ++index) {
        auto spent = budget.Spend(1);
        if (!spent) {
            return Result<RootRef<Object>>::Failure(spent.Error());
        }
        auto created = transaction.Create<Object>(nodes[index].type);
        if (!created) {
            return Result<RootRef<Object>>::Failure(created.Error());
        }
        nodes[index].handle = created.Value();
    }
    for (usize index = 0; index < nodes.Size(); ++index) {
        auto& node = nodes[index];
        for (usize fieldIndex = 0; fieldIndex < node.values.Size(); ++fieldIndex) {
            auto& decoded = node.values[fieldIndex];
            const auto& schema = node.lease.Schema().fields[decoded.schemaIndex];
            auto accessor = FindOps(node.lease, schema.id, budget);
            if (!accessor) {
                return Result<RootRef<Object>>::Failure(accessor.Error());
            }
            usize work = 0;
            if (!TryMultiply(decoded.value.string.ByteCount(), 2, work) || !TryAdd(work, 1, work)) {
                return Result<RootRef<Object>>::Failure(ErrorCode::Overflow);
            }
            auto spent = budget.Spend(work);
            if (!spent) {
                return Result<RootRef<Object>>::Failure(spent.Error());
            }
            if (decoded.value.kind == ValueKind::ObjectReference) {
                decoded.value.reference =
                    decoded.localReference == 0
                        ? ObjectHandle{}
                        : nodes[static_cast<usize>(decoded.localReference - 1)].handle;
            }
            auto written = accessor.Value()->writeStaged(transaction, node.handle, decoded.value);
            if (!written) {
                return Result<RootRef<Object>>::Failure(written.Error());
            }
        }
    }
    auto spent = budget.Spend(1);
    if (!spent) {
        return Result<RootRef<Object>>::Failure(spent.Error());
    }
    return transaction.Commit();
}

} // namespace

Result<RootRef<Object>> LoadObjectGraph(Allocator& allocator, ObjectRegistry& registry,
                                        TypeRegistry& types, const ObjectAccessContext& access,
                                        Span<const uint8> source,
                                        ObjectGraphLoadOptions options) noexcept {
    if (!registry.UsesTypeRegistry(types)) {
        return Result<RootRef<Object>>::Failure(ErrorCode::WrongContext);
    }
    Budget budget(options.limits);
    auto nodes = ReadEnvelope(allocator, source, budget);
    if (!nodes) {
        return Result<RootRef<Object>>::Failure(nodes.Error());
    }
    for (usize index = 0; index < nodes.Value().Size(); ++index) {
        auto decoded = DecodeNode(allocator, types, nodes.Value()[index], nodes.Value().Size(),
                                  budget, options);
        if (!decoded) {
            return Result<RootRef<Object>>::Failure(decoded.Error());
        }
    }
    auto reachable = ValidateReachability(allocator, types, nodes.Value(), budget);
    if (!reachable) {
        return Result<RootRef<Object>>::Failure(reachable.Error());
    }
    return ConstructGraph(registry, access, nodes.Value(), budget);
}

} // namespace rocks
