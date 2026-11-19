#pragma once

#include <AssetRuntime/PackageReader.h>
#include <Object/TypeLease.h>
#include <ObjectSerialization/ObjectGraph.h>

namespace rocks {

class PackageImportResolver;

namespace detail {

// 패키지 로드의 그래프 hook이다(Soft 참조·BulkDataRef 관찰·진단 수집). Prepare의 스레드에서 쓰이고
// RequiredMissing만 소유 스레드(CheckRequired)에서 온다 — 한 번에 한 스레드만 쓴다.
class PackageLoadHooks final : public FieldCodecHooks {
public:
    PackageLoadHooks(Allocator& allocator, bool cooked) noexcept;

    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override;
    Result<void> CollectSoftReference(const SoftReferenceInfo& info) noexcept override;
    Span<const TypeId> ObservedStructs() noexcept override;
    Result<void> ObserveStruct(const FieldCodecFrame& frame, TypeId type, const void* nativeKey,
                               const void* value) noexcept override;

    // 수집한 Soft 참조 쓰임(에셋·EditorOnly·Required)이다.
    struct SoftUse {
        AssetId asset{};
        bool editorOnly = false;
        bool required = false;
    };

    // 관찰한 bulk ref(빈 값 제외)다.
    struct BulkUse {
        uint32 bulkId = 0;
        uint64 rawBytes = 0;
        Sha256Digest sha256{};
    };

    Array<SoftUse> softUses;
    Array<BulkUse> bulkUses;
    // 읽기 전용으로 열어야 하는 진단(모르는 필드·더 새 버전)을 받았는지다.
    bool sawNewer = false;
    // Required 실효 값 누락 수다.
    uint32 missingRequired = 0;

private:
    bool cooked_ = true;
    TypeId observed_[1] = {};
};

} // namespace detail

// 패키지 로드 옵션이다.
struct PackageLoadOptions {
    // 기대 종류·커스텀 버전·엔진 버전이다(types는 PackageLoadState의 TypeRegistry로 채운다).
    PackageReadOptions read;
    // 압축을 푼 bulk raw byte 합의 상한이다(넘으면 CapacityExceeded — 자원 한도라 손상이 아니다).
    uint64 maxBulkRawBytes = uint64{16} * 1024 * 1024 * 1024;
    // 그래프 한도다(기본은 AS 5.1절 상한과 같다).
    ObjectGraphLimits limits;
};

// 패키지 하나의 로드 상태다(Docs/Design/AssetSystem.ko.md 11.2절 단계): 소유 스레드에서 만들고 →
// Prepare(어느 스레드 — 검증·압축 해제·graph 1단계·Dependencies·Bulk 대조) →
// InstantiatePackages(소유 스레드, 한 트랜잭션 — 객체 생성·import 연결·적용·Required·id 설정) →
// 호출자 Commit.
//
// Prepare가 하는 대조(실패는 CorruptPackage / InvalidArgument):
// - Objects 노드의 저장 버전 = Summary 타입 표의 버전.
// - 모든 BulkDataRef(관찰)가 BulkIndex 항목과 같은 rawBytes·SHA-256을 가진다.
// - 모든 BulkIndex entry가 어떤 BulkDataRef에 쓰인다(읽기 전용 열기는 보지 않는다), 빈
//   BulkDataRef(bulkId 0)는 rawBytes·SHA-256도 0이다(AS 5.7절 14 — 이것은 Objects 손상).
// - Dependencies의 Hard·Soft·EditorOnly 항목 = 디코딩 결과(외부 참조 쓰임 + Soft 참조)를
//   writer와 같은 규칙(MergeDependencyUses)으로 합친 것. 에디터 패키지의 읽기 전용 열기는 "디코딩
//   결과 ⊆ Dependencies"만 본다(AS 5.3절). 모든 import의 에셋(outer import만 있는 에셋 포함)은
//   Hard 또는 EditorOnly 의존이어야 하고, Hard 의존 에셋의 대표 에셋 import 타입은 그 의존의
//   expectedType과 같다.
// - 모든 import는 Objects가 가리키거나 뒤 import의 outer다(AS 5.7절 15 — 읽기 전용 열기는 보지
//   않는다).
//
// 사용 예(동기 로드 — 시험·도구):
//   PackageLoadState state(allocator, registry, types, access, options);   // 소유 스레드
//   if (auto prepared = state.Prepare(Move(bytes), &failure); !prepared) { ... }
//   ConstructionContext transaction(registry, access);
//   PackageLoadState* states[] = {&state};
//   ObjectHandle packages[1];
//   if (auto made = InstantiatePackages(transaction, states, nullptr, packages, &failure); !made) {
//       ...
//   }
//   RootRef<Object> roots[1];
//   auto committed = transaction.Commit(packages, roots);
//
// 수명: allocator는 스레드 사이 할당·해제를 지원해야 한다(Prepare가 worker에서 할당한 것을 소유
// 스레드가 쓰고 해제한다). Prepare는 이 allocator만 쓰고 Registry allocator는 쓰지 않는다(그것은
// 소유 스레드 전용이다 — InstantiatePackages만 쓴다). 압축을 푼 bulk 저장소는 복사 없이 Package가
// 이어받으므로 allocator는 이 상태로 만든 Package(Registry)보다 오래 살아야 한다(AS 5.7절 16).
// registry·types는 이 상태보다 오래 살아야 한다. Prepare에 준 file byte는
// 이 상태가 소유한다(그래프 1단계가 이름·필드 view를 빌린다). 이동·복사 금지다.
class PackageLoadState {
public:
    // 소유 스레드에서 만든다(그래프 Stage가 소유 스레드·접근 문맥을 기억한다).
    PackageLoadState(Allocator& allocator, ObjectRegistry& registry, TypeRegistry& types,
                     const ObjectAccessContext& access, const PackageLoadOptions& options) noexcept;
    ~PackageLoadState();
    PackageLoadState(const PackageLoadState&) = delete;
    PackageLoadState& operator=(const PackageLoadState&) = delete;

