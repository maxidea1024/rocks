#pragma once

#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/FilePackageBulkStore.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/PackageLoad.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/OverrideSet.h>
#include <Object/ReferenceObserver.h>
#include <Platform/File.h>

// 에디터가 연 패키지 집합이다(Docs/Design/AssetSystem 7.1절 3·15 — A2c2a 열기, A2c2b 저장).
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
    AliasTableUnreadable,
    // 이동: OS 이동이 실패해 먼저 쓴 별칭 표를 되돌리려 했지만 되돌리지 못했다(진단 None과 그 오류
    // — 별칭 표에 옮기지 못한 옛 경로가 남는다, 해석은 색인을 먼저 보므로 해롭지 않다 — 7.1절 17
    // A2e2).
    AliasNotRestored,
    // 삭제: 지운 에셋을 가리키던 별칭을 지웠다(진단 None, asset = 지운 에셋, text = 별칭 경로 —
    // 7.1절 17 A2e3).
    AliasRemoved,
    // 복제: 원본의 ImportRecord가 다른 임포트의 출력을 가리키는 참조 기록이라 사본에서 버렸다(진단
    // None, asset = 사본 id, related = 그 소유자 — 7.1절 17 A2e3b).
    ImportRecordDropped,
    // 임포트: 원본·임포터 경고다(진단 = 그 코드 — AbsoluteSourcePath(detail = 원본 번호)·임포터가
    // Warn으로 남긴 코드, asset = 주 출력 — 7.1절 18 A2f).
    ImportWarning,
    // ID 수정: 새 id로 바꾼 패키지나 그 형제 출력이 고아가 됐다(진단 OrphanedOutput, asset = 고아,
    // related = 그 owner — 7.1절 17 A2e4b).
    OrphanedOutput,
    // 열기: 저장 당시와 지금의 클래스 기본값이 다른 export 클래스다(A5c — 진단 None, asset =
    // 패키지, related = 그 TypeId, text = 그 클래스의 export 수). 열기는 계속되고, 파일에 없던 값은
    // 새 기본값이 된다(RuntimeClassMetadata 'A5c 지문 연결').
    ClassDefaultsChanged,
    // 열기: 지금 클래스 기본값 레코드를 만들 수 없는 export 클래스다(A5c — 진단 None, asset =
    // 패키지, related = 그 TypeId, text = 이유 이름). 그 클래스를 고치기 전까지 이 패키지는 저장할
    // 수 없다(차이 저장이 레코드를 요구한다).
    ClassDefaultsUnavailable,
    // 원본 재구성: 원본 묶음 hook의 경고다(진단 = 그 코드 — SourceCleared, asset = 그 에셋,
    // detail — Docs/Design/AssetSystem 12.14절 A5j2).
    SourceRebuildWarning,
    // 열기: 이벤트 바인딩을 다시 만들지 못해 버렸다(진단 asset = 패키지, detail = 버린 수 — 대상이
    // 그 사이 사라짐·보존 실패 등, Docs/Design/EventReflection.ko.md ED22). 다시 저장하면 그
    // 바인딩이 사라진다.
    DroppedEventBindings,
    // 열기: 풀리지 않은 이벤트 바인딩(함수 없음·서명 다름)을 미해결 레코드로 보존했다(진단 asset =
    // 패키지, detail = 보존한 수 — ED24). 다시 저장하면 그대로 쓰이지만 호출되지 않고, 함수를
    // 고치거나 바인딩을 지우기 전에는 쿠킹이 거부한다.
    UnresolvedEventBindings
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

    // Initialize가 쓴 경로의 override 집합이다(없으면 null — RuntimeClassMetadata 'A5b'). Create가
    // 공개 전에 새 대표 에셋의 집합으로 설치한다.
    virtual const OverrideSet* InitialOverrides() const noexcept {
        return nullptr;
    }

protected:
    ~EditorAssetInitializer() = default;
};

