#include "GraphInternal.h"
#include <Core/Text/NamePool.h>
#include <ObjectSerialization/StructRecord.h>

// 독립 struct Record다(Docs/Design/ReflectionExtensions.ko.md 26절). 저장은 합성 struct 필드 하나의
// 위치에서 EncodeStructRecord를, 읽기는 같은 위치에서 DecodeStructValue를 그래프와 같은 두 단계로
// 부른다. 그래프 저장·복원 문맥(GraphSaveInternal.h·GraphLoad.cpp·GraphApply.cpp)을 값 하나로 줄인
// 것이다: 노드·외부 참조·NodeTable이 없고, 강한 참조는 담기 조건이 막으며, weak 참조는 null만 쓴다.
namespace rocks {
namespace {

using namespace graph_detail;

// 참조 wire는 그래프와 같은 U32다(InstancedStruct 필드 내용과 같은 바이트 — weak null은 0).
static_assert(ReferenceWire == static_cast<uint32>(WireType::U32));

constexpr char ValueFieldName[] = {'v', 'a', 'l', 'u', 'e'};

// struct 값 하나를 감싸는 합성 필드다(최상위 위치 — 필드 ID는 struct ID, 플래그 0).
FieldSchema ValueField(const TypeSchema& schema) noexcept {
    FieldSchema field;
    field.id = schema.id;
    field.kind = FieldKind::Value;
    field.name = Span<const char>(ValueFieldName, sizeof(ValueFieldName));
    field.value = ValueType{ValueKind::Struct, schema.id};
    return field;
}

// 담을 수 있는 struct lease인지 본다(InstancedStruct 담기 조건과 위치 깊이 0의 깊이 합).
Result<void> CheckStandaloneStruct(const TypeLease& type) noexcept {
    if (!type.IsValid() || type.Kind() != TypeKind::Struct) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const StructView view = type.Struct();
    if (auto allowed = detail::CheckInstancedStructContent(view); !allowed) {
        return allowed;
    }

    return detail::CheckInstancedStructDepth(0, view);
}

// 저장 문맥이다(GraphEncodeContext를 값 하나로 줄였다). 강한 참조는 담기 조건이 막으므로 오면 계약
// 위반이고, weak 참조는 그래프 저장의 Reject 정책(저장 집합 밖 대상)과 같이 null로 쓴다.
class StandaloneEncodeContext final : public FieldEncodeContext {
public:
    StandaloneEncodeContext(Allocator& allocator, ObjectRegistry& registry,
                            const ObjectAccessContext& access, Budget& budget,
                            const StructRecordOptions& options, TypeId type) noexcept
        : FieldEncodeContext(options.encodePolicy, options.hooks, ReferenceWire),
          registry_(registry), access_(access), budget_(budget), scratch_(allocator, budget),
          observation_(allocator, budget, false) {
        SetNames(registry.Names());
        SetObject(type, 0);
    }

    Result<void> Spend(uint64 work) noexcept override {
        return budget_.Spend(work);
    }

    Result<void> ReserveApply(uint64) noexcept override {
        return Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return scratch_;
    }

    Allocator& ObservationScratch() noexcept override {
        return observation_;
    }

    Result<void> AddFields(usize count) noexcept override {
        return budget_.AddFields(count);
    }

    Result<void> AddElements(usize count) noexcept override {
        return budget_.AddElements(count);
    }

    // ClassRef 값의 클래스 판정이다(그래프 저장과 같다 — 이 Registry의 TypeRegistry).
    Result<TypeLease> AcquireClassReference(TypeId type, TypeId base) noexcept override {
        return detail::CheckClassReferenceLease(registry_.AcquireRegisteredType(access_, type),
                                                base, nullptr);
    }

