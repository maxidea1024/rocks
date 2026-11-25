#pragma once

#include <Core/Containers/HashMap.h>
#include <Object/GeneratedFieldCodec.h>
#include <ObjectSerialization/ObjectGraph.h>

namespace rocks::graph_detail {

// graph v2 형식(Docs/Design/ObjectFoundations.ko.md 5.1절)이다. header는 magic(8) · formatVersion
// u32 · flags u32 · nodeCount u32 · externalCount u32의 24 byte이고, 그 뒤에 [NodeTable] nodeCount
// × {Guid type, u32 outer, String name}, nodeCount × Bytes(Record)가 오며 남는 byte가 없다.
inline constexpr uint8 Magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
inline constexpr uint32 FormatVersion = ObjectGraphFormatVersion;
inline constexpr usize HeaderBytes = 24;
// flags 비트다. BaseOuter는 NodeTable과만 쓰고 outer 0 노드의 Outer가 기준 Outer임을 뜻한다.
inline constexpr uint32 NodeTableFlag = 1;
inline constexpr uint32 BaseOuterFlag = 2;
inline constexpr uint32 KnownFlags = NodeTableFlag | BaseOuterFlag;
// NodeTable 항목 하나의 고정 byte(Guid 16 + outer 4 + 이름 길이 8 — 이름 byte는 따로)다.
inline constexpr usize NodeEntryBytes = 28;
// Record 하나를 감싼 Bytes의 길이 prefix(8)와 Record header(44)다(노드마다 최소 byte).
inline constexpr usize RecordPrefixBytes = 8;
inline constexpr usize RecordHeaderBytes = 44;
// 강한 참조 wire는 U32다: 0 = null, 1..nodeCount = 노드(local ID), ExternalReferenceBit | i(1 ≤ i ≤
// externalCount) = 외부 참조 i. 고정 4 byte라 외부 번호 재배정이 제자리 덮어쓰기다(5.3절).
inline constexpr uint32 ReferenceWire = static_cast<uint32>(WireType::U32);
inline constexpr uint32 ExternalReferenceBit = 0x80000000u;
// local ID가 외부 참조 비트와 겹치지 않는 노드 수 상한이다(maxObjects가 넘으면 옵션 오류
// InvalidArgument).
inline constexpr uint32 MaxNodeCount = ExternalReferenceBit - 1;
// 외부 번호가 외부 참조 비트와 겹치지 않는 외부 참조 수 상한이다(maxExternalReferences가 넘으면
// 옵션 오류 InvalidArgument).
inline constexpr uint32 MaxExternalCount = ExternalReferenceBit - 1;
// header의 externalCount 칸 위치다(AssignExternalIndices가 덮어쓴다).
inline constexpr usize ExternalCountOffset = 20;
// 2a의 Name intern 하나에 1단계가 미리 청구하는 상수 몫이다(표시 문자열 byte에 더한다 —
// Docs/Design/ReflectionExtensions.ko.md 4.1절). 근거: 2a는 값마다 중복 표 조회(byte 해시·비교)를
// 하고 서로 다른 표시마다 TryIntern(gate 2–3번, base 해시, bucket 사슬 훑기, 새 entry면 할당) 한
// 번을 한다. 한계: NamePool은 64 bucket 연결 목록이라 사슬 길이가 entry 수 / 64에 비례하고(기본
// maxEntries 65,536이면 약 1,024) 그 훑기는 이 상수에 들지 않는다 — "1단계 청구가 2a 몫을 덮는다"는
// pool 크기에 비례하는 그 배수 안에서만 성립한다(pool 자체는 entry·storage 한도로 묶인다).
inline constexpr uint64 NameInternWork = 16;

// 그래프 하나의 누적 작업량 예산이다. ArchiveBudget 하나를 모든 Record 트리(값 reader 포함)와
// 공유하므로 그래프 청구와 reader 청구가 같은
// 누계다(Docs/Design/ReflectionFoundations.ko.md 6.4절).
class Budget {
public:
    static Result<Budget> Create(Allocator& allocator, ObjectGraphLimits limits) noexcept {
        auto shared = SharedPtr<ArchiveBudget>::TryMake(allocator, limits.maxWork, limits.maxWork);
        if (!shared) {
            return Result<Budget>::Failure(shared.Error());
        }

        return Result<Budget>::Success(Budget(limits, Move(shared.Value())));
    }

