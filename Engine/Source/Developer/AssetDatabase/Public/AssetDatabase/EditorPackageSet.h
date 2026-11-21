#pragma once

#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/FilePackageBulkStore.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/PackageLoad.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/ReferenceObserver.h>
#include <Platform/File.h>

// 에디터가 연 패키지 집합이다(Docs/Design/AssetSystem.ko.md 7.1절 3·15 — A2c2a 열기, A2c2b 저장).
// 패키지 정책 에셋 Registry 하나와 그 관찰자, bulk 파일 핸들 캐시를 소유하고, 색인(AssetIndex)으로
// 찾은 에디터 패키지를 Hard·EditorOnly 의존 닫힘째 한 트랜잭션으로 연다. bulk는 지연 읽기(파일
// 저장소)다. UE 에디터의 패키지 로드(LoadPackage + FLinkerLoad의 누락 import 경고)에 대응한다.
//
// 누락 의존은 열기를 막지 않는다: 의존 패키지가 색인에 없거나 읽을 수 없으면 그 패키지를 빼고,
// 그것을 가리키는 참조는 null로 이은 뒤 경고를 남긴다. 그런 패키지는 미해결 import를 기록하고
// 저장(A2c2b)은 명시 선택 없이는 거부한다(대상 id를 잃지 않게).
//
// 저장(5.5절)은 임시 파일에 쓰고 다시 읽어 검증한 뒤 대상과 교체한다. 편집은 MarkDirty로 알리고
// 저장은 시작한 세대가 그대로일 때만 dirty를 지운다. 새 패키지는 Create로 만든다(첫 저장이 파일을
// 만든다).
//
// 사용 예:
//   EditorPackageSetConfig config;
//   config.types = &types;
//   config.files = &platform::PhysicalFileSystem();
//   config.layout = &layout;
//   config.index = &index;            // Rebuild를 마친 색인
//   EditorPackageSet set(allocator, config);
//   if (auto ready = set.Initialize(); !ready) { ... }
//   Array<EditorWarning> warnings(allocator);
//   AssetDiagnostic failure;
//   auto hero = set.Open(heroId, &warnings, &failure);
//   const EditorPackageInfo* info = set.Find(heroId);   // 파일 이름·지문·읽기 전용·미해결 import
//   ... 객체를 고친다 ...
//   set.MarkDirty(heroId);
//   AssetDatabaseLock lock;
//   lock.Acquire(layout, 0);
//   set.Save(heroId, lock, EditorSaveOptions{}, &warnings, &failure);
namespace rocks {

struct EditorPackageSetConfig {
    TypeRegistry* types = nullptr;
    platform::FileSystem* files = nullptr;
    // 프로젝트 배치와 색인이다(빌림 — 집합보다 오래 산다). 색인은 Open이 id·경로 → 파일 이름에
    // 쓴다.
    const ProjectLayout* layout = nullptr;
    // 저장이 그 파일 하나를 다시 읽어 갱신한다(Refresh).
    AssetIndex* index = nullptr;
    // 저장의 Tags·Build 의존을 만드는 타입 hook 표다(빌림 — 저장에는 필수: null이면 Save가
    // InvalidArgument. 태그가 필요 없는 host도 빈 표를 넘긴다).
    const AssetTypeHookTable* hooks = nullptr;
    // 저장 Summary의 savedBy다(빈 값이면 build::CurrentEngineVersion() — 시험은 고정 값).
    EngineVersion writeEngineVersion{};
    // 읽기 옵션이다(customVersions·engineVersion — expectedKind는 Editor, types는 위 것으로
    // 채운다).
    PackageReadOptions read;
    // 에셋 Registry의 문맥 id와 설정이다(policy·referenceObserver는 집합이 정한다).
    ContextId registryContext{};
    RegistryOptions registryOptions;
    usize observerMaxKeys = ReferenceObserver::DefaultMaxKeys;
    // bulk 파일 핸들 캐시 용량이다(0이면 InvalidArgument).
    usize fileCacheCapacity = DefaultPackageFileCacheCapacity;
};

// 열기·저장 경고 종류다.
enum class EditorWarningKind : uint8 {
    // 열기: 의존 패키지를 묶음에서 뺐다(진단 asset = 뺀 패키지, related = 처음 가리킨 패키지).
    ExcludedDependency,
    // 열기: import를 null로 이었다(진단 asset = 가리킨 패키지, related = 대상, section·detail =
    // import 위치 — PackageUnresolvedImport).
    UnresolvedImport,
    // 저장(dropUnresolved): 미해결 import를 null로 저장했다(진단 asset = 패키지, related = 대상,
    // detail = import 위치).
    DroppedUnresolvedImport,
    // 저장: 교체는 보이지만 디렉터리 동기화를 확인하지 못했다(진단 NotDurable — 저장은 성공).
    NotDurable,
    // 저장: 그 디렉터리의 고아 임시 파일을 지우지 못했다(진단 None과 그 오류 — 저장은 성공).
    OrphanNotDeleted,
    // 해석: 색인에 없는 논리 경로를 별칭으로 풀었다(진단 AliasResolved, asset = 대상, text = 옛
    // 경로 — 7.1절 17 A2e1).
    AliasResolved,
    // 해석: 별칭 표를 읽지 못해 색인에 없는 경로를 풀지 못했다(진단 = 별칭 표 진단 — 대입은 따로
    // Unresolved로 실패한다).
    AliasTableUnreadable
};

// 경고의 경로 텍스트 칸 크기다(논리 경로 상한 + NUL — 할당 없이 복사되는 경고).
inline constexpr usize EditorWarningTextBytes = 257;

struct EditorWarning {
    EditorWarningKind kind = EditorWarningKind::ExcludedDependency;
    AssetDiagnostic diagnostic;
    // 경고 종류가 정한 경로 텍스트다(AliasResolved — 옛 경로, NUL로 끝남).
    char text[EditorWarningTextBytes] = {};

