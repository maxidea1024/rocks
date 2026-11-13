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
    // 노드 수만큼 먼저 예약하므로 이후 원소가 옮겨지지 않는다(FieldView가 node.migrated를
    // 가리킨다).
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

// 1단계(검증) 문맥이다. 값 reader는 그래프 공유 예산으로 열고, 강한 참조 token을 범위 검사해
// 기록하며, 진단·수집을 hook으로 보낸다. Spend는 2단계 몫까지 두 배, ReserveApply는 한 배를
// 청구한다.
class GraphValidateContext final : public FieldDecodeContext {
public:
    GraphValidateContext(Allocator& allocator, Budget& budget,
                         const ObjectGraphLoadOptions& options, usize objectCount) noexcept
        : FieldDecodeContext(options.policy, options.hooks, ReferenceWire), budget_(budget),
          objectCount_(objectCount), scratch_(allocator, budget) {
    }

    void BeginNode(LoadNode& node, const RecordReader* record, uint32 index) noexcept {
        node_ = &node;
        record_ = record;
        SetObject(node.type, index);
    }

    // 필드 하나의 decode 전에 부른다. 그 필드가 참조를 기록하면 기대 형식을 한 번만 보관한다.
    void BeginField() noexcept {
        charged_ = 0;
        expectation_ = NoExpectation;
    }

    uint64 Charged() const noexcept {
        return charged_;
    }

    Result<ArchiveReader> OpenValue(const FieldView& field) noexcept override {
        if (!record_) {
            return Result<ArchiveReader>::Failure(ErrorCode::InvalidArgument);
        }

        return record_->OpenValue(field);
    }

    Result<DecodedReference>
    ReadReference(ArchiveReader& reader,
                  const ReferenceExpectation& expectation) noexcept override {
        auto local = reader.ReadU64();
        if (!local) {
            return Result<DecodedReference>::Failure(local.Error());
        }

        if (local.Value() > objectCount_) {
            return Result<DecodedReference>::Failure(ErrorCode::InvalidArgument);
        }

        DecodedReference decoded;
        decoded.isNull = local.Value() == 0;
        if (decoded.isNull) {
            return Result<DecodedReference>::Success(decoded);
        }

        if (auto spent = Charge(1); !spent) {
            return Result<DecodedReference>::Failure(spent.Error());
        }

        // 기대 형식은 필드 안에서 같은 값이 반복되면(B5 배열 원소) 한 번만 보관하고, 다르면(B4
        // struct의 서로 다른 참조 멤버) 따로 보관한다.
        if (expectation_ == NoExpectation ||
            node_->expectations[expectation_].schemaType != expectation.schemaType ||
            node_->expectations[expectation_].staticNative != expectation.staticNative) {
            if (!node_->expectations.TryPushBack(expectation)) {
                return Result<DecodedReference>::Failure(ErrorCode::OutOfMemory);
            }

            expectation_ = static_cast<uint32>(node_->expectations.Size() - 1);
        }

        ReferenceToken token;
        token.target = static_cast<uint32>(local.Value());
        token.expectation = expectation_;
        if (!node_->tokens.TryPushBack(token)) {
            return Result<DecodedReference>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<DecodedReference>::Success(decoded);
    }

    Result<void> Spend(uint64 work) noexcept override {
        uint64 doubled = 0;
        if (!TryMultiply(work, uint64{2}, doubled)) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        return Charge(doubled);
    }

    Result<void> ReserveApply(uint64 work) noexcept override {
        return Charge(work);
    }

    Allocator& Scratch() noexcept override {
        return scratch_;
    }

private:
    static constexpr uint32 NoExpectation = ~0u;

    Result<void> Charge(uint64 work) noexcept {
        auto spent = budget_.Spend(work);
        if (spent) {
            charged_ += work;
        }

        return spent;
    }

    Budget& budget_;
    usize objectCount_;
    BudgetAllocator scratch_;
    LoadNode* node_ = nullptr;
    const RecordReader* record_ = nullptr;
    uint64 charged_ = 0;
    uint32 expectation_ = NoExpectation;
};

// 2단계(적용) 문맥이다. 그래프 예산에 접근하지 않는다: 값 reader는 같은 크기 한도·무제한 work로
// 열고, 참조 token을 만든 객체 handle로 바꾸며, 진단·수집·청구는 버린다.
class GraphApplyContext final : public FieldDecodeContext {
public:
    GraphApplyContext(Allocator& allocator, const ObjectGraphLoadOptions& options,
                      const Array<LoadNode>& nodes) noexcept
        : FieldDecodeContext(options.policy, options.hooks, ReferenceWire), allocator_(allocator),
          limits_(options.limits), nodes_(nodes) {
        SetRecording(false);
    }

