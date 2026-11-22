#pragma once

#include <AssetRuntime/BulkData.h>
#include <AssetRuntime/BulkStore.h>
#include <AssetRuntime/PackageFormat.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Meta/Annotations.h>
#include <Object/AssetId.h>
#include <Object/ModuleRegistration.h>
#include <Object/Object.h>
#include <Object/ObjectRegistry.h>

namespace rocks {

class ConstructionContext;

namespace detail {
// 로더·writer가 Package의 비영속 상태(id·종류·bulk 저장소)를 채우는 통로다(정의는 Private).
struct PackageAccess;
} // namespace detail

// 에셋 패키지다(Docs/Design/AssetSystem.ko.md 6.1절, UE UPackage 대응). 에셋 객체 Registry(패키지
// 정책 Registry)에서 Outer·이름이 없는 유일한 객체이며 정체성은 AssetId다. 소속 객체(export)는
// 목록이 아니라 Registry의 자식 고리가 붙잡고(패키지 정책 — outer → inner 강한 간선), 소속 객체는
// Discard로만 죽는다 (ObjectFoundations 1.7절). 패키지 사이 하드 의존 목록은 두지 않는다 —
// AssetManager의 상주 단위 표가 의존과 간선 다중도를 가진다(구현에서 정한 점, AS 6.1절).
//
// 영속 필드는 없다(패키지 파일의 header가 정체성이다). id는 Transient·ReadOnly 리플렉션 필드로
// 도구가 읽을 수 있고, 로드가 Commit 전에 설정한다.
//
// bulk: 로드한 패키지의 Bulk entry는 압축을 푼 raw byte로 이 객체가 소유한다(쿠킹 패키지 v1은
// 패키지 전체를 읽어 SHA-256을 확인하므로 지연 읽기가 없다 — mip 스트리밍의 부분 읽기는
// TextureStreaming 단계).
//
// 사용 예:
//   auto bytes = package->BulkBytes(texture->platformData.mips[0].data);
//   if (!bytes) { return Result<void>::Failure(bytes.Error()); }
//
// 스레드: 에셋 객체 Registry의 소유 스레드(Game) 전용이다. BulkBytes가 준 span은 이 객체가 살아
// 있는 동안 유효하다(Borrow 범위 안에서만 쓴다).
ROCKS_CLASS()
class Package final : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit Package(Allocator& allocator) noexcept : bulkBytes_(allocator), bulkSlots_(allocator) {
    }

    // 이 패키지(와 대표 에셋)의 AssetId다.
    AssetId Id() const noexcept {
        return id_;
    }

    // 이 객체를 만든 패키지 파일의 종류다(에디터·쿠킹).
    PackageKind Kind() const noexcept {
        return kind_;
    }

    // bulk entry 수다(bulkId 1..이 값).
    uint32 BulkCount() const noexcept {
        return static_cast<uint32>(bulkSlots_.Size());
    }

    // ref가 가리키는 entry의 raw byte다. bulk 저장소가 있으면(에디터 패키지 — AS 7.1절 15)
    // 저장소에 묻는다(지연 읽기).
    //
    // 오류: 빈 ref(bulkId 0)는 InvalidArgument, 이 패키지에 없는 bulkId는 NotFound,
    // rawBytes·sha256이 entry와 다르면 TypeMismatch(다른 패키지의 값)다. 저장소가 있으면 그
    // 지연 읽기의 오류(PackageBulkStore::Read — 손상 CorruptPackage, 외부 변경
    // ExternalModification, IO 오류)도 failure에 진단과 함께 온다.
    Result<Span<const uint8>> BulkBytes(const BulkDataRef& ref,
                                        AssetDiagnostic* failure = nullptr) const noexcept;

    // bulk 저장소다(에디터 패키지 — 없으면 null).
    PackageBulkStore* BulkStore() const noexcept {
        return bulkStore_.Get();
    }

private:
    friend struct detail::PackageAccess;

    ROCKS_PROPERTY(Transient, ReadOnly) AssetId id_;
    ROCKS_NATIVE() PackageKind kind_ = PackageKind::Cooked;
    ROCKS_NATIVE() Array<uint8> bulkBytes_;
    ROCKS_NATIVE() Array<PackageBulkSlot> bulkSlots_;
    ROCKS_NATIVE() UniquePtr<PackageBulkStore> bulkStore_;
};

