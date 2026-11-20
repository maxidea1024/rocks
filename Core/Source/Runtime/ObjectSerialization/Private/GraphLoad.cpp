#include "GraphInternal.h"
#include <Core/Algorithms.h>

// 그래프 복원 1단계(검증)다(Docs/Design/ObjectFoundations.ko.md 5.4절 — ObjectGraphStage::
// Validate의 본문 ValidateGraph): envelope 뒤 노드 출처·노드 정체성(GraphIdentity.cpp)·Record·값을
// 검증하고(참조 값의 범위와 외부 참조의 쓰임 집계 포함) 참조 token의 형식을 검사하며
// (GraphReferences.cpp) 2단계 몫의 작업량을 미리 청구한다. ObjectRegistry에 닿지 않는다.
namespace rocks {
namespace {

using namespace graph_detail;

// 1단계(검증) 문맥이다. 값 reader는 그래프 공유 예산으로 열고, 강한 참조 token을 범위 검사해
// 기록하며(외부 참조는 import마다 쓰임 — 출현 수·공통 플래그·Required — 도 모은다), 진단·수집을
// hook으로 보낸다. Spend는 2단계 몫까지 두 배, ReserveApply는 한 배를 청구한다. 관찰
// 할당(ObservationScratch)은 1단계에만 있는 일이라 Charge로 한 번만 청구한다.
class GraphValidateContext final : public FieldDecodeContext {
public:
    GraphValidateContext(Allocator& allocator, TypeRegistry& types, Budget& budget,
                         const ObjectGraphLoadOptions& options, usize objectCount,
                         Array<GraphExternalUse>& externalUses) noexcept
        : FieldDecodeContext(options.policy, options.hooks, ReferenceWire), allocator_(allocator),
          types_(types), budget_(budget), options_(options), objectCount_(objectCount),
          externalUses_(externalUses), scratch_(allocator, budget), observation_(allocator, *this) {
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

    // 필드 값 안의 Record 내용(Optional<값 struct>의 내부 — 리플렉션 B9e)이다. 맨 위(없으면 객체)
    // Record 입력 안의 범위를 같은 예산으로 연다(색인만 청구 — 내용 byte는 그 Record의 Open이 이미
    // 청구했다).
    Result<RecordScope> OpenRecordBytes(Span<const uint8> record, TypeId expected,
                                        uint32 minVersion, uint32 maxVersion) noexcept override {
        const RecordReader* parent = CurrentRecord() ? CurrentRecord() : record_;
        if (!parent) {
            return Result<RecordScope>::Failure(ErrorCode::InvalidArgument);
        }

        // 최상위 필드의 내용은 2단계가 단독 reader로 넘기고(내용 byte 청구) 단독 Record로 다시
        // 열므로(payload byte 청구) 그 두 배를 미리 청구한다(1단계는 객체 Record가 이미 청구했다 —
        // ReflectionExtensions.ko.md 1.2절 "뒤 단계 주의").
        if (!CurrentRecord()) {
            uint64 work = 0;
            if (!TryMultiply(uint64{record.Size()}, uint64{2}, work)) {
                return Result<RecordScope>::Failure(ErrorCode::CapacityExceeded);
            }

            if (auto spent = Charge(work); !spent) {
                return Result<RecordScope>::Failure(spent.Error());
            }
        }

        auto opened = parent->OpenNestedRecord(allocator_, record, expected, minVersion, maxVersion,
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

    // ClassRef 값의 클래스 판정이다(리플렉션 B9d2). 1단계는 TypeRegistry만 조회한다(잠금으로 보호 —
    // ObjectRegistry에 닿지 않는다). 조회 몫은 codec이 Spend했다.
    Result<TypeLease> AcquireClassReference(TypeId type, TypeId base) noexcept override {
        return detail::CheckClassReferenceLease(types_.Acquire(type), base, nullptr);
    }

    Result<void> AddElements(usize count) noexcept override {
        return budget_.AddElements(count);
    }

    // InstancedStruct 값의 담긴 struct다(리플렉션 B9j). 1단계는 TypeRegistry만 조회한다(잠금으로
    // 보호 — ObjectRegistry에 닿지 않는다). 조회 몫 1을 청구한다(2단계는 조회하지 않으므로 한 번).
    Result<TypeLease> AcquireInstancedStructType(TypeId type) noexcept override {
        if (auto spent = Charge(1); !spent) {
            return Result<TypeLease>::Failure(spent.Error());
        }

        return types_.AcquireValueType(type, TypeKind::Struct);
    }

    // InstancedStruct 판정 하나를 노드의 기록에 더한다(리플렉션 B9j — 2단계가 같은 순서로 lease를
    // 값으로 옮긴다). 기록 메모리 몫(항목 byte)을 한 번 청구한다.
    Result<void> CollectInstancedStruct(TypeId stored, TypeLease type) noexcept override {
        if (!node_) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto spent = Charge(sizeof(StoredInstancedStruct)); !spent) {
            return spent;
        }

        StoredInstancedStruct record;
        record.stored = stored;
        record.type = Move(type);
        if (!node_->instanced.TryPushBack(Move(record))) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<void>::Success();
    }

    Result<DecodedReference>
    ReadReference(ArchiveReader& reader,
                  const ReferenceExpectation& expectation) noexcept override {
        return ReadToken(reader, expectation, false);
    }

    // weak 참조는 같은 그래프 안 노드 번호만 받는다(외부 참조 번호는 writer가 쓰지 않는다 —
    // InvalidArgument, Docs/Design/ReflectionExtensions.ko.md 5.1절). 기대 형식은 강한 참조와 같이
    // token으로 기록해 모든 노드를 해석한 뒤 판정한다(weak token은 도달 검사가 따라가지 않는다).
    Result<DecodedReference>
    ReadWeakReference(ArchiveReader& reader,
                      const ReferenceExpectation& expectation) noexcept override {
        return ReadToken(reader, expectation, true);
    }

    // 참조 하나를 읽어 범위를 검사하고 non-null이면 token으로 기록한다(weak는 노드 번호만).
    Result<DecodedReference>
    ReadToken(ArchiveReader& reader, const ReferenceExpectation& expectation, bool weak) noexcept {
        auto local = reader.ReadU32();
        if (!local) {
            return Result<DecodedReference>::Failure(local.Error());
        }

        // 외부 참조(ExternalReferenceBit | i)는 1 ≤ i ≤ externalCount, 노드 번호는 nodeCount
        // 이하다(그 밖 — 0x80000000 포함 — 은 파일 손상). weak 값에는 외부 참조가 없다.
        const bool isExternal = (local.Value() & ExternalReferenceBit) != 0;
        const uint32 external = isExternal ? local.Value() & ~ExternalReferenceBit : 0;
        const bool inRange = isExternal ? !weak && external != 0 && external <= externalUses_.Size()
                                        : local.Value() <= objectCount_;
        if (!inRange) {
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
        token.weak = weak;
        if (!node_->tokens.TryPushBack(token)) {
            return Result<DecodedReference>::Failure(ErrorCode::OutOfMemory);
        }

        // 외부 참조의 쓰임은 저장의 GraphExternal과 같은 규칙으로 모은다(위치의 유효 플래그 —
        // 배열 원소는 배열 필드의 것).
        if (external != 0) {
            GraphExternalUse& use = externalUses_[external - 1];
            const uint32 flags = EffectiveFlags();
            ++use.occurrences;
            use.commonFlags &= flags;
            use.required = use.required || HasFlag(flags, FieldFlags::Required);
        }

        return Result<DecodedReference>::Success(decoded);
    }

    // 검증한 Name 표시 문자열을 노드의 기록에 더하고 2a intern 몫(byte + NameInternWork)을 한 번
    // 청구한다(2단계는 intern하지 않으므로 Spend의 두 배가 아니다 — Charge로 세어 reader 작업 Δ
    // 청구와 겹치지 않는다).
    Result<void> CollectName(StringView text) noexcept override {
        if (!node_) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        uint64 work = 0;
        if (!TryAdd(uint64{text.ByteCount()}, NameInternWork, work)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        if (auto spent = Charge(work); !spent) {
            return spent;
        }

        StoredName stored;
        stored.text = text;
        if (!node_->names.TryPushBack(stored)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<void>::Success();
    }

    // ClassRef 판정 하나를 노드의 기록에 더한다(리플렉션 B9d2 — 2단계가 같은 순서로 소비한다). 기록
    // 메모리 몫(항목 byte)을 한 번 청구한다(2단계는 조회하지 않으므로 Spend의 두 배가 아니다).
    Result<void> CollectClassReference(TypeId stored, bool nulled) noexcept override {
        if (!node_) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto spent = Charge(sizeof(StoredClassReference)); !spent) {
            return spent;
        }

        if (!node_->classes.TryPushBack(StoredClassReference{stored, nulled})) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<void>::Success();
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

        // 상위 allocator에 그대로 넘긴다(Allocator.h).
        void TrimMemory() noexcept override {
            allocator_.TrimMemory();
        }

        void MarkThreadIdle() noexcept override {
            allocator_.MarkThreadIdle();
        }

        void MarkThreadActive() noexcept override {
            allocator_.MarkThreadActive();
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
    TypeRegistry& types_;
    Budget& budget_;
    const ObjectGraphLoadOptions& options_;
    usize objectCount_;
    // 외부 참조 i의 쓰임이다(i − 1 위치, 크기 = externalCount).
    Array<GraphExternalUse>& externalUses_;
    BudgetAllocator scratch_;
    ObservationAllocator observation_;
    LoadNode* node_ = nullptr;
    const RecordReader* record_ = nullptr;
    uint64 charged_ = 0;
    usize expectationStart_ = 0;
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

// 1단계: 노드 하나를 검증하고 필드 색인·참조 token을 보관한다. 생성자를 실행하지 않는다. 노드 표의
// 정체성 규칙(CheckNewObjectIdentity·형제 유일성 — identity)은 Record보다 먼저 본다(5.4절 노드 검사
// 순서). Record에 없는 영속 필드는 오류가 아니며 생성자 값(기준)을 유지한다(누락 = 기준 —
// Docs/Design/ObjectFoundations.ko.md 4절). 파일에 있는 필드마다 검증 뒤 MarkPresent를 한 번
// 부른다(A5 override 집합 자리 — detail::DecodeRecordField). Required 진단은 여기서 하지
// 않는다(적용 뒤 CheckRequiredFields).
Result<void> DecodeNode(Allocator& allocator, TypeRegistry& types, LoadNode& node, uint32 index,
                        Budget& budget, const ObjectGraphLoadOptions& options,
                        GraphValidateContext& context, NodeIdentityCheck& identity) noexcept {
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
    // outer ≤ index다.
    if (node.outer > index) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 노드 표의 이름·정책·대표 자식·경로 규칙과 형제 유일성을 생성 전에 판정한다(Registry 정책
    // 사본 — 2a의 Create는 노드 표 때문에 실패하지 않는다).
    if (auto identified = identity.Check(node, budget); !identified) {
        return identified;
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
    // 저장 불가 클래스(Transient·Deprecated — 상속 포함, 리플렉션 B9h2) 노드는 정상으로 읽고 진단을
    // 낸다(받을 hook이 없으면 하지 않는다).
    if (HasFlag(schema.flags, TypeFlags::Transient)) {
        if (auto reported = context.Report(FieldDiagnosticKind::TransientType, 0, schema.id);
            !reported) {
            return reported;
        }
    }

    if (HasFlag(schema.flags, TypeFlags::Deprecated)) {
        if (auto reported = context.Report(FieldDiagnosticKind::DeprecatedType, 0, schema.id);
            !reported) {
            return reported;
        }
    }

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

        // 이 데이터에 없어야 하는 플래그(쿠킹 로드의 EditorOnly)의 필드와 포함 필터(includeFlags —
        // 리플렉션 B9h1) 밖의 필드가 있으면 거부한다(최상위 필드의 유효 플래그는 필드 플래그다 —
        // struct 안 멤버는 DecodeStructRecord가 본다).
        if (context.IsExcluded(schema.fields[schemaIndex].flags) ||
            (context.Policy().includeFlags != 0 &&
             (schema.fields[schemaIndex].flags & context.Policy().includeFlags) == 0)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto accessor = FindOps(node.lease, field.Value().id, budget);
        if (!accessor) {
            return Result<void>::Failure(accessor.Error());
        }

        FieldScope scope(context, schema.fields[schemaIndex], schema.id,
                         node.lease.FieldEnum(schemaIndex), node.lease.FieldStruct(schemaIndex),
                         FieldCodecFrame::NoIndex, node.lease.FieldKeyEnum(schemaIndex));
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

} // namespace

namespace graph_detail {

Result<void> CheckLoadOptions(const ObjectGraphLoadOptions& options) noexcept {
    // 값을 바꾸거나 모르는 내용을 건너뛰는 정책은 진단을 받을 곳이 있어야 한다. 노드 수·외부 참조
    // 수 상한은 local ID·외부 번호가 외부 참조 비트와 겹치지 않게 2^31 − 1 이하다(저장과 같다).
    if ((!options.hooks &&
         (options.policy.undeclaredEnum == UndeclaredEnumPolicy::ReportAndDefault ||
          options.policy.newerVersion == NewerVersionPolicy::LoadNewerSkippingUnknown)) ||
        options.limits.maxObjects > MaxNodeCount ||
        options.limits.maxExternalReferences > MaxExternalCount) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<UniquePtr<StageData>> ValidateGraph(Allocator& allocator, TypeRegistry& types,
                                           const RegistryPolicy& policy,
                                           const ObjectGraphLoadOptions& options,
                                           const ObjectGraphInput& input) noexcept {
    using Output = Result<UniquePtr<StageData>>;
    if (auto checked = CheckLoadOptions(options); !checked) {
        return Output::Failure(checked.Error());
    }

    auto budget = Budget::Create(allocator, options.limits);
    if (!budget) {
        return Output::Failure(budget.Error());
    }

    auto envelope = ReadEnvelope(allocator, input.bytes, budget.Value());
    if (!envelope) {
        return Output::Failure(envelope.Error());
    }

    // 노드 출처: 그래프의 NodeTable과 호출자 노드 표 중 정확히 하나다. NodeTable이면 BaseOuter
    // 비트가 hasBase와 같아야 하고, 호출자 노드 표는 nodeCount개다. 외부 참조의 선언
    // 타입(importTypes)은 externalCount개이고 0이 없다.
    const uint32 flags = envelope.Value().flags;
    const bool nodeTable = (flags & NodeTableFlag) != 0;
    const usize count = envelope.Value().nodes.Size();
    const uint32 externalCount = envelope.Value().externalCount;
    // 기준 Outer 요약의 경로 길이도 Registry 규칙(4 KiB) 안이어야 한다(호출자가 준 값 — 넘으면 어떤
    // 기준 Outer와도 맞지 않는다).
    if ((nodeTable && input.nodes.Size() != 0) || (!nodeTable && input.nodes.Size() == 0) ||
        (nodeTable && ((flags & BaseOuterFlag) != 0) != input.hasBase) ||
        (!nodeTable && input.nodes.Size() != count) || input.importTypes.Size() != externalCount ||
        (input.hasBase && input.base.pathBytes > MaxObjectPathBytes)) {
        return Output::Failure(ErrorCode::InvalidArgument);
    }

    if (auto spent = budget.Value().Spend(externalCount); !spent) {
        return Output::Failure(spent.Error());
    }

    for (TypeId type : input.importTypes) {
        if (type == TypeId{}) {
            return Output::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (!nodeTable) {
        // 호출자 노드 표를 노드에 옮긴다(이름은 호출자 byte를 빌린다). 타입 0은 NodeTable과 같이
        // 손상이다.
        if (auto spent = budget.Value().Spend(count); !spent) {
            return Output::Failure(spent.Error());
        }

        for (usize index = 0; index < count; ++index) {
            const GraphNodeDesc& desc = input.nodes[index];
            if (desc.type == TypeId{}) {
                return Output::Failure(ErrorCode::InvalidArgument);
            }

            LoadNode& node = envelope.Value().nodes[index];
            node.type = desc.type;
            node.outer = desc.outer;
            node.name = desc.name;
        }
    }

    // 노드는 여기서 자리를 잡은 뒤 옮기지 않는다(FieldView가 node.migrated를 가리킨다).
    auto made = UniquePtr<StageData>::TryMake(allocator, allocator, Move(budget.Value()),
                                              Move(envelope.Value()));
    if (!made) {
        return Output::Failure(made.Error());
    }

    StageData& data = *made.Value();
    Array<LoadNode>& nodes = data.envelope.nodes;
    // 외부 참조 i의 쓰임(출현 0·공통 플래그 ~0·Required 아님에서 시작 — 노드 출처에서 청구했다).
    if (auto resized = data.externalUses.TryResize(externalCount); !resized) {
        return Output::Failure(resized.Error());
    }

    GraphValidateContext context(allocator, types, data.budget, options, nodes.Size(),
                                 data.externalUses);
    // 관찰할 struct ID를 정렬해 둔다(미등록 NotFound, 참조(강한·weak)나 Name을 담는 struct
    // InvalidArgument — 관찰 값은 1단계 임시 값인데 1단계는 참조 대상도 Name 토큰도 만들 수 없다).
    Array<TypeId> observed(allocator);
    if (options.hooks) {
        const Span<const TypeId> requested = options.hooks->ObservedStructs();
        auto reserved = observed.TryReserve(requested.Size());
        if (!reserved) {
            return Output::Failure(reserved.Error());
        }

        for (TypeId id : requested) {
            auto spent = data.budget.Spend(1);
            if (!spent) {
                return Output::Failure(spent.Error());
            }

            auto lease = types.AcquireValueType(id, TypeKind::Struct);
            if (!lease) {
                return Output::Failure(lease.Error());
            }

            // InstancedStruct를 담은 struct도 관찰하지 않는다(리플렉션 B9j — 1단계 임시 값에 담긴
            // 타입의 lease를 둘 곳이 없다).
            if (lease.Value().Struct().ContainsStrongReference() ||
                lease.Value().Struct().ContainsWeakReference() ||
                lease.Value().Struct().ContainsName() ||
                lease.Value().Struct().ContainsInstancedStruct()) {
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

        auto spent = data.budget.Spend(sortWork);
        if (!spent) {
            return Output::Failure(spent.Error());
        }

        algo::Sort(observed.View(), [](TypeId left, TypeId right) noexcept {
            return left.high != right.high ? left.high < right.high : left.low < right.low;
        });
        context.Observe(observed.View());
    }

    NodeIdentityCheck identity(allocator, policy, input.hasBase, input.base);
    if (auto reserved = identity.Reserve(nodes.Size()); !reserved) {
        return Output::Failure(reserved.Error());
    }

    usize references = 0;
    usize nameBytes = 0;
    for (usize index = 0; index < nodes.Size(); ++index) {
        auto decoded = DecodeNode(allocator, types, nodes[index], static_cast<uint32>(index),
                                  data.budget, options, context, identity);
        if (!decoded) {
            return Output::Failure(decoded.Error());
        }

        references += nodes[index].tokens.Size();
        nameBytes += nodes[index].name.ByteCount();
    }

    if (auto typed = ValidateReferenceTypes(allocator, types, data, input.importTypes); !typed) {
        return Output::Failure(typed.Error());
    }

    // 2단계의 노드 생성(노드 수 + 이름 byte)·Commit·staged 간선 검증 몫과 2b의 imports 확인 몫
    // (import마다 타입 조회 + 기록한 쌍마다 판정)을 미리 청구한다(2단계는 예산으로 실패하지
    // 않는다).
    uint64 phaseTwo = 0;
    if (!TryAdd(static_cast<uint64>(nodes.Size()) + 1, static_cast<uint64>(references), phaseTwo) ||
        !TryAdd(phaseTwo, static_cast<uint64>(nameBytes), phaseTwo) ||
        !TryAdd(phaseTwo, static_cast<uint64>(externalCount), phaseTwo) ||
        !TryAdd(phaseTwo, static_cast<uint64>(data.importChecks.Size()), phaseTwo)) {
        return Output::Failure(ErrorCode::Overflow);
    }

    if (auto spent = data.budget.Spend(phaseTwo); !spent) {
        return Output::Failure(spent.Error());
    }

    return Output::Success(Move(made.Value()));
}

} // namespace graph_detail

} // namespace rocks