    Result<void> Spend(uint64 count) noexcept {
        return shared_->Spend(count);
    }

    uint64 Used() const noexcept {
        return shared_->WorkUsed();
    }

    Result<void> AddFields(usize count) noexcept {
        if (count > limits.maxTotalFields - fields) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        fields += static_cast<uint32>(count);
        return Result<void>::Success();
    }

    // 배열 원소 누계(maxTotalElements)에 더한다(넘으면 CapacityExceeded).
    Result<void> AddElements(usize count) noexcept {
        if (count > limits.maxTotalElements - elements) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        elements += static_cast<uint32>(count);
        return Result<void>::Success();
    }

    // 공유 예산을 쓰지 않는 writer·envelope reader의 한도다(남은 work의 스냅샷). 쓴 양은 호출자가
    // WorkUsed로 청구한다.
    ArchiveLimits Archive(bool outer = false) const noexcept {
        ArchiveLimits result;
        result.maxTotalBytes = limits.maxBytes;
        result.maxValueBytes = outer ? limits.maxBytes : limits.maxValueBytes;
        result.maxWork = limits.maxWork > Used() ? limits.maxWork - Used() : 0;
        result.maxOperations = result.maxWork;
        return result;
    }

    // Record 하나의 한도다. 필드 수 한도는 남은 그래프 누계다.
    RecordLimits Record() const noexcept {
        return {Archive(), RemainingFields()};
    }

    uint32 RemainingFields() const noexcept {
        return limits.maxTotalFields > fields ? limits.maxTotalFields - fields : 0;
    }

    uint32 RemainingElements() const noexcept {
        return limits.maxTotalElements > elements ? limits.maxTotalElements - elements : 0;
    }

    SharedPtr<ArchiveBudget> Shared() const noexcept {
        return shared_;
    }

    ObjectGraphLimits limits;
    uint32 fields = 0;
    uint32 elements = 0;
    usize migrationBytes = 0;

private:
    Budget(ObjectGraphLimits value, SharedPtr<ArchiveBudget> shared) noexcept
        : limits(value), shared_(Move(shared)) {
    }

    SharedPtr<ArchiveBudget> shared_;
};

// 값 임시 저장 할당이 크기 한도와 예산을 지나게 한다(codec 문맥의 Scratch — 단일 할당이
// maxValueBytes 이하, singleLimit 거짓이면 단일 할당 한도 없이 work 예산만 — 저장 문맥의
// ObservationScratch. 로드 1단계의 관찰 할당은 그 문맥의 Charge로 따로 청구한다). 이 adapter를
// 빌리는 값은 그 호출 안에서 파괴하며 결과·노드에 adapter 주소가 탈출하지 않는다.
class BudgetAllocator final : public Allocator {
public:
    BudgetAllocator(Allocator& allocator, Budget& budget, bool singleLimit = true) noexcept
        : allocator_(allocator), budget_(budget), singleLimit_(singleLimit) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        const usize maximum = budget_.limits.maxValueBytes;
        if (singleLimit_ && bytes != 0 && bytes - 1 > maximum) {
            return Result<void*>::Failure(ErrorCode::CapacityExceeded);
        }

