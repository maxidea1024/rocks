#include "GraphInternal.h"
#include <Core/Algorithms.h>

namespace rocks {
namespace {

using namespace graph_detail;

// graph v2 envelope를 읽은 결과다. 노드 표(NodeTable)가 없으면 노드의 type·outer·name은 비어 있다.
struct Envelope {
    explicit Envelope(Allocator& allocator) noexcept : nodes(allocator) {
    }

    uint32 flags = 0;
    uint32 externalCount = 0;
    Array<LoadNode> nodes;
};

// header·NodeTable·Record 경계를 읽는다(Docs/Design/ObjectFoundations.ko.md 5.1절). 순서와 오류:
// magic InvalidArgument → formatVersion이 2가 아니면 Unsupported(v1은 읽지 않는다) → 정의되지 않은
// flags·NodeTable 없는 BaseOuter InvalidArgument → nodeCount 0 InvalidArgument·maxObjects 초과
// CapacityExceeded → externalCount가 maxExternalReferences 초과 CapacityExceeded → 노드 수에 비해
// 짧은 입력 InvalidArgument(할당 전) → NodeTable 항목(타입 0 InvalidArgument)·Record 길이·남는
// byte.
Result<Envelope> ReadEnvelope(Allocator& allocator, Span<const uint8> source,
                              Budget& budget) noexcept {
    auto envelope = ArchiveReader::Create(source, budget.Archive(true));
    if (!envelope) {
        return Result<Envelope>::Failure(envelope.Error());
    }

    auto magic = envelope.Value().ReadRaw(sizeof(Magic));
    if (!magic) {
        return Result<Envelope>::Failure(magic.Error());
    }

    for (usize index = 0; index < sizeof(Magic); ++index) {
        if (magic.Value()[index] != Magic[index]) {
            return Result<Envelope>::Failure(ErrorCode::InvalidArgument);
        }
    }

    auto version = envelope.Value().ReadU32();
    if (!version) {
        return Result<Envelope>::Failure(version.Error());
    }

    if (version.Value() != FormatVersion) {
        return Result<Envelope>::Failure(ErrorCode::Unsupported);
    }

    auto flags = envelope.Value().ReadU32();
    if (!flags) {
        return Result<Envelope>::Failure(flags.Error());
    }

    const bool nodeTable = (flags.Value() & NodeTableFlag) != 0;
    if ((flags.Value() & ~KnownFlags) != 0 ||
        ((flags.Value() & BaseOuterFlag) != 0 && !nodeTable)) {
        return Result<Envelope>::Failure(ErrorCode::InvalidArgument);
    }

    auto count = envelope.Value().ReadU32();
    if (!count) {
        return Result<Envelope>::Failure(count.Error());
    }

    if (count.Value() == 0) {
        return Result<Envelope>::Failure(ErrorCode::InvalidArgument);
    }

    if (count.Value() > budget.limits.maxObjects) {
        return Result<Envelope>::Failure(ErrorCode::CapacityExceeded);
    }

    auto externals = envelope.Value().ReadU32();
    if (!externals) {
        return Result<Envelope>::Failure(externals.Error());
    }

    if (externals.Value() > budget.limits.maxExternalReferences) {
        return Result<Envelope>::Failure(ErrorCode::CapacityExceeded);
    }

    // 노드마다 최소 byte(Record 길이 prefix와 header, NodeTable이면 항목)를 할당 전에 확인한다.
    const usize perNode = RecordPrefixBytes + RecordHeaderBytes + (nodeTable ? NodeEntryBytes : 0);
    if (static_cast<uint64>(count.Value()) * perNode > envelope.Value().Remaining()) {
        return Result<Envelope>::Failure(ErrorCode::InvalidArgument);
    }

    auto spent = budget.Spend(envelope.Value().WorkUsed());
    if (!spent) {
        return Result<Envelope>::Failure(spent.Error());
    }

    spent = budget.Spend(count.Value());
    if (!spent) {
        return Result<Envelope>::Failure(spent.Error());
    }

    Envelope result(allocator);
    result.flags = flags.Value();
    result.externalCount = externals.Value();
    // 노드 수만큼 먼저 예약하므로 이후 원소가 옮겨지지 않는다(FieldView가 node.migrated를
    // 가리킨다).
    auto reserved = result.nodes.TryReserve(count.Value());
    if (!reserved) {
        return Result<Envelope>::Failure(reserved.Error());
    }

    for (uint32 index = 0; index < count.Value(); ++index) {
        if (!result.nodes.TryEmplaceBack(allocator)) {
            platform::Fatal("Object graph node reservation failed");
        }
    }

    // 예약/초기화 비용을 먼저 차감한 나머지 예산만 body cursor에 준다.
    auto body = ArchiveReader::Create({source.Data() + HeaderBytes, source.Size() - HeaderBytes},
                                      budget.Archive(true));
    if (!body) {
        return Result<Envelope>::Failure(body.Error());
    }

    for (usize index = 0; nodeTable && index < result.nodes.Size(); ++index) {
        auto type = body.Value().ReadGuid();
        if (!type) {
            return Result<Envelope>::Failure(type.Error());
        }

        if (type.Value() == Guid{}) {
            return Result<Envelope>::Failure(ErrorCode::InvalidArgument);
        }

        auto outer = body.Value().ReadU32();
        if (!outer) {
            return Result<Envelope>::Failure(outer.Error());
        }

        auto name = body.Value().ReadStringView();
        if (!name) {
            return Result<Envelope>::Failure(name.Error());
        }

        result.nodes[index].type = type.Value();
        result.nodes[index].outer = outer.Value();
        result.nodes[index].name = name.Value();
    }

    for (usize index = 0; index < result.nodes.Size(); ++index) {
        auto record = body.Value().ReadBytes();
        if (!record) {
            return Result<Envelope>::Failure(record.Error());
        }

        result.nodes[index].source = record.Value();
    }

    if (!body.Value().AtEnd()) {
        return Result<Envelope>::Failure(ErrorCode::InvalidArgument);
    }

    spent = budget.Spend(body.Value().WorkUsed());
    if (!spent) {
        return Result<Envelope>::Failure(spent.Error());
    }

    return Result<Envelope>::Success(Move(result));
}

// 1단계(검증) 문맥이다. 값 reader는 그래프 공유 예산으로 열고, 강한 참조 token을 범위 검사해
// 기록하며, 진단·수집을 hook으로 보낸다. Spend는 2단계 몫까지 두 배, ReserveApply는 한 배를
// 청구한다. 관찰 할당(ObservationScratch)은 1단계에만 있는 일이라 Charge로 한 번만 청구한다.
class GraphValidateContext final : public FieldDecodeContext {
public:
    GraphValidateContext(Allocator& allocator, Budget& budget,
                         const ObjectGraphLoadOptions& options, usize objectCount) noexcept
        : FieldDecodeContext(options.policy, options.hooks, ReferenceWire), allocator_(allocator),
          budget_(budget), options_(options), objectCount_(objectCount),
          scratch_(allocator, budget), observation_(allocator, *this) {
    }

