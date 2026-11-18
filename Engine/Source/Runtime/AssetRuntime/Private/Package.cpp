#include "PackageAccess.h"

#include <AssetRuntime/AssetRuntime.module.generated.h>
#include <AssetRuntime/Package.generated.h>
#include <Object/ConstructionContext.h>

namespace rocks {

Result<Span<const uint8>> Package::BulkBytes(const BulkDataRef& ref) const noexcept {
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

    return Result<Span<const uint8>>::Success(
        Span<const uint8>(bulkBytes_.Data() + slot.offset, static_cast<usize>(slot.rawBytes)));
}

Result<RootRef<Package>> NewPackage(ObjectRegistry& registry, const ObjectAccessContext& access,
                                    AssetId id, PackageKind kind) noexcept {
    if (id.IsNull() || (kind != PackageKind::Editor && kind != PackageKind::Cooked)) {
        return Result<RootRef<Package>>::Failure(ErrorCode::InvalidArgument);
    }

    ConstructionContext transaction(registry, access);
    auto created = transaction.Create<Package>(GeneratedType<Package>::Id());
    if (!created) {
        return Result<RootRef<Package>>::Failure(created.Error());
    }

    auto package = transaction.GetForWrite<Package>(created.Value());
    if (!package) {
        return Result<RootRef<Package>>::Failure(package.Error());
    }

    detail::PackageAccess::SetIdentity(*package.Value(), id, kind);
    auto committed = transaction.Commit();
    if (!committed) {
        return Result<RootRef<Package>>::Failure(committed.Error());
    }

    auto typed = registry.TryPin<Package>(access, created.Value());
    if (!typed) {
        return Result<RootRef<Package>>::Failure(typed.Error());
    }

    return typed;
}

Result<void> RegisterAssetRuntimeTypes(ModuleRegistration& registration) noexcept {
    return generated::AssetRuntime::RegisterModuleTypes(registration);
}

} // namespace rocks