        auto spent = budget_.Spend(bytes);
        if (!spent) {
            return Result<void*>::Failure(spent.Error());
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
    Budget& budget_;
    bool singleLimit_;
};

// struct 원소 배열(RecordList)의 로드 목록 한도다(Docs/Design/ReflectionFoundations.ko.md 5.6절).
// 저장 codec(EncodeArrayField)과 같은 함수 detail::ArrayRecordListLimits로 만든다(항목 수
// MaxArrayElements, payload maxValueBytes, 항목 하나 그 − 12 — 저장의 NestedRecordLimits 전체
// 크기도 maxValueBytes라 같은 값이다). 1단계는 여기에 남은 원소 누계를 항목 수 한도로 더 건다.
inline RecordListLimits ArrayLoadLimits(const ObjectGraphLimits& limits) noexcept {
    return detail::ArrayRecordListLimits(limits.maxValueBytes);
}

// ObjectHandle 전체(registry domain·slot·generation)를 키로 쓰는 해시다(저장 쪽 handle → local ID
// 캐시 — 다른 Registry의 handle이 캐시로 domain 검사를 건너뛰지 않게).
struct HandleHash {
    uint64 operator()(const ObjectHandle& handle) const noexcept {
        return MixHash(handle.registry ^
                       MixHash((uint64{handle.slot} << 32) | uint64{handle.generation}));
    }
};

// 필드 codec 함수표를 찾는다(GUID별 1:1, encode·decode가 있어야 한다 — 없으면 Unsupported).
Result<const RuntimeFieldOps*> FindOps(const TypeLease&, Guid, Budget&) noexcept;
// 그래프에 저장할 수 있는 완전한 필드 배치인지 본다.
Result<void> ValidateLayout(const TypeLease&, Budget&) noexcept;

// 1단계가 보관하는 필드 하나다: 스키마 위치, 등록 함수표(node.lease가 보유), 원본·마이그레이션
// 결과를 빌리는 FieldView.
struct StoredField {
    usize schemaIndex = 0;
    const RuntimeFieldOps* ops = nullptr;
    FieldView view;
};

// 1단계가 모은 참조 하나다. target은 저장 값 그대로다 — local ID(1부터) 또는 외부 참조
// ExternalReferenceBit | i(null은 기록하지 않는다 — 배열은 non-null 원소마다 하나). expectation은
// LoadNode::expectations의 위치(필드 안 서로 다른 기대마다 하나 — 원소마다 복제하지 않는다). weak는
// weak 참조(리플렉션 B9d1 — 늘 노드 token이다)이고 기대 형식 검사는 강한 참조와 같지만 도달 검사가
// 따라가지 않는다(Docs/Design/ReflectionExtensions.ko.md 5.1절).
struct ReferenceToken {
    uint32 target = 0;
    uint32 expectation = 0;
    bool weak = false;
};

// 외부 참조 token이면 그 번호 i(1부터)를, 노드 token이면 0을 돌려준다.
inline uint32 ExternalIndexOf(const ReferenceToken& token) noexcept {
    return (token.target & ExternalReferenceBit) != 0 ? token.target & ~ExternalReferenceBit : 0;
}

// 1단계가 기록한 (외부 참조 i, 기대 형식) 쌍 하나다(서로 다른 쌍마다 하나 — 2b가 실제 대상으로
// 판정한다).
struct ImportExpectation {
    uint32 externalIndex = 0;
    ReferenceExpectation expectation;
};

// 1단계가 기록한 Name 값 하나다(방문 순서 — Docs/Design/ReflectionExtensions.ko.md 4.1절). text는
// 내용 규칙을 지난 표시 문자열이며 복원 입력·node.migrated·struct 마이그레이션 결과를 빌린다(Stage
// 수명 동안 유효). token은 2a(InternNodeNames)가 대상 Registry의 pool로 intern한 결과이고(None
// 문자열은 기본 None) 2단계가 같은 순서로 소비한다.
struct StoredName {
    StringView text;
    Name token{};
};

// 1단계가 기록한 ClassRef 판정 하나다(null이 아닌 저장 값의 방문 순서 — Docs/Design/
// ReflectionExtensions.ko.md 5.2절). nulled는 미등록 클래스를 decode 정책 ReportAndNull로 null로
// 바꿨다는 뜻이다. 2단계가 같은 순서로 stored를 대조해 판정 뒤 값을 쓴다(TypeRegistry 재조회 없음).
struct StoredClassReference {
    TypeId stored{};
    bool nulled = false;
};

// 1단계가 기록한 InstancedStruct 판정 하나다(비어 있지 않은 저장 값의 방문 순서 — 리플렉션 B9j,
// Docs/Design/ReflectionExtensions.ko.md 11절). type은 담긴 struct의 lease이고 비어 있으면 미등록
// struct를 decode 정책 ReportAndClear로 비웠다는 뜻이다. 2단계가 같은 순서로 stored를 대조해
// lease를 값으로 옮긴다(TypeRegistry 재조회 없음 — 2단계는 할당 말고 실패하지 않는다).
struct StoredInstancedStruct {
    TypeId stored{};
    TypeLease type;
};

// 1단계가 보관한 struct 마이그레이션 결과 하나다(발생 순서). 2단계가 같은 순서로 (type, from)을
// 대조해 쓴다. bytes의 heap 블록은 Array 이동에도 그대로라 view가 유효하다.
struct StoredStructMigration {
    explicit StoredStructMigration(Allocator& allocator) noexcept : bytes(allocator) {
    }