    // 관찰할 struct ID(정렬됨, 로드 동안 유효)를 정한다.
    void Observe(Span<const TypeId> sorted) noexcept {
        SetObservedStructs(sorted);
    }

    void BeginNode(LoadNode& node, const RecordReader* record, uint32 index) noexcept {
        node_ = &node;
        record_ = record;
        SetObject(node.type, index);
    }

    // 필드 하나의 decode 전에 부른다. 그 필드가 참조를 기록하면 서로 다른 기대 형식마다 한 번만
    // 보관한다(필드의 기대 시작 위치를 기록해 [시작, 끝)에서 재사용한다).
    void BeginField() noexcept {
        charged_ = 0;
        expectationStart_ = node_ ? node_->expectations.Size() : 0;
    }

    uint64 Charged() const noexcept {
        return charged_;
    }

    Result<RecordScope> OpenRecord(const FieldView& field, TypeId expected, uint32 minVersion,
                                   uint32 maxVersion) noexcept override {
        if (field.wireType != static_cast<uint32>(WireType::Record)) {
            return Result<RecordScope>::Failure(ErrorCode::TypeMismatch);
        }

        const RecordReader* parent = CurrentRecord() ? CurrentRecord() : record_;
        if (!parent) {
            return Result<RecordScope>::Failure(ErrorCode::InvalidArgument);
        }

        // 최상위 struct는 2단계가 독립 reader로 다시 열며 payload 바이트를 청구하므로 그만큼 미리
        // 청구한다(1단계는 객체 Record가 이미 청구했다).
        if (!CurrentRecord()) {
            if (auto spent = Charge(field.payload.Size()); !spent) {
                return Result<RecordScope>::Failure(spent.Error());
            }
        }

        // 공유 예산으로 열고(header·색인만 청구 — 그 작업은 DecodeNode가 2단계 몫까지 청구한다)
        // 필드 수 한도는 남은 그래프 누계다.
        auto opened = parent->OpenRecordField(allocator_, field, expected, minVersion, maxVersion,
                                              budget_.RemainingFields());
        if (!opened) {
            return Result<RecordScope>::Failure(opened.Error());
        }

        if (auto counted = budget_.AddFields(opened.Value().FieldCount()); !counted) {
            return Result<RecordScope>::Failure(counted.Error());
        }

        return PushRecord(Move(opened.Value()));
    }

    Result<RecordScope> OpenMigratedRecord(Span<const uint8> record, TypeId expected,
                                           uint32 version) noexcept override {
        // 콜백 결과는 신뢰하지 않는다. 정확히 현재 버전의 envelope와 모든 TLV를 다시 검사한다.
        auto opened = RecordReader::Open(allocator_, record, expected, version, version,
                                         budget_.Record(), budget_.Shared());
        if (!opened) {
            return Result<RecordScope>::Failure(opened.Error());
        }

        if (auto counted = budget_.AddFields(opened.Value().FieldCount()); !counted) {
            return Result<RecordScope>::Failure(counted.Error());
        }

        return PushRecord(Move(opened.Value()));
    }

    Result<RecordListReader> OpenRecordList(const FieldView& field,
                                            TypeId itemSchema) noexcept override {
        if (field.wireType != static_cast<uint32>(WireType::RecordList)) {
            return Result<RecordListReader>::Failure(ErrorCode::TypeMismatch);
        }

        const RecordReader* parent = CurrentRecord() ? CurrentRecord() : record_;
        if (!parent) {
            return Result<RecordListReader>::Failure(ErrorCode::InvalidArgument);
        }

        // 최상위 목록은 2단계가 독립 reader로 다시 열며 항목 byte를 청구하므로 그만큼 미리
        // 청구한다(1단계는 객체 Record가 이미 청구했다). 항목은 색인 전용이다 — 그 작업은
        // DecodeNode의 reader 작업 Δ 2배가 덮는다.
        if (!CurrentRecord()) {
            if (auto spent = Charge(field.payload.Size()); !spent) {
                return Result<RecordListReader>::Failure(spent.Error());
            }
        }

        // 항목 수 한도는 남은 원소 누계로도 건다 — 색인을 할당하기 전에 AddElements와 같은
        // CapacityExceeded가 나게(2단계는 1단계가 판정했으므로 MaxArrayElements 그대로다).
        RecordListLimits limits = ArrayLoadLimits(budget_.limits);
        const uint32 remaining = budget_.RemainingElements();
        limits.maxItems = remaining < limits.maxItems ? remaining : limits.maxItems;
        return RecordListReader::OpenField(*parent, field, itemSchema, 1,
                                           NumericLimits<uint32>::Max(), limits);
    }