    // 중첩 Record는 값 한도 안이다(그래프 저장과 같다).
    RecordLimits NestedRecordLimits() const noexcept override {
        RecordLimits limits = budget_.Record();
        limits.archive.maxTotalBytes = budget_.limits.maxValueBytes;
        return limits;
    }

protected:
    // 담기 조건이 강한 참조를 막는다 — 여기 오면 InvalidArgument다.
    Result<void> WriteReferenceWire(ArchiveWriter&, ObjectHandle,
                                    const ReferenceExpectation&) noexcept override {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // weak 참조는 그래프 밖이라 대상을 쓸 곳이 없다. 그래프 저장의 집합 밖 Reject 규칙과 같다(Docs/
    // Design/ReflectionExtensions.ko.md 26절): 다른 Registry의 handle은 WrongContext, 파괴된 대상은
    // 0, 살아 있는 대상은 기대 형식 검사 뒤 ExternalReference 진단(값 2) 뒤 0이다.
    Result<void> WriteWeakReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                        const ReferenceExpectation& expectation) noexcept override {
        if (target.IsNull()) {
            return out.WriteU32(0);
        }

        if (!registry_.OwnsHandle(target)) {
            return Result<void>::Failure(ErrorCode::WrongContext);
        }

        if (auto spent = budget_.Spend(1); !spent) {
            return spent;
        }

        auto actual = registry_.AcquireType(access_, target);
        if (!actual) {
            const bool dead =
                actual.Error() == ErrorCode::StaleHandle || actual.Error() == ErrorCode::Destroyed;
            return dead ? out.WriteU32(0) : Result<void>::Failure(actual.Error());
        }

        if (expectation.staticNative && !actual.Value().IsNativeA(expectation.staticNative)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (expectation.schemaType != Guid{}) {
            auto expected = registry_.AcquireRegisteredType(access_, expectation.schemaType);
            if (!expected) {
                return Result<void>::Failure(expected.Error());
            }

            if (!actual.Value().IsNativeA(expected.Value().Ops().nativeType)) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }
        }

        if (auto reported = Report(FieldDiagnosticKind::ExternalReference, 2); !reported) {
            return reported;
        }

        return out.WriteU32(0);
    }

private:
    ObjectRegistry& registry_;
    const ObjectAccessContext& access_;
    Budget& budget_;
    BudgetAllocator scratch_;
    BudgetAllocator observation_;
};

// 1단계가 기록하고 2단계가 같은 순서로 소비하는 판정들이다(그래프 LoadNode의 기록과 같은 모델).
struct StandaloneRecords {
    explicit StandaloneRecords(Allocator& allocator) noexcept
        : migrations(allocator), names(allocator), classes(allocator) {
    }

    Array<StoredStructMigration> migrations;
    Array<StoredName> names;
    Array<StoredClassReference> classes;
};

// 읽기 문맥이다. 1단계(Recording 참 — GraphValidateContext를 값 하나로 줄였다)는 공유 예산으로
// Record를 열고 마이그레이션·Name·ClassRef 판정을 records에 기록하며 2단계 몫까지 청구한다.
// 2단계(Recording 거짓 — GraphApplyContext와 같다)는 같은 크기 한도·무제한 작업으로 다시 열고
// 기록을 순서대로 소비한다.
class StandaloneDecodeContext final : public FieldDecodeContext {
public:
    StandaloneDecodeContext(Allocator& allocator, TypeRegistry& types, NamePool* names,
                            Budget& budget, const StructRecordOptions& options,
                            StandaloneRecords& records, TypeId type, bool validate) noexcept
        : FieldDecodeContext(options.decodePolicy, options.hooks, ReferenceWire),
          allocator_(allocator), types_(types), pool_(names), budget_(budget), options_(options),
          records_(records), validate_(validate), scratch_(allocator, budget),
          observation_(allocator, budget, false) {
        SetObject(type, 0);
        SetRecording(validate);
    }

    // 1단계가 문맥으로 청구한 작업량이다(reader가 공유 예산에 청구한 몫과 가르는 데 쓴다).
    uint64 Charged() const noexcept {
        return charged_;
    }

