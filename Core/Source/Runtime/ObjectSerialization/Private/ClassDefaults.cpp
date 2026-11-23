#include "GraphSaveInternal.h"

#include <Core/Encoding/Sha256.h>
#include <Object/ClassRef.h>
#include <Object/TypeRegistry.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <Platform/Fatal.h>

// 클래스 기본값 레코드를 만든다(Docs/Design/RuntimeClassMetadata.ko.md '클래스 기본값 구현 명세 —
// A5a 레코드'). 그래프 저장의 필드 루프(EncodeObjectFields)를 원본 모드 문맥으로 부른다 — 같은
// thunk·같은 순서라 같은 객체의 그래프 저장 Record와 byte가 같다.
namespace rocks {
namespace {

using namespace graph_detail;

// 지문 머리다(Docs/Design/RuntimeClassMetadata.ko.md 'A5a 레코드' 6번).
constexpr char FingerprintDomain[] = {'R', 'o', 'c', 'k', 's', 'C', 'l', 'a', 's',
                                      's', 'D', 'e', 'f', 'a', 'u', 'l', 't', 's'};
// u32 LE byte 수다.
constexpr usize VersionBytes = 4;

// 레코드 전용 hook이다: Soft 수집은 거부용(non-null Soft 기본값 — 경로를 남기고 실패), 치환은 값
// 그대로(기본 구현).
class DefaultsHooks final : public FieldCodecHooks {
public:
    explicit DefaultsHooks(ClassDefaultsDiagnostic& diagnostic) noexcept : diagnostic_(diagnostic) {
    }

    Result<void> CollectSoftReference(const SoftReferenceInfo& info) noexcept override {
        diagnostic_.reason = ClassDefaultsReason::NonNullReference;
        diagnostic_.SetFieldPath(info.path);
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

private:
    ClassDefaultsDiagnostic& diagnostic_;
};

// 레코드 문맥이다(원본 모드 — Registry 밖 임시 인스턴스를 읽는다). 강한·weak 참조는 null만 쓰고
// (그래프 저장과 같은 U32 0), ClassRef는 TypeRegistry로 판정하며, Name 풀이 없다(None만).
class DefaultsEncodeContext final : public FieldEncodeContext {
public:
    DefaultsEncodeContext(Allocator& allocator, TypeRegistry& types, Budget& budget,
                          const FieldEncodePolicy& policy, DefaultsHooks& hooks,
                          const Object& source, TypeId type,
                          ClassDefaultsDiagnostic& diagnostic) noexcept
        : FieldEncodeContext(policy, &hooks, ReferenceWire), types_(types), budget_(budget),
          scratch_(allocator, budget), observation_(allocator, budget, false), source_(source),
          diagnostic_(diagnostic) {
        SetObject(type, 0);
    }

    const Object* RawSource() const noexcept override {
        return &source_;
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

    // ClassRef 값의 클래스 판정이다(복원 1단계와 같다 — 잠금 보호 TypeRegistry::Acquire).
    Result<TypeLease> AcquireClassReference(TypeId type, TypeId base) noexcept override {
        return detail::CheckClassReferenceLease(types_.Acquire(type), base, nullptr);
    }

    // 중첩 Record는 값 한도 안이다(그래프 저장과 같다).
    RecordLimits NestedRecordLimits() const noexcept override {
        RecordLimits limits = budget_.Record();
        limits.archive.maxTotalBytes = budget_.limits.maxValueBytes;
        return limits;
    }

protected:
    Result<void> WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                    const ReferenceExpectation&) noexcept override {
        if (target.IsNull()) {
            return out.WriteU32(0);
        }

        return RejectNonNull();
    }

    Result<void> WriteWeakReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                        const ReferenceExpectation&) noexcept override {
        if (target.IsNull()) {
            return out.WriteU32(0);
        }

        return RejectNonNull();
    }

private:
    // 참조 기본값은 null만이다(생성자는 강한 참조를 세울 수 없지만 handle 값을 직접 넣는 계약
    // 위반을 잡는다).
    Result<void> RejectNonNull() noexcept {
        PathStep steps[MaxPropertyPathSteps];
        diagnostic_.reason = ClassDefaultsReason::NonNullReference;
        diagnostic_.SetFieldPath(BuildPath(steps));
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    TypeRegistry& types_;
    Budget& budget_;
    BudgetAllocator scratch_;
    BudgetAllocator observation_;
    const Object& source_;
    ClassDefaultsDiagnostic& diagnostic_;
};

// 레코드 필드 루프의 lease다(노드 표가 없어 늘 같은 lease).
class LeaseFieldSink final : public ObjectFieldSink {
public:
    explicit LeaseFieldSink(const TypeLease& lease) noexcept : lease_(lease) {
    }

