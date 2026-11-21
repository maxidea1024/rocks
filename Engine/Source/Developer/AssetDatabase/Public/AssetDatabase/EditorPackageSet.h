#pragma once

#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/FilePackageBulkStore.h>
#include <AssetRuntime/PackageLoad.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/ObjectRegistry.h>
#include <Object/ReferenceObserver.h>
#include <Platform/File.h>

// 에디터가 연 패키지 집합이다(Docs/Design/AssetSystem.ko.md 7.1절 3·15 — A2c2a). 패키지 정책
// 에셋 Registry 하나와 그 관찰자, bulk 파일 핸들 캐시를 소유하고, 색인(AssetIndex)으로 찾은 에디터
// 패키지를 Hard·EditorOnly 의존 닫힘째 한 트랜잭션으로 연다. bulk는 지연 읽기(파일 저장소)다. UE
// 에디터의 패키지 로드(LoadPackage + FLinkerLoad의 누락 import 경고)에 대응한다.
//
// 누락 의존은 열기를 막지 않는다: 의존 패키지가 색인에 없거나 읽을 수 없으면 그 패키지를 빼고,
// 그것을 가리키는 참조는 null로 이은 뒤 경고를 남긴다. 그런 패키지는 미해결 import를 기록하고
// 저장(A2c2b)은 명시 선택 없이는 거부한다(대상 id를 잃지 않게).
//
// 사용 예:
//   EditorPackageSetConfig config;
//   config.types = &types;
//   config.files = &platform::PhysicalFileSystem();
//   config.layout = &layout;
//   config.index = &index;            // Rebuild를 마친 색인
//   EditorPackageSet set(allocator, config);
//   if (auto ready = set.Initialize(); !ready) { ... }
//   Array<EditorOpenWarning> warnings(allocator);
//   AssetDiagnostic failure;
//   auto hero = set.Open(heroId, &warnings, &failure);
//   const EditorPackageInfo* info = set.Find(heroId);   // 파일 이름·지문·읽기 전용·미해결 import
namespace rocks {

struct EditorPackageSetConfig {
    TypeRegistry* types = nullptr;
    platform::FileSystem* files = nullptr;
    // 프로젝트 배치와 색인이다(빌림 — 집합보다 오래 산다). 색인은 Open이 id·경로 → 파일 이름에
    // 쓴다.
    const ProjectLayout* layout = nullptr;
    const AssetIndex* index = nullptr;
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

// 열기 경고 종류다.
enum class EditorOpenWarningKind : uint8 {
    // 의존 패키지를 묶음에서 뺐다(진단 asset = 뺀 패키지, related = 처음 가리킨 패키지).
    ExcludedDependency,
    // import를 null로 이었다(진단 asset = 가리킨 패키지, related = 대상, section·detail = import
    // 위치 — PackageUnresolvedImport).
    UnresolvedImport
};

struct EditorOpenWarning {
    EditorOpenWarningKind kind = EditorOpenWarningKind::ExcludedDependency;
    AssetDiagnostic diagnostic;
};

// 열린 패키지 하나의 정보다.
struct EditorPackageInfo {
    explicit EditorPackageInfo(Allocator& allocator) noexcept
        : fileName(allocator), unresolved(allocator) {
    }

    EditorPackageInfo(EditorPackageInfo&&) noexcept = default;
    EditorPackageInfo& operator=(EditorPackageInfo&&) noexcept = default;

    AssetId id{};
    ObjectHandle package{};
    // Content 아래 OS 상대 이름(색인 항목의 fileName)이다.
    String fileName;
    // 열 때 읽은 헤더 + 섹션 표 지문이다(저장의 외부 변경 판정 — 5.5절 1).
    PackageFingerprint fingerprint{};
    // 더 새 타입·커스텀 버전·섹션, 다른 스키마·더 새 버전 ImportRecord로 읽기 전용이다.
    bool readOnly = false;
    // null로 이은 import다(비어 있지 않으면 저장은 명시 선택이 있어야 한다 — A2c2b).
    Array<PackageUnresolvedImport> unresolved;
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
    Result<ObjectHandle> Open(AssetId id, Array<EditorOpenWarning>* warnings = nullptr,
                              AssetDiagnostic* failure = nullptr) noexcept;
    // 논리 경로로 연다(색인 FindByPath — 같은 경로 키 둘은 DuplicatePath / AlreadyExists, 없음은
    // NotFound). 그 밖은 Open(id)와 같다.
    Result<ObjectHandle> Open(const LogicalPath& path, Array<EditorOpenWarning>* warnings = nullptr,
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

    Result<ObjectHandle> OpenEntry(AssetId id, Array<EditorOpenWarning>* warnings,
                                   AssetDiagnostic* failure) noexcept;
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
};

} // namespace rocks