// 저장 검증 scratch 크기다(VerifyPackageFile이 건너뛴 섹션을 이 크기 조각으로 훑는다).
inline constexpr usize EditorSaveVerifyScratchBytes = 64 * 1024;

// 에디터 패키지 bulk의 저장 codec이다(Docs/Design/AssetSystem 5절·12.12절 — UE FEditorBulkData처럼
// 원본을 압축한다). 바뀌지 않은 entry는 다시 압축하지 않고 저장 byte를 복사한다
// (PackageBulkSource::ReadStoredBulk).
inline constexpr BulkCodec EditorPackageBulkCodec = BulkCodec::Zstd;

// 열린 패키지 하나의 정보다.
struct EditorPackageInfo {
    explicit EditorPackageInfo(Allocator& allocator) noexcept
        : fileName(allocator), logicalPath(allocator), unresolved(allocator),
          importRecord(allocator), importOwner(allocator), overrides(allocator) {
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
    // 이 패키지 객체들의 override 집합이다(Docs/Design/RuntimeClassMetadata.ko.md 'A5b' — 차이
    // 형식 저장이 기준과 같아도 쓰는 값 경로). 열 때 파일에 있던 값 경로로 채우고, 경로 쓰기
    // (RecordOverride)가 더한다. 패키지를 닫으면 함께 사라진다.
    HashMap<ObjectHandle, OverrideSet> overrides;
};

// 열린 패키지 경로 바꾸기 준비다(EditorPackageSet::PrepareRelocate가 채우고 CommitRelocate가 쓴다 —
// 7.1절 17 A2e2). 새 이름을 모두 할당해 두고 확정은 옮기기만 한다(실패 없음). 준비와 확정 사이에
// 집합을 바꾸면(열기·만들기·저장) 안 된다.
class EditorRelocation {
public:
    explicit EditorRelocation(Allocator& allocator) noexcept : changes_(allocator) {
    }

    EditorRelocation(const EditorRelocation&) = delete;
    EditorRelocation& operator=(const EditorRelocation&) = delete;

    // 바뀌는 열린 패키지 수다.
    usize Count() const noexcept {
        return changes_.Size();
    }

private:
    friend class EditorPackageSet;

    struct Change {
        explicit Change(Allocator& allocator) noexcept
            : fileName(allocator), logicalPath(allocator) {
        }

        Change(Change&&) noexcept = default;
        Change& operator=(Change&&) noexcept = default;

        AssetId id{};
        String fileName;
        String logicalPath;
    };

    Array<Change> changes_;
    usize openCount_ = 0;
    bool ready_ = false;
};

// 열린 패키지 사이 강한 간선 표의 키다(소유 패키지 → 대상 패키지 — 7.1절 17 A2e3).
struct EditorEdgeKey {
    ObjectHandle owner{};
    ObjectHandle target{};

    bool operator==(const EditorEdgeKey&) const noexcept = default;
};

template <>
struct Hash<EditorEdgeKey> {
    uint64 operator()(const EditorEdgeKey& key) const noexcept {
        return HashCombine(Hash<ObjectHandle>{}(key.owner), Hash<ObjectHandle>{}(key.target));
    }
};

class ROCKS_ASSETDATABASE_API EditorPackageSet final : public PackageImportResolver {
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
    // 게시한 뒤 이번에 새로 연 패키지마다(id 순, 읽기 전용·미해결 import 패키지 제외) 대표 에셋의
    // 원본 재구성 hook을 까닭 Open으로 부른다(Docs/Design/AssetSystem 12.14절 A5j2c — UE PostLoad).
    // hook이 원본을 다시 썼으면 그 패키지를 dirty로 표시하고(저장하지 않는다), 경고는
    // SourceRebuildWarning으로 붙는다.
    //
    // 오류(failure 진단): Initialize 전 InvalidArgument, 색인에 없음 NotFound(진단 None), 같은 id
    // 파일 둘 DuplicateAssetId / AlreadyExists, 파일 열기 실패(없음 MissingFile / NotFound, 그 밖의
    // IO 진단 None과 그 오류), 패키지 판정(Prepare)의 진단, 헤더 id가 요청과 다름(색인 뒤 파일이
    // 바뀜) ExternalModification / StaleHandle, ImportRecord 형식 위반 CorruptPackage /
    // InvalidArgument(칸 ImportRecord), ImportOwner 대조 위반 CorruptPackage / InvalidArgument(칸
    // Dependencies), 묶음 인스턴스의 오류(InstantiatePackages — 트랜잭션 전체 실패, 아무것도 열리지
    // 않는다), 열 때 원본 재구성 hook의 오류(hook 진단 — 이번에 연 패키지를 모두 닫고 이번 호출의
    // 경고를 되돌린다), 할당 실패(진단 None). 의존 패키지의 같은 실패는 경고다.
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