    Result<RecordScope> OpenListItem(RecordListReader& list, usize index) noexcept override {
        // 필드 수 한도는 남은 그래프 누계다(큰 항목이 누계 검사 전에 색인을 할당하지 않게).
        auto opened = list.OpenItem(index, budget_.RemainingFields());
        if (!opened) {
            return Result<RecordScope>::Failure(opened.Error());
        }

        if (auto counted = budget_.AddFields(opened.Value().FieldCount()); !counted) {
            return Result<RecordScope>::Failure(counted.Error());
        }

        return PushRecord(Move(opened.Value()));
    }

    Result<Span<const uint8>> MigrateStruct(TypeId type, uint32 fromVersion, uint32 toVersion,
                                            Span<const uint8> record) noexcept override {
        const FieldCodecFrame* frame = Current();
        if (!node_ || !frame || frame->structure.IsEmpty()) {
            return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
        }

        // 원본 byte를 먼저 청구한다(소진된 예산으로 콜백을 실행하지 않는다 — 객체 경로와 같다).
        if (auto spent = Charge(record.Size()); !spent) {
            return Result<Span<const uint8>>::Failure(spent.Error());
        }

        // 로드 옵션 override를 먼저 묻고(Unsupported면 다음), 타입이 등록한 마이그레이션을 쓴다.
        Result<Array<uint8>> migrated = Result<Array<uint8>>::Failure(ErrorCode::Unsupported);
        if (options_.migrateStruct) {
            migrated = options_.migrateStruct(allocator_, type, fromVersion, toVersion, record,
                                              options_.migrationUser);
        }

        const StructMigration own = frame->structure.StructOps().migrate;
        if (!migrated && migrated.Error() == ErrorCode::Unsupported && own) {
            migrated = own(allocator_, type, fromVersion, toVersion, record);
        }

        if (!migrated) {
            return Result<Span<const uint8>>::Failure(migrated.Error());
        }

        const usize bytes = migrated.Value().Size();
        if (bytes > budget_.limits.maxBytes ||
            bytes > budget_.limits.maxMigrationBytes - budget_.migrationBytes) {
            return Result<Span<const uint8>>::Failure(ErrorCode::CapacityExceeded);
        }

        // 결과 byte를 이 문맥의 청구로 센다(reader 작업 Δ 청구와 이중이 되지 않게).
        if (auto spent = Charge(bytes); !spent) {
            return Result<Span<const uint8>>::Failure(spent.Error());
        }

        StoredStructMigration stored(allocator_);
        stored.type = type;
        stored.from = fromVersion;
        stored.bytes = Move(migrated.Value());
        if (!node_->structMigrations.TryPushBack(Move(stored))) {
            return Result<Span<const uint8>>::Failure(ErrorCode::OutOfMemory);
        }

        budget_.migrationBytes += bytes;
        return Result<Span<const uint8>>::Success(
            node_->structMigrations[node_->structMigrations.Size() - 1].bytes.View());
    }

    Result<void> AddFields(usize count) noexcept override {
        return budget_.AddFields(count);
    }

    Result<void> AddElements(usize count) noexcept override {
        return budget_.AddElements(count);
    }