    void BeginNode(const LoadNode& node, uint32 index) noexcept {
        SetObject(node.type, index);
    }

    Result<ArchiveReader> OpenValue(const FieldView& field) noexcept override {
        ArchiveLimits limits;
        limits.maxTotalBytes = limits_.maxBytes;
        limits.maxValueBytes = limits_.maxValueBytes;
        limits.maxOperations = ~uint64{0};
        limits.maxWork = ~uint64{0};
        return ArchiveReader::Create(field.payload, limits);
    }

    Result<DecodedReference> ReadReference(ArchiveReader& reader,
                                           const ReferenceExpectation&) noexcept override {
        auto local = reader.ReadU64();
        if (!local) {
            return Result<DecodedReference>::Failure(local.Error());
        }

        DecodedReference decoded;
        decoded.isNull = local.Value() == 0;
        if (!decoded.isNull) {
            // 범위는 1단계가 검사했다. 다르면 원본이 바뀐 계약 위반이다.
            if (local.Value() > nodes_.Size()) {
                return Result<DecodedReference>::Failure(ErrorCode::InvalidArgument);
            }

            decoded.handle = nodes_[static_cast<usize>(local.Value() - 1)].handle;
        }

        return Result<DecodedReference>::Success(decoded);
    }

    Result<void> Spend(uint64) noexcept override {
        return Result<void>::Success();
    }

