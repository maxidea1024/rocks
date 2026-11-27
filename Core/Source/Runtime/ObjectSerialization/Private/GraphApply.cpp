#include "GraphInternal.h"
#include <Core/Text/NamePool.h>

// 그래프 복원 2단계의 생성(2a)·필드 적용(2b — 2단계 문맥)이다(Docs/Design/ObjectFoundations.ko.md
// 5.4절). 단계형 복원(ObjectGraphStage)이 사전 확인(GraphIdentity.cpp)·imports 확인
// (GraphExternal.cpp) 뒤에 부르고, Required 검사(2c)는 GraphRequired.cpp, Commit은 호출자다.
namespace rocks::graph_detail {
namespace {

// 2단계(적용) 문맥이다. 그래프 예산에 접근하지 않는다: 값 reader는 같은 크기 한도·무제한 work로
// 열고, 참조 값을 만든 객체 handle(노드)이나 imports handle(외부 참조)로 바꾸며, 진단·수집·청구는
// 버린다.
class GraphApplyContext final : public FieldDecodeContext {
public:
    GraphApplyContext(Allocator& allocator, const ObjectGraphLoadOptions& options,
                      Array<LoadNode>& nodes, Span<const ObjectHandle> imports) noexcept
        : FieldDecodeContext(options.policy, options.hooks, ReferenceWire), allocator_(allocator),
          limits_(options.limits), nodes_(nodes), imports_(imports) {
        SetRecording(false);
    }

    void BeginNode(LoadNode& node, uint32 index) noexcept {
        SetObject(node.type, index);
        node_ = &node;
        migration_ = 0;
        name_ = 0;
        class_ = 0;
        instanced_ = 0;
    }