    TypeId type{};
    uint32 from = 0;
    Array<uint8> bytes;
};

// 1단계가 노드 하나에 보관하는 것이다. type·outer·name은 노드 표(NodeTable)에서, version은 Record
// header에서 온다. name은 원본 byte를 빌린다.
struct LoadNode {
    explicit LoadNode(Allocator& allocator) noexcept
        : migrated(allocator), fields(allocator), expectations(allocator), tokens(allocator),
          structMigrations(allocator), names(allocator), classes(allocator), instanced(allocator) {
    }

    TypeId type{};
    // 0 = 그래프 밖(BaseOuter면 기준 Outer, 아니면 Outer 없음), 1..자기 번호 − 1 = 노드.
    uint32 outer = 0;
    StringView name{};
    uint32 version = 0;
    Span<const uint8> source;
    TypeLease lease;
    Array<uint8> migrated;
    Array<StoredField> fields;
    Array<ReferenceExpectation> expectations;
    Array<ReferenceToken> tokens;
    Array<StoredStructMigration> structMigrations;
    Array<StoredName> names;
    Array<StoredClassReference> classes;
    Array<StoredInstancedStruct> instanced;
    ObjectHandle handle{};
};

// graph v2 envelope를 읽은 결과다. 노드 표(NodeTable)가 없으면 노드의 type·outer·name은 비어 있다.
struct Envelope {
    explicit Envelope(Allocator& allocator) noexcept : nodes(allocator) {
    }

    uint32 flags = 0;
    uint32 externalCount = 0;
    Array<LoadNode> nodes;
};

// header·NodeTable·Record 경계를 읽는다(정의·순서와 오류는 GraphEnvelope.cpp).
Result<Envelope> ReadEnvelope(Allocator& allocator, Span<const uint8> source,
                              Budget& budget) noexcept;

// 단계형 복원(ObjectGraphStage)이 Validate에서 만들어 이후 단계까지 쥐는 상태다. 노드(필드 색인·
// 참조 token·노드 타입 lease·마이그레이션 결과)와 그래프 예산, 참조 형식 검사에 쓴 기대 타입
// lease, 외부 참조의 선언 타입 lease·쓰임·기록한 기대 형식 쌍을 소유하고 입력 byte·노드 이름은
// 빌린다. 노드 원소는 만든 뒤 옮기지 않는다(FieldView가 node.migrated를 가리킨다).
struct StageData {
    StageData(Allocator& allocator, Budget&& graphBudget, Envelope&& graphEnvelope) noexcept
        : budget(Move(graphBudget)), envelope(Move(graphEnvelope)), expectedLeases(allocator),
          importLeases(allocator), externalUses(allocator), importChecks(allocator) {
    }