    Result<void> ReserveApply(uint64) noexcept override {
        return Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return allocator_;
    }

private:
    Allocator& allocator_;
    ObjectGraphLimits limits_;
    const Array<LoadNode>& nodes_;
};

Result<RecordReader> OpenRecord(Allocator& allocator, Span<const uint8> source, TypeId type,
                                uint32 version, Budget& budget) noexcept {
    // Record 트리는 그래프 공유 예산을 쓰므로 그 작업량이 그래프 누계에 바로 들어간다.
    auto record = RecordReader::Open(allocator, source, type, version, version, budget.Record(),
                                     budget.Shared());
    if (!record) {
        return record;
    }

    auto fields = budget.AddFields(record.Value().FieldCount());
    if (!fields) {
        return Result<RecordReader>::Failure(fields.Error());
    }

    return record;
}

Result<RecordReader> CurrentRecord(Allocator& allocator, LoadNode& node, Budget& budget,
                                   const ObjectGraphLoadOptions& options,
                                   GraphValidateContext& context) noexcept {
    const uint32 target = node.lease.Schema().version;
    if (node.version == target) {
        return OpenRecord(allocator, node.source, node.type, node.version, budget);
    }

    // 저장 버전이 더 높으면 migrate를 부르지 않는다(AS 5.3 타입 데이터 축 — 높으면 읽기 전용 열기).
    if (node.version > target) {
        if (options.policy.newerVersion != NewerVersionPolicy::LoadNewerSkippingUnknown) {
            return Result<RecordReader>::Failure(ErrorCode::Unsupported);
        }

        auto record = OpenRecord(allocator, node.source, node.type, node.version, budget);
        if (!record) {
            return record;
        }

        if (auto reported = context.Report(FieldDiagnosticKind::NewerVersion, node.version);
            !reported) {
            return Result<RecordReader>::Failure(reported.Error());
        }

        return record;
    }

    if (options.migrate == nullptr) {
        return Result<RecordReader>::Failure(ErrorCode::Unsupported);
    }

    auto record = OpenRecord(allocator, node.source, node.type, node.version, budget);
    if (!record) {
        return record;
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

// 1단계: 노드 하나를 검증하고 필드 색인·참조 token을 보관한다. 생성자를 실행하지 않는다.
Result<void> DecodeNode(Allocator& allocator, TypeRegistry& types, LoadNode& node, uint32 index,
                        Budget& budget, const ObjectGraphLoadOptions& options,
                        GraphValidateContext& context) noexcept {
    auto type = types.Acquire(node.type);
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    node.lease = Move(type.Value());
    // 타입 기반 생성 거부는 모든 노드를 만들기 전에 판정한다.
    if (HasFlag(node.lease.Schema().flags, TypeFlags::Abstract) ||
        node.lease.Ops().construct == nullptr) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    auto layout = ValidateLayout(node.lease, budget);
    if (!layout) {
        return layout;
    }

    context.BeginNode(node, nullptr, index);
    auto record = CurrentRecord(allocator, node, budget, options, context);
    if (!record) {
        return Result<void>::Failure(record.Error());
    }

    // 이 함수가 끝나면(성공·실패) 지역 Record를 가리키지 않게 한다.
    struct RecordBinding {
        GraphValidateContext& context;
        LoadNode& node;
        uint32 index;

        ~RecordBinding() {
            context.BeginNode(node, nullptr, index);
        }
    } binding{context, node, index};
    context.BeginNode(node, &record.Value(), index);
    const auto& schema = node.lease.Schema();
    auto spent = budget.Spend(record.Value().FieldCount() + schema.fields.Size());
    if (!spent) {
        return spent;
    }

    // 누락 허용 파일이 큰 schema를 가리켜도 없는 필드 수만큼 색인을 선할당하지 않는다.
    auto reserved = node.fields.TryReserve(record.Value().FieldCount());
    if (!reserved) {
        return reserved;
    }

    Array<uint8> present(allocator);
    reserved = present.TryReserve(schema.fields.Size());
    if (!reserved) {
        return reserved;
    }

    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        if (!present.TryPushBack(uint8{0})) {
            platform::Fatal("Object graph field presence reservation failed");
        }
    }

    for (usize fieldIndex = 0; fieldIndex < record.Value().FieldCount(); ++fieldIndex) {
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
            if (options.policy.unknownField == UnknownFieldPolicy::Reject) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            if (auto reported = context.ReportUnknownField(field.Value().id); !reported) {
                return reported;
            }

            continue;
        }

        if (HasFlag(schema.fields[schemaIndex].flags, FieldFlags::Transient)) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        auto accessor = FindOps(node.lease, field.Value().id, budget);
        if (!accessor) {
            return Result<void>::Failure(accessor.Error());
        }

        FieldScope scope(context, schema.fields[schemaIndex], schema.id,
                         node.lease.FieldEnum(schemaIndex));
        context.BeginField();
        const uint64 before = budget.Used();
        auto decoded = accessor.Value()->decode(*accessor.Value(), nullptr, ObjectHandle{},
                                                field.Value(), context);
        if (!decoded) {
            return decoded;
        }

        // 2단계는 같은 reader 호출을 다시 실행하므로 1단계 reader 작업량만큼 미리 청구한다.
        const uint64 used = budget.Used() - before;
        spent = budget.Spend(used > context.Charged() ? used - context.Charged() : 0);
        if (!spent) {
            return spent;
        }

        StoredField stored;
        stored.schemaIndex = schemaIndex;
        stored.ops = accessor.Value();
        stored.view = field.Value();
        if (!node.fields.TryPushBack(stored)) {
            platform::Fatal("Object graph field index reservation failed");
        }

        present[schemaIndex] = 1;
    }

    // Record에 없는 영속 필드: 누락이 허용되지 않으면 NotFound, 허용된 Required 누락은 진단한다.
    for (usize schemaIndex = 0; schemaIndex < schema.fields.Size(); ++schemaIndex) {
        const auto& field = schema.fields[schemaIndex];
        if (present[schemaIndex] != 0 || HasFlag(field.flags, FieldFlags::Transient)) {
            continue;
        }

        FieldScope scope(context, field, schema.id, node.lease.FieldEnum(schemaIndex));
        if (!context.IsMissingAllowed(context.EffectiveFlags())) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        if (auto reported = context.ReportIfRequiredNull(true); !reported) {
            return reported;
        }
    }

    return Result<void>::Success();
}

// 강한 참조 대상 타입 검사의 캐시다: (기대 TypeId, 대상 TypeId) 결과와 기대 타입 lease(로드 끝까지
// 쥔다).
struct TypeCheck {
    TypeId expected{};
    TypeId actual{};
    bool accepted = false;
};

struct ExpectedLease {
    TypeId type{};
    TypeLease lease;
};

// leases는 검사에 쓴 기대 타입 lease를 받아 호출자가 로드 끝까지 쥔다.
Result<void> ValidateReachability(Allocator& allocator, TypeRegistry& types,
                                  const Array<LoadNode>& nodes, Budget& budget,
                                  Array<ExpectedLease>& leases) noexcept {
    auto spent = budget.Spend(nodes.Size());
    if (!spent) {
        return spent;
    }

    Array<uint8> visited(allocator);
    Array<usize> queue(allocator);
    Array<TypeCheck> checks(allocator);
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
        for (usize tokenIndex = 0; tokenIndex < node.tokens.Size(); ++tokenIndex) {
            spent = budget.Spend(1);
            if (!spent) {
                return spent;
            }

            const ReferenceToken& token = node.tokens[tokenIndex];
            const usize target = static_cast<usize>(token.target - 1);
            const ReferenceExpectation& expectation = node.expectations[token.expectation];
            // setter와 같은 native 상속 계약을 생성 전에 검사한다. 선언 타입의 파생 객체와 같은
            // native의 다른 TypeId는 허용한다. 정적 thunk의 대상 타입도 함께 만족해야 한다.
            if (expectation.staticNative &&
                !nodes[target].lease.IsNativeA(expectation.staticNative)) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }

            if (expectation.schemaType != Guid{}) {
                bool cached = false;
                bool accepted = false;
                for (usize check = 0; check < checks.Size(); ++check) {
                    spent = budget.Spend(1);
                    if (!spent) {
                        return spent;
                    }

                    if (checks[check].expected == expectation.schemaType &&
                        checks[check].actual == nodes[target].type) {
                        cached = true;
                        accepted = checks[check].accepted;
                        break;
                    }
                }

                if (!cached) {
                    const TypeLease* expected = nullptr;
                    for (usize lease = 0; lease < leases.Size(); ++lease) {
                        spent = budget.Spend(1);
                        if (!spent) {
                            return spent;
                        }

                        if (leases[lease].type == expectation.schemaType) {
                            expected = &leases[lease].lease;
                            break;
                        }
                    }

                    if (!expected) {
                        // 조회와 마찬가지로 lease 획득·삽입도 청구한다.
                        spent = budget.Spend(1);
                        if (!spent) {
                            return spent;
                        }

                        auto acquired = types.Acquire(expectation.schemaType);
                        if (!acquired) {
                            return Result<void>::Failure(acquired.Error());
                        }

                        auto added = leases.TryEmplaceBack();
                        if (!added) {
                            return Result<void>::Failure(added.Error());
                        }

                        added.Value()->type = expectation.schemaType;
                        added.Value()->lease = Move(acquired.Value());
                        expected = &added.Value()->lease;
                    }

                    accepted = nodes[target].lease.IsNativeA(expected->Ops().nativeType);
                    spent = budget.Spend(1);
                    if (!spent) {
                        return spent;
                    }

                    TypeCheck entry;
                    entry.expected = expectation.schemaType;
                    entry.actual = nodes[target].type;
                    entry.accepted = accepted;
                    if (!checks.TryPushBack(entry)) {
                        return Result<void>::Failure(ErrorCode::OutOfMemory);
                    }
                }

                if (!accepted) {
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

// 2단계: 모든 객체를 만들고 보관한 필드를 staged 객체에 쓴 뒤 Commit한다. 그래프 예산에 접근하지
// 않는다.
Result<RootRef<Object>> ConstructGraph(Allocator& allocator, ObjectRegistry& registry,
                                       const ObjectAccessContext& access, Array<LoadNode>& nodes,
                                       const ObjectGraphLoadOptions& options) noexcept {
    ConstructionContext transaction(registry, access);
    for (usize index = 0; index < nodes.Size(); ++index) {
        auto created = transaction.Create<Object>(nodes[index].type);
        if (!created) {
            return Result<RootRef<Object>>::Failure(created.Error());
        }

        nodes[index].handle = created.Value();
    }

    GraphApplyContext context(allocator, options, nodes);
    for (usize index = 0; index < nodes.Size(); ++index) {
        const auto& node = nodes[index];
        const auto& schema = node.lease.Schema();
        context.BeginNode(node, static_cast<uint32>(index));
        for (usize fieldIndex = 0; fieldIndex < node.fields.Size(); ++fieldIndex) {
            const StoredField& stored = node.fields[fieldIndex];
            FieldScope scope(context, schema.fields[stored.schemaIndex], schema.id,
                             node.lease.FieldEnum(stored.schemaIndex));
            auto written =
                stored.ops->decode(*stored.ops, &transaction, node.handle, stored.view, context);
            if (!written) {
                return Result<RootRef<Object>>::Failure(written.Error());
            }
        }
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

    // 값을 바꾸거나 모르는 내용을 건너뛰는 정책은 진단을 받을 곳이 있어야 한다.
    if (!options.hooks &&
        (options.policy.undeclaredEnum == UndeclaredEnumPolicy::ReportAndDefault ||
         options.policy.newerVersion == NewerVersionPolicy::LoadNewerSkippingUnknown)) {
        return Result<RootRef<Object>>::Failure(ErrorCode::InvalidArgument);
    }

    auto budget = Budget::Create(allocator, options.limits);
    if (!budget) {
        return Result<RootRef<Object>>::Failure(budget.Error());
    }

    auto nodes = ReadEnvelope(allocator, source, budget.Value());
    if (!nodes) {
        return Result<RootRef<Object>>::Failure(nodes.Error());
    }

    GraphValidateContext context(allocator, budget.Value(), options, nodes.Value().Size());
    usize references = 0;
    for (usize index = 0; index < nodes.Value().Size(); ++index) {
        auto decoded = DecodeNode(allocator, types, nodes.Value()[index],
                                  static_cast<uint32>(index), budget.Value(), options, context);
        if (!decoded) {
            return Result<RootRef<Object>>::Failure(decoded.Error());
        }

        references += nodes.Value()[index].tokens.Size();
    }

    Array<ExpectedLease> expectedLeases(allocator);
    auto reachable =
        ValidateReachability(allocator, types, nodes.Value(), budget.Value(), expectedLeases);
    if (!reachable) {
        return Result<RootRef<Object>>::Failure(reachable.Error());
    }

    // 2단계의 노드 생성·Commit·staged 간선 검증 몫을 미리 청구한다(2단계는 예산으로 실패하지
    // 않는다).
    auto spent = budget.Value().Spend(static_cast<uint64>(nodes.Value().Size()) + 1 + references);
    if (!spent) {
        return Result<RootRef<Object>>::Failure(spent.Error());
    }

    return ConstructGraph(allocator, registry, access, nodes.Value(), options);
}

} // namespace rocks
