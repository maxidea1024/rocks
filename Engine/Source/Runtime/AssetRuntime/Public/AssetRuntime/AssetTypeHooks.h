#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/BulkData.h>
#include <AssetRuntime/PackageReader.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/Containers/Array.h>
#include <Object/ObjectRegistry.h>

// 에셋 타입 hook 표다(Docs/Design/AssetSystem 7.1절 15, 16.2절 — A2c1). 에디터 저장이 대표
// 에셋에 hook을 불러 Tags 섹션(태그 — UE UObject::GetAssetRegistryTags 대응)과 Build 의존(16.2절
// DependencyDeclarer)을 만든다. 런타임은 쓰지 않는다(쿠커 A4가 Build hook을 쓴다). 에셋 서비스는
// 설정을 바꾼 뒤(임포트·재임포트·asset-set) 설정 정규화 hook을 부른다(UE
// UTexture::ValidateSettingsAfterImportOrEdit 대응 — Docs/Design/AssetSystem 12.12절).
//
// 조회: 대표 에셋 타입에서 base 사슬(TypeSchema::baseType)을 따라 가장 가까운 등록 hook 하나를
// 부르고, hook은 CallBase로 그 위의 등록 hook을 부를 수 있다(UE Super::GetAssetRegistryTags —
// 하위 타입이 부모 태그를 끄거나 바꿀 수 있다).
//
// 사용 예(타입 모듈 등록 — 등록 handle을 모듈 수명 동안 쥔다):
//   Result<void> TextureTags(const AssetTypeHookContext& context, AssetTagSink& sink) noexcept {
//       if (auto base = context.CallBaseTags(sink); !base) { return base; }
//       return sink.Add("Texture.Format", "BC7");
//   }
//   auto registration =
//       table.Register(GeneratedType<Texture2D>::Id(), {&TextureTags, nullptr, nullptr});
//
// 스레드: 등록·조회는 한 스레드에서 한다(에디터 Registry 소유 스레드).
namespace rocks {

class AssetTypeHookTable;
class TypeRegistry;

// 태그를 모은다. (키, 값) UTF-8 byte 순으로 두고 같은 쌍은 하나로 합친다. Write가 Tags Record
// byte(PackageTags 값 스키마)를 만든다.
class ROCKS_ASSETRUNTIME_API AssetTagSink {
public:
    explicit AssetTagSink(Allocator& allocator) noexcept : tags_(allocator) {
    }

    // 오류: 빈 키·키 256 byte 초과·값 4 KiB 초과·잘못된 UTF-8 InvalidArgument, 할당 실패.
    Result<void> Add(StringView key, StringView value) noexcept;

    Span<const PackageTag> Tags() const noexcept {
        return tags_.View();
    }

    // 모은 태그의 Tags Record byte다(없으면 빈 byte). 오류: WritePackageTags의 오류.
    Result<Array<uint8>> Write(Allocator& allocator) const noexcept;

private:
    Array<PackageTag> tags_;
};

// Build 의존을 모은다. AssetId 순으로 정렬하고 같은 id는 하나로 합친다.
class ROCKS_ASSETRUNTIME_API AssetDependencySink {
public:
    explicit AssetDependencySink(Allocator& allocator) noexcept : dependencies_(allocator) {
    }

    // 오류: null id InvalidArgument, 할당 실패.
    Result<void> Add(AssetId asset) noexcept;

    // Dependencies 항목(종류 Build — PackageWriteOptions::extraDependencies에 넣는다)이다.
    Span<const PackageDependencyEntry> Dependencies() const noexcept {
        return dependencies_.View();
    }

private:
    Array<PackageDependencyEntry> dependencies_;
};

// 원본 재구성을 부른 까닭이다(12.14절 A5j2).
enum class AssetSourceRebuildCause : uint8 {
    // asset-set — 대입·되돌린 필드(assignedFields)에 묶음 필드가 있을 때만 다시 만든다.
    Edit,
    // asset-new — Edit와 같고, 팩토리 기본 원본(Volume 1×1)을 둘 수 있다.
    New,
    // 원소 재임포트 전파 — assignedFields는 비고, 묶음 필드가 ChangedSource를 가리킬 때만 지금 값
    // 그대로 다시 복사한다(Volume 타일을 다시 정하지 않는다 — UE UReimportTextureFactory).
    Propagate,
    // 에디터 패키지 열기(12.14절 A5j2c — UE PostLoad) — assignedFields는 비고 ChangedSource는
    // null이다. 다른 에셋에서 원본을 굽는 타입이 낡은 저장 원본을 다시 굽는다(지금 원본과 같으면
    // 쓰지 않는다 — 낡지 않은 패키지가 dirty가 되지 않게).
    Open
};

// 원본 재구성 hook이 서비스에 닿는 길이다(Docs/Design/AssetSystem 12.14절 A5j2 — EditorPackageSet이
// 구현한다). hook 호출 동안만 유효하다.
class AssetSourceRebuildSink {
public:
    // object가 속한 패키지(outermost)의 bulk raw byte다(열린 패키지의 bulk 저장소 — 손상·외부 변경
    // 확인). span은 sink가 살아 있는 동안 유효하다. 오류: 빈 ref InvalidArgument, 그 패키지가 열려
    // 있지 않거나 bulk가 없음 NotFound, 저장소 읽기의 오류(진단은 Fail로 낸다).
    virtual Result<Span<const uint8>> ReadBulk(ObjectHandle object,
                                               const BulkDataRef& ref) noexcept = 0;

