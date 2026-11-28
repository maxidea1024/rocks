#pragma once

#include <AssetRuntime/Package.h>

namespace rocks::detail {

// Package·Asset의 비영속 상태를 로더가 채우는 통로다(Package.h friend).
struct PackageAccess {
    static void SetPackage(Package& package, AssetId id, PackageKind kind, Array<uint8>&& bulkBytes,
                           Array<PackageBulkSlot>&& bulkSlots) noexcept {
        package.id_ = id;
        package.kind_ = kind;
        package.bulkBytes_ = Move(bulkBytes);
        package.bulkSlots_ = Move(bulkSlots);
    }

    static void SetBulkStore(Package& package, UniquePtr<PackageBulkStore>&& store) noexcept {
        package.bulkStore_ = Move(store);
    }

    static void SetSidecar(Package& package, UniquePtr<PackageSidecarState>&& sidecar) noexcept {
        package.sidecar_ = Move(sidecar);
    }

    static void SetAssetId(Asset& asset, AssetId id) noexcept {
        asset.assetId_ = id;
    }

    // 도구·시험이 새 패키지를 만들 때 id·종류를 정한다(로드가 아닌 생성 경로 — NewPackage).
    static void SetIdentity(Package& package, AssetId id, PackageKind kind) noexcept {
        package.id_ = id;
        package.kind_ = kind;
    }
};

} // namespace rocks::detail