    // 패키지 byte를 받아 검증·해석한다. 한 번만 부른다(두 번째는 InvalidArgument). 어느
    // 스레드에서나 부를 수 있지만 같은 상태의 다른 호출과 겹치면 안 된다.
    //
    // 오류: ReadPackage·DecodePackageBulk의 오류, 현재 클래스가 없는 export 타입(미등록·클래스가
    // 아닌 타입 id — reader 판정)은 UnsupportedVersion / Unsupported(Summary), 그래프 1단계 오류는
    // 분류해 돌려준다(할당 실패는 그대로, 미등록 타입 NotFound·버전 Unsupported는
    // UnsupportedVersion, 그 밖은 CorruptPackage — 원래 오류는 AssetDiagnostic::error에 남는다),
    // 타입 저장소 조회 실패(Prepare 도중 Shutdown의 ShuttingDown 등)는 진단 None과 그 오류, 위 대조
    // 위반은 CorruptPackage다.
    Result<void> Prepare(Array<uint8>&& file, AssetDiagnostic* failure = nullptr) noexcept;

    bool IsPrepared() const noexcept {
        return prepared_;
    }

    // Prepare가 성공한 뒤에만 유효하다.
    AssetId Id() const noexcept {
        return contents_.header.assetId;
    }

    const PackageContents& Contents() const noexcept {
        return contents_;
    }

    // 에디터 패키지를 읽기 전용으로 열었는지다(reader 판정 + 그래프 1단계의 모르는 필드·더 새
    // 버전).
    bool IsReadOnly() const noexcept {
        return readOnly_;
    }

    // 그래프 Stage다(InstantiatePackages가 쓴다).
    ObjectGraphStage& Stage() noexcept {
        return stage_;
    }

private:
    friend Result<void> InstantiatePackages(ConstructionContext&, Span<PackageLoadState* const>,
                                            PackageImportResolver*, Span<ObjectHandle>,
                                            AssetDiagnostic*) noexcept;

    Result<void> CrossCheck(AssetDiagnostic* failure, const TypeLease& representativeType) noexcept;