    StringView Text() const noexcept {
        return StringView(text);
    }
};

// 저장 옵션이다.
struct EditorSaveOptions {
    // 미해결 import가 있어도 null로 저장한다(명령 --force — 경고 DroppedUnresolvedImport).
    bool dropUnresolved = false;
};

// 새 대표 에셋의 공개 전 초기화다(EditorPackageSet::Create — asset-new의 대입 묶음, 7.1절 15
// A2c3). Create가 패키지·대표 에셋을 만든 트랜잭션에서 공개 전에 한 번 부른다.
//
// 계약: transaction을 끝내지 않는다(Commit·Rollback은 Create가 한다). 같은 트랜잭션에 하위 객체를
// 만들 수 있고 같은 집합의 다른 패키지를 열 수 있다(Open — 참조 해석). 그 안의 Create·Save는
// Busy다(경로·id 판정과 표 자리가 바깥 만들기와 엇갈린다). 실패를 돌려주면 Create가 트랜잭션을
// 되돌리고 그 오류로 실패한다(자세한 진단은 구현이 따로 남긴다).
//
// 사용 예:
//   class AssignNew final : public EditorAssetInitializer {
//   public:
//       Result<void> Initialize(ConstructionContext& transaction, ObjectHandle asset) noexcept
//           override {
//           auto prepared = reflection_text::PrepareAssignments(
//               reflection_text::AssignmentTarget::Staged(transaction, asset), inputs, options,
//               diagnostics);
//           ...
//       }
//   };
class EditorAssetInitializer {
public:
    virtual Result<void> Initialize(ConstructionContext& transaction,
                                    ObjectHandle asset) noexcept = 0;

protected:
    ~EditorAssetInitializer() = default;
};

// 저장 검증 scratch 크기다(VerifyPackageFile이 건너뛴 섹션을 이 크기 조각으로 훑는다).
inline constexpr usize EditorSaveVerifyScratchBytes = 64 * 1024;

// 열린 패키지 하나의 정보다.
struct EditorPackageInfo {
    explicit EditorPackageInfo(Allocator& allocator) noexcept
        : fileName(allocator), logicalPath(allocator), unresolved(allocator),
          importRecord(allocator), importOwner(allocator) {
    }

    EditorPackageInfo(EditorPackageInfo&&) noexcept = default;
    EditorPackageInfo& operator=(EditorPackageInfo&&) noexcept = default;