    // 2단계가 기록을 모두 소비했는지다(남으면 두 단계의 방문 순서가 어긋난 계약 위반).
    Result<void> Finish() const noexcept {
        if (!validate_ && (migration_ != records_.migrations.Size() ||
                           name_ != records_.names.Size() || class_ != records_.classes.Size())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<void>::Success();
    }

    Result<RecordScope> OpenRecord(const FieldView& field, TypeId expected, uint32 minVersion,
                                   uint32 maxVersion) noexcept override {
        if (field.wireType != static_cast<uint32>(WireType::Record)) {
            return Result<RecordScope>::Failure(ErrorCode::TypeMismatch);
        }

        // 최상위 값은 Record 내용(OpenRecordBytes)으로 열므로 필드 Record는 늘 맨 위 Record 안이다.
        RecordReader* parent = CurrentRecord();
        if (!parent) {
            return Result<RecordScope>::Failure(ErrorCode::InvalidArgument);
        }

        auto opened = parent->OpenRecordField(allocator_, field, expected, minVersion, maxVersion,
                                              FieldLimit());
        return Push(Move(opened));
    }

    // 최상위 Record(스택이 빔)는 단독으로 연다: 1단계는 공유 예산(크기 한도는 값 한도), 2단계는
    // 같은 크기 한도·무제한 작업. 안쪽 Record 내용(Optional<struct>)은 맨 위 Record의
    // OpenNestedRecord다.
    Result<RecordScope> OpenRecordBytes(Span<const uint8> record, TypeId expected,
                                        uint32 minVersion, uint32 maxVersion) noexcept override {
        if (RecordReader* parent = CurrentRecord()) {
            auto opened = parent->OpenNestedRecord(allocator_, record, expected, minVersion,
                                                   maxVersion, FieldLimit());
            return Push(Move(opened));
        }

        return Push(OpenStandalone(record, expected, minVersion, maxVersion));
    }

    // 마이그레이션 결과는 정확히 현재 버전으로 다시 검사한다(콜백 결과를 신뢰하지 않는다).
    Result<RecordScope> OpenMigratedRecord(Span<const uint8> record, TypeId expected,
                                           uint32 version) noexcept override {
        return Push(OpenStandalone(record, expected, version, version));
    }

    Result<RecordListReader> OpenRecordList(const FieldView& field,
                                            TypeId itemSchema) noexcept override {
        if (field.wireType != static_cast<uint32>(WireType::RecordList)) {
            return Result<RecordListReader>::Failure(ErrorCode::TypeMismatch);
        }

        RecordReader* parent = CurrentRecord();
        if (!parent) {
            return Result<RecordListReader>::Failure(ErrorCode::InvalidArgument);
        }

        // 1단계는 남은 원소 누계로도 항목 수를 묶는다(그래프 1단계와 같다).
        RecordListLimits limits = ArrayLoadLimits(budget_.limits);
        if (validate_) {
            const uint32 remaining = budget_.RemainingElements();
            limits.maxItems = remaining < limits.maxItems ? remaining : limits.maxItems;
        }

        return RecordListReader::OpenField(*parent, field, itemSchema, 1,
                                           NumericLimits<uint32>::Max(), limits);
    }

    Result<RecordScope> OpenListItem(RecordListReader& list, usize index) noexcept override {
        auto opened = list.OpenItem(index, FieldLimit());
        return Push(Move(opened));
    }

    Result<Span<const uint8>> MigrateStruct(TypeId type, uint32 fromVersion, uint32 toVersion,
                                            Span<const uint8> record) noexcept override {
        if (!validate_) {
            if (migration_ >= records_.migrations.Size()) {
                return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
            }

            const StoredStructMigration& stored = records_.migrations[migration_];
            if (stored.type != type || stored.from != fromVersion) {
                return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
            }

            ++migration_;
            return Result<Span<const uint8>>::Success(stored.bytes.View());
        }

        const FieldCodecFrame* frame = Current();
        if (!frame || frame->structure.IsEmpty()) {
            return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto spent = Charge(record.Size()); !spent) {
            return Result<Span<const uint8>>::Failure(spent.Error());
        }

        // 옵션 override를 먼저 묻고(Unsupported면 다음), 타입이 등록한 마이그레이션을 쓴다.
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

        if (auto spent = Charge(bytes); !spent) {
            return Result<Span<const uint8>>::Failure(spent.Error());
        }

        StoredStructMigration stored(allocator_);
        stored.type = type;
        stored.from = fromVersion;
        stored.bytes = Move(migrated.Value());
        if (!records_.migrations.TryPushBack(Move(stored))) {
            return Result<Span<const uint8>>::Failure(ErrorCode::OutOfMemory);
        }

        budget_.migrationBytes += bytes;
        return Result<Span<const uint8>>::Success(
            records_.migrations[records_.migrations.Size() - 1].bytes.View());
    }

    Result<void> AddFields(usize count) noexcept override {
        return validate_ ? budget_.AddFields(count) : Result<void>::Success();
    }

    Result<void> AddElements(usize count) noexcept override {
        return validate_ ? budget_.AddElements(count) : Result<void>::Success();
    }

    // ClassRef 판정은 1단계만 한다(TypeRegistry 조회 — 2단계는 기록을 소비한다).
    Result<TypeLease> AcquireClassReference(TypeId type, TypeId base) noexcept override {
        return detail::CheckClassReferenceLease(types_.Acquire(type), base, nullptr);
    }

    // 담기 조건이 강한 참조를 막는다 — 여기 오면 InvalidArgument다.
    Result<DecodedReference> ReadReference(ArchiveReader&,
                                           const ReferenceExpectation&) noexcept override {
        return Result<DecodedReference>::Failure(ErrorCode::InvalidArgument);
    }

    // weak 참조는 0(null)만 받는다.
    Result<DecodedReference> ReadWeakReference(ArchiveReader& reader,
                                               const ReferenceExpectation&) noexcept override {
        auto local = reader.ReadU32();
        if (!local) {
            return Result<DecodedReference>::Failure(local.Error());
        }

        if (local.Value() != 0) {
            return Result<DecodedReference>::Failure(ErrorCode::CorruptData);
        }

        DecodedReference decoded;
        decoded.isNull = true;
        return Result<DecodedReference>::Success(decoded);
    }

    // 1단계는 Name 표시 문자열을 기록하고 intern 몫을 청구한다(그래프 1단계와 같다).
    Result<void> CollectName(StringView text) noexcept override {
        uint64 work = 0;
        if (!TryAdd(uint64{text.ByteCount()}, NameInternWork, work)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        if (auto spent = Charge(work); !spent) {
            return spent;
        }

        StoredName stored;
        stored.text = text;
        if (!records_.names.TryPushBack(stored)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<void>::Success();
    }

    // 2단계는 기록과 같은 문자열을 받아 intern한 토큰을 돌려준다(InternNames가 미리 만든다).
    Result<Name> TakeName(StringView text) noexcept override {
        if (name_ >= records_.names.Size() || !(records_.names[name_].text == text)) {
            return Result<Name>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Name>::Success(records_.names[name_++].token);
    }

    Result<void> CollectClassReference(TypeId stored, bool nulled) noexcept override {
        if (auto spent = Charge(sizeof(StoredClassReference)); !spent) {
            return spent;
        }

        if (!records_.classes.TryPushBack(StoredClassReference{stored, nulled})) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<void>::Success();
    }

    Result<TypeId> TakeClassReference(TypeId stored) noexcept override {
        if (class_ >= records_.classes.Size() || !(records_.classes[class_].stored == stored)) {
            return Result<TypeId>::Failure(ErrorCode::InvalidArgument);
        }

        const bool nulled = records_.classes[class_++].nulled;
        return Result<TypeId>::Success(nulled ? TypeId{} : stored);
    }

    // 1단계 Spend는 2단계 몫까지 두 배를 청구한다(그래프 1단계와 같다). 2단계는 청구하지 않는다.
    Result<void> Spend(uint64 work) noexcept override {
        if (!validate_) {
            return Result<void>::Success();
        }

        uint64 doubled = 0;
        if (!TryMultiply(work, uint64{2}, doubled)) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        return Charge(doubled);
    }

    Result<void> ReserveApply(uint64 work) noexcept override {
        return validate_ ? Charge(work) : Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return validate_ ? static_cast<Allocator&>(scratch_) : allocator_;
    }

    Allocator& ObservationScratch() noexcept override {
        return validate_ ? static_cast<Allocator&>(observation_) : allocator_;
    }

    // 2단계 전에 기록한 Name을 pool에 intern한다(None은 기본 Name — 그래프 2a InternNodeNames와
    // 같다).
    Result<void> InternNames() noexcept {
        for (StoredName& stored : records_.names) {
            if (IsNoneNameText(stored.text)) {
                stored.token = Name{};
                continue;
            }

            if (pool_ == nullptr) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            auto token = pool_->TryIntern(stored.text);
            if (!token) {
                return Result<void>::Failure(token.Error());
            }

            stored.token = token.Value();
        }

        return Result<void>::Success();
    }

protected:
    // 최상위 값은 Record 내용으로 열므로 단독 필드 값을 열 일이 없다.
    Result<ArchiveReader> OpenTopLevelValue(const FieldView&) noexcept override {
        return Result<ArchiveReader>::Failure(ErrorCode::InvalidArgument);
    }

private:
    Result<void> Charge(uint64 work) noexcept {
        auto spent = budget_.Spend(work);
        if (spent) {
            charged_ += work;
        }

        return spent;
    }

    uint32 FieldLimit() const noexcept {
        return validate_ ? budget_.RemainingFields() : budget_.limits.maxTotalFields;
    }

    Result<RecordReader> OpenStandalone(Span<const uint8> record, TypeId expected,
                                        uint32 minVersion, uint32 maxVersion) noexcept {
        if (validate_) {
            RecordLimits limits = budget_.Record();
            limits.archive.maxTotalBytes = budget_.limits.maxValueBytes;
            return RecordReader::Open(allocator_, record, expected, minVersion, maxVersion, limits,
                                      budget_.Shared());
        }

        ArchiveLimits archive;
        archive.maxTotalBytes = budget_.limits.maxValueBytes;
        archive.maxValueBytes = budget_.limits.maxValueBytes;
        archive.maxOperations = NumericLimits<uint64>::Max();
        archive.maxWork = NumericLimits<uint64>::Max();
        return RecordReader::Open(allocator_, record, expected, minVersion, maxVersion,
                                  {archive, budget_.limits.maxTotalFields});
    }

    // 1단계는 연 Record의 필드 수를 누계에 더한다(큰 입력이 누계 검사 전에 색인을 할당하지 않게 연
    // 쪽이 남은 누계를 한도로 썼다).
    Result<RecordScope> Push(Result<RecordReader> opened) noexcept {
        if (!opened) {
            return Result<RecordScope>::Failure(opened.Error());
        }

        if (validate_) {
            if (auto counted = budget_.AddFields(opened.Value().FieldCount()); !counted) {
                return Result<RecordScope>::Failure(counted.Error());
            }
        }

        return PushRecord(Move(opened.Value()));
    }

    Allocator& allocator_;
    TypeRegistry& types_;
    NamePool* pool_;
    Budget& budget_;
    const StructRecordOptions& options_;
    StandaloneRecords& records_;
    bool validate_;
    BudgetAllocator scratch_;
    BudgetAllocator observation_;
    usize migration_ = 0;
    usize name_ = 0;
    usize class_ = 0;
    uint64 charged_ = 0;
};

// 옵션 검사다. 한도는 값 하나라 노드·외부 참조 한도를 보지 않는다. 값을 바꾸거나 모르는 내용을
// 건너뛰는 읽기 정책은 그래프 로드(CheckLoadOptions)와 같이 진단 받을 곳(hooks)이 있어야 한다.
Result<void> CheckOptions(const StructRecordOptions& options, bool load) noexcept {
    if (options.limits.maxValueBytes == 0 || options.limits.maxTotalFields == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (load && !options.hooks &&
        (options.decodePolicy.undeclaredEnum == UndeclaredEnumPolicy::ReportAndDefault ||
         options.decodePolicy.newerVersion == NewerVersionPolicy::LoadNewerSkippingUnknown)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

// 값 블록 태그다("STRC").
constexpr uint32 ValueBlockTag = 0x53545243u;

// 값 블록 하나의 수명이다(모든 경로에서 소멸·반환).
struct ValueBlock {
    ~ValueBlock() {
        if (memory) {
            ops->destroy(memory);
            if (!allocator->Deallocate(memory)) {
                platform::Fatal("Struct record value block ownership mismatch");
            }
        }
    }

    const RuntimeStructOps* ops = nullptr;
    Allocator* allocator = nullptr;
    void* memory = nullptr;
};

} // namespace

Result<Array<uint8>> SaveStructRecord(Allocator& allocator, ObjectRegistry& registry,
                                      const ObjectAccessContext& access, const TypeLease& type,
                                      const void* value,
                                      const StructRecordOptions& options) noexcept {
    if (value == nullptr) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto checked = CheckStandaloneStruct(type); !checked) {
        return Result<Array<uint8>>::Failure(checked.Error());
    }

    if (auto checked = CheckOptions(options, false); !checked) {
        return Result<Array<uint8>>::Failure(checked.Error());
    }

    auto budget = Budget::Create(allocator, options.limits);
    if (!budget) {
        return Result<Array<uint8>>::Failure(budget.Error());
    }

    const StructView view = type.Struct();
    const TypeSchema& schema = view.Schema();
    const FieldSchema field = ValueField(schema);
    StandaloneEncodeContext context(allocator, registry, access, budget.Value(), options,
                                    schema.id);
    ArchiveLimits limits = budget.Value().Archive();
    limits.maxTotalBytes = options.limits.maxValueBytes;
    ArchiveWriter out(allocator, limits);
    {
        FieldScope scope(context, field, TypeId{}, nullptr, view);
        auto written = detail::EncodeStructRecord(view, value, out, context);
        if (!written) {
            return Result<Array<uint8>>::Failure(written.Error());
        }
    }

    if (auto spent = budget.Value().Spend(out.WorkUsed()); !spent) {
        return Result<Array<uint8>>::Failure(spent.Error());
    }

    return Result<Array<uint8>>::Success(out.TakeBytes());
}

Result<void> LoadStructRecord(Allocator& allocator, ObjectRegistry& registry,
                              const ObjectAccessContext& access, Span<const uint8> record,
                              const TypeLease& type, void* value,
                              const StructRecordOptions& options) noexcept {
    if (value == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto checked = CheckStandaloneStruct(type); !checked) {
        return checked;
    }

    if (auto checked = CheckOptions(options, true); !checked) {
        return checked;
    }

    const StructView view = type.Struct();
    const RuntimeStructOps& ops = view.StructOps();
    if (ops.swap == nullptr || ops.construct == nullptr || ops.destroy == nullptr) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (record.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (record.Size() > options.limits.maxValueBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    auto budget = Budget::Create(allocator, options.limits);
    if (!budget) {
        return Result<void>::Failure(budget.Error());
    }

    const TypeSchema& schema = view.Schema();
    const FieldSchema field = ValueField(schema);
    StandaloneRecords records(allocator);
    detail::StructRecordSource source;
    source.record = record;

    // 1단계: 값에 닿지 않는 검증과 기록. 끝에 2단계 reader가 다시 할 몫(1단계 reader가 공유 예산에
    // 청구한 작업 — 문맥 청구를 뺀 나머지)을 한 번 더 청구한다(그래프 로드 DecodeNode와 같다).
    {
        const uint64 before = budget.Value().Used();
        StandaloneDecodeContext validate(allocator, registry.Types(), registry.Names(),
                                         budget.Value(), options, records, schema.id, true);
        FieldScope scope(validate, field, TypeId{}, nullptr, view);
        if (auto decoded =
                detail::DecodeStructValue(view, nullptr, ObjectHandle{}, nullptr, source, validate);
            !decoded) {
            return decoded;
        }

        const uint64 used = budget.Value().Used() - before;
        const uint64 readerWork = used > validate.Charged() ? used - validate.Charged() : 0;
        if (auto spent = budget.Value().Spend(readerWork); !spent) {
            return spent;
        }
    }

    // 2단계는 Registry 소유 스레드의 트랜잭션 안이다. 열리지 않으면(다른 스레드·문맥, 종료 중)
    // pool· 객체 allocator에 닿기 전에 그 오류로 끝난다.
    ConstructionContext transaction(registry, access);
    if (auto open = transaction.CheckOpen(); !open) {
        return open;
    }

    // 2단계: 기본 생성 값에 적용하고 value와 맞바꾼다.
    StandaloneDecodeContext apply(allocator, registry.Types(), registry.Names(), budget.Value(),
                                  options, records, schema.id, false);
    if (auto interned = apply.InternNames(); !interned) {
        return interned;
    }

    Allocator& objects = registry.ObjectAllocator();
    ValueBlock block;
    auto memory = objects.Allocate(ops.size, ops.alignment, ValueBlockTag);
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    ops.construct(memory.Value(), objects);
    block.ops = &ops;
    block.allocator = &objects;
    block.memory = memory.Value();
    {
        FieldScope scope(apply, field, TypeId{}, nullptr, view);
        if (auto decoded = detail::DecodeStructValue(view, &transaction, ObjectHandle{},
                                                     block.memory, source, apply);
            !decoded) {
            return decoded;
        }
    }

    if (auto finished = apply.Finish(); !finished) {
        return finished;
    }

    // 새 값을 게시하고 옛 값은 block과 함께 소멸한다.
    ops.swap(value, block.memory);
    return Result<void>::Success();
}

Result<TypeLease> AcquireStructRecordType(TypeRegistry& types, Span<const uint8> record) noexcept {
    auto header = ReadRecordHeader(record);
    if (!header) {
        return Result<TypeLease>::Failure(header.Error());
    }

    return types.AcquireValueType(header.Value().schema, TypeKind::Struct);
}

} // namespace rocks