    Budget budget;
    Envelope envelope;
    HashMap<TypeId, TypeLease> expectedLeases;
    // 외부 참조 i의 선언 타입(importTypes) lease와 쓰임이다(i − 1 위치, 크기 = externalCount).
    Array<TypeLease> importLeases;
    Array<GraphExternalUse> externalUses;
    Array<ImportExpectation> importChecks;
};

// ObjectGraphStage 내부 통로다(편의 복원의 도달 검사가 1단계 결과를 읽는다).
struct StageAccess {
    static StageData* Data(ObjectGraphStage& stage) noexcept {
        return stage.data_.Get();
    }
};

// 복원 옵션 검사다: 값을 바꾸거나 모르는 내용을 건너뛰는 정책(ReportAndDefault·
// LoadNewerSkippingUnknown)에 hooks가 없거나 maxObjects가 MaxNodeCount를,
// maxExternalReferences가 MaxExternalCount를 넘으면 InvalidArgument다.
Result<void> CheckLoadOptions(const ObjectGraphLoadOptions& options) noexcept;

// 1단계(GraphLoad.cpp): envelope·노드 출처·노드 정체성·Record·값·참조를 검증하고 2단계 몫의
// 작업량을 미리 청구한 상태를 돌려준다. ObjectRegistry에 닿지 않는다(policy는 Registry 정책의
// 사본이다). 순서와 오류는 ObjectGraphStage::Validate 주석이다.
Result<UniquePtr<StageData>> ValidateGraph(Allocator& allocator, TypeRegistry& types,
                                           const RegistryPolicy& policy,
                                           const ObjectGraphLoadOptions& options,
                                           const ObjectGraphInput& input) noexcept;

// 1단계 참조 형식 검사다(GraphReferences.cpp — 모든 노드를 해석한 뒤): importTypes의 선언 타입
// lease를 data.importLeases에 연다(미등록 NotFound·클래스 아님 TypeMismatch) → token마다 노드
// 대상은 기대 형식(TypeMismatch, 기대 타입 미등록 NotFound), 외부 참조는 선언 타입과 기대 타입의
// 양립(TypeMismatch)을 보고 서로 다른 (import, 기대 형식) 쌍을 data.importChecks에 기록한다. 쓴
// 기대 타입 lease는 data.expectedLeases에 남는다. 조회·삽입마다 작업량 1을 그래프 예산에 청구한다.
Result<void> ValidateReferenceTypes(Allocator& allocator, TypeRegistry& types, StageData& data,
                                    Span<const TypeId> importTypes) noexcept;

// 1단계 노드 정체성 검사다(GraphIdentity.cpp, Docs/Design/ObjectFoundations.ko.md 5.4절). 노드를
// local ID 순서로 Check에 넘기면 앞 노드들로 계산한 Outer 요약(outer 0이면 hasBase ? base : Outer
// 없음 — 앞서 나온 outer 0 형제의 대표 자식·이름 있는 Inner 여부를 더한다)으로
// detail::CheckNewObjectIdentity를 판정하고, 그래프 안 형제 유일성((outer, 비교 키) — 중복은 파일
// 손상이라 InvalidArgument)을 본다. 그래서 2a의 실제 Create는 노드 표 때문에 실패하지 않는다.
class NodeIdentityCheck {
public:
    NodeIdentityCheck(Allocator& allocator, const RegistryPolicy& policy, bool hasBase,
                      const OuterSummary& base) noexcept;

    // 노드 count개분 상태를 미리 잡는다(할당 실패 OutOfMemory).
    Result<void> Reserve(usize count) noexcept;

    // 다음 노드(local ID = 지금까지 Check한 수 + 1)를 판정하고 기록한다. node.lease·outer·name이
    // 정해져 있어야 하고 outer는 이미 범위를 검사한 값이다. 오류: CheckNewObjectIdentity의 것
    // (WorldOnly Unsupported, 이름 형식·패키지·빈 이름·대표 자식 InvalidArgument, 경로 길이
    // CapacityExceeded, NFC 판정 할당 OutOfMemory) → 작업량(이름 byte + 1, 같은 해시 비교마다 키
    // byte + 1) CapacityExceeded → 형제 중복 InvalidArgument. 비교 키·표 할당 실패는 OutOfMemory,
    // 키 저장소 누계가 uint32를 넘으면 CapacityExceeded다.
    Result<void> Check(const LoadNode& node, Budget& budget) noexcept;

private:
    // 노드 하나를 Outer로 볼 때의 요약 재료와 형제 색인 사슬이다.
    struct NodeShape {
        uint32 outer = 0;
        uint32 pathBytes = 0;
        uint32 keyOffset = 0;
        uint32 keyBytes = 0;
        uint32 nextSameHash = 0; // 같은 해시의 앞 노드(local ID, 0 = 없음)
        bool isOutermost = false;
        bool outermostIsPackage = false;
        bool hasRepresentative = false;
        bool hasNamedInners = false;
        // 이 노드의 타입이다(자식 노드의 Within 판정 — 리플렉션 B9h2, 노드 배열은 판정 동안 바뀌지
        // 않는다).
        const TypeLease* type = nullptr;
    };