    const TypeLease& Lease() noexcept override {
        return lease_;
    }

private:
    const TypeLease& lease_;
};

// Registry 밖 임시 인스턴스 하나다(저장소 시작 주소를 기억해 그것을 반환한다 — construct가 준
// Object*는 vfptr 등으로 저장소 시작과 다를 수 있다).
class TemporaryInstance {
public:
    TemporaryInstance(Allocator& allocator, const RuntimeTypeOps& ops) noexcept
        : allocator_(allocator), ops_(ops) {
    }

    TemporaryInstance(const TemporaryInstance&) = delete;
    TemporaryInstance& operator=(const TemporaryInstance&) = delete;

    ~TemporaryInstance() {
        if (object_ != nullptr) {
            ops_.destroy(object_);
        }

        if (memory_ != nullptr && !allocator_.Deallocate(memory_)) {
            platform::Fatal("Class defaults instance storage ownership mismatch");
        }
    }

    Result<void> Create() noexcept {
        auto memory = allocator_.Allocate(ops_.size, ops_.alignment);
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        memory_ = memory.Value();
        object_ = ops_.construct(memory_, allocator_);
        return Result<void>::Success();
    }

    const Object& Get() const noexcept {
        return *object_;
    }

private:
    Allocator& allocator_;
    const RuntimeTypeOps& ops_;
    void* memory_ = nullptr;
    Object* object_ = nullptr;
};

// 객체 하나를 policy로 인코딩한 클래스 Record다(그래프 저장 기본 한도 — 객체 하나는 그래프 전체
// 한도 안에 든다).
Result<Array<uint8>> EncodeDefaults(Allocator& scratch, const TypeLease& lease, TypeRegistry& types,
                                    const Object& source, const FieldEncodePolicy& policy,
                                    ClassDefaultsDiagnostic& diagnostic) noexcept {
    using Out = Result<Array<uint8>>;
    auto budget = Budget::Create(scratch, ObjectGraphLimits{});
    if (!budget) {
        return Out::Failure(budget.Error());
    }

    if (auto layout = ValidateLayout(lease, budget.Value()); !layout) {
        return Out::Failure(layout.Error());
    }

    const TypeSchema& schema = lease.Schema();
    if (auto counted = budget.Value().AddFields(schema.fields.Size()); !counted) {
        return Out::Failure(counted.Error());
    }

    const RecordLimits limits = budget.Value().Record();
    DefaultsHooks hooks(diagnostic);
    DefaultsEncodeContext context(scratch, types, budget.Value(), policy, hooks, source, schema.id,
                                  diagnostic);
    RecordBuilder record(scratch, schema.id, schema.version, limits);
    LeaseFieldSink sink(lease);
    usize payloadBytes = 0;
    if (auto encoded = EncodeObjectFields(scratch, nullptr, ObjectAccessContext{}, ObjectHandle{},
                                          budget.Value(), limits.archive.maxTotalBytes, context,
                                          record, sink, payloadBytes);
        !encoded) {
        return Out::Failure(encoded.Error());
    }

    return record.Build();
}

// 두 editor Record에서 처음 다른 필드 id다(같은 스키마 순서로 썼으므로 차례로 비교한다). 열지
// 못하면 빈 id다.
Guid FirstDifferentField(Allocator& scratch, const TypeSchema& schema, Span<const uint8> left,
                         Span<const uint8> right) noexcept {
    auto first = RecordReader::Open(scratch, left, schema.id, schema.version, schema.version);
    auto second = RecordReader::Open(scratch, right, schema.id, schema.version, schema.version);
    if (!first || !second) {
        return Guid{};
    }

    for (;;) {
        auto a = first.Value().Next();
        auto b = second.Value().Next();
        if (!a || !b) {
            return a ? a.Value().id : (b ? b.Value().id : Guid{});
        }

        const FieldView& x = a.Value();
        const FieldView& y = b.Value();
        bool same =
            x.id == y.id && x.wireType == y.wireType && x.payload.Size() == y.payload.Size();
        for (usize index = 0; same && index < x.payload.Size(); ++index) {
            same = x.payload[index] == y.payload[index];
        }

        if (!same) {
            return x.id;
        }
    }
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

Sha256Digest Fingerprint(ClassDefaultsConfiguration configuration,
                         Span<const uint8> record) noexcept {
    Sha256 hasher;
    hasher.Update(Span<const uint8>(reinterpret_cast<const uint8*>(FingerprintDomain),
                                    sizeof(FingerprintDomain)));
    const uint8 configurationByte = static_cast<uint8>(configuration);
    hasher.Update(Span<const uint8>(&configurationByte, 1));
    uint8 version[VersionBytes];
    for (usize index = 0; index < VersionBytes; ++index) {
        version[index] = static_cast<uint8>(ObjectGraphFormatVersion >> (8 * index));
    }

    hasher.Update(Span<const uint8>(version, VersionBytes));
    hasher.Update(record);
    return hasher.Finish();
}

// 만드는 쪽 입력이다(초안 byte는 scratch 배열 — 게시가 복사한다).
struct BuildInput {
    Allocator* scratch = nullptr;
    Array<uint8>* editor = nullptr;
    Array<uint8>* cooked = nullptr;
};

Result<void> BuildDefaults(void* user, const TypeLease& lease, TypeRegistry& types,
                           ClassDefaultsDraft& draft,
                           ClassDefaultsDiagnostic& diagnostic) noexcept {
    BuildInput& input = *static_cast<BuildInput*>(user);
    Allocator& scratch = *input.scratch;
    const RuntimeTypeOps& ops = lease.Ops();
    if (ops.construct == nullptr) {
        diagnostic.reason = ClassDefaultsReason::Abstract;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (!ops.completeFieldLayout) {
        diagnostic.reason = ClassDefaultsReason::IncompleteLayout;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (!lease.HasGeneratedFieldCodecs()) {
        diagnostic.reason = ClassDefaultsReason::ManualFields;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 두 인스턴스를 동시에 산 채로 둔다(주소에 기대는 값이 저장소 재사용으로 가려지지 않게).
    TemporaryInstance first(scratch, ops);
    TemporaryInstance second(scratch, ops);
    if (auto created = first.Create(); !created) {
        return created;
    }

    if (auto created = second.Create(); !created) {
        return created;
    }

    const FieldEncodePolicy editorPolicy{};
    auto editor = EncodeDefaults(scratch, lease, types, first.Get(), editorPolicy, diagnostic);
    if (!editor) {
        return Result<void>::Failure(editor.Error());
    }

    auto again = EncodeDefaults(scratch, lease, types, second.Get(), editorPolicy, diagnostic);
    if (!again) {
        return Result<void>::Failure(again.Error());
    }

    if (!SameBytes(editor.Value().View(), again.Value().View())) {
        diagnostic.reason = ClassDefaultsReason::NonDeterministic;
        const PathStep step{FirstDifferentField(scratch, lease.Schema(), editor.Value().View(),
                                                again.Value().View())};
        diagnostic.SetFieldPath(Span<const PathStep>(&step, 1));
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // cooked 구성 = 쿠킹 저장과 같은 제외(EditorOnly 실효 필드·멤버 — PackageWriter).
    FieldEncodePolicy cookedPolicy;
    cookedPolicy.excludedFlags = static_cast<uint32>(FieldFlags::EditorOnly);
    auto cooked = EncodeDefaults(scratch, lease, types, first.Get(), cookedPolicy, diagnostic);
    if (!cooked) {
        return Result<void>::Failure(cooked.Error());
    }

    *input.editor = Move(editor.Value());
    *input.cooked = Move(cooked.Value());
    draft.editorRecord = input.editor->View();
    draft.cookedRecord = input.cooked->View();
    draft.editorFingerprint = Fingerprint(ClassDefaultsConfiguration::Editor, draft.editorRecord);
    draft.cookedFingerprint = Fingerprint(ClassDefaultsConfiguration::Cooked, draft.cookedRecord);
    return Result<void>::Success();
}

} // namespace

Result<const ClassDefaults*> AcquireClassDefaults(const TypeLease& lease, Allocator& scratch,
                                                  ClassDefaultsDiagnostic* diagnostic) noexcept {
    if (!lease.IsValid()) {
        return Result<const ClassDefaults*>::Failure(ErrorCode::InvalidArgument);
    }

    Array<uint8> editor(scratch);
    Array<uint8> cooked(scratch);
    BuildInput input{&scratch, &editor, &cooked};
    return lease.ResolveClassDefaults(&BuildDefaults, &input, diagnostic);
}

Result<DefaultPayload> FindDefaultPayload(const TypeLease& lease, const ClassDefaults& defaults,
                                          const PropertyPathBuffer& path,
                                          ClassDefaultsConfiguration configuration,
                                          Allocator& scratch) noexcept {
    using Out = Result<DefaultPayload>;
    if (!lease.IsValid() || path.count == 0) {
        return Out::Failure(ErrorCode::InvalidArgument);
    }

    const TypeSchema& schema = lease.Schema();
    auto opened = RecordReader::Open(scratch, defaults.Record(configuration), schema.id,
                                     schema.version, schema.version);
    if (!opened) {
        return Out::Failure(opened.Error());
    }

    RecordReader current = Move(opened.Value());
    // view가 비어 있으면 지금 단계는 클래스의 최상위 필드다.
    StructView view{};
    FieldView found{};
    for (usize step = 0; step < path.count; ++step) {
        const PathStep& at = path.steps[step];
        if (at.selector != PathSelector::None) {
            return Out::Failure(ErrorCode::Unsupported);
        }

        const bool top = view.IsEmpty();
        const usize index = top ? lease.FieldIndex(at.fieldId) : view.FieldIndex(at.fieldId);
        if (index == TypeLease::NoField) {
            return Out::Failure(ErrorCode::NotFound);
        }

        auto field = current.Find(at.fieldId);
        if (!field) {
            return Out::Failure(field.Error());
        }

        found = field.Value();
        if (step + 1 == path.count) {
            break;
        }

        // 다음 단계는 struct 멤버다: 이 값은 단일 struct여야 하고 Atomic이면 멤버로 내려가지
        // 않는다(통째 단위).
        const FieldSchema& schemaField = top ? schema.fields[index] : view.Schema().fields[index];
        const StructView inner = top ? lease.FieldStruct(index) : view.FieldStruct(index);
        if (schemaField.value.kind != ValueKind::Struct || inner.IsEmpty()) {
            return Out::Failure(ErrorCode::TypeMismatch);
        }

        if (inner.IsAtomic()) {
            return Out::Failure(ErrorCode::InvalidArgument);
        }

        const TypeSchema& innerSchema = inner.Schema();
        auto nested = current.OpenRecordField(scratch, found, innerSchema.id, innerSchema.version,
                                              innerSchema.version,
                                              static_cast<uint32>(innerSchema.fields.Size()));
        if (!nested) {
            return Out::Failure(nested.Error());
        }

        current = Move(nested.Value());
        view = inner;
    }

    return Out::Success(DefaultPayload{found.wireType, found.payload});
}

Result<void> ForEachClassDefaults(TypeRegistry& types, Allocator& scratch,
                                  ClassDefaultsVisitor visitor, void* user) noexcept {
    if (visitor == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Array<TypeVersionEntry> listed(scratch);
    if (auto copied = types.ListTypeVersions(listed); !copied) {
        return copied;
    }

    for (const TypeVersionEntry& entry : listed) {
        if (entry.kind != TypeKind::Class) {
            continue;
        }

        auto lease = types.Acquire(entry.type);
        if (!lease) {
            // 목록을 복사한 뒤 해제된 타입은 건너뛴다.
            if (lease.Error() == ErrorCode::NotFound) {
                continue;
            }

            return Result<void>::Failure(lease.Error());
        }

        if (lease.Value().Ops().construct == nullptr) {
            continue;
        }

        ClassDefaultsDiagnostic diagnostic;
        diagnostic.type = entry.type;
        auto defaults = AcquireClassDefaults(lease.Value(), scratch, &diagnostic);
        if (auto visited =
                visitor(user, entry.type, defaults ? defaults.Value() : nullptr, diagnostic);
            !visited) {
            return visited;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