    // 열린 패키지 source의 메모리 상태(저장하지 않은 편집 포함 — UE DuplicateAsset)를 newId로
    // target에 새 파일로 쓴다(7.1절 17 A2e3b). 사본은 집합에 열지 않고 원본 정보(파일·지문·dirty·
    // 저장소)는 바꾸지 않는다. writer 복제 옵션(PackageWriteOptions::duplicateId —
    // DuplicateTransient 필드는 기본값, 원본 id를 가리키는 Soft 참조는 newId)으로 쓰고,
    // ImportRecord는 소유 기록이면 owner와 원본 자신의 출력만 newId로 남겨 쓰고(형제 출력은 원본
    // 임포트의 것) 참조 기록이면 기록과 ImportOwner 의존을 버린다(경고 ImportRecordDropped).
    // hook에는 저장할 id로 newId를 준다. 쓰기는 저장과 같은 길(임시 → 재검증 → Replace → 색인
    // Refresh)이다. 별칭 점유는 보지 않는다(서비스가 본다).
    //
    // 오류(failure 진단 — 앞에서 걸리면 아무것도 쓰지 않는다): Initialize 전·hook 표 없음·잠금
    // 아님·newId null InvalidArgument, Create 초기화 안 Busy, 원본이 열리지 않음 NotFound, newId가
    // 열려 있거나 색인에 있음 DuplicateAssetId / AlreadyExists, 읽기 전용 NewerVersion /
    // Unsupported, 미해결 import MissingDependency / InvalidArgument(related = 첫 대상), 대상
    // 점유(파일·색인 같은 경로 키·저장하지 않은 새 패키지 경로) DuplicatePath / AlreadyExists, OS
    // 경로 상한 InvalidPath / CapacityExceeded, 대표 에셋·hook·WritePackage·IO·재검증의 오류(저장과
    // 같다), 할당 실패.
    Result<void> Duplicate(AssetId source, const LogicalPath& target, AssetId newId,
                           const AssetDatabaseLock& lock, Array<EditorWarning>* warnings = nullptr,
                           AssetDiagnostic* failure = nullptr) noexcept;