    // hook 대상 패키지에 저장 전 bulk로 더하고 그 ref를 준다(raw는 소비된다). 오류: 저장소의 오류.
    virtual Result<BulkDataRef> AddBulk(Array<uint8>&& raw) noexcept = 0;

    // hook 대상 에셋에 ImportRecord가 있는지다(파일로 임포트한 에셋 — 12.14절 임포트한 배열 보호).
    virtual bool HasImportRecord() const noexcept = 0;

    // 부른 까닭과, Propagate일 때 재임포트한 원소의 대표 에셋이다(그 밖은 null).
    virtual AssetSourceRebuildCause Cause() const noexcept = 0;
    virtual ObjectHandle ChangedSource() const noexcept = 0;

    // 진단을 남기고 error로 실패한다(hook은 이 값을 그대로 돌려준다).
    virtual Result<void> Fail(AssetDiagnosticCode code, ErrorCode error,
                              uint64 detail) noexcept = 0;

    // 경고를 남긴다. 오류: 할당 실패.
    virtual Result<void> Warn(AssetDiagnosticCode code, uint64 detail) noexcept = 0;

    // hook이 대상의 원본(또는 묶음 칸)을 실제로 다시 썼음을 알린다(서비스가 전파 대상 중 바뀐 것만
    // 정규화·저장한다 — 12.14절).
    virtual void MarkRebuilt() noexcept = 0;

protected:
    ~AssetSourceRebuildSink() = default;
};

// hook에 넘기는 문맥이다(hook 호출 동안만 유효하다).
struct ROCKS_ASSETRUNTIME_API AssetTypeHookContext {
    ObjectRegistry& registry;
    const ObjectAccessContext& access;
    ObjectHandle object;
    // 지금 부른 hook이 등록된 타입이다(CallBase가 그 위부터 찾는다).
    TypeId hookType;
    const AssetTypeHookTable& table;
    TypeRegistry& types;
    // 저장할 패키지 id다(복제면 사본 id — AssetSystem 7.1절 17 A2e3b). object는 원본
    // 객체이므로 태그·Build 의존이 패키지 정체성(id)을 써야 하면 객체의 값 대신 이것을 쓴다.
    AssetId asset{};

    // base 사슬에서 hookType 위의 가장 가까운 태그·Build hook을 부른다(없으면 아무것도 하지 않고
    // 성공). 오류: 그 hook의 오류, 타입 조회 오류.
    Result<void> CallBaseTags(AssetTagSink& sink) const noexcept;
    Result<void> CallBaseBuildDependencies(AssetDependencySink& sink) const noexcept;
    Result<void> CallBaseNormalizeSettings(Span<const StringView> assignedFields) const noexcept;
    Result<void> CallBaseRebuildSource(Span<const StringView> assignedFields,
                                       AssetSourceRebuildSink& sink) const noexcept;
};

using AssetTagHook = Result<void> (*)(const AssetTypeHookContext& context,
                                      AssetTagSink& sink) noexcept;
using AssetBuildDependencyHook = Result<void> (*)(const AssetTypeHookContext& context,
                                                  AssetDependencySink& sink) noexcept;
// 설정 정규화 hook이다(12.12절). context.object의 설정을 UE와 같은 규칙으로 고쳐 쓴다(쓰기는
// ObjectRegistry::ResolveForWrite). assignedFields는 이번 명령이 대입·되돌린 최상위 필드 이름이다
// (새 임포트·재임포트에서 대입이 없으면 빈다). 두 번 불러도 결과가 같아야 한다. 오류: 객체 접근
// 오류(실패하면 명령이 실패한다).
using AssetSettingsNormalizeHook = Result<void> (*)(const AssetTypeHookContext& context,
                                                    Span<const StringView> assignedFields) noexcept;
// 원본 재구성 hook이다(12.14절 A5j2 — UE UpdateSourceFromSourceTextures 대응). 다른 에셋을 원본으로
// 묶는 필드(assignedFields에 있을 때만)에서 context.object의 원본을 다시 만든다. 서비스가 정규화
// hook 앞에서 부른다(asset-set·asset-new·원소 재임포트 전파). EditorPackageSet::Open도 새로 연
// 패키지마다 부른다(까닭 Open — A5j2c). 두 번 불러도 결과가 같아야 한다.
// 오류: sink.Fail의 오류(진단), 객체 접근 오류, 할당 실패.
using AssetSourceRebuildHook = Result<void> (*)(const AssetTypeHookContext& context,
                                                Span<const StringView> assignedFields,
                                                AssetSourceRebuildSink& sink) noexcept;

// 타입 하나의 hook이다(모두 null일 수 있다 — null이면 그 종류는 base 사슬 위로 찾는다).
struct AssetTypeHooks {
    AssetTagHook tags = nullptr;
    AssetBuildDependencyHook buildDependencies = nullptr;
    AssetSettingsNormalizeHook normalizeSettings = nullptr;
    AssetSourceRebuildHook rebuildSource = nullptr;
};

// 등록 handle이다. 소멸하거나 Release하면 표에서 그 항목을 지운다(모듈 등록 수명에 묶는다).
class ROCKS_ASSETRUNTIME_API AssetTypeHookRegistration {
public:
    AssetTypeHookRegistration() noexcept = default;
    AssetTypeHookRegistration(AssetTypeHookRegistration&& other) noexcept;
    AssetTypeHookRegistration& operator=(AssetTypeHookRegistration&& other) noexcept;
    AssetTypeHookRegistration(const AssetTypeHookRegistration&) = delete;
    AssetTypeHookRegistration& operator=(const AssetTypeHookRegistration&) = delete;
    ~AssetTypeHookRegistration();