    // outer 값의 요약이다(Outer 없음이면 nullptr, 아니면 summary를 채워 그 주소).
    const OuterSummary* OuterOf(uint32 outer, OuterSummary& summary) const noexcept;

    Allocator& allocator_;
    const RegistryPolicy& policy_;
    bool hasBase_;
    OuterSummary base_;
    Array<NodeShape> shapes_;
    Array<char> keys_;
    HashMap<uint64, uint32> heads_;
};

// 2a 사전 확인이다(GraphIdentity.cpp, 5.4절): hasBase와 baseOuter 유무가 다르면 InvalidArgument →
// hasBase면 transaction.DescribeOuter(baseOuter)(트랜잭션 상태·접근·StaleHandle)의 isOutermost·
// outermostIsPackage·pathBytes가 base와 다르면 InvalidArgument(handle·Inner 유무는 비교하지 않는다)
// → 기준 Outer 노드마다 실제 요약(앞선 기준 Outer 노드를 더한 것)으로 CheckNewObjectIdentity 재판정
// → 기준 Outer 노드 이름의 Registry 점유(대표 노드는 빈 비교 키 — AlreadyExists), hasBase가 아니면
// outer 0 이름 있는 노드의 최상위 이름 공간 점유(AlreadyExists). 조회 실패는 IsNameTaken의 오류다.
Result<void> CheckCreatePreconditions(Allocator& allocator, ConstructionContext& transaction,
                                      const RegistryPolicy& policy, const Array<LoadNode>& nodes,
                                      bool hasBase, const OuterSummary& base,
                                      ObjectHandle baseOuter) noexcept;

// 2a 생성이다(GraphApply.cpp): 노드를 local ID 순으로 노드 표의 Outer(outer 0은 baseOuter —
// null이면 Outer 없음)·이름으로 만들고 node.handle과 handles(비운 뒤 채운다)에 둔다. 실패는
// Create의 오류이고 만든 객체는 트랜잭션에 남는다(호출자 Rollback).
Result<void> CreateNodes(ConstructionContext& transaction, Array<LoadNode>& nodes,
                         ObjectHandle baseOuter, Array<ObjectHandle>& handles) noexcept;

// 2a의 Name intern이다(GraphApply.cpp, Docs/Design/ReflectionExtensions.ko.md 4.1절 — 사전 확인
// 뒤·생성 전): 1단계가 기록한 Name 값을 노드 순서·방문 순서대로 pool(대상 Registry의
// RegistryOptions::names)에 TryIntern해 StoredName::token에 둔다. 그래프 안에서 byte가 같은 표시
// 문자열은 처음 나온 것만 intern하고 나머지는 그 토큰을 쓴다(같은 byte의 TryIntern은 같은 토큰이다
// — 순서는 첫 출현 순이라 결정적이다). None 문자열(IsNoneNameText)은 intern하지 않고 기본 None이다.
// None이 아닌 값이 있는데 pool이 없으면 Unsupported(아무것도 바꾸지 않는다), 중복 표 할당 실패는
// OutOfMemory, TryIntern의 실패(CapacityExceeded·OutOfMemory·ShuttingDown 등)는 그 오류로 끝난다 —
// 손상이 아니라 pool 한도이고, 이미 intern한 entry는 pool에 남는다(pool 수명 자원 — 다시 쓰인다).
// 작업량은 1단계가 Name마다 byte + NameInternWork로 미리 청구했다(한계는 NameInternWork 주석).
// TryIntern은 pool allocator callback(사용자 코드)을 부른다 — callback이 Registry에 재진입해 이름을
// 점유하면 뒤의 Create가 AlreadyExists로 실패하고 트랜잭션이 고정된다(손상은 없다 — 계약 위반).
Result<void> InternNodeNames(Allocator& allocator, NamePool* pool, Array<LoadNode>& nodes) noexcept;

// 2b 확인이다(GraphExternal.cpp, 5.4절 — 쓰기 전): imports 수가 선언 타입 수와 다르면
// InvalidArgument → import마다 transaction.AcquireType(생존·이 트랜잭션 객체 — StaleHandle 등) →
// 실제 타입이 선언 타입의 native 파생이 아니면 InvalidArgument → 기록한 (import, 기대 형식) 쌍마다
// 실제 대상이 기대를 만족하지 않으면 TypeMismatch. 확인 표 할당 실패는 OutOfMemory다.
Result<void> CheckImports(Allocator& allocator, ConstructionContext& transaction,
                          const StageData& data, Span<const ObjectHandle> imports) noexcept;

// CheckImports의 판정 부분이다(GraphExternal.cpp): actual(import마다 실제 타입 lease — i − 1
// 위치)의 수가 선언 타입 수와 다르면 InvalidArgument → 실제 타입이 선언 타입의 native 파생이 아니면
// InvalidArgument → 기록한 (import, 기대 형식) 쌍마다 실제 대상이 기대를 만족하지 않으면
// TypeMismatch. 편의 복원은 imports의 실제 타입을 이미 알므로 생성자 전에 이것으로 판정한다.
Result<void> CheckImportTargets(const StageData& data, Span<const TypeLease> actual) noexcept;

// 2b 적용이다(GraphApply.cpp): 1단계가 보관한 필드를 staged 객체에 쓴다(그래프 예산에 접근하지
// 않는다 — 노드 참조는 노드 handle로, 외부 참조 i는 imports[i − 1]로, ClassRef 값은 1단계 판정
// 기록으로, InstancedStruct 값은 1단계 판정 기록의 lease를 값으로 옮겨 바꾼다 — 그래서 한 번만
// 부른다). 실패는 쓰기 codec·Registry의 오류이고 호출자가 트랜잭션을 버린다.
Result<void> ApplyNodeFields(Allocator& allocator, ConstructionContext& transaction,
                             Array<LoadNode>& nodes, const ObjectGraphLoadOptions& options,
                             Span<const ObjectHandle> imports) noexcept;

// 적용을 마친 staged 노드들의 Required 실효 값을 진단한다(Docs/Design/ObjectFoundations.ko.md 4절
// — 2c 단계, GraphRequired.cpp). Required를 품은 최상위 필드(필드 자신이 Required이거나 struct
// 필드·struct 원소 배열의 struct가 ContainsRequired — detail::CarriesRequired)만 encode 경로의
// 검사 모드로 staged 객체에서 읽고 출력은 버린다(struct 값은 Required를 품지 않는 멤버를 쓰지
// 않는다). null인 Required 위치마다 options.hooks->Report(RequiredMissing)가 한 번 온다
// (거부하지 않는다). hooks가 없으면 진단을 받을 곳이 없으므로 아무것도 하지 않는다. 작업량·크기
// 한도는 걸지 않는다(파일 값은 1단계가 한도로 검증했고 생성자 값은 신뢰 상태다). writer는 파일에
// 있는 Required를 품은 가장 큰 필드의 payload로 미리 잡는다. 실패는 hook 실패·할당·readStaged 없는
// 수동 Required 필드(Unsupported)·Required 위치 자체의 저장 불가 값(예: 생성자가 둔 Required Soft의
// 규칙 위반, MaxArrayElements를 넘는 Required 배열)·계약 위반뿐이며 호출자가 rollback한다. 검사
// 동안 transaction에 검사 중 표시가 선다(그 트랜잭션의 쓰기·GetForWrite·Commit Busy).
Result<void> CheckRequiredFields(Allocator& allocator, ConstructionContext& transaction,
                                 const Array<LoadNode>& nodes,
                                 const ObjectGraphLoadOptions& options) noexcept;

// 편의 복원의 도달 검사다(GraphReachability.cpp).
Result<void> CheckReachable(Allocator& allocator, const Array<LoadNode>& nodes, bool packagePolicy,
                            bool hasBase, Budget& budget) noexcept;

} // namespace rocks::graph_detail