    Result<DecodedReference>
    ReadReference(ArchiveReader& reader,
                  const ReferenceExpectation& expectation) noexcept override {
        auto local = reader.ReadU32();
        if (!local) {
            return Result<DecodedReference>::Failure(local.Error());
        }

        // 외부 참조(ExternalReferenceBit | i)는 i ≤ externalCount여야 하는데 이 복원은
        // externalCount 0만 받으므로(외부 참조 대상은 A0-Oe) 외부 비트는 모두 범위 밖이다. 노드
        // 번호는 nodeCount 이하다.
        if ((local.Value() & ExternalReferenceBit) != 0 || local.Value() > objectCount_) {
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

        // 기대 형식은 이 필드가 보관한 것([시작, 끝))에서 같은 값을 선형으로 찾아 재사용하고(비교
        // 수를 청구한다), 없으면 더한다. 한 필드 안 서로 다른 기대의 수는 스키마(참조 멤버 종류
        // 수)가 정하므로 배열 원소마다 불어나지 않는다.
        uint32 found = NoExpectation;
        for (usize index = expectationStart_; index < node_->expectations.Size(); ++index) {
            if (auto spent = Charge(1); !spent) {
                return Result<DecodedReference>::Failure(spent.Error());
            }

            if (node_->expectations[index].schemaType == expectation.schemaType &&
                node_->expectations[index].staticNative == expectation.staticNative) {
                found = static_cast<uint32>(index);
                break;
            }
        }

        if (found == NoExpectation) {
            if (!node_->expectations.TryPushBack(expectation)) {
                return Result<DecodedReference>::Failure(ErrorCode::OutOfMemory);
            }

            found = static_cast<uint32>(node_->expectations.Size() - 1);
        }

        ReferenceToken token;
        token.target = static_cast<uint32>(local.Value());
        token.expectation = found;
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

    Allocator& ObservationScratch() noexcept override {
        return observation_;
    }

protected:
    Result<ArchiveReader> OpenTopLevelValue(const FieldView& field) noexcept override {
        if (!record_) {
            return Result<ArchiveReader>::Failure(ErrorCode::InvalidArgument);
        }

        return record_->OpenValue(field);
    }

private:
    static constexpr uint32 NoExpectation = NumericLimits<uint32>::Max();

    // 관찰 임시 값(관찰 struct 안 배열의 원소 버퍼 포함)의 할당기다. 단일 할당 한도 없이 바이트를
    // 이 문맥의 Charge로 청구한다 — 2단계는 관찰하지 않으므로 DecodeNode의 reader 작업 Δ 2배
    // 청구(charged_를 뺀 나머지)에 들어가지 않고 한 번만 센다.
    class ObservationAllocator final : public Allocator {
    public:
        ObservationAllocator(Allocator& allocator, GraphValidateContext& context) noexcept
            : allocator_(allocator), context_(context) {
        }

        Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
            if (auto charged = context_.Charge(bytes); !charged) {
                return Result<void*>::Failure(charged.Error());
            }

            return allocator_.Allocate(bytes, alignment, tag);
        }

        Result<void> Deallocate(void* value) noexcept override {
            return allocator_.Deallocate(value);
        }

    private:
        Allocator& allocator_;
        GraphValidateContext& context_;
    };

    Result<void> Charge(uint64 work) noexcept {
        auto spent = budget_.Spend(work);
        if (spent) {
            charged_ += work;
        }

        return spent;
    }

    Allocator& allocator_;
    Budget& budget_;
    const ObjectGraphLoadOptions& options_;
    usize objectCount_;
    BudgetAllocator scratch_;
    ObservationAllocator observation_;
    LoadNode* node_ = nullptr;
    const RecordReader* record_ = nullptr;
    uint64 charged_ = 0;
    usize expectationStart_ = 0;
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
        node_ = &node;
        migration_ = 0;
    }