    // fileName(Content 상대 이름 — 색인 항목이 같은 id 둘 이상인 DuplicateAssetId 파일)의 패키지를
    // newId로 다시 쓴다(7.1절 17 A2e4b — asset-fix-duplicate-id). 그 파일을 이름으로 열고(의존
    // 닫힘째), 복제 모드 쓰기(writer duplicateId)로 같은 경로에 교체한다. 교체 직전에 그 패키지를
    // 이번 열기가 연 패키지째 닫는다(의존·Hard 순환 상대도 — 전에 열려 있던 것은 그대로다).
    // ImportRecord: 소유 기록은
    // owner와 자기 출력을 newId로 쓰고 형제 출력은 뺀다(형제는 남는 같은 id 파일의 임포트다), 참조
    // 기록은 그대로다. 쓰기 뒤 색인 판정(AssetIndex::OrphanReason)으로 고친 항목과 형제 항목 가운데
    // 고아가 된 것마다 경고 OrphanedOutput(related = 그 owner, detail = 사유)이다.
    //
    // 사용 예(서비스 — 색인 항목의 파일 이름과 새 id):
    //   auto made = AssetId::TryNew();
    //   if (made) { auto fixed = set.FixDuplicateId("Copies/Hero2.rasset", made.Value(), lock); }
    //
    // 오류(failure 진단 — 앞에서 걸리면 아무것도 바꾸지 않는다): Initialize 전·hook 표 없음·잠금
    // 아님·newId null InvalidArgument, Create 초기화 안 Busy, 색인에 그 파일 항목이 없음 NotFound,
    // 같은 id 둘이 아님 InvalidArgument, 그 id가 열려 있음 Busy, 읽기 전용 NewerVersion /
    // Unsupported, newId가 열려 있거나 색인에 있음 DuplicateAssetId / AlreadyExists, 열기의 오류,
    // 미해결 import MissingDependency / InvalidArgument, hook·쓰기·IO·재검증의 오류(저장과 같다),
    // 닫기의 Discard 오류(임시 파일을 지운다), 할당 실패. 열기 뒤 실패면 이번에 연 패키지를 닫는다.
    // 닫은 뒤 교체가 실패하면 패키지는 닫힌 채 파일·색인 그대로다.
    Result<void> FixDuplicateId(StringView fileName, AssetId newId, const AssetDatabaseLock& lock,
                                Array<EditorWarning>* warnings = nullptr,
                                AssetDiagnostic* failure = nullptr) noexcept;

    // 열린 패키지 정보다(없으면 null). 다음 Open까지 유효하다.
    const EditorPackageInfo* Find(AssetId id) const noexcept;

    // object(열린 패키지 안 객체)의 경로 쓰기를 override 집합에 기록한다(RuntimeClassMetadata 'A5b'
    // — asset-set·asset-new·임포트 설정 대입). path는 그 객체 타입 스키마의 경로이고 단위로
    // 정규화한다 (AddOverrideUnits — 원소·key는 필드, Atomic은 struct, 비Atomic struct 끝은 잎
    // 멤버로 펼침). 오류: 열린 패키지 객체가 아님 NotFound, 타입 조회·정규화의 오류, 할당 실패.
    Result<void> RecordOverride(ObjectHandle object, Span<const PathStep> path) noexcept;
    // 값 게시 전에 pending(단위 집합)을 object 집합에 합칠 자리를 잡는다(집합 항목을 만들고 용량을
    // 예약 — RuntimeClassMetadata 'A5b'). 그 뒤 MergeOverrides는 실패하지 않는다. 오류: 열린 패키지
    // 객체가 아님 NotFound, 할당 실패.
    Result<void> ReserveOverrides(ObjectHandle object, const OverrideSet& pending) noexcept;
    // ReserveOverrides 뒤 pending을 object 집합에 합친다(할당 없음).
    void MergeOverrides(ObjectHandle object, const OverrideSet& pending) noexcept;
    // object 집합에서 prefixes의 각 경로로 시작하는 단위를 모두 지운다(그 경로 자체 포함 — 기본값
    // 되돌리기 A5b2, 비Atomic struct 통째면 그 아래 잎과 옛 스키마의 잎까지). 할당하지 않는다(열린
    // 패키지 객체가 아니거나 집합이 없으면 아무것도 하지 않는다).
    void RemoveOverridesUnder(ObjectHandle object, const OverrideSet& prefixes) noexcept;
    // object의 override 집합이다(없으면 null — 객체의 패키지가 닫히면 무효).
    const OverrideSet* FindOverrides(ObjectHandle object) const noexcept;
    // object(열린 패키지 asset의 대표 에셋)의 설정을 정규화한다(타입 hook 표의 정규화 hook —
    // Docs/Design/AssetSystem 12.12절, UE ValidateSettingsAfterImportOrEdit). assignedFields는 이번
    // 명령이 대입·되돌린 최상위 필드 이름이다. hook 표가 없거나 그 타입에 hook이 없으면 아무것도
    // 하지 않는다. 오류: 타입 조회 오류, hook의 오류.
    Result<void> NormalizeSettings(ObjectHandle object, AssetId asset,
                                   Span<const StringView> assignedFields) noexcept;
    // object(열린 패키지 asset의 대표 에셋)의 원본을 묶음 필드에서 다시 만든다(타입 hook 표의 원본
    // 재구성 hook — 12.14절 A5j2, UE UpdateSourceFromSourceTextures). 묶음 원소의 bulk는 그 원소가
    // 속한 열린 패키지에서 읽고 새 원본은 asset 패키지의 저장 전 bulk로 더한다. 정규화보다 먼저
    // 부른다. hook 표가 없거나 hook이 없으면 아무것도 하지 않는다. 경고는 warnings(있으면)에
    // SourceRebuildWarning으로 붙는다. cause·changed는 sink의 Cause·ChangedSource다(changed는
    // Propagate일 때 재임포트한 원소). rebuilt가 있으면 hook이 원본을 실제로 다시 썼는지를 둔다
    // (sink MarkRebuilt). 오류: asset이 열려 있지 않음 NotFound, 타입 조회 오류, hook의 오류(진단은
    // failure).
    Result<void> RebuildSource(ObjectHandle object, AssetId asset,
                               Span<const StringView> assignedFields, AssetSourceRebuildCause cause,
                               ObjectHandle changed, Array<EditorWarning>* warnings,
                               AssetDiagnostic* failure, bool* rebuilt = nullptr) noexcept;