    void Release() noexcept;

    bool IsActive() const noexcept {
        return table_ != nullptr;
    }

private:
    friend class AssetTypeHookTable;

    AssetTypeHookTable* table_ = nullptr;
    TypeId type{};
};

class ROCKS_ASSETRUNTIME_API AssetTypeHookTable {
public:
    AssetTypeHookTable(Allocator& allocator, TypeRegistry& types) noexcept;
    // 모든 등록 handle을 놓은 뒤에 소멸해야 한다(남아 있으면 Fatal — 수명 계약 위반).
    ~AssetTypeHookTable();
    AssetTypeHookTable(const AssetTypeHookTable&) = delete;
    AssetTypeHookTable& operator=(const AssetTypeHookTable&) = delete;

    // type의 hook을 등록한다. 오류: 0 type·모두 null인 hooks InvalidArgument, 이미 등록한 type
    // AlreadyExists, 할당 실패. 표는 등록 handle보다 오래 살아야 한다.
    Result<AssetTypeHookRegistration> Register(TypeId type, AssetTypeHooks hooks) noexcept;

    // object(대표 에셋 — 타입 type)의 태그·Build 의존을 sink에 모은다. type에서 base 사슬로 가장
    // 가까운 hook 하나를 부른다(없으면 아무것도 하지 않고 성공). 오류: hook의 오류, 타입 조회
    // 오류(미등록 타입은 그 오류).
    // asset은 저장할 패키지 id다(문맥의 asset — 복제면 사본 id).
    Result<void> CollectTags(ObjectRegistry& registry, const ObjectAccessContext& access,
                             ObjectHandle object, TypeId type, AssetId asset,
                             AssetTagSink& sink) const noexcept;
    Result<void> CollectBuildDependencies(ObjectRegistry& registry,
                                          const ObjectAccessContext& access, ObjectHandle object,
                                          TypeId type, AssetId asset,
                                          AssetDependencySink& sink) const noexcept;
    // object의 설정을 정규화한다(가장 가까운 정규화 hook 하나 — 없으면 아무것도 하지 않고 성공).
    // 오류: hook의 오류, 타입 조회 오류.
    Result<void> NormalizeSettings(ObjectRegistry& registry, const ObjectAccessContext& access,
                                   ObjectHandle object, TypeId type, AssetId asset,
                                   Span<const StringView> assignedFields) const noexcept;
    // object의 원본을 다시 만든다(가장 가까운 원본 재구성 hook 하나 — 없으면 아무것도 하지 않고
    // 성공). 오류: hook의 오류, 타입 조회 오류.
    Result<void> RebuildSource(ObjectRegistry& registry, const ObjectAccessContext& access,
                               ObjectHandle object, TypeId type, AssetId asset,
                               Span<const StringView> assignedFields,
                               AssetSourceRebuildSink& sink) const noexcept;

    usize Count() const noexcept {
        return entries_.Size();
    }

private:
    friend struct AssetTypeHookContext;
    friend class AssetTypeHookRegistration;

    struct Entry {
        TypeId type{};
        AssetTypeHooks hooks{};
    };

    // hook 종류다(Find가 찾는 칸).
    enum class HookKind : uint8 {
        Tags,
        BuildDependencies,
        NormalizeSettings,
        RebuildSource,
    };

    static bool HasHook(const AssetTypeHooks& hooks, HookKind kind) noexcept;

    // start부터(skipStart면 start의 base부터) base 사슬로 kind hook이 있는 가장 가까운 항목을
    // 찾는다(없으면 null). 오류: 사슬의 타입 조회 오류.
    Result<const Entry*> Find(TypeId start, HookKind kind, bool skipStart) const noexcept;
    void Remove(TypeId type) noexcept;

    TypeRegistry* types_;
    Array<Entry> entries_;
};

} // namespace rocks