    // 노드의 모든 필드를 적용한 뒤 부른다. 1단계가 보관한 struct 마이그레이션은 모두 소비되어야
    // 한다(남으면 두 단계의 방문 순서가 어긋난 계약 위반).
    Result<void> EndNode() noexcept {
        if (node_ && migration_ != node_->structMigrations.Size()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<void>::Success();
    }

    Result<RecordScope> OpenRecord(const FieldView& field, TypeId expected, uint32 minVersion,
                                   uint32 maxVersion) noexcept override {
        if (field.wireType != static_cast<uint32>(WireType::Record)) {
            return Result<RecordScope>::Failure(ErrorCode::TypeMismatch);
        }

        // 1단계와 같은 크기 한도·버전 범위, 무제한 operation·work, 필드 수는 그래프 누계 한도.
        if (RecordReader* parent = CurrentRecord()) {
            auto opened = parent->OpenRecordField(allocator_, field, expected, minVersion,
                                                  maxVersion, limits_.maxTotalFields);
            if (!opened) {
                return Result<RecordScope>::Failure(opened.Error());
            }

            return PushRecord(Move(opened.Value()));
        }

        auto opened = RecordReader::Open(allocator_, field.payload, expected, minVersion,
                                         maxVersion, {Unlimited(), limits_.maxTotalFields});
        if (!opened) {
            return Result<RecordScope>::Failure(opened.Error());
        }

        return PushRecord(Move(opened.Value()));
    }

    Result<RecordScope> OpenMigratedRecord(Span<const uint8> record, TypeId expected,
                                           uint32 version) noexcept override {
        auto opened = RecordReader::Open(allocator_, record, expected, version, version,
                                         {Unlimited(), limits_.maxTotalFields});
        if (!opened) {
            return Result<RecordScope>::Failure(opened.Error());
        }

        return PushRecord(Move(opened.Value()));
    }

    Result<RecordListReader> OpenRecordList(const FieldView& field,
                                            TypeId itemSchema) noexcept override {
        if (field.wireType != static_cast<uint32>(WireType::RecordList)) {
            return Result<RecordListReader>::Failure(ErrorCode::TypeMismatch);
        }

        // 1단계와 같은 목록 한도·항목 버전 범위, 무제한 operation·work. 스택이 비면 단독 Open(항목
        // Record 크기 한도는 1단계 객체 Record와 같다), 있으면 맨 위 Record의 OpenField다.
        if (RecordReader* parent = CurrentRecord()) {
            return RecordListReader::OpenField(*parent, field, itemSchema, 1,
                                               NumericLimits<uint32>::Max(),
                                               ArrayLoadLimits(limits_));
        }

        return RecordListReader::Open(
            allocator_, field.payload, itemSchema, 1, NumericLimits<uint32>::Max(),
            {Unlimited(), limits_.maxTotalFields}, ArrayLoadLimits(limits_));
    }

    Result<RecordScope> OpenListItem(RecordListReader& list, usize index) noexcept override {
        auto opened = list.OpenItem(index, limits_.maxTotalFields);
        if (!opened) {
            return Result<RecordScope>::Failure(opened.Error());
        }

        return PushRecord(Move(opened.Value()));
    }

    Result<Span<const uint8>> MigrateStruct(TypeId type, uint32 fromVersion, uint32,
                                            Span<const uint8>) noexcept override {
        if (!node_ || migration_ >= node_->structMigrations.Size()) {
            return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
        }

        const StoredStructMigration& stored = node_->structMigrations[migration_];
        if (stored.type != type || stored.from != fromVersion) {
            return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
        }

        ++migration_;
        return Result<Span<const uint8>>::Success(stored.bytes.View());
    }

    Result<void> AddFields(usize) noexcept override {
        return Result<void>::Success();
    }

    Result<void> AddElements(usize) noexcept override {
        return Result<void>::Success();
    }

    Result<DecodedReference> ReadReference(ArchiveReader& reader,
                                           const ReferenceExpectation&) noexcept override {
        auto local = reader.ReadU32();
        if (!local) {
            return Result<DecodedReference>::Failure(local.Error());
        }

        DecodedReference decoded;
        decoded.isNull = local.Value() == 0;
        if (!decoded.isNull) {
            // 범위는 1단계가 검사했다(외부 비트 포함). 다르면 원본이 바뀐 계약 위반이다.
            if (local.Value() > nodes_.Size()) {
                return Result<DecodedReference>::Failure(ErrorCode::InvalidArgument);
            }

            decoded.handle = nodes_[local.Value() - 1].handle;
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

    // 2단계는 관찰하지 않는다(계약상 부르지 않는다).
    Allocator& ObservationScratch() noexcept override {
        return allocator_;
    }

protected:
    Result<ArchiveReader> OpenTopLevelValue(const FieldView& field) noexcept override {
        return ArchiveReader::Create(field.payload, Unlimited());
    }

private:
    ArchiveLimits Unlimited() const noexcept {
        ArchiveLimits limits;
        limits.maxTotalBytes = limits_.maxBytes;
        limits.maxValueBytes = limits_.maxValueBytes;
        limits.maxOperations = NumericLimits<uint64>::Max();
        limits.maxWork = NumericLimits<uint64>::Max();
        return limits;
    }

    Allocator& allocator_;
    ObjectGraphLimits limits_;
    const Array<LoadNode>& nodes_;
    const LoadNode* node_ = nullptr;
    usize migration_ = 0;
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

// 노드의 저장 Record를 연다(Record 버전이 저장 당시 스키마 버전이다 — graph v2 envelope는 버전을
// 따로 쓰지 않는다). Record의 schema가 노드 타입과 다르면 InvalidArgument다(파일 손상 —
// Docs/Design/ObjectFoundations.ko.md 5.1절. Record 계층은 TypeMismatch로 알린다). 버전은
// node.version에 둔다.
Result<RecordReader> OpenStoredRecord(Allocator& allocator, LoadNode& node,
                                      Budget& budget) noexcept {
    auto record =
        RecordReader::Open(allocator, node.source, node.type, 1, NumericLimits<uint32>::Max(),
                           budget.Record(), budget.Shared());
    if (!record) {
        return Result<RecordReader>::Failure(record.Error() == ErrorCode::TypeMismatch
                                                 ? ErrorCode::InvalidArgument
                                                 : record.Error());
    }

    auto fields = budget.AddFields(record.Value().FieldCount());
    if (!fields) {
        return Result<RecordReader>::Failure(fields.Error());
    }

    node.version = record.Value().SchemaVersion();
    return record;
}

Result<RecordReader> CurrentRecord(Allocator& allocator, LoadNode& node, Budget& budget,
                                   const ObjectGraphLoadOptions& options,
                                   GraphValidateContext& context) noexcept {
    auto record = OpenStoredRecord(allocator, node, budget);
    if (!record) {
        return record;
    }

    const uint32 target = node.lease.Schema().version;
    if (node.version == target) {
        return record;
    }

    // 저장 버전이 더 높으면 migrate를 부르지 않는다(AS 5.3 타입 데이터 축 — 높으면 읽기 전용 열기).
    if (node.version > target) {
        if (options.policy.newerVersion != NewerVersionPolicy::LoadNewerSkippingUnknown) {
            return Result<RecordReader>::Failure(ErrorCode::Unsupported);
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

// 1단계: 노드 하나를 검증하고 필드 색인·참조 token을 보관한다. 생성자를 실행하지 않는다. Record에
// 없는 영속 필드는 오류가 아니며 생성자 값(기준)을 유지한다(누락 = 기준 — Docs/Design/
// ObjectFoundations.ko.md 4절). 파일에 있는 필드마다 검증 뒤 MarkPresent를 한 번 부른다(A5 override
// 집합 자리 — detail::DecodeRecordField). Required 진단은 여기서 하지 않는다(적용 뒤
// CheckRequiredFields).
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

    // 노드 표의 outer는 0(그래프 밖) 또는 자기보다 앞의 노드(1..자기 번호 − 1)다 — index는 0부터라
    // outer ≤ index다. 노드 표의 이름·정책 규칙은 생성(2단계 Create)이 판정한다(생성 전 판정은
    // 단계형 복원 A0-Od2).
    if (node.outer > index) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
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
    // Record 필드 순회 몫이다(빠진 필드를 도는 일은 없다 — 누락 = 기준).
    auto spent = budget.Spend(record.Value().FieldCount());
    if (!spent) {
        return spent;
    }

    // 빠진 필드가 많은 파일이 큰 schema를 가리켜도, 모르는 필드가 많아도 필요 이상으로 선할당하지
    // 않는다.
    const usize stored = record.Value().FieldCount() < schema.fields.Size()
                             ? record.Value().FieldCount()
                             : schema.fields.Size();
    auto reserved = node.fields.TryReserve(stored);
    if (!reserved) {
        return reserved;
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
            if (auto skipped = detail::SkipUnknownField(context, field.Value().id); !skipped) {
                return skipped;
            }

            continue;
        }

        if (HasFlag(schema.fields[schemaIndex].flags, FieldFlags::Transient)) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        // 이 데이터에 없어야 하는 플래그(쿠킹 로드의 EditorOnly)의 필드가 있으면 거부한다(최상위
        // 필드의 유효 플래그는 필드 플래그다 — struct 안 멤버는 DecodeStructRecord가 본다).
        if (context.IsExcluded(schema.fields[schemaIndex].flags)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto accessor = FindOps(node.lease, field.Value().id, budget);
        if (!accessor) {
            return Result<void>::Failure(accessor.Error());
        }

        FieldScope scope(context, schema.fields[schemaIndex], schema.id,
                         node.lease.FieldEnum(schemaIndex), node.lease.FieldStruct(schemaIndex));
        context.BeginField();
        const uint64 before = budget.Used();
        // 해석과 present 판정(override 집합 자리 — 지금은 무동작)을 한 곳에서 한다.
        auto decoded = detail::DecodeRecordField(*accessor.Value(), nullptr, ObjectHandle{},
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

        StoredField kept;
        kept.schemaIndex = schemaIndex;
        kept.ops = accessor.Value();
        kept.view = field.Value();
        if (!node.fields.TryPushBack(kept)) {
            // 모르는 필드를 뺀 수만큼 예약했으므로 실패하지 않는다.
            platform::Fatal("Object graph field index reservation failed");
        }
    }

    return Result<void>::Success();
}

// 강한 참조 대상 타입 검사 캐시의 키다: (기대 TypeId, 대상 TypeId).
struct TypePair {
    TypeId expected{};
    TypeId actual{};

    bool operator==(const TypePair&) const noexcept = default;
};

struct TypePairHash {
    uint64 operator()(const TypePair& pair) const noexcept {
        return MixHash(Hash<Guid>{}(pair.expected) ^ (Hash<Guid>{}(pair.actual) >> 1));
    }
};

// 모든 노드의 강한 참조 token이 대상 노드의 타입 기대를 만족하는지 검사한다(도달 여부와 무관 —
// setter와 같은 native 상속 계약을 생성 전에). leases는 검사에 쓴 기대 타입 lease(기대 TypeId →
// lease)를 받아 호출자가 로드 끝까지 쥔다. (기대, 실제) 결과 캐시와 기대 lease 목록은 해시 표이고
// 조회·삽입마다 작업량 1을 청구한다(원소 참조가 많아도 대상 타입 수에 비례하지 않는다).
Result<void> ValidateReferenceTypes(Allocator& allocator, TypeRegistry& types,
                                    const Array<LoadNode>& nodes, Budget& budget,
                                    HashMap<TypeId, TypeLease>& leases) noexcept {
    HashMap<TypePair, bool, TypePairHash> checks(allocator);
    for (const LoadNode& node : nodes) {
        for (usize tokenIndex = 0; tokenIndex < node.tokens.Size(); ++tokenIndex) {
            auto spent = budget.Spend(1);
            if (!spent) {
                return spent;
            }

            const ReferenceToken& token = node.tokens[tokenIndex];
            const LoadNode& target = nodes[token.target - 1];
            const ReferenceExpectation& expectation = node.expectations[token.expectation];
            // 선언 타입의 파생 객체와 같은 native의 다른 TypeId는 허용한다. 정적 thunk의 대상
            // 타입도 함께 만족해야 한다.
            if (expectation.staticNative && !target.lease.IsNativeA(expectation.staticNative)) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }

            if (expectation.schemaType == Guid{}) {
                continue;
            }

            spent = budget.Spend(1);
            if (!spent) {
                return spent;
            }

            const TypePair key{expectation.schemaType, target.type};
            bool accepted = false;
            if (const bool* cached = checks.Find(key)) {
                accepted = *cached;
            } else {
                spent = budget.Spend(1);
                if (!spent) {
                    return spent;
                }

                const TypeLease* expected = leases.Find(expectation.schemaType);
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

                    auto added = leases.Insert(expectation.schemaType, Move(acquired.Value()));
                    if (!added) {
                        return Result<void>::Failure(added.Error());
                    }

                    expected = leases.Find(expectation.schemaType);
                }

                accepted = target.lease.IsNativeA(expected->Ops().nativeType);
                spent = budget.Spend(1);
                if (!spent) {
                    return spent;
                }

                if (auto added = checks.Insert(key, accepted); !added) {
                    return Result<void>::Failure(added.Error());
                }
            }

            if (!accepted) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }
        }
    }

    return Result<void>::Success();
}

// 편의 복원의 도달 검사다(Docs/Design/ObjectFoundations.ko.md 5.5절): 노드 1에서 영속 강한 참조,
// inner → outer(노드 표의 outer), 패키지 정책 Registry면 outer → inner와 기준 Outer 아래 보존(기준
// Outer가 있으면 outer 0 노드는 기준 Outer가 붙잡는다 — 1.7절)으로 닿지 않는 노드가 있으면
// InvalidArgument다(공개 뒤 다음 GC가 조용히 지우지 않게 — 생성 전에 판정한다). 패키지 정책이고
// 기준 Outer가 없으면 노드 1이 아닌 outer 0 노드(다른 패키지)도 InvalidArgument다.
Result<void> CheckReachable(Allocator& allocator, const Array<LoadNode>& nodes, bool packagePolicy,
                            bool hasBase, Budget& budget) noexcept {
    const usize count = nodes.Size();
    auto spent = budget.Spend(count);
    if (!spent) {
        return spent;
    }

    // visited와 큐, 패키지 정책이면 자식 목록(첫 자식·다음 형제 — 노드 번호 + 1, 0 = 없음)이다.
    Array<uint8> visited(allocator);
    Array<uint32> queue(allocator);
    Array<uint32> firstChild(allocator);
    Array<uint32> nextSibling(allocator);
    if (auto resized = visited.TryResize(count); !resized) {
        return resized;
    }

    if (auto reserved = queue.TryReserve(count); !reserved) {
        return reserved;
    }

    if (packagePolicy) {
        if (auto resized = firstChild.TryResize(count); !resized) {
            return resized;
        }

        if (auto resized = nextSibling.TryResize(count); !resized) {
            return resized;
        }

        // 역순으로 앞에 끼워 자식 목록이 노드 순서가 되게 한다(순서는 판정에 영향이 없다).
        for (usize index = count; index > 0; --index) {
            const uint32 outer = nodes[index - 1].outer;
            if (outer != 0) {
                nextSibling[index - 1] = firstChild[outer - 1];
                firstChild[outer - 1] = static_cast<uint32>(index);
            }
        }
    }

    auto visit = [&](usize node) noexcept {
        if (visited[node] == 0) {
            visited[node] = 1;
            if (!queue.TryPushBack(static_cast<uint32>(node))) {
                platform::Fatal("Object graph reachability queue reservation failed");
            }
        }
    };

    visit(0);
    for (usize index = 0; packagePolicy && index < count; ++index) {
        if (nodes[index].outer != 0) {
            continue;
        }

        // 패키지 정책에서 기준 Outer는 생존 객체이고 outer → inner로 자손을 붙잡는다. 기준 Outer가
        // 없으면 outer 0 노드는 패키지이고, 편의 복원은 노드 1(root)인 패키지 하나만 공개한다 —
        // 다른 패키지는 RootRef 없이 공개되어 다음 GC가 Discard 없이 회수하므로 거부한다(여러
        // 패키지는 단계형 복원과 Commit(roots) — 저장 쪽 결정 17과 대칭).
        if (!hasBase && index != 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        visit(index);
    }

    for (usize position = 0; position < queue.Size(); ++position) {
        const LoadNode& node = nodes[queue[position]];
        spent = budget.Spend(node.tokens.Size() + 2);
        if (!spent) {
            return spent;
        }

        for (const ReferenceToken& token : node.tokens) {
            visit(token.target - 1);
        }

        if (node.outer != 0) {
            visit(node.outer - 1);
        }

        for (uint32 child = packagePolicy ? firstChild[queue[position]] : 0; child != 0;
             child = nextSibling[child - 1]) {
            spent = budget.Spend(1);
            if (!spent) {
                return spent;
            }

            visit(child - 1);
        }
    }

    if (queue.Size() != count) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

// 2단계: 모든 노드를 노드 표의 Outer·이름으로 만들고 보관한 필드를 staged 객체에 쓴 뒤, Required
// 실효 값을 진단하고(2c — CheckRequiredFields) prepare를 부르고 노드 1을 root로 Commit한다. 그래프
// 예산에 접근하지 않는다. 노드 표의 이름·정책 규칙 위반은 여기서 Create가 거부한다(rollback — 생성
// 전 판정은 단계형 복원 A0-Od2).
Result<RootRef<Object>> ConstructGraph(Allocator& allocator, ObjectRegistry& registry,
                                       const ObjectAccessContext& access, Array<LoadNode>& nodes,
                                       const ObjectGraphLoadOptions& options) noexcept {
    ConstructionContext transaction(registry, access);
    for (usize index = 0; index < nodes.Size(); ++index) {
        const uint32 outerIndex = nodes[index].outer;
        const ObjectHandle outer =
            outerIndex == 0 ? options.baseOuter : nodes[outerIndex - 1].handle;
        auto created = transaction.Create<Object>(nodes[index].type, outer, nodes[index].name);
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
                             node.lease.FieldEnum(stored.schemaIndex),
                             node.lease.FieldStruct(stored.schemaIndex));
            auto written =
                stored.ops->decode(*stored.ops, &transaction, node.handle, stored.view, context);
            if (!written) {
                return Result<RootRef<Object>>::Failure(written.Error());
            }
        }

        if (auto ended = context.EndNode(); !ended) {
            return Result<RootRef<Object>>::Failure(ended.Error());
        }
    }

    if (auto checked = CheckRequiredFields(allocator, transaction, nodes, options); !checked) {
        return Result<RootRef<Object>>::Failure(checked.Error());
    }

    if (options.prepare) {
        Array<ObjectHandle> handles(allocator);
        if (auto reserved = handles.TryReserve(nodes.Size()); !reserved) {
            return Result<RootRef<Object>>::Failure(reserved.Error());
        }

        for (const LoadNode& node : nodes) {
            if (!handles.TryPushBack(node.handle)) {
                platform::Fatal("Object graph prepare handle reservation failed");
            }
        }

        if (auto prepared = options.prepare(transaction, handles.View(), options.prepareUser);
            !prepared) {
            return Result<RootRef<Object>>::Failure(prepared.Error());
        }
    }

    return transaction.Commit();
}

} // namespace

Result<RootRef<Object>> LoadObjectGraph(Allocator& allocator, ObjectRegistry& registry,
                                        TypeRegistry& types, const ObjectAccessContext& access,
                                        Span<const uint8> source,
                                        ObjectGraphLoadOptions options) noexcept {
    using Output = Result<RootRef<Object>>;
    if (!registry.UsesTypeRegistry(types)) {
        return Output::Failure(ErrorCode::WrongContext);
    }

    // 값을 바꾸거나 모르는 내용을 건너뛰는 정책은 진단을 받을 곳이 있어야 한다. 노드 수 상한은
    // local ID가 외부 참조 비트와 겹치지 않게 MaxNodeCount 이하다.
    if ((!options.hooks &&
         (options.policy.undeclaredEnum == UndeclaredEnumPolicy::ReportAndDefault ||
          options.policy.newerVersion == NewerVersionPolicy::LoadNewerSkippingUnknown)) ||
        options.limits.maxObjects > MaxNodeCount) {
        return Output::Failure(ErrorCode::InvalidArgument);
    }

    // 기준 Outer는 생존 객체여야 한다(단계형 복원 입력의 base 요약 — 접근·종료·handle 오류는
    // DescribeOuter의 것).
    const bool hasBase = options.baseOuter != ObjectHandle{};
    if (hasBase) {
        if (auto described = registry.DescribeOuter(access, options.baseOuter); !described) {
            return Output::Failure(described.Error());
        }
    }

    auto budget = Budget::Create(allocator, options.limits);
    if (!budget) {
        return Output::Failure(budget.Error());
    }

    auto envelope = ReadEnvelope(allocator, source, budget.Value());
    if (!envelope) {
        return Output::Failure(envelope.Error());
    }

    // 노드 출처: 편의 복원은 그래프의 NodeTable로 노드를 만든다. BaseOuter 비트는 baseOuter 유무와
    // 같아야 하고, 외부 참조 대상(imports — A0-Oe)을 줄 수 없으므로 externalCount는 0이어야 한다.
    const uint32 flags = envelope.Value().flags;
    if ((flags & NodeTableFlag) == 0 || ((flags & BaseOuterFlag) != 0) != hasBase ||
        envelope.Value().externalCount != 0) {
        return Output::Failure(ErrorCode::InvalidArgument);
    }

    Array<LoadNode>& nodes = envelope.Value().nodes;
    GraphValidateContext context(allocator, budget.Value(), options, nodes.Size());
    // 관찰할 struct ID를 정렬해 둔다(미등록 NotFound, 참조를 담는 struct InvalidArgument).
    Array<TypeId> observed(allocator);
    if (options.hooks) {
        const Span<const TypeId> requested = options.hooks->ObservedStructs();
        auto reserved = observed.TryReserve(requested.Size());
        if (!reserved) {
            return Output::Failure(reserved.Error());
        }

        for (TypeId id : requested) {
            auto spent = budget.Value().Spend(1);
            if (!spent) {
                return Output::Failure(spent.Error());
            }

            auto lease = types.AcquireValueType(id, TypeKind::Struct);
            if (!lease) {
                return Output::Failure(lease.Error());
            }

            if (lease.Value().Struct().ContainsStrongReference()) {
                return Output::Failure(ErrorCode::InvalidArgument);
            }

            if (!observed.TryPushBack(id)) {
                platform::Fatal("Object graph observed struct reservation failed");
            }
        }

        uint64 sortWork = 0;
        if (!TryMultiply(uint64{observed.Size()}, uint64{64}, sortWork)) {
            return Output::Failure(ErrorCode::Overflow);
        }

        auto spent = budget.Value().Spend(sortWork);
        if (!spent) {
            return Output::Failure(spent.Error());
        }

        algo::Sort(observed.View(), [](TypeId left, TypeId right) noexcept {
            return left.high != right.high ? left.high < right.high : left.low < right.low;
        });
        context.Observe(observed.View());
    }

    usize references = 0;
    usize nameBytes = 0;
    for (usize index = 0; index < nodes.Size(); ++index) {
        auto decoded = DecodeNode(allocator, types, nodes[index], static_cast<uint32>(index),
                                  budget.Value(), options, context);
        if (!decoded) {
            return Output::Failure(decoded.Error());
        }

        references += nodes[index].tokens.Size();
        nameBytes += nodes[index].name.ByteCount();
    }

    HashMap<TypeId, TypeLease> expectedLeases(allocator);
    if (auto typed =
            ValidateReferenceTypes(allocator, types, nodes, budget.Value(), expectedLeases);
        !typed) {
        return Output::Failure(typed.Error());
    }

    // 2단계의 노드 생성(노드 수 + 이름 byte)·Commit·staged 간선 검증 몫을 미리 청구한다(2단계는
    // 예산으로 실패하지 않는다).
    uint64 phaseTwo = 0;
    if (!TryAdd(static_cast<uint64>(nodes.Size()) + 1, static_cast<uint64>(references), phaseTwo) ||
        !TryAdd(phaseTwo, static_cast<uint64>(nameBytes), phaseTwo)) {
        return Output::Failure(ErrorCode::Overflow);
    }

    if (auto spent = budget.Value().Spend(phaseTwo); !spent) {
        return Output::Failure(spent.Error());
    }

    const bool packagePolicy = registry.Policy().packageNativeType != nullptr;
    if (auto reachable = CheckReachable(allocator, nodes, packagePolicy, hasBase, budget.Value());
        !reachable) {
        return Output::Failure(reachable.Error());
    }

    return ConstructGraph(allocator, registry, access, nodes, options);
}

} // namespace rocks