    // 이동하는 열린 패키지의 경로를 바꿀 준비를 한다(7.1절 17 A2e2). 저장된 패키지는 fileName이
    // fromFile(Content 상대 OS 이름 — 파일 또는 디렉터리, 이동하는 실제 이름)과 같거나 그 아래면 그
    // 앞부분을 toFile로 바꾼다(OS 이동과 같은 규칙 — 디스크의 실제 이름, 빈 fromFile은 없음).
    // 저장하지 않은 새 패키지는 logicalPath(첫 저장의 경로)가 from(논리 경로 — 패키지 또는 논리
    // 디렉터리)과 같거나 그 아래면 to 뒤에 나머지 구성 요소를 붙인다. 오류(failure 진단): 새 논리
    // 경로 규칙 위반(길이 등) InvalidPath / InvalidArgument, 할당 실패 — 둘 다 집합 그대로.
    Result<void> PrepareRelocate(const LogicalPath& from, const LogicalPath& to,
                                 StringView fromFile, StringView toFile, EditorRelocation& plan,
                                 AssetDiagnostic* failure = nullptr) noexcept;
    // 준비한 경로 바꾸기를 할당 없이 확정한다(준비하지 않은 plan·그 사이 집합 변경은 계약 위반 —
    // 단언).
    void CommitRelocate(EditorRelocation& plan) noexcept;

    // fileName이 fromFile(Content 상대 OS 이름 — 파일 또는 디렉터리)과 같거나 그 아래인 저장된 열린
    // 패키지의 bulk 저장소를 파일에서 떼어 낸다(FilePackageBulkStore::Detach — 읽지 않은 bulk를
    // 메모리로 읽고 캐시 핸들을 닫는다, UE ResetLoaders, 7.1절 6). 이동·삭제 전에 부른다. 오류:
    // Detach의 오류(그때까지 뗀 것은 그대로 — 메모리 bulk는 동작이 같다).
    Result<void> DetachUnder(StringView fromFile, AssetDiagnostic* failure = nullptr) noexcept;