    // 노드의 모든 필드를 적용한 뒤 부른다. 1단계가 보관한 struct 마이그레이션·Name 기록·ClassRef
    // 판정·InstancedStruct 판정은 모두 소비되어야 한다(남으면 두 단계의 방문 순서가 어긋난 계약
    // 위반).
    Result<void> EndNode() noexcept {
        if (node_ &&
            (migration_ != node_->structMigrations.Size() || name_ != node_->names.Size() ||
             class_ != node_->classes.Size() || instanced_ != node_->instanced.Size())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<void>::Success();
    }

    // 1단계가 같은 순서로 기록한 Name의 2a 토큰이다. 다시 읽은 표시 문자열이 기록과 다르면(방문
    // 순서 어긋남) InvalidArgument다.
    Result<Name> TakeName(StringView text) noexcept override {
        if (!node_ || name_ >= node_->names.Size() || !(node_->names[name_].text == text)) {
            return Result<Name>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Name>::Success(node_->names[name_++].token);
    }

    // 1단계가 같은 순서로 기록한 ClassRef 판정이다(리플렉션 B9d2 — TypeRegistry를 다시 조회하지
    // 않는다). 다시 읽은 저장 값이 기록과 다르면(방문 순서 어긋남) InvalidArgument다.
    Result<TypeId> TakeClassReference(TypeId stored) noexcept override {
        if (!node_ || class_ >= node_->classes.Size() ||
            !(node_->classes[class_].stored == stored)) {
            return Result<TypeId>::Failure(ErrorCode::InvalidArgument);
        }

        const bool nulled = node_->classes[class_++].nulled;
        return Result<TypeId>::Success(nulled ? TypeId{} : stored);
    }

    // 1단계가 같은 순서로 기록한 InstancedStruct 판정이다(리플렉션 B9j — 기록의 lease를 넘겨받는다,
    // TypeRegistry를 다시 조회하지 않는다). 다시 읽은 저장 struct ID가 기록과 다르면(방문 순서
    // 어긋남) InvalidArgument다.
    Result<TypeLease> TakeInstancedStruct(TypeId stored) noexcept override {
        if (!node_ || instanced_ >= node_->instanced.Size() ||
            !(node_->instanced[instanced_].stored == stored)) {
            return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<TypeLease>::Success(Move(node_->instanced[instanced_++].type));
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

    // 필드 값 안의 Record 내용(Optional<값 struct>의 내부 — 리플렉션 B9e)이다. 1단계와 같은 버전
    // 범위·크기 한도, 무제한 operation·work. 스택이 비면(최상위 필드 — 값 reader가 단독이다) 단독
    // Open, 있으면 맨 위 Record의 OpenNestedRecord다.
    Result<RecordScope> OpenRecordBytes(Span<const uint8> record, TypeId expected,
                                        uint32 minVersion, uint32 maxVersion) noexcept override {
        if (RecordReader* parent = CurrentRecord()) {
            auto opened = parent->OpenNestedRecord(allocator_, record, expected, minVersion,
                                                   maxVersion, limits_.maxTotalFields);
            if (!opened) {
                return Result<RecordScope>::Failure(opened.Error());
            }

            return PushRecord(Move(opened.Value()));
        }

        auto opened = RecordReader::Open(allocator_, record, expected, minVersion, maxVersion,
                                         {Unlimited(), limits_.maxTotalFields});
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
        if (decoded.isNull) {
            return Result<DecodedReference>::Success(decoded);
        }

        // 범위는 1단계가 검사했다. 다르면 원본이 바뀐 계약 위반이다. 외부 참조 i는 2b 확인을 지난
        // imports[i − 1]이다.
        if ((local.Value() & ExternalReferenceBit) != 0) {
            const uint32 external = local.Value() & ~ExternalReferenceBit;
            if (external == 0 || external > imports_.Size()) {
                return Result<DecodedReference>::Failure(ErrorCode::CorruptData);
            }

            // null handle import는 미해결이라 그 참조는 null이다(ObjectFoundations 5.4절).
            decoded.handle = imports_[external - 1];
            decoded.isNull = decoded.handle.IsNull();
            return Result<DecodedReference>::Success(decoded);
        }

        if (local.Value() > nodes_.Size()) {
            return Result<DecodedReference>::Failure(ErrorCode::CorruptData);
        }

        decoded.handle = nodes_[local.Value() - 1].handle;
        return Result<DecodedReference>::Success(decoded);
    }

    // weak 참조는 2a가 만든 노드 handle이다(외부 번호는 1단계가 거부했다 — 다르면 원본이 바뀐 계약
    // 위반).
    Result<DecodedReference> ReadWeakReference(ArchiveReader& reader,
                                               const ReferenceExpectation&) noexcept override {
        auto local = reader.ReadU32();
        if (!local) {
            return Result<DecodedReference>::Failure(local.Error());
        }

        DecodedReference decoded;
        decoded.isNull = local.Value() == 0;
        if (decoded.isNull) {
            return Result<DecodedReference>::Success(decoded);
        }

        if ((local.Value() & ExternalReferenceBit) != 0 || local.Value() > nodes_.Size()) {
            return Result<DecodedReference>::Failure(ErrorCode::CorruptData);
        }

        decoded.handle = nodes_[local.Value() - 1].handle;
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
    Span<const ObjectHandle> imports_;
    LoadNode* node_ = nullptr;
    usize migration_ = 0;
    usize name_ = 0;
    // 다음에 소비할 ClassRef 판정 기록의 위치다(리플렉션 B9d2).
    usize class_ = 0;
    // 다음에 소비할 InstancedStruct 판정 기록의 위치다(리플렉션 B9j).
    usize instanced_ = 0;
};

} // namespace

Result<void> CreateNodes(ConstructionContext& transaction, Array<LoadNode>& nodes,
                         ObjectHandle baseOuter, Array<ObjectHandle>& handles) noexcept {
    handles.Clear();
    if (auto reserved = handles.TryReserve(nodes.Size()); !reserved) {
        return reserved;
    }

    // 노드 표의 정체성 규칙은 1단계와 사전 확인이 끝냈으므로 Create는 할당·슬롯
    // 상한·종료·재진입으로만 실패한다. Outer는 언제나 앞 노드이거나 기준 Outer다.
    for (usize index = 0; index < nodes.Size(); ++index) {
        const uint32 outerIndex = nodes[index].outer;
        const ObjectHandle outer = outerIndex == 0 ? baseOuter : nodes[outerIndex - 1].handle;
        auto created = transaction.Create<Object>(nodes[index].type, outer, nodes[index].name);
        if (!created) {
            return Result<void>::Failure(created.Error());
        }

        nodes[index].handle = created.Value();
        if (!handles.TryPushBack(created.Value())) {
            platform::Fatal("Object graph node handle reservation failed");
        }
    }

    return Result<void>::Success();
}

Result<void> InternNodeNames(Allocator& allocator, NamePool* pool,
                             Array<LoadNode>& nodes) noexcept {
    // pool이 없으면 None이 아닌 값을 둘 곳이 없다 — intern을 시작하기 전에 판정한다(None 값만 있는
    // 그래프는 pool 없이 복원된다).
    usize named = 0;
    for (const LoadNode& node : nodes) {
        for (const StoredName& stored : node.names) {
            if (!IsNoneNameText(stored.text)) {
                ++named;
            }
        }
    }

    if (named == 0) {
        for (LoadNode& node : nodes) {
            for (StoredName& stored : node.names) {
                stored.token = Name{};
            }
        }

        return Result<void>::Success();
    }

    if (pool == nullptr) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 같은 byte의 표시 문자열은 한 번만 intern한다(첫 출현 순 — 결정적). text는 Stage 수명 동안
    // 유효하므로 표의 키로 빌린다.
    HashMap<StringView, Name> interned(allocator);
    if (auto reserved = interned.Reserve(named); !reserved) {
        return reserved;
    }

    for (LoadNode& node : nodes) {
        for (StoredName& stored : node.names) {
            if (IsNoneNameText(stored.text)) {
                stored.token = Name{};
                continue;
            }

            if (const Name* seen = interned.Find(stored.text)) {
                stored.token = *seen;
                continue;
            }

            auto token = pool->TryIntern(stored.text);
            if (!token) {
                return Result<void>::Failure(token.Error());
            }

            if (auto added = interned.Insert(stored.text, token.Value()); !added) {
                return Result<void>::Failure(added.Error());
            }

            stored.token = token.Value();
        }
    }

    return Result<void>::Success();
}

Result<void> ApplyNodeFields(Allocator& allocator, ConstructionContext& transaction,
                             Array<LoadNode>& nodes, const ObjectGraphLoadOptions& options,
                             Span<const ObjectHandle> imports,
                             Array<PendingEventBinding>& pendingEvents) noexcept {
    GraphApplyContext context(allocator, options, nodes, imports);
    for (usize index = 0; index < nodes.Size(); ++index) {
        auto& node = nodes[index];
        const auto& schema = node.lease.Schema();
        context.BeginNode(node, static_cast<uint32>(index));
        for (usize fieldIndex = 0; fieldIndex < node.fields.Size(); ++fieldIndex) {
            const StoredField& stored = node.fields[fieldIndex];
            FieldScope scope(context, schema.fields[stored.schemaIndex], schema.id,
                             node.lease.FieldEnum(stored.schemaIndex),
                             node.lease.FieldStruct(stored.schemaIndex), FieldCodecFrame::NoIndex,
                             node.lease.FieldKeyEnum(stored.schemaIndex));
            // 이벤트 바인딩 기록은 대상 handle을 보류 목록에 쌓는다(Commit 뒤 바인딩 — ED21).
            auto written = stored.events != nullptr
                               ? DecodeEventBindings(stored.view, context, &pendingEvents,
                                                     static_cast<uint32>(index), schema.id,
                                                     stored.schemaIndex, stored.events)
                               : stored.ops->decode(*stored.ops, &transaction, node.handle,
                                                    stored.view, context);
            if (!written) {
                return written;
            }
        }

        if (auto ended = context.EndNode(); !ended) {
            return ended;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::graph_detail
