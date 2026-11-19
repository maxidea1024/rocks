#include "PackageAccess.h"

#include <AssetRuntime/AssetRuntime.module.generated.h>
#include <AssetRuntime/BulkData.generated.h>
#include <AssetRuntime/Package.generated.h>
#include <Core/Assert.h>
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

    // 방금 만든 이 트랜잭션의 객체다.
    auto package = transaction.GetForWrite<Package>(created.Value());
    ROCKS_ASSERT(package.IsOk());

    detail::PackageAccess::SetIdentity(*package.Value(), id, kind);
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

Result<void> RegisterAssetRuntimeTypes(ModuleRegistration& registration) noexcept {
    return generated::AssetRuntime::RegisterModuleTypes(registration);
}

} // namespace rocks