    // 관찰자에 쌓인 패키지 사이 강한 간선 변화를 간선 표에 적용한다(7.1절 17 A2e3). 묶음을 쌍별로
    // 합친 뒤 표 용량을 먼저 잡고 무할당으로 적용한다. Open(새 패키지)·Create·Save·Close가 시작할
    // 때 부르므로(실패하면 그 호출이 아무것도 바꾸지 않고 그 오류) 관찰자는 그 호출 사이의 편집만
    // 쌓는다(키 상한 observerMaxKeys) — 편집만 오래 하는 host는 직접 불러도 된다. 오류: Initialize
    // 전 InvalidArgument, TakeChanges의 오류, 할당 실패(꺼낸 묶음을 쥔 채 다음 호출이 다시 적용한다
    // — 잃지 않는다).
    Result<void> SyncReferences() noexcept;

    // 다른 열린 패키지에서 id 패키지의 객체로 들어오는 강한 참조 다중도 합이다(SyncReferences
    // 뒤 — 저장하지 않은 편집 포함). referencer가 있으면 첫 참조 패키지(id 순 — 없으면 null id)를
    // 둔다. 열리지 않은 id는 0이다. 오류: SyncReferences의 오류.
    //
    // 사용 예:
    //   AssetId first;
    //   auto incoming = set.IncomingReferences(heroId, &first);
    //   if (incoming && incoming.Value() != 0) { /* first가 Hero를 참조한다 */ }
    Result<usize> IncomingReferences(AssetId id, AssetId* referencer = nullptr) noexcept;

    // 열린 패키지를 닫는다(7.1절 17 A2e3 — 삭제): 그 패키지를 Discard하고(되돌릴 수 없다 —
    // 저장하지 않은 편집도 버린다) 파일 캐시 핸들을 닫고 표에서 뺀다. 오류(failure 진단):
    // Initialize 전 InvalidArgument, Create의 초기화 안 Busy, 열리지 않음 NotFound(진단 None), 다른
    // 열린 패키지가 참조함 ReferencedBy / Busy(related = 첫 참조자 — 끊긴 참조가 남지 않게, 그
    // 패키지를 먼저 닫거나 참조를 고친다), SyncReferences·Discard의 오류(진단 None — 아무것도
    // 바뀌지 않는다), 할당 실패.
    Result<void> Close(AssetId id, AssetDiagnostic* failure = nullptr) noexcept;

    // before(id 순 — 어떤 일 전에 열려 있던 id)에 없는 열린 패키지를 모두 닫는다(7.1절 17 A2e4b —
    // ID 수정·임포트가 연 패키지: 고칠 패키지와 그 의존 닫힘, Hard 순환 상대 포함). 원래 열려 있던
    // 패키지는 새로 연 패키지를 참조할 수 없으므로(열기는 기존 패키지의 미해결 import를 다시 묶지
    // 않는다) 끊긴 참조가 남지 않는다. Discard는 id 역순이다. 오류: 할당 실패(아무것도 닫지
    // 않는다), Discard 실패(그 오류 — 앞서 닫은 것은 닫힌 채, 남은 것은 열린 채이고 닫힌 패키지로의
    // 참조는 Discard된 객체를 가리킨다(Resolve는 Destroyed) — 같은 before로 다시 부르면 남은 것을
    // 닫는다). 닫을 것이 없으면 성공이다.
    Result<void> CloseOpened(Span<const AssetId> before) noexcept;
    // 사용 예(서비스 — 일 전에 열린 id를 적어 두고 실패하면 그 뒤에 연 것을 닫는다):
    //   Array<AssetId> before(allocator);   // set.At(i).id를 차례로
    //   if (!done) { (void)set.CloseOpened(before.View()); }

    // 열린 패키지 id의 ImportRecord를 bytes로, ImportOwner 의존을 owner(null이면 없음 — 소유
    // 기록)로 바꾸고 dirty로 표시한다(저장이 쓴다 — 7.1절 18 A2f). bytes는 WriteImportRecord가 만든
    // 것이다 (검증하지 않는다 — 저장의 writer가 쓴다). 오류: Initialize 전 InvalidArgument, 열리지
    // 않음 NotFound, 할당 실패(바꾸지 않는다).
    Result<void> SetImportRecord(AssetId id, Span<const uint8> bytes, AssetId owner) noexcept;

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