// 에셋 클래스의 기반이다(UE UObject 중 에셋인 것 — Texture·Material 등이 상속한다). 패키지당 대표
// 에셋 하나이고 대표 에셋은 패키지 바로 아래 빈 이름 객체다(AS 4.1절). 하위 객체는 Asset이 아니어도
// 된다.
//
// assetId는 Transient·ReadOnly 리플렉션 필드이며 로드가 Commit 전에 소속 패키지의 id로
// 설정한다(대표 에셋만 — 하위 Asset 파생 객체는 null로 둔다).
ROCKS_CLASS(Abstract)
class ROCKS_ASSETRUNTIME_API Asset : public Object {
    ROCKS_GENERATED_BODY()

public:
    // 대표 에셋이면 소속 패키지의 AssetId, 아니면 null이다.
    AssetId Id() const noexcept {
        return assetId_;
    }

private:
    friend struct detail::PackageAccess;

    ROCKS_PROPERTY(Transient, ReadOnly) AssetId assetId_;
};

// 새 빈 패키지를 만들어 공개한다(에셋 생성·임포트·시험 — 로드가 아닌 경로). id는 null이 아니어야
// 한다. 대표 에셋은 그 뒤 registry.NewObject<T>(access, type, package, "")로 만든다.
//
// 오류: null id는 InvalidArgument, 그 밖은 ConstructionContext::Create·Commit의 오류다(패키지
// 정책이 아닌 Registry에서 Package 생성 규칙 위반 등).
//
// 사용 예:
//   auto package = NewPackage(registry, access, AssetId::TryNew().Value(), PackageKind::Editor);
//   auto texture = registry.NewObject<Texture2D>(access, textureType, package.Value().Handle(),
//   "");
Result<RootRef<Package>> NewPackage(ObjectRegistry& registry, const ObjectAccessContext& access,
                                    AssetId id, PackageKind kind = PackageKind::Editor) noexcept;

// transaction 안에 새 빈 패키지를 만든다(공개 전 — Docs/Design/AssetSystem.ko.md 7.1절 15 A2c3).
// 같은 트랜잭션에서 대표 에셋을 만들고(NewRepresentativeAsset) 값을 채운 뒤 Commit으로 함께
// 공개한다. Commit 전에 실패하거나 Rollback하면 아무것도 남지 않는다. 반환은 공개 전 Package
// handle이다.
//
// 오류: null id·모르는 kind InvalidArgument(트랜잭션은 그대로), 그 밖은 transaction.Create의
// 오류(트랜잭션이 오류 상태로 고정된다).
//
// 사용 예:
//   ConstructionContext transaction(registry, access);
//   auto package = NewPackage(transaction, id, PackageKind::Editor);
//   auto asset = NewRepresentativeAsset(transaction, package.Value(), meshType);
//   ... transaction.GetForWrite<Mesh>(asset.Value()) 로 값을 채운다 ...
//   auto root = transaction.Commit();   // 첫 객체(Package)의 root
Result<ObjectHandle> NewPackage(ConstructionContext& transaction, AssetId id,
                                PackageKind kind = PackageKind::Editor) noexcept;

// transaction의 공개 전 Package(NewPackage(transaction, ...))에 대표 에셋(type — Asset 파생
// 클래스)을 만들고 Id()를 그 패키지 id로 둔다. 반환은 공개 전 에셋 handle이다.
//
// 오류: package가 이 트랜잭션의 Package가 아니면 transaction.Get의 오류(StaleHandle·TypeMismatch
// 등), type이 Asset 파생이 아니거나 대표 자식이 이미 있으면 transaction.Create의 오류(트랜잭션이
// 오류 상태로 고정된다).
Result<ObjectHandle> NewRepresentativeAsset(ConstructionContext& transaction, ObjectHandle package,
                                            TypeId type) noexcept;

// package에 bulk 저장소를 잇는다(에디터가 새로 만든 패키지 — 로드한 패키지는
// PackageLoadState::SetBulkStore). 오류: null 저장소, 이미 저장소나 로드한 bulk가 있는 패키지는
// InvalidArgument(저장소는 호출자에게 남는다).
Result<void> AttachBulkStore(Package& package, UniquePtr<PackageBulkStore>& store) noexcept;

// AssetRuntime 리플렉션 타입(Package·Asset·BulkDataRef)을 등록한다(생성 집계 함수
// generated::AssetRuntime::RegisterModuleTypes를 부른다). 엔진은 EngineAssets가, 도구·시험은 직접
// 부른다.
Result<void> RegisterAssetRuntimeTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