    AssetId id{};
    ObjectHandle package{};
    // Content 아래 OS 상대 이름(색인 항목의 fileName)이다. 아직 저장하지 않은 새 패키지는 비고
    // logicalPath가 첫 저장의 경로다.
    String fileName;
    String logicalPath;
    // 열 때 읽은 헤더 + 섹션 표 지문이다(저장의 외부 변경 판정 — 5.5절 1).
    PackageFingerprint fingerprint{};
    // 더 새 타입·커스텀 버전·섹션, 다른 스키마·더 새 버전 ImportRecord로 읽기 전용이다.
    bool readOnly = false;
    // null로 이은 import다(비어 있지 않으면 저장은 명시 선택이 있어야 한다 — A2c2b).
    Array<PackageUnresolvedImport> unresolved;
    // 열 때 읽은 ImportRecord byte와 ImportOwner 의존이다(저장이 그대로 쓴다 — 임포트 A2f가 새로
    // 만든다).
    Array<uint8> importRecord;
    Array<PackageDependencyEntry> importOwner;
    // 편집 세대(MarkDirty마다 오른다)와 dirty, 아직 파일이 없는 새 패키지인지다.
    uint64 generation = 0;
    bool dirty = false;
    bool unsaved = false;
    // 이 패키지의 파일 bulk 저장소다(Package가 소유 — 저장의 bulk source·Rebind).
    FilePackageBulkStore* store = nullptr;
};

class EditorPackageSet final : public PackageImportResolver {
public:
    // allocator는 집합보다 오래 산다(Registry·표·bulk raw byte가 쓴다).
    EditorPackageSet(Allocator& allocator, const EditorPackageSetConfig& config) noexcept;
    // 열린 패키지를 모두 놓고 Registry를 파괴한다(소유 스레드에서).
    ~EditorPackageSet();
    EditorPackageSet(const EditorPackageSet&) = delete;
    EditorPackageSet& operator=(const EditorPackageSet&) = delete;

    // 관찰자·Registry·핸들 캐시를 만든다. 오류: 설정의 null 포인터·캐시 용량 0·두 번째 호출은
    // InvalidArgument, 할당 실패.
    Result<void> Initialize() noexcept;

    // id 패키지를 연다(이미 열렸으면 그 handle — 경고 없음). 닫히지 않은 Hard·EditorOnly 의존을
    // 읽은 파일의 Dependencies로 넓혀 한 트랜잭션으로 열고, 이미 열린 패키지는 그대로 잇는다.
    // warnings가 있으면 경고를 붙인다(빼낸 의존 패키지·null로 이은 import, 위 종류).
    //
    // 오류(failure 진단): Initialize 전 InvalidArgument, 색인에 없음 NotFound(진단 None), 같은 id
    // 파일 둘 DuplicateAssetId / AlreadyExists, 파일 열기 실패(없음 MissingFile / NotFound, 그 밖의
    // IO 진단 None과 그 오류), 패키지 판정(Prepare)의 진단, 헤더 id가 요청과 다름(색인 뒤 파일이
    // 바뀜) ExternalModification / StaleHandle, ImportRecord 형식 위반 CorruptPackage /
    // InvalidArgument(칸 ImportRecord), ImportOwner 대조 위반 CorruptPackage / InvalidArgument(칸
    // Dependencies), 묶음 인스턴스의 오류(InstantiatePackages — 트랜잭션 전체 실패, 아무것도 열리지
    // 않는다), 할당 실패(진단 None). 의존 패키지의 같은 실패는 경고다.
    Result<ObjectHandle> Open(AssetId id, Array<EditorWarning>* warnings = nullptr,
                              AssetDiagnostic* failure = nullptr) noexcept;
    // 논리 경로로 연다(색인 FindByPath — 같은 경로 키 둘은 DuplicatePath / AlreadyExists, 없음은
    // NotFound). 그 밖은 Open(id)와 같다.
    Result<ObjectHandle> Open(const LogicalPath& path, Array<EditorWarning>* warnings = nullptr,
                              AssetDiagnostic* failure = nullptr) noexcept;

    // path에 새 패키지와 대표 에셋(type)을 만든다(파일은 첫 Save가 만든다). id가 null이면 새로
    // 발급한다. 패키지·대표 에셋(Id()는 패키지 id)을 한 트랜잭션으로 만들고, initializer가 있으면
    // 공개 전에 Initialize(transaction, 대표 에셋)를 부른 뒤 함께 공개한다(asset-new의 대입 —
    // 7.1절 15 A2c3). initializer나 공개가 실패하면 트랜잭션을 되돌려 아무것도 남지 않는다. 새
    // 패키지는 dirty다. 반환은 Package handle이다.
    //
    // 오류(failure 진단): Initialize 전·type 0 InvalidArgument, 같은 경로 키가 색인에 있거나 집합의
    // 저장하지 않은 새 패키지와 겹치면 DuplicatePath / AlreadyExists, 파일 절대 경로(임시 접미사
    // 포함)가 OS 상한을 넘으면 InvalidPath / CapacityExceeded, 같은 id가 열려 있거나 색인에 있으면
    // DuplicateAssetId / AlreadyExists, id 발급 실패(진단 None과 그 오류), type이 에셋 클래스가
    // 아니면 Registry의 오류(진단 None), initializer의 오류(진단 None — 자세한 내용은 initializer가
    // 남긴다), initializer 안에서 부르면 Busy, 할당 실패.
    Result<ObjectHandle> Create(const LogicalPath& path, TypeId type, AssetId id = {},
                                AssetDiagnostic* failure = nullptr,
                                EditorAssetInitializer* initializer = nullptr) noexcept;