    // rootFile이 있으면 루트 패키지(id)는 색인 id 조회 대신 그 파일을 연다(ID 수정 — 같은 id 둘).
    Result<ObjectHandle> OpenEntry(AssetId id, Array<EditorWarning>* warnings,
                                   AssetDiagnostic* failure, StringView rootFile = {}) noexcept;
    // 열린 저장된 패키지의 캐시 핸들 경로(Content/fileName — 저장하지 않은 새 패키지는 빈
    // 문자열)다.
    Result<void> OpenCachePath(const EditorPackageInfo& info, String& out) const noexcept;
    // 열린 패키지 at을 Discard하고 캐시 핸들(cachePath — 비면 없음)을 닫고 표에서 뺀다.
    Result<void> DiscardOpen(usize at, StringView cachePath) noexcept;
    // before(id 순)에 없는 열린 패키지마다 원본 재구성 hook을 까닭 Open으로 부른다(A5j2c — Open의
    // 설명). 오류: hook·RebuildSource의 오류(진단은 failure — hook이 진단을 남기지 않았으면 None과
    // 그 오류), MarkDirty의 오류. 실패해도 닫지 않는다(부른 쪽이 CloseOpened로 닫는다).
    Result<void> RebuildOpened(Span<const AssetId> before, Array<EditorWarning>* warnings,
                               AssetDiagnostic* failure) noexcept;
    // 저장의 판정(교체 전 — 실패하면 아무것도 쓰지 않는다)과 쓰기·교체·뒷정리다.
    struct SavePlan;
    // 복제 쓰기다(사본 id와 다시 쓴 ImportRecord byte).
    // closeSource면 교체 직전에 openedBefore에 없는 열린 패키지(같은 경로의 원본과 그 의존 — ID
    // 수정)를 닫는다(CloseOpened).
    struct DuplicateWrite {
        AssetId id{};
        Span<const uint8> importRecord{};
        bool closeSource = false;
        Span<const AssetId> openedBefore{};
    };
    Result<void> PlanSave(EditorPackageInfo& info, const EditorSaveOptions& options, SavePlan& plan,
                          AssetDiagnostic& diagnostic) noexcept;
    // 새 파일 대상(논리 경로의 NFC 이름 — 첫 저장·복제)을 plan에 정한다.
    Result<void> PlanNewTarget(const LogicalPath& path, AssetId id, SavePlan& plan,
                               AssetDiagnostic& diagnostic) noexcept;
    // 대표 에셋 타입 hook의 Tags·Build 의존과 importOwner를 plan에 모은다(asset은 저장할 id — hook
    // 문맥에 준다). Tags에는 bundle 태그(A4e2b)와, importRecord(이번에 쓸 기록 byte — self =
    // asset)가 소유 기록이면 그 임포터 태그(A2f3a)도 더한다.
    Result<void> PlanHookOutputs(const EditorPackageInfo& info, AssetId asset,
                                 Span<const PackageDependencyEntry> importOwner,
                                 Span<const uint8> importRecord, SavePlan& plan,
                                 AssetDiagnostic& diagnostic) noexcept;
    // duplicate가 있으면 복제 모드다(원본 정보·저장소를 바꾸지 않는다).
    Result<void> WriteAndReplace(EditorPackageInfo& info, SavePlan& plan,
                                 Array<EditorWarning>* warnings, AssetDiagnostic& diagnostic,
                                 const DuplicateWrite* duplicate = nullptr) noexcept;
    void DeleteOrphans(AssetId id, StringView target, Array<EditorWarning>* warnings) noexcept;
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
    // 패키지 사이 강한 간선 다중도 표와 꺼냈지만 아직 적용하지 못한 관찰자 묶음이다.
    HashMap<EditorEdgeKey, int64> edges_;
    Array<ReferenceEdgeDelta> pendingEdges_;
    // Create가 초기화를 부르는 동안이다(그 안의 Create·Save는 Busy).
    bool creating_ = false;
};

} // namespace rocks