    Allocator& allocator_;
    ObjectRegistry& registry_;
    TypeRegistry& types_;
    PackageLoadOptions options_;
    detail::PackageLoadHooks hooks_;
    ObjectGraphStage stage_;
    Array<uint8> file_;
    PackageContents contents_;
    Array<GraphNodeDesc> nodes_;
    Array<TypeId> importTypes_;
    // import i가 그 에셋의 첫 항목이고 그 에셋이 Hard 의존이면 expectedType, 아니면 0이다
    // (CrossCheck가 채운다 — InstantiatePackages가 대상 대표 에셋 타입과 대조한다).
    Array<TypeId> hardTypes_;
    // 압축을 푼 bulk(상태 allocator로 할당 — Package가 복사 없이 이어받는다).
    Array<uint8> bulkBytes_;
    Array<PackageBulkSlot> bulkSlots_;
    bool prepared_ = false;
    bool attempted_ = false;
    bool readOnly_ = false;
    // InstantiatePackages에 한 번 넘겼는지다(성공·실패 모두 — 다시 넘기면 InvalidArgument).
    bool instantiated_ = false;
};

// 이미 공개된 패키지를 AssetId로 찾는다(로더·AssetManager가 구현한다). InstantiatePackages가 같은
// 묶음에 없는 import 대상 패키지를 이것으로 찾는다.
class PackageImportResolver {
public:
    // 생존(공개된) 패키지 handle이다. 없으면 NotFound다.
    virtual Result<ObjectHandle> FindPackage(AssetId asset) noexcept = 0;

protected:
    ~PackageImportResolver() = default;
};

// Prepare를 마친 패키지 묶음(하드 참조 SCC — AS 11.3절)을 transaction 하나에 만든다: 모든 Package
// 생성 → 모든 그래프 CreateObjects → import 해석(같은 묶음은 트랜잭션 안 객체, 밖은 resolver —
// 에셋마다 패키지를 한 번 찾고 outer import 아래 이름 하나씩 푼다, import 수에 비례) → 모든
// ApplyFields → 모든 CheckRequired → id·bulk 설정. 공개(Commit)는 호출자가 한다 — 관찰자 Commit
// 기록의 transaction id를 Commit 전에 읽어 둘 수 있게.
//
// 인자: packages(Prepare 완료, 아직 InstantiatePackages에 넘긴 적 없음, 서로 다른 AssetId),
// resolver(null이면 묶음 밖 import는 MissingDependency), outPackages(크기 = packages, i번째
// Package handle — 공개 전 객체).
//
// 오류: 인자 위반 InvalidArgument(이미 넘긴 상태를 다시 넘긴 경우, 패키지 정책(packageNativeType =
// Package)이 없는 Registry의 트랜잭션 포함 — 상태는 성공했든 실패했든 한 번만 쓴다), resolver가
// 준 handle이 그 에셋의 공개된 Package가 아니면(null·회수됨·Package가 아닌 객체·다른 AssetId) 진단
// None + InvalidArgument, 묶음 밖 import 대상 패키지·객체·Hard 의존
// 대표 에셋이 없으면 MissingDependency / NotFound(related = 그 에셋), 대상 타입이 import 선언
// 타입의 파생이 아니거나 Hard 의존 대표 에셋 타입이 expectedType의 파생이 아니면 TypeMismatch /
// TypeMismatch, import 대상의 타입 조회 실패와
// 트랜잭션·그래프 단계의 오류(할당·스레드·접근 문맥·종료·재진입)는 진단 None과 그 오류, Required
// 실효 값 누락(쿠킹)은 CorruptPackage / InvalidArgument다. 객체를 만든 뒤의 실패는 트랜잭션을
// 그 오류로 고정한다(호출자가 Rollback하거나 소멸자가 되돌린다 — 부분 공개 없음).
Result<void> InstantiatePackages(ConstructionContext& transaction,
                                 Span<PackageLoadState* const> packages,
                                 PackageImportResolver* resolver, Span<ObjectHandle> outPackages,
                                 AssetDiagnostic* failure = nullptr) noexcept;

} // namespace rocks