    // 열린 패키지가 바뀌었음을 알린다(세대를 올리고 dirty를 켠다). 오류: 열리지 않음 NotFound.
    Result<void> MarkDirty(AssetId id) noexcept;

    // 열린 패키지를 저장한다(5.5절 1–8, 7.1절 15 A2c2b). lock은 이 프로젝트의 잡은 쓰기 잠금이다.
    // warnings가 있으면 경고를 붙인다(DroppedUnresolvedImport·NotDurable·OrphanNotDeleted).
    //
    // 오류(failure 진단 — 앞에서 걸리면 아무것도 쓰지 않는다): Initialize 전·hook 표 없음·이
    // 프로젝트의 잡은 잠금이 아님 InvalidArgument, 열리지 않음 NotFound, 읽기 전용 NewerVersion /
    // Unsupported, 미해결 import가 있는데 dropUnresolved가 아님 MissingDependency /
    // InvalidArgument(related = 첫 대상), 새 패키지의 대상이 이미 있거나 색인에 같은 경로 키
    // DuplicatePath / AlreadyExists, 외부 변경(대상의 지문이 열 때와 다름·대상 없음·대상 헤더 손상)
    // ExternalModification / StaleHandle, 대표 에셋 찾기·타입 조회·hook 오류(진단 None),
    // Create의 초기화 안에서 부르면 Busy, WritePackage의 진단, 파일
    // 만들기·쓰기·Flush·Close·Replace·디렉터리 만들기의 IO(진단 None과 그 오류 — 임시 파일을
    // 지운다), 재검증 실패(그 진단 — writer·sink 결함), 할당 실패. 교체 뒤의 일(저장소 다시
    // 잇기·색인 갱신)은 교체 전에 준비하므로 교체가 끝나면 실패하지 않는다(7.1절 15).
    Result<void> Save(AssetId id, const AssetDatabaseLock& lock, const EditorSaveOptions& options,
                      Array<EditorWarning>* warnings = nullptr,
                      AssetDiagnostic* failure = nullptr) noexcept;

    // 열린 패키지 정보다(없으면 null). 다음 Open까지 유효하다.
    const EditorPackageInfo* Find(AssetId id) const noexcept;

    // 열린 패키지 수와 id 순 i번째 정보다.
    usize Count() const noexcept {
        return open_.Size();
    }

    const EditorPackageInfo& At(usize index) const noexcept {
        return open_[index];
    }

    // 에셋 객체 Registry와 소유 접근 문맥이다(Initialize 뒤).
    ObjectRegistry& Registry() noexcept {
        return *registry_;
    }

    ObjectAccessContext Access() const noexcept {
        return registry_->OwnerAccess();
    }

    // 열린 패키지 handle이다(PackageImportResolver — 없으면 NotFound).
    Result<ObjectHandle> FindPackage(AssetId asset) noexcept override;

private:
    struct Pending;

    Result<ObjectHandle> OpenEntry(AssetId id, Array<EditorWarning>* warnings,
                                   AssetDiagnostic* failure) noexcept;
    // 저장의 판정(교체 전 — 실패하면 아무것도 쓰지 않는다)과 쓰기·교체·뒷정리다.
    struct SavePlan;
    Result<void> PlanSave(EditorPackageInfo& info, const EditorSaveOptions& options, SavePlan& plan,
                          AssetDiagnostic& diagnostic) noexcept;
    Result<void> WriteAndReplace(EditorPackageInfo& info, SavePlan& plan,
                                 Array<EditorWarning>* warnings,
                                 AssetDiagnostic& diagnostic) noexcept;
    void DeleteOrphans(const EditorPackageInfo& info, StringView target,
                       Array<EditorWarning>* warnings) noexcept;
    Result<void> PreparePending(Pending& pending, AssetDiagnostic& diagnostic) noexcept;
    usize LowerBound(AssetId id) const noexcept;

    Allocator* allocator_;
    EditorPackageSetConfig config_;
    UniquePtr<ReferenceObserver> observer_;
    UniquePtr<PackageFileCache> cache_;
    UniquePtr<ObjectRegistry> registry_;
    // 열린 패키지 정보(id 순)와 그 RootRef(같은 순서)다.
    Array<EditorPackageInfo> open_;
    Array<RootRef<Object>> roots_;
    // Create가 초기화를 부르는 동안이다(그 안의 Create·Save는 Busy).
    bool creating_ = false;
};

} // namespace rocks
