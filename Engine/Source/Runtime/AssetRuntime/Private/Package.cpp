#include "PackageAccess.h"

#include <AssetRuntime/AssetRuntime.module.generated.h>
#include <AssetRuntime/BulkData.generated.h>
#include <AssetRuntime/Package.generated.h>
#include <Core/Assert.h>
#include <Object/ConstructionContext.h>

namespace rocks {

Result<Span<const uint8>> Package::BulkBytes(const BulkDataRef& ref,
                                             AssetDiagnostic* failure) const noexcept {
    if (bulkStore_.Get() != nullptr) {
        return bulkStore_->Read(ref, failure);
    }

    if (ref.IsEmpty()) {
        return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    if (ref.bulkId > bulkSlots_.Size()) {
        return Result<Span<const uint8>>::Failure(ErrorCode::NotFound);
    }

    const PackageBulkSlot& slot = bulkSlots_[ref.bulkId - 1];
    if (slot.rawBytes != ref.rawBytes || !(slot.sha256 == ref.sha256)) {
        return Result<Span<const uint8>>::Failure(ErrorCode::TypeMismatch);
    }

    if (slot.streamed) {
        return Result<Span<const uint8>>::Failure(ErrorCode::Unsupported);
    }

    return Result<Span<const uint8>>::Success(
        Span<const uint8>(bulkBytes_.Data() + slot.offset, static_cast<usize>(slot.rawBytes)));
}

Result<PackageBulkSlot> Package::BulkSlot(const BulkDataRef& ref) const noexcept {
    if (bulkStore_.Get() != nullptr) {
        return Result<PackageBulkSlot>::Failure(ErrorCode::Unsupported);
    }

    if (ref.IsEmpty()) {
        return Result<PackageBulkSlot>::Failure(ErrorCode::InvalidArgument);
    }

    if (ref.bulkId > bulkSlots_.Size()) {
        return Result<PackageBulkSlot>::Failure(ErrorCode::NotFound);
    }

    const PackageBulkSlot& slot = bulkSlots_[ref.bulkId - 1];
    if (slot.rawBytes != ref.rawBytes || !(slot.sha256 == ref.sha256)) {
        return Result<PackageBulkSlot>::Failure(ErrorCode::TypeMismatch);
    }

    return Result<PackageBulkSlot>::Success(slot);
}

Result<void> AttachBulkStore(Package& package, UniquePtr<PackageBulkStore>& store) noexcept {
    if (store.Get() == nullptr || package.BulkStore() != nullptr || package.BulkCount() != 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::PackageAccess::SetBulkStore(package, Move(store));
    return Result<void>::Success();
}

Result<RootRef<Package>> NewPackage(ObjectRegistry& registry, const ObjectAccessContext& access,
                                    AssetId id, PackageKind kind) noexcept {
    ConstructionContext transaction(registry, access);
    auto created = NewPackage(transaction, id, kind);
    if (!created) {
        return Result<RootRef<Package>>::Failure(created.Error());
    }

    // Commit의 root를 typed root를 얻을 때까지 쥔다(그 사이 GC가 패키지 정책 객체를 Discard 없이
    // 거두지 않게). 쥔 root가 있는 공개된 Package의 typed root는 실패하지 않는다.
    auto committed = transaction.Commit();
    if (!committed) {
        return Result<RootRef<Package>>::Failure(committed.Error());
    }

    auto typed = registry.TryPin<Package>(access, created.Value());
    ROCKS_ASSERT(typed.IsOk());
    return typed;
}

Result<ObjectHandle> NewPackage(ConstructionContext& transaction, AssetId id,
                                PackageKind kind) noexcept {
    if (id.IsNull() || (kind != PackageKind::Editor && kind != PackageKind::Cooked)) {
        return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
    }

    auto created = transaction.Create<Package>(GeneratedType<Package>::Id());
    if (!created) {
        return created;
    }

    // 방금 만든 이 트랜잭션의 객체다(Required 검사 중 표시가 있었다면 Create가 먼저 Busy였다).
    auto package = transaction.GetForWrite<Package>(created.Value());
    ROCKS_ASSERT(package.IsOk());
    detail::PackageAccess::SetIdentity(*package.Value(), id, kind);
    return created;
}

Result<ObjectHandle> NewRepresentativeAsset(ConstructionContext& transaction, ObjectHandle package,
                                            TypeId type) noexcept {
    auto owner = transaction.Get<Package>(package);
    if (!owner) {
        return Result<ObjectHandle>::Failure(owner.Error());
    }

    const AssetId id = owner.Value()->Id();
    auto created = transaction.Create<Asset>(type, package);
    if (!created) {
        return created;
    }

    // 방금 만든 이 트랜잭션의 객체다.
    auto asset = transaction.GetForWrite<Asset>(created.Value());
    ROCKS_ASSERT(asset.IsOk());
    detail::PackageAccess::SetAssetId(*asset.Value(), id);
    return created;
}

Result<void> RegisterAssetRuntimeTypes(ModuleRegistration& registration) noexcept {
    return generated::AssetRuntime::RegisterModuleTypes(registration);
}

} // namespace rocks
